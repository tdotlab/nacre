# Nacre roadmap

## Current state

Nacre has a C core, independent 60 Hz timer scheduling, real timer-driven sound,
a terminal/headless interface, a native AppKit Mac app, a native Win32/GDI+ Windows
app, and an SDL3 Linux frontend. See [architecture](ARCHITECTURE.md) for source
ownership and [README](../README.md) for setup and controls.

The Mac design adds warm pearl chrome, a dark integer-scaled display, three
phosphor palettes, native buttons and file sheets, recent files, drag-and-drop,
menus, fullscreen, persisted settings, and a live physical-keyboard guide.
The universal build targets macOS 11+. Its CI snapshot mode exercises pause,
mute, restart, and keypad down/up, then captures the real view.

Core, scheduler, and original welcome-ROM tests pass locally and in Linux CI.
The Mac interface build, native control/key smoke checks, and capture passed in
[CI run 43649531447](https://github.com/tdotlab/nacre/actions/runs/43649531447);
Windows and Linux builds also passed. Automated UI capture and compilation
are separate from hands-on input/audio acceptance. Live sound still needs a
listening check on real devices. Linux visual parity is not implemented.

The Windows app mirrors the Mac design (sidebar, display well, palettes, keycaps,
native menus/dialogs, drag-and-drop, recents, saved preferences, generated icon,
embedded welcome ROM) as one static exe. It builds warning-free at /W4 with MSVC,
and its `--snapshot` self-check (pause, mute, restart, physical key down/up, then a
window capture) passes locally. A local run also showed Pong and Tetris rendering,
held-key keycaps, pause state, key delivery through a focused sidebar button, and
the error dialog for an empty ROM. Not yet checked by a person on real hardware:
audible sound, the Open dialog, drag-and-drop from Explorer, the speed/palette
menus, fullscreen, high-DPI and multi-monitor moves, recent files across
restarts, and Narrator.

## Game Boy

Order of work:

1. CPU.
2. Timers, interrupts and PPU.
3. Cartridges and saves.
4. APU.
5. The app frontends.
6. Accuracy follow-up.

Steps 1–4 and 6 are done at the core level. All 116 ROM checks pass:

- Blargg: `cpu_instrs`, `instr_timing`, `mem_timing` (both), `halt_bug`, `dmg_sound` 01–08, and all eight `oam_bug` tests.
- `dmg-acid2`, pixel-exact.
- Mooneye: the boot state (`boot_regs`, `boot_div`, `boot_hwio`, `boot_sclk_align`), the timer, instruction and interrupt timing, HALT/EI/DI, OAM DMA (including restart and sources), the PPU timing tests (`intr_*`, `lcdon_*`, `stat_*`, `hblank_ly_scx`, `vblank_stat_intr`), the bit and DAA tests, and the MBC1 (including the 8 Mb multicart), MBC2 and MBC5 tests.

A built-in check covers the MBC3 save and RTC round trip. Donkey Kong (1994) also runs headlessly through the title, file select, intro and stage 1.

No Game Boy audio has been heard yet. The APU passes the `dmg_sound` register and timing tests, but that is not a listening check.

Step 5 is done on Windows. The app picks the core by cartridge header. It runs
wall-clock-paced emulation at 0.5×, 1× or 2×, plays stereo audio, and shows the
160×144 screen in four shades of each palette, with D-pad/A/B/Select/Start
keycaps. It keeps `.sav` files beside the ROM and accepts `.gb`/`.gbc` in Open.
The snapshot self-check covers the Game Boy key and frame path on a stand-in
cartridge. Local `--seconds 10 --snapshot` runs show the title or intro of
Donkey Kong, DuckTales, Link's Awakening, Super Mario Land 2 and Wario Land
rendering correctly.

Not yet checked by a person: hearing the audio, playing with real key input,
and a save surviving an exit and relaunch.

The Linux SDL frontend (`src/desktop/main.c`) has the same Game Boy support, with
`F3`/`F4` for speed and palette. CI compiles it with GCC, but nobody has run it
yet, since CI does not launch it.

The Mac app (`src/macos/main.m`) has the same Game Boy support: arrows or WASD, X/K and Z/J,
Return, Delete or right Shift, a CPU-rate speed choice for CHIP-8 and a real-time
multiple for everything else, stereo audio and `.sav` files, with `.gb`/`.gbc` registered as document types. It was
built and checked only through CI. The snapshot drives the Game Boy key and frame
path on a stand-in cartridge that draws a corner block, and the captured
`Nacre-preview-gb.png` shows the screen the right way up with its keycaps. Nobody
has run it on a real Mac, played a game on it, or heard its audio.

Step 5 is therefore complete in code on all three frontends. Hands-on checks
remain: audio by ear, real key play, and a save surviving a relaunch, on each OS.

Step 6 covered the boot state, the serial clock, PPU access windows and STAT
timing, mid-line register writes, the STAT-write and LY=153 quirks, OAM DMA
details, the OAM corruption bug and MBC1 multicarts. Five commercial games still
render through their title screens headlessly after these changes.

`oam_bug/7-timing_effect` is checked through the combined `oam_bug.gb`, not its
single. The single sweeps `inc de` across one scanline a cycle at a time and
prints a 525-byte OAM dump for each of the ~19 corrupting steps. That is about
10 KiB, and the shell's unbounded text log runs from $A004 past $BFFF into the
test's own code at $C000, so it restarts forever. The combined ROM checks the
same CRC without that log and passes, so our timing and patterns match the
recorded hardware result.

Step 7 added input and state on the Game Boy side of all three frontends: SDL3 gamepad
support, four save-state slots, hold-to-fast-forward at 4× and rebindable keys.
The core (`src/gb/gb_state.c`) serialises the machine; the platform-neutral
`src/gb_input.c` owns bindings and their text form; `src/desktop/pad.c` is the shared
controller layer. `tests/test_gb_input.c` and the core state tests run in `make test`.
The Windows, Linux and Mac stages each built in CI, and the Mac `--snapshot` check now
drives fast-forward and a save-state round trip. Not checked by a person: a real
controller, the rebinding windows by hand, how fast-forward and state loads feel and
sound, and the Linux and Mac frontends on real machines.

What remains:

- A real pixel FIFO. Spans put mid-line writes at about the right pixel, but
  effects that depend on exact fetcher timing are approximate.
- Externally clocked serial and the link cable: there is no second machine.

Game Boy Color support is a separate scope decision.

## Next acceptance pass

For Windows: run the CI artifact on a clean Windows 10 and 11 machine (SmartScreen
will warn about the unsigned exe), then walk the same checklist below using the
Open button, File menu, recent files, and dragging from Explorer. Also check
200% display scaling, moving between monitors of different DPI, F11/Esc, a ROM
path with spaces or non-ASCII characters, and Narrator on the sidebar buttons.

Have the friend launch the downloaded, quarantined app from Finder, open ROMs
using the button, menu, recent files, and drag-and-drop, then check keyboard input,
focus loss, pause/restart, mute, palettes, resizing/fullscreen, and audible sound.
Try cancelling a sheet, a moved recent file, an oversized ROM, and a ROM that
fails at execution. Current-session preservation and recovery need human review.
Record macOS version and Intel/Apple Silicon architecture with results.

Review Timendus opcode, flags, keypad, and quirks test displays. Earlier automated
runs executed seven classic ROMs for 20,000 cycles without errors; that does not
establish correct displayed results or compatibility. The current profile uses
Vx shifts, unchanged I on register transfer, V0 jump offset, and wrapping sprites.
SUPER-CHIP and XO-CHIP are not implemented. Legacy 0nnn instructions are ignored;
known unsupported extension encodings should eventually produce clear errors.

## Design follow-through

Refine the Mac interface using actual play feedback. The original shell artwork is
drawn in code and used for the running app's Dock icon and generated Finder icon.
Both use the same source in `src/macos/branding.h`. Native buttons have accessibility labels;
full VoiceOver coverage of the custom display/key guide remains open.
The Windows shell uses native BUTTON controls (owner-drawn) so focus and names stay
accessible; the custom display and keycap guide have no accessible description yet.
Windows still needs code signing before a normal download and an installer or
portable-zip decision. Linux packaging follows when there is a tester and target
distribution.

## Before a normal release

The project is GPL-3.0-or-later (MIT before 2026-11-17); still verify legacy ROM/asset provenance. The Mac welcome
ROM is original and generated by `tools/build-demo.py`; it replaces the legacy
startup demo in the Mac bundle. Include third-party
notices. Confirm deployment-target support on real hardware, inspect dependencies,
and test offline launch from paths containing spaces. CI artifacts are unsigned
previews. Developer ID signing, hardened runtime, notarization, and stapling are
still needed for a normal Mac download. No purchase or signing-account enrollment
is authorized by this roadmap.

## Later features

Prioritize compatibility profiles and useful diagnostics, then recent-session
recovery, screenshots, and a small ROM library. Game Boy save states exist but are
build-specific; a portable, versioned format (and CHIP-8 states) with ROM identity,
profile, RNG, and machine state is still open. SUPER-CHIP,
XO-CHIP, rewind, and web builds each deserve a separate scope decision.

## PS2

A PS2 core derived from PCSX2 is built in on Windows. docs/PS2.md has the
interface mapping, the layout and build, how upstream fixes are ported, and the
current status.

## Nintendo DS

A DS core of our own, from GBATEK, is under way. docs/NDS.md has the plan,
decisions and milestones. M0 (plumbing) is done: pointer input in the core API,
DS buttons in the key bindings and gamepad map, late-sized saves, detection, and
black frames. M1a is done too: both ARM CPUs, memory maps, our own BIOS and
direct boot. M1b is done: armwrestler (9 pages) and rockwrestler (23 tests)
pass by frame hash in `make test`, and `test_nds --run` scripts input and
benchmarks. The core runs at 74-80% of real time on the i5-8400, so the
interpreter speed pass may move ahead of M7. M2 is done: timers, DMA, SPI
(firmware, touch, power), the RTC and cart reads, checked at register level.
Two of three commercial games tried now get further and wait on backup memory, so the
save chips may move ahead of M3 (2D), which is next on the plan.

## Suggested implementation settings

| Work | Codex | Claude | Reason |
| --- | --- | --- | --- |
| Mac acceptance fixes / UI adaptation | Sol, high | Sonnet, high | Native event and packaging behavior needs careful checking |
| Small specified controls or layout changes | Luna, max | Haiku, high | Bounded implementation with visual acceptance |
| Compatibility and timing design | Sol, xhigh | Opus, high | Interacting machine profiles and state semantics |
| Game Boy frontend integration | Sol, high | Opus, high | Three shells, audio streaming and save I/O must agree |
| DS CPUs, 2D and sound (M1-M5) | Sol, xhigh | Opus, high | Large spec-driven core work checked by test ROMs |
| DS 3D (M6) | Sol, max | Opus, xhigh | Rasterizer quirks with no test suite, checked by oracle diffs |

Keep this document current rather than appending session history.
