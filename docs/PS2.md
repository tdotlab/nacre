# PS2 core plan (PCSX2)

The PS2 core will be PCSX2's emulator library behind `NacreCore`, with Nacre's
own adapter where PCSX2's Qt app would be. This note maps the two interfaces and
lists what has to change on each side. Upstream reference: PCSX2 `8407746`
(2026-11-16), GPL-3.0+, the same license Nacre moved to for this.

## The seam

PCSX2 already separates the emulator from its frontend. `pcsx2/` is the machine
plus `VMManager` (boot, run, pause, states, speed). The frontend supplies about 60
functions in the `Host::` namespace: settings, OSD text, the render window, thread
hand-offs and VM lifecycle notices. `pcsx2-gsrunner/Main.cpp` is a Qt-free
frontend that implements all of them in about 1,200 lines, mostly as stubs. The
Nacre adapter starts as a copy of that file's `Host::` half, which proves the core
links and runs without Qt.

PCSX2's own agent rules forbid automated issues, PRs or comments upstream.
Nacre's adapter work stays in Nacre and its copy of PCSX2; nothing gets posted
to the PCSX2 project.

## Mapping

| Nacre | PCSX2 |
| --- | --- |
| `NACRE_CORE_THREADED`, `set_running` | The adapter owns a CPU thread that calls `VMManager::Initialize` then loops `VMManager::Execute`; `set_running` maps to `VMManager::SetPaused` |
| `NACRE_CORE_SINGLETON` | PCSX2 is global state throughout; one VM per process |
| `NACRE_CORE_NEEDS_PATH` | Disc images by path (`VMBootParameters::filename`); ISO, CHD, CSO, ELF |
| `NACRE_CORE_NEEDS_SYSTEM_FILES`, `system_dir` | `Folders/Bios` and `Filenames/BIOS` settings; the adapter points the folder at `system_dir` and picks the BIOS whose region matches the disc |
| `save_dir`, battery hooks report 0 | Memory cards are PCSX2's own files (`Folders/MemoryCards`) pointed into `save_dir`; they are per-system, not per-game, so the session's `.sav` does not apply |
| `NACRE_CORE_NATIVE_VIDEO`, `attach_surface(NacreSurface)` | `Host::AcquireRenderWindow` returns a `WindowInfo`. The fields line up: kind→`type`, handle→`window_handle`, display→`display_connection`, width/height/scale→`surface_*`. On Mac PCSX2 makes its own `CAMetalLayer` on the NSView when `surface_handle` is empty. A re-attach after resize maps to `MTGS::ResizeDisplayWindow(width, height, scale)` |
| `wake` | Errors and VM stop (`Host::OnVMDestroyed`, `ReportErrorAsync`). Frames don't need it: the GS thread presents on its own |
| `set_speed` | `VMManager::SetLimiterMode`: 100% is `Nominal`, fast-forward is `Turbo`, slow is `Slomo` (their percentages are settings) |
| `audio_rate`, `audio_read` | **Gap.** SPU2 writes float stereo into an `AudioStream` that owns a cubeb or SDL device. Needs a small PCSX2 patch: a Nacre backend that resamples nothing and fills a ring the adapter converts to int16. Keeps mute, speed and the 40-150 ms queue policy in Nacre |
| `set_buttons`, `set_axis` | `Pad::SetControllerState(port, bind, value)` with the DualShock 2 binds. All 16 `NACRE_PAD_*` buttons exist on the DS2 (South=Cross, East=Circle, West=Square, North=Triangle). Sticks map to `PAD_L_*`/`PAD_R_*` as half-axes. PCSX2's own `InputManager` sources stay off so it never opens gamepads behind Nacre's back |
| `state_size/save/load` | **Mismatch.** PCSX2 states are zip files written asynchronously to a path (`VMManager::SaveState(filename, ...)`). The adapter can save to a temp file and read it back, which works but is slow. Better: give the session a path-based state hook for cores that write their own files |
| `set_option` | Writes into an `INISettingsInterface` the adapter owns (renderer, upscale, etc.) then `VMManager::ApplySettings` |
| `title` | `VMManager::GetTitle` |

## Interface changes this implies

Two changes to the Nacre interface, both additive:

- A path-based save-state hook (core writes and reads the state file itself), used
  by the session when present. PCSX2 states are tens of megabytes and already
  compressed; copying them through a buffer adds nothing.
- Native-surface frontends. No frontend calls `attach_surface` yet. The Windows
  frontend gets a child HWND inside the display well and forwards resizes; Mac
  gets an NSView. This is the next milestone, built against PCSX2 itself.

## Build

PCSX2 is C++20, built with CMake and its own dependency bundle
(`.github/workflows/scripts/windows/build-dependencies.bat`: SDL3, zstd, lz4,
libpng, libjpeg, webp, freetype, harfbuzz, shaderc, DirectX headers, Qt and
more). Qt and KDDockWidgets are only for `pcsx2-qt`; the core doesn't need them.

Two collisions with Nacre's current Windows build:

- C runtime: Nacre links it statically (`MultiThreaded`), PCSX2 and its
  dependencies use the DLL runtime. A process can't mix them safely across C++
  boundaries, so builds with the PS2 core switch Nacre to the DLL runtime.
- SDL3: Nacre compiles SDL 3.4.20 in statically; PCSX2 links its own SDL3 DLL.
  One SDL per process, so the PS2 build uses PCSX2's.

The PS2 core is an optional build (`NACRE_WITH_PS2`), so the default Nacre build
stays a single small executable with no dependency bundle. Windows x64 first.
Mac and Linux follow PCSX2's own CI scripts; whether PCSX2's recompilers run
natively on Apple Silicon has not been checked.

## Tracking upstream

Nacre carries its own copy of PCSX2 and changes it freely where that is cleaner
than working around it. To keep monthly upstream merges mechanical:

- The upstream base commit is recorded in one machine-readable file next to the
  copy, so a scheduled job can compare it with upstream `master`.
- Nacre's changes to PCSX2 sit on top of upstream as small, labelled commits, so a
  conflict names the patch it hit.
- Merges target a PCSX2 nightly tag, which has passed their CI, not an arbitrary
  commit.
- A merge is done when the PS2 smoke test passes: the test disc boots and produces
  frames through the Nacre adapter.

The owner plans a monthly scheduled check (Hermes) that reports a newer upstream
or dispatches an agent to do the merge.

## Status

Stock PCSX2 builds and runs on the owner's Windows machine (2026-11-17). Upstream
`8407746` is cloned outside the repo at `tools/source-code-of-open-source/pcsx2`.
The dependency bundle, Release build (Qt UI plus gsrunner, no LTO) and
`cmake --install` all succeed with VS 18 Build Tools. The SCPH-70012 USA BIOS
(v02.00) boots San Andreas (SLUS-20946) to the intro credits at 60 FPS on D3D12.
No Nacre code is involved yet.

Local build quirks, kept in `tools/source-code-of-open-source/pcsx2-local/`:

- `vswhere` needs `-products *` to see a Build Tools-only install.
- Archive symlinks need Developer Mode. Without it, a 7-Zip wrapper skips them.
- Ninja and CMake come from Build Tools' bundled copies.

Test BIOS and disc images live in `C:\Users\ringd\tools\ps2\`, never in a repo.
The next milestone is the adapter skeleton: `Host::` from gsrunner, a VMManager
thread, and a native-surface Windows frontend.
