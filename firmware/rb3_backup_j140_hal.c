/*
 * rb3_backup_j140_hal.c - Real J140 Cortex-A9 Hardware Bindings (ZERO mocks!)
 * Links directly against 16 verified read-only stock J140 symbols in rb3_backup_symbols.ld.
 *
 * Strictly READ-ONLY on all 5 non-volatile storage targets:
 *   - IC704: SPIBSC0 XIP read-only (0x18000000..0x18FFFFFF)
 *   - IC705: RIIC0 synchronous physical read (0x2005BCB4, mapped 0x00000..0x1FFFF)
 *   - IC706: RIIC0 synchronous physical read (0x2005BCB4, mapped 0x20000..0x3FFFF)
 *   - WLAN:  PSR2 WI_PWS=1 + 0x2006DA84 + RSPI2 0x2006A61C (ONLY 0x9F/0x03)
 *   - IC553: 0x2006A2B0 + RSPI4 0x20069FC8 (ONLY 0x9F/0x03)
 */
#include "rb3_backup_core.h"

/* Stock J140 ARM symbols provided by rb3_backup_symbols.ld */
extern int32_t j140_grp_fs_open(const char *path, uint32_t flags, int32_t *out_fd, void *stat_buf);
extern int32_t j140_grp_fs_close(int32_t fd, uint32_t arg1);
extern int32_t j140_grp_fs_read(int32_t fd, void *buf, uint32_t len, uint32_t *out_read);
extern int32_t j140_grp_fs_write(int32_t fd, const void *buf, uint32_t len, uint32_t *out_written);
extern int32_t j140_grp_fs_sync_all(void);
extern int32_t j140_grp_fs_unmount(const char *dev_name, uint32_t flags);
extern int32_t j140_grp_fs_wait_raw(int32_t req_id, uint32_t mode);
extern int32_t j140_t28_sync_wait(int32_t req_id, uint32_t kind_char, uint32_t drive_slot);
/* Stock 0x20005B6C uses microseconds, not RTX ticks. */
extern void    j140_delay_us(uint32_t usec);
extern int32_t j140_os_delay(uint32_t milliseconds);

extern void    j140_riic0_eeprom_read(uint32_t mapped_addr, uint8_t *dst, uint32_t len);
extern void    j140_wlan_bus_acquire(void);
extern void    j140_wlan_flash_reset(void);
extern void    j140_rspi2_tx_rx(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len);
extern void    j140_ic553_bus_acquire(void);
extern void    j140_rspi4_tx_rx(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len);

extern void    rb3b_cache_clean_range(const void *addr, uint32_t len);

/* Verified J140 MMIO & RAM addresses (CONFIRMED_STATIC) */
#define J140_APP_ID_VA           0x20005040u /* "Q70RBTXTFUZ0LTF2" */
#define J140_XIP_IC704_BASE      0x18000000u
#define J140_TX_INHIBIT_F5_VA    0x203806F5u /* |= 0x48 checked by 0x2002EBCC */
#define J140_TX_INHIBIT_F6_VA    0x203806F6u /* |= 0x01 checked by 0x2002EBCC */
#define J140_T28_SUPPRESS_EE_VA  0x203802C2u /* [0x203802BC + 6], stock fw_update_in_progress */
#define J140_RIIC0_STATE_VA      0x20399F24u /* 0 = idle */
#define J140_EEPROM_MBOX_VA      0x20399F78u /* 0 = idle */
#define J140_PSR2_VA             0xFCFE3108u /* 0xFCFE3000 + 0x108 (WI_PWS = P2_0) */

#define J140_FS_OPEN_RDONLY      0x0000u
#define J140_FS_OPEN_CREAT_EXCL  0x8601u
#define J140_FS_OPEN_WR_TRUNC    0x8401u
#define J140_FS_KIND_FILE        0x46u
#define J140_FS_DRIVE_C          0u
#define J140_FS_ERR_EXISTS       (-8)

/* 64-bit same-boot one-shot latch magic (safe inside NOLOAD SRAM) */
#define RB3B_LATCH_MAGIC0        0x5242334Cu /* "RB3L" */
#define RB3B_LATCH_MAGIC1        0x444F4E45u /* "DONE" */

