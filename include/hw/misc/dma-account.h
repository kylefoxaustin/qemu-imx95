/*
 * Device-initiated DMA accounting.
 *
 * The TCG plugin API is scoped entirely to vCPU execution -- every callback is
 * qemu_plugin_register_vcpu_*, and the memory callback takes a CPUState. A DMA
 * access has no vCPU to attribute it to, so peripheral traffic is invisible to
 * it by construction. That matters for activity-factor work: a DPU scanning
 * out 1920x1200 at 60 Hz moves 0.55 GB/s that the CPU never issues, which is
 * more than the busiest application in our 50-app corpus.
 *
 * This is the device-side stopgap: models call dma_account() at their DMA call
 * sites and the totals are printed at exit. The record format is deliberately
 * the one a future plugin-based DMA callback should emit, so the measurement
 * methodology does not change when the accounting moves into the plugin.
 *
 * TRANSACTIONS AS WELL AS BYTES. An interconnect or DRAM model cannot work
 * from bytes alone: 1 MiB moved as one descriptor is not the same load as
 * 1 MiB moved as 16384 of them. Each dma_account() call counts as one
 * transaction, so the per-row ratio gives the mean burst size.
 *
 * WINDOWED OUTPUT. Exit-only totals average away the thing an activity factor
 * is meant to capture. Set QEMU_DMA_ACCOUNT_CSV to also emit a per-window CSV
 * on guest-time intervals, which is what an offline model consumes.
 *
 * THE TOTAL IS A LOWER BOUND, NEVER A TOTAL. Only instrumented call sites are
 * counted, and CPU load/store traffic is not counted at all -- guest RAM is
 * reached through a direct host pointer (memory_access_is_direct() in
 * system/physmem.c), so no callback runs for it. An un-instrumented master is
 * missing data, not an idle master.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_DMA_ACCOUNT_H
#define HW_MISC_DMA_ACCOUNT_H

/*
 * Record `bytes` moved by device `dev` on logical channel `chan`, as one
 * transaction. `dev` and `chan` must be string literals or otherwise outlive
 * the run. No-op unless accounting is enabled (QEMU_DMA_ACCOUNT=1).
 */
void dma_account(const char *dev, const char *chan, bool is_write,
                 uint64_t bytes);

/* True when accounting is on; lets hot paths skip argument setup. */
bool dma_account_enabled(void);

/* Print accumulated totals. Called automatically at exit. */
void dma_account_report(void);

#endif /* HW_MISC_DMA_ACCOUNT_H */
