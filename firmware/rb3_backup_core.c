/*
 * rb3_backup_core.c - Dedicated One-Time Backup Firmware 9-Stage State Machine
 * Pure freestanding C99 (zero libc, zero heap, zero VFP/NEON, zero storage writes).
 *
 * Key Final-Audit Guarantees:
 *   - One-shot trigger consumption: armed RB3_*.TRG files are rewritten to "CONSUMED\r\n"
 *     immediately after Stage 2 SD self-test (before any storage read) and ignored on
 *     all subsequent boots/scans.
 *   - EEPROM readback protection: sets [0x203802BC+6]=1 during Stages 4..5 to suppress
 *     0x200D5120 periodic background EEPROM dirty flushes (0x2004E820/0x200605C0), and
 *     restores [0x203802BC+6] + TX inhibit flags on non-disruptive exit.
 *   - Disruptive exit (Stages 6..7): syncs SD, unmounts "sd2", keeps [0x203802BC+6]=1
 *     (suppresses 0x2005EA44 shutdown EEPROM commit) and TX inhibit active.
 */
#include "rb3_backup_core.h"

static uint32_t rb3b_strlen(const char *s) {
    uint32_t n = 0u;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

static int rb3b_memeq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void rb3b_zero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0u; i < n; ++i) {
        b[i] = 0u;
    }
}

static void rb3b_build_path(char *out, uint32_t cap, const char *dir, const char *leaf) {
    uint32_t pos = 0u;
    uint32_t i = 0u;
    while (dir[i] != '\0' && pos + 1u < cap) {
        out[pos++] = dir[i++];
    }
    i = 0u;
    while (leaf[i] != '\0' && pos + 1u < cap) {
        out[pos++] = leaf[i++];
    }
    out[pos] = '\0';
}

static uint32_t rb3b_append_str(char *dst, uint32_t pos, uint32_t cap, const char *s) {
    uint32_t i = 0u;
    while (s[i] != '\0' && pos + 1u < cap) {
        dst[pos++] = s[i++];
    }
    dst[pos] = '\0';
    return pos;
}

static uint32_t rb3b_append_u32(char *dst, uint32_t pos, uint32_t cap, uint32_t v) {
    char tmp[12];
    uint32_t n = 0u;
    if (v == 0u) {
        return rb3b_append_str(dst, pos, cap, "0");
    }
    while (v > 0u && n < 11u) {
        uint32_t q = v / 10u;
        uint32_t r = v - q * 10u;
        tmp[n++] = (char)('0' + r);
        v = q;
    }
    while (n > 0u && pos + 1u < cap) {
        dst[pos++] = tmp[--n];
    }
    dst[pos] = '\0';
    return pos;
}

static uint32_t rb3b_append_i32(char *dst, uint32_t pos, uint32_t cap, int32_t v) {
    if (v < 0) {
        pos = rb3b_append_str(dst, pos, cap, "-");
        return rb3b_append_u32(dst, pos, cap, (uint32_t)(-(v + 1)) + 1u);
    }
    return rb3b_append_u32(dst, pos, cap, (uint32_t)v);
}

static uint32_t rb3b_append_hex8(char *dst, uint32_t pos, uint32_t cap, uint8_t b) {
    static const char HEX[] = "0123456789ABCDEF";
    if (pos + 2u < cap) {
        dst[pos++] = HEX[(b >> 4) & 0x0fu];
        dst[pos++] = HEX[b & 0x0fu];
        dst[pos] = '\0';
    }
    return pos;
}

int rb3b_spi_opcode_is_readonly(uint8_t op) {
    return (op == RB3B_SPI_OP_READ || op == RB3B_SPI_OP_RDID) ? 1 : 0;
}

uint32_t rb3b_eeprom_stripe_map(uint32_t logical_addr) {
    /* Verified J140 0x2005C41C exact 64-byte stripe bijection */
    uint32_t chip = (logical_addr >> 6) & 1u;
    uint32_t phys = ((logical_addr >> 7) << 6) | (logical_addr & 0x3fu);
    return (chip << 17) | phys;
}

static rb3b_status_t rb3b_guarded_spi(
    int (*fn)(void *, const uint8_t *, uint32_t, uint8_t *, uint32_t),
    void *ctx, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
    if (tx_len == 0u || !rb3b_spi_opcode_is_readonly(tx[0])) {
        return RB3B_ERR_FORBIDDEN_OPCODE;
    }
    return (fn(ctx, tx, tx_len, rx, rx_len) == 0) ? RB3B_OK : RB3B_ERR_HW_READ;
}

static rb3b_status_t rb3b_write_and_verify_file(const rb3b_hal_t *hal,
                                                const char *path,
                                                const uint8_t *data,
                                                uint32_t len)
{
    int32_t fd = -1;
    uint32_t written = 0u;
    uint32_t nread = 0u;
    uint32_t i;
    rb3b_status_t st = RB3B_OK;

    if (len > RB3B_STREAM_CHUNK) {
        return RB3B_ERR_SD_WRITE;
    }
    for (i = 0u; i < len; ++i) {
        hal->nc_stream_buf[i] = data[i];
    }
    hal->cache_clean_range(hal->ctx, hal->nc_stream_buf, len);

    if (hal->sd_open_write(hal->ctx, path, &fd) != 0 || fd < 0) {
        return RB3B_ERR_SD_OPEN;
    }
    if (hal->sd_write(hal->ctx, fd, hal->nc_stream_buf, len, &written) != 0 || written != len) {
        st = RB3B_ERR_SD_WRITE;
    }
    if (hal->sd_close(hal->ctx, fd) != 0 && st == RB3B_OK) {
        st = RB3B_ERR_SD_CLOSE;
    }
    if (st != RB3B_OK) {
        return st;
    }

    for (i = 0u; i < len; ++i) {
        hal->nc_stream_buf[i] = 0u;
    }
    hal->cache_clean_range(hal->ctx, hal->nc_stream_buf, len);

    fd = -1;
    if (hal->sd_open_read(hal->ctx, path, &fd) != 0 || fd < 0) {
        return RB3B_ERR_VERIFY_READBACK;
    }
    if (hal->sd_read(hal->ctx, fd, hal->nc_stream_buf, len, &nread) != 0 || nread != len) {
        st = RB3B_ERR_VERIFY_READBACK;
    }
    if (hal->sd_close(hal->ctx, fd) != 0 && st == RB3B_OK) {
        st = RB3B_ERR_SD_CLOSE;
    }
    if (st != RB3B_OK) {
        return st;
    }
    if (!rb3b_memeq(hal->nc_stream_buf, data, len)) {
        return RB3B_ERR_VERIFY_READBACK;
    }
    return RB3B_OK;
}

