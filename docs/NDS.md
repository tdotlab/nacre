# Nintendo DS core

Status: M1 (CPUs, memory, BIOS, direct boot, test-ROM validation) done; M2
(timers, DMA, SPI, RTC, cart reads) next. Both ARM CPUs run a cartridge from
direct boot through our own BIOS, and display modes 0-3 show. armwrestler
passes all nine of its pages and rockwrestler all 23 of its tests, checked by
frame hash in `make test`. Audio is still silent. The core runs at 74-80% of
real time on the i5-8400 (see Risks). This file is the plan until the core exists, then it becomes the core's
reference, the way docs/PS2.md is for PS2.

## Shape

The DS core is Nacre's own code, written like the Game Boy core: plain C99, no
platform calls, an opaque `Nds` machine behind `include/nds.h`, and a thin
`src/core/nds_core.c` table (`nacre_nds_core`, id `nds`, name "Nintendo DS",
extension `nds`). It is a stepped core with framebuffer video, so every frontend
and CI build it on Windows, Mac and Linux. It is the first big core to do that;
PS2 builds on Windows only.

| File | Holds |
| --- | --- |
| `include/nds.h` | Public API, same pattern as `gb.h`: create, load, advance_ns, framebuffer, buttons, touch, audio ring, saves, states, title |
| `src/nds/nds.c`, `nds_internal.h` | Machine, both buses and memory maps, VRAM bank mapping, scheduler, IRQs, timers, DMA, IPC sync/FIFO, math unit, keypad |
| `src/nds/nds_arm.c` | One ARM/Thumb interpreter for both CPUs: ARMv5TE for the ARM946E-S, ARMv4T for the ARM7TDMI |
| `src/nds/nds_cp15.c` | ARM9 coprocessor: protection unit, ITCM/DTCM placement, cache control (caches are not modelled) |
| `src/nds/nds_bios.c` | Our own BIOS: the SWI calls games use, implemented from the docs, plus direct boot |
| `src/nds/nds_cart.c` | Header, ROM commands in data mode, backup chips (EEPROM, flash, FRAM) |
| `src/nds/nds_spi.c` | Firmware (generated user settings), touchscreen controller, power management |
| `src/nds/nds_rtc.c` | Real-time clock |
| `src/nds/nds_gpu2d.c` | Both 2D engines, master brightness, display capture |
| `src/nds/nds_gpu3d.c`, `nds_raster.c` | Geometry engine; software rasterizer |
| `src/nds/nds_spu.c` | 16 sound channels, capture, mix resampled to 48 kHz |
| `src/nds/nds_state.c` | Save states |
| `tests/test_nds.c`, `tools/get-nds-test-roms.py` | ROM-driven checks and the headless run/press/touch/dump/bench tool, like `test_gb` |

## Fixed decisions

