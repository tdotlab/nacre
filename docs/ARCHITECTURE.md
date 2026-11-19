# Nacre architecture

The CHIP-8 machine, scheduler, and oscillator are C99, and so is the Game Boy core.
Platform interfaces own windows, input, ROM selection, preferences, files, and
audio devices.

| Entry point | Responsibility |
| --- | --- |
| `src/chip8.c`, `include/chip8.h` | Machine state, instructions, ROM bytes, key edges |
| `src/scheduler.c` | Independent CPU / 60 Hz timer clocks, bounded catch-up, pause |
| `src/audio.c` | Shared tone generator, CLI WAV and optional SDL2 live sink |
| `include/gb.h`, `src/gb/gb.c` | Game Boy API, bus/memory map, post-boot state, timer, DIV-clocked serial, OAM DMA, joypad, audio ring |
| `src/gb/gb_cpu.c` | SM83 CPU, interrupts, HALT/EI/DI timing, OAM-bug triggers on bus and IDU cycles |
| `src/gb/gb_ppu.c` | Per-dot PPU: modes, STAT/LYC edges, VRAM/OAM access windows, span render (BG, window, sprites), OAM corruption |
| `src/gb/gb_apu.c` | Four channels, frame sequencer, 48 kHz stereo mix |
| `src/gb/gb_cart.c` | Header checks, MBC1 (and multicart)/2/3/5, RTC, `.sav` format |
| `src/gb/gb_state.c` | Game Boy save states: whole-machine snapshot with a build/ROM check; bad input is rejected without side effects |
| `include/gb_input.h`, `src/gb_input.c` | Platform-neutral Game Boy key bindings: defaults, lookup, steal-on-assign, text form for preferences |
| `src/desktop/pad.c` | SDL3 gamepad layer shared by the Mac, Windows and Linux frontends |
| `include/nacre_core.h`, `src/core/registry.c` | System-agnostic core interface (stepped or threaded cores, framebuffer or native-surface video, emulation-locked stereo audio, positional pad, battery saves, states, options) and the built-in core registry with probe-based detection |
| `src/core/chip8_core.c`, `src/core/gb_core.c` | CHIP-8 and Game Boy behind the core interface |
| `include/nacre_session.h`, `src/core/session.c` | One running game: core choice, candidate-swap open, sibling `.sav` with debounced writes, `.state1`..`.state4` slots, remembered per-core options, the native-video surface (attached after load, before the first run); file I/O through host callbacks |
| `src/desktop/audio_out.c` | SDL3 output for any core: one stream at the core's rate, speed as frequency ratio, 40-150 ms queue, silent fast-forward |
| `tests/test_gb.c`, `tools/get-gb-test-roms.py` | ROM-driven core checks; headless run/dump/press tool |
| `src/macos/main.m` | ARC-managed AppKit app: native menus/panels, drawn sidebar and display, rebinding sheet; runs every registered core through a session, with the same per-system look table as Windows |
| `src/macos/Info.plist.in` | Mac identity, version, document associations, OS requirement |
| `src/windows/main.cpp` | Win32 + GDI+ app: window, owner-drawn controls, menus, dialogs, drop target, registry prefs; runs every registered core through a session, with per-system looks (keypad or pad, speed in Hz or ×) keyed by core id; loads plugin cores (`cores\<name>\nacre_*.dll`, `NACRE_CORE_DIRS`) and gives native-video ones a child window in the display well |
| `third_party/pcsx2/` | PCSX2 as a squashed git subtree; `pcsx2-nacre/` is the PS2 adapter, built as the `nacre_ps2.dll` plugin (see docs/PS2.md) |
| `src/windows/branding.h` | Shell mark and icon geometry (same shapes as the Mac header), shared by the app and the icon tool |
| `src/windows/nacre.rc.in`, `nacre.manifest` | Embedded icon and welcome ROM, version info; comctl32 v6, PerMonitorV2 DPI |
| `tools/windows-icon.cpp` | Build-time generator of `Nacre.ico` (PNG frames, 16–256 px) |
| `src/desktop/main.c` | SDL3 window/input/file picker/drop for Linux; runs every registered core through a session and knows no system itself |
| `src/main.c` | Terminal and headless CLI |
| `CMakeLists.txt` | Platform selection, static SDL3, bundle/resources, Windows icon/rc/manifest |
| `.github/workflows/desktop.yml` | OS builds, universal Mac artifact, native UI smoke capture on Mac and Windows |

## Thread and state ownership

