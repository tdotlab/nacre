# Nacre architecture

The CHIP-8 machine, scheduler, and oscillator are C99, and so is the Game Boy core.
Platform interfaces own windows, input, ROM selection, preferences, files, and
audio devices.

| Entry point | Responsibility |
| --- | --- |
| `src/chip8.c`, `include/chip8.h` | Machine state, instructions, ROM bytes, key edges |
| `src/scheduler.c` | Independent CPU / 60 Hz timer clocks, bounded catch-up, pause |
| `src/audio.c` | Shared tone generator, CLI WAV and optional SDL2 live sink |
| `include/gb.h`, `src/gb/gb.c` | Game Boy API, bus/memory map, timer, serial, OAM DMA, joypad, audio ring |
| `src/gb/gb_cpu.c` | SM83 CPU, interrupts, HALT/EI/DI timing |
| `src/gb/gb_ppu.c` | Per-dot PPU: modes, STAT, line render (BG, window, sprites) |
| `src/gb/gb_apu.c` | Four channels, frame sequencer, 48 kHz stereo mix |
| `src/gb/gb_cart.c` | Header checks, MBC1/2/3/5, RTC, `.sav` format |
| `tests/test_gb.c`, `tools/get-gb-test-roms.py` | ROM-driven core checks; headless run/dump/press tool |
| `src/macos/main.m` | ARC-managed AppKit app, native menus/panels, display, controls, SDL3 audio |
| `src/macos/Info.plist.in` | Mac identity, version, document associations, OS requirement |
| `src/windows/main.cpp` | Win32 + GDI+ app: window, owner-drawn controls, menus, dialogs, drop target, registry prefs, SDL3 audio, Game Boy display/keys/`.sav` |
| `src/windows/branding.h` | Shell mark and icon geometry (same shapes as the Mac header), shared by the app and the icon tool |
| `src/windows/nacre.rc.in`, `nacre.manifest` | Embedded icon and welcome ROM, version info; comctl32 v6, PerMonitorV2 DPI |
| `tools/windows-icon.cpp` | Build-time generator of `Nacre.ico` (PNG frames, 16–256 px) |
| `src/desktop/main.c` | SDL3 window/input/file picker/drop/audio for Linux, CHIP-8 and Game Boy |
| `src/main.c` | Terminal and headless CLI |
| `CMakeLists.txt` | Platform selection, static SDL3, bundle/resources, Windows icon/rc/manifest |
| `.github/workflows/desktop.yml` | OS builds, universal Mac artifact, native UI smoke capture on Mac and Windows |

## Thread and state ownership

AppKit, the Win32 message loop, and the SDL event loop each run the machine on
their main thread. The audio callback reads an atomic active flag and owns its
oscillator. It never reads the machine directly. Mac schedules at 60 Hz using a
common-mode NSTimer; Windows uses a 16 ms `SetTimer` with `timeBeginPeriod(1)`.
Elapsed time comes from SDL's monotonic clock on both. The shared scheduler caps
catch-up at 250 ms.
Focus loss, panels, user pause, and execution faults stop advancement and silence
playback. CPU speed changes leave the CHIP-8 timer frequency unchanged.

Mac loads a candidate machine before replacing the current session, retains ROM
bytes for restart, and uses NSUserDefaults for speed/palette/mute. Invalid files
leave the current machine intact. Native file menus and drag-and-drop converge on
the same loader. Preferences never go inside the app bundle.

## Game Boy core

`Gb` is an opaque machine with no file, clock, or platform calls. Everything is
driven by memory cycles. Each CPU memory access first advances the whole machine
by one M-cycle (4 T-cycles), stepping the timer, serial, DMA, PPU, APU, and the
MBC3 clock in that order, and only then touches the bus.

Interrupts are sampled after the opcode-fetch cycle. When the CPU wakes from HALT,
the halted cycle stands in for that fetch. These two rules are what the Mooneye
timing tests pin down.

The PPU renders a whole line when mode 3 starts, so mid-line register tricks are
not modelled. Mode 3 length still follows SCX and the sprite count. The
framebuffer holds shade indices 0–3, and the frontends choose the colors.

Audio goes into a ring buffer of 48 kHz stereo `int16`. If nobody drains it, the
oldest samples are dropped. Saves are byte buffers: `gb_save_write`/`gb_save_load`
plus a dirty flag, and the frontend owns the `.sav` file. `gb_set_wall_clock`
feeds real time to the MBC3 clock, which then catches up when a save is loaded.

`gb_advance_ns` turns wall time into cycles at a speed percentage and carries the
fractional cycle, so a frontend calls it once per UI tick and stays exact over
long runs. Each frontend picks the core from the ROM's header
(`gb_rom_looks_valid`), not its extension. The Windows shell keeps both machines
alive and switches between them. It writes `.sav` files a second after the cart
goes dirty, and on restart, load and exit, through a temporary file and
`MoveFileEx`. Game Boy audio is pushed into its own SDL3 stream: stereo, no
callback, 48 kHz, resampled for 0.5×/2×. The queue is held between about 40 and
150 ms, padding with silence when it runs short and dropping a batch when it
runs long.

## Presentation

Mac uses native AppKit controls and text drawing. Its custom view draws the shell
mark, sidebar, live keycaps, status, and display surround. The 64×32 framebuffer
uses centered integer scaling. Fern, Amber, and Pearl change pixel ink; the dark
stage and warm chrome stay stable. The minimum window keeps all controls visible.
The current design is intentionally light even under macOS dark appearance.

The Mac shell is Objective-C to call the existing C APIs directly and use Cocoa
without another binding layer. No emulator code depends on AppKit.

The Windows shell is C++ over raw Win32 (no framework, static CRT, one exe). One
back-buffered GDI+ surface paints the sidebar, header, keycaps, and display with
clip-based partial repaints; buttons are owner-drawn standard `BUTTON` controls so
focus, Tab order, and screen-reader names stay native, and the speed/palette
dropdowns open native popup menus. Layout is in logical units scaled by the window
DPI (PerMonitorV2). The keypad maps from scan codes, so it works on any keyboard
layout, and game keys are routed even when a sidebar button has focus. ROM loading,
the Open dialog (`IFileOpenDialog`), drag-and-drop (`IDropTarget`), and recents all
converge on one loader that validates before replacing the session. Preferences and
recents live under `HKCU\Software\tdotlab\Nacre`; `--snapshot` runs without touching
them. Linux retains the SDL shell and can adopt the visual language independently.