/* Explicit Non-Cacheable SRAM arena at 0x209F3380 (NOLOAD, 32-byte aligned) */
typedef struct {
    uint8_t              stream_buf[RB3B_STREAM_CHUNK]; /* 4096 B */
    char                 text_buf[2048];                /* 2048 B */
    rb3b_session_state_t session;                       /* ~800 B */
    uint32_t             latch_magic0;
    uint32_t             latch_magic1;
    uint8_t              saved_tx_f5;
    uint8_t              saved_tx_f6;
    uint8_t              saved_ee_sup;
    uint8_t              saved_valid;
    uint32_t             receipt_magic;
    uint8_t              receipt_hash[32];
} rb3b_nc_arena_t;

__attribute__((section(".ncbuf"), aligned(32), used))
static rb3b_nc_arena_t g_rb3b_nc;

static const char g_j140_expected_id[16] = "Q70RBTXTFUZ0LTF2";
static const char g_sd_dev_name[] = "sd2";
static const char g_sd_dir_prefix[] = "C:\\IC-705\\";

static int hal_check_j140_identity(void *ctx) {
    const volatile uint8_t *p = (const volatile uint8_t *)J140_APP_ID_VA;
    uint32_t i;
    (void)ctx;
    for (i = 0u; i < 16u; ++i) {
        if (p[i] != (uint8_t)g_j140_expected_id[i]) {
            return -1;
        }
    }
    return 0;
}

static int hal_assert_tx_inhibit(void *ctx) {
    volatile uint8_t *f5 = (volatile uint8_t *)J140_TX_INHIBIT_F5_VA;
    volatile uint8_t *f6 = (volatile uint8_t *)J140_TX_INHIBIT_F6_VA;
    volatile uint8_t *ee = (volatile uint8_t *)J140_T28_SUPPRESS_EE_VA;
    (void)ctx;
    g_rb3b_nc.saved_tx_f5  = *f5;
    g_rb3b_nc.saved_tx_f6  = *f6;
    g_rb3b_nc.saved_ee_sup = *ee;
    g_rb3b_nc.saved_valid  = 1u;
    *f5 = (uint8_t)(*f5 | 0x48u);
    *f6 = (uint8_t)(*f6 | 0x01u);
    return (((*f5 & 0x48u) == 0x48u) && ((*f6 & 0x01u) == 0x01u)) ? 0 : -1;
}

static void hal_restore_tx_inhibit(void *ctx) {
    volatile uint8_t *f5 = (volatile uint8_t *)J140_TX_INHIBIT_F5_VA;
    volatile uint8_t *f6 = (volatile uint8_t *)J140_TX_INHIBIT_F6_VA;
    (void)ctx;
    if (g_rb3b_nc.saved_valid) {
        *f5 = g_rb3b_nc.saved_tx_f5;
        *f6 = g_rb3b_nc.saved_tx_f6;
    }
}

static void hal_cache_clean(void *ctx, const void *buf, uint32_t len) {
    (void)ctx;
    rb3b_cache_clean_range(buf, len);
}

static void hal_delay_us(void *ctx, uint32_t usec) {
    (void)ctx;
    j140_delay_us(usec);
}

static int hal_sd_open_write(void *ctx, const char *path, int32_t *out_fd) {
    uint32_t stat_buf[16];
    int32_t req, rc;
    uint32_t i;
    (void)ctx;
    for (i = 0u; i < 16u; ++i) stat_buf[i] = 0u;
    req = j140_grp_fs_open(path, J140_FS_OPEN_CREAT_EXCL, out_fd, stat_buf);
    rc = j140_t28_sync_wait(req, J140_FS_KIND_FILE, J140_FS_DRIVE_C);
    if (rc == J140_FS_ERR_EXISTS) {
        for (i = 0u; i < 16u; ++i) stat_buf[i] = 0u;
        req = j140_grp_fs_open(path, J140_FS_OPEN_WR_TRUNC, out_fd, stat_buf);
        rc = j140_t28_sync_wait(req, J140_FS_KIND_FILE, J140_FS_DRIVE_C);
    }
    return (rc == 0) ? 0 : -1;
}

