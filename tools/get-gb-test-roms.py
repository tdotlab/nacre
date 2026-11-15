#!/usr/bin/env python3
"""Fetch the public Game Boy test ROMs used by tests/test_gb.c into .roms-cache/gb.

Nothing here is committed or redistributed by Nacre; the files stay in the
git-ignored cache. Sources:
  Blargg's test ROMs     https://github.com/retrio/gb-test-roms
  dmg-acid2 (MIT)        https://github.com/mattcurrie/dmg-acid2
  Mooneye Test Suite     https://github.com/Gekkio/mooneye-test-suite (MIT)

The dmg-acid2 reference PNG is converted to raw 160x144 shade indices
(dmg-acid2.shades) so the C test needs no PNG decoder.
"""

import io
import os
import struct
import subprocess
import sys
import tarfile
import urllib.request
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".roms-cache", "gb")
BLARGG = "https://github.com/retrio/gb-test-roms.git"
ACID2_ROM = "https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb"
ACID2_REF = "https://raw.githubusercontent.com/mattcurrie/dmg-acid2/master/img/reference-dmg.png"
MOONEYE = ("https://gekkio.fi/files/mooneye-test-suite/mts-20260714-0944-31510e1/"
           "mts-20260714-0944-31510e1.tar.gz")


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "nacre-test-fetch"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def decode_png(data):
    """Minimal decoder for non-interlaced PNGs: 8-bit gray/RGB/RGBA, or palette and
    gray at 1, 2, 4 or 8 bits per pixel."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    chunks = {}
    idat = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IDAT":
            idat += body
        else:
            chunks[kind] = body
    width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    if interlace or (depth != 8 and not (channels == 1 and depth in (1, 2, 4))):
        raise ValueError("unsupported PNG layout")
    raw = zlib.decompress(idat)
    stride = (width * channels * depth + 7) // 8
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(height):
        kind = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 255
        rows.append(line)
        prev = line
    palette = chunks.get(b"PLTE")
    pixels = []
    for line in rows:
        if depth < 8:
            per = 8 // depth
            line = bytearray((line[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1)
                             for x in range(width))
            if color == 0:
                line = bytearray(v * 255 // ((1 << depth) - 1) for v in line)
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if color == 3:
                r, g, b = palette[px[0] * 3:px[0] * 3 + 3]
            elif channels >= 3:
                r, g, b = px[0], px[1], px[2]
            else:
                r = g = b = px[0]
            pixels.append((r, g, b))
    return width, height, pixels


def shades_from_png(data):
    width, height, pixels = decode_png(data)
    if (width, height) != (160, 144):
        raise ValueError("reference image is %dx%d" % (width, height))
    out = bytearray()
    for r, g, b in pixels:
        luma = (r * 299 + g * 587 + b * 114) // 1000
        out.append(0 if luma >= 213 else 1 if luma >= 128 else 2 if luma >= 43 else 3)
    return bytes(out)


def main():
    os.makedirs(ROOT, exist_ok=True)
    blargg = os.path.join(ROOT, "gb-test-roms")
    if not os.path.isdir(blargg):
        print("Cloning Blargg's test ROMs")
        subprocess.check_call(["git", "clone", "-q", "--depth", "1", BLARGG, blargg])

    acid = os.path.join(ROOT, "dmg-acid2.gb")
    if not os.path.isfile(acid):
        print("Fetching dmg-acid2")
        with open(acid, "wb") as f:
            f.write(fetch(ACID2_ROM))
    ref = os.path.join(ROOT, "dmg-acid2.shades")
    if not os.path.isfile(ref):
        shades = shades_from_png(fetch(ACID2_REF))
        with open(ref, "wb") as f:
            f.write(shades)

    mooneye = os.path.join(ROOT, "mooneye")
    if not os.path.isdir(mooneye):
        print("Fetching the Mooneye Test Suite")
        with tarfile.open(fileobj=io.BytesIO(fetch(MOONEYE)), mode="r:gz") as archive:
            for member in archive.getmembers():
                parts = member.name.split("/", 1)
                if len(parts) < 2 or not member.isfile() or not member.name.endswith(".gb"):
                    continue
                target = os.path.normpath(os.path.join(mooneye, parts[1]))
                if not target.startswith(os.path.normpath(mooneye) + os.sep):
                    continue  # refuse paths that escape the cache
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with archive.extractfile(member) as src, open(target, "wb") as dst:
                    dst.write(src.read())
    print("Game Boy test ROMs are in", os.path.normpath(ROOT))


if __name__ == "__main__":
    sys.exit(main())
