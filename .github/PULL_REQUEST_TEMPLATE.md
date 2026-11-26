## Summary

<!-- Describe what this pull request changes or adds. -->

## Clean-Room & Safety Checklist (Required)

- [ ] **Zero proprietary files**: No official Icom `.dat` files, extracted binaries, or disassembled proprietary Icom code blocks are included.
- [ ] **Zero private dumps**: No `IC704`, `IC705`, `IC706`, `EEPROM_logical`, `WLAN_FLASH`, or `IC553` dumps or unit-specific calibration/hashes are included.
- [ ] **All English**: All documentation, comments, and UI strings are in English.
- [ ] **Read-only storage contract**: Any firmware payload changes maintain `storage_write_operations = 0` and leave the 64 KiB stock bootstrap untouched.
- [ ] **Tests pass locally**:
  - `python3 tests/check_clean_room.py`
  - `python3 tests/test_python_tools.py`
  - `node tests/test_js_patcher.mjs`

## Hardware Verification Scope (if applicable)

- Radio region & official `.dat` version:
- Hardware tested? (`Yes` / `No — host-tested only`):
- Notes:
