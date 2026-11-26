#!/usr/bin/env python3
"""
test_python_tools.py — Automated unit and synthetic integration tests for:
  - tools/ic_rza1_codec.py
  - tools/reconstruct_eeprom.py
  - tools/verify_b5_backup.py & tools/analyze_b5_progress.py
  - firmware/rb3_backup_state_test.c (host C99 state-machine & abrupt-stop tests)
  - Optional E2E real .dat check when IC705_J140_DAT env var is provided
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_b5_progress  # noqa: E402
import ic_rza1_codec as codec  # noqa: E402
import patch_dat  # noqa: E402
import reconstruct_eeprom as ee  # noqa: E402
import verify_b5_backup  # noqa: E402


def test_codec_roundtrip() -> None:
    sample = bytes((i * 29 + (i >> 3)) & 0xFF for i in range(4096))
    enc = codec.autokey_encode(sample, 0x64, 25, 160)
    dec, _ = codec.autokey_decode(enc, 0x64, 25, 160)
    assert dec == sample
    comp = codec.lzss_compress(sample)
    decomp, used = codec.lzss_decompress(comp, len(sample))
    assert used == len(comp) and decomp == sample


def test_eeprom_bijection_and_anchors() -> None:
    logical = bytearray((i * 37 + 11) & 0xFF for i in range(ee.LOGICAL_SIZE))
    logical[0x3FB50] = sum(logical[0x3FB47:0x3FB50]) & 0xFF
    logical[0x3FB5B] = sum(logical[0x3FB51:0x3FB5B]) & 0xFF
    logical[0x3FF40:0x3FF50] = b"SX4075 V0.10-000"
    p705, p706 = ee.split_logical_to_physical(bytes(logical))
    rebuilt = ee.reconstruct_logical_from_physical(p705, p706)
    assert rebuilt == bytes(logical)
    anchors = ee.inspect_logical_anchors(rebuilt)
    assert anchors["sum8_domain1_0x3fb47_0x3fb50"]["valid"] is True
    assert anchors["sum8_domain2_0x3fb51_0x3fb5b"]["valid"] is True
    assert bytes.fromhex(anchors["touch_marker_0x3ff40_hex"]) == b"SX4075 V0.10-000"


def test_c_state_machine_and_verifiers() -> None:
    fw = ROOT / "firmware"
    with tempfile.TemporaryDirectory(prefix="ic705_b5_ci_") as td:
        exe = Path(td) / "state_test"
        subprocess.run(
            [
                "cc",
                "-std=c99",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(fw / "rb3_backup_core.c"),
                str(fw / "rb3_backup_sha256.c"),
                str(fw / "rb3_backup_state_test.c"),
                "-o",
                str(exe),
            ],
            check=True,
        )
        sd = Path(td) / "sd"
        subprocess.run([str(exe), str(sd)], check=True, capture_output=True, text=True)
        res = verify_b5_backup.verify(sd)
        assert res["overall_ok"] is True, res
        prog = analyze_b5_progress.inspect(sd)
        assert prog["latest"]["event"] == 95 and prog["latest"]["post_restart"] is True


def test_optional_real_dat_e2e() -> None:
    dat_env = os.environ.get("IC705_J140_DAT")
    if not dat_env or not Path(dat_env).exists():
        print("INFO: IC705_J140_DAT not set; skipping local official .dat E2E test.")
        return
    patch = json.loads((ROOT / "docs/patches/705J140_rb3_b5.json").read_text(encoding="utf-8"))
    raw = Path(dat_env).read_bytes()
    out, out_md5, out_sha256 = patch_dat.build_patched(raw, patch)
    assert len(out) == patch["output"]["size_bytes"]
    assert out_md5 == patch["output"]["md5"]
    assert out_sha256 == patch["output"]["sha256"]
    print(f"PASS (real J140 .dat Python E2E): SHA-256={out_sha256}")


def main() -> None:
    test_codec_roundtrip()
    test_eeprom_bijection_and_anchors()
    test_c_state_machine_and_verifiers()
    test_optional_real_dat_e2e()
    print("ALL PYTHON & C STATE-MACHINE TESTS PASSED.")


if __name__ == "__main__":
    main()
