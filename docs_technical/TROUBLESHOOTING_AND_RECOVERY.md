# Troubleshooting, Progress Checkpoints & Firmware Recovery Guide

This guide explains how to interpret the B5 diagnostic files written to `/IC-705/` on your SD card, how to verify a completed backup, and how to cleanly return to stock or compatible third-party firmware.

---

## 1. Expected Files After a Successful Full B5 Backup

After flashing `705RB3B5.dat`, running `RB3_FULL_BACKUP.TRG`, waiting for the automatic Watchdog reboot, and entering `Firmware Update` once more **without** a new armed trigger, `/IC-705/` should contain:

| File | Expected Size | Meaning |
| --- | ---: | --- |
| `IC704_ARM.bin` | `16,777,216 B` | Post-modification active `IC704` SPI NOR dump (NOT a pre-mod external T48 factory dump) |
| `IC705_EEPROM.bin` | `131,072 B` | Physical `IC705` EEPROM dump (even 64-byte stripes) — **keep private** |
| `IC706_EEPROM.bin` | `131,072 B` | Physical `IC706` EEPROM dump (odd 64-byte stripes) — **keep private** |
| `WLAN_FLASH.bin` | `4,194,304 B` | Physical WLAN SPI NOR dump (JEDEC `C22816`) — **keep private** |
| `IC553_DSP_FPGA.bin` | `2,097,152 B` | Physical `IC553` DSP/FPGA SPI NOR dump (JEDEC `9D6015`) |
| `RB3_SELFTEST.TXT` | `132 B` | Confirms `BUILD_ID=RB3-DEDICATED-BACKUP-FINAL-V5` and zero-write mode |
| `RB3_FULL_BACKUP.TRG` | `10 B` | Overwritten to `CONSUMED\r\n` so it cannot re-run accidentally |
| `SHA256SUMS.txt` | ~`412 B` | On-device SHA-256 checksums for all 5 dumps |
| `RB3_session_manifest.json` | ~`1.6 KiB` | Target statuses, capacities, JEDEC IDs, and `"storage_write_operations": 0` |
| `RB5_COMPLETION.json` | ~`435 B` | Post-restart receipt (`"post_restart_receipt": true`, all status fields `0`) |
| `RB5_POST_RESTART.json` | ~`339 B` | Retained-SRAM final checkpoint (`"event": 95`, `"post_restart": true`) |
| `RB5_PROGRESS_A.json` / `B.json` | ~`340 B` each | Alternating synced progress checkpoints (latest pre-unmount event is `92`) |

### Example of a Valid `RB5_COMPLETION.json` (Placeholder Hashes)
```json
{
  "build_id": "RB3-DEDICATED-BACKUP-FINAL-J140-V5",
  "requested_mask": 63,
  "backup_status": 0,
  "completion_status": 0,
  "progress_log_status": 0,
  "sd_sync_status": 0,
  "sd_unmount_status": 0,
  "completion_log_status": 0,
  "post_restart_receipt": true,
  "wlan_sha256": "<64-hex-char SHA-256 matching WLAN_FLASH.bin>",
  "arm_sha256": "<64-hex-char SHA-256 matching IC704_ARM.bin>"
}
```

### Automated Verification on PC
```bash
python3 tools/verify_b5_backup.py /path/to/SD/IC-705
python3 tools/analyze_b5_progress.py /path/to/SD/IC-705
python3 tools/reconstruct_eeprom.py \
  --ic705 /path/to/SD/IC-705/IC705_EEPROM.bin \
  --ic706 /path/to/SD/IC-705/IC706_EEPROM.bin \
  --out /path/to/EEPROM_logical.bin
```

---

## 2. Progress Checkpoint Event Codes (`RB5_PROGRESS_A/B.json` & `RB5_POST_RESTART.json`)

If an unexpected interruption ever occurs, preserve `RB5_PROGRESS_A.json` and `RB5_PROGRESS_B.json` before re-running and inspect them with `tools/analyze_b5_progress.py`.

| Event Code | Meaning |
| --- | --- |
| `100..106` | `IC704` (`0`=bus acquire, `1`=pass 1 start, `2`=pass 1 close, `3`=pass 2 256 KiB step, `4`=pass 2 complete, `5`=hash match, `6`=target return) |
| `110..116` | `IC705` EEPROM (same sub-codes `0..6`) |
| `120..126` | `IC706` EEPROM (same sub-codes `0..6`) |
| `130..136` | `WLAN` Flash (same sub-codes `0..6`) |
| `140..146` | `IC553` DSP/FPGA Flash (same sub-codes `0..6`; e.g., `143` with `offset=262144` means Pass 2 reached 256 KiB) |
| `80` / `81` | `SHA256SUMS.txt` start / complete |
| `82` / `83` | `RB3_session_manifest.json` start / complete |
| `84` / `85` | Provisional `RB5_COMPLETION.json` start / complete |
| `90` / `91` | Final `grp_fs_sync_all()` start / returned |
| `92` | About to call `grp_fs_unmount("sd2", 0)` — **last permitted SD write before unmount** |
| `93` / `94` / `95` | Unmount returned / core returned / restart gate passed (stored in retained SRAM; exported to `RB5_POST_RESTART.json` after reboot) |
| `96` | Completion error hold (Watchdog reboot inhibited because sync/unmount failed) |
| `97` / `98` | Error or non-disruptive report sync boundary |

> **Important interpretation note**: A checkpoint proves that the firmware reached that boundary; it does not prove that its own SD sync completed or that the next operation started. Never mix stale `RB5_PROGRESS_*.json` files from different runs in the same folder.

---

## 3. Returning to Official Stock Firmware (or Compatible Third-Party Firmware)

1. **Remove all trigger files**: Delete every `RB3_*.TRG` file from `/IC-705/` on the SD card.
2. **Copy official stock firmware**: Place your unmodified official `705J140.dat` into `/IC-705/` (keep only that `.dat`).
3. **Run the stock updater**: On the radio, go to `MENU → SET → SD Card → Firmware Update`, select `705J140`, and confirm.
4. **Dual-slot edge case (`0x00BF0000` slot selector)**:
   - Icom's stock updater only flips the active slot selector when the newly written inactive slot differs from the previous content of that inactive slot.
   - In normal use after running B5 (or B4 → B5), flashing official `705J140.dat` immediately switches back to stock J140.
   - If you ever encounter an edge case where the inactive slot already held an identical J140 image and the radio stays on the backup slot, flash an official different version (e.g., official `705J132.dat`) once, and then flash official `705J140.dat`.
5. **Disaster recovery caveat**:
   - The IC-705 stock `loader.bin` does **not** perform automatic rollback if an active firmware slot is corrupted or if power is lost while writing sector 0 (`bootstrap.bin`). Because `705RB3B5.dat` never modifies `bootstrap.bin`, sector 0 is not rewritten during normal updates. Always use a well-charged battery or stable external DC supply when flashing any firmware.
