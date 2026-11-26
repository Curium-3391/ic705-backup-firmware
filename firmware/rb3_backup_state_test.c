/*
 * rb3_backup_state_test.c - Offline State-Machine Verifier for rb3_backup_core.c
 *
 * Compiles the EXACT target state machine (rb3_backup_core.c + rb3_backup_sha256.c)
 * to verify all 9 stages, one-shot trigger consumption ("CONSUMED\r\n"), hard self-test
 * gating, strict IC553-last ordering, zero storage writes, double-pass verification,
 * non-disruptive TX/EEPROM-flag restoration, ENOSPC mid-stream safe abort, and
 * disruptive SD unmount + EEPROM shutdown commit suppression.
 */
#include "rb3_backup_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    uint8_t *ic704;
    uint8_t *ic705;
    uint8_t *ic706;
    uint8_t *wlan;
    uint8_t *ic553;
    uint8_t  wlan_jedec[3];
    uint8_t  ic553_jedec[3];
    int      fail_j140_id;
    int      fail_tx_inhibit;
    int      fail_sd_write;
    int      fail_sd_sync;
    int      fail_sd_sync_call;
    int      fail_sd_unmount;
    int      fail_completion_log;
    int      fail_progress_sync;
    int      progress_sync_calls;
    int      volume_unmounted;
    uint32_t enospc_after_bytes;
    uint32_t sd_bytes_written;
    int      fail_eeprom_busy;
    int      flip_ic553_pass2;
    int      wlan_rdid_zero_count;
    int      wlan_rdid_calls;
    uint32_t retry_delay_calls;
    uint32_t retry_delay_total_us;

    int      tx_inhibit_active;
    int      ee_suppress_active;
    int      ee_suppress_seen_during_eeprom;
    int      open_fds;
    int      max_open_fds;
    int      storage_write_attempts;
    int      sd_sync_calls;
    int      sd_unmount_calls;
    int      wlan_bus_calls;
    int      ic553_bus_calls;
    int      ic553_reads;
    int      stage_order[10];
    int      stage_order_len;
    FILE    *fds[4];
    int fd_progress[4];
    uint32_t last_progress_event, last_progress_offset;
} mock_ctx_t;

static uint32_t g_stop_event, g_stop_offset;
static uint8_t g_test_nc_stream[RB3B_STREAM_CHUNK];
static char    g_test_nc_text[2048];

static void fill_det(uint8_t *b, uint32_t n, uint32_t seed) {
    uint32_t x = seed, i;
    for (i = 0u; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b[i] = (uint8_t)(x & 0xffu);
    }
}

static void record_stage(mock_ctx_t *m, int stg) {
    if (m->stage_order_len == 0 || m->stage_order[m->stage_order_len - 1] != stg) {
        if (m->stage_order_len < 10) {
            m->stage_order[m->stage_order_len++] = stg;
        }
    }
}

static int m_check_id(void *v) { return ((mock_ctx_t *)v)->fail_j140_id ? -1 : 0; }
static int m_tx_inh(void *v) {
    mock_ctx_t *m = (mock_ctx_t *)v;
    if (m->fail_tx_inhibit) return -1;
    m->tx_inhibit_active = 1;
    return 0;
}
static void m_tx_restore(void *v) { ((mock_ctx_t *)v)->tx_inhibit_active = 0; }
static void m_cache(void *v, const void *b, uint32_t n) { (void)v; (void)b; (void)n; }
static void m_delay(void *v, uint32_t usec) { mock_ctx_t *m = (mock_ctx_t *)v; m->retry_delay_calls++; m->retry_delay_total_us += usec; }

static int m_open_mode(mock_ctx_t *m, const char *p, const char *mode, int32_t *out_fd) {
    int i;
    FILE *f;
    if (m->volume_unmounted) return -1;
    if (m->fail_completion_log && strstr(p,"RB5_COMPLETION.json")) return -1;
    for (i = 0; i < 4; ++i) if (!m->fds[i]) break;
    if (i >= 4) return -1;
    f = fopen(p, mode);
    if (!f) return -1;
    m->fds[i] = f;
    m->fd_progress[i] = strstr(p,"RB5_PROGRESS_") != NULL;
    m->open_fds++;
    if (m->open_fds > m->max_open_fds) m->max_open_fds = m->open_fds;
    *out_fd = i;
    return 0;
}

