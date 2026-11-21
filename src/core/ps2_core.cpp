// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team, 2026 tdotlab
// SPDX-License-Identifier: GPL-3.0+

// Nacre's PS2 core: the emulator in src/ps2 (derived from PCSX2) behind Nacre's core
// interface, registered as nacre_ps2_core in src/core/registry.c.
//
// This file is the frontend half PCSX2 expects (the Host:: namespace), adapted from
// PCSX2's gsrunner, plus the table. One VM per process (SINGLETON). The VM
// runs on a CPU thread this file owns; Nacre's calls arrive on the host's UI thread
// and are queued onto the CPU thread, the way pcsx2-qt queues onto its EmuThread.
//
// Needs proper testing beyond Windows x64 and the smoke test.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "fmt/format.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/WindowInfo.h"

#include "PrecompiledHeader.h"

#include "Achievements.h"
#include "Config.h"
#include "GS.h"
#include "GameList.h"
#include "Host.h"
#include "Host/AudioStream.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "SPU2/spu2.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "VMManager.h"
#include "ps2/BiosTools.h"

#include "nacre_core.h"

namespace
{
	// ---- State shared between the host's UI thread and the CPU thread ----------

	struct Instance
	{
		NacreHost host{};
		std::string content_path;
		std::string data_dir;
		bool loaded = false;
	};

	Instance* s_instance = nullptr; // SINGLETON
	bool s_folders_ready = false;

	MemorySettingsInterface s_settings;

	std::mutex s_queue_mutex;
	std::condition_variable s_queue_cv;
	std::deque<std::function<void()>> s_queue;
	bool s_cpu_done = false; // the CPU thread has left its loop; guarded by s_queue_mutex

	std::thread s_cpu_thread;
	std::thread::id s_cpu_thread_id;
	std::atomic<bool> s_cpu_thread_exit{false};
	std::atomic<bool> s_parked{false};
	std::atomic<bool> s_vm_started{false};

	std::mutex s_surface_mutex;
	WindowInfo s_window_info; // Surfaceless until attach_surface

	std::mutex s_text_mutex;
	std::string s_error;
	std::string s_error_out; // copy handed to the host
	std::string s_title;
	std::string s_title_out;

	std::atomic<uint64_t> s_frames_presented{0};
	unsigned s_speed_percent = 100;
	uint32_t s_pad_buttons[2] = {};

	std::unique_ptr<AudioStream> CreateAudioStream(u32 sample_rate); // SPU2's output, see Audio below

	void Wake()
	{
		if (s_instance && s_instance->host.wake)
			s_instance->host.wake(s_instance->host.userdata);
	}

	void HostLog(NacreLogLevel level, const std::string& message)
	{
		if (s_instance && s_instance->host.log)
			s_instance->host.log(s_instance->host.userdata, level, message.c_str());
	}

	void SetError(std::string message)
	{
		{
			std::lock_guard lock(s_text_mutex);
			if (!s_error.empty())
				return; // keep the first fault
			s_error = std::move(message);
		}
		Wake();
	}

	void CopyError(char* error, size_t capacity, const std::string& message)
	{
		if (error && capacity)
			StringUtil::Strlcpy(error, message.c_str(), capacity);
	}

	bool OnCPUThread()
	{
		return std::this_thread::get_id() == s_cpu_thread_id;
	}

	void DrainQueue()
	{
		for (;;)
		{
			std::function<void()> fn;
			{
				std::lock_guard lock(s_queue_mutex);
				if (s_queue.empty())
					return;
				fn = std::move(s_queue.front());
				s_queue.pop_front();
			}
			fn();
		}
	}

	// Queue work for the CPU thread. With block, wait until it ran.
	// The UI thread owns the window PCSX2 presents to, and DXGI sends that thread messages
	// while the GS thread creates, resizes or releases a swap chain. A UI thread that waits
	// on the VM without taking those messages stalls until DXGI gives up (seconds), so every
	// wait here lets sent messages through. Posted messages stay queued: nothing re-enters
	// the frontend beyond what a SendMessage from another thread would do anyway.
	void TakeSentMessages()
	{
#ifdef _WIN32
		MSG msg;
		PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
#endif
	}

	void JoinTakingMessages(std::thread& thread)
	{
#ifdef _WIN32
		const HANDLE handle = static_cast<HANDLE>(thread.native_handle());
		while (MsgWaitForMultipleObjectsEx(1, &handle, INFINITE, QS_SENDMESSAGE, 0) == WAIT_OBJECT_0 + 1)
			TakeSentMessages();
#endif
		thread.join();
	}

