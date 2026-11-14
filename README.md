# Nacre

A compact CHIP-8 interpreter with a native Mac app, a portable SDL3 frontend, and a terminal interface. All three use the same C emulator core and sound oscillator.

![IBM Logo](screenshots/ibm-logo.png)
*IBM Logo — the classic first test.*

![Tetris](screenshots/tetris.png)
*Tetris — a piece falling into the well.*

![Breakout](screenshots/breakout.png)
*Breakout — bricks and paddle, waiting for serve.*

These images show the emulator's framebuffer as PNGs.

## Build and play

You need a C99 compiler, `make`, and SDL2 development files for live audio. `pkg-config` is used to find SDL2.

```sh
make
./play path/to/game.ch8
# Or run a specific ROM directly:
./nacre path/to/game.ch8 --play
```

The live sound is a smooth 440 Hz tone that follows the CHIP-8 sound timer. The `m` key mutes and unmutes it. Use `--sound off` for a silent play session. SDL2 is only needed for live audio; without it, the program still builds for headless use and WAV capture.

The terminal uses half-block characters by default to fit the display into 64×16 cells. Use `--render full` for double-width blocks. In play mode, use lowercase letters for the standard CHIP-8 key map; uppercase `R` resets the game:

```text
1 2 3 4        1 2 3 C
q w e r   →    4 5 6 D
a s d f        7 8 9 E
z x c v        A 0 B F
```

| Key | Action |
| --- | --- |
| `p` | Pause or resume |
| `R` | Reset the ROM |
| `+` / `-` | Increase or decrease speed |
| `m` | Mute or unmute sound |
| `Esc` | Quit |

## Desktop app

The Mac app pairs a warm pearl sidebar with a dark, pixel-perfect display. It has
native menus and file sheets, drag-and-drop ROM loading, recent files, a live
keyboard guide, pause/restart/sound buttons, and Fern, Amber, and Pearl palettes.
Speed, palette, and mute preferences are saved automatically. The emulator pauses
and releases held keys when the window loses focus.

Build with CMake 3.20+ and a C compiler (Xcode Command Line Tools on Mac):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The first configure downloads pinned SDL3 source. SDL3 is linked statically and
its license is bundled. Mac uses AppKit for the interface and SDL3 for audio;
Windows and Linux currently retain the basic SDL3 window. Their visual adaptation
is future work; the emulator core stays shared.

The Mac result is `build/Nacre.app`; Windows and Linux produce `Nacre` alongside
`assets/hello.ch8` and `THIRD_PARTY_NOTICES.txt`. Launch opens the bundled demo.
Use Open ROM, drag a file onto the Mac window, or select File → Open Recent.
Restart uses the loaded bytes, so it still works if the original file was moved.

| Action | Mac | SDL frontend |
| --- | --- | --- |
| CHIP-8 keypad | `1–4`, `QWER`, `ASDF`, `ZXCV` | Same |
| Pause / resume | `Space` or `⌘P` | `Space` |
| Restart ROM | `⌘R` or `F2` | `F2` |
| Sound | `M` | `M` |
| Open ROM | `⌘O` | `Ctrl+O` |
| Full screen | View menu / `⌃⌘F` | Window manager |
| Quit | `⌘Q` | `Ctrl+Q` / `Esc` |

The [Desktop builds workflow](https://github.com/tdotlab/nacre/actions/workflows/desktop.yml)
uploads unsigned artifacts. Mac CI targets macOS 11+ with both Intel and Apple
Silicon architectures. It also launches the AppKit app, exercises basic controls
and key dispatch, and captures the actual interface as `Nacre-preview.png`.
These automated checks do not establish audible output or real MacBook acceptance.
The preview is unsigned and unnotarized. A normal downloadable release still needs
signing and notarization.

Linux build dependencies are listed in `.github/workflows/desktop.yml`.
For a Mac UI capture from a built bundle:

```sh
build/Nacre.app/Contents/MacOS/Nacre --snapshot /absolute/path/preview.png
```

## Interpreter behavior

Nacre implements the classic CHIP-8 instruction set with these choices: `8xy6` and
`8xyE` shift `Vx`; `Fx55` and `Fx65` leave `I` unchanged; `Bnnn` adds `V0`; and
sprites wrap at screen edges. `Fx0A` waits for a new key press and its release.
SUPER-CHIP and XO-CHIP are not implemented. Legacy `0nnn` system calls are ignored.
Seven classic Timendus ROMs ran for 20,000 simulated cycles without an emulator
error. This was a crash smoke check; their displayed results have not been reviewed.

The default speed is 10 instructions per 60 Hz virtual frame (600 instructions per
second). `--speed N` changes CPU speed without changing timer or display rates.

## ROMs

The Mac app bundles an original welcome ROM, `assets/roms/nacre.ch8`: it draws Nacre and toggles a sparkle with a short beep when a keypad key is pressed and released. Regenerate it with `python3 tools/build-demo.py`; check reproducibility with `--check`.

`hello.ch8` draws a small legacy demo. `sound.ch8` sets the buzzer timer and loops, so it is a quick way to hear the sound output.

Pass any compatible `.ch8` file to `./play` or `./nacre`. To download the optional game collection for the terminal picker's local library, run:

```sh
./get-roms.sh
./play
```

The collection is cloned into `.roms-cache/`; `roms/` is a local symlink to its `games/` directory. Both paths are ignored by Git. The ROM collection comes from [kripod/chip8-roms](https://github.com/kripod/chip8-roms).

## Headless use

```sh
./nacre hello.ch8 --cycles 50
./nacre path/to/rom.ch8 --cycles 2000 --dump framebuffer.txt
./nacre sound.ch8 --cycles 300 --sound wav --sound-file sound.wav
./nacre --selftest
```

Headless mode runs 2,000 CPU cycles by default at 600 cycles per virtual second and
prints the 64×32 framebuffer as `#` and `.`. `--dump PATH` writes it to a file;
`--dump -` writes to standard output. `--sound wav` records a mono 48 kHz WAV,
including silence; CHIP-8 timers tick at 60 Hz, so 600 cycles produce one simulated
second. The default output path is `nacre.wav`.

`make clean` removes the compiled executable. The ROMs and generated WAV files are not tracked by Git.

## Development

Run `make test` for the SDL-free scheduler and key-wait checks. The desktop source,
cross-platform build, and remaining Mac acceptance work are tracked in
[the roadmap](docs/ROADMAP.md).
