/*
 * Device-initiated DMA accounting. See include/hw/misc/dma-account.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "hw/misc/dma-account.h"

#define DMA_ACCOUNT_MAX 32
#define DMA_ACCOUNT_DEFAULT_WINDOW_MS 100

typedef struct {
    const char *dev;
    const char *chan;
    uint64_t rd;            /* bytes read by the device  */
    uint64_t wr;            /* bytes written by the device */
    uint64_t rd_txns;       /* read transactions  */
    uint64_t wr_txns;       /* write transactions */
    /* Snapshot at the end of the previous CSV window. Main loop only. */
    uint64_t p_rd, p_wr, p_rd_txns, p_wr_txns;
} DmaAccountEntry;

static DmaAccountEntry entries[DMA_ACCOUNT_MAX];
static int n_entries;                    /* only grows; read without the lock */
static int enabled = -1;                 /* -1 = not yet probed */
static bool exit_registered;

/*
 * Guards entry *creation* only. Accumulation is atomic and lock-free because
 * it is hot: a 10G ENETC run accounts on the order of a million descriptors a
 * second, while new rows appear a handful of times per run.
 *
 * A zero-initialised static GMutex is valid in GLib, which avoids needing an
 * init hook to run before the first DMA.
 */
static GMutex entry_lock;

/* Windowed CSV (optional). Created lazily on the first accounted txn. */
static FILE *csv;
static QEMUTimer *csv_timer;
static int64_t csv_window_ns = DMA_ACCOUNT_DEFAULT_WINDOW_MS * 1000000LL;
static int64_t csv_t0_ns;
static bool csv_tried;

bool dma_account_enabled(void)
{
    if (enabled < 0) {
        const char *e = getenv("QEMU_DMA_ACCOUNT");
        enabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return enabled == 1;
}

static void dma_account_csv_tick(void *opaque)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double secs = (double)csv_window_ns / 1e9;
    int n = qatomic_read(&n_entries);
    int i;

    for (i = 0; i < n; i++) {
        DmaAccountEntry *e = &entries[i];
        uint64_t rd = qatomic_read(&e->rd), wr = qatomic_read(&e->wr);
        uint64_t rt = qatomic_read(&e->rd_txns), wt = qatomic_read(&e->wr_txns);
        uint64_t d_rd = rd - e->p_rd, d_wr = wr - e->p_wr;
        uint64_t d_rt = rt - e->p_rd_txns, d_wt = wt - e->p_wr_txns;

        e->p_rd = rd; e->p_wr = wr;
        e->p_rd_txns = rt; e->p_wr_txns = wt;

        /* One row per direction, and only for directions that moved. */
        if (d_rt) {
            fprintf(csv, "%" PRId64 ",%s,%s,read,%" PRIu64 ",%" PRIu64
                    ",%.0f\n", (now - csv_t0_ns) / 1000000, e->dev, e->chan,
                    d_rt, d_rd, (double)d_rd / secs);
        }
        if (d_wt) {
            fprintf(csv, "%" PRId64 ",%s,%s,write,%" PRIu64 ",%" PRIu64
                    ",%.0f\n", (now - csv_t0_ns) / 1000000, e->dev, e->chan,
                    d_wt, d_wr, (double)d_wr / secs);
        }
    }
    fflush(csv);
    timer_mod(csv_timer, now + csv_window_ns);
}

/* Kept out of the hot path: runs once, under entry_lock. */
static void dma_account_csv_open(void)
{
    const char *spec = getenv("QEMU_DMA_ACCOUNT_CSV");
    g_autofree char *path = NULL;
    char *colon;

    csv_tried = true;
    if (!spec || !*spec) {
        return;
    }

    path = g_strdup(spec);
    colon = strrchr(path, ':');
    if (colon) {
        int ms = atoi(colon + 1);

        if (ms > 0) {
            csv_window_ns = (int64_t)ms * 1000000LL;
            *colon = '\0';
        }
    }

    csv = fopen(path, "w");
    if (!csv) {
        error_report("dma-account: cannot open %s: %s", path, strerror(errno));
        return;
    }
    fprintf(csv, "guest_ms,dev,chan,dir,txns,bytes,bytes_per_s\n");
    fflush(csv);

    csv_t0_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    csv_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dma_account_csv_tick, NULL);
    timer_mod(csv_timer, csv_t0_ns + csv_window_ns);
    info_report("dma-account: windowed CSV to %s, window %" PRId64 " ms "
                "(device DMA only; CPU traffic is not counted)",
                path, csv_window_ns / 1000000);
}

