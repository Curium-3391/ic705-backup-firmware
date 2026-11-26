#!/usr/bin/env python3
"""Read-only B5 acceptance: backup files AND a matching post-restart receipt.

Usage: python3 verify_b5_backup.py /path/to/IC-705
No reconstruction or writes to the input directory are performed.
"""
import hashlib
import json
import sys
import analyze_b5_progress
from pathlib import Path


def verify(directory):
    directory = Path(directory)
    errors = []
    manifest = json.loads((directory / "RB3_session_manifest.json").read_text())
    receipt = json.loads((directory / "RB5_COMPLETION.json").read_text())
    sums = {}
    for line in (directory / "SHA256SUMS.txt").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            digest, name = line.split()
            sums[name] = digest
    if manifest.get("build_id") != "RB3-DEDICATED-BACKUP-FINAL-J140-V5":
        errors.append("Manifest is not V5")
    if receipt.get("build_id") != manifest.get("build_id"):
        errors.append("Receipt version mismatch")
    if not manifest.get("stage1_native_ok") or not manifest.get("stage2_sd_ok"):
        errors.append("Self-test did not pass")
    if not manifest.get("trigger_consumed") or manifest.get("backup_status") != 0:
        errors.append("Backup failed or trigger not consumed")
    if manifest.get("overall_status") != 0:
        errors.append("Manifest records an error")
    if receipt.get("requested_mask") != manifest.get("requested_mask"):
        errors.append("Receipt belongs to a different requested mask")
    if not receipt.get("post_restart_receipt"):
        errors.append("Final receipt missing: after automatic restart enter Firmware Update once without a new trigger")
    for key in ["backup_status", "completion_status", "sd_sync_status", "sd_unmount_status", "completion_log_status", "progress_log_status"]:
        if receipt.get(key) != 0:
            errors.append(f"Receipt {key}={receipt.get(key)}")
    progress = analyze_b5_progress.inspect(directory)
    final = progress["latest"]
    if not final or not final.get("post_restart") or final.get("event") != 95:
        errors.append("Missing retained restart-gate progress export")
    elif final.get("requested_mask") != manifest.get("requested_mask") or final.get("wlan_sha256") != receipt.get("wlan_sha256"):
        errors.append("Retained progress belongs to a different backup")
    files = {}
    expected = [(2,"IC704",16777216), (4,"IC705",131072), (8,"IC706",131072),
                (16,"WLAN",4194304), (32,"IC553",2097152)]
    for bit, key, size in expected:
        if not manifest.get("requested_mask",0) & bit:
            continue
        target = manifest["targets"][key]
        path = directory / target["file"]
        if not target.get("executed") or not target.get("passed") or target.get("status") != 0:
            errors.append(f"{key} did not pass")
        if not path.exists():
            errors.append(f"Missing {path.name}")
            continue
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        files[key] = {"bytes":len(data), "sha256":digest}
        if len(data) != size or target.get("bytes") != size:
            errors.append(f"{key} size mismatch")
        if digest != target.get("sha256") or digest != sums.get(path.name):
            errors.append(f"{key} hash mismatch")
        receipt_key = {"WLAN":"wlan_sha256", "IC704":"arm_sha256"}.get(key)
        if receipt_key and receipt.get(receipt_key) != digest:
            errors.append(f"{key} receipt belongs to a different backup")
    if manifest["targets"]["WLAN"].get("passed") and manifest["targets"]["WLAN"].get("jedec") != "C22816":
        errors.append("Unexpected WLAN JEDEC on this radio")
    if manifest["targets"]["IC553"].get("passed") and manifest["targets"]["IC553"].get("jedec") != "9D6015":
        errors.append("Unexpected IC553 JEDEC")
    return {"overall_ok":not errors, "errors":errors, "files":files,
            "note":"Backup integrity and final sync/unmount receipt checked; physical power-key shutdown still requires operator observation."}


if __name__ == "__main__":
    try:
        result = verify(sys.argv[1])
    except (OSError, ValueError, KeyError, IndexError) as exc:
        result = {"overall_ok":False,"errors":[str(exc)]}
    print(json.dumps(result,indent=2))
    sys.exit(0 if result["overall_ok"] else 1)