	void QueueOnCPU(std::function<void()> fn, bool block)
	{
		if (OnCPUThread())
		{
			fn();
			return;
		}
		if (!s_cpu_thread.joinable())
		{
			// No VM yet: nothing to queue onto. Run it here; callers only use this for
			// settings that the boot will pick up anyway.
			fn();
			return;
		}

		if (!block)
		{
			{
				std::lock_guard lock(s_queue_mutex);
				if (s_cpu_done)
					return; // the VM is gone; the work is moot
				s_queue.push_back(std::move(fn));
			}
			s_queue_cv.notify_one();
			return;
		}

		std::mutex done_mutex;
		std::condition_variable done_cv;
		bool done = false;
		{
			std::lock_guard lock(s_queue_mutex);
			if (s_cpu_done)
				return;
			s_queue.push_back([&]() {
				fn();
				std::lock_guard dlock(done_mutex);
				done = true;
				done_cv.notify_one();
			});
		}
		s_queue_cv.notify_one();
		std::unique_lock dlock(done_mutex);
		while (!done_cv.wait_for(dlock, std::chrono::milliseconds(5), [&]() { return done; }))
		{
			dlock.unlock();
			TakeSentMessages();
			dlock.lock();
		}
	}

	// ---- Folders and settings -----------------------------------------------

	// The folder of the running app, where the build puts PCSX2's resources/.
	std::string AppDirectory()
	{
#ifdef _WIN32
		wchar_t buffer[4096];
		const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
		return std::string(Path::GetDirectory(StringUtil::WideStringToUTF8String(std::wstring_view(buffer, len))));
#else
		return std::string(); // TODO: dladdr on Mac/Linux.
#endif
	}

	bool InitFolders(const NacreHost& host, std::string* error)
	{
		if (s_folders_ready)
			return true;

		EmuFolders::AppRoot = AppDirectory();
		if (!EmuFolders::SetResourcesDirectory())
		{
			*error = "PS2 resources folder not found next to the app";
			return false;
		}

		// Everything PCSX2 writes (memory cards, shader cache, logs) goes under ps2/ in
		// the host's save directory, which other cores share, or beside the app when
		// the host gives none.
		EmuFolders::DataRoot = host.save_dir && *host.save_dir ? Path::Combine(host.save_dir, "ps2") :
		                                                          Path::Combine(EmuFolders::AppRoot, "data");
		EmuFolders::Settings = Path::Combine(EmuFolders::DataRoot, "inis");
		Error err;
		if (!FileSystem::EnsureDirectoryExists(EmuFolders::DataRoot.c_str(), true, &err))
		{
			*error = fmt::format("Can't create {}: {}", EmuFolders::DataRoot, err.GetDescription());
			return false;
		}

		const char* hw_error;
		if (!VMManager::PerformEarlyHardwareChecks(&hw_error))
		{
			*error = hw_error;
			return false;
		}

		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			*error = fmt::format("Missing font {}", roboto_path);
			return false;
		}
		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fonts.push_back(fi);
		ImGuiManager::SetFonts(std::move(fonts));

