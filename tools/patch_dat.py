#!/usr/bin/env python3
"""
patch_dat.py — Offline CLI patcher for IC-705 Full-Storage Read-Only Backup Firmware.

Applies a clean-room patch JSON (e.g. docs/patches/705J140_rb3_b5.json) to a
user-supplied unmodified official Icom .dat file and refuses to write the output
unless BOTH MD5 and SHA-256 match the hardware-verified build byte-for-byte.

Usage:
  python3 tools/patch_dat.py \
    --input /path/to/705J140.dat \
    --patch docs/patches/705J140_rb3_b5.json \
    --output 705RB3B5.dat
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ic_rza1_codec as codec  # noqa: E402


def md5_hex(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def apply_ops(stock: bytes, patch: dict) -> bytes:
    base = int(str(patch.get("load_address", 0)), 0)
    size = int(patch.get("image_size", len(stock)))
    img = bytearray(size)
    n_copy = min(size, len(stock))
    img[:n_copy] = stock[:n_copy]

    for op in patch["ops"]:
        at = int(str(op["at"]), 0) - base
        data = bytes.fromhex(op["hex"])
        if at < 0 or at + len(data) > size:
            raise ValueError(f"Patch op at {op['at']} falls outside image bounds")
        if "expected_orig_hex" in op:
            expected = bytes.fromhex(op["expected_orig_hex"])
            actual = stock[at : at + len(expected)]
            if actual != expected:
                raise ValueError(
                    f"Original bytes at {op['at']} mismatch: got {actual.hex()}, expected {expected.hex()}"
                )
        img[at : at + len(data)] = data
    return bytes(img)


def build_patched(raw: bytes, patch: dict) -> tuple[bytes, str, str]:
    in_md5 = md5_hex(raw)
    in_sha256 = sha256_hex(raw)
    if in_md5 != patch["input"]["md5"] or (patch["input"].get("sha256") and in_sha256 != patch["input"]["sha256"]):
        raise ValueError(
            f"Input file mismatch: got MD5={in_md5} SHA256={in_sha256}, "
            f"expected MD5={patch['input']['md5']} SHA256={patch['input'].get('sha256')}"
        )

    stock, _, _ = codec.extract(raw)
    if md5_hex(stock) != patch["input"]["main_md5"]:
        raise ValueError("Decoded stock ARM image MD5 mismatch")
    if patch["input"].get("main_sha256") and sha256_hex(stock) != patch["input"]["main_sha256"]:
        raise ValueError("Decoded stock ARM image SHA-256 mismatch")

    patched_img = apply_ops(stock, patch)
    if md5_hex(patched_img) != patch["output"]["main_md5"]:
        raise ValueError("Patched ARM image MD5 mismatch")
    if patch["output"].get("main_sha256") and sha256_hex(patched_img) != patch["output"]["main_sha256"]:
        raise ValueError("Patched ARM image SHA-256 mismatch")

    out = codec.repack(raw, patched_img, new_bootstrap=None)
    out_md5 = md5_hex(out)
    out_sha256 = sha256_hex(out)
    if out_md5 != patch["output"]["md5"] or (patch["output"].get("sha256") and out_sha256 != patch["output"]["sha256"]):
        raise ValueError(
            f"Output .dat mismatch: got MD5={out_md5} SHA256={out_sha256}, "
            f"expected MD5={patch['output']['md5']} SHA256={patch['output'].get('sha256')}"
        )
    return out, out_md5, out_sha256


def main() -> None:
    ap = argparse.ArgumentParser(description="Build IC-705 read-only backup firmware from official .dat")
    ap.add_argument("--input", "-i", type=Path, required=True, help="Unmodified official Icom .dat file")
    ap.add_argument("--patch", "-p", type=Path, required=True, help="Clean-room patch JSON (e.g. docs/patches/705J140_rb3_b5.json)")
    ap.add_argument("--output", "-o", type=Path, required=True, help="Output .dat path")
    args = ap.parse_args()

    patch = json.loads(args.patch.read_text(encoding="utf-8"))
    raw = args.input.read_bytes()
    out, out_md5, out_sha256 = build_patched(raw, patch)
    args.output.write_bytes(out)
    print(f"OK: wrote {args.output} ({len(out):,} bytes)")
    print(f"    MD5:    {out_md5}")
    print(f"    SHA256: {out_sha256}")


if __name__ == "__main__":
    main()
