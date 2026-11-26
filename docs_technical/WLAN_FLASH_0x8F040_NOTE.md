# Technical Note: Observed WLAN Flash Change at Offset `0x8F040` (`/sys/mode.cfg`)

During real-hardware testing comparing a standalone WLAN-only backup capture against a subsequent full 5-chip backup capture on the same IC-705 JP radio, we observed a **687-byte** difference inside `WLAN_FLASH.bin` (4 MiB Macronix SPI NOR, JEDEC `C22816`, TI CC3120 SimpleLink SFLASH).

We document this observation openly so users and auditors understand what changed, why it is not caused by our read-only backup code, and why no forced modification is needed.

---

## Summary Table

| Item | Finding |
| --- | --- |
| **Flash Location** | WLAN Flash file `/sys/mode.cfg` (`0x8E000` / `0x8F000` record pair; changed byte at `0x8F040`) |
| **File Payload Offset** | Offset `0x0C` within the 80-byte valid file payload |
| **Observed Change** | 32-bit little-endian value changed from `4 -> 0` (only a single payload byte changed: `0x04 -> 0x00`) |
| **Likely Semantics** | Strong candidate: CC3120 current-role NetApp service enable bitmap with **mDNS bit `0x04` cleared** (not yet formally proven 1:1 via live API correlation) |
| **Fix Needed?** | **No — currently not required, and this byte should NOT be forced back to `0x04`.** |

---

## Confirmed Evidence

1. **Only 1 payload byte changed**: Across the 687 differing bytes between the two captures, **686 bytes** are SimpleLink SFLASH record headers, monotonic sequence counters, and mirrored backup blocks (affecting `/sys/devname.cfg`, `/sys/httpsrv.cfg`, and `/sys/mode.cfg` pairs). Actual file payload content changed at only one byte: `0x8F040` (`0x04 -> 0x00`).
2. **Not a read glitch**: Both the standalone WLAN backup and the full 5-chip backup independently passed dual-pass hardware SHA-256 readback verification on the radio. The difference is consistent with a persistent filesystem update committed by the CC3120 between the two runs.
3. **Zero flash writes in backup payload**: The RB3 backup firmware contains zero flash write/erase opcodes (`storage_write_operations = 0`, verified by static audit and symbol whitelist).

---

## Likely Writer Path (Static Analysis of Stock ARM Firmware)

Static analysis of stock IC-705 J140 ARM firmware confirms that Icom's own WLAN stop and initialization paths interact with the NetApp mDNS bit:
- **Stop path**: `sl_NetAppGet(0x40, 0, ...)` → if `(bitmap & 4)` then calls `sl_NetAppStop(4)` (opcode `0x9C61`) → `sl_Stop(200)`.
- **Init path**: Stops existing NetApps, then calls `sl_NetAppStart(4 | AP_specific_mask)` (opcode `0x9C0A`).
- **Physical writer**: The TI CC3120 Network Processor (NWP) internal filesystem executes the actual SPI NOR page updates.

### What Remains Unproven (Honest Boundaries)
- Without a live temporal trace on the radio, we **cannot** establish whether the observed clearing occurred during normal boot, Wi-Fi stop, mode switching, or while background tasks ran prior to bus acquisition.
- We have not performed a live 1:1 `sl_NetAppGet` vs. raw `/sys/mode.cfg` offset `0x0C` correlation on hardware.

---

## Current Disposition

- Keep the original value and preserve backups untouched.
- Classify as *"highly likely normal CC3120 runtime/configuration persistence; exact bit semantics pending live confirmation."*
- There is **no evidence** linking `0x8F040` to any shutdown hang, and neither B4 nor B5 modifies this address.
