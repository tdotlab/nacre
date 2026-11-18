// SPDX-FileCopyrightText: 2026 tdotlab
// SPDX-License-Identifier: GPL-3.0+

// nacre_ps2_smoke: loads nacre_ps2 the way a Nacre frontend would and boots a disc
// in a plain Win32 window.
//
//   nacre_ps2_smoke <disc> <bios-dir> [seconds=20] [capture.bmp] [rounds=1]
//
// Checks: the table loads, a VM starts, frames are presented, pausing stops frame
// production and resuming restarts it, and the core tears down (and boots again
// when rounds > 1). Prints PASS or FAIL lines and exits non-zero on failure.

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

typedef const NacreCore* (*PluginFn)(void);
typedef uint64_t (*FramesFn)(void);

static FramesFn g_frames;
static int g_failures;

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

	const uint64_t start_frames = g_frames ? g_frames() : 0;
	core->set_running(self, 1);
	Pump(static_cast<DWORD>(seconds) * 500);

	const uint64_t mid = g_frames ? g_frames() : 0;
	std::printf("  %llu frames after %ds\n", static_cast<unsigned long long>(mid - start_frames), seconds / 2);
	Check(mid > start_frames, "frames presented");

	// Pause must stop frame production; resume must restart it.
	core->set_running(self, 0);
	const uint64_t paused_at = g_frames ? g_frames() : 0;
	Pump(1000);
	const uint64_t paused_after = g_frames ? g_frames() : 0;
	std::printf("  %llu frames while paused 1s\n", static_cast<unsigned long long>(paused_after - paused_at));
	Check(paused_after - paused_at <= 2, "pause stops frames");
	core->set_running(self, 1);

	// Exercise input: tap Start.
	core->set_pad(self, 0, NACRE_PAD_BIT(NACRE_PAD_START));
	Pump(200);
	core->set_pad(self, 0, 0);

	Pump(static_cast<DWORD>(seconds) * 500);
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
		Check(SaveCapture(hwnd, capture), "capture shows an image");

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

	// The DLL lives beside this exe, with its own dependency DLLs.
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dll(exe);
	dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"nacre_ps2.dll";
	HMODULE mod = LoadLibraryExW(dll.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	Check(mod != nullptr, "LoadLibrary nacre_ps2.dll");
	if (!mod)
	{
		std::printf("  error %lu\n", GetLastError());
		return 1;
	}
	auto plugin = reinterpret_cast<PluginFn>(GetProcAddress(mod, "nacre_plugin_core"));
	g_frames = reinterpret_cast<FramesFn>(GetProcAddress(mod, "nacre_ps2_frames_presented"));
	Check(plugin != nullptr && g_frames != nullptr, "exports found");
	if (!plugin)
		return 1;
	const NacreCore* core = plugin();
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