AppKit, the Win32 message loop, and the SDL event loop each run the game on
their main thread. Each holds one `NacreSession` and calls
`nacre_session_tick` each UI tick; `audio_out` pulls the core's samples into one
SDL3 stream on that same thread. No frontend includes a system's own header: it
reads frames, audio and labels through the core table and keys per-system wording
off the core id. Mac schedules at 60 Hz using a
common-mode NSTimer; Windows uses a 16 ms `SetTimer` with `timeBeginPeriod(1)`.
Elapsed time comes from SDL's monotonic clock on both. The shared scheduler caps
catch-up at 250 ms.
Focus loss, panels, user pause, and execution faults stop advancement and silence
playback. CPU speed changes leave the CHIP-8 timer frequency unchanged.

The session loads a candidate core before replacing the running one, so invalid
files leave the current game intact. Mac uses NSUserDefaults for speed, palette,
mute, bindings and the state slot. Native file menus and drag-and-drop converge on
the same loader. Preferences never go inside the app bundle.

## Game Boy core

`Gb` is an opaque machine with no file, clock, or platform calls. Everything is
driven by memory cycles. Each CPU memory access first advances the whole machine
by one M-cycle (4 T-cycles), stepping the timer, serial, DMA, PPU, APU, and the
MBC3 clock in that order, and only then touches the bus.

Interrupts are sampled after the opcode-fetch cycle. When the CPU wakes from HALT,
the halted cycle stands in for that fetch. These two rules are what the Mooneye
timing tests pin down.

Reset leaves the machine where the DMG boot ROM hands off: registers, I/O
values, and a DIV phase taken from Mooneye's `boot_*` tests. The serial port
shifts on falling edges of bit 8 of the DIV counter, as the hardware does, so an
internally clocked transfer takes 4096 T-cycles and lines up with DIV. The
capture log records the byte the CPU last wrote to SB, because test ROMs often
overwrite SB before a slow transfer finishes.

The PPU keeps the dot timings the Mooneye PPU tests measure. They are listed at
the top of `gb_ppu.c`: when VRAM and OAM lock and open for the CPU, when STAT
modes change, and when LY=LYC is compared and dropped. STAT is an edge-triggered
OR of its sources, and a DMG STAT write raises the spurious interrupt. Pixels are
drawn in spans. A write to a PPU register during mode 3 first draws the line up
to the current pixel, so mid-line palette, scroll and LCDC changes land at about
the right place. A full pixel FIFO is not modelled, so effects that depend on
exact fetcher timing are approximate. Mode 3 length follows SCX, sprites and the
window. The framebuffer holds shade indices 0–3, and the frontends choose the
colors.

The OAM corruption bug follows Pan Docs. During the OAM scan the PPU reads one
8-byte row per M-cycle, and a CPU access at dot 4k lands on row k+1. The CPU
reports a read, a write, or a read in the same cycle as a 16-bit
increment/decrement whenever the address or register value is in $FE00–$FEFF.
That covers `inc/dec rr`, `ld a,(hl±)`, push, pop, call, rst, interrupt dispatch
and fetches from OAM. `gb_oam_bug` then garbles that row from the row before it.

Audio goes into a ring buffer of 48 kHz stereo `int16`. If nobody drains it, the
oldest samples are dropped. Saves are byte buffers: `gb_save_write`/`gb_save_load`
plus a dirty flag, and the frontend owns the `.sav` file. `gb_set_wall_clock`
feeds real time to the MBC3 clock, which then catches up when a save is loaded.

`gb_advance_ns` turns wall time into cycles at a speed percentage and carries the
fractional cycle, so a frontend calls it once per UI tick and stays exact over
long runs. Cores are picked from the file's contents (`gb_rom_looks_valid`
behind the registry's probe), not its extension. The session owns the `.sav` on every
frontend. Saves are written a second after the cart goes dirty, and on
restart, load and exit, through a temporary file and an atomic rename
(`MoveFileEx` on Windows, `NSDataWritingAtomic` on Mac, `SDL_RenamePath` on
Linux). The Mac draws multi-shade and RGB frames through a CGBitmapContext image with
interpolation off, flipped for its top-left view coordinates. Core audio is pushed into one SDL3 stream: stereo, no
callback, at the core's rate, resampled for 0.5×/2×. The queue is held between about 40 and
150 ms, padding with silence when it runs short and dropping a batch when it
runs long.

## Presentation

Mac uses native AppKit controls and text drawing. Its custom view draws the shell
mark, sidebar, live keycaps (a keypad or a pad cross, from the core's table), status, and display
surround. Square-pixel frames use centered integer scaling; others fit their aspect. Fern, Amber, and Pearl change pixel ink; the dark
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