static int hal_sd_open_read(void *ctx, const char *path, int32_t *out_fd) {
    uint32_t stat_buf[16];
    int32_t req, rc;
    uint32_t i;
    (void)ctx;
    for (i = 0u; i < 16u; ++i) stat_buf[i] = 0u;
    req = j140_grp_fs_open(path, J140_FS_OPEN_RDONLY, out_fd, stat_buf);
    rc = j140_t28_sync_wait(req, J140_FS_KIND_FILE, J140_FS_DRIVE_C);
    return (rc == 0) ? 0 : -1;
}

static int hal_sd_write(void *ctx, int32_t fd, const uint8_t *buf, uint32_t len, uint32_t *out_w) {
    int32_t req, rc;
    (void)ctx;
    *out_w = 0u;
    req = j140_grp_fs_write(fd, buf, len, out_w);
    rc = j140_t28_sync_wait(req, J140_FS_KIND_FILE, J140_FS_DRIVE_C);
    return (rc == 0) ? 0 : -1;
}

static int hal_sd_read(void *ctx, int32_t fd, uint8_t *buf, uint32_t len, uint32_t *out_r) {
    int32_t req, rc;
    (void)ctx;
    *out_r = 0u;
    req = j140_grp_fs_read(fd, buf, len, out_r);
    rc = j140_t28_sync_wait(req, J140_FS_KIND_FILE, J140_FS_DRIVE_C);
    return (rc == 0) ? 0 : -1;
}

static int hal_sd_close(void *ctx, int32_t fd) {
    int32_t req, rc;
    (void)ctx;
    req = j140_grp_fs_close(fd, 0u);
    rc = j140_t28_sync_wait(req, J140_FS_KIND_FILE, J140_FS_DRIVE_C);
    return (rc == 0) ? 0 : -1;
}

static int hal_sd_sync_all(void *ctx) {
    (void)ctx;
    return j140_grp_fs_sync_all();
}

static int hal_sd_unmount(void *ctx) {
    int32_t req, rc;
    uint32_t polls;
    (void)ctx;
    req = j140_grp_fs_unmount(g_sd_dev_name, 0u);
    if (req > 0) {
        for (polls = 0u; polls < 500u; ++polls) {
            /* mode=0 is the stock bounded signal-wait variant; mode=1 is infinite. */
            rc = j140_grp_fs_wait_raw(req, 0u);
            if (rc != 1) return rc;
            (void)j140_os_delay(10u);
        }
        return -1001; /* bounded polling budget exhausted; NEVER restart */
    }
    return req == 0 ? -1002 : req; /* unexpected zero request id */
}

#define RB5_RECEIPT_MAGIC 0x42354350u
static void rb4_session_hash(uint8_t digest[32]) {
    rb3b_sha256_ctx_t ctx;
    rb3b_sha256_init(&ctx);
    rb3b_sha256_update(&ctx,(const uint8_t *)&g_rb3b_nc.session,sizeof(g_rb3b_nc.session));
    rb3b_sha256_final(&ctx,digest);
}

/* RZ/A1H WDT restart sequence: Renesas BSP / Linux rza_wdt.c.
 * Counter starts at zero (not prohibited 0xff). Only called after the portable
 * completion gate proves successful sync+unmount and one-shot consumption.
 * No FLASH/EEPROM writes and no change to the boot-slot selector. */
static void rb4_restart_after_unmount(void) __attribute__((noreturn));
static void rb4_restart_after_unmount(void) {
    volatile uint16_t *csr = (volatile uint16_t *)0xFCFE0000u;
    volatile uint16_t *cnt = (volatile uint16_t *)0xFCFE0002u;
    volatile uint16_t *rcsr = (volatile uint16_t *)0xFCFE0004u;
    volatile uint8_t *rcsr_read = (volatile uint8_t *)0xFCFE0004u;
    uint8_t dummy;
    __asm__ volatile("cpsid if\n\tdsb sy" ::: "memory");
    *csr = 0xA500u;
    dummy = *rcsr_read;
    (void)dummy;
    *rcsr = 0xA500u;
    *rcsr = 0x5A5Fu;
    *cnt = 0x5A00u;
    *csr = 0xA578u;
    __asm__ volatile("dsb sy" ::: "memory");
    for (;;) __asm__ volatile("wfi");
}

static void hal_set_eeprom_write_suppress(void *ctx, uint8_t enable) {
    volatile uint8_t *flag = (volatile uint8_t *)J140_T28_SUPPRESS_EE_VA;
    (void)ctx;
    if (enable) {
        *flag = 1u;
    } else if (g_rb3b_nc.saved_valid) {
        *flag = g_rb3b_nc.saved_ee_sup;
    } else {
        *flag = 0u;
    }
}

