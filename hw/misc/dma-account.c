/*
 * Device-initiated DMA byte accounting. See include/hw/misc/dma-account.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/misc/dma-account.h"

#define DMA_ACCOUNT_MAX 32

typedef struct {
    const char *dev;
    const char *chan;
    uint64_t rd;
    uint64_t wr;
} DmaAccountEntry;

static DmaAccountEntry entries[DMA_ACCOUNT_MAX];
static int n_entries;
static int enabled = -1;                 /* -1 = not yet probed */
static bool exit_registered;

bool dma_account_enabled(void)
{
    if (enabled < 0) {
        const char *e = getenv("QEMU_DMA_ACCOUNT");
        enabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return enabled == 1;
}

void dma_account(const char *dev, const char *chan, bool is_write,
                 uint64_t bytes)
{
    DmaAccountEntry *slot = NULL;
    int i;

    if (!dma_account_enabled() || bytes == 0) {
        return;
    }

    /*
     * Linear scan: the table holds one row per (device, channel) pair, of
     * which there are a handful. Pointer comparison is enough because callers
     * pass string literals.
     */
    for (i = 0; i < n_entries; i++) {
        if (entries[i].dev == dev && entries[i].chan == chan) {
            slot = &entries[i];
            break;
        }
    }
    if (!slot) {
        if (n_entries == DMA_ACCOUNT_MAX) {
            return;                      /* table full: drop rather than grow */
        }
        slot = &entries[n_entries++];
        slot->dev = dev;
        slot->chan = chan;
    }
    if (!exit_registered) {
        exit_registered = true;
        atexit(dma_account_report);
    }
    if (is_write) {
        slot->wr += bytes;
    } else {
        slot->rd += bytes;
    }
}

void dma_account_report(void)
{
    uint64_t trd = 0, twr = 0;
    int i;

    if (!dma_account_enabled() || n_entries == 0) {
        return;
    }
    for (i = 0; i < n_entries; i++) {
        fprintf(stderr, "wattson-dma: dev=%s chan=%s rd=%" PRIu64
                " wr=%" PRIu64 "\n",
                entries[i].dev, entries[i].chan, entries[i].rd, entries[i].wr);
        trd += entries[i].rd;
        twr += entries[i].wr;
    }
    fprintf(stderr, "wattson-dma: TOTAL rd=%" PRIu64 " wr=%" PRIu64
            " bytes=%" PRIu64 "\n", trd, twr, trd + twr);
}
