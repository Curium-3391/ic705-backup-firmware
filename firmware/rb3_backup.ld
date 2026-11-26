/*
 * rb3_backup.ld - Cortex-A9 freestanding linker script for Dedicated Backup Firmware
 *
 * Placement verified against J140 scatterload (0x2000530C):
 *   - Stock J140 image:  0x20005000..0x20369ABC
 *   - Unreferenced gap:  0x20369ABC..0x2037FFFF (91,460 B executable RWX SRAM)
 *   - RB3 Backup Code:   0x2036A000..0x2036DFFF (strict 16 KiB ceiling)
 *   - Non-Cacheable DMA: 0x209F3380..0x209F537F (8 KiB inside 52,336 B NC SRAM tail)
 */

INCLUDE rb3_backup_symbols.ld

ENTRY(rb3b_t28_trampoline_0x3d)

MEMORY
{
    RB3_CODE (rwx) : ORIGIN = 0x2036A000, LENGTH = 0x00004000
    RB3_NC   (rw)  : ORIGIN = 0x209F3380, LENGTH = 0x00002000
}

SECTIONS
{
    .text 0x2036A000 : ALIGN(4)
    {
        KEEP(*(.text.rb3b_vectors))
        *(.text .text.*)
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > RB3_CODE

    .data :
    {
        *(.data .data.*)
    } > RB3_CODE
    ASSERT(SIZEOF(.data) == 0, "Dedicated Backup FW must have zero initialized .data")

    .bss (NOLOAD) :
    {
        *(.bss .bss.* COMMON)
    } > RB3_NC
    ASSERT(SIZEOF(.bss) == 0, "Dedicated Backup FW must place all buffers explicitly in .ncbuf")

    .ncbuf 0x209F3380 (NOLOAD) : ALIGN(32)
    {
        KEEP(*(.ncbuf))
    } > RB3_NC

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
