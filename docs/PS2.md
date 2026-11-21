# PS2 core

Nacre's PS2 core is a built-in core like Game Boy and CHIP-8: `nacre_ps2_core` in
`src/core/ps2_core.cpp`, registered in `src/core/registry.c`. The emulator under
it, `src/ps2/`, is derived from PCSX2 by the PCSX2 Dev Team (https://pcsx2.net,
GPL-3.0+, the same license Nacre moved to for this). It was taken from PCSX2
v2.9.181 (`8407746`, 2026-11-16), recorded in `src/ps2/UPSTREAM`, and Nacre
changes it as its own code. This note maps the two interfaces, the layout, the
build and the status.

## The seam

PCSX2 already separates the emulator from its frontend. `pcsx2/` is the machine
plus `VMManager` (boot, run, pause, states, speed). The frontend supplies about 60
functions in the `Host::` namespace: settings, OSD text, the render window, thread
hand-offs and VM lifecycle notices. `pcsx2-gsrunner/Main.cpp` is a Qt-free
frontend that implements all of them in about 1,200 lines, mostly as stubs. The
Nacre core began as a copy of that file's `Host::` half. Qt and the gsrunner are
not part of Nacre.

PCSX2's own agent rules forbid automated issues, PRs or comments upstream.
Nacre's work stays in Nacre; nothing gets posted to the PCSX2 project.

## Mapping

| Nacre | PCSX2 |
| --- | --- |
| `NACRE_CORE_THREADED`, `set_running` | The core owns a CPU thread that calls `VMManager::Initialize` then loops `VMManager::Execute`; `set_running` maps to `VMManager::SetPaused` |
| `NACRE_CORE_SINGLETON` | PCSX2 is global state throughout; one VM per process |
| `NACRE_CORE_NEEDS_PATH` | Disc images by path (`VMBootParameters::filename`); ISO, CHD, CSO, ELF |
| `NACRE_CORE_NEEDS_SYSTEM_FILES`, `system_dir` | `Folders/Bios` and `Filenames/BIOS` settings; the core points the folder at `system_dir` and picks the BIOS whose region matches the disc |
| `save_dir`, battery hooks report 0 | Memory cards are PCSX2's own files (`Folders/MemoryCards`) pointed into `save_dir`; they are per-system, not per-game, so the session's `.sav` does not apply |
| `NACRE_CORE_NATIVE_VIDEO`, `attach_surface(NacreSurface)` | `Host::AcquireRenderWindow` returns a `WindowInfo`. The fields line up: kind→`type`, handle→`window_handle`, display→`display_connection`, width/height/scale→`surface_*`. On Mac PCSX2 makes its own `CAMetalLayer` on the NSView when `surface_handle` is empty. A re-attach after resize maps to `MTGS::ResizeDisplayWindow(width, height, scale)` |
| `wake` | Errors and VM stop (`Host::OnVMDestroyed`, `ReportErrorAsync`). Frames don't need it: the GS thread presents on its own |
| `set_speed` | `VMManager::SetLimiterMode`: 100% is `Nominal`, fast-forward is `Turbo`, slow is `Slomo` (their percentages are settings) |
| `audio_rate`, `audio_read`, `audio_clear` | SPU2 writes float stereo into an `AudioStream`. A PCSX2 patch (`SPU2::SetOutputStreamFactory`) lets the core supply its own: `NacreAudioStream`, with no device, stretching or resampling. `audio_read` drains its ring on the host's thread and converts to int16; anything older than a third of a second is dropped on read. Mute, speed and the 40-150 ms queue policy stay in Nacre |
| `set_pad`, `set_axis` | `Pad::SetControllerState(port, bind, value)` with the DualShock 2 binds. All 16 `NACRE_PAD_*` buttons exist on the DS2 (South=Cross, East=Circle, West=Square, North=Triangle). Sticks map to `PAD_L_*`/`PAD_R_*` as half-axes. PCSX2's own `InputManager` sources stay off so it never opens gamepads behind Nacre's back |
| `state_size/save/load` | **Mismatch.** PCSX2 states are zip files written asynchronously to a path (`VMManager::SaveState(filename, ...)`). The core can save to a temp file and read it back, which works but is slow. Better: give the session a path-based state hook for cores that write their own files |
| `set_option` | Writes into an `INISettingsInterface` the core owns (renderer, upscale, etc.) then `VMManager::ApplySettings` |
| `title` | `VMManager::GetTitle` |

## Interface changes this implies

Two changes to the Nacre interface, both additive:

- A path-based save-state hook (core writes and reads the state file itself), used
  by the session when present. PCSX2 states are tens of megabytes and already
  compressed; copying them through a buffer adds nothing. Not built yet.
- Native-surface frontends. Done on Windows: `nacre_session_set_surface` stores
  where native-video cores present, the session attaches it after a load and
  before the first `set_running`, and refuses to open such a game without one.
  Mac (an NSView) and Linux are not built.

## Windows frontend