static int m_open_w(void *v, const char *p, int32_t *fd) { return m_open_mode((mock_ctx_t *)v, p, "wb", fd); }
static int m_open_r(void *v, const char *p, int32_t *fd) { return m_open_mode((mock_ctx_t *)v, p, "rb", fd); }

static int m_write(void *v, int32_t fd, const uint8_t *b, uint32_t n, uint32_t *ow) {
    mock_ctx_t *m = (mock_ctx_t *)v;
    if (m->fail_sd_write || fd < 0 || fd >= 4 || !m->fds[fd]) return -1;
    if (m->enospc_after_bytes > 0u && m->sd_bytes_written + n > m->enospc_after_bytes) {
        *ow = 0u;
        return -1;
    }
    if (m->fd_progress[fd] && n < 512u) {
        char text[512], *q;
        memcpy(text,b,n); text[n] = 0;
        q = strstr(text,"\"event\": ");
        if (q) m->last_progress_event = (uint32_t)strtoul(q+9,NULL,10);
        q = strstr(text,"\"offset\": ");
        if (q) m->last_progress_offset = (uint32_t)strtoul(q+10,NULL,10);
    }
    *ow = (uint32_t)fwrite(b, 1u, n, m->fds[fd]);
    m->sd_bytes_written += *ow;
    return (*ow == n) ? 0 : -1;
}

static int m_read(void *v, int32_t fd, uint8_t *b, uint32_t n, uint32_t *or_len) {
    mock_ctx_t *m = (mock_ctx_t *)v;
    if (fd < 0 || fd >= 4 || !m->fds[fd]) return -1;
    *or_len = (uint32_t)fread(b, 1u, n, m->fds[fd]);
    return 0;
}

static int m_close(void *v, int32_t fd) {
    mock_ctx_t *m = (mock_ctx_t *)v;
    if (fd < 0 || fd >= 4 || !m->fds[fd]) return -1;
    fclose(m->fds[fd]);
    m->fds[fd] = NULL;
    m->open_fds--;
    return 0;
}

static int m_progress_sync(void *v) { mock_ctx_t *m = (mock_ctx_t *)v; m->progress_sync_calls++;
    if (g_stop_event && m->last_progress_event == g_stop_event && m->last_progress_offset == g_stop_offset)
        exit(77); /* host-only abrupt stop after checkpoint close, before later operation */
    return m->fail_progress_sync ? -61 : 0; }
static int m_sync(void *v)    { mock_ctx_t *m = (mock_ctx_t *)v; m->sd_sync_calls++; return (m->fail_sd_sync || m->sd_sync_calls == m->fail_sd_sync_call) ? -31 : 0; }
static int m_unmount(void *v) { mock_ctx_t *m = (mock_ctx_t *)v; m->sd_unmount_calls++; if (m->fail_sd_unmount) return -47; m->volume_unmounted = 1; return 0; }
static void m_set_ee(void *v, uint8_t en) { ((mock_ctx_t *)v)->ee_suppress_active = en ? 1 : 0; }

static int m_ic704(void *v, uint32_t off, uint8_t *dst, uint32_t n) {
    mock_ctx_t *m = (mock_ctx_t *)v;
    record_stage(m, 3);
    memcpy(dst, m->ic704 + off, n);
    return 0;
}

static int m_ee_idle(void *v) { return ((mock_ctx_t *)v)->fail_eeprom_busy ? -1 : 0; }

static int m_ee_read(void *v, uint8_t chip, uint32_t off, uint8_t *dst, uint32_t n) {
    mock_ctx_t *m = (mock_ctx_t *)v;
    if (m->ee_suppress_active) m->ee_suppress_seen_during_eeprom = 1;
    record_stage(m, chip == 0u ? 4 : 5);
    memcpy(dst, (chip == 0u ? m->ic705 : m->ic706) + off, n);
    return 0;
}

static int m_wlan_acq(void *v)  { mock_ctx_t *m = (mock_ctx_t *)v; m->wlan_bus_calls++; record_stage(m, 6); return 0; }
static int m_ic553_acq(void *v) { mock_ctx_t *m = (mock_ctx_t *)v; m->ic553_bus_calls++; record_stage(m, 7); return 0; }

