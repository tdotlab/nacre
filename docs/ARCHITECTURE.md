# Nacre architecture

The CHIP-8 machine, scheduler, and oscillator are C99. Platform interfaces own
windows, input, ROM selection, preferences, and audio devices.

| Entry point | Responsibility |
| --- | --- |
| `src/chip8.c`, `include/chip8.h` | Machine state, instructions, ROM bytes, key edges |
| `src/scheduler.c` | Independent CPU / 60 Hz timer clocks, bounded catch-up, pause |
| `src/audio.c` | Shared tone generator, CLI WAV and optional SDL2 live sink |
| `src/macos/main.m` | ARC-managed AppKit app, native menus/panels, display, controls, SDL3 audio |
| `src/macos/Info.plist.in` | Mac identity, version, document associations, OS requirement |
| `src/desktop/main.c` | SDL3 window/input/file picker/audio for Windows and Linux |
| `src/main.c` | Terminal and headless CLI |
| `CMakeLists.txt` | Platform selection, static SDL3, bundle/resources |
| `.github/workflows/desktop.yml` | OS builds, universal Mac artifact, native UI smoke capture |

## Thread and state ownership

AppKit and the SDL event loop each run the machine on their main thread. The audio
callback reads an atomic active flag and owns its oscillator. It never reads the
machine directly. Mac schedules at 60 Hz using a common-mode NSTimer; elapsed time
comes from SDL's monotonic clock. The shared scheduler caps catch-up at 250 ms.
Focus loss, panels, user pause, and execution faults stop advancement and silence
playback. CPU speed changes leave the CHIP-8 timer frequency unchanged.

Mac loads a candidate machine before replacing the current session, retains ROM
bytes for restart, and uses NSUserDefaults for speed/palette/mute. Invalid files
leave the current machine intact. Native file menus and drag-and-drop converge on
the same loader. Preferences never go inside the app bundle.

## Presentation

Mac uses native AppKit controls and text drawing. Its custom view draws the shell
mark, sidebar, live keycaps, status, and display surround. The 64×32 framebuffer
uses centered integer scaling. Fern, Amber, and Pearl change pixel ink; the dark
stage and warm chrome stay stable. The minimum window keeps all controls visible.
The current design is intentionally light even under macOS dark appearance.

The Mac shell is Objective-C to call the existing C APIs directly and use Cocoa
without another binding layer. Windows/Linux retain their SDL shell; they can
adopt the visual language independently. No emulator code depends on AppKit.