The PS2 core is in `nacre_cores()` like every other core, after Game Boy and
CHIP-8, so it wins detection only for files they don't claim.

For a native-video game the frontend shows a child window (`NacreDisplay`) in the
display well, fitted to 4:3, and passes it to the session; every resize, DPI
change and full-screen switch passes the new size. The child is hidden for
software-video cores. Core wakeups (errors, title changes, VM start/stop) arrive
as a posted message, coalesced, and run a normal tick.

Folders the core sees: `system_dir` is the folder chosen with File > System
Files Folder (stored as `systemDir`), else `%LOCALAPPDATA%\Nacre\System`, which
is created at startup when a core needs system files. The PS2 core looks for BIOS
images there. `save_dir` is `%LOCALAPPDATA%\Nacre\Data`; the core keeps its data
under `ps2\` in it (logs, cache, and later memory cards). Its `resources\` folder
(fonts, shaders, game database) must sit beside `Nacre.exe`.

The UI thread owns the display window, and DXGI sends it messages while the GS
thread creates, resizes or releases a swap chain. Every wait the core does on
the UI thread lets those sent messages through (`MsgWaitForMultipleObjectsEx`
with `QS_SENDMESSAGE`). Before that, closing a game after a resize and a
full-screen switch stalled for 5 to 13 seconds.

## Layout

| Path | What |
| --- | --- |
| `src/core/ps2_core.cpp` | The `NacreCore` table: CPU thread, `Host::` functions, audio stream, pad, options |
| `src/ps2/` | The emulator (EE/IOP recompilers, GS renderers, SPU2, CDVD, ...), derived from PCSX2's `pcsx2/` |
| `src/ps2/common/` | Its support library, derived from PCSX2's `common/` |
| `src/ps2/CMakeLists.txt` | Nacre's glue: keeps PCSX2's compiler settings inside this directory and builds `nacre_ps2` |
| `src/ps2/targets.cmake` | The emulator's source list and per-ISA GS libraries |
| `src/ps2/UPSTREAM` | The PCSX2 release the code derives from |
| `cmake/ps2/` | Build modules derived from PCSX2's (`SearchForStuff`, `BuildParameters`, ...) |
| `third_party/<lib>` | Libraries the core builds from source: fmt, rcheevos, imgui, xbyak, cpuinfo, Vulkan headers, ... |
| `assets/ps2/` | `resources/`, copied beside the app, and the dependency licenses, shipped as `PS2_THIRD_PARTY_LICENSES.html` |
| `tools/ps2-deps/` | The script that builds the prebuilt dependency bundle |
| `tests/test_ps2_smoke.cpp` | `nacre-ps2-smoke`, the PS2 boot test |

## Build

`NACRE_PS2` turns the core on. It defaults on for Windows when a dependency bundle
exists at `NACRE_PS2_DEPS` (default `deps/ps2`, ignored by Git) and off
otherwise, so a plain checkout still builds the small CHIP-8 and Game Boy app.
From a VS x64 developer prompt (VS 18 Build Tools works):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDISABLE_ADVANCE_SIMD=ON
cmake --build build --target nacre-desktop nacre-ps2-smoke
build\nacre-ps2-smoke.exe <disc> <bios-dir> [seconds] [capture.bmp] [rounds]
```

