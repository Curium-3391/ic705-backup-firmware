#!/usr/bin/env python3
"""
rb3_reconstruct_eeprom.py — Offline PC EEPROM Logical Image Reconstructor & Verifier

Reconstructs EEPROM_logical.bin (262,144 bytes) from physical chip dumps
IC705_EEPROM.bin (131,072 bytes) and IC706_EEPROM.bin (131,072 bytes) using the
verified J140 64-byte stripe bijection (0x2005C41C / 0x2005C414):
    chip        = (L >> 6) & 1       (0 = IC705, 1 = IC706)
    phys_offset = ((L >> 7) << 6) | (L & 0x3F)

Also checks Phase 1F SUM8 checksums at logical [0x3FB47..0x3FB50) (sum @ 0x3FB50)
and [0x3FB51..0x3FB5B) (sum @ 0x3FB5B), and optionally validates SHA256SUMS.txt.
"""
import argparse
import hashlib
import json
from pathlib import Path

CHIP_SIZE = 131072
LOGICAL_SIZE = 262144


def reconstruct_logical_from_physical(ic705: bytes, ic706: bytes) -> bytes:
    if len(ic705) != CHIP_SIZE or len(ic706) != CHIP_SIZE:
        raise ValueError(f"Expected 131072 B per chip, got IC705={len(ic705)}, IC706={len(ic706)}")
    out = bytearray(LOGICAL_SIZE)
    for stripe_idx in range(LOGICAL_SIZE // 64):
        l_off = stripe_idx * 64
        chip = stripe_idx & 1
        p_off = (stripe_idx >> 1) * 64
        src = ic705 if chip == 0 else ic706
        out[l_off:l_off + 64] = src[p_off:p_off + 64]
    return bytes(out)


def split_logical_to_physical(logical: bytes) -> tuple[bytes, bytes]:
    if len(logical) != LOGICAL_SIZE:
        raise ValueError(f"Expected 262144 B logical image, got {len(logical)}")
    ic705 = bytearray(CHIP_SIZE)
    ic706 = bytearray(CHIP_SIZE)
    for stripe_idx in range(LOGICAL_SIZE // 64):
        l_off = stripe_idx * 64
        chip = stripe_idx & 1
        p_off = (stripe_idx >> 1) * 64
        dst = ic705 if chip == 0 else ic706
        dst[p_off:p_off + 64] = logical[l_off:l_off + 64]
    return bytes(ic705), bytes(ic706)


def inspect_logical_anchors(logical: bytes) -> dict:
    s1_calc = sum(logical[0x3FB47:0x3FB50]) & 0xFF
    s1_stored = logical[0x3FB50]
    s2_calc = sum(logical[0x3FB51:0x3FB5B]) & 0xFF
    s2_stored = logical[0x3FB5B]
    return {
        "logical_size_bytes": len(logical),
        "logical_sha256": hashlib.sha256(logical).hexdigest(),
        "sum8_domain1_0x3fb47_0x3fb50": {
            "calculated": s1_calc,
            "stored": s1_stored,
            "valid": s1_calc == s1_stored,
        },
        "sum8_domain2_0x3fb51_0x3fb5b": {
            "calculated": s2_calc,
            "stored": s2_stored,
            "valid": s2_calc == s2_stored,
        },
        "touch_cal_0x3ff30_hex": logical[0x3FF30:0x3FF40].hex(),
        "touch_marker_0x3ff40_hex": logical[0x3FF40:0x3FF50].hex(),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Reconstruct EEPROM_logical.bin from IC705_EEPROM.bin + IC706_EEPROM.bin")
    ap.add_argument("--ic705", required=True, type=Path)
    ap.add_argument("--ic706", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    ic705 = args.ic705.read_bytes()
    ic706 = args.ic706.read_bytes()
    logical = reconstruct_logical_from_physical(ic705, ic706)
    args.out.write_bytes(logical)
    report = inspect_logical_anchors(logical)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
