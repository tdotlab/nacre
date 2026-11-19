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
| `audio_rate`, `audio_read`, `audio_clear` | SPU2 writes float stereo into an `AudioStream`. A PCSX2 patch (`SPU2::SetOutputStreamFactory`) lets the adapter supply its own: `NacreAudioStream`, with no device, stretching or resampling. `audio_read` drains its ring on the host's thread and converts to int16; anything older than a third of a second is dropped on read. Mute, speed and the 40-150 ms queue policy stay in Nacre |
| `set_pad`, `set_axis` | `Pad::SetControllerState(port, bind, value)` with the DualShock 2 binds. All 16 `NACRE_PAD_*` buttons exist on the DS2 (South=Cross, East=Circle, West=Square, North=Triangle). Sticks map to `PAD_L_*`/`PAD_R_*` as half-axes. PCSX2's own `InputManager` sources stay off so it never opens gamepads behind Nacre's back |
| `state_size/save/load` | **Mismatch.** PCSX2 states are zip files written asynchronously to a path (`VMManager::SaveState(filename, ...)`). The adapter can save to a temp file and read it back, which works but is slow. Better: give the session a path-based state hook for cores that write their own files |
| `set_option` | Writes into an `INISettingsInterface` the adapter owns (renderer, upscale, etc.) then `VMManager::ApplySettings` |
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

`Nacre.exe` loads plugins at startup from `<exe folder>\cores\<name>\nacre_*.dll`
and from each folder in `NACRE_CORE_DIRS` (separated by `;`). Built-in cores stay
first in detection order. A plugin that fails to load is logged and skipped.

For a native-video game the frontend shows a child window (`NacreDisplay`) in the
display well, fitted to 4:3, and passes it to the session; every resize, DPI
change and full-screen switch passes the new size. The child is hidden for
software-video cores. Core wakeups (errors, title changes, VM start/stop) arrive
as a posted message, coalesced, and run a normal tick.

Folders the core sees: `system_dir` is the folder chosen with File > System
Files Folder (stored as `systemDir`), else `%LOCALAPPDATA%\Nacre\System`, which
is created when a plugin needs system files. The PS2 core looks for BIOS images
there. `save_dir` is `%LOCALAPPDATA%\Nacre\Data`; PCSX2 keeps its data under
`ps2\` in it (logs, cache, and later memory cards).

The UI thread owns the display window, and DXGI sends it messages while the GS
thread creates, resizes or releases a swap chain. Every wait the adapter does on
the UI thread lets those sent messages through (`MsgWaitForMultipleObjectsEx`
with `QS_SENDMESSAGE`). Before that, closing a game after a resize and a
full-screen switch stalled for 5 to 13 seconds.

## Build

PCSX2 lives in this repo as a squashed `git subtree` at `third_party/pcsx2/`.
The adapter is `third_party/pcsx2/pcsx2-nacre/`, built by PCSX2's own CMake when
`ENABLE_NACRE_CORE=ON`, into a self-contained folder `<build>/nacre-ps2/`:

- `nacre_ps2.dll`: PCSX2 plus the adapter. Exports `nacre_plugin_core()`, which
  returns the `NacreCore` table, and `nacre_ps2_frames_presented()` for tests.
- `nacre_ps2_smoke.exe`: loads the DLL like a frontend, boots a disc in a Win32
  window, checks frames, audio rate and level, pause/resume and teardown, and
  saves a capture plus the audio as a `.wav` beside it.
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
  `pcsx2-nacre/`; edits to upstream files stay minimal and are listed under
  Status.
- Merges target a PCSX2 nightly tag, which has passed their CI, not an arbitrary
  commit.
- A merge is done when the PS2 smoke test passes: the test disc boots and produces
  frames through the Nacre adapter.

The owner plans a monthly scheduled check (Hermes) that reports a newer upstream
or dispatches an agent to do the merge.

## Status

**P1 done (2026-11-18): the adapter boots games through `NacreCore`, with audio.** On the
owner's Windows machine (i5-8400, GTX 1070), `nacre_ps2_smoke` boots San Andreas
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

The adapter covers threaded run/pause on its own CPU thread (a command queue
drained at vsync), native-surface attach and resize, DualShock 2 buttons and
sticks, speed (`Nominal`/`Turbo`/`Slomo`), title, BIOS choice, and the options
`bios`, `renderer`, `upscale`, `fast_boot` and `osd`, and audio through
`audio_read`. States and memory-card mapping are not wired, and PCSX2's own
on-screen messages are off by default.

Changes to upstream PCSX2 files so far:

- `CMakeLists.txt`: the `ENABLE_NACRE_CORE` option.
- `pcsx2/vtlb.cpp`: install the page-fault handler once per process. Upstream
  assumes one VM-memory allocation per process and asserts on the second boot.
- `pcsx2/SPU2/spu2.h`, `spu2.cpp`: `SPU2::SetOutputStreamFactory`, a host-supplied
  output stream that replaces the configured backend.

Known issues:

- A failed `pxAssertRel` opens a modal Win32 dialog (`common/Assertions.cpp`),
  which looks like a hang inside a frontend. On the owner's machine,
  `tools/junk/pcsx2-spike/read-dialogs.ps1` reads such dialogs with UI
  Automation. A host hook that logs and stops the core would be better.
- PCSX2's base settings layer can be set once per process, so the adapter keeps
  one `MemorySettingsInterface` and resets it on each `create`.
- PCSX2 empties the audio ring from the CPU thread when the speed changes
  (`SPU2::OnTargetSpeedChanged`), which races the reader as it does with
  PCSX2's own device backends. Worst case is one burst of stale audio.
- Not checked: sustained play, memory growth over many rounds, Vulkan and
  OpenGL, other games, PSX-mode (44.1 kHz) audio.

- The plugin is not built by CI or shipped beside `Nacre.exe`; a local run needs
  `NACRE_CORE_DIRS` or a copy under `cores\ps2\`.
- PCSX2 logs that `resources/patches.zip` is missing, so game patches are off.
- The palette control does nothing for PS2.

Next, in order: memory cards in `save_dir`; the path-based save-state hook; an
assert hook; packaging the plugin with the Windows build; Mac and Linux.

Local build quirks for the dependency bundle, kept in
`tools/source-code-of-open-source/pcsx2-local/` on the owner's machine:

- `vswhere` needs `-products *` to see a Build Tools-only install.
- Archive symlinks need Developer Mode. Without it, a 7-Zip wrapper skips them.
- Ninja and CMake come from Build Tools' bundled copies.

Test BIOS and disc images live in `C:\Users\ringd\tools\ps2\`, never in a repo.