With PS2 on, a post-build step copies the bundle's DLLs (zstd, shaderc, ffmpeg,
...), `D3D12\`, `resources\` and `PS2_THIRD_PARTY_LICENSES.html` beside
`Nacre.exe`. The whole app then uses the DLL C runtime (`/MD`), because the
bundle does, so it needs the VC++ redistributable; without PS2 it stays one
statically linked exe. The core shares Nacre's static SDL3, and its SDL input and
audio are off, so it never opens devices behind Nacre's back.

`nacre-ps2-smoke` gets the core from the registry like a frontend, boots a disc
in a Win32 window, checks frames, audio rate and level, pause/resume and
teardown, and saves a capture plus the audio as a `.wav` beside it.

The dependency bundle comes from `tools/ps2-deps/build-dependencies.bat`, which
builds into `deps\ps2-build` and installs into `deps\ps2`. It still builds Qt,
which the core doesn't need, and it has not been run from its new location.
Windows x64 first; Mac and Linux follow PCSX2's own CI scripts, and whether its
recompilers run natively on Apple Silicon has not been checked.

## Tracking upstream

The core is Nacre's code now; there is no subtree to pull. PCSX2 fixes come in
by hand:

- `src/ps2/UPSTREAM` records the PCSX2 release the code derives from (key=value:
  repo, tag, commit, date). The build reads it for version strings, and a
  scheduled job can compare it with upstream tags.
- To port a release, diff PCSX2 between the recorded tag and the new one for
  `pcsx2/`, `common/`, `cmake/` and the `3rdparty/` libraries Nacre uses, apply
  what matters to the matching Nacre paths, then update `UPSTREAM`. Commits that
  port upstream work are prefixed `ps2: port`.
- Port from PCSX2 nightly tags, which have passed their CI.
- A port is done when `nacre-ps2-smoke` passes: the test disc boots and produces
  frames.

The owner plans a monthly scheduled check (Hermes) that reports a newer upstream
release.

## Status

**P1 done (2026-11-18): the core boots games through `NacreCore`, with audio.** On the
owner's Windows machine (i5-8400, GTX 1070), the smoke test boots San Andreas
(SLUS-20946) with the SCPH-70012 USA BIOS, picked by region from a folder of
three. It passes every check over three create/run/pause/destroy rounds in one
process. Steady state is ~60 presented frames per second on D3D12 after a few
seconds of shader compilation. Pause stops frames within a vsync, resume
restarts them, and a capture at 40 s shows the intro movie with no PCSX2 UI.
Audio drained the way the desktop frontend drains it arrives at 100% of 48 kHz
once boot is past, stops while paused, and the recorded intro is real sound
(about -18 dBFS of music and speech, stereo, no clipping or clicks).

**P2 done on Windows (2026-11-19): Nacre.exe plays PS2 discs.** Opening the San
Andreas ISO in the real app boots it inside the display well, with the header
showing the system and the game title. On the same machine, driven by
`tools/junk/pcsx2-spike/app-ps2.ps1` and `app-switch.ps1` (muted): the picture
follows a window resize and full screen, stays still while paused, switching to
the welcome demo takes about 310 ms after any mix of pause, resize and full
screen, and the app closes in about 150 ms with exit code 0. Nobody has played it
with a controller or listened to it through the app yet.

**Built in (2026-11-20).** The core moved from a plugin DLL into Nacre's own tree
and build. Rechecked on the same machine: `nacre-ps2-smoke` passes three rounds
(~57 presented frames per second, audio at 100% of 48 kHz), the app follows
resize, pause and full screen, and closes in about 170 ms. In 24 interleaved
runs against the old plugin build, switching to the demo took 306-337 ms in
both. `Nacre --snapshot` passes with and without `NACRE_PS2`.

The core covers threaded run/pause on its own CPU thread (a command queue
drained at vsync), native-surface attach and resize, DualShock 2 buttons and
sticks, speed (`Nominal`/`Turbo`/`Slomo`), title, BIOS choice, and the options
`bios`, `renderer`, `upscale`, `fast_boot` and `osd`, and audio through
`audio_read`. States and memory-card mapping are not wired, and PCSX2's own
on-screen messages are off by default.

Changes from PCSX2 beyond moving it into Nacre's tree:

- PCSX2's top-level CMake, Qt app, gsrunner, updater, tests and Visual Studio
  projects are gone; `src/ps2/CMakeLists.txt` takes the top-level file's place.
- Version strings come from `UPSTREAM` instead of Git.
- A few includes that named the old `pcsx2/` folder are relative now.
- `vtlb.cpp`: install the page-fault handler once per process. Upstream
  assumes one VM-memory allocation per process and asserts on the second boot.
- `SPU2/spu2.h`, `spu2.cpp`: `SPU2::SetOutputStreamFactory`, a host-supplied
  output stream that replaces the configured backend.

Known issues:

- A failed `pxAssertRel` opens a modal Win32 dialog (`common/Assertions.cpp`),
  which looks like a hang inside a frontend. On the owner's machine,
  `tools/junk/pcsx2-spike/read-dialogs.ps1` reads such dialogs with UI
  Automation. A host hook that logs and stops the core would be better.
- PCSX2's base settings layer can be set once per process, so the core keeps
  one `MemorySettingsInterface` and resets it on each `create`.
- PCSX2 empties the audio ring from the CPU thread when the speed changes
  (`SPU2::OnTargetSpeedChanged`), which races the reader as it does with
  PCSX2's own device backends. Worst case is one burst of stale audio.
- Switching away from a PS2 game after a pause took 0.95, 2.6 and 4.3 s in three
  of the first ~20 runs after the first built-in build, and never in the 24 runs
  after (either build). Cause unknown. One suspect: `set_running(0)` spins until
  the CPU thread parks without letting sent messages through.
- Not checked: sustained play, memory growth over many rounds, Vulkan and
  OpenGL, other games, PSX-mode (44.1 kHz) audio.

- CI does not build the PS2 core; it needs the dependency bundle.
- PCSX2 logs that `resources/patches.zip` is missing, so game patches are off.
- The palette control does nothing for PS2.

Next, in order: memory cards in `save_dir`; the path-based save-state hook; an
assert hook; a CI build with a cached dependency bundle; Mac and Linux.

Local build quirks for the dependency bundle, kept in
`tools/source-code-of-open-source/pcsx2-local/` on the owner's machine:

- `vswhere` needs `-products *` to see a Build Tools-only install.
- Archive symlinks need Developer Mode. Without it, a 7-Zip wrapper skips them.
- Ninja and CMake come from Build Tools' bundled copies.

Test BIOS and disc images live in `C:\Users\ringd\tools\ps2\`, never in a repo.
