/*
 * rb3_backup_core.h - Dedicated One-Time Backup Firmware 9-Stage State Machine
 * Phase ARM-RB3 Final Packaging
 *
 * Single dedicated backup firmware image with 9 strictly ordered, one-shot user-gated stages:
 *   Stage 1: Native Execution Self-test (CPU/SHA-256 KAT/Stripe/Opcode guard/TX inhibit/J140 ID)
 *   Stage 2: SD Filesystem Self-test (create/write/close/reopen/readback verify RB3_SELFTEST.TXT)
 *            + one-shot trigger consumption (rewrites matched RB3_*.TRG -> "CONSUMED\r\n")
 *   Stage 3: IC704 Readback       -> IC704_ARM.bin      (16 MiB, SPIBSC0 XIP, non-disruptive)
 *   Stage 4: IC705 EEPROM         -> IC705_EEPROM.bin   (128 KiB, RIIC0 Chip 0, non-disruptive)
 *   Stage 5: IC706 EEPROM         -> IC706_EEPROM.bin   (128 KiB, RIIC0 Chip 1, non-disruptive)
 *   Stage 6: WLAN Flash           -> WLAN_FLASH.bin     (4 MiB, RSPI2, EXPLICIT disruptive gate!)
 *   Stage 7: IC553 DSP/FPGA Flash -> IC553_DSP_FPGA.bin (2 MiB, RSPI4, EXPLICIT disruptive gate, LAST!)
 *   Stage 8: Backup Integrity Verification (write & readback-verify SHA256SUMS.txt + RB3_session_manifest.json)
 *   Stage 9: Safe Completion:
 *            - Non-disruptive (1..5): SD sync + restore TX inhibit & [0x203802BC+6] -> clean stock UI return
 *            - Disruptive (6..7): checked SD sync/unmount + suppress EEPROM commit -> gated restart in HAL
 */
#ifndef RB3_BACKUP_CORE_H
#define RB3_BACKUP_CORE_H

#include <stdint.h>
#include "rb3_backup_sha256.h"

/* Storage capacities (bytes) */
#define RB3B_IC704_SIZE      (16u * 1024u * 1024u)
#define RB3B_IC705_SIZE      (128u * 1024u)
#define RB3B_IC706_SIZE      (128u * 1024u)
#define RB3B_WLAN_SIZE       (4u * 1024u * 1024u)
#define RB3B_IC553_SIZE      (2u * 1024u * 1024u)

#define RB3B_STREAM_CHUNK    4096u
#define RB3B_EEPROM_CHUNK    64u

/* Strictly whitelisted READ-ONLY SPI opcodes */
#define RB3B_SPI_OP_READ     0x03u
#define RB3B_SPI_OP_RDID     0x9Fu

/* Stage selection bitmask (determined ONLY by explicit armed user SD trigger files) */
#define RB3B_MASK_SELFTEST   (1u << 0) /* Stages 1 + 2 (always run first) */
#define RB3B_MASK_IC704      (1u << 1) /* Stage 3 */
#define RB3B_MASK_IC705      (1u << 2) /* Stage 4 */
#define RB3B_MASK_IC706      (1u << 3) /* Stage 5 */
#define RB3B_MASK_WLAN       (1u << 4) /* Stage 6 (DISRUPTIVE) */
#define RB3B_MASK_IC553      (1u << 5) /* Stage 7 (DISRUPTIVE, ALWAYS LAST) */

#define RB3B_MASK_SAFE_ALL   (RB3B_MASK_SELFTEST | RB3B_MASK_IC704 | RB3B_MASK_IC705 | RB3B_MASK_IC706)
#define RB3B_MASK_FULL_ALL   (RB3B_MASK_SAFE_ALL | RB3B_MASK_WLAN | RB3B_MASK_IC553)

typedef enum {
    RB3B_OK                   =  0,
    RB3B_NO_TRIGGER           =  1,
    RB3B_ERR_SELFTEST_NATIVE  = -1,
    RB3B_ERR_SELFTEST_SD      = -2,
    RB3B_ERR_TX_INHIBIT       = -3,
    RB3B_ERR_SD_OPEN          = -4,
    RB3B_ERR_SD_WRITE         = -5,
    RB3B_ERR_SD_READ          = -6,
    RB3B_ERR_SD_CLOSE         = -7,
    RB3B_ERR_BUS_ACQUIRE      = -8,
    RB3B_ERR_JEDEC            = -9,
    RB3B_ERR_HW_READ          = -10,
    RB3B_ERR_EEPROM_BUSY      = -11,
    RB3B_ERR_DOUBLE_MISMATCH  = -12,
    RB3B_ERR_VERIFY_READBACK  = -13,
    RB3B_ERR_FORBIDDEN_OPCODE = -14,
    RB5_ERR_SD_SYNC          = -15,
    RB5_ERR_SD_UNMOUNT       = -16,
    RB5_ERR_COMPLETION_LOG   = -17
} rb3b_status_t;

typedef struct {
    uint8_t       executed;
    uint8_t       passed;
    uint8_t       jedec[3];
    uint8_t       jedec_ok;
    rb3b_status_t status;
    uint32_t      bytes_written;
    char          sha256_pass1[RB3B_SHA256_HEX];
    char          sha256_pass2[RB3B_SHA256_HEX];
} rb3b_target_log_t;