static int hal_ic704_xip_read(void *ctx, uint32_t off, uint8_t *dst, uint32_t len) {
    const volatile uint8_t *src = (const volatile uint8_t *)(J140_XIP_IC704_BASE + off);
    uint32_t i;
    (void)ctx;
    if (off + len > RB3B_IC704_SIZE) return -1;
    for (i = 0u; i < len; ++i) {
        dst[i] = src[i];
    }
    return 0;
}

static int hal_eeprom_wait_idle(void *ctx) {
    const volatile uint8_t *riic0 = (const volatile uint8_t *)J140_RIIC0_STATE_VA;
    const volatile uint8_t *mbox  = (const volatile uint8_t *)J140_EEPROM_MBOX_VA;
    uint32_t tries;
    (void)ctx;
    for (tries = 0u; tries < 500u; ++tries) {
        if (*riic0 == 0u && *mbox == 0u) {
            return 0;
        }
        j140_delay_us(1u);
    }
    return -1;
}

static int hal_eeprom_phys_read(void *ctx, uint8_t chip, uint32_t off, uint8_t *dst, uint32_t len) {
    uint32_t mapped;
    (void)ctx;
    if (chip > 1u || off + len > RB3B_IC705_SIZE || len > RB3B_EEPROM_CHUNK) {
        return -1;
    }
    mapped = (((uint32_t)chip) << 17) | off;
    j140_riic0_eeprom_read(mapped, dst, len);
    return 0;
}

static int hal_wlan_bus_acquire(void *ctx) {
    volatile uint32_t *psr2 = (volatile uint32_t *)J140_PSR2_VA;
    (void)ctx;
    /*
     * Mirror stock updater order (0x2006E268 .. 0x2006E394):
     *   1. 0x2006A2B0 : hold FPGA/SHARC/C5517 in reset + RSPI4 init (stock does this before WLAN stage)
     *   2. PSR2 WI_PWS=1 (P2_0), 0x2006DA84 : WI_NRESET=0 + RSPI2 init
     *   3. settle, then 0x2006D908 : WLAN flash Reset Enable (0x66) + Reset (0x99)
     */
    /* B3: explicit microsecond waits: 5 / 20 / 20 / 30 ms.
     * Timing-only experiment; WLAN task ownership is unchanged. */
    j140_ic553_bus_acquire();
    j140_delay_us(5000u);
    *psr2 = 0x00010001u;
    j140_delay_us(20000u);
    j140_wlan_bus_acquire();
    j140_delay_us(20000u);
    j140_wlan_flash_reset();
    j140_delay_us(30000u);
    return 0;
}

static int hal_wlan_spi_tx_rx(void *ctx, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len) {
    (void)ctx;
    j140_rspi2_tx_rx(tx, tx_len, rx, rx_len);
    return 0;
}

static int hal_ic553_bus_acquire(void *ctx) {
    (void)ctx;
    j140_ic553_bus_acquire();
    j140_delay_us(5u);
    return 0;
}

static int hal_ic553_spi_tx_rx(void *ctx, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len) {
    (void)ctx;
    j140_rspi4_tx_rx(tx, tx_len, rx, rx_len);
    return 0;
}