		s_folders_ready = true;
		return true;
	}

	void ApplyBaseSettings(const NacreHost& host)
	{
		MemorySettingsInterface& si = s_settings;
		// PCSX2 allows the base layer to be set once per process; later creates reuse
		// it and only reset its contents.
		static bool layer_set = false;
		{
			auto lock = Host::GetSettingsLock();
			si.Clear();
		}
		if (!layer_set)
		{
			Host::Internal::SetBaseSettingsLayer(&si);
			layer_set = true;
		}
		VMManager::SetDefaultSettings(si, true, true, true, true, true);

		if (host.system_dir && *host.system_dir)
			si.SetStringValue("Folders", "Bios", host.system_dir);

		// Nacre owns input and audio. PCSX2 must not open gamepads or an audio device.
		si.SetBoolValue("InputSources", "SDL", false);
		si.SetBoolValue("InputSources", "XInput", false);
		si.SetBoolValue("InputSources", "DInput", false);
		Pad::ClearPortBindings(si, 0);
		Pad::ClearPortBindings(si, 1);
		si.ClearSection("Hotkeys");
		// SPU2's stream comes from NacreAudioStream (SPU2::SetOutputStreamFactory); the
		// backend setting is only the fallback if that ever fails. Full volume while
		// fast-forwarding too: Nacre decides what to play at other speeds.
		si.SetStringValue("SPU2/Output", "Backend", "Null");
		si.SetStringValue("SPU2/Output", "SyncMode", "Disabled");
		si.SetIntValue("SPU2/Output", "StandardVolume", 100);
		si.SetIntValue("SPU2/Output", "FastForwardVolume", 100);
		si.SetBoolValue("SPU2/Output", "OutputMuted", false);

		// No UI on top of the game, no prompts.
		si.SetBoolValue("UI", "ConfirmShutdown", false);
		si.SetBoolValue("EmuCore", "EnableDiscordPresence", false);
		si.SetBoolValue("Achievements", "Enabled", false);
		// PCSX2's on-screen notifications: the Nacre frontend shows its own status.
		si.SetIntValue("EmuCore/GS", "OsdMessagesPos", static_cast<int>(OsdOverlayPos::None));

		si.SetBoolValue("Logging", "EnableSystemConsole", false);
		si.SetBoolValue("Logging", "EnableFileLogging", true);
		si.SetBoolValue("Logging", "EnableTimestamps", true);

		VMManager::Internal::LoadStartupSettings();
	}

	// Picks a BIOS from the system directory: the one named by the "bios" option, else
	// the first USA one, else the first one found. Returns the file name or empty.
	std::string PickBios(const std::string& dir, const std::string& wanted, std::string* found_list)
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);
		std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.FileName < b.FileName; });

		std::string first, usa;
		for (const FILESYSTEM_FIND_DATA& fd : files)
		{
			if (fd.Size < 4 * 1024 * 1024 || fd.Size > 8 * 1024 * 1024)
				continue;
			u32 version = 0, region = 0;
			std::string description, zone;
			if (!IsBIOS(fd.FileName.c_str(), version, description, region, zone))
				continue;
			const std::string name(Path::GetFileName(fd.FileName));
			if (found_list)
				*found_list += fmt::format("{}{} ({})", found_list->empty() ? "" : ", ", name, zone);
			if (!wanted.empty() && StringUtil::compareNoCase(name, wanted))
				return name;
			if (first.empty())
				first = name;
			if (usa.empty() && zone == "USA")
				usa = name;
		}
		if (!wanted.empty())
			return std::string();
		return usa.empty() ? first : usa;
	}

	// ---- The CPU thread -----------------------------------------------------

	void ApplySpeedOnCPU(unsigned percent)
	{
		const float scalar = static_cast<float>(percent) / 100.0f;
		if (percent > 100)
			s_settings.SetFloatValue("Framerate", "TurboScalar", scalar);
		else if (percent < 100)
			s_settings.SetFloatValue("Framerate", "SlomoScalar", scalar);
		if (VMManager::HasValidVM())
		{
			VMManager::ApplySettings();
			VMManager::SetLimiterMode(percent > 100 ? LimiterModeType::Turbo :
			                          percent < 100 ? LimiterModeType::Slomo :
			                                          LimiterModeType::Nominal);
		}
	}

	// Marks the CPU thread finished and runs what was queued, so no caller stays blocked.
	void FinishCPUThread()
	{
		{
			std::lock_guard lock(s_queue_mutex);
			s_cpu_done = true;
		}
		DrainQueue();
		s_parked = true;
	}

	void CPUThreadMain(std::string path)
	{
		s_cpu_thread_id = std::this_thread::get_id();

		HostLog(NACRE_LOG_DEBUG, "PS2: CPU thread start");
		if (!VMManager::Internal::CPUThreadInitialize())
		{
			SetError("PCSX2 failed to initialize its CPU thread");
			FinishCPUThread();
			return;
		}

		VMManager::ApplySettings();

		VMBootParameters params;
		params.filename = std::move(path);
		Error error;
		const VMBootResult result = VMManager::Initialize(params, &error);
		HostLog(NACRE_LOG_DEBUG, "PS2: VM initialized");
		if (result != VMBootResult::StartupSuccess)
		{
			SetError(error.IsValid() ? error.GetDescription() : std::string("PCSX2 could not start the disc"));
			VMManager::Internal::CPUThreadShutdown();
			FinishCPUThread();
			return;
		}

		s_vm_started = true;
		ApplySpeedOnCPU(s_speed_percent);
		VMManager::SetState(VMState::Running);

		while (!s_cpu_thread_exit.load())
		{
			DrainQueue();
			switch (VMManager::GetState())
			{
				case VMState::Running:
					s_parked = false;
					VMManager::Execute();
					break;

				case VMState::Paused:
				{
					s_parked = true;
					std::unique_lock lock(s_queue_mutex);
					s_queue_cv.wait(lock, []() { return !s_queue.empty() || s_cpu_thread_exit.load(); });
					break;
				}

				case VMState::Stopping:
				case VMState::Shutdown:
				default:
					s_cpu_thread_exit = true;
					break;
			}
		}

		// HasValidVM() is false once Stopping, so test the state itself.
		if (VMManager::GetState() != VMState::Shutdown)
		{
			VMManager::Shutdown(false);
			HostLog(NACRE_LOG_DEBUG, "PS2: VM shut down");
		}
		VMManager::Internal::CPUThreadShutdown();
		HostLog(NACRE_LOG_DEBUG, "PS2: CPU thread shut down");
		FinishCPUThread();
	}

	void StopCPUThread()
	{
		if (!s_cpu_thread.joinable())
			return;
		{
			std::lock_guard lock(s_queue_mutex);
			s_queue.push_back([]() {
				s_cpu_thread_exit = true;
				if (VMManager::HasValidVM())
					VMManager::SetState(VMState::Stopping);
			});
		}
		s_queue_cv.notify_one();
		HostLog(NACRE_LOG_DEBUG, "PS2: stopping CPU thread");
		JoinTakingMessages(s_cpu_thread);
		HostLog(NACRE_LOG_DEBUG, "PS2: CPU thread joined");
		s_cpu_thread_exit = false;
		s_vm_started = false;
		std::lock_guard lock(s_queue_mutex);
		s_queue.clear();
	}

	// ---- Nacre table entries --------------------------------------------------

	int Probe(const uint8_t* head, size_t head_size, size_t size, const char* path)
	{
		int score = 0;
		std::string_view ext;
		if (path)
			ext = Path::GetExtension(path);
		const bool disc_ext = StringUtil::compareNoCase(ext, "iso") || StringUtil::compareNoCase(ext, "chd") ||
		                      StringUtil::compareNoCase(ext, "cso") || StringUtil::compareNoCase(ext, "zso") ||
		                      StringUtil::compareNoCase(ext, "gz") || StringUtil::compareNoCase(ext, "elf");
		if (disc_ext)
			score = 40;

		// ISO 9660 primary volume descriptor at sector 16 (2048-byte sectors), with
		// "PLAYSTATION" as the system identifier on PS2 (and PS1) discs.
		constexpr size_t pvd = 16 * 2048;
		if (head_size >= pvd + 40 && std::memcmp(head + pvd + 1, "CD001", 5) == 0)
		{
			score = std::max(score, 30);
			if (std::memcmp(head + pvd + 8, "PLAYSTATION", 11) == 0)
				score = 90;
		}
		return score;
	}

	void* Create(const NacreHost* host, char* error, size_t error_capacity)
	{
		if (s_instance)
		{
			CopyError(error, error_capacity, "The PS2 core is already running a game");
			return nullptr;
		}
		auto* inst = new Instance();
		if (host)
			inst->host = *host;
		s_instance = inst;

		std::string err;
		if (!InitFolders(inst->host, &err))
		{
			CopyError(error, error_capacity, err);
			s_instance = nullptr;
			delete inst;
			return nullptr;
		}
		ApplyBaseSettings(inst->host);
		SPU2::SetOutputStreamFactory(&CreateAudioStream);

		{
			std::lock_guard lock(s_text_mutex);
			s_error.clear();
			s_title.clear();
		}
		{
			std::lock_guard lock(s_surface_mutex);
			s_window_info = WindowInfo();
		}
		s_frames_presented = 0;
		s_speed_percent = 100;
		s_pad_buttons[0] = s_pad_buttons[1] = 0;
		return inst;
	}

	void Destroy(void* self)
	{
		auto* inst = static_cast<Instance*>(self);
		if (!inst)
			return;
		StopCPUThread();
		HostLog(NACRE_LOG_INFO, fmt::format("PS2: presented {} frames", s_frames_presented.load()));
		s_instance = nullptr;
		delete inst;
	}

	int Load(void* self, const NacreContent* content, char* error, size_t error_capacity)
	{
		auto* inst = static_cast<Instance*>(self);
		if (!content || !content->path || !*content->path)
		{
			CopyError(error, error_capacity, "The PS2 core needs a file path");
			return 0;
		}
		if (!FileSystem::FileExists(content->path))
		{
			CopyError(error, error_capacity, fmt::format("File not found: {}", content->path));
			return 0;
		}

		const std::string bios_dir = s_settings.GetStringValue("Folders", "Bios");
		if (bios_dir.empty() || !FileSystem::DirectoryExists(bios_dir.c_str()))
		{
			CopyError(error, error_capacity, "No PS2 BIOS folder. Put a BIOS dump in the system folder.");
			return 0;
		}
		std::string found;
		const std::string bios = PickBios(bios_dir, s_settings.GetStringValue("Filenames", "BIOS"), &found);
		if (bios.empty())
		{
			CopyError(error, error_capacity,
				found.empty() ? fmt::format("No PS2 BIOS found in {}", bios_dir) :
				                fmt::format("BIOS not found. Available: {}", found));
			return 0;
		}
		s_settings.SetStringValue("Filenames", "BIOS", bios.c_str());
		HostLog(NACRE_LOG_INFO, fmt::format("PS2: BIOS {} (found: {})", bios, found));

		inst->content_path = content->path;
		inst->loaded = true;
		return 1;
	}

	void SetRunning(void* self, int running)
	{
		auto* inst = static_cast<Instance*>(self);
		if (!inst->loaded)
			return;

		if (running)
		{
			if (!s_cpu_thread.joinable())
			{
				s_parked = false;
				{
					std::lock_guard lock(s_queue_mutex);
					s_cpu_done = false;
				}
				s_cpu_thread = std::thread(CPUThreadMain, inst->content_path);
				return;
			}
			QueueOnCPU([]() { VMManager::SetPaused(false); }, false);
			return;
		}

		if (!s_cpu_thread.joinable())
			return;
		QueueOnCPU([]() {
			if (!VMManager::HasValidVM())
				return;
			VMManager::SetPaused(true);
			s_parked = false; // the run loop sets it again once Execute has unwound
		}, true);
		// Return once Execute has unwound and the thread waits for work.
		while (!s_parked.load())
			std::this_thread::yield();
	}

	void Reset(void* self)
	{
		auto* inst = static_cast<Instance*>(self);
		if (!s_cpu_thread.joinable())
			return;
		{
			std::lock_guard lock(s_text_mutex);
			s_error.clear();
		}
		QueueOnCPU([]() {
			if (VMManager::HasValidVM())
				VMManager::Reset();
		}, true);
		(void)inst;
	}

	void SetSpeed(void*, unsigned percent)
	{
		percent = std::clamp(percent, 5u, 1000u);
		s_speed_percent = percent;
		if (s_vm_started)
			QueueOnCPU([percent]() { ApplySpeedOnCPU(percent); }, false);
	}

	const char* GetError(void*)
	{
		std::lock_guard lock(s_text_mutex);
		if (s_error.empty())
			return nullptr;
		s_error_out = s_error;
		return s_error_out.c_str();
	}

	int AttachSurface(void*, const NacreSurface* surface, char* error, size_t error_capacity)
	{
		WindowInfo wi;
		if (surface)
		{
			switch (surface->kind)
			{
#ifdef _WIN32
				case NACRE_SURFACE_WIN32_HWND:
					wi.type = WindowInfo::Type::Win32;
					break;
#endif
#ifdef __APPLE__
				case NACRE_SURFACE_COCOA_NSVIEW:
					wi.type = WindowInfo::Type::MacOS;
					break;
#endif
#if defined(__linux__)
				case NACRE_SURFACE_X11_WINDOW:
					wi.type = WindowInfo::Type::X11;
					break;
				case NACRE_SURFACE_WAYLAND:
					wi.type = WindowInfo::Type::Wayland;
					break;
#endif
				default:
					CopyError(error, error_capacity, "Unsupported surface kind for this platform");
					return 0;
			}
			wi.window_handle = surface->handle;
			wi.display_connection = surface->display;
			wi.surface_width = surface->width;
			wi.surface_height = surface->height;
			wi.surface_scale = surface->scale > 0.0f ? surface->scale : 1.0f;
		}

		bool same_window, resized;
		{
			std::lock_guard lock(s_surface_mutex);
			same_window = s_window_info.type == wi.type && s_window_info.window_handle == wi.window_handle;
			resized = s_window_info.surface_width != wi.surface_width ||
			          s_window_info.surface_height != wi.surface_height ||
			          s_window_info.surface_scale != wi.surface_scale;
			s_window_info = wi;
		}

		if (s_vm_started)
		{
			if (!same_window)
				QueueOnCPU([]() { MTGS::UpdateDisplayWindow(); }, false);
			else if (resized)
				QueueOnCPU([w = wi.surface_width, h = wi.surface_height, s = wi.surface_scale]() {
					MTGS::ResizeDisplayWindow(w, h, s);
				}, false);
		}
		return 1;
	}

	// ---- Audio ----------------------------------------------------------------
	//
	// SPU2 writes float stereo into an AudioStream on the CPU thread; normally a cubeb
	// or SDL device pulls from it. Here the host pulls instead: audio_read drains the
	// same ring on the host's thread and converts to int16. One writer, one reader, the
	// ring's own atomics, the same arrangement as PCSX2's device backends. No time
	// stretching or resampling: Nacre's audio output handles rate and queue depth.

	class NacreAudioStream;

	std::mutex s_audio_mutex; // guards s_audio_stream's lifetime, not the ring
	NacreAudioStream* s_audio_stream = nullptr;
	std::atomic<unsigned> s_audio_rate{SPU2::SAMPLE_RATE};

	class NacreAudioStream final : public AudioStream
	{
	public:
		// Ring capacity. The host sees at most KEEP_DIVISOR-th of a second; anything
		// older is dropped on read, which is the drop-oldest policy the interface asks for.
		static constexpr u16 RING_MS = 500;
		static constexpr u32 KEEP_DIVISOR = 3;

		static std::unique_ptr<AudioStream> Create(u32 sample_rate)
		{
			AudioStreamParameters params;
			params.expansion_mode = AudioExpansionMode::Disabled;
			params.buffer_ms = RING_MS;

			std::unique_ptr<NacreAudioStream> stream(new NacreAudioStream(sample_rate, params));
			stream->BaseInitialize(&StereoSampleReaderImpl, false);

			std::lock_guard lock(s_audio_mutex);
			s_audio_stream = stream.get();
			s_audio_rate.store(sample_rate);
			return stream;
		}

		~NacreAudioStream() override
		{
			std::lock_guard lock(s_audio_mutex);
			if (s_audio_stream == this)
				s_audio_stream = nullptr;
		}

		// Reader side; call with s_audio_mutex held.
		size_t Read(int16_t* out, size_t max_frames)
		{
			u32 available = GetBufferedFramesRelaxed();
			const u32 keep = GetSampleRate() / KEEP_DIVISOR;
			if (available > keep)
			{
				Discard(available - keep);
				available = keep;
			}

			// Never ask for more than is buffered: ReadFrames pads a short read with a
			// crude resample of what it has, which Nacre does better with silence.
			size_t remaining = std::min<size_t>(available, max_frames);
			const size_t total = remaining;
			while (remaining > 0)
			{
				const u32 n = static_cast<u32>(std::min<size_t>(remaining, SCRATCH_FRAMES));
				ReadFrames(m_scratch, n);
				for (u32 i = 0; i < n * 2; i++)
				{
					const float v = std::clamp(m_scratch[i], -1.0f, 1.0f);
					*out++ = static_cast<int16_t>(v * 32767.0f);
				}
				remaining -= n;
			}
			return total;
		}

		// Reader side: drop what is buffered by consuming it, so only the reader moves
		// the read position. (AudioStream::EmptyBuffer moves the write position and
		// belongs to the writer's thread.)
		void Clear() { Discard(GetBufferedFramesRelaxed()); }

	private:
		static constexpr u32 SCRATCH_FRAMES = 1024;

		NacreAudioStream(u32 sample_rate, const AudioStreamParameters& params)
			: AudioStream(sample_rate, params)
		{
		}

		void Discard(u32 frames)
		{
			while (frames > 0)
			{
				const u32 n = std::min(frames, SCRATCH_FRAMES);
				ReadFrames(m_scratch, n);
				frames -= n;
			}
		}

		float m_scratch[SCRATCH_FRAMES * 2];
	};

	std::unique_ptr<AudioStream> CreateAudioStream(u32 sample_rate)
	{
		return NacreAudioStream::Create(sample_rate);
	}

	unsigned AudioRate(void*) { return s_audio_rate.load(); }

	size_t AudioRead(void*, int16_t* stereo, size_t max_frames)
	{
		std::lock_guard lock(s_audio_mutex);
		return (s_audio_stream && stereo) ? s_audio_stream->Read(stereo, max_frames) : 0;
	}

	void AudioClear(void*)
	{
		std::lock_guard lock(s_audio_mutex);
		if (s_audio_stream)
			s_audio_stream->Clear();
	}

	// DualShock 2 bind for each Nacre pad button.
	constexpr u32 PAD_BINDS[NACRE_PAD_BUTTON_COUNT] = {
		PadDualshock2::Inputs::PAD_UP,
		PadDualshock2::Inputs::PAD_DOWN,
		PadDualshock2::Inputs::PAD_LEFT,
		PadDualshock2::Inputs::PAD_RIGHT,
		PadDualshock2::Inputs::PAD_CROSS,    // SOUTH
		PadDualshock2::Inputs::PAD_CIRCLE,   // EAST
		PadDualshock2::Inputs::PAD_SQUARE,   // WEST
		PadDualshock2::Inputs::PAD_TRIANGLE, // NORTH
		PadDualshock2::Inputs::PAD_L1,
		PadDualshock2::Inputs::PAD_R1,
		PadDualshock2::Inputs::PAD_L2,
		PadDualshock2::Inputs::PAD_R2,
		PadDualshock2::Inputs::PAD_L3,
		PadDualshock2::Inputs::PAD_R3,
		PadDualshock2::Inputs::PAD_SELECT,
		PadDualshock2::Inputs::PAD_START,
	};

	void SetPad(void*, unsigned port, uint32_t buttons)
	{
		if (port > 1 || !s_vm_started)
			return;
		const uint32_t changed = buttons ^ s_pad_buttons[port];
		s_pad_buttons[port] = buttons;
		if (!changed)
			return;
		QueueOnCPU([port, buttons, changed]() {
			for (int b = 0; b < NACRE_PAD_BUTTON_COUNT; b++)
			{
				if (changed & NACRE_PAD_BIT(b))
					Pad::SetControllerState(port, PAD_BINDS[b], (buttons & NACRE_PAD_BIT(b)) ? 1.0f : 0.0f);
			}
		}, false);
	}

	void SetAxis(void*, unsigned port, NacreAxis axis, int16_t value)
	{
		if (port > 1 || !s_vm_started || axis >= NACRE_AXIS_COUNT)
			return;
		static constexpr u32 NEG[NACRE_AXIS_COUNT] = {PadDualshock2::Inputs::PAD_L_LEFT, PadDualshock2::Inputs::PAD_L_UP,
			PadDualshock2::Inputs::PAD_R_LEFT, PadDualshock2::Inputs::PAD_R_UP};
		static constexpr u32 POS[NACRE_AXIS_COUNT] = {PadDualshock2::Inputs::PAD_L_RIGHT, PadDualshock2::Inputs::PAD_L_DOWN,
			PadDualshock2::Inputs::PAD_R_RIGHT, PadDualshock2::Inputs::PAD_R_DOWN};
		const float neg = value < 0 ? static_cast<float>(-static_cast<int>(value)) / 32768.0f : 0.0f;
		const float pos = value > 0 ? static_cast<float>(value) / 32767.0f : 0.0f;
		QueueOnCPU([port, n = NEG[axis], p = POS[axis], neg, pos]() {
			Pad::SetControllerState(port, n, neg);
			Pad::SetControllerState(port, p, pos);
		}, false);
	}

	int SetOption(void*, const char* key, const char* value)
	{
		if (!key || !value)
			return 0;
		const std::string_view k(key);
		if (k == "bios")
		{
			// Takes effect at the next load.
			s_settings.SetStringValue("Filenames", "BIOS", value);
			return 1;
		}
		if (k == "renderer")
		{
			// PCSX2's GSRendererType numbers: -1 auto, 3 D3D11, 12 D3D12, 13 software,
			// 14 Vulkan, 17 OpenGL, 17... see GSRendererType in pcsx2/Config.h.
			const std::optional<int> n = StringUtil::FromChars<int>(value);
			if (!n.has_value())
				return 0;
			s_settings.SetIntValue("EmuCore/GS", "Renderer", n.value());
		}
		else if (k == "upscale")
		{
			const std::optional<float> f = StringUtil::FromChars<float>(value);
			if (!f.has_value() || f.value() < 1.0f || f.value() > 8.0f)
				return 0;
			s_settings.SetFloatValue("EmuCore/GS", "upscale_multiplier", f.value());
		}
		else if (k == "osd")
		{
			// PCSX2's own notifications: "1" top-left, "0" off (the default).
			s_settings.SetIntValue("EmuCore/GS", "OsdMessagesPos",
				static_cast<int>(std::strcmp(value, "0") != 0 ? OsdOverlayPos::TopLeft : OsdOverlayPos::None));
		}
		else if (k == "fast_boot")
		{
			s_settings.SetBoolValue("EmuCore", "EnableFastBoot", std::strcmp(value, "0") != 0);
			return 1;
		}
		else
		{
			return 0;
		}
		if (s_vm_started)
			QueueOnCPU([]() { VMManager::ApplySettings(); }, false);
		return 1;
	}

	const char* GetTitle(void*)
	{
		std::lock_guard lock(s_text_mutex);
		if (s_title.empty())
			return nullptr;
		s_title_out = s_title;
		return s_title_out.c_str();
	}

	NacreCore MakeCore()
	{
		NacreCore c{};
		c.api_version = NACRE_CORE_API_VERSION;
		c.id = "ps2";
		c.name = "PlayStation 2";
		c.extensions = "iso;chd;cso;zso;gz;elf";
		c.flags = NACRE_CORE_THREADED | NACRE_CORE_NATIVE_VIDEO | NACRE_CORE_SINGLETON | NACRE_CORE_NEEDS_PATH |
		          NACRE_CORE_NEEDS_SYSTEM_FILES;
		c.keypad_keys = 0;
		c.pad_labels[NACRE_PAD_UP] = "Up";
		c.pad_labels[NACRE_PAD_DOWN] = "Down";
		c.pad_labels[NACRE_PAD_LEFT] = "Left";
		c.pad_labels[NACRE_PAD_RIGHT] = "Right";
		c.pad_labels[NACRE_PAD_SOUTH] = "Cross";
		c.pad_labels[NACRE_PAD_EAST] = "Circle";
		c.pad_labels[NACRE_PAD_WEST] = "Square";
		c.pad_labels[NACRE_PAD_NORTH] = "Triangle";
		c.pad_labels[NACRE_PAD_L1] = "L1";
		c.pad_labels[NACRE_PAD_R1] = "R1";
		c.pad_labels[NACRE_PAD_L2] = "L2";
		c.pad_labels[NACRE_PAD_R2] = "R2";
		c.pad_labels[NACRE_PAD_L3] = "L3";
		c.pad_labels[NACRE_PAD_R3] = "R3";
		c.pad_labels[NACRE_PAD_SELECT] = "Select";
		c.pad_labels[NACRE_PAD_START] = "Start";
		c.probe = Probe;
		c.create = Create;
		c.destroy = Destroy;
		c.load = Load;
		c.reset = Reset;
		c.advance = nullptr;
		c.set_running = SetRunning;
		c.set_speed = SetSpeed;
		c.error = GetError;
		c.video = nullptr;
		c.attach_surface = AttachSurface;
		c.audio_rate = AudioRate;
		c.audio_read = AudioRead;
		c.audio_clear = AudioClear;
		c.set_pad = SetPad;
		c.set_axis = SetAxis;
		c.set_keypad = nullptr;
		c.set_option = SetOption;
		c.title = GetTitle;
		return c;
	}
} // namespace