typedef struct {
    uint32_t          progress_seq;
    uint32_t          progress_event;
    uint32_t          progress_offset;
    int32_t           progress_result;
    int32_t           progress_log_status;
    uint32_t          requested_mask;
    rb3b_status_t     overall_status;
    rb3b_status_t     backup_status;
    int32_t           completion_status; /* 1 pending, 0 complete, negative error */
    int32_t           sd_sync_status;
    int32_t           sd_unmount_status; /* 1 pending/not requested */
    int32_t           completion_log_status;
    uint8_t           completion_error_report_synced;
    uint8_t           stage1_native_ok;
    uint8_t           stage2_sd_ok;
    uint8_t           trigger_consumed;
    uint8_t           tx_inhibit_ok;
    uint8_t           disruptive_entered;
    uint8_t           stage8_verify_ok;
    uint8_t           sd_synced;
    uint8_t           sd_unmounted;
    uint8_t           eeprom_shutdown_suppressed;
    uint8_t           runtime_state_restored;
    rb3b_target_log_t ic704;
    rb3b_target_log_t ic705;
    rb3b_target_log_t ic706;
    rb3b_target_log_t wlan;
    rb3b_target_log_t ic553;
} rb3b_session_state_t;

/*
 * Strictly READ-ONLY target HAL vtable (NO storage write/erase APIs exist!).
 */
typedef struct {
    void     *ctx;
    uint8_t  *nc_stream_buf; /* 4096 B 32-byte aligned Non-Cacheable SRAM */
    char     *nc_text_buf;   /* 2048 B 32-byte aligned Non-Cacheable SRAM */
    uint32_t  nc_text_cap;

    int  (*check_j140_identity)(void *ctx);
    int  (*assert_tx_inhibit)(void *ctx);
    void (*restore_tx_inhibit)(void *ctx);
    void (*cache_clean_range)(void *ctx, const void *buf, uint32_t len);
    void (*delay_us)(void *ctx, uint32_t usec);

    /* SD / GRP_FS client hooks (strictly <= 1 open handle at any time) */
    int  (*sd_open_write)(void *ctx, const char *path, int32_t *out_fd);
    int  (*sd_open_read)(void *ctx, const char *path, int32_t *out_fd);
    int  (*sd_write)(void *ctx, int32_t fd, const uint8_t *buf, uint32_t len, uint32_t *out_written);
    int  (*sd_read)(void *ctx, int32_t fd, uint8_t *buf, uint32_t len, uint32_t *out_read);
    int  (*sd_close)(void *ctx, int32_t fd);
    int  (*sd_sync_all)(void *ctx);
    int  (*progress_sync)(void *ctx);
    int  (*sd_unmount)(void *ctx);
    void (*set_eeprom_write_suppress)(void *ctx, uint8_t enable);

    /* Strictly read-only target storage hooks */
    int  (*ic704_xip_read)(void *ctx, uint32_t off, uint8_t *dst, uint32_t len);
    int  (*eeprom_wait_idle)(void *ctx);
    int  (*eeprom_phys_read)(void *ctx, uint8_t chip, uint32_t off, uint8_t *dst, uint32_t len);
    int  (*wlan_bus_acquire)(void *ctx);
    int  (*wlan_spi_tx_rx)(void *ctx, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len);
    int  (*ic553_bus_acquire)(void *ctx);
    int  (*ic553_spi_tx_rx)(void *ctx, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len);
} rb3b_hal_t;

/* Event numbers are stable diagnostic identifiers, not completion proofs. */
#define RB5_EVT_STAGE1_BEGIN 10u
#define RB5_EVT_STAGE1_END 11u
#define RB5_EVT_STAGE2_BEGIN 20u
#define RB5_EVT_STAGE2_END 21u
#define RB5_EVT_TRIGGER_BEGIN 22u
#define RB5_EVT_TRIGGER_END 23u
#define RB5_EVT_SUMS_BEGIN 80u
#define RB5_EVT_SUMS_END 81u
#define RB5_EVT_MANIFEST_BEGIN 82u
#define RB5_EVT_MANIFEST_END 83u
#define RB5_EVT_RECEIPT_BEGIN 84u
#define RB5_EVT_RECEIPT_END 85u
#define RB5_EVT_SYNC_BEGIN 90u
#define RB5_EVT_SYNC_END 91u
#define RB5_EVT_UNMOUNT_BEGIN 92u
#define RB5_EVT_UNMOUNT_END 93u
#define RB5_EVT_RETURN 94u
#define RB5_EVT_RESTART_GATE 95u
#define RB5_EVT_ERROR_HOLD 96u
#define RB5_EVT_REPORT_SYNC_BEGIN 97u
#define RB5_EVT_REPORT_SYNC_END 98u
#define RB5_EVT_TARGET_BASE 100u /* +kind*10: 0 acquire,1 pass1,2 pass1 end,3 pass2,4 pass2 end,5 result */
int rb5_progress_export(const rb3b_hal_t *hal, const char *prefix,
                        const rb3b_session_state_t *s, uint8_t post_restart);
void rb5_progress(const rb3b_hal_t *hal, const char *prefix,
                  rb3b_session_state_t *s, uint32_t event, uint32_t offset, int32_t result);

int rb4_restart_allowed(const rb3b_session_state_t *s);
rb3b_status_t rb4_write_completion_report(const rb3b_hal_t *hal,
                                         const char *dir_prefix,
                                         const rb3b_session_state_t *s,
                                         uint8_t after_restart);

int rb3b_spi_opcode_is_readonly(uint8_t op);
uint32_t rb3b_eeprom_stripe_map(uint32_t logical_addr);

/* Probes armed one-shot SD trigger files and runs authorized stages (1..9) */
rb3b_status_t rb3b_run_from_sd_triggers(const rb3b_hal_t *hal,
                                        const char *dir_prefix,
                                        rb3b_session_state_t *out_state);

#endif /* RB3_BACKUP_CORE_H */
