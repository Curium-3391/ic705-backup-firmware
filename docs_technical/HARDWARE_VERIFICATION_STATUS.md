# Hardware Verification Status & Honest Boundaries

## What Has Been Verified on Real Hardware

All seven lifecycle gates (G1–G7) were verified on a real **Japanese domestic Icom IC-705 (`JP`, stock `705J140.dat` v1.40)** using `705RB3B5.dat` (SHA-256 `17841ee947ad2a20912b289770776817963d73cfcf9d6b16b76cd8c32fc903bb`):

| Gate | Requirement | Status | Evidence Summary |
| --- | --- | --- | --- |
| **G1** | Native ARM Execution | **Hardware Verified (JP J140)** | `RB3_SELFTEST.TXT` generated; one-shot `ARMED -> CONSUMED` trigger disarm confirmed |
| **G2** | Complete 5-Chip Readback | **Hardware Verified (JP J140)** | `IC704` (16 MiB), `IC705` (128 KiB), `IC706` (128 KiB), `WLAN` (4 MiB, `C22816`), `IC553` (2 MiB, `9D6015`) dumped without disassembly |
| **G3** | Backup Integrity | **Hardware Verified (JP J140)** | `verify_b5_backup.py` passed (`overall_ok: true`, 0 errors); reconstructed 256 KiB `EEPROM_logical.bin` passed both Phase 1F SUM8 checksums and `SX4075 V0.10-000` marker; physical `IC553` matched official J140 `dsp.image.bin` 100% bit-for-bit across 3 independent runs |
| **G4** | Zero Non-Volatile Modification | **Hardware Verified (JP J140)** | `storage_write_operations = 0`; cross-reboot EEPROM reads identical; radio operated normally after backup |
| **G5** | Safe Sync, Unmount & Auto-Reboot | **Hardware Verified (JP J140)** | `sd_sync_status = 0`, `sd_unmount_status = 0`, Watchdog auto-reboot, retained-SRAM receipt exported (`post_restart_receipt: true`, `event: 95`) |
| **G6** | Stock Firmware Restoration | **Hardware Verified (JP J140)** | Official `705J140.dat` restored cleanly via stock SD Firmware Updater (no disassembly / no external programmer) |
| **G7** | Third-Party Interoperability | **Hardware Verified (JP J140)** | Compatible third-party firmware (SQ6EMM) installed cleanly via stock SD Firmware Updater |

## What Has NOT Been Verified Yet (Community Help Needed)

1. **Non-JP Regions (`705E140.dat` EU / Global / USA and other versions)**:
   - **NOT yet supported or tested.** ARM application addresses differ between JP and Export/Global `.dat` releases. Do not attempt to bypass digest checks or flash JP firmware onto a non-JP radio.
   - Contributions to map symbols and verify `705E140.dat` are warmly welcomed.
2. **Independent Multi-Unit JP Verification**:
   - Currently tested on a single JP IC-705 unit. Additional independent reports from JP IC-705 v1.40 owners are very welcome!
3. **External Bare-Metal Disaster Recovery Write-Back**:
   - External programmer write-back (e.g., T48 writing `IC704` or external SPI writing `IC553`/`WLAN`/EEPROM after a brick) has **not** been tested on real hardware.
4. **No Real Hardware Dumps Included**:
   - This public repository intentionally contains zero real hardware dumps and zero official firmware files.
