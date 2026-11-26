# IC-705 Full-Storage Read-Only Backup Firmware & Web Patcher

An independent, clean-room open-source toolkit and client-side GitHub Pages web patcher that converts your own **official, unmodified Icom IC-705 firmware `.dat`** into a **dedicated read-only 5-chip hardware backup firmware** (`705RB3B5.dat`).

> **Live Web Patcher (GitHub Pages)**: https://curium-3391.github.io/ic705-backup-firmware/ (runs 100% locally in your browser — zero server uploads).
>
> Inspired by the clean-room client-side patching architecture of [`sq6emm/icom-custom-firmware`](https://github.com/sq6emm/icom-custom-firmware) ([live page](https://sq6emm.github.io/icom-custom-firmware/)).

---

## Important Honest Disclosures

1. **No real hardware dumps or official Icom firmware files are included.**
   - For copyright and user-privacy reasons, this repository contains **zero** official Icom `.dat` files, **zero** extracted Icom binaries, and **zero** real radio dumps (`IC704`, `IC553`, `IC705`/`IC706` EEPROM, or `WLAN` Flash).
   - You supply your own legally downloaded official Icom `.dat` locally in your browser or CLI.
   - **Community verification and contributions are warmly welcomed!**
2. **Hardware-verified on the Japanese domestic version (`IC-705 JP`, `705J140.dat` v1.40) ONLY so far.**
   - Full hardware lifecycle verification (G1–G7: native execution, all 5 chips readback, SHA-256 integrity, zero storage writes, Watchdog auto-reboot + retained-SRAM post-restart receipt, stock J140 restoration, and compatible third-party SQ6EMM restoration) has so far been tested **only on a JP domestic IC-705 running official `705J140.dat`**.
   - Export/Global/US variants (e.g., `705E140.dat`) have different ARM application addresses and are **not yet supported**. We actively welcome community contributors to help port symbols and verify other regions!
3. **Unofficial software — use at your own risk.**
   - This project is not affiliated with or endorsed by Icom Inc. Always use reliable power when flashing firmware, and keep your unmodified official `.dat` ready to restore stock firmware afterward.

---

## What the Backup Firmware Reads (Zero Non-Volatile Writes)

Without opening the case, desoldering chips, or attaching clip programmers, `705RB3B5.dat` streams all **five** non-volatile storage devices to `/IC-705/` on the SD card, performs an independent **second hardware read pass** on every chip to verify SHA-256 identity, and enforces `storage_write_operations = 0`:

| Chip | Device & Capacity | SD Output File | Sensitivity |
| --- | --- | --- | --- |
| `IC704` | ISSI `IS25LP128F` ARM SPI NOR (`16 MiB`) | `IC704_ARM.bin` | Post-modification active dump (not a pre-mod external T48 factory dump) |
| `IC705` | I²C EEPROM Bank 0 (`128 KiB`, even 64-byte stripes) | `IC705_EEPROM.bin` | **PRIVATE** — unit-specific RF/touch calibration & user settings |
| `IC706` | I²C EEPROM Bank 1 (`128 KiB`, odd 64-byte stripes) | `IC706_EEPROM.bin` | **PRIVATE** — pair with `IC705_EEPROM.bin` to reconstruct `EEPROM_logical.bin` |
| `WLAN Flash` | Macronix SPI NOR (`4 MiB`, JEDEC `C22816`) | `WLAN_FLASH.bin` | **PRIVATE** — TI CC3120 SimpleLink SFLASH (may hold Wi-Fi config/MAC) |
| `IC553` | ISSI `IS25LP016D` DSP/FPGA Flash (`2 MiB`, JEDEC `9D6015`) | `IC553_DSP_FPGA.bin` | C5517 DV-DSP + ADSP-21478 SHARC IF-DSP + Voice Synth + Cyclone 10 LP FPGA |

### Minimal Footprint & Safety Design
- **Stock 64 KiB bootstrap (`bootstrap.bin`) is untouched bit-for-bit.**
- **Loader, DSP/FPGA image, and WLAN image in the `.dat` are untouched bit-for-bit.**
- Modifies **only 4 bytes** in the stock J140 main ARM image (`0x20071F18`: single ARM `BL 0x2036A000`) plus appends our **13,660-byte open-source Cortex-A9 payload** compiled from [`firmware/`](firmware/).
- **One-shot trigger protection**: Only runs when an explicit trigger file (`RB3_FULL_BACKUP.TRG` containing `ARMED\r\n`) is present; immediately disarms it to `CONSUMED\r\n` before reading storage.
- **TX inhibit** is asserted before hardware access.
- **Safe completion lifecycle**: Syncs and unmounts the SD card (`sd_sync_status = 0`, `sd_unmount_status = 0`), triggers an RZ/A1H Watchdog auto-reboot, and exports a retained-SRAM receipt (`RB5_COMPLETION.json` + `RB5_POST_RESTART.json`) on the next menu entry.

---

## Known Observation: WLAN Flash Offset `0x8F040` (`/sys/mode.cfg`)

We transparently document a minor 1-byte difference observed in `WLAN_FLASH.bin` between a standalone WLAN-only backup and a subsequent full backup. See [`docs_technical/WLAN_FLASH_0x8F040_NOTE.md`](docs_technical/WLAN_FLASH_0x8F040_NOTE.md) for the full technical breakdown:

| Item | Conclusion |
| --- | --- |
| Location | WLAN Flash file `/sys/mode.cfg` (flash offset `0x8F040`) |
| Payload Offset | Offset `0x0C` inside the 80-byte file payload |
| Observed Change | 32-bit value `4 -> 0` (only a single payload byte changed: `0x04 -> 0x00`) |
| Likely Meaning | Strong candidate: CC3120 NetApp service enable bitmap with **mDNS bit `0x04` cleared** (not yet formally proven 1:1 via live API correlation) |
| Fix Needed? | **No — do NOT force this byte back to `0x04`.** |

Of the **687 bytes** differing between the two runs, 686 bytes are TI SimpleLink SFLASH record headers, sequence counters, and mirrored backup blocks; only `0x8F040` changed in payload content, and both backups passed dual-pass hardware SHA-256 verification. Stock Icom ARM WLAN routines call `sl_NetAppStop(4)` / `sl_NetAppStart(...)` while the CC3120 NWP performs physical SFLASH writes. Because there was no live temporal trace, we cannot establish whether the CC3120 wrote this during boot, Wi-Fi stop, mode switching, or before bus acquisition. There is no evidence it causes any shutdown issue, and B4/B5 never write to this address.

---

## Quick Start

### Option A: Browser Web Patcher (GitHub Pages)
1. Open the GitHub Pages site (`docs/index.html`).
2. Drop your own official, unmodified `705J140.dat`.
3. Click **Build & Verify Backup .dat** — the Web Worker builds `705RB3B5.dat` locally and refuses to offer a download unless **both MD5 and SHA-256 match** the hardware-verified build (`17841ee947ad2a20912b289770776817963d73cfcf9d6b16b76cd8c32fc903bb`).
4. Download `RB3_FULL_BACKUP.TRG` from the **One-Shot SD Trigger Kit** section.
5. After backing up your radio, you can also drop `IC705_EEPROM.bin` + `IC706_EEPROM.bin` into the **Local Offline EEPROM Reconstructor** right on the page (100% local in browser) to verify SUM8 checksums and export `EEPROM_logical.bin`.

### Option B: Command-Line Python Tools (Zero External Dependencies)

Build `705RB3B5.dat` from your local official `705J140.dat`:
```bash
python3 tools/patch_dat.py \
  --input /path/to/705J140.dat \
  --patch docs/patches/705J140_rb3_b5.json \
  --output 705RB3B5.dat
```

Verify a completed B5 SD backup folder (read-only):
```bash
python3 tools/verify_b5_backup.py /path/to/SD/IC-705
python3 tools/analyze_b5_progress.py /path/to/SD/IC-705
```

Reconstruct `EEPROM_logical.bin` (256 KiB) offline on PC:
```bash
python3 tools/reconstruct_eeprom.py \
  --ic705 /path/to/IC705_EEPROM.bin \
  --ic706 /path/to/IC706_EEPROM.bin \
  --out /path/to/EEPROM_logical.bin
```

---

## Repository Layout

- `docs/` — GitHub Pages static web patcher, SD trigger generator, and local offline EEPROM verifier/reconstructor
- `docs/patches/` — Clean-room patch registry (`index.json`) and J140 B5 patch (`705J140_rb3_b5.json`)
- `firmware/` — 100% open-source Cortex-A9 C & Assembly source code and host C99 state-machine test suite
- `tools/` — Standalone Python CLI patcher, RZ/A1 codec, backup verifiers, and EEPROM reconstructor
- `docs_technical/` — Architecture & safety model ([`ARCHITECTURE_AND_SAFETY.md`](docs_technical/ARCHITECTURE_AND_SAFETY.md)), verification matrix ([`HARDWARE_VERIFICATION_STATUS.md`](docs_technical/HARDWARE_VERIFICATION_STATUS.md)), porting guide for EU/Global/US regions ([`PORTING_TO_OTHER_REGIONS.md`](docs_technical/PORTING_TO_OTHER_REGIONS.md)), troubleshooting & recovery ([`TROUBLESHOOTING_AND_RECOVERY.md`](docs_technical/TROUBLESHOOTING_AND_RECOVERY.md)), and WLAN `0x8F040` analysis ([`WLAN_FLASH_0x8F040_NOTE.md`](docs_technical/WLAN_FLASH_0x8F040_NOTE.md))
- `tests/` — Automated Node.js, Python, C state-machine, and clean-room zero-proprietary-files CI checks

---

## Running Tests

```bash
# 1. Clean-room guard (ensures zero proprietary firmware or dumps exist in repo)
python3 tests/check_clean_room.py

# 2. Python + Host C99 state-machine tests
python3 tests/test_python_tools.py

# 3. Browser JS module unit & schema tests (Node.js >= 20)
node tests/test_js_patcher.mjs

# Optional: End-to-end byte-identity test against your own local 705J140.dat
IC705_J140_DAT=/path/to/705J140.dat node tests/test_js_patcher.mjs
IC705_J140_DAT=/path/to/705J140.dat python3 tests/test_python_tools.py
```

---

## Contributing & Regional Support

Want to help verify on another JP IC-705, or port to **EU/Global/US (`705E140.dat`)** or future firmware versions?
Please read [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`SECURITY_AND_PRIVACY.md`](SECURITY_AND_PRIVACY.md).

## Special Thanks & Acknowledgments

Special thanks to **Dawid ([SQ6EMM](https://github.com/sq6emm))** for his pioneering open-source work on the Icom RZ/A1 transceiver platform! This project would not have been possible without his research, tooling, and clean-room distribution architecture:

- **[`sq6emm/icom-custom-firmware`](https://github.com/sq6emm/icom-custom-firmware)** ([live web patcher](https://sq6emm.github.io/icom-custom-firmware/)) — Inspiration for the zero-upload client-side GitHub Pages patcher design, and the MIT-licensed RZ/A1 container codec (`docs/js/codec.js`, `tools/ic_rza1_codec.py`) and incremental MD5 implementation (`docs/js/md5.js`).
- **[`sq6emm/ic705-ic9700-ic905-bring-your-own-firmware`](https://github.com/sq6emm/ic705-ic9700-ic905-bring-your-own-firmware)** & **[`sq6emm/icap-sdk`](https://github.com/sq6emm/icap-sdk)** — Foundational Icom `.dat` container documentation and open-source ecosystem that also enabled independent round-trip verification and third-party interoperability testing (G7).

## License

- Licensed under the [MIT License](LICENSE).
- See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for full upstream copyright and license notices.