void dma_account(const char *dev, const char *chan, bool is_write,
                 uint64_t bytes)
{
    DmaAccountEntry *slot = NULL;
    int i, n;

    if (!dma_account_enabled() || bytes == 0) {
        return;
    }

    /*
     * Fast path: find an existing row without taking the lock. n_entries only
     * ever grows and a row's dev/chan are set before it is published, so a
     * reader either does not see the row yet (and falls into the slow path) or
     * sees it fully initialised.
     */
    n = qatomic_read(&n_entries);
    for (i = 0; i < n; i++) {
        /* Pointer comparison is enough: callers pass string literals. */
        if (entries[i].dev == dev && entries[i].chan == chan) {
            slot = &entries[i];
            break;
        }
    }

    if (!slot) {
        g_mutex_lock(&entry_lock);
        /* Re-check: another thread may have created it while we waited. */
        n = qatomic_read(&n_entries);
        for (i = 0; i < n; i++) {
            if (entries[i].dev == dev && entries[i].chan == chan) {
                slot = &entries[i];
                break;
            }
        }
        if (!slot) {
            if (n == DMA_ACCOUNT_MAX) {
                g_mutex_unlock(&entry_lock);
                return;                  /* table full: drop rather than grow */
            }
            entries[n].dev = dev;
            entries[n].chan = chan;
            slot = &entries[n];
            qatomic_set(&n_entries, n + 1);   /* publish after init */
        }
        if (!exit_registered) {
            exit_registered = true;
            atexit(dma_account_report);
        }
        if (!csv_tried) {
            dma_account_csv_open();
        }
        g_mutex_unlock(&entry_lock);
    }

    /*
     * Atomic because initiators really are concurrent: a DPU frame tick runs
     * on the main loop while an ENETC transmit can be driven from a vCPU
     * thread under MTTCG. Plain increments here would silently under-count.
     */
    if (is_write) {
        qatomic_add(&slot->wr, bytes);
        qatomic_add(&slot->wr_txns, 1);
    } else {
        qatomic_add(&slot->rd, bytes);
        qatomic_add(&slot->rd_txns, 1);
    }
}

void dma_account_report(void)
{
    uint64_t trd = 0, twr = 0, trt = 0, twt = 0;
    int n = qatomic_read(&n_entries);
    int i;

    if (!dma_account_enabled() || n == 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        DmaAccountEntry *e = &entries[i];
        uint64_t rd = qatomic_read(&e->rd), wr = qatomic_read(&e->wr);
        uint64_t rt = qatomic_read(&e->rd_txns), wt = qatomic_read(&e->wr_txns);

        fprintf(stderr, "wattson-dma: dev=%s chan=%s rd=%" PRIu64
                " wr=%" PRIu64 " rd_txns=%" PRIu64 " wr_txns=%" PRIu64 "\n",
                e->dev, e->chan, rd, wr, rt, wt);
        trd += rd; twr += wr; trt += rt; twt += wt;
    }
    fprintf(stderr, "wattson-dma: TOTAL rd=%" PRIu64 " wr=%" PRIu64
            " bytes=%" PRIu64 " txns=%" PRIu64 "\n",
            trd, twr, trd + twr, trt + twt);
    fprintf(stderr, "wattson-dma: NOTE this is a lower bound - only "
            "instrumented DMA sites are counted and CPU traffic is not "
            "counted at all; an absent master is missing data, not an idle "
            "master.\n");
    if (csv) {
        fflush(csv);
    }
}
