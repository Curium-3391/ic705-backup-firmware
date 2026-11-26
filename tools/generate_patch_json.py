#!/usr/bin/env python3
"""
generate_patch_json.py — Reproducibly generates a clean-room patch JSON
(docs/patches/705J140_rb3_b5.json) from:
  1) Compiled open-source rb3_backup_fw.bin (built from firmware/*.c & *.S)
  2) User's local unmodified official 705J140.dat (never committed to Git!)

The resulting JSON contains ONLY:
  - Input/output cryptographic digests (MD5 + SHA-256)
  - Single 4-byte ARM BL hook instruction at 0x20071F18 (+ 4-byte expected original opcode check)
  - Our open-source 13,660-byte Cortex-A9 read-only backup payload at 0x2036A000
ZERO proprietary Icom code is embedded in the patch JSON.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ic_rza1_codec as codec  # noqa: E402

J140_BASE = 0x20005000
HOOK_VA = 0x20071F18
PAYLOAD_VA = 0x2036A000
EXPECTED_ORIG_OPCODE = bytes.fromhex("dfe9ffeb")


def md5_hex(b: bytes) -> str:
    return hashlib.md5(b).hexdigest()


def sha256_hex(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def encode_arm_bl(src_va: int, dst_va: int) -> bytes:
    delta = dst_va - (src_va + 8)
    assert delta % 4 == 0
    return struct.pack("<I", 0xEB000000 | ((delta >> 2) & 0x00FFFFFF))


def main() -> None:
    ap = argparse.ArgumentParser(description="Generate clean-room RB3 B5 patch JSON")
    ap.add_argument("--official-dat", type=Path, required=True, help="Local unmodified official 705J140.dat")
    ap.add_argument("--payload-bin", type=Path, required=True, help="Compiled open-source rb3_backup_fw.bin")
    ap.add_argument("--out", type=Path, required=True, help="Output patch JSON path")
    args = ap.parse_args()

    raw = args.official_dat.read_bytes()
    payload = args.payload_bin.read_bytes()
    stock, _, _ = codec.extract(raw)

    hook_off = HOOK_VA - J140_BASE
    if stock[hook_off : hook_off + 4] != EXPECTED_ORIG_OPCODE:
        raise SystemExit(
            f"Unexpected original opcode at 0x{HOOK_VA:x}: {stock[hook_off:hook_off+4].hex()} != {EXPECTED_ORIG_OPCODE.hex()}"
        )

    bl_bytes = encode_arm_bl(HOOK_VA, PAYLOAD_VA)
    image = bytearray(stock)
    image[hook_off : hook_off + 4] = bl_bytes
    payload_off = PAYLOAD_VA - J140_BASE
    if payload_off < len(image):
        raise SystemExit("Payload offset overlaps stock image")
    image.extend(bytes(payload_off - len(image)))
    image.extend(payload)
    patched_image = bytes(image)

    patched_dat = codec.repack(raw, patched_image, new_bootstrap=None)

    patch_obj = {
        "schema_version": 1,
        "id": "rb3-b5-j140",
        "build_id": "RB3-DEDICATED-BACKUP-FINAL-J140-V5",
        "description": "IC-705 JP v1.40 read-only 5-chip full-storage backup firmware (RB3 B5). Clean-room patch: 4-byte ARM BL hook + open-source Cortex-A9 payload.",
        "hardware_verified": True,
        "verified_scope": "IC-705 JP (705J140.dat v1.40) ONLY",
        "bootloader_modified": False,
        "load_address": f"0x{J140_BASE:08x}",
        "image_size": len(patched_image),
        "input": {
            "radio": "IC-705",
            "region": "JP",
            "version": "1.40",
            "file": "705J140.dat",
            "size_bytes": len(raw),
            "md5": md5_hex(raw),
            "sha256": sha256_hex(raw),
            "main_size_bytes": len(stock),
            "main_md5": md5_hex(stock),
            "main_sha256": sha256_hex(stock),
        },
        "output": {
            "file": "705RB3B5.dat",
            "size_bytes": len(patched_dat),
            "md5": md5_hex(patched_dat),
            "sha256": sha256_hex(patched_dat),
            "main_size_bytes": len(patched_image),
            "main_md5": md5_hex(patched_image),
            "main_sha256": sha256_hex(patched_image),
            "payload_size_bytes": len(payload),
            "payload_sha256": sha256_hex(payload),
        },
        "ops": [
            {
                "comment": "Single ARM BL hook at stock Firmware Update menu entry (0x20071F18 -> 0x2036A000)",
                "at": f"0x{HOOK_VA:08x}",
                "expected_orig_hex": EXPECTED_ORIG_OPCODE.hex(),
                "hex": bl_bytes.hex(),
            },
            {
                "comment": "Open-source Cortex-A9 read-only 5-chip backup payload compiled from firmware/",
                "at": f"0x{PAYLOAD_VA:08x}",
                "hex": payload.hex(),
            },
        ],
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(patch_obj, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "out": str(args.out),
        "output_md5": patch_obj["output"]["md5"],
        "output_sha256": patch_obj["output"]["sha256"],
        "payload_bytes": len(payload),
    }, indent=2))


if __name__ == "__main__":
    main()
