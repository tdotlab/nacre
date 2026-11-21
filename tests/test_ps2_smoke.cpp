// SPDX-FileCopyrightText: 2026 tdotlab
// SPDX-License-Identifier: GPL-3.0+

// test_ps2_smoke: finds the PS2 core in Nacre's registry the way a frontend does and
// boots a disc in a plain Win32 window. Built with NACRE_PS2 (target nacre-ps2-smoke),
// beside Nacre.exe so it finds the same DLLs and resources.
//
//   nacre-ps2-smoke <disc> <bios-dir> [seconds=20] [capture.bmp] [rounds=1]
//
// Checks: the table is registered and valid, a VM starts, frames are presented, pausing stops frame
// production and resuming restarts it, audio arrives at the core's rate and is not
// silent, and the core tears down (and boots again when rounds > 1). With a capture
// path, the last round also writes the second half's audio beside it as .wav.
// Prints PASS or FAIL lines and exits non-zero on failure.

#ifndef _WIN32
int main() { return 0; }
#else

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nacre_core.h"

// Diagnostics from src/core/ps2_core.cpp; not part of the core interface.
extern "C" uint64_t nacre_ps2_frames_presented(void);
static uint64_t (*const g_frames)(void) = nacre_ps2_frames_presented;
static int g_failures;

// Audio drained the way the desktop frontend does it: everything available, every pump.
static const NacreCore* g_core;
static void* g_self;
static uint64_t g_audio_frames;
static int g_audio_peak;
static bool g_audio_record;
static std::vector<int16_t> g_audio_recorded;

static void DrainAudio()
{
	if (!g_core || !g_self)
		return;
	static int16_t buf[4096 * 2];
	size_t n;
	while ((n = g_core->audio_read(g_self, buf, 4096)) > 0)
	{
		g_audio_frames += n;
		for (size_t i = 0; i < n * 2; i++)
			g_audio_peak = std::max(g_audio_peak, std::abs(static_cast<int>(buf[i])));
		if (g_audio_record)
			g_audio_recorded.insert(g_audio_recorded.end(), buf, buf + n * 2);
	}
}

static bool SaveWav(const std::string& path, const std::vector<int16_t>& samples, unsigned rate)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
	auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f);
	u32(36 + data_bytes);
	std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16);
	u16(1); // PCM
	u16(2);
	u32(rate);
	u32(rate * 4);
	u16(4);
	u16(16);
	std::fwrite("data", 1, 4, f);
	u32(data_bytes);
	std::fwrite(samples.data(), 2, samples.size(), f);
	std::fclose(f);
	return true;
}

static void Check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		g_failures++;
}

static void HostLogCb(void*, NacreLogLevel level, const char* message)
{
	static const char* names[] = {"debug", "info", "warn", "error"};
	std::printf("  [%s] %s\n", names[level & 3], message);
}

static void HostWake(void*) {}

// Pumps window messages for the given time.
static void Pump(DWORD ms)
{
	const DWORD end = GetTickCount() + ms;
	while ((LONG)(end - GetTickCount()) > 0)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		DrainAudio();
		Sleep(10);
	}
}

