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
| `set_pad`, `set_axis` | `Pad::SetControllerState(port, bind, value)` with the DualShock 2 binds. All 16 `NACRE_PAD_*` buttons exist on the DS2 (South=Cross, East=Circle, West=Square, North=Triangle). Sticks map to `PAD_L_*`/`PAD_R_*` as half-axes. PCSX2's own `InputManager` sources stay off so it never opens gamepads behind Nacre's back |
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

PCSX2 lives in this repo as a squashed `git subtree` at `third_party/pcsx2/`.
The adapter is `third_party/pcsx2/pcsx2-nacre/`, built by PCSX2's own CMake when
`ENABLE_NACRE_CORE=ON`, into a self-contained folder `<build>/nacre-ps2/`:

- `nacre_ps2.dll`: PCSX2 plus the adapter. Exports `nacre_plugin_core()`, which
  returns the `NacreCore` table, and `nacre_ps2_frames_presented()` for tests.
- `nacre_ps2_smoke.exe`: loads the DLL like a frontend, boots a disc in a Win32
  window, checks frames, pause/resume and teardown, and saves a capture.
- PCSX2's runtime DLLs (SDL3, zstd, shaderc, ffmpeg, ...), `D3D12/` and `resources/`.

The core is a runtime plugin rather than linked into Nacre because the two
builds disagree: Nacre links the C runtime and SDL3 statically; PCSX2 and its
dependency bundle use the DLL runtime and SDL3.dll. Behind a plain-C table those
never meet, so the default Nacre build stays one small executable and needs no
dependency bundle. A frontend loads the plugin with
`LoadLibraryEx(..., LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`
so its dependencies resolve from its own folder. PCSX2's SDL input and audio are
off, so its SDL copy never touches devices Nacre's SDL owns.

Windows build, from a VS x64 developer prompt (VS 18 Build Tools works):

```
cd third_party\pcsx2
mklink /J deps <path to a built PCSX2 dependency bundle>
cmake . -B build-nacre -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%CD%\deps ^
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_NACRE_CORE=ON -DDISABLE_ADVANCE_SIMD=ON
cmake --build build-nacre --target nacre_ps2 nacre_ps2_smoke
build-nacre\nacre-ps2\nacre_ps2_smoke.exe <disc> <bios-dir> [seconds] [capture.bmp] [rounds]
```

The dependency bundle comes from
`.github/workflows/scripts/windows/build-dependencies.bat` (see Status for the
local quirks). `deps/` and `build*/` are ignored. Windows x64 first; Mac and
Linux follow PCSX2's own CI scripts, and whether its recompilers run natively on
Apple Silicon has not been checked.

## Tracking upstream

Nacre carries its own copy of PCSX2 and changes it freely where that is cleaner
than working around it. To keep monthly upstream merges mechanical:

- The upstream base is recorded in `third_party/PCSX2_UPSTREAM` (key=value: repo,
  tag, commit, date), so a scheduled job can compare it with upstream tags. Sync
  with `git subtree pull --prefix=third_party/pcsx2 <repo> <tag> --squash`, then
  update that file.
- Nacre's changes to PCSX2 sit on top of upstream as small commits prefixed
  `pcsx2:`, so a conflict names the patch it hit. New files go in
  `pcsx2-nacre/`; edits to upstream files stay minimal (today: one `option` and
  `add_subdirectory` in the top-level `CMakeLists.txt`).
- Merges target a PCSX2 nightly tag, which has passed their CI, not an arbitrary
  commit.
- A merge is done when the PS2 smoke test passes: the test disc boots and produces
  frames through the Nacre adapter.

The owner plans a monthly scheduled check (Hermes) that reports a newer upstream
or dispatches an agent to do the merge.

## Status

**P1 done (2026-11-18): the adapter boots games through `NacreCore`.** On the
owner's Windows machine (i5-8400, GTX 1070), `nacre_ps2_smoke` boots San Andreas
(SLUS-20946) with the SCPH-70012 USA BIOS, picked by region from a folder of
three. It passes every check over three create/run/pause/destroy rounds in one
process. Steady state is ~60 presented frames per second on D3D12 after a few
seconds of shader compilation. Pause stops frames within a vsync, resume
restarts them, and a capture at 40 s shows the intro movie with no PCSX2 UI.
No Nacre frontend loads the plugin yet; that is the next milestone.

The adapter covers threaded run/pause on its own CPU thread (a command queue
drained at vsync), native-surface attach and resize, DualShock 2 buttons and
sticks, speed (`Nominal`/`Turbo`/`Slomo`), title, BIOS choice, and the options
`bios`, `renderer`, `upscale`, `fast_boot` and `osd`. Audio is stubbed
(`SPU2/Output` is `Null`), states and memory-card mapping are not wired, and
PCSX2's own on-screen messages are off by default.

Changes to upstream PCSX2 files so far:

- `CMakeLists.txt`: the `ENABLE_NACRE_CORE` option.
- `pcsx2/vtlb.cpp`: install the page-fault handler once per process. Upstream
  assumes one VM-memory allocation per process and asserts on the second boot.

Known issues:

- A failed `pxAssertRel` opens a modal Win32 dialog (`common/Assertions.cpp`),
  which looks like a hang inside a frontend. On the owner's machine,
  `tools/junk/pcsx2-spike/read-dialogs.ps1` reads such dialogs with UI
  Automation. A host hook that logs and stops the core would be better.
- PCSX2's base settings layer can be set once per process, so the adapter keeps
  one `MemorySettingsInterface` and resets it on each `create`.
- Not checked: sustained play, memory growth over many rounds, Vulkan and
  OpenGL, other games.

Next, in order: a Nacre `AudioStream` backend feeding `audio_read`; Windows
frontend plugin loading and `attach_surface` (a child HWND in the display well),
with `system_dir` and `save_dir` wiring; memory cards in `save_dir`; the
path-based save-state hook; Mac and Linux.

Local build quirks for the dependency bundle, kept in
`tools/source-code-of-open-source/pcsx2-local/` on the owner's machine:

- `vswhere` needs `-products *` to see a Build Tools-only install.
- Archive symlinks need Developer Mode. Without it, a 7-Zip wrapper skips them.
- Ninja and CMake come from Build Tools' bundled copies.

Test BIOS and disc images live in `C:\Users\ringd\tools\ps2\`, never in a repo.
