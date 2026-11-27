#!/usr/bin/env python3
"""Fetch and build the public DS test ROMs used by tests/test_nds.c into .roms-cache/nds.

Nothing here is committed or redistributed by Nacre; the files stay in the
git-ignored cache. Every download is pinned to a commit and a SHA-256.

  rockwrestler     https://github.com/RockPolish/rockwrestler  (prebuilt .nds)
  armwrestler      https://github.com/mic-/armwrestler          (assembly source)
  font8x8.pat      https://github.com/arisotura/arm7wrestler    (armwrestler's font,
                                                                which its repo lacks)

armwrestler ships as source only. It is assembled with the GNU Arm toolchain
(arm-none-eabi binutils: on PATH, in $NACRE_ARM_TOOLCHAIN, or --toolchain DIR)
and packed into a .nds by pack_nds() below, which writes a header of our own:
no Nintendo logo, just a logo field whose checksum matches. Without the
toolchain, armwrestler is skipped and rockwrestler is still fetched.
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".roms-cache", "nds")

RAW = "https://raw.githubusercontent.com"
ROCKWRESTLER = (RAW + "/RockPolish/rockwrestler/b0067a06cddb9bbc9c46ecf0c60a2d572594e581/rockwrestler.nds",
                "f905e16510b00500d432b62dbfafc33e9a7179187475981fe74d9e691a96069a")
ARMWRESTLER = RAW + "/mic-/armwrestler/9702182be3d3452a1b07e3ae960070837c593ebf/"
ARMWRESTLER_FILES = {
    "armwrestler-ds.asm": "02b8b4b2bfa332165dfd3950a5af092aa63a1edbc881a053a0d1aa759b3b8db9",
    "thumbwrestler-ds.asm": "b7998a6897dd6c4fab5c3a1a5c040054549f71143cb2db9e306c01246c05a340",
    "armwrestler-arm7.asm": "8aa39ca421b1529c4629f659f060bafe3ce7a4a442c70cf79188a861bada1bbf",
    "ds_arm9_crt0.S": "ee70933a22f370ea6d8a4180b78fa084751b06c51e886d83027cba05082da503",
    "ds_arm9.ld": "5807bb18cfa4fa8e5ce20e4912306d20e975bca0a9dfea95284e7a0783e814bc",
}
FONT = (RAW + "/arisotura/arm7wrestler/05137604887ee1d184ec5c26899782151dca270f/arm7/data/font8x8.pat",
        "2606b327467986bcce76fc59b39d7942cb7189c08c476285a93c2f79f5112c90")


def fetch(url, sha256):
    request = urllib.request.Request(url, headers={"User-Agent": "nacre-test-fetch"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != sha256:
        raise RuntimeError("checksum mismatch for " + url)
    return data


def crc16(data, crc=0xFFFF):
    """The DS BIOS CRC-16 (reflected polynomial 0xA001)."""
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def pack_nds(title, code, arm9, arm9_address, arm7, arm7_address):
    """A minimal direct-boot cartridge: header, ARM9 binary at 0x4000, ARM7 after."""
    def align(n):
        return (n + 0x1FF) & ~0x1FF

    arm9_offset = 0x4000
    arm7_offset = align(arm9_offset + len(arm9))
    total = align(arm7_offset + len(arm7))
    header = bytearray(0x200)
    header[0x00:0x0C] = title.encode("ascii")[:12].ljust(12, b"\0")
    header[0x0C:0x10] = code.encode("ascii")
    header[0x10:0x12] = b"00"
    capacity = 0
    while (128 * 1024) << capacity < total:
        capacity += 1
    header[0x14] = capacity
    struct.pack_into("<IIII", header, 0x20, arm9_offset, arm9_address, arm9_address, len(arm9))
    struct.pack_into("<IIII", header, 0x30, arm7_offset, arm7_address, arm7_address, len(arm7))
    struct.pack_into("<II", header, 0x80, total, 0x4000)
    # The logo field stays blank; its checksum still has to match.
    struct.pack_into("<H", header, 0x15C, crc16(header[0xC0:0x15C]))
    struct.pack_into("<H", header, 0x15E, crc16(header[0x00:0x15E]))
    rom = bytearray(total)
    rom[0:0x200] = header
    rom[arm9_offset:arm9_offset + len(arm9)] = arm9
    rom[arm7_offset:arm7_offset + len(arm7)] = arm7
    return bytes(rom)


def find_toolchain(given):
    for directory in (given, os.environ.get("NACRE_ARM_TOOLCHAIN")):
        if directory:
            for candidate in (os.path.join(directory, "bin"), directory):
                if shutil.which("arm-none-eabi-as", path=candidate):
                    return candidate
    found = shutil.which("arm-none-eabi-as")
    return os.path.dirname(found) if found else None


def build_armwrestler(toolchain):
    """The steps of armwrestler's makeawds.bat, with arm-none-eabi tools."""
    def tool(name):
        return shutil.which("arm-none-eabi-" + name, path=toolchain)

    with tempfile.TemporaryDirectory() as work:
        for name, sha256 in ARMWRESTLER_FILES.items():
            with open(os.path.join(work, name), "wb") as f:
                f.write(fetch(ARMWRESTLER + name, sha256))
        with open(os.path.join(work, "font8x8.pat"), "wb") as f:
            f.write(fetch(*FONT))

        def run(*args):
            subprocess.run([tool(args[0])] + list(args[1:]), cwd=work, check=True)

        run("as", "-W", "-march=armv5te", "-EL", "-o", "awr9.o", "armwrestler-ds.asm")
        run("as", "-mcpu=arm9tdmi", "-mthumb", "-EL", "-o", "twr9.o", "thumbwrestler-ds.asm")
        run("as", "-mcpu=arm9tdmi", "-EL", "-o", "crt0.o", "ds_arm9_crt0.S")
        run("ld", "-T", "ds_arm9.ld", "-EL", "-e", "_start", "-o", "a9.elf", "crt0.o", "awr9.o", "twr9.o")
        run("objcopy", "-O", "binary", "a9.elf", "arm9.bin")
        run("as", "-mcpu=arm7tdmi", "-EL", "-o", "awr7.o", "armwrestler-arm7.asm")
        run("ld", "-Ttext", "0x3800000", "-EL", "-e", "arm7_main", "-o", "a7.elf", "awr7.o")
        run("objcopy", "-O", "binary", "a7.elf", "arm7.bin")
        with open(os.path.join(work, "arm9.bin"), "rb") as f:
            arm9 = f.read()
        with open(os.path.join(work, "arm7.bin"), "rb") as f:
            arm7 = f.read()
    return pack_nds("ARMWRESTLER", "AWRT", arm9, 0x02004000, arm7, 0x03800000)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--toolchain", help="directory holding arm-none-eabi-as (or its bin/)")
    args = parser.parse_args()
    os.makedirs(ROOT, exist_ok=True)

    path = os.path.join(ROOT, "rockwrestler.nds")
    if not os.path.exists(path):
        with open(path, "wb") as f:
            f.write(fetch(*ROCKWRESTLER))
    print("rockwrestler.nds ok")

    path = os.path.join(ROOT, "armwrestler.nds")
    if not os.path.exists(path):
        toolchain = find_toolchain(args.toolchain)
        if not toolchain:
            print("armwrestler.nds skipped: no arm-none-eabi-as (install GNU Arm binutils, "
                  "or pass --toolchain)")
            return 0
        rom = build_armwrestler(toolchain)
        with open(path, "wb") as f:
            f.write(rom)
    print("armwrestler.nds ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
