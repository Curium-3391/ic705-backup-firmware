#!/usr/bin/env python3
"""
check_clean_room.py — CI guard verifying the repository contains ZERO proprietary
Icom firmware files, ZERO real hardware dumps, and ZERO private/personal backups.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_EXTS = {".dat", ".bin", ".elf", ".img", ".rom", ".zip", ".tar", ".gz", ".xz"}
FORBIDDEN_NAME_SUBSTRINGS = [
    "IC704_ARM",
    "IC705_EEPROM",
    "IC706_EEPROM",
    "EEPROM_logical",
    "WLAN_FLASH",
    "IC553_DSP_FPGA",
    "IC705_J_original",
    "705J140.dat",
    "705E140.dat",
    "705J132.dat",
    "705RB3",
    "dsp.image",
    "wlan.image",
    "main.application",
]
MAX_FILE_BYTES = 512 * 1024  # 512 KiB hard ceiling for any committed file


def main() -> None:
    errors: list[str] = []
    for p in sorted(ROOT.rglob("*")):
        if ".git" in p.parts or "__pycache__" in p.parts or not p.is_file():
            continue
        rel = p.relative_to(ROOT).as_posix()
        if p.suffix.lower() in FORBIDDEN_EXTS:
            errors.append(f"Forbidden binary/archive extension: {rel}")
        if p.suffix.lower() != ".md":
            for bad in FORBIDDEN_NAME_SUBSTRINGS:
                if bad.lower() in p.name.lower():
                    errors.append(f"Forbidden dump/firmware filename pattern ({bad}): {rel}")
        sz = p.stat().st_size
        if sz > MAX_FILE_BYTES:
            errors.append(f"File exceeds clean-room size cap ({sz} > {MAX_FILE_BYTES}): {rel}")

    # Verify patch JSONs only contain small clean-room ops (never megabyte stock dumps)
    for pj in sorted((ROOT / "docs/patches").glob("*.json")):
        if pj.name == "index.json":
            continue
        data = json.loads(pj.read_text(encoding="utf-8"))
        total_patch_bytes = sum(len(op.get("hex", "")) // 2 for op in data.get("ops", []))
        if total_patch_bytes > 65536:
            errors.append(f"Patch JSON {pj.name} embeds suspiciously large payload ({total_patch_bytes} B)")

    if errors:
        print("CLEAN-ROOM AUDIT FAILED:")
        for e in errors:
            print("  -", e)
        sys.exit(1)

    print("CLEAN-ROOM AUDIT PASSED: zero proprietary firmware files, zero real dumps, zero oversized binaries.")


if __name__ == "__main__":
    main()