**No Nintendo files.** The core boots games directly. It copies the ARM9 and
ARM7 binaries from the header into RAM, sets the post-boot state the firmware
leaves, and answers SWIs with its own BIOS code. Firmware user settings
(nickname, language, touchscreen calibration) are generated. Without the BIOS
there is no KEY1 cart encryption, because a direct boot reads the cart only in
data mode. Dumped BIOS and firmware support can come later as an option, kept in
`C:\Users\ringd\tools\nds\` and never in the repo. Game dumps live in
`C:\Users\ringd\tools\nds\games\`.

**Provenance.** The specification is GBATEK (Martin Korth's hardware reference)
plus public test ROMs and homebrew. We do not read other emulators' source
(melonDS, DeSmuME, NooDS). Running them as black-box oracles to compare frames
and behavior is fine, and is how 3D gets checked without real hardware. Any
outside source beyond GBATEK and test ROM READMEs is logged here, with what was
learned, in behavior terms. That keeps "our own code" true and checkable.

Sources used so far: GBATEK (CPU, CP15, memory maps, I/O, BIOS functions, boot
state). The rockwrestler ROM (prebuilt, run only) and armwrestler (assembled
from its public source with the GNU Arm toolchain, run only) are test inputs,
not references. Their source was read only to learn how to drive them (which
button opens which page) and what a failing test number checks.

**Video.** One XRGB8888 frame, 256×384: top screen over bottom, no gap, square
pixels, so the frontends integer-scale it as they do the Game Boy. DS colors are
RGB555 (2D) or RGB666 (3D), expanded to 8 bits. A `layout` option (side by side,
gap, swap) comes later.

**Timing.** ARM7 33.513982 MHz, ARM9 twice that. A frame is 263 lines of 355 dots
at 6 ARM7 cycles each, which comes to 59.8261 Hz. An event scheduler runs the
ARM9 for a slice, then lets the ARM7 catch up. Bus timing uses the documented
waitstates, with caches treated as always hitting. That is approximate, and most
games tolerate it. `nds_advance_ns` carries fractional cycles like `gb_advance_ns`.

**Audio.** The DS mixes at about 32,728.5 Hz. The core resamples to 48 kHz in
step with emulated time, the way the Game Boy core already produces 48 kHz. If
the core reported a rounded 32,768 Hz instead, the session's queue would drift
about 40 ms every half minute and drop audio.

**Out of scope:** DSi mode, Wi-Fi and local multiplayer (stubbed only so games
don't hang), the GBA slot, booting the firmware menu, a JIT, and upscaled or GPU
3D. The microphone is silence. A "blow" key can come later.

## Changes outside the core

These are small and come first (M0), so the core plugs into code that already
expects it.

**Touch in the core API.** Add `set_pointer(self, port, x, y, pressed)`, with
`x`/`y` in frame pixels and -1 when off the frame, plus a
`NACRE_CORE_HAS_POINTER` flag that `nacre_core_validate` checks. Bump
`NACRE_CORE_API_VERSION` to 2; no external plugins load anymore, so nothing
breaks. Each frontend maps the mouse through the same destination rectangle it
draws the frame into. The core treats y ≥ 192 as the touchscreen. The API is
generic, so a light gun or a PS1 mouse could use it later.

**Full pad bindings.** Keyboard bindings live in `gb_input.c`. X, Y, L and R
now sit after Start (`GBI_X`..`GBI_R`, 16 actions in all), with default keys C/I,
V/U, Q and E. The saved string is format 2; a format-1 string (12 actions) still
parses, and the new buttons keep their defaults unless the old string already
used those keys elsewhere. `gbi_labeled_buttons(core)` tells the gamepad code
which buttons the core names: when it names X, Y, L and R, the shoulders are L
and R, the right trigger fast-forwards and the left trigger is the state
modifier. Otherwise the Game Boy mapping is unchanged. Keycaps in the Windows and
Mac sidebars switch to a four-button layout for such a core.

**Saves sized after load.** The session reads `.sav` only when `save_size` is
nonzero at load time. A DS cart doesn't announce its backup chip. The core can
size it from an existing save file, or detect the type from the game's first
SPI commands, but only if the session hands over the file regardless of size.
A core sets `NACRE_CORE_SAVE_SIZED_LATER` (which requires `HAS_BATTERY`) to get
an existing save while its size is still 0; the flag keeps the Game Boy, whose
size is known, from getting a stray load for carts without a battery. Writes
already query the size at write time. Saves up to 1 MiB fit
`NACRE_MAX_SAVE_BYTES`; carts with large NAND storage are out of scope. The DS
table sets neither flag until M5.

**Per-system look.** Add `{"nds", false, "59.8 HZ FRAMES"}` to the Windows and
Mac look tables. Pad labels: A EAST, B SOUTH, X NORTH, Y WEST, L L1, R R1,
Select, Start.

**Probe.** A DS header is identified by a logo CRC at 0x15C that matches the logo
field (0xCF56 on retail carts; homebrew for flashcards may carry its own logo,
as rockwrestler does) and a valid header CRC16 at 0x15E, which gives 100. A Game Boy ROM can't match both, so probe
ties don't arise.

## Milestones

Each one ends green, tested, committed and pushed on its own. The test
harness grows with each milestone, as `test_gb` did.

1. **M0, plumbing.** Done.
   - The interface changes above, `include/nds.h` and a core table that loads a
     header and shows black frames.
   - `tests/test_nds.c`: header, CRC, timing and input checks against a
     synthetic ROM built in `tests/nds_test_rom.h` (its logo field is filler
     whose CRC is solved for, so no Nintendo data is in the repo), plus
     `--run/--seconds/--dump`. The Windows `--snapshot` self-test clicks the
     lower screen to check the mouse path.
   - Moved to M1, where they first have something to test: `--press/--touch`,
     the ROM fetch tool (`tools/get-nds-test-roms.py`), and the BlocksDS SDK in
     `C:\Users\ringd\tools\blocksds\` for our own small hardware tests.
2. **M1, CPUs and boot.** Split in two.
   - **M1a, done.** The ARM/Thumb interpreter (`nds_arm.c`, ARMv5TE and ARMv4T
     in one), CP15 and TCM (`nds_cp15.c`), both memory maps with WRAM and VRAM
     bank mapping (`nds_mem.c`), interrupts, IPCSYNC/FIFO, the math unit and
     keys (`nds_io.c`), our BIOS and direct boot (`nds_bios.c`), display modes
     0-3 with VRAM display from banks A-D (`nds_gpu2d.c`), and the line
     scheduler in `nds.c` (ARM9 then ARM7 in 64-cycle slices, HBlank, VBlank
     and VCount IRQs).
   - The BIOS is ours: exception vectors, an IRQ trampoline that calls the
     handler at DTCM+0x3FFC (ARM9) or 0x0380FFFC (ARM7), and every SWI run in C.
     That covers SoftReset, WaitByLoop, IntrWait and VBlankIntrWait, Halt,
     Sleep, Div, Sqrt, CpuSet, CpuFastSet, CRC16, BitUnPack, the LZ77, Huffman
     and RL decompressors (callback forms run guest code), and Diff8/16.
     SoundBias is a no-op and the sound tables (0x1A-0x1C) return 0 until M4.
   - Direct boot leaves the ARM9's stacks in shared WRAM it can't see, as
     hardware does (GBATEK marks it "(!)"); every game sets its own.
   - The built-in test cartridge (`tests/nds_test_rom.s`) exercises boot, a
     SWI, IPCSYNC, the display and VBlank IRQs through the BIOS trampoline.
   - **M1b, done.** `test_nds --run` takes `--press KEY@F[+L]`,
     `--touch X,Y@F[+L]`, `--dump [FILE][@F]` (prints a frame hash, optionally
     writes a PPM) and `--bench`. `tools/get-nds-test-roms.py` fetches
     rockwrestler and armwrestler's source, pinned by commit and SHA-256,
     assembles armwrestler with GNU Arm binutils and packs it with its own
     `.nds` packer (blank logo, matching checksum). `test_nds .roms-cache/nds`
     drives one machine through armwrestler's nine pages and rockwrestler's 23
     tests and compares each screen with a hash recorded after checking it by
     eye; every rockwrestler test ends on the same "OK" screen. CI installs
     `binutils-arm-none-eabi` and runs them on Linux.
   - The one bug they found: the divider skipped GBATEK's upper-half inversion
     on 32-bit DIV0 whenever the whole denominator was zero, because the DIV0
     flag was folded into the mode before the mode was tested. `math_checks`
     covers the overflow cases without a ROM.
   - BlocksDS stays optional.
   - Deferred to M2: timers, DMA, SPI (firmware settings, touch, power), RTC
     and cart data-mode reads.
3. **M2, the rest of the system.**
   - IRQs, timers, DMA, IPC, the math unit, keypad, SPI (firmware, touch,
     power), RTC, and cart data-mode reads.
   - Frontend touch input.
   - Done when the BlocksDS/libnds examples for console text, timers, touch,
     RTC and IPC behave, with frame hashes in the test suite.
4. **M3, 2D.**
   - Both engines: every BG mode including extended rotation and bitmaps,
     sprites, windows, blending, extended palettes, mosaic, master brightness
     and display capture.
   - Done when the 2D examples match the oracle, and a few 2D-heavy commercial
     games reach gameplay headlessly (Castlevania: Dawn of Sorrow and Advance
     Wars: Dual Strike are good first targets).
5. **M4, sound.**
   - PCM8/PCM16/IMA-ADPCM/PSG/noise channels, capture, and the 48 kHz
     resampled mix.
   - Done when the maxmod examples and game music play. A listening check is
     still a separate step.
6. **M5, saves.**
   - EEPROM, flash and FRAM, with type from file size or detection, a
     `save_type` override option, and the RTC from wall-clock time.
   - Done when a save round-trips headlessly and survives an app relaunch.
7. **M6, 3D.** This is the big one, in three parts:
   - Geometry: the command FIFO and GXFIFO DMA, matrix stacks, lighting,
     clipping, culling and the polygon and vertex limits.
   - The rasterizer: DS interpolation, Z and W buffers, every texture format.
   - Effects: alpha test and blend, toon and highlight, edge marking, fog,
     antialiasing, shadow polygons and the clear/rear plane.
   - Done when the 3D homebrew tests we write match the oracle frame for
     frame, and Super Mario 64 DS, Mario Kart DS and New Super Mario Bros.
     play.
8. **M7, states, speed and compatibility.**
   - Save states, profiling, and a C-interpreter speed pass (decode caching,
     fast memory paths).
   - Done when the core runs full speed at 1× and fast-forward is useful on
     the i5-8400.
   - A tested-games table here, with what works and what doesn't.

## Risks

**3D accuracy has no test suite.** The DS rasterizer has quirks (interpolation,
edge rules, polygon limits) that only show in games. Oracle frame diffs plus our
own BlocksDS tests are the plan. Real hardware would be better: a DS with a
flashcart running the same test ROMs gives ground truth.

**Speed.** An interpreted 67 MHz ARM9 plus a software rasterizer, in C, has to
hit 60 fps on an i5-8400. It needs decode caching and fast paths, not a naive
loop. Measured at the end of M1 (MSVC /O2, `test_nds --run ... --bench`, 10
emulated seconds): 74% of real time on rockwrestler's menu and 80% on
armwrestler, with no 2D layers, 3D or sound yet. The speed pass is planned for
M7; doing at least the interpreter part before M3 would keep testing games
practical.

**Timing-sensitive games.** Caches and exact bus timing are not modelled. A few
games will need fixes once they are found.

**Save detection.** A wrong chip type means a lost save. Detect from the file
when one exists, and fall back to an override option. Build our own small game
table from testing, never copied from another emulator.
