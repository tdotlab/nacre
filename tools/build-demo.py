#!/usr/bin/env python3
"""Assemble Nacre's original welcome ROM. No external assets or assembler needed.

Run normally to regenerate assets/roms/nacre.ch8; use --check for a byte comparison.
The ROM draws NACRE and a shell, then toggles a sparkle and beeps on each key.
"""
from pathlib import Path
import sys

code = bytearray()
patches = []
labels = {}

def emit(op):
    code.extend(op.to_bytes(2, "big"))

def sprite(name, x, y, rows):
    emit(0x6000 | x)
    emit(0x6100 | y)
    patches.append((len(code), name))
    emit(0xA000)
    emit(0xD010 | len(rows))

sprites = {
    "shell": [0x1C, 0x62, 0x92, 0xAA, 0xAA, 0x92, 0x64, 0x18],
    "N": [0x88, 0xC8, 0xC8, 0xA8, 0x98, 0x98, 0x88],
    "A": [0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88],
    "C": [0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70],
    "R": [0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88],
    "E": [0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8],
    "spark": [0x20, 0x70, 0xF8, 0x70, 0x20],
}
emit(0x00E0)
sprite("shell", 7, 12, sprites["shell"])
for i, letter in enumerate("NACRE"):
    sprite(letter, 20 + i * 6, 12, sprites[letter])
wait = 0x200 + len(code)
emit(0xF20A)  # Wait for a fresh key press and release.
emit(0x6203)
emit(0xF218)  # Three 60 Hz ticks of sound.
sprite("spark", 54, 13, sprites["spark"])
emit(0x1000 | wait)
for name, rows in sprites.items():
    labels[name] = 0x200 + len(code)
    code.extend(rows)
for offset, name in patches:
    code[offset:offset + 2] = (0xA000 | labels[name]).to_bytes(2, "big")

path = Path(__file__).resolve().parents[1] / "assets/roms/nacre.ch8"
if "--check" in sys.argv:
    if path.read_bytes() != code:
        sys.exit("Welcome ROM differs; run tools/build-demo.py")
    print(f"Welcome ROM matches source ({len(code)} bytes)")
else:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(code)
    print(f"Wrote {path} ({len(code)} bytes)")