extern "C" const NacreCore nacre_ps2_core = MakeCore();

// Diagnostics for the smoke test; not part of the core interface.
extern "C" uint64_t nacre_ps2_frames_presented(void)
{
	return s_frames_presented.load();
}

//////////////////////////////////////////////////////////////////////////
// Host:: implementation (from pcsx2-gsrunner)
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges() {}
void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock) {}
void Host::CheckForSettingsChanges(const Pcsx2Config& old_config) {}
bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui) { return false; }
void Host::SetDefaultUISettings(SettingsInterface& si) {}
bool Host::LocaleCircleConfirm() { return false; }

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!message.empty())
		HostLog(NACRE_LOG_INFO, fmt::format("PS2: {}{}{}", title, title.empty() ? "" : ": ", message));
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (message.empty())
		return;
	const std::string text = title.empty() ? std::string(message) : fmt::format("{}: {}", title, message);
	HostLog(NACRE_LOG_ERROR, "PS2: " + text);
	SetError(text);
}

void Host::OpenURL(const std::string_view url) {}
bool Host::CopyTextToClipboard(const std::string_view text) { return false; }
std::string Host::GetTextFromClipboard() { return std::string(); }
void Host::BeginTextInput() {}
void Host::EndTextInput() {}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	std::lock_guard lock(s_surface_mutex);
	return s_window_info;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name) {}