int32_t rb3b_j140_entry_from_t28(void) {
    rb3b_hal_t hal;
    rb3b_status_t st;

    /* Same-boot duplicate-execution guard: if already ran once this boot, pass straight to stock updater */
    if (g_rb3b_nc.latch_magic0 == RB3B_LATCH_MAGIC0 &&
        g_rb3b_nc.latch_magic1 == RB3B_LATCH_MAGIC1) {
        return 0;
    }

    g_rb3b_nc.saved_valid = 0u;

    hal.ctx                       = (void *)0;
    hal.nc_stream_buf             = g_rb3b_nc.stream_buf;
    hal.nc_text_buf               = g_rb3b_nc.text_buf;
    hal.nc_text_cap               = (uint32_t)sizeof(g_rb3b_nc.text_buf);
    hal.check_j140_identity       = hal_check_j140_identity;
    hal.assert_tx_inhibit         = hal_assert_tx_inhibit;
    hal.restore_tx_inhibit        = hal_restore_tx_inhibit;
    hal.cache_clean_range         = hal_cache_clean;
    hal.delay_us               = hal_delay_us;
    hal.sd_open_write             = hal_sd_open_write;
    hal.sd_open_read              = hal_sd_open_read;
    hal.sd_write                  = hal_sd_write;
    hal.sd_read                   = hal_sd_read;
    hal.sd_close                  = hal_sd_close;
    hal.sd_sync_all               = hal_sd_sync_all;
    hal.progress_sync             = hal_sd_sync_all;
    hal.sd_unmount                = hal_sd_unmount;
    hal.set_eeprom_write_suppress = hal_set_eeprom_write_suppress;
    hal.ic704_xip_read            = hal_ic704_xip_read;
    hal.eeprom_wait_idle          = hal_eeprom_wait_idle;
    hal.eeprom_phys_read          = hal_eeprom_phys_read;
    hal.wlan_bus_acquire          = hal_wlan_bus_acquire;
    hal.wlan_spi_tx_rx            = hal_wlan_spi_tx_rx;
    hal.ic553_bus_acquire         = hal_ic553_bus_acquire;
    hal.ic553_spi_tx_rx           = hal_ic553_spi_tx_rx;

    /* NOLOAD tail is outside the stock scatterload clears. Validate the whole
     * retained session before exporting a receipt; never trust uninitialized RAM.
     * If SRAM was lost on reset, the pre-unmount report remains PENDING. */
    if (g_rb3b_nc.receipt_magic == RB5_RECEIPT_MAGIC) {
        uint8_t digest[32];
        uint32_t i;
        uint8_t equal = 1u;
        rb4_session_hash(digest);
        for (i = 0u; i < sizeof(digest); ++i) {
            if (digest[i] != g_rb3b_nc.receipt_hash[i]) equal = 0u;
        }
        if (equal && rb4_restart_allowed(&g_rb3b_nc.session)) {
            st = rb4_write_completion_report(&hal,g_sd_dir_prefix,&g_rb3b_nc.session,1u);
            if (st != RB3B_OK || hal_sd_sync_all(0) != 0 ||
                rb5_progress_export(&hal,g_sd_dir_prefix,&g_rb3b_nc.session,1u) != 0) {
                /* Leave the validated receipt available for another menu entry.
                 * Do not consume a new trigger if the previous receipt failed. */
                return RB5_ERR_COMPLETION_LOG;
            }
        }
        g_rb3b_nc.receipt_magic = 0u;
    }

    st = rb3b_run_from_sd_triggers(&hal, g_sd_dir_prefix, &g_rb3b_nc.session);

    if (st != RB3B_NO_TRIGGER) {
        /* Latch one-shot execution for remainder of this boot session */
        g_rb3b_nc.latch_magic0 = RB3B_LATCH_MAGIC0;
        g_rb3b_nc.latch_magic1 = RB3B_LATCH_MAGIC1;
    }

    if (g_rb3b_nc.session.disruptive_entered) {
        if (rb4_restart_allowed(&g_rb3b_nc.session)) {
            rb5_progress(&hal,g_sd_dir_prefix,&g_rb3b_nc.session,RB5_EVT_RESTART_GATE,0u,0);
            g_rb3b_nc.receipt_magic = 0u;
            rb4_session_hash(g_rb3b_nc.receipt_hash);
            g_rb3b_nc.latch_magic0 = 0u;
            g_rb3b_nc.latch_magic1 = 0u;
            __asm__ volatile("dsb sy" ::: "memory");
            g_rb3b_nc.receipt_magic = RB5_RECEIPT_MAGIC;
            __asm__ volatile("dsb sy" ::: "memory");
            rb4_restart_after_unmount();
        }
        /* Completion error: never reset with a mounted/unverified card. Let
         * lower-priority tasks run; the error report is best-effort on the SD.
         * This error state does not claim the reset peripherals are restored. */
        rb5_progress(&hal,g_sd_dir_prefix,&g_rb3b_nc.session,RB5_EVT_ERROR_HOLD,0u,st);
        for (;;) {
            (void)j140_os_delay(100u);
        }
    }

    return (st == RB3B_OK || st == RB3B_NO_TRIGGER) ? 0 : (int32_t)st;
}