static int m_spi(mock_ctx_t *m, const uint8_t *img, const uint8_t jedec[3], int is_ic553,
                 const uint8_t *tx, uint32_t txl, uint8_t *rx, uint32_t rxl) {
    if (txl == 0u || !rb3b_spi_opcode_is_readonly(tx[0])) { m->storage_write_attempts++; return -1; }
    if (tx[0] == 0x9Fu) { memcpy(rx, jedec, 3u); return 0; }
    if (tx[0] == 0x03u) {
        uint32_t a = ((uint32_t)tx[1] << 16) | ((uint32_t)tx[2] << 8) | tx[3];
        memcpy(rx, img + a, rxl);
        if (is_ic553) {
            if (m->flip_ic553_pass2 && m->ic553_reads >= (int)RB3B_IC553_SIZE && a == 0u) rx[0] ^= 1u;
            m->ic553_reads += (int)rxl;
        }
        return 0;
    }
    return -1;
}

static int m_wlan_spi(void *v, const uint8_t *tx, uint32_t txl, uint8_t *rx, uint32_t rxl) {
    mock_ctx_t *mm = (mock_ctx_t *)v;
    if (txl > 0u && tx[0] == 0x9Fu) {
        mm->wlan_rdid_calls++;
        if (mm->wlan_rdid_calls <= mm->wlan_rdid_zero_count) {
            rx[0] = 0u; rx[1] = 0u; rx[2] = 0u;
            return 0;
        }
    }
    return m_spi((mock_ctx_t *)v, ((mock_ctx_t *)v)->wlan, ((mock_ctx_t *)v)->wlan_jedec, 0, tx, txl, rx, rxl);
}
static int m_ic553_spi(void *v, const uint8_t *tx, uint32_t txl, uint8_t *rx, uint32_t rxl) {
    return m_spi((mock_ctx_t *)v, ((mock_ctx_t *)v)->ic553, ((mock_ctx_t *)v)->ic553_jedec, 1, tx, txl, rx, rxl);
}

static void arm_trigger(const char *dir, const char *name) {
    char p[256];
    FILE *f;
    snprintf(p, sizeof(p), "%s%s", dir, name);
    f = fopen(p, "wb");
    if (f) {
        fputs("ARMED\r\n", f);
        fclose(f);
    }
}

static void remove_file(const char *dir, const char *name) {
    char p[256];
    snprintf(p, sizeof(p), "%s%s", dir, name);
    (void)remove(p);
}

static int file_contains(const char *dir, const char *name, const char *needle) {
    char path[256], data[4096];
    FILE *f;
    size_t n;
    snprintf(path,sizeof(path),"%s%s",dir,name);
    f = fopen(path,"rb");
    if (!f) return 0;
    n = fread(data,1,sizeof(data)-1,f); fclose(f); data[n] = 0;
    return strstr(data,needle) != NULL;
}

static void reset_m(mock_ctx_t *m) {
    m->wlan_jedec[0] = 0xC2; m->wlan_jedec[1] = 0x28; m->wlan_jedec[2] = 0x16;
    m->ic553_jedec[0] = 0x9D; m->ic553_jedec[1] = 0x60; m->ic553_jedec[2] = 0x15;
    m->fail_j140_id = 0; m->fail_tx_inhibit = 0; m->fail_sd_write = 0;
    m->fail_progress_sync = 0; m->progress_sync_calls = 0;
    m->fail_sd_sync_call = 0; m->fail_sd_sync = 0; m->fail_sd_unmount = 0; m->fail_completion_log = 0; m->volume_unmounted = 0;
    m->enospc_after_bytes = 0u; m->sd_bytes_written = 0u;
    m->fail_eeprom_busy = 0; m->flip_ic553_pass2 = 0;
    m->wlan_rdid_zero_count = 0; m->wlan_rdid_calls = 0;
    m->retry_delay_calls = 0; m->retry_delay_total_us = 0;
    m->tx_inhibit_active = 0; m->ee_suppress_active = 0; m->ee_suppress_seen_during_eeprom = 0;
    m->open_fds = 0; m->max_open_fds = 0; m->storage_write_attempts = 0;
    m->sd_sync_calls = 0; m->sd_unmount_calls = 0;
    m->wlan_bus_calls = 0; m->ic553_bus_calls = 0; m->ic553_reads = 0;
    m->stage_order_len = 0;
}