void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier) {}
void Host::SetMouseMode(bool relative_mode, bool hide_cursor) {}
void Host::SetMouseLock(bool state) {}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	std::lock_guard lock(s_surface_mutex);
	return s_window_info;
}

void Host::ReleaseRenderWindow() {}

void Host::BeginPresentFrame()
{
	s_frames_presented.fetch_add(1, std::memory_order_relaxed);
}

void Host::RequestResizeHostDisplay(s32 width, s32 height) {}
void Host::OnVMStarting() {}
void Host::OnVMStarted() { Wake(); }
void Host::OnVMDestroyed() { Wake(); }
void Host::OnVMPaused() {}
void Host::OnVMResumed() {}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	{
		std::lock_guard lock(s_text_mutex);
		s_title = title;
	}
	if (!title.empty())
		HostLog(NACRE_LOG_INFO, fmt::format("PS2: {} [{}]", title, disc_serial));
	Wake();
}

void Host::OnPerformanceMetricsUpdated() {}
void Host::OnSaveStateLoading(const std::string_view filename) {}
void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful) {}
void Host::OnSaveStateSaved(const std::string_view filename) {}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	QueueOnCPU(std::move(function), block);
}

void Host::RefreshGameListAsync(bool invalidate_cache) {}
void Host::CancelGameListRefresh() {}
void Host::OnGameListFavoriteChanged(const std::string& path) {}
void Host::OnGameSettingsChanged(const std::string& path) {}
bool Host::IsFullscreen() { return false; }
void Host::SetFullscreen(bool enabled) {}
void Host::OnCaptureStarted(const std::string& filename) {}
void Host::OnCaptureStopped() {}
void Host::RequestExitApplication(bool allow_confirm) {}
void Host::RequestExitBigPicture() {}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages) {}
void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason) {}
void Host::OnAchievementsHardcoreModeChanged(bool enabled) {}
void Host::OnAchievementsRefreshed() {}
bool Host::InBatchMode() { return false; }
bool Host::InNoGUIMode() { return true; }
bool Host::ShouldPreferHostFileSelector() { return false; }

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str) { return std::nullopt; }
std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code) { return std::nullopt; }
const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code) { return nullptr; }

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

void Host::PumpMessagesOnCPUThread()
{
	// Called by the VM every vsync while it runs: our queue is how Nacre's calls
	// (pause, input, speed, resize) reach the CPU thread.
	DrainQueue();
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, 2, count_str.view());
	}

	return ret;
}
