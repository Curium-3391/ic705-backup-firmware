/*
 * rb3_backup_entry.S - Cortex-A9 ARM single-hook trampoline & cache maintenance
 * Phase ARM-RB3 Final: Dedicated One-Time Backup Firmware
 *
 * Single minimal 4-byte stock hook:
 *   Stock 0x20071F18: bl 0x2006C69C -> bl 0x2036A000 (rb3b_t28_trampoline_0x3d)
 *
 * When no armed RB3_*.TRG exists on SD (or session already ran), this trampoline
 * restores {r4, lr} and tail-branches directly to j140_t28_fw_scan_0x3d (0x2006C69C),
 * preserving 100% stock arguments, registers, stack, return value, and updater UI.
 */
    .syntax unified
    .cpu cortex-a9
    .arm

    .section .text.rb3b_vectors, "ax", %progbits
    .align 2

    .global rb3b_t28_trampoline_0x3d
    .type   rb3b_t28_trampoline_0x3d, %function
rb3b_t28_trampoline_0x3d:
    push    {r4, lr}
    bl      rb3b_j140_entry_from_t28
    pop     {r4, lr}
    b       j140_t28_fw_scan_0x3d
    .size   rb3b_t28_trampoline_0x3d, . - rb3b_t28_trampoline_0x3d

/* void rb3b_cache_clean_range(const void *addr, uint32_t len) */
    .section .text.rb3b_cache_clean_range, "ax", %progbits
    .align 2
    .global rb3b_cache_clean_range
    .type   rb3b_cache_clean_range, %function
rb3b_cache_clean_range:
    cmp     r1, #0
    bxeq    lr
    push    {r4, r5, r6, lr}
    add     r5, r0, r1
    bic     r4, r0, #31
1:
    mcr     p15, 0, r4, c7, c10, 1  /* L1 DCCMVAC */
    dsb     sy
    mov     r0, r4
    bl      j140_l2c_clean_pa       /* PL310 clean by PA + sync (0x2012547C) */
    dsb     sy
    add     r4, r4, #32
    cmp     r4, r5
    blo     1b
    pop     {r4, r5, r6, pc}
    .size   rb3b_cache_clean_range, . - rb3b_cache_clean_range

/* Linker NOTYPE absolute odd symbols can produce an ARM BL with the low bit
 * discarded. Explicit BX retains the Thumb entry bit, LR and arguments. */
    .section .text.rb5_os_delay, "ax", %progbits
    .align 2
    .global j140_os_delay
    .type j140_os_delay, %function
j140_os_delay:
    ldr ip, =0x202D44D1
    bx ip
    .ltorg
    .size j140_os_delay, . - j140_os_delay
