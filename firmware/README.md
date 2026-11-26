# Open-Source Cortex-A9 Backup Payload (`firmware/`)

This directory contains the 100% open-source C99 and ARMv7-A assembly source code for the **RB3 B5** read-only 5-chip backup payload (`RB3-DEDICATED-BACKUP-FINAL-J140-V5`).

## Key Files

- `rb3_backup_entry.S` — ARMv7-A entry stub, cache-clean helper, and explicit ARM-to-Thumb `bx ip` veneer for `osDelay` (fixing the B4 linker interworking defect)
- `rb3_backup_core.c` / `rb3_backup_core.h` — Hardware-agnostic read-only backup state machine, dual-pass SHA-256 verification, alternating SD progress checkpoints (`RB5_PROGRESS_A/B.json`), and post-restart SRAM receipt export
- `rb3_backup_j140_hal.c` — Read-only hardware abstraction layer bound to verified IC-705 **JP J140** symbols (`storage_write_operations = 0`)
- `rb3_backup_sha256.c` / `rb3_backup_sha256.h` — Minimal freestanding SHA-256 implementation
- `rb3_backup.ld` / `rb3_backup_symbols.ld` — Linker layout (`0x2036A000` payload base, `0x209F3380` non-cacheable SRAM arena) and J140 read-only symbol bindings
- `rb3_backup_state_test.c` — Host C99 unit & fault-injection test suite (17 state-machine scenarios + abrupt checkpoint stops + truncated-record fallback)

## Quick Build & Verification via `Makefile`

```bash
cd firmware
make host-test      # Compiles & runs host C99 state-machine tests + verify_b5_backup.py
make verify-hash    # Requires arm-none-eabi-gcc (14.3.Rel1); verifies bit-identical SHA-256
make clean
```

## Running Host C99 State-Machine Tests Manually

```bash
cc -std=c99 -O2 -Wall -Wextra -Werror \
  firmware/rb3_backup_core.c \
  firmware/rb3_backup_sha256.c \
  firmware/rb3_backup_state_test.c \
  -o /tmp/rb3_state_test

/tmp/rb3_state_test /tmp/rb3_test_sd
python3 tools/verify_b5_backup.py /tmp/rb3_test_sd
```

## Reproducing the ARMv7-A Payload Binary (`rb3_backup_fw.bin`)

The reference hardware-verified build was compiled with **Arm GNU Toolchain `14.3.Rel1`** (`arm-none-eabi-gcc 14.3.1 20250623`):

```bash
mkdir -p build
CFLAGS="-mcpu=cortex-a9 -marm -mfloat-abi=soft -mno-unaligned-access -std=c99 -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-common -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror"

arm-none-eabi-gcc $CFLAGS -c firmware/rb3_backup_entry.S    -o build/entry.o
arm-none-eabi-gcc $CFLAGS -c firmware/rb3_backup_sha256.c   -o build/sha256.o
arm-none-eabi-gcc $CFLAGS -c firmware/rb3_backup_core.c     -o build/core.o
arm-none-eabi-gcc $CFLAGS -c firmware/rb3_backup_j140_hal.c -o build/hal.o

arm-none-eabi-ld -L firmware -T rb3_backup.ld \
  build/entry.o build/sha256.o build/core.o build/hal.o -o build/rb3_backup_fw.elf
arm-none-eabi-objcopy -O binary build/rb3_backup_fw.elf build/rb3_backup_fw.bin
```

Expected payload size and SHA-256 for J140 B5:
- Size: `13,660` bytes
- SHA-256: `882e500c8617f9e116662198ea770979ed1020bd3fe9a0a4ee977bc2ae25d294`
