/*
 * rb3_backup_symbols.ld - Verified stock J140 ARM ROM symbols for Dedicated Backup Firmware
 * Base image: extracted/J140/main.application.bin
 * SHA-256:    d25971deae61ab0db52ed80338516119367887728da6e67a29d05710aceac4a9
 *
 * STRICT READ-ONLY SYMBOL WHITELIST (16 symbols; ZERO write/erase/unlock/EEPROM-write symbols):
 */

/* GRP_FS / T23 filesystem client APIs (all verified ARM mode, AAPCS32) */
PROVIDE(j140_grp_fs_open       = 0x201F2864);
PROVIDE(j140_grp_fs_close      = 0x201F28BC);
PROVIDE(j140_grp_fs_read       = 0x201F2914);
PROVIDE(j140_grp_fs_write      = 0x201F296C);
PROVIDE(j140_grp_fs_sync_all   = 0x201E9170);
PROVIDE(j140_grp_fs_unmount    = 0x201F2ACC);
PROVIDE(j140_grp_fs_wait_raw   = 0x201EF558);
PROVIDE(j140_t28_sync_wait     = 0x200642C4);

/* U-21 PL310 L2 cache maintenance (single 32-byte line clean by PA + sync) */
PROVIDE(j140_l2c_clean_pa      = 0x2012547C);

/* Busy microsecond timer delay; not an RTOS yield. */
PROVIDE(j140_delay_us       = 0x20005B6C);

/* Strictly READ-ONLY storage drivers (ZERO write/erase APIs!) */
PROVIDE(j140_riic0_eeprom_read = 0x2005BCB4); /* RIIC0 synchronous physical read (<=64 B) */
PROVIDE(j140_wlan_flash_reset  = 0x2006D908); /* stock: Reset Enable 0x66 + Reset 0x99 (no memory change) */
PROVIDE(j140_wlan_bus_acquire  = 0x2006DA84); /* P2_4 WI_NRESET=0 + RSPI2 init 16 MHz */
PROVIDE(j140_rspi2_tx_rx       = 0x2006A61C); /* RSPI2 PIO TX-then-RX (RDID/READ only) */
PROVIDE(j140_ic553_bus_acquire = 0x2006A2B0); /* FPGA/SHARC/C5517 reset + RSPI4 init 32 MHz */
PROVIDE(j140_rspi4_tx_rx       = 0x20069FC8); /* RSPI4 PIO TX-then-RX (RDID/READ only) */

/* T28 Firmware Update scan chain target (100% preserved stock behavior) */
PROVIDE(j140_t28_fw_scan_0x3d  = 0x2006C69C);

/* j140_os_delay is an explicit ARM-to-Thumb wrapper in rb3_backup_entry.S. */
