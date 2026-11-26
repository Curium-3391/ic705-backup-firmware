# Architecture & Safety Model (RB3 B5)

## 1. Zero-Write Read-Only Storage Guarantee

The RB3 B5 Cortex-A9 payload (`firmware/rb3_backup_core.c` + `firmware/rb3_backup_j140_hal.c`) is strictly read-only across all five non-volatile chips:
- **IC704 (16 MiB ARM SPI NOR)**: Direct memory-mapped SPIBSC0 XIP read (`0x18000000..0x18FFFFFF`) — zero erase/program commands.
- **IC705 & IC706 (2 × 128 KiB I²C EEPROM)**: Stock J140 synchronous `RIIC0` read (`0x2005BCB4`) — zero write calls; T28 background EEPROM commit suppression (`[0x203802BC + 6] = 1`) asserted during disruptive stages.
- **WLAN Flash (4 MiB SPI NOR, JEDEC `C22816`)**: Stock J140 `RSPI2` PIO read (`0x2006A61C`) using strictly SPI opcodes `0x9F` (RDID) and `0x03` (READ).
- **IC553 (2 MiB DSP/FPGA SPI NOR, JEDEC `9D6015`)**: Stock J140 `RSPI4` PIO read (`0x20069FC8`) after bus isolation (`0x2006A2B0`) using strictly SPI opcodes `0x9F` (RDID) and `0x03` (READ).
- Every session records `"storage_write_operations": 0` in `RB3_session_manifest.json`.

## 2. Minimal Binary Modification & Stock Bootloader Preservation

- **64 KiB stock `bootstrap.bin`**: Untouched bit-for-bit (`0` bytes changed).
- **Stock `loader.bin`, `dsp.image.bin`, `wlan.image.bin`**: Untouched bit-for-bit.
- **Stock `main.application.bin` (J140)**: Only **4 bytes** modified at `0x20071F18` (single ARM `BL 0x2036A000`, replacing stock opcode `dfe9ffeb` with `38e00beb`), plus our 13,660-byte open-source payload appended at `0x2036A000`.
- **ARM-to-Thumb Interworking Fix (B5)**: Earlier B4 linked `j140_os_delay` via `PROVIDE`, causing `arm-none-eabi-ld` to emit an ARM `BL` into Thumb-2 `0x202D44D0`. B5 fixes this with an explicit ARM-to-Thumb `bx ip` veneer in `firmware/rb3_backup_entry.S`.

## 3. One-Shot Trigger, TX Inhibit, and Dual-Pass Verification

- **One-shot trigger**: The backup executes **only** when a trigger file (e.g., `RB3_FULL_BACKUP.TRG`) contains `ARMED\r\n`, and immediately rewrites it to `CONSUMED\r\n` before dumping storage.
- **TX inhibit**: Asserts J140 TX inhibit flags (`0x203806F5 |= 0x48`, `0x203806F6 |= 0x01`) before hardware access.
- **Dual-pass verification**: Pass 1 streams each chip to SD card while hashing SHA-256; Pass 2 re-reads the physical chip independently and compares its SHA-256 against Pass 1. Any mismatch aborts and records an error.

## 4. Safe Completion, Watchdog Auto-Reboot, and Retained-SRAM Receipt

1. Alternating progress checkpoints (`RB5_PROGRESS_A.json` / `RB5_PROGRESS_B.json`) track Pass 2 offsets (every 256 KiB) and stage 8/9 boundaries using at most one open SD file descriptor at a time.
2. After writing `SHA256SUMS.txt`, `RB3_session_manifest.json`, and provisional `RB5_COMPLETION.json`, B5 invokes `grp_fs_sync_all()` (`sd_sync_status = 0`) and `grp_fs_unmount("sd2", 0)` (`sd_unmount_status = 0`).
3. **Zero SD writes occur after unmount.** Post-unmount milestones (events 93/94/95) and completion status are stored in a hash-protected retained SRAM record.
4. Only if both sync and unmount succeed does B5 arm the RZ/A1H Watchdog for a clean automatic reboot.
5. On the next entry into `Firmware Update` (with no new trigger), B5 exports the verified `RB5_COMPLETION.json` (`post_restart_receipt: true`) and `RB5_POST_RESTART.json` (`event: 95`).

## 5. Returning to Stock or Third-Party Firmware

IC-705 uses a dual-slot flash layout (`Slot 0` and `Slot 1` in `IC704`). Because stock `bootstrap.bin` and the stock SD Firmware Updater remain intact:
- Delete all `RB3_*.TRG` files from `/IC-705/`.
- Flash official `705J140.dat` (or a compatible third-party `.dat`) via `MENU → SET → SD Card → Firmware Update`.
- Note: The stock updater only flips the active slot selector (`0x00BF0000`) if the newly written inactive slot differs from what was previously stored in that inactive slot. If an edge case ever occurs where the inactive slot already holds identical J140, flashing official `705J132.dat` once and then `705J140.dat` guarantees a clean slot switch.