#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

int main(int argc, char **argv) {
    const char *base_dir = (argc > 1) ? argv[1] : "/tmp/rb3_backup_sm_test";
    char prefix[256];
    mock_ctx_t m;
    rb3b_hal_t hal;
    rb3b_session_state_t s;
    rb3b_status_t rc;

    (void)mkdir(base_dir, 0755);
    snprintf(prefix, sizeof(prefix), "%s/", base_dir);

    memset(&m, 0, sizeof(m));
    m.ic704 = (uint8_t *)malloc(RB3B_IC704_SIZE);
    m.ic705 = (uint8_t *)malloc(RB3B_IC705_SIZE);
    m.ic706 = (uint8_t *)malloc(RB3B_IC706_SIZE);
    m.wlan  = (uint8_t *)malloc(RB3B_WLAN_SIZE);
    m.ic553 = (uint8_t *)malloc(RB3B_IC553_SIZE);
    fill_det(m.ic704, RB3B_IC704_SIZE, 0x1001u);
    fill_det(m.ic705, RB3B_IC705_SIZE, 0x2002u);
    fill_det(m.ic706, RB3B_IC706_SIZE, 0x3003u);
    fill_det(m.wlan,  RB3B_WLAN_SIZE,  0x4004u);
    fill_det(m.ic553, RB3B_IC553_SIZE, 0x5005u);

    memset(&hal, 0, sizeof(hal));
    hal.ctx = &m; hal.nc_stream_buf = g_test_nc_stream;
    hal.nc_text_buf = g_test_nc_text; hal.nc_text_cap = sizeof(g_test_nc_text);
    hal.check_j140_identity = m_check_id; hal.assert_tx_inhibit = m_tx_inh;
    hal.restore_tx_inhibit = m_tx_restore; hal.cache_clean_range = m_cache;
    hal.delay_us = m_delay;
    hal.sd_open_write = m_open_w; hal.sd_open_read = m_open_r;
    hal.sd_write = m_write; hal.sd_read = m_read; hal.sd_close = m_close;
    hal.sd_sync_all = m_sync; hal.progress_sync = m_progress_sync; hal.sd_unmount = m_unmount;
    hal.set_eeprom_write_suppress = m_set_ee;
    hal.ic704_xip_read = m_ic704; hal.eeprom_wait_idle = m_ee_idle;
    hal.eeprom_phys_read = m_ee_read; hal.wlan_bus_acquire = m_wlan_acq;
    hal.wlan_spi_tx_rx = m_wlan_spi; hal.ic553_bus_acquire = m_ic553_acq;
    hal.ic553_spi_tx_rx = m_ic553_spi;

    if (argc > 2) {
        g_stop_event = (uint32_t)strtoul(argv[2],NULL,10);
        g_stop_offset = argc > 3 ? (uint32_t)strtoul(argv[3],NULL,10) : 0u;
        reset_m(&m);
        arm_trigger(prefix,"RB3_FULL_BACKUP.TRG");
        (void)rb3b_run_from_sd_triggers(&hal,prefix,&s);
        return 78; /* requested checkpoint was not reached */
    }

    /* Test 1: No trigger -> NO_TRIGGER, zero side effects */
    reset_m(&m);
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    CHECK(rc == RB3B_NO_TRIGGER && m.wlan_bus_calls == 0 && m.ic553_bus_calls == 0, "no trigger idle");

    /* Test 2: Self-test only (RB3_STG12.TRG) + verify one-shot CONSUMED disarm on immediate re-probe */
    reset_m(&m);
    arm_trigger(prefix, "RB3_STG12.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    CHECK(rc == RB3B_OK && s.stage1_native_ok && s.stage2_sd_ok && s.trigger_consumed && s.stage8_verify_ok, "stg12 pass");
    CHECK(!s.ic704.executed && !s.ic705.executed && !s.ic706.executed && !s.wlan.executed && !s.ic553.executed, "stg12 zero storage reads");
    CHECK(s.sd_synced && !s.sd_unmounted && !s.disruptive_entered && s.runtime_state_restored, "stg12 restored");
    CHECK(m.tx_inhibit_active == 0 && m.ee_suppress_active == 0, "stg12 flags cleanly restored");
    /* Re-probe WITHOUT deleting RB3_STG12.TRG -> MUST return NO_TRIGGER because it was CONSUMED! */
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    CHECK(rc == RB3B_NO_TRIGGER, "oneshot trigger consumed across reprobe");
    remove_file(prefix, "RB3_STG12.TRG");

    /* Test 3: Stage 1 failure hard-gates ALL storage reads even with RB3_FULL_BACKUP.TRG */
    reset_m(&m);
    m.fail_j140_id = 1;
    arm_trigger(prefix, "RB3_FULL_BACKUP.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    CHECK(rc == RB3B_ERR_SELFTEST_NATIVE && !s.stage1_native_ok && m.stage_order_len == 0 && s.runtime_state_restored, "stg1 hard gate");

    /* Test 4: Stage 2 SD failure hard-gates ALL storage reads */
    reset_m(&m);
    m.fail_sd_write = 1;
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    CHECK(rc == RB3B_ERR_SELFTEST_SD && s.stage1_native_ok && !s.stage2_sd_ok && m.stage_order_len == 0 && s.runtime_state_restored, "stg2 hard gate");
    remove_file(prefix, "RB3_FULL_BACKUP.TRG");

    /* Test 5: Non-disruptive SAFE_ALL (Stages 1..5 -> EEPROM suppress held during 4..5, restored on exit!) */
    reset_m(&m);
    arm_trigger(prefix, "RB3_SAFE_ALL.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_SAFE_ALL.TRG");
    CHECK(rc == RB3B_OK && s.ic704.passed && s.ic705.passed && s.ic706.passed, "safe_all passed");
    CHECK(m.ee_suppress_seen_during_eeprom == 1, "ee suppress active during stages 4..5");
    CHECK(m.ee_suppress_active == 0 && m.tx_inhibit_active == 0 && s.runtime_state_restored, "safe_all flags restored");
    CHECK(!s.wlan.executed && !s.ic553.executed && m.wlan_bus_calls == 0 && m.ic553_bus_calls == 0, "safe_all zero disruptive calls");
    CHECK(s.sd_synced && !s.sd_unmounted && m.open_fds == 0 && m.max_open_fds == 1, "safe_all clean fd");

    /* Test 6: Full 9-stage backup (RB3_FULL_BACKUP.TRG) -> strict order 3->4->5->6->7 (IC553 LAST!) */
    reset_m(&m);
    arm_trigger(prefix, "RB3_FULL_BACKUP.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    CHECK(rc == RB3B_OK && s.ic704.passed && s.ic705.passed && s.ic706.passed && s.wlan.passed && s.ic553.passed, "full 9-stage pass");
    CHECK(m.stage_order_len == 5 && m.stage_order[0] == 3 && m.stage_order[1] == 4 &&
          m.stage_order[2] == 5 && m.stage_order[3] == 6 && m.stage_order[4] == 7, "strict IC553-last order");
    CHECK(m.storage_write_attempts == 0 && m.max_open_fds == 1 && m.open_fds == 0, "0 writes, <=1 fd");
    CHECK(s.stage8_verify_ok && s.sd_synced && s.sd_unmounted && s.eeprom_shutdown_suppressed && m.ee_suppress_active == 1 && m.tx_inhibit_active == 1,
          "stage8+9 unmount & suppress ee & hold tx inhibit");
    /* Verify RB3_FULL_BACKUP.TRG is now CONSUMED even if left on SD after reboot */
    CHECK(rb4_restart_allowed(&s), "B5 successful FULL permits restart");
    m.volume_unmounted = 0; /* next boot remount */
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    CHECK(rc == RB3B_NO_TRIGGER, "full backup trigger consumed post-reboot");
    remove_file(prefix, "RB3_FULL_BACKUP.TRG");

    /* Test 7: IC553 pass-2 bit flip detected and still cleanly unmounts SD + suppresses EEPROM commit */
    reset_m(&m);
    m.flip_ic553_pass2 = 1;
    arm_trigger(prefix, "RB3_STG7_IC553.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_STG7_IC553.TRG");
    CHECK(rc == RB3B_ERR_DOUBLE_MISMATCH && !s.ic553.passed && s.sd_unmounted && s.eeprom_shutdown_suppressed && m.open_fds == 0,
          "pass2 mismatch safe unmount");

    /* Test 8: ENOSPC (SD disk full mid-IC704 stream) aborts cleanly, closes FD, restores flags, never touches EEPROM/WLAN/IC553 */
    reset_m(&m);
    m.enospc_after_bytes = 65536u;
    arm_trigger(prefix, "RB3_FULL_BACKUP.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_FULL_BACKUP.TRG");
    CHECK(s.backup_status == RB3B_ERR_SD_WRITE && rc != 0 && s.ic704.executed && !s.ic704.passed && !s.ic705.executed && !s.wlan.executed && !s.ic553.executed,
          "enospc aborts before later stages");
    CHECK(m.open_fds == 0 && m.tx_inhibit_active == 0 && m.ee_suppress_active == 0 && s.runtime_state_restored,
          "enospc closes fd & restores non-disruptive flags");

    /* Test 8b: WLAN RDID returns 000000 for first 3 attempts (slow power-up) -> retries and still succeeds */
    reset_m(&m);
    m.wlan_rdid_zero_count = 3;
    arm_trigger(prefix, "RB3_STG6_WLAN.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_STG6_WLAN.TRG");
    CHECK(rc == RB3B_OK && s.wlan.passed && m.wlan_rdid_calls == 4, "wlan jedec retry recovers");

    CHECK(m.retry_delay_calls == 3 && m.retry_delay_total_us == 60000u, "B3 retries wait 20 ms each");

    /* Test 8c: WLAN RDID permanently 000000 -> JEDEC error, 0 bytes written, 0 flash writes, safe unmount */
    reset_m(&m);
    m.wlan_rdid_zero_count = 1000;
    arm_trigger(prefix, "RB3_STG6_WLAN.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_STG6_WLAN.TRG");
    CHECK(rc == RB3B_ERR_JEDEC && !s.wlan.passed && s.wlan.bytes_written == 0u && m.storage_write_attempts == 0 &&
          s.sd_unmounted && m.open_fds == 0 && !s.ic553.executed, "wlan permanent jedec fail is safe");

    CHECK(m.retry_delay_calls == 10 && m.retry_delay_total_us == 200000u, "B3 failed probe delay budget is 200 ms");

    /* B5 completion failures: raw status preserved; no reset and no unsafe unmount. */
    reset_m(&m);
    m.fail_sd_sync = 1;
    arm_trigger(prefix, "RB3_STG6_WLAN.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_STG6_WLAN.TRG");
    CHECK(rc == RB5_ERR_SD_SYNC && s.backup_status == 0 && s.sd_sync_status == -31 && m.sd_unmount_calls == 0 && !rb4_restart_allowed(&s), "B5 sync fail stops restart");
    CHECK(file_contains(prefix,"RB5_COMPLETION.json","\"sd_sync_status\": -31"), "sync raw error persisted");
    CHECK(file_contains(prefix,"RB3_session_manifest.json","\"overall_status\": -15"), "sync failure manifest persisted");
    reset_m(&m);
    m.fail_sd_unmount = 1;
    arm_trigger(prefix, "RB3_STG6_WLAN.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_STG6_WLAN.TRG");
    CHECK(rc == RB5_ERR_SD_UNMOUNT && s.backup_status == 0 && s.sd_unmount_status == -47 && !rb4_restart_allowed(&s), "B5 unmount fail stops restart");
    CHECK(file_contains(prefix,"RB5_COMPLETION.json","\"sd_unmount_status\": -47"), "unmount raw error persisted");
    CHECK(file_contains(prefix,"RB3_session_manifest.json","\"overall_status\": -16"), "unmount failure manifest persisted");
    reset_m(&m);
    m.fail_completion_log = 1;
    arm_trigger(prefix, "RB3_STG6_WLAN.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_STG6_WLAN.TRG");
    CHECK(rc == RB5_ERR_COMPLETION_LOG && m.sd_unmount_calls == 0 && !rb4_restart_allowed(&s), "B5 receipt write failure stops restart");
    reset_m(&m);
    arm_trigger(prefix, "RB3_STG6_WLAN.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_STG6_WLAN.TRG");
    CHECK(rc == 0 && rb4_restart_allowed(&s), "B5 WLAN completion passes gate");
    { rb3b_session_state_t saved = s;
      s.sd_unmounted = 0; CHECK(!rb4_restart_allowed(&s), "no reset before unmount"); s = saved;
      s.sd_synced = 0; CHECK(!rb4_restart_allowed(&s), "no reset before sync"); s = saved;
      s.trigger_consumed = 0; CHECK(!rb4_restart_allowed(&s), "no reset with armed trigger"); s = saved;
      s.completion_status = -16; CHECK(!rb4_restart_allowed(&s), "no reset with completion error"); s = saved;
    }
    m.volume_unmounted = 0; /* simulated reboot: FS mounted, retained session unchanged */
    CHECK(rb4_write_completion_report(&hal,prefix,&s,1u) == 0, "B5 exports retained post-restart receipt");

    reset_m(&m);
    m.fail_sd_sync_call = 2;
    arm_trigger(prefix,"RB3_STG12.TRG");
    rc = rb3b_run_from_sd_triggers(&hal,prefix,&s);
    remove_file(prefix,"RB3_STG12.TRG");
    CHECK(rc == RB5_ERR_SD_SYNC && s.sd_sync_status == -31 && !s.sd_synced && s.runtime_state_restored, "late non-disruptive sync error propagated");
    CHECK(file_contains(prefix,"RB5_COMPLETION.json","\"sd_sync_status\": -31"), "late sync error persisted");

    reset_m(&m);
    m.fail_progress_sync = 1;
    arm_trigger(prefix,"RB3_STG6_WLAN.TRG");
    rc = rb3b_run_from_sd_triggers(&hal,prefix,&s);
    remove_file(prefix,"RB3_STG6_WLAN.TRG");
    CHECK(rc == RB5_ERR_COMPLETION_LOG && s.progress_log_status == -61 && !rb4_restart_allowed(&s) && m.sd_unmount_calls == 0, "progress sync failure inhibits restart");
    CHECK(file_contains(prefix,"RB5_PROGRESS_A.json","\"prior_log_status\": -61") || file_contains(prefix,"RB5_PROGRESS_B.json","\"prior_log_status\": -61"), "progress raw sync error persisted");

    /* Test 9: Final clean full backup output in base_dir for Python hashlib cross-check */
    reset_m(&m);
    arm_trigger(prefix, "RB3_FULL_BACKUP.TRG");
    rc = rb3b_run_from_sd_triggers(&hal, prefix, &s);
    remove_file(prefix, "RB3_FULL_BACKUP.TRG");
    CHECK(rc == RB3B_OK, "final clean full backup output");
    CHECK(m.max_open_fds == 1 && m.progress_sync_calls > 90, "progress checkpoints keep one open handle");
    CHECK(s.progress_event == RB5_EVT_RETURN && s.sd_unmounted, "post-unmount progress retained only in SRAM");
    CHECK(file_contains(prefix,"RB5_PROGRESS_A.json","\"event\": 92") || file_contains(prefix,"RB5_PROGRESS_B.json","\"event\": 92"), "last on-card checkpoint precedes unmount");

    CHECK(file_contains(prefix,"RB5_COMPLETION.json","\"post_restart_receipt\": false"), "preunmount receipt remains provisional");
    rb5_progress(&hal,prefix,&s,RB5_EVT_RESTART_GATE,0u,0);
    m.volume_unmounted = 0; /* host-only simulated reboot, not hardware evidence */
    CHECK(rb4_write_completion_report(&hal,prefix,&s,1u) == 0, "final synthetic reboot receipt");
    CHECK(rb5_progress_export(&hal,prefix,&s,1u) == 0, "export retained progress on simulated reboot");

    printf("RB3_BACKUP_STATE_MACHINE_TESTS_PASS: 17/17 suites passed (strict 9-stage order, oneshot consumed, ENOSPC safe, IC553 last, 0 writes, max_fd=1, B5 completion failure gates)\n");
    free(m.ic704); free(m.ic705); free(m.ic706); free(m.wlan); free(m.ic553);
    return 0;
}
