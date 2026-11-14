#!/usr/bin/env python3
"""hud_strip_sites.py - list/verify the code sites that bake the 80 px HUD strip into the world view.

Static check only (reads Game.exe, writes nothing). See docs/HUD_STRIP.md.
  python tools/hud_strip_sites.py          - verify original bytes of every patch site
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rh import PE, DEFAULT_EXE  # noqa: E402

# fsub dword ptr [0x677598] (80.0f) -> fsub dword ptr [0x677488] (0.0f); 6 bytes, patch bytes 2..5
FSUB_SITES = [
    0x4C0414, 0x4C046F,            # 0x4C0340 full-map screenshot tiles
    0x4C7D2C,                      # CanZoomOut 0x4C7CF0
    0x4C808C, 0x4C80B5,            # CenterCameraOn 0x4C7F60 (bottom clamp)
    0x4CED05, 0x4CED62,            # ClampScrollDelta 0x4CEC60
    0x4CEEFB, 0x4CEF21, 0x4CEF3D, 0x4CEF61,  # Scroll 0x4CEDE0 (self-blit rects)
    0x4CF715, 0x4CF723,            # zoom-out animation 0x4CF610
    0x4D01CD, 0x4D01DD, 0x4D0229,  # zoom-in animation 0x4CFCE0
    0x575A11, 0x575A58,            # camera clamp 0x5758D0
]
FSUB_ORIG = bytes.fromhex("D82598756700")
FSUB_NEW = bytes.fromhex("D82588746700")

# push 0xC2A00000 (-80.0f, y of Vec2(x,-80) added to the viewport size) -> push 0; 5 bytes
PUSH_SITES = [
    0x4BEDFD, 0x4C16F2, 0x4C37C7, 0x4C748E, 0x4C87A1, 0x4C8994, 0x4C8DD9, 0x4C92E7,
    0x4C94E7, 0x4C9B67, 0x4C9C38, 0x4CEF8C, 0x4CF851, 0x4CF92C, 0x4CFA5B, 0x4CFD7E,
    0x4CFDED, 0x4CFFF4, 0x4D00CC, 0x4D0A8E, 0x4D910E, 0x4DA4AF, 0x4DA570, 0x4DA6A2,
]
PUSH_ORIG = bytes.fromhex("680000A0C2")
PUSH_NEW = bytes.fromhex("6800000000")

# References to 0x677598 that must NOT be changed (not the HUD strip)
KEEP_SITES = [0x4456FA, 0x44C1BF, 0x44C1D0, 0x44C1D6, 0x5B2FB9]


def main():
    pe = PE(DEFAULT_EXE)
    ok = True
    for va in FSUB_SITES:
        b = pe.read(va, 6)
        good = b == FSUB_ORIG
        ok &= good
        print(f"{va:08X} fsub  {b.hex(' ').upper():20s} {'OK' if good else 'MISMATCH'} -> {FSUB_NEW.hex(' ').upper()}")
    for va in PUSH_SITES:
        b = pe.read(va, 5)
        good = b == PUSH_ORIG
        ok &= good
        print(f"{va:08X} push  {b.hex(' ').upper():20s} {'OK' if good else 'MISMATCH'} -> {PUSH_NEW.hex(' ').upper()}")
    for va in KEEP_SITES:
        print(f"{va:08X} keep  {pe.read(va, 6).hex(' ').upper()}")
    print(f"{len(FSUB_SITES)} fsub + {len(PUSH_SITES)} push sites, all original: {ok}")
    zero = pe.read(0x677488, 4)
    print(f"[0x677488] = {zero.hex(' ').upper()} (must be 00 00 00 00)")
    return 0 if ok and zero == b"\0\0\0\0" else 1


if __name__ == "__main__":
    sys.exit(main())
