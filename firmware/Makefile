# Reproducible build & host test Makefile for the IC-705 RB3-B5 read-only backup payload
# Reference toolchain: Arm GNU Toolchain 14.3.Rel1 (arm-none-eabi-gcc 14.3.1 20250623)

ARM_PREFIX ?= arm-none-eabi-
ARM_CC     := $(ARM_PREFIX)gcc
ARM_LD     := $(ARM_PREFIX)ld
ARM_OBJCOPY := $(ARM_PREFIX)objcopy
HOST_CC    ?= cc

BUILD_DIR  ?= build
EXPECTED_SHA256 := 882e500c8617f9e116662198ea770979ed1020bd3fe9a0a4ee977bc2ae25d294

ARM_CFLAGS := -mcpu=cortex-a9 -marm -mfloat-abi=soft -mno-unaligned-access \
              -std=c99 -Os -ffreestanding -fno-builtin -fno-stack-protector \
              -fno-common -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -Wall -Wextra -Werror

HOST_CFLAGS := -std=c99 -O2 -Wall -Wextra -Werror

OBJS := $(BUILD_DIR)/entry.o $(BUILD_DIR)/sha256.o $(BUILD_DIR)/core.o $(BUILD_DIR)/hal.o

.PHONY: all verify-hash host-test clean

all: $(BUILD_DIR)/rb3_backup_fw.bin

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/entry.o: rb3_backup_entry.S | $(BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(BUILD_DIR)/sha256.o: rb3_backup_sha256.c rb3_backup_sha256.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(BUILD_DIR)/core.o: rb3_backup_core.c rb3_backup_core.h rb3_backup_sha256.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(BUILD_DIR)/hal.o: rb3_backup_j140_hal.c rb3_backup_core.h | $(BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(BUILD_DIR)/rb3_backup_fw.elf: $(OBJS) rb3_backup.ld rb3_backup_symbols.ld
	$(ARM_LD) -L . -T rb3_backup.ld $(OBJS) -o $@

$(BUILD_DIR)/rb3_backup_fw.bin: $(BUILD_DIR)/rb3_backup_fw.elf
	$(ARM_OBJCOPY) -O binary $< $@

verify-hash: $(BUILD_DIR)/rb3_backup_fw.bin
	@echo "$(EXPECTED_SHA256)  $(BUILD_DIR)/rb3_backup_fw.bin" | shasum -a 256 -c -

host-test: | $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) rb3_backup_core.c rb3_backup_sha256.c rb3_backup_state_test.c -o $(BUILD_DIR)/rb3_state_test
	$(BUILD_DIR)/rb3_state_test $(BUILD_DIR)/sd_test
	python3 ../tools/verify_b5_backup.py $(BUILD_DIR)/sd_test

clean:
	rm -rf $(BUILD_DIR)
