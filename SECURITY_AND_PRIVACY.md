# Security & Privacy Policy

## Keep Your Radio Dumps Private

When you run the IC-705 backup firmware on your own radio, the resulting files on your SD card contain sensitive unit-specific and proprietary data:

| File | Why It Must Stay Private |
| --- | --- |
| `IC705_EEPROM.bin`, `IC706_EEPROM.bin`, `EEPROM_logical.bin` | Contains unit-specific RF calibration, touch-screen calibration, and user settings |
| `WLAN_FLASH.bin` | Contains TI CC3120 SimpleLink SFLASH, which may hold Wi-Fi SSIDs, credentials, MAC address, and device certificates |
| `IC704_ARM.bin`, `IC553_DSP_FPGA.bin`, `705J140.dat`, `705RB3B5.dat` | Contains copyrighted proprietary Icom firmware |

**Never commit these files to Git, attach them to GitHub issues, or upload them to public file hosts.**

## Browser Security Model

The GitHub Pages web patcher (`docs/index.html`) is designed for strict local-only execution:
- Enforces a restrictive Content Security Policy (`default-src 'self'; connect-src 'self'; form-action 'none'`).
- Performs all MD5/SHA-256 hashing, decoding, patching, LZSS compression, and EEPROM striping locally in your browser's Web Worker / memory.
- Makes zero external network requests and uploads zero user bytes.
- Refuses to offer a downloadable `.dat` unless both **MD5** and **SHA-256** match the hardware-verified build byte-for-byte.