/* Alternating checkpoints, written only with no dump file open. The snapshot
 * describes a reached boundary, NOT proof that its own sync completed. A later
 * checkpoint establishes forward progress. No recursive filesystem tracing. */
int rb5_progress_export(const rb3b_hal_t *hal, const char *prefix,
                        const rb3b_session_state_t *s, uint8_t post_restart)
{
    char b[512], path[128];
    uint32_t p = 0u;
    int rc;
    b[0] = 0;
    p = rb3b_append_str(b,p,sizeof(b),"{\n  \"build_id\": \"RB3-DEDICATED-BACKUP-FINAL-J140-V5\",\n  \"seq\": ");
    p = rb3b_append_u32(b,p,sizeof(b),s->progress_seq);
    p = rb3b_append_str(b,p,sizeof(b),",\n  \"event\": ");
    p = rb3b_append_u32(b,p,sizeof(b),s->progress_event);
    p = rb3b_append_str(b,p,sizeof(b),",\n  \"offset\": ");
    p = rb3b_append_u32(b,p,sizeof(b),s->progress_offset);
    p = rb3b_append_str(b,p,sizeof(b),",\n  \"result\": ");
    p = rb3b_append_i32(b,p,sizeof(b),s->progress_result);
    p = rb3b_append_str(b,p,sizeof(b),",\n  \"prior_log_status\": ");
    p = rb3b_append_i32(b,p,sizeof(b),s->progress_log_status);
    p = rb3b_append_str(b,p,sizeof(b),",\n  \"requested_mask\": ");
    p = rb3b_append_u32(b,p,sizeof(b),s->requested_mask);
    p = rb3b_append_str(b,p,sizeof(b),",\n  \"post_restart\": ");
    p = rb3b_append_str(b,p,sizeof(b),post_restart ? "true" : "false");
    p = rb3b_append_str(b,p,sizeof(b),",\n  \"wlan_sha256\": \"");
    p = rb3b_append_str(b,p,sizeof(b),s->wlan.sha256_pass1);
    p = rb3b_append_str(b,p,sizeof(b),"\",\n  \"note\": \"Reached checkpoint; its own sync may still be pending\"\n}\n");
    rb3b_build_path(path,sizeof(path),prefix,post_restart ? "RB5_POST_RESTART.json" :
                    ((s->progress_seq & 1u) ? "RB5_PROGRESS_A.json" : "RB5_PROGRESS_B.json"));
    rc = rb3b_write_and_verify_file(hal,path,(const uint8_t *)b,p);
    if (rc == 0) rc = hal->progress_sync(hal->ctx);
    return rc;
}

void rb5_progress(const rb3b_hal_t *hal, const char *prefix,
                  rb3b_session_state_t *s, uint32_t event, uint32_t offset, int32_t result)
{
    int rc;
    s->progress_seq++;
    s->progress_event = event;
    s->progress_offset = offset;
    s->progress_result = result;
    if (s->sd_unmounted) return; /* SRAM only after successful unmount */
    rc = rb5_progress_export(hal,prefix,s,0u);
    if (rc != 0 && s->progress_log_status == 0) s->progress_log_status = rc;
}

/* ==========================================================================
 * One-Shot Trigger Table & Armed/Consumed Helpers (Zero Boot Auto-Run!)
 * ========================================================================== */
typedef struct {
    const char *filename;
    uint32_t    mask;
} rb3b_trigger_entry_t;

static const rb3b_trigger_entry_t RB3B_TRIGGER_TABLE[8] = {
    { "RB3_FULL_BACKUP.TRG", RB3B_MASK_FULL_ALL },
    { "RB3_SAFE_ALL.TRG",    RB3B_MASK_SAFE_ALL },
    { "RB3_STG12.TRG",       RB3B_MASK_SELFTEST },
    { "RB3_STG3_IC704.TRG",  RB3B_MASK_SELFTEST | RB3B_MASK_IC704 },
    { "RB3_STG4_IC705.TRG",  RB3B_MASK_SELFTEST | RB3B_MASK_IC705 },
    { "RB3_STG5_IC706.TRG",  RB3B_MASK_SELFTEST | RB3B_MASK_IC706 },
    { "RB3_STG6_WLAN.TRG",   RB3B_MASK_SELFTEST | RB3B_MASK_WLAN  },
    { "RB3_STG7_IC553.TRG",  RB3B_MASK_SELFTEST | RB3B_MASK_IC553 }
};

static const uint8_t RB3B_CONSUMED_MARKER[10] = {
    'C','O','N','S','U','M','E','D','\r','\n'
};

static int rb3b_trigger_is_armed(const rb3b_hal_t *hal, const char *path) {
    int32_t fd = -1;
    uint32_t nread = 0u;
    uint32_t i;

    if (hal->sd_open_read(hal->ctx, path, &fd) != 0 || fd < 0) {
        return 0;
    }
    for (i = 0u; i < 16u; ++i) {
        hal->nc_stream_buf[i] = 0u;
    }
    hal->cache_clean_range(hal->ctx, hal->nc_stream_buf, 32u);
    (void)hal->sd_read(hal->ctx, fd, hal->nc_stream_buf, 8u, &nread);
    (void)hal->sd_close(hal->ctx, fd);

    if (nread >= 8u && rb3b_memeq(hal->nc_stream_buf, RB3B_CONSUMED_MARKER, 8u)) {
        return 0; /* Already consumed on an earlier run */
    }
    return 1;
}

static uint32_t rb3b_probe_user_triggers(const rb3b_hal_t *hal,
                                         const char *dir_prefix,
                                         uint8_t *out_matched_bits)
{
    char path[128];
    uint32_t mask = 0u;
    uint8_t matched = 0u;
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        rb3b_build_path(path, sizeof(path), dir_prefix, RB3B_TRIGGER_TABLE[i].filename);
        if (rb3b_trigger_is_armed(hal, path)) {
            matched |= (uint8_t)(1u << i);
            mask |= RB3B_TRIGGER_TABLE[i].mask;
        }
    }
    *out_matched_bits = matched;
    return mask;
}