static bool SaveCapture(HWND hwnd, const char* path)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	const int w = rc.right, h = rc.bottom;
	HDC wdc = GetDC(hwnd);
	HDC mdc = CreateCompatibleDC(wdc);
	HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
	HGDIOBJ old = SelectObject(mdc, bmp);
	// PW_CLIENTONLY | PW_RENDERFULLCONTENT: picks up D3D/Vulkan swap chains.
	BOOL ok = PrintWindow(hwnd, mdc, 1 | 2);
	SelectObject(mdc, old);

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = h; // bottom-up, as BMP files store it
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	const int stride = (w * 3 + 3) & ~3;
	std::vector<uint8_t> pixels(static_cast<size_t>(stride) * h);
	GetDIBits(mdc, bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
	DeleteObject(bmp);
	DeleteDC(mdc);
	ReleaseDC(hwnd, wdc);
	if (!ok)
		return false;

	// Count non-black pixels so the log says whether anything was drawn.
	size_t lit = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			const uint8_t* p = &pixels[static_cast<size_t>(y) * stride + x * 3];
			if (p[0] > 16 || p[1] > 16 || p[2] > 16)
				lit++;
		}
	std::printf("  capture %dx%d, %.1f%% non-black\n", w, h, 100.0 * lit / (static_cast<double>(w) * h));

	FILE* f = std::fopen(path, "wb");
	if (!f)
		return false;
	BITMAPFILEHEADER fh{};
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
	fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
	std::fwrite(&fh, sizeof(fh), 1, f);
	std::fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
	std::fwrite(pixels.data(), pixels.size(), 1, f);
	std::fclose(f);
	return lit > 0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool RunRound(const NacreCore* core, HWND hwnd, const char* disc, const char* bios_dir, int seconds,
	const char* capture)
{
	NacreHost host{};
	host.system_dir = bios_dir;
	host.save_dir = nullptr; // beside the DLL
	host.log = HostLogCb;
	host.wake = HostWake;

	char err[512] = {};
	void* self = core->create(&host, err, sizeof(err));
	Check(self != nullptr, "create");
	if (!self)
	{
		std::printf("  %s\n", err);
		return false;
	}

	NacreContent content{};
	content.path = disc;
	const int loaded = core->load(self, &content, err, sizeof(err));
	Check(loaded != 0, "load");
	if (!loaded)
	{
		std::printf("  %s\n", err);
		core->destroy(self);
		return false;
	}

	RECT rc;
	GetClientRect(hwnd, &rc);
	NacreSurface surface{};
	surface.kind = NACRE_SURFACE_WIN32_HWND;
	surface.handle = hwnd;
	surface.width = static_cast<unsigned>(rc.right);
	surface.height = static_cast<unsigned>(rc.bottom);
	surface.scale = 1.0f;
	Check(core->attach_surface(self, &surface, err, sizeof(err)) != 0, "attach_surface");

	g_core = core;
	g_self = self;
	g_audio_frames = 0;
	g_audio_peak = 0;
	g_audio_recorded.clear();
	const unsigned audio_rate = core->audio_rate(self);
	std::printf("  audio_rate %u\n", audio_rate);
	Check(audio_rate == 48000, "audio_rate is 48 kHz");

	const uint64_t start_frames = g_frames ? g_frames() : 0;
	core->set_running(self, 1);
	Pump(static_cast<DWORD>(seconds) * 500);

	const uint64_t mid = g_frames ? g_frames() : 0;
	std::printf("  %llu frames after %ds\n", static_cast<unsigned long long>(mid - start_frames), seconds / 2);
	Check(mid > start_frames, "frames presented");

	// Pause must stop frame production; resume must restart it.
	core->set_running(self, 0);
	const uint64_t paused_at = g_frames ? g_frames() : 0;
	Pump(100);
	core->audio_clear(self); // as the frontend does on stop
	const uint64_t audio_paused_at = g_audio_frames;
	Pump(900);
	const uint64_t paused_after = g_frames ? g_frames() : 0;
	const uint64_t audio_while_paused = g_audio_frames - audio_paused_at;
	std::printf("  %llu frames, %llu audio frames while paused 1s\n",
		static_cast<unsigned long long>(paused_after - paused_at), static_cast<unsigned long long>(audio_while_paused));
	Check(paused_after - paused_at <= 2, "pause stops frames");
	Check(audio_while_paused <= 256, "pause stops audio");
	core->set_running(self, 1);

	// Exercise input: tap Start.
	core->set_pad(self, 0, NACRE_PAD_BIT(NACRE_PAD_START));
	Pump(200);
	core->set_pad(self, 0, 0);

	// Second half: past boot and shader warm-up, so emulation should be at full
	// speed and audio should arrive at about audio_rate frames per wall second.
	const uint64_t audio_start = g_audio_frames;
	g_audio_peak = 0;
	g_audio_record = capture && *capture;
	const DWORD audio_t0 = GetTickCount();
	Pump(static_cast<DWORD>(seconds) * 500);
	const double audio_secs = (GetTickCount() - audio_t0) / 1000.0;
	g_audio_record = false;
	const double audio_per_sec = static_cast<double>(g_audio_frames - audio_start) / audio_secs;
	std::printf("  %.0f audio frames per second (%.0f%% of audio_rate), peak %d\n", audio_per_sec,
		100.0 * audio_per_sec / audio_rate, g_audio_peak);
	Check(audio_per_sec > audio_rate * 0.85 && audio_per_sec < audio_rate * 1.15, "audio arrives at audio_rate");
	Check(g_audio_peak > 328, "audio is not silent"); // above -40 dBFS
	const uint64_t end = g_frames ? g_frames() : 0;
	std::printf("  %llu frames in total\n", static_cast<unsigned long long>(end - start_frames));
	Check(end > paused_after, "resume restarts frames");

	const double fps = static_cast<double>(end - start_frames - (paused_after - paused_at)) / seconds;
	std::printf("  ~%.1f presented frames per second\n", fps);

	if (const char* title = core->title(self))
		std::printf("  title: %s\n", title);
	const char* e = core->error(self);
	Check(e == nullptr, "no core error");
	if (e)
		std::printf("  %s\n", e);

	if (capture && *capture)
	{
		Check(SaveCapture(hwnd, capture), "capture shows an image");
		std::string wav = capture;
		const size_t dot = wav.find_last_of('.');
		wav = (dot == std::string::npos ? wav : wav.substr(0, dot)) + ".wav";
		Check(SaveWav(wav, g_audio_recorded, audio_rate), "audio written");
		std::printf("  %s (%.1f s)\n", wav.c_str(), g_audio_recorded.size() / 2.0 / audio_rate);
	}

	g_self = nullptr;
	core->destroy(self);
	Check(true, "destroy returned");
	return true;
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s <disc> <bios-dir> [seconds] [capture.bmp] [rounds]\n", argv[0]);
		return 2;
	}
	const char* disc = argv[1];
	const char* bios_dir = argv[2];
	const int seconds = argc > 3 ? std::max(2, std::atoi(argv[3])) : 20;
	const char* capture = argc > 4 ? argv[4] : nullptr;
	const int rounds = argc > 5 ? std::max(1, std::atoi(argv[5])) : 1;

	std::setvbuf(stdout, nullptr, _IONBF, 0); // progress shows even if a step hangs
	SetProcessDPIAware();

	const NacreCore* core = nacre_core_by_id("ps2");
	Check(core != nullptr, "ps2 core registered");
	if (!core)
		return 1;
	char problem[256] = "";
	Check(nacre_core_validate(core, problem, sizeof(problem)) == 1, "table validates");
	if (problem[0])
		std::printf("  %s\n", problem);
	Check(core && core->api_version == NACRE_CORE_API_VERSION, "api version");
	Check(core->advance == nullptr && core->attach_surface && core->set_running, "threaded native-video table");
	std::printf("  core: %s (%s)\n", core->name, core->extensions);

	// Probe the disc's head the way the registry does.
	{
		FILE* f = std::fopen(disc, "rb");
		std::vector<uint8_t> head(64 * 1024);
		size_t n = 0;
		long long size = 0;
		if (f)
		{
			n = std::fread(head.data(), 1, head.size(), f);
			_fseeki64(f, 0, SEEK_END);
			size = _ftelli64(f);
			std::fclose(f);
		}
		const int score = core->probe(head.data(), n, static_cast<size_t>(size), disc);
		std::printf("  probe score %d\n", score);
		Check(score >= 50, "probe recognises the disc");
	}

	WNDCLASSW wc{};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"NacrePS2Smoke";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	RECT r = {0, 0, 1280, 896};
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowW(wc.lpszClassName, L"nacre_ps2 smoke test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
	Pump(100);

	for (int i = 0; i < rounds; i++)
	{
		std::printf("round %d\n", i + 1);
		if (!RunRound(core, hwnd, disc, bios_dir, seconds, i == rounds - 1 ? capture : nullptr))
			break;
	}

	DestroyWindow(hwnd);
	std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
	std::fflush(stdout);
	// Leave unloading to process exit, as a frontend would at quit.
	return g_failures ? 1 : 0;
}

#endif
