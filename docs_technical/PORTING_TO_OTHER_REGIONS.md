# Porting Guide: Supporting Other IC-705 Regions & Firmware Versions (e.g. `705E140.dat`)

Currently, the RB3-B5 backup firmware has been mapped and hardware-verified **only for the Japanese domestic IC-705 v1.40 (`705J140.dat`)**.

Because the ARM main application binary (`main.application.bin`) differs between regional variants (`705J140.dat` vs. `705E140.dat` EU/Global/US) and firmware versions, virtual addresses **must never be reused blindly across variants**. This guide lists the exact clean-room symbol contract required to port RB3-B5 to another official IC-705 `.dat`.

---

## 1. Required Read-Only Stock ARM Symbols (17 Functions + 1 Thumb Entry)

All symbols below are strictly read-only on non-volatile storage (zero flash write/erase/unlock and zero EEPROM write symbols are ever linked):

| Symbol Name | J140 Reference VA | Calling Mode | Purpose & Contract |
| --- | --- | --- | --- |
| `grp_fs_open` | `0x201F2864` | ARM (AAPCS32) | Asynchronous SD filesystem open `(path, flags, *out_fd, stat_buf)` |
| `grp_fs_close` | `0x201F28BC` | ARM (AAPCS32) | Asynchronous SD filesystem close `(fd, 0)` |
| `grp_fs_read` | `0x201F2914` | ARM (AAPCS32) | Asynchronous SD filesystem read `(fd, buf, len, *out_read)` |
| `grp_fs_write` | `0x201F296C` | ARM (AAPCS32) | Asynchronous SD filesystem write `(fd, buf, len, *out_written)` |
| `grp_fs_sync_all` | `0x201E9170` | ARM (AAPCS32) | Flush all pending filesystem buffers to SD card |
| `grp_fs_unmount` | `0x201F2ACC` | ARM (AAPCS32) | Unmount SD device (`"sd2"`, `0`) cleanly before reboot |
| `grp_fs_wait_raw` | `0x201EF558` | ARM (AAPCS32) | Wait for filesystem request completion |
| `t28_sync_wait` | `0x200642C4` | ARM (AAPCS32) | Task-28 synchronous wait helper `(req_id, 'F', 0)` |
| `l2c_clean_pa` | `0x2012547C` | ARM (AAPCS32) | PL310 L2 cache single 32-byte line clean by physical address + sync |
| `delay_us` | `0x20005B6C` | ARM (AAPCS32) | Busy microsecond timer delay (not an RTOS scheduler yield) |
| `os_delay` | `0x202D44D1` | **Thumb-2 (bit 0 = 1)** | RTOS millisecond sleep (`osDelay`); **MUST be called via explicit ARM-to-Thumb `bx ip` veneer in `rb3_backup_entry.S`!** |
| `riic0_eeprom_read` | `0x2005BCB4` | ARM (AAPCS32) | Synchronous physical I²C EEPROM read (`mapped_addr`, `dst`, `len <= 64`) |
| `wlan_flash_reset` | `0x2006D908` | ARM (AAPCS32) | Sends SPI Reset Enable (`0x66`) + Reset (`0x99`) to WLAN flash (no memory write) |
| `wlan_bus_acquire` | `0x2006DA84` | ARM (AAPCS32) | Holds `WI_NRESET` low and initializes `RSPI2` at 16 MHz |
| `rspi2_tx_rx` | `0x2006A61C` | ARM (AAPCS32) | `RSPI2` PIO TX-then-RX transfer (used strictly with `0x9F` / `0x03`) |
| `ic553_bus_acquire` | `0x2006A2B0` | ARM (AAPCS32) | Holds FPGA/SHARC/C5517 in reset and initializes `RSPI4` at 32 MHz |
| `rspi4_tx_rx` | `0x20069FC8` | ARM (AAPCS32) | `RSPI4` PIO TX-then-RX transfer (used strictly with `0x9F` / `0x03`) |
| `t28_fw_scan_0x3d` | `0x2006C69C` | ARM (AAPCS32) | Original Firmware Update menu scan helper invoked after hook returns |

---

## 2. Required RAM Addresses, Identity Anchor & Hook Site

In addition to the 17 functions above, verify these target-specific addresses in `firmware/rb3_backup_j140_hal.c` and `firmware/rb3_backup.ld`:

1. **Application identity string (`J140_APP_ID_VA`)**: 16-byte firmware ID anchor checked at runtime before touching any hardware (e.g., `"Q70RBTXTFUZ0LTF2"` at `0x20005040` on J140).
2. **TX Inhibit & EEPROM Commit Suppression flags**:
   - `TX_INHIBIT_F5_VA` (`0x203806F5` on J140, `|= 0x48`)
   - `TX_INHIBIT_F6_VA` (`0x203806F6` on J140, `|= 0x01`)
   - `T28_SUPPRESS_EE_VA` (`0x203802C2` on J140, stock `fw_update_in_progress` flag)
   - `RIIC0_STATE_VA` (`0x20399F24` on J140) & `EEPROM_MBOX_VA` (`0x20399F78` on J140)
3. **Single Hook Site (`HOOK_VA`)**: The single `BL t28_fw_scan_0x3d` instruction inside the Firmware Update menu handler (`0x20071F18` on J140).
4. **Payload Placement (`PAYLOAD_VA`) & Non-Cacheable SRAM Arena (`NCBUF_BASE`)**:
   - Verify free trailing code space after the target `main.application.bin` end and before runtime BSS/data (`0x2036A000` on J140).
   - Verify an unused non-cacheable SRAM region of ~8 KiB (`0x209F3380` on J140) that survives RZ/A1H Watchdog reset for the post-restart receipt.

---

## 3. Critical Pitfall to Avoid: ARM-to-Thumb Interworking (`osDelay`)

> **Lesson learned from B4 → B5**: Using linker `PROVIDE(os_delay = 0x....1)` produces a `NOTYPE` symbol, which can cause `arm-none-eabi-ld` to emit an ARM `BL` directly to a Thumb-2 entry point (hanging the CPU). Always verify the target `osDelay` entry instruction set (Thumb vs. ARM) and call Thumb targets exclusively through an explicit `bx ip` veneer as in `firmware/rb3_backup_entry.S`.

---

## 4. Submitting a Regional Port PR

1. Add a new symbol script (e.g. `firmware/rb3_backup_symbols_e140.ld`) and target HAL config without breaking the verified J140 build.
2. Generate the clean-room patch JSON using `tools/generate_patch_json.py` against your own local unmodified official `.dat` (never commit the `.dat`!).
3. Add an entry to `docs/patches/index.json` with `"hardware_verified": false` until full G1–G7 hardware testing is documented.
4. Run all checks (`tests/check_clean_room.py`, `tests/test_python_tools.py`, `tests/test_js_patcher.mjs`) and open a PR using the repository PR checklist.