static rb3b_status_t rb3b_consume_matched_triggers(const rb3b_hal_t *hal,
                                                   const char *dir_prefix,
                                                   uint8_t matched_bits)
{
    char path[128];
    uint32_t i;
    rb3b_status_t rc = RB3B_OK;

    for (i = 0u; i < 8u; ++i) {
        if ((matched_bits & (uint8_t)(1u << i)) != 0u) {
            rb3b_build_path(path, sizeof(path), dir_prefix, RB3B_TRIGGER_TABLE[i].filename);
            if (rb3b_write_and_verify_file(hal, path, RB3B_CONSUMED_MARKER, 10u) != RB3B_OK) {
                rc = RB3B_ERR_SD_WRITE;
            }
        }
    }
    return rc;
}

/* ==========================================================================
 * Stage 1: Native Execution Self-test
 * ========================================================================== */
static rb3b_status_t rb3b_stage1_native_selftest(const rb3b_hal_t *hal,
                                                 rb3b_session_state_t *st)
{
    static const uint8_t KAT_ABC[3] = { 'a', 'b', 'c' };
    static const uint8_t KAT_EXP[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    rb3b_sha256_ctx_t ctx;
    uint8_t dig[32];

    if (hal->check_j140_identity(hal->ctx) != 0) {
        return RB3B_ERR_SELFTEST_NATIVE;
    }
    rb3b_sha256_init(&ctx);
    rb3b_sha256_update(&ctx, KAT_ABC, 3u);
    rb3b_sha256_final(&ctx, dig);
    if (!rb3b_memeq(dig, KAT_EXP, 32u)) {
        return RB3B_ERR_SELFTEST_NATIVE;
    }
    if (rb3b_eeprom_stripe_map(0x3ff30u) != 0x0001ffb0u ||
        rb3b_eeprom_stripe_map(0x3ff40u) != 0x0003ff80u) {
        return RB3B_ERR_SELFTEST_NATIVE;
    }
    if (!rb3b_spi_opcode_is_readonly(0x03u) || !rb3b_spi_opcode_is_readonly(0x9Fu) ||
        rb3b_spi_opcode_is_readonly(0x06u) || rb3b_spi_opcode_is_readonly(0x02u) ||
        rb3b_spi_opcode_is_readonly(0x20u) || rb3b_spi_opcode_is_readonly(0xD8u) ||
        rb3b_spi_opcode_is_readonly(0xC7u)) {
        return RB3B_ERR_SELFTEST_NATIVE;
    }
    hal->nc_stream_buf[0] = 0x5Au;
    hal->nc_stream_buf[RB3B_STREAM_CHUNK - 1u] = 0xA5u;
    hal->cache_clean_range(hal->ctx, hal->nc_stream_buf, 64u);
    if (hal->nc_stream_buf[0] != 0x5Au || hal->nc_stream_buf[RB3B_STREAM_CHUNK - 1u] != 0xA5u) {
        return RB3B_ERR_SELFTEST_NATIVE;
    }
    if (hal->assert_tx_inhibit(hal->ctx) != 0) {
        return RB3B_ERR_TX_INHIBIT;
    }
    st->tx_inhibit_ok = 1u;
    st->stage1_native_ok = 1u;
    return RB3B_OK;
}

/* ==========================================================================
 * Stage 2: SD Filesystem Self-test (create -> write -> close -> reopen -> verify)
 * ========================================================================== */
static rb3b_status_t rb3b_stage2_sd_selftest(const rb3b_hal_t *hal,
                                             const char *dir_prefix,
                                             rb3b_session_state_t *st)
{
    static const char PAYLOAD[] =
        "RB3_BACKUP_FW_SELFTEST_OK\r\n"
        "TARGET=IC-705-JP-J140\r\n"
        "BUILD_ID=RB3-DEDICATED-BACKUP-FINAL-V5\r\n"
        "MODE=READONLY-STORAGE-ZERO-WRITE-ONESHOT\r\n";
    char path[128];
    rb3b_status_t rc;

    rb3b_build_path(path, sizeof(path), dir_prefix, "RB3_SELFTEST.TXT");
    rc = rb3b_write_and_verify_file(hal, path, (const uint8_t *)PAYLOAD,
                                    (uint32_t)(sizeof(PAYLOAD) - 1u));
    if (rc != RB3B_OK) {
        return RB3B_ERR_SELFTEST_SD;
    }
    st->stage2_sd_ok = 1u;
    return RB3B_OK;
}

/* ==========================================================================
 * Generic Dual-Pass Readback Engine (Stages 3..7)
 * ========================================================================== */
typedef enum {
    KIND_IC704 = 0,
    KIND_IC705 = 1,
    KIND_IC706 = 2,
    KIND_WLAN  = 3,
    KIND_IC553 = 4
} rb3b_kind_t;

static rb3b_status_t rb3b_read_hw_chunk(const rb3b_hal_t *hal,
                                        rb3b_kind_t kind,
                                        uint32_t offset,
                                        uint8_t *dst,
                                        uint32_t len)
{
    uint8_t cmd[4];
    uint32_t p;

    switch (kind) {
        case KIND_IC704:
            return (hal->ic704_xip_read(hal->ctx, offset, dst, len) == 0)
                   ? RB3B_OK : RB3B_ERR_HW_READ;

        case KIND_IC705:
        case KIND_IC706: {
            uint8_t chip = (kind == KIND_IC705) ? 0u : 1u;
            for (p = 0u; p < len; p += RB3B_EEPROM_CHUNK) {
                uint32_t sub = len - p;
                if (sub > RB3B_EEPROM_CHUNK) {
                    sub = RB3B_EEPROM_CHUNK;
                }
                if (hal->eeprom_wait_idle(hal->ctx) != 0) {
                    return RB3B_ERR_EEPROM_BUSY;
                }
                if (hal->eeprom_phys_read(hal->ctx, chip, offset + p, dst + p, sub) != 0) {
                    return RB3B_ERR_HW_READ;
                }
            }
            return RB3B_OK;
        }

        case KIND_WLAN:
            cmd[0] = RB3B_SPI_OP_READ;
            cmd[1] = (uint8_t)((offset >> 16) & 0xffu);
            cmd[2] = (uint8_t)((offset >> 8)  & 0xffu);
            cmd[3] = (uint8_t)(offset & 0xffu);
            return rb3b_guarded_spi(hal->wlan_spi_tx_rx, hal->ctx, cmd, 4u, dst, len);

        case KIND_IC553:
            cmd[0] = RB3B_SPI_OP_READ;
            cmd[1] = (uint8_t)((offset >> 16) & 0xffu);
            cmd[2] = (uint8_t)((offset >> 8)  & 0xffu);
            cmd[3] = (uint8_t)(offset & 0xffu);
            return rb3b_guarded_spi(hal->ic553_spi_tx_rx, hal->ctx, cmd, 4u, dst, len);

        default:
            return RB3B_ERR_HW_READ;
    }
}

/* RDID (0x9F) with settle-delay retries; returns RB3B_OK once a plausible ID is read. */
static rb3b_status_t rb3b_probe_jedec(const rb3b_hal_t *hal,
                                      int (*fn)(void *, const uint8_t *, uint32_t, uint8_t *, uint32_t),
                                      uint8_t *jedec, int is_ic553)
{
    uint8_t cmd[1] = { RB3B_SPI_OP_RDID };
    uint32_t attempt;
    for (attempt = 0u; attempt < 10u; ++attempt) {
        rb3b_status_t rc = rb3b_guarded_spi(fn, hal->ctx, cmd, 1u, jedec, 3u);
        if (rc == RB3B_OK) {
            if (is_ic553) {
                if (jedec[0] == 0x9Du && jedec[1] == 0x60u && jedec[2] == 0x15u) {
                    return RB3B_OK;
                }
            } else if (!(jedec[0] == 0x00u && jedec[1] == 0x00u && jedec[2] == 0x00u) &&
                       !(jedec[0] == 0xffu && jedec[1] == 0xffu && jedec[2] == 0xffu) &&
                       jedec[2] == 0x16u) {
                return RB3B_OK;
            }
        }
        hal->delay_us(hal->ctx, 20000u); /* 20 ms; stock helper takes microseconds. */
    }
    return RB3B_ERR_JEDEC;
}

static rb3b_status_t rb3b_dump_target(const rb3b_hal_t *hal,
                                      const char *dir_prefix,
                                      const char *filename,
                                      rb3b_kind_t kind,
                                      uint32_t total_size,
                                      rb3b_session_state_t *sess,
                                      rb3b_target_log_t *log)
{
    char path[128];
    rb3b_sha256_ctx_t s1, s2;
    uint8_t d1[32], d2[32];
    uint32_t off;
    int32_t fd = -1;
    rb3b_status_t rc = RB3B_OK;

    log->executed = 1u;
    log->passed = 0u;
    log->status = RB3B_OK;

    rb5_progress(hal,dir_prefix,sess,RB5_EVT_TARGET_BASE+(uint32_t)kind*10u,0u,0);
    /* Disruptive bus acquisition + JEDEC verification */
    if (kind == KIND_WLAN) {
        sess->disruptive_entered = 1u;
        if (hal->wlan_bus_acquire(hal->ctx) != 0) {
            log->status = RB3B_ERR_BUS_ACQUIRE;
            return log->status;
        }
        rc = rb3b_probe_jedec(hal, hal->wlan_spi_tx_rx, log->jedec, 0);
        if (rc != RB3B_OK) {
            log->status = RB3B_ERR_JEDEC;
            return log->status;
        }
        log->jedec_ok = 1u;
    } else if (kind == KIND_IC553) {
        sess->disruptive_entered = 1u;
        if (hal->ic553_bus_acquire(hal->ctx) != 0) {
            log->status = RB3B_ERR_BUS_ACQUIRE;
            return log->status;
        }
        rc = rb3b_probe_jedec(hal, hal->ic553_spi_tx_rx, log->jedec, 1);
        if (rc != RB3B_OK) {
            log->status = RB3B_ERR_JEDEC;
            return log->status;
        }
        log->jedec_ok = 1u;
    }

    rb5_progress(hal,dir_prefix,sess,RB5_EVT_TARGET_BASE+(uint32_t)kind*10u+1u,0u,0);
    /* Pass 1: Read hardware -> SHA-256 -> Cache Clean -> SD Stream */
    rb3b_build_path(path, sizeof(path), dir_prefix, filename);
    if (hal->sd_open_write(hal->ctx, path, &fd) != 0 || fd < 0) {
        log->status = RB3B_ERR_SD_OPEN;
        return log->status;
    }

    rb3b_sha256_init(&s1);
    for (off = 0u; off < total_size; off += RB3B_STREAM_CHUNK) {
        uint32_t chunk = total_size - off;
        uint32_t written = 0u;
        if (chunk > RB3B_STREAM_CHUNK) {
            chunk = RB3B_STREAM_CHUNK;
        }
        rc = rb3b_read_hw_chunk(hal, kind, off, hal->nc_stream_buf, chunk);
        if (rc != RB3B_OK) {
            break;
        }
        rb3b_sha256_update(&s1, hal->nc_stream_buf, chunk);
        hal->cache_clean_range(hal->ctx, hal->nc_stream_buf, chunk);
        if (hal->sd_write(hal->ctx, fd, hal->nc_stream_buf, chunk, &written) != 0 || written != chunk) {
            rc = RB3B_ERR_SD_WRITE;
            break;
        }
        log->bytes_written += written;
    }

    if (hal->sd_close(hal->ctx, fd) != 0 && rc == RB3B_OK) {
        rc = RB3B_ERR_SD_CLOSE;
    }
    if (rc != RB3B_OK) {
        log->status = rc;
        return rc;
    }
    rb5_progress(hal,dir_prefix,sess,RB5_EVT_TARGET_BASE+(uint32_t)kind*10u+2u,off,rc);
    rb3b_sha256_final(&s1, d1);
    rb3b_sha256_hex(d1, log->sha256_pass1);

    /* Pass 2: Independent full hardware re-read -> SHA-256 repeatability verification */
    rb5_progress(hal,dir_prefix,sess,RB5_EVT_TARGET_BASE+(uint32_t)kind*10u+3u,0u,0);
    rb3b_sha256_init(&s2);
    for (off = 0u; off < total_size; off += RB3B_STREAM_CHUNK) {
        uint32_t chunk = total_size - off;
        if (chunk > RB3B_STREAM_CHUNK) {
            chunk = RB3B_STREAM_CHUNK;
        }
        rc = rb3b_read_hw_chunk(hal, kind, off, hal->nc_stream_buf, chunk);
        if (rc != RB3B_OK) {
            log->status = rc;
            return rc;
        }
        rb3b_sha256_update(&s2, hal->nc_stream_buf, chunk);
        if (((off + chunk) & 0x3ffffu) == 0u || off + chunk == total_size)
            rb5_progress(hal,dir_prefix,sess,RB5_EVT_TARGET_BASE+(uint32_t)kind*10u+3u,off+chunk,0);
    }
    rb5_progress(hal,dir_prefix,sess,RB5_EVT_TARGET_BASE+(uint32_t)kind*10u+4u,off,0);
    rb3b_sha256_final(&s2, d2);
    rb3b_sha256_hex(d2, log->sha256_pass2);

    if (!rb3b_memeq(d1, d2, 32u)) {
        log->status = RB3B_ERR_DOUBLE_MISMATCH;
        return log->status;
    }

    rb5_progress(hal,dir_prefix,sess,RB5_EVT_TARGET_BASE+(uint32_t)kind*10u+5u,off,0);
    log->passed = 1u;
    log->status = RB3B_OK;
    return RB3B_OK;
}

/* ==========================================================================
 * Stage 8: Emit & Readback-Verify SHA256SUMS.txt + RB3_session_manifest.json
 * ========================================================================== */
static uint32_t append_target_json(char *b, uint32_t p, uint32_t cap,
                                   const char *key, const char *fname,
                                   const rb3b_target_log_t *t, int trailing_comma)
{
    p = rb3b_append_str(b, p, cap, "    \"");
    p = rb3b_append_str(b, p, cap, key);
    p = rb3b_append_str(b, p, cap, "\": {\"file\":\"");
    p = rb3b_append_str(b, p, cap, fname);
    p = rb3b_append_str(b, p, cap, "\",\"executed\":");
    p = rb3b_append_str(b, p, cap, t->executed ? "true" : "false");
    p = rb3b_append_str(b, p, cap, ",\"passed\":");
    p = rb3b_append_str(b, p, cap, t->passed ? "true" : "false");
    p = rb3b_append_str(b, p, cap, ",\"status\":");
    p = rb3b_append_i32(b, p, cap, (int32_t)t->status);
    p = rb3b_append_str(b, p, cap, ",\"bytes\":");
    p = rb3b_append_u32(b, p, cap, t->bytes_written);
    p = rb3b_append_str(b, p, cap, ",\"jedec\":\"");
    p = rb3b_append_hex8(b, p, cap, t->jedec[0]);
    p = rb3b_append_hex8(b, p, cap, t->jedec[1]);
    p = rb3b_append_hex8(b, p, cap, t->jedec[2]);
    p = rb3b_append_str(b, p, cap, "\",\"sha256\":\"");
    p = rb3b_append_str(b, p, cap, t->passed ? t->sha256_pass1 : "");
    p = rb3b_append_str(b, p, cap, "\"}");
    if (trailing_comma) {
        p = rb3b_append_str(b, p, cap, ",");
    }
    p = rb3b_append_str(b, p, cap, "\n");
    return p;
}

static rb3b_status_t rb3b_stage8_write_manifests(const rb3b_hal_t *hal,
                                                 const char *dir_prefix,
                                                 rb3b_session_state_t *s)
{
    char path[128];
    char *tb = hal->nc_text_buf;
    uint32_t cap = hal->nc_text_cap;
    uint32_t p = 0u;
    rb3b_status_t rc;

    rb5_progress(hal,dir_prefix,s,RB5_EVT_SUMS_BEGIN,0u,0);
    /* 8a. Write & readback-verify SHA256SUMS.txt */
    tb[0] = '\0';
    if (s->ic704.passed) {
        p = rb3b_append_str(tb, p, cap, s->ic704.sha256_pass1);
        p = rb3b_append_str(tb, p, cap, "  IC704_ARM.bin\n");
    }
    if (s->ic705.passed) {
        p = rb3b_append_str(tb, p, cap, s->ic705.sha256_pass1);
        p = rb3b_append_str(tb, p, cap, "  IC705_EEPROM.bin\n");
    }
    if (s->ic706.passed) {
        p = rb3b_append_str(tb, p, cap, s->ic706.sha256_pass1);
        p = rb3b_append_str(tb, p, cap, "  IC706_EEPROM.bin\n");
    }
    if (s->wlan.passed) {
        p = rb3b_append_str(tb, p, cap, s->wlan.sha256_pass1);
        p = rb3b_append_str(tb, p, cap, "  WLAN_FLASH.bin\n");
    }
    if (s->ic553.passed) {
        p = rb3b_append_str(tb, p, cap, s->ic553.sha256_pass1);
        p = rb3b_append_str(tb, p, cap, "  IC553_DSP_FPGA.bin\n");
    }
    if (p == 0u) {
        p = rb3b_append_str(tb, p, cap, "# RB3 SELFTEST ONLY - NO STORAGE DUMPS REQUESTED\n");
    }
    rb3b_build_path(path, sizeof(path), dir_prefix, "SHA256SUMS.txt");
    rc = rb3b_write_and_verify_file(hal, path, (const uint8_t *)tb, rb3b_strlen(tb));
    if (rc != RB3B_OK) {
        return rc;
    }

    rb5_progress(hal,dir_prefix,s,RB5_EVT_SUMS_END,0u,0);
    rb5_progress(hal,dir_prefix,s,RB5_EVT_MANIFEST_BEGIN,0u,0);
    /* 8b. Write & readback-verify RB3_session_manifest.json */
    p = 0u;
    tb[0] = '\0';
    p = rb3b_append_str(tb, p, cap, "{\n");
    p = rb3b_append_str(tb, p, cap, "  \"build_id\": \"RB3-DEDICATED-BACKUP-FINAL-J140-V5\",\n");
    p = rb3b_append_str(tb, p, cap, "  \"requested_mask\": ");
    p = rb3b_append_u32(tb, p, cap, s->requested_mask);
    p = rb3b_append_str(tb, p, cap, ",\n  \"overall_status\": ");
    p = rb3b_append_i32(tb, p, cap, (int32_t)s->overall_status);
    p = rb3b_append_str(tb, p, cap, ",\n  \"backup_status\": ");
    p = rb3b_append_i32(tb, p, cap, (int32_t)s->backup_status);
    p = rb3b_append_str(tb, p, cap, ",\n  \"completion_status\": ");
    p = rb3b_append_i32(tb, p, cap, s->completion_status);
    p = rb3b_append_str(tb,p,cap,",\n  \"progress_log_status\": ");
    p = rb3b_append_i32(tb,p,cap,s->progress_log_status);
    p = rb3b_append_str(tb, p, cap, ",\n  \"sd_sync_status\": ");
    p = rb3b_append_i32(tb, p, cap, s->sd_sync_status);
    p = rb3b_append_str(tb, p, cap, ",\n  \"sd_unmount_status\": ");
    p = rb3b_append_i32(tb, p, cap, s->sd_unmount_status);
    p = rb3b_append_str(tb, p, cap, ",\n  \"completion_note\": \"1=PENDING; final result is in RB5_COMPLETION.json after restart/menu\"");
    p = rb3b_append_str(tb, p, cap, ",\n  \"stage1_native_ok\": ");
    p = rb3b_append_str(tb, p, cap, s->stage1_native_ok ? "true" : "false");
    p = rb3b_append_str(tb, p, cap, ",\n  \"stage2_sd_ok\": ");
    p = rb3b_append_str(tb, p, cap, s->stage2_sd_ok ? "true" : "false");
    p = rb3b_append_str(tb, p, cap, ",\n  \"trigger_consumed\": ");
    p = rb3b_append_str(tb, p, cap, s->trigger_consumed ? "true" : "false");
    p = rb3b_append_str(tb, p, cap, ",\n  \"tx_inhibit_ok\": ");
    p = rb3b_append_str(tb, p, cap, s->tx_inhibit_ok ? "true" : "false");
    p = rb3b_append_str(tb, p, cap, ",\n  \"disruptive_entered\": ");
    p = rb3b_append_str(tb, p, cap, s->disruptive_entered ? "true" : "false");
    p = rb3b_append_str(tb, p, cap, ",\n  \"storage_write_operations\": 0,\n");
    p = rb3b_append_str(tb, p, cap, "  \"ic704_note\": \"POST_MOD_ACTIVE_READBACK_DO_NOT_OVERWRITE_PRE_MOD_T48_GOLDEN_BACKUP\",\n");
    p = rb3b_append_str(tb, p, cap, "  \"eeprom_logical_note\": \"RECONSTRUCT_OFFLINE_ON_PC_FROM_IC705_AND_IC706\",\n");
    p = rb3b_append_str(tb, p, cap, "  \"targets\": {\n");
    p = append_target_json(tb, p, cap, "IC704", "IC704_ARM.bin", &s->ic704, 1);
    p = append_target_json(tb, p, cap, "IC705", "IC705_EEPROM.bin", &s->ic705, 1);
    p = append_target_json(tb, p, cap, "IC706", "IC706_EEPROM.bin", &s->ic706, 1);
    p = append_target_json(tb, p, cap, "WLAN",  "WLAN_FLASH.bin", &s->wlan, 1);
    p = append_target_json(tb, p, cap, "IC553", "IC553_DSP_FPGA.bin", &s->ic553, 0);
    p = rb3b_append_str(tb, p, cap, "  }\n}\n");

    rb3b_build_path(path, sizeof(path), dir_prefix, "RB3_session_manifest.json");
    rc = rb3b_write_and_verify_file(hal, path, (const uint8_t *)tb, rb3b_strlen(tb));
    if (rc != RB3B_OK) {
        return rc;
    }

    rb5_progress(hal,dir_prefix,s,RB5_EVT_MANIFEST_END,0u,0);
    s->stage8_verify_ok = 1u;
    return RB3B_OK;
}

int rb4_restart_allowed(const rb3b_session_state_t *s)
{
    return s->stage1_native_ok && s->stage2_sd_ok && s->trigger_consumed &&
           s->stage8_verify_ok && s->disruptive_entered &&
           s->sd_synced && s->sd_unmounted &&
           s->sd_sync_status == 0 && s->sd_unmount_status == 0 &&
           s->completion_status == 0 && s->completion_log_status == 0 && s->progress_log_status == 0;
}

rb3b_status_t rb4_write_completion_report(const rb3b_hal_t *hal,
                                         const char *dir_prefix,
                                         const rb3b_session_state_t *s,
                                         uint8_t after_restart)
{
    char path[128];
    char *b = hal->nc_text_buf;
    uint32_t cap = hal->nc_text_cap, p = 0u;
    b[0] = '\0';
    p = rb3b_append_str(b,p,cap,"{\n  \"build_id\": \"RB3-DEDICATED-BACKUP-FINAL-J140-V5\",\n  \"requested_mask\": ");
    p = rb3b_append_u32(b,p,cap,s->requested_mask);
    p = rb3b_append_str(b,p,cap,",\n  \"backup_status\": ");
    p = rb3b_append_i32(b,p,cap,s->backup_status);
    p = rb3b_append_str(b,p,cap,",\n  \"completion_status\": ");
    p = rb3b_append_i32(b,p,cap,s->completion_status);
    p = rb3b_append_str(b,p,cap,",\n  \"progress_log_status\": ");
    p = rb3b_append_i32(b,p,cap,s->progress_log_status);
    p = rb3b_append_str(b,p,cap,",\n  \"sd_sync_status\": ");
    p = rb3b_append_i32(b,p,cap,s->sd_sync_status);
    p = rb3b_append_str(b,p,cap,",\n  \"sd_unmount_status\": ");
    p = rb3b_append_i32(b,p,cap,s->sd_unmount_status);
    p = rb3b_append_str(b,p,cap,",\n  \"completion_log_status\": ");
    p = rb3b_append_i32(b,p,cap,s->completion_log_status);
    p = rb3b_append_str(b,p,cap,",\n  \"post_restart_receipt\": ");
    p = rb3b_append_str(b,p,cap,after_restart ? "true" : "false");
    p = rb3b_append_str(b,p,cap,",\n  \"wlan_sha256\": \"");
    p = rb3b_append_str(b,p,cap,s->wlan.passed ? s->wlan.sha256_pass1 : "");
    p = rb3b_append_str(b,p,cap,"\",\n  \"arm_sha256\": \"");
    p = rb3b_append_str(b,p,cap,s->ic704.passed ? s->ic704.sha256_pass1 : "");
    p = rb3b_append_str(b,p,cap,"\"\n}\n");
    if (p >= cap - 1u) return RB5_ERR_COMPLETION_LOG;
    rb3b_build_path(path,sizeof(path),dir_prefix,"RB5_COMPLETION.json");
    return rb3b_write_and_verify_file(hal,path,(const uint8_t *)b,p);
}

/* ==========================================================================
 * Master 9-Stage Backup State Machine
 * ========================================================================== */
rb3b_status_t rb3b_run_from_sd_triggers(const rb3b_hal_t *hal,
                                        const char *dir_prefix,
                                        rb3b_session_state_t *out_state)
{
    uint32_t mask;
    uint8_t matched_bits = 0u;
    rb3b_status_t rc = RB3B_OK;

    rb3b_zero(out_state, (uint32_t)sizeof(*out_state));
    out_state->completion_status = 1;
    out_state->sd_sync_status = 1;
    out_state->sd_unmount_status = 1;

    mask = rb3b_probe_user_triggers(hal, dir_prefix, &matched_bits);
    if (mask == 0u) {
        out_state->overall_status = RB3B_NO_TRIGGER;
        return RB3B_NO_TRIGGER;
    }
    out_state->requested_mask = mask;

    /* Stage 1: Native Execution Self-test (HARD GATE) */
    rc = rb3b_stage1_native_selftest(hal, out_state);
    if (rc != RB3B_OK) {
        out_state->overall_status = rc;
        hal->restore_tx_inhibit(hal->ctx);
        out_state->runtime_state_restored = 1u;
        return rc;
    }

    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_STAGE1_END,0u,rc);
    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_STAGE2_BEGIN,0u,0);
    /* Stage 2: SD Filesystem Self-test (HARD GATE - abort BEFORE any storage read) */
    rc = rb3b_stage2_sd_selftest(hal, dir_prefix, out_state);
    if (rc != RB3B_OK) {
        out_state->overall_status = rc;
        hal->restore_tx_inhibit(hal->ctx);
        out_state->runtime_state_restored = 1u;
        return rc;
    }

    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_STAGE2_END,0u,rc);
    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_TRIGGER_BEGIN,0u,0);
    /* One-shot trigger consumption: disarm matched .TRG files BEFORE any storage access */
    if (rb3b_consume_matched_triggers(hal, dir_prefix, matched_bits) == RB3B_OK) {
        out_state->trigger_consumed = 1u;
    } else {
        out_state->overall_status = RB3B_ERR_VERIFY_READBACK;
        hal->restore_tx_inhibit(hal->ctx);
        out_state->runtime_state_restored = 1u;
        return out_state->overall_status;
    }

    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_TRIGGER_END,0u,rc);
    /* Stage 3: IC704 Main NOR (16 MiB, non-disruptive XIP) */
    if (rc == RB3B_OK && (mask & RB3B_MASK_IC704) != 0u) {
        rc = rb3b_dump_target(hal, dir_prefix, "IC704_ARM.bin",
                              KIND_IC704, RB3B_IC704_SIZE, out_state, &out_state->ic704);
        rb5_progress(hal,dir_prefix,out_state,RB5_EVT_TARGET_BASE+0u*10u+6u,out_state->ic704.bytes_written,rc);
    }

    /* Suppress periodic background EEPROM dirty-flush (0x200D5120) during Stages 4..5 */
    if (rc == RB3B_OK && ((mask & (RB3B_MASK_IC705 | RB3B_MASK_IC706)) != 0u)) {
        hal->set_eeprom_write_suppress(hal->ctx, 1u);
    }

    /* Stage 4: IC705 EEPROM (128 KiB, physical Chip 0, non-disruptive) */
    if (rc == RB3B_OK && (mask & RB3B_MASK_IC705) != 0u) {
        rc = rb3b_dump_target(hal, dir_prefix, "IC705_EEPROM.bin",
                              KIND_IC705, RB3B_IC705_SIZE, out_state, &out_state->ic705);
        rb5_progress(hal,dir_prefix,out_state,RB5_EVT_TARGET_BASE+1u*10u+6u,out_state->ic705.bytes_written,rc);
    }

    /* Stage 5: IC706 EEPROM (128 KiB, physical Chip 1, non-disruptive) */
    if (rc == RB3B_OK && (mask & RB3B_MASK_IC706) != 0u) {
        rc = rb3b_dump_target(hal, dir_prefix, "IC706_EEPROM.bin",
                              KIND_IC706, RB3B_IC706_SIZE, out_state, &out_state->ic706);
        rb5_progress(hal,dir_prefix,out_state,RB5_EVT_TARGET_BASE+2u*10u+6u,out_state->ic706.bytes_written,rc);
    }

    /* Stage 6: WLAN Flash (4 MiB, RSPI2, EXPLICIT disruptive confirmation) */
    if (rc == RB3B_OK && (mask & RB3B_MASK_WLAN) != 0u) {
        rc = rb3b_dump_target(hal, dir_prefix, "WLAN_FLASH.bin",
                              KIND_WLAN, RB3B_WLAN_SIZE, out_state, &out_state->wlan);
        rb5_progress(hal,dir_prefix,out_state,RB5_EVT_TARGET_BASE+3u*10u+6u,out_state->wlan.bytes_written,rc);
    }

    /* Stage 7: IC553 DSP/FPGA Flash (2 MiB, RSPI4, ALWAYS LAST!) */
    if (rc == RB3B_OK && (mask & RB3B_MASK_IC553) != 0u) {
        rc = rb3b_dump_target(hal, dir_prefix, "IC553_DSP_FPGA.bin",
                              KIND_IC553, RB3B_IC553_SIZE, out_state, &out_state->ic553);
        rb5_progress(hal,dir_prefix,out_state,RB5_EVT_TARGET_BASE+4u*10u+6u,out_state->ic553.bytes_written,rc);
    }

    out_state->overall_status = rc;
    out_state->backup_status = rc;

    /* Stage 8: Emit & Readback-Verify SHA256SUMS.txt + RB3_session_manifest.json */
    {
        rb3b_status_t mrc = rb3b_stage8_write_manifests(hal, dir_prefix, out_state);
        if (rc == RB3B_OK && mrc != RB3B_OK) {
            rc = mrc;
            out_state->overall_status = rc;
        }
    }

    /* Invalidate any previous successful receipt BEFORE the final unmount. */
    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_RECEIPT_BEGIN,0u,0);
    out_state->completion_log_status = rb4_write_completion_report(hal,dir_prefix,out_state,0u);
    if (!out_state->stage8_verify_ok) out_state->completion_log_status = RB5_ERR_COMPLETION_LOG;
    if (out_state->completion_log_status != 0) {
        out_state->completion_status = RB5_ERR_COMPLETION_LOG;
        out_state->overall_status = RB5_ERR_COMPLETION_LOG;
    }
    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_RECEIPT_END,0u,out_state->completion_log_status);
    if (out_state->progress_log_status != 0) {
        out_state->completion_log_status = RB5_ERR_COMPLETION_LOG;
        out_state->completion_status = RB5_ERR_COMPLETION_LOG;
        out_state->overall_status = RB5_ERR_COMPLETION_LOG;
    }
    /* Stage 9. Raw statuses are retained; no reset before both operations pass. */
    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_SYNC_BEGIN,0u,0);
    out_state->sd_sync_status = hal->sd_sync_all(hal->ctx);
    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_SYNC_END,0u,out_state->sd_sync_status);
    if (out_state->sd_sync_status == 0) out_state->sd_synced = 1u;
    else {
        out_state->completion_status = RB5_ERR_SD_SYNC;
        out_state->overall_status = RB5_ERR_SD_SYNC;
    }
    if (out_state->disruptive_entered) {
        if (out_state->sd_synced && out_state->completion_log_status == 0 && out_state->progress_log_status == 0) {
            rb5_progress(hal,dir_prefix,out_state,RB5_EVT_UNMOUNT_BEGIN,0u,0);
            if (out_state->progress_log_status == 0)
                out_state->sd_unmount_status = hal->sd_unmount(hal->ctx);
            else out_state->sd_unmount_status = RB5_ERR_COMPLETION_LOG;
            if (out_state->sd_unmount_status == 0) out_state->sd_unmounted = 1u;
            rb5_progress(hal,dir_prefix,out_state,RB5_EVT_UNMOUNT_END,0u,out_state->sd_unmount_status);
            if (out_state->sd_unmount_status != 0) {
                out_state->completion_status = RB5_ERR_SD_UNMOUNT;
                out_state->overall_status = RB5_ERR_SD_UNMOUNT;
            }
        }
        /* Keep [0x203802BC+6]=1 (suppresses 0x2005EA44 shutdown EEPROM commit) & TX inhibit */
        hal->set_eeprom_write_suppress(hal->ctx, 1u);
        out_state->eeprom_shutdown_suppressed = 1u;
    } else {
        /* Non-disruptive run: cleanly restore [0x203802BC+6] and TX inhibit before stock UI */
        hal->set_eeprom_write_suppress(hal->ctx, 0u);
        hal->restore_tx_inhibit(hal->ctx);
        out_state->runtime_state_restored = 1u;
    }

    if (out_state->completion_status == 1) out_state->completion_status = 0;
    if (!out_state->sd_unmounted) {
        /* On error, persist best-effort details while the volume may be usable.
         * On success after unmount, never reopen the card: the retained receipt
         * is exported on the next Firmware Update menu entry after reset. */
        rb3b_status_t mrc = rb3b_stage8_write_manifests(hal,dir_prefix,out_state);
        rb3b_status_t lrc = rb4_write_completion_report(hal,dir_prefix,out_state,0u);
        if (mrc != RB3B_OK || lrc != RB3B_OK) {
            out_state->completion_log_status = RB5_ERR_COMPLETION_LOG;
            out_state->completion_status = RB5_ERR_COMPLETION_LOG;
            out_state->overall_status = RB5_ERR_COMPLETION_LOG;
        }
        {
            int final_sync;
            rb5_progress(hal,dir_prefix,out_state,RB5_EVT_REPORT_SYNC_BEGIN,0u,0);
            final_sync = hal->sd_sync_all(hal->ctx);
            rb5_progress(hal,dir_prefix,out_state,RB5_EVT_REPORT_SYNC_END,0u,final_sync);
            if (final_sync == 0) out_state->completion_error_report_synced = 1u;
            else if (out_state->completion_status == 0) {
                out_state->sd_sync_status = final_sync;
                out_state->sd_synced = 0u;
                out_state->completion_status = RB5_ERR_SD_SYNC;
                out_state->overall_status = RB5_ERR_SD_SYNC;
                /* One best-effort error snapshot; never leave a known failure
                 * represented only by the earlier success report. */
                mrc = rb3b_stage8_write_manifests(hal,dir_prefix,out_state);
                lrc = rb4_write_completion_report(hal,dir_prefix,out_state,0u);
                if (mrc != RB3B_OK || lrc != RB3B_OK)
                    out_state->completion_log_status = RB5_ERR_COMPLETION_LOG;
                if (hal->sd_sync_all(hal->ctx) == 0)
                    out_state->completion_error_report_synced = 1u;
            }
        }
    }

    if (out_state->progress_log_status != 0 && out_state->overall_status == 0) {
        out_state->completion_status = RB5_ERR_COMPLETION_LOG;
        out_state->overall_status = RB5_ERR_COMPLETION_LOG;
    }
    rb5_progress(hal,dir_prefix,out_state,RB5_EVT_RETURN,0u,out_state->overall_status);
    if (out_state->progress_log_status != 0 && out_state->overall_status == 0)
        out_state->overall_status = RB5_ERR_COMPLETION_LOG;
    return out_state->overall_status;
}
