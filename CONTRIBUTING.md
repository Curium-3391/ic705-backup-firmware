# Contributing to `ic705-backup-firmware`

Thank you for helping improve and verify this open-source IC-705 backup project!

Because this repository ships **no real hardware dumps** and has so far been hardware-verified **only on the Japanese domestic IC-705 (`705J140.dat` v1.40)**, community contributions are especially valuable.

## Ways to Contribute

### 1. Independent Hardware Verification Reports (JP `705J140.dat`)
- Use the **Hardware Verification Report** issue template (`.github/ISSUE_TEMPLATE/hardware-verification-report.yml`).
- Share only non-sensitive metadata: radio region, official input SHA-256, built output SHA-256, `RB3_session_manifest.json` (or status flags), `RB5_COMPLETION.json`, `RB5_POST_RESTART.json`, and whether stock restoration succeeded.
- **NEVER upload or paste `IC705_EEPROM.bin`, `IC706_EEPROM.bin`, `EEPROM_logical.bin`, `WLAN_FLASH.bin`, `IC704_ARM.bin`, `IC553_DSP_FPGA.bin`, or official `.dat` files.**

### 2. Regional Ports (EU / Global / US `705E140.dat` and Other Versions)
The ARM main application virtual addresses in `firmware/rb3_backup_symbols.ld` and `firmware/rb3_backup_j140_hal.c` are strictly bound to **JP J140** (`main.application.bin` SHA-256 `d25971deae61ab0db52ed80338516119367887728da6e67a29d05710aceac4a9`).
To add support for another region (such as `705E140.dat`):
1. Identify the corresponding stock read-only symbols (`grp_fs_*`, `t28_sync_wait`, `delay_us`, `osDelay` Thumb-2 entry, `riic0_eeprom_read`, `wlan_bus_acquire`, `wlan_flash_reset`, `rspi2_tx_rx`, `ic553_bus_acquire`, `rspi4_tx_rx`, TX inhibit flags, and menu hook address) in your legally owned `.dat`.
2. Add a dedicated symbol file (e.g., `firmware/rb3_backup_symbols_e140.ld`) and clean-room patch JSON (`docs/patches/705E140_rb3_b5.json`).
3. Mark `"hardware_verified": false` until full G1–G7 hardware verification is completed and documented.
4. Ensure all CI checks pass (`tests/check_clean_room.py`, `tests/test_python_tools.py`, `tests/test_js_patcher.mjs`).

## Strict Clean-Room Rules
- **NEVER** commit official Icom `.dat` files, extracted Icom binaries, or any hardware dump.
- **NEVER** include proprietary disassembled code blocks from Icom firmware.
- Keep all documentation, comments, commit messages, and UI text in **English**.
