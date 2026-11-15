#!/bin/sh
# Drives the SDL3 desktop frontend headlessly (Xvfb + SDL dummy audio) using its
# deterministic test mode, and checks real input dispatch, rendering, and the
# audio callback against the bundled welcome ROM.
#
# Usage: tests/linux-desktop-smoke.sh path/to/Nacre [path/to/nacre.ch8]
#
# This verifies logic and rendering. It cannot show that anything is audible.
set -eu

BIN=${1:?usage: $0 path/to/Nacre [rom.ch8]}
ROM=${2:-"$(dirname "$0")/../assets/roms/nacre.ch8"}

if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "SKIP: xvfb-run not found" >&2
    exit 77
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export SDL_AUDIODRIVER=dummy
export XDG_RUNTIME_DIR="$WORK"

fail() { echo "FAIL: $*" >&2; exit 1; }

# xvfb-run merges the app's stderr into stdout, so callers capture both with > file 2>&1.
nacre() {
    xvfb-run -a -s "-screen 0 1280x720x24" "$BIN" "$ROM" "$@"
}

field() { # field NAME < status-line
    sed -n "s/.*$1=\([0-9]*\).*/\1/p"
}

# 1. Idle: the wordmark is drawn, and no sound is playing.
nacre --cycles 600 --dump "$WORK/idle.txt" --snapshot "$WORK/idle.png" \
    > "$WORK/idle.log" 2>&1 || fail "idle run exited non-zero"
grep -q '#' "$WORK/idle.txt" || fail "idle framebuffer is empty"
[ "$(field audio_active < "$WORK/idle.log")" = 0 ] || fail "audio active while idle"
[ "$(head -c 4 "$WORK/idle.png" | tail -c 3)" = PNG ] || fail "snapshot is not a PNG"

# 2. A key that is only held changes nothing: the welcome ROM reacts to release.
nacre --cycles 600 --key 5@100+5000 --dump "$WORK/held.txt" \
    > "$WORK/held.log" 2>&1 || fail "held run exited non-zero"
cmp -s "$WORK/idle.txt" "$WORK/held.txt" || fail "holding a key changed the display"

# 3. Press and release: the sparkle appears, the buzzer starts, and the audio
#    callback actually pulls samples.
nacre --cycles 140 --key 5@100+30 --dump "$WORK/released.txt" \
    > "$WORK/released.log" 2>&1 || fail "release run exited non-zero"
cmp -s "$WORK/idle.txt" "$WORK/released.txt" && fail "release did not draw the sparkle"
[ "$(field sound_timer < "$WORK/released.log")" -gt 0 ] || fail "sound timer not set"
[ "$(field audio_active < "$WORK/released.log")" = 1 ] || fail "audio not active"
[ "$(field audio_active_samples < "$WORK/released.log")" -gt 0 ] ||
    fail "audio callback produced no samples"

# 4. The buzzer is short: later in the run it has stopped.
nacre --cycles 600 --key 5@100+30 --dump "$WORK/later.txt" \
    > "$WORK/later.log" 2>&1 || fail "later run exited non-zero"
[ "$(field audio_active < "$WORK/later.log")" = 0 ] || fail "audio never stopped"

# 5. Bad arguments are rejected.
if nacre --key zz >/dev/null 2>&1; then fail "invalid --key was accepted"; fi

echo "linux desktop smoke checks passed"
