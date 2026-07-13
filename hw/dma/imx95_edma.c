/*
 * NXP i.MX 95 eDMA v3 (fsl,imx95-edma3) controller
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register/TCD layout follows the Linux fsl-edma driver (drivers/dma/
 * fsl-edma-common.h, imx95_data3). Channel N's page is at base + (N+1)*0x10000
 * and contains the channel control registers followed by a 32-byte TCD at
 * offset 0x20.
 *
 * A transfer is executed when the driver sets CH_CSR.ERQ. Real hardware paces
 * each minor loop off a peripheral DMA request; here we run the whole TCD at
 * once. To preserve ordering for peripherals that need command words pushed
 * before data is read back (e.g. the LPI2C: a TX channel writes RECV_DATA
 * commands to MTDR, then an RX channel drains MRDR), a memory->device (TX)
 * channel runs immediately and then drains any device->memory (RX) channel
 * that was armed first. Direction comes from CH_SBR (RD = rx, WR = tx).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/dma/imx95_edma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Per-channel control register offsets (struct fsl_edma3_ch_reg). */
#define CH_CSR      0x00
#define CH_ES       0x04
#define CH_INT      0x08
#define CH_SBR      0x0c
#define CH_PRI      0x10
#define CH_MUX      0x14
#define CH_MATTR    0x18
/*
 * TCD field offsets within the channel page. eDMA v3 ("fsl,imx93-edma3") uses a
 * 32-bit TCD (struct fsl_edma_hw_tcd); eDMA v5 ("fsl,imx95-edma5") a 64-bit TCD
 * (struct fsl_edma_hw_tcd64) with 64-bit SADDR/DADDR/SLAST/DLAST_SGA, so SOFF
 * and every later field shift. The layout is selected per controller via
 * s->tcd64; reading the wrong one misses TCD_CSR.ESG and the channel is never
 * recognised as cyclic (audio capture/playback then never advances).
 */
typedef struct {
    unsigned saddr, soff, attr, nbytes, slast, daddr, dlast, doff, citer, csr,
             biter;
    bool a64;       /* saddr/daddr/slast/dlast_sga are 64-bit */
} EdmaTcdLayout;

static const EdmaTcdLayout tcd_v3 = {
    .saddr = 0x20, .soff = 0x24, .attr = 0x26, .nbytes = 0x28, .slast = 0x2c,
    .daddr = 0x30, .dlast = 0x38, .doff = 0x34, .citer = 0x36, .csr = 0x3c,
    .biter = 0x3e, .a64 = false,
};
static const EdmaTcdLayout tcd_v5 = {
    .saddr = 0x20, .soff = 0x28, .attr = 0x2a, .nbytes = 0x2c, .slast = 0x30,
    .daddr = 0x38, .dlast = 0x40, .doff = 0x48, .citer = 0x4a, .csr = 0x4c,
    .biter = 0x4e, .a64 = true,
};
#define TCD(s)  ((s)->tcd64 ? &tcd_v5 : &tcd_v3)

#define CH_CSR_ERQ      (1u << 0)
#define CH_CSR_EARQ     (1u << 1)
#define CH_CSR_EEI      (1u << 2)
#define CH_CSR_DONE     (1u << 30)
#define CH_CSR_ACTIVE   (1u << 31)

#define CH_SBR_WR       (1u << 21)      /* tx: memory -> device */
#define CH_SBR_RD       (1u << 22)      /* rx: device -> memory */

#define TCD_CSR_INTMAJ  (1u << 1)       /* interrupt on major-loop complete */
#define TCD_CSR_ESG     (1u << 4)       /* enable scatter/gather (cyclic)   */

#define NBYTES_SMLOE    (1u << 31)      /* source minor-loop offset enable  */
#define NBYTES_DMLOE    (1u << 30)      /* dest minor-loop offset enable    */

#define ATTR_DSIZE(a)   ((a) & 0x7)
#define ATTR_SSIZE(a)   (((a) >> 8) & 0x7)
#define ITER_MASK       0x7fff

static inline uint32_t ld32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t ld16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static inline void st32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static inline uint64_t ld64(const uint8_t *p)
{
    return ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

static inline void st64(uint8_t *p, uint64_t v)
{
    st32(p, (uint32_t)v); st32(p + 4, (uint32_t)(v >> 32));
}

/* Read/write a SADDR/DADDR-class address honouring the 32/64-bit TCD layout. */
static inline uint64_t ld_addr(const EdmaTcdLayout *L, const uint8_t *p)
{
    return L->a64 ? ld64(p) : ld32(p);
}

static inline void st_addr(const EdmaTcdLayout *L, uint8_t *p, uint64_t v)
{
    if (L->a64) {
        st64(p, v);
    } else {
        st32(p, (uint32_t)v);
    }
}

/*
 * Decode the TCD NBYTES word. With the minor-loop offset enabled (SMLOE or
 * DMLOE) the byte count is only bits [9:0] and bits [29:10] are a signed offset
 * (returned via *mloff) added to SADDR/DADDR once per minor loop; otherwise the
 * count is bits [29:0]. MICFIL multichannel capture sets SMLOE with a negative
 * MLOFF to rewind SADDR after reading DATACH0..n.
 */
static uint32_t edma_decode_nbytes(uint32_t raw, int32_t *mloff)
{
    if (raw & (NBYTES_SMLOE | NBYTES_DMLOE)) {
        *mloff = (int32_t)(raw << 2) >> 12;     /* sign-extend bits [29:10] */
        return raw & 0x3ff;
    }
    *mloff = 0;
    return raw & 0x3fffffff;
}

/*
 * Cap a single channel kick's total element transfers. The transfer below runs
 * synchronously in the MMIO write handler, one element (esize bytes) per loop,
 * so an arbitrarily large guest-programmed CITER * (NBYTES / esize) would spin
 * QEMU's main loop and hang the VM. 64M elements is far above any realistic
 * single-kick eDMA transfer while bounding the worst-case loop to well under a
 * second; refuse a larger descriptor rather than hang.
 */
#define IMX95_EDMA_MAX_XFER_ELEMS (64u * 1024 * 1024)

/*
 * Mark a channel's major loop complete: persist the (advanced) pointers, read
 * CITER back as 0 so the driver computes a full real count, clear ERQ/ACTIVE,
 * set DONE, and raise the channel interrupt.
 */
static void edma_finish(IMX95EdmaState *s, int ch, uint64_t saddr,
                        uint64_t daddr)
{
    uint8_t *t = s->chan[ch].regs;
    const EdmaTcdLayout *L = TCD(s);
    uint32_t csr;

    st_addr(L, t + L->saddr, saddr);
    st_addr(L, t + L->daddr, daddr);
    t[L->citer] = 0;
    t[L->citer + 1] = 0;

    csr = ld32(t + CH_CSR);
    csr &= ~(CH_CSR_ERQ | CH_CSR_ACTIVE);
    csr |= CH_CSR_DONE;
    st32(t + CH_CSR, csr);

    st32(t + CH_INT, 1);
    if (ch < s->num_channels) {
        qemu_irq_raise(s->irq[ch]);
    }
}

/* Execute a channel's TCD: move CITER * NBYTES bytes SADDR->DADDR. */
static void edma_run_channel(IMX95EdmaState *s, int ch)
{
    IMX95EdmaChan *c = &s->chan[ch];
    uint8_t *t = c->regs;
    const EdmaTcdLayout *L = TCD(s);
    uint64_t saddr = ld_addr(L, t + L->saddr);
    uint64_t daddr = ld_addr(L, t + L->daddr);
    int16_t soff = (int16_t)ld16(t + L->soff);
    int16_t doff = (int16_t)ld16(t + L->doff);
    uint16_t attr = ld16(t + L->attr);
    uint32_t nb_raw = ld32(t + L->nbytes);
    uint16_t citer = ld16(t + L->citer) & ITER_MASK;
    uint32_t ssize = 1u << ATTR_SSIZE(attr);
    uint32_t dsize = 1u << ATTR_DSIZE(attr);
    uint32_t esize = MAX(ssize, dsize);
    int32_t mloff;
    uint32_t nbytes = edma_decode_nbytes(nb_raw, &mloff);
    bool smloe = nb_raw & NBYTES_SMLOE;
    bool dmloe = nb_raw & NBYTES_DMLOE;
    uint8_t buf[8];

    if (citer == 0) {
        citer = 1;
    }
    if (nbytes == 0 || esize == 0 || esize > sizeof(buf)) {
        return;
    }
    if ((uint64_t)citer * (nbytes / esize) > IMX95_EDMA_MAX_XFER_ELEMS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "imx95-edma: ch%d transfer too large (citer=%u * "
                      "nbytes=%u / esize=%u elements); refusing to avoid a VM "
                      "hang\n", ch, citer, nbytes, esize);
        return;
    }

    /*
     * Transfer element by element so that fixed peripheral addresses (SOFF or
     * DOFF == 0) are hit with the access width the device register expects,
     * while the memory side walks linearly. After each minor loop apply the
     * minor-loop offset to the configured side(s).
     */
    for (uint16_t major = 0; major < citer; major++) {
        for (uint32_t done = 0; done + esize <= nbytes; done += esize) {
            address_space_read(&address_space_memory, saddr,
                               MEMTXATTRS_UNSPECIFIED, buf, esize);
            address_space_write(&address_space_memory, daddr,
                                MEMTXATTRS_UNSPECIFIED, buf, esize);
            saddr += soff;
            daddr += doff;
        }
        if (smloe) {
            saddr += mloff;
        }
        if (dmloe) {
            daddr += mloff;
        }
    }

    /* Completion: channel done, request disabled, interrupt pending. */
    edma_finish(s, ch, saddr, daddr);
}

/*
 * Decode the linear (non-cyclic, no minor-loop-offset) element walk of a
 * channel's TCD - the common shape for a peripheral TX or RX slave transfer.
 * Returns the number of ESIZE elements to move, and the walk parameters. NULL
 * out / a false return when the TCD is cyclic or uses a minor-loop offset
 * go through edma_run_channel / edma_service_minor unchanged).
 */
typedef struct {
    uint64_t saddr, daddr;
    int16_t soff, doff;
    uint32_t esize;
    uint64_t elems;
} EdmaWalk;

static bool edma_decode_walk(IMX95EdmaState *s, int ch, EdmaWalk *w)
{
    uint8_t *t = s->chan[ch].regs;
    const EdmaTcdLayout *L = TCD(s);
    uint32_t nb_raw = ld32(t + L->nbytes);
    uint16_t attr = ld16(t + L->attr);
    uint16_t citer = ld16(t + L->citer) & ITER_MASK;
    uint32_t ssize = 1u << ATTR_SSIZE(attr);
    uint32_t dsize = 1u << ATTR_DSIZE(attr);
    int32_t mloff;
    uint32_t nbytes = edma_decode_nbytes(nb_raw, &mloff);

    w->esize = MAX(ssize, dsize);
    if (nb_raw & (NBYTES_SMLOE | NBYTES_DMLOE)) {
        return false;               /* minor-loop offset: not this path */
    }
    if (citer == 0) {
        citer = 1;
    }
    if (w->esize == 0 || nbytes == 0 || nbytes % w->esize) {
        return false;
    }
    w->saddr = ld_addr(L, t + L->saddr);
    w->daddr = ld_addr(L, t + L->daddr);
    w->soff = (int16_t)ld16(t + L->soff);
    w->doff = (int16_t)ld16(t + L->doff);
    w->elems = (uint64_t)citer * (nbytes / w->esize);
    return w->elems <= IMX95_EDMA_MAX_XFER_ELEMS;
}

/*
 * Run a memory->peripheral (TX) channel byte-locked with a peripheral->memory
 * (RX) channel, one element each in turn - the way a full-duplex peripheral
 * (LPSPI) paces TX and RX DMA requests off the same shifted word: each
 * word shifted out produces one word shifted in. Running TX to completion
 * first would push the whole transfer into the device's small RX FIFO
 * (16 deep on imx95_lpspi), overflow it, and the RX drain would then read
 * stale/empty data past the FIFO depth. Interleaving keeps one element
 * flight, so any transfer length crosses intact.
 *
 * ONLY for the byte-locked case: TX and RX must move the same element count.
 * That holds for SPI (one rx word per tx word) and for a pure LPI2C RXDATA run
 * (each MTDR RECV_DATA command synchronously pushes one MRDR byte). A command
 * stream where some tx words produce no rx data - an LPI2C sequence carrying
 * START/STOP, tx count != rx count - is NOT byte-locked; those keep the ordered
 * TX-then-RX drain, the shape hw/i2c/imx_lpi2c relies on.
 */
static void edma_run_txrx(IMX95EdmaState *s, int tx, int rx)
{
    EdmaWalk tw, rw;
    uint64_t i;
    uint8_t buf[8];

    if (!edma_decode_walk(s, tx, &tw) || !edma_decode_walk(s, rx, &rw) ||
        tw.esize > sizeof(buf) || rw.esize > sizeof(buf) ||
        tw.elems != rw.elems) {
        /* Not a byte-locked pair: keep the ordered TX-then-RX drain. */
        edma_run_channel(s, tx);
        edma_run_channel(s, rx);
        return;
    }

    for (i = 0; i < tw.elems; i++) {
        /* one TX element: mem -> device (fills the device RX FIFO by one) */
        address_space_read(&address_space_memory, tw.saddr,
                           MEMTXATTRS_UNSPECIFIED, buf, tw.esize);
        address_space_write(&address_space_memory, tw.daddr,
                            MEMTXATTRS_UNSPECIFIED, buf, tw.esize);
        tw.saddr += tw.soff;
        tw.daddr += tw.doff;
        /* one RX element: device -> mem (drains the byte just produced) */
        address_space_read(&address_space_memory, rw.saddr,
                           MEMTXATTRS_UNSPECIFIED, buf, rw.esize);
        address_space_write(&address_space_memory, rw.daddr,
                            MEMTXATTRS_UNSPECIFIED, buf, rw.esize);
        rw.saddr += rw.soff;
        rw.daddr += rw.doff;
    }
    edma_finish(s, tx, tw.saddr, tw.daddr);
    edma_finish(s, rx, rw.saddr, rw.daddr);
}

/*
 * Service one minor loop of a cyclic (scatter/gather) channel - the way audio
 * capture runs: the peripheral (MICFIL) requests a burst as its FIFO fills, and
 * each request moves NBYTES from the peripheral's data register to the ring
 * buffer. At the end of a major loop (one period) the channel raises its
 * interrupt, then follows DLAST_SGA to the next period's TCD - looping until
 * the driver clears ERQ. This keeps the transfer paced by the peripheral
 * instead of running the whole ring at once.
 */
static void edma_service_minor(IMX95EdmaState *s, int ch)
{
    IMX95EdmaChan *c = &s->chan[ch];
    uint8_t *t = c->regs;
    const EdmaTcdLayout *L = TCD(s);
    uint64_t saddr = ld_addr(L, t + L->saddr);
    uint64_t daddr = ld_addr(L, t + L->daddr);
    int16_t soff = (int16_t)ld16(t + L->soff);
    int16_t doff = (int16_t)ld16(t + L->doff);
    uint16_t attr = ld16(t + L->attr);
    uint32_t nb_raw = ld32(t + L->nbytes);
    int32_t mloff;
    uint32_t nbytes = edma_decode_nbytes(nb_raw, &mloff);
    uint16_t citer = ld16(t + L->citer) & ITER_MASK;
    uint16_t biter = ld16(t + L->biter) & ITER_MASK;
    uint16_t csr = ld16(t + L->csr);
    uint32_t ssize = 1u << ATTR_SSIZE(attr);
    uint32_t dsize = 1u << ATTR_DSIZE(attr);
    uint32_t esize = MAX(ssize, dsize);
    uint8_t buf[8];

    if (!(ld32(t + CH_CSR) & CH_CSR_ERQ)) {
        return;                 /* stream stopped between request and service */
    }
    if (nbytes == 0 || esize == 0 || esize > sizeof(buf)) {
        return;
    }

    /* One minor loop: NBYTES, element by element (peripheral side is fixed). */
    for (uint32_t done = 0; done + esize <= nbytes; done += esize) {
        address_space_read(&address_space_memory, saddr,
                           MEMTXATTRS_UNSPECIFIED, buf, esize);
        address_space_write(&address_space_memory, daddr,
                            MEMTXATTRS_UNSPECIFIED, buf, esize);
        saddr += soff;
        daddr += doff;
    }
    /* Apply the minor-loop offset to whichever side enabled it. */
    if (nb_raw & NBYTES_SMLOE) {
        saddr += mloff;
    }
    if (nb_raw & NBYTES_DMLOE) {
        daddr += mloff;
    }
    /*
     * Persist both pointers. The fixed (peripheral) side has off == 0 so it is
     * unchanged; the advancing (memory) side must carry over to the next minor
     * loop, or every minor loop would overwrite the same ring bytes.
     */
    st_addr(L, t + L->saddr, saddr);
    st_addr(L, t + L->daddr, daddr);

    if (citer > 1) {
        citer--;
        t[L->citer] = citer;
        t[L->citer + 1] = citer >> 8;
        return;
    }

    /* Major loop complete: one period delivered. */
    if (csr & TCD_CSR_INTMAJ) {
        st32(t + CH_INT, 1);
        if (ch < s->num_channels) {
            qemu_irq_raise(s->irq[ch]);
        }
    }
    if (csr & TCD_CSR_ESG) {
        /*
         * Follow DLAST_SGA to the next period's TCD. The descriptor is the
         * whole TCD (0x20 bytes for the 32-bit layout, 0x30 for the 64-bit
         * one) and is loaded over the live TCD starting at SADDR.
         */
        uint64_t next = ld_addr(L, t + L->dlast);
        unsigned tcdsz = L->a64 ? 0x30 : 0x20;
        address_space_read(&address_space_memory, next, MEMTXATTRS_UNSPECIFIED,
                           t + L->saddr, tcdsz);
    } else {
        t[L->citer] = biter;
        t[L->citer + 1] = biter >> 8;
    }
}

/* A peripheral DMA request: advance the cyclic channel that serves it. */
static void edma_dma_request(void *opaque, int n, int level)
{
    IMX95EdmaState *s = opaque;
    int i;

    if (!level) {
        return;
    }
    for (i = 0; i < s->num_channels; i++) {
        IMX95EdmaChan *c = &s->chan[i];

        if (c->cyclic && (ld32(c->regs + CH_CSR) & CH_CSR_ERQ)) {
            edma_service_minor(s, i);
            return;
        }
    }
}

static void edma_drain_armed_rx(IMX95EdmaState *s)
{
    for (int i = 0; i < s->num_channels; i++) {
        if (s->chan[i].armed) {
            s->chan[i].armed = false;
            edma_run_channel(s, i);
        }
    }
}

static int edma_find_armed_rx(IMX95EdmaState *s)
{
    for (int i = 0; i < s->num_channels; i++) {
        if (s->chan[i].armed) {
            return i;
        }
    }
    return -1;
}

/* CH_CSR was written with ERQ set: arm or run the channel. */
static void edma_trigger(IMX95EdmaState *s, int ch)
{
    IMX95EdmaChan *c = &s->chan[ch];
    const EdmaTcdLayout *L = TCD(s);
    uint32_t sbr = ld32(c->regs + CH_SBR);
    int16_t soff = (int16_t)ld16(c->regs + L->soff);
    bool is_rx;

    /*
     * Scatter/gather (ESG) means a cyclic, peripheral-paced transfer (audio):
     * don't run it now, wait for the peripheral's DMA requests to drive it one
     * minor loop at a time. Non-SG transfers keep the run-it-now behaviour.
     */
    if (ld16(c->regs + L->csr) & TCD_CSR_ESG) {
        c->cyclic = true;
        return;
    }
    c->cyclic = false;

    /* Prefer CH_SBR direction; fall back to "source offset fixed" = rx. */
    if (sbr & (CH_SBR_RD | CH_SBR_WR)) {
        is_rx = sbr & CH_SBR_RD;
    } else {
        is_rx = (soff == 0);
    }

    if (is_rx) {
        /* device -> memory: defer until the peripheral has data (a tx run). */
        c->armed = true;
    } else {
        /*
         * memory -> device. If an rx channel is armed for the same peripheral
         * (the fsl-lpspi driver issues rx before tx), interleave them so each
         * shifted word's tx-fill is drained immediately - a full-duplex device
         * paces tx/rx off one word, and its rx FIFO is small (16). Running tx
         * to completion first would overflow that FIFO and the rx drain would
         * reads empty past it. No armed rx: a plain tx (or mem->mem).
         */
        int rx = edma_find_armed_rx(s);

        if (rx >= 0) {
            s->chan[rx].armed = false;
            edma_run_txrx(s, ch, rx);
        } else {
            edma_run_channel(s, ch);
        }
        edma_drain_armed_rx(s);
    }
}

static int edma_channel_of(IMX95EdmaState *s, hwaddr offset)
{
    if (offset < IMX95_EDMA_CHAN_OFFSET) {
        return -1;
    }
    return (offset - IMX95_EDMA_CHAN_OFFSET) / s->chan_stride;
}

static uint64_t imx95_edma_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95EdmaState *s = opaque;
    int ch = edma_channel_of(s, offset);
    uint64_t val = 0;

    if (ch < 0) {
        uint32_t idx = offset >> 2;
        return idx < IMX95_EDMA_MGMT_REGS ? s->mgmt[idx] : 0;
    }
    if (ch >= s->num_channels) {
        return 0;
    }

    hwaddr coff = (offset - IMX95_EDMA_CHAN_OFFSET) % s->chan_stride;
    if (coff + size <= IMX95_EDMA_CHAN_REGS_SZ) {
        for (unsigned i = 0; i < size; i++) {
            val |= (uint64_t)s->chan[ch].regs[coff + i] << (8 * i);
        }
    }
    return val;
}

static void imx95_edma_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    IMX95EdmaState *s = opaque;
    int ch = edma_channel_of(s, offset);

    if (ch < 0) {
        uint32_t idx = offset >> 2;
        if (idx < IMX95_EDMA_MGMT_REGS) {
            s->mgmt[idx] = value;
        }
        return;
    }
    if (ch >= s->num_channels) {
        return;
    }

    hwaddr coff = (offset - IMX95_EDMA_CHAN_OFFSET) % s->chan_stride;
    if (coff + size > IMX95_EDMA_CHAN_REGS_SZ) {
        return;
    }

    /* CH_INT is write-1-to-clear; also drops the channel interrupt line. */
    if (coff == CH_INT && size >= 4) {
        if (value & 1) {
            st32(s->chan[ch].regs + CH_INT, 0);
            qemu_irq_lower(s->irq[ch]);
        }
        return;
    }

    for (unsigned i = 0; i < size; i++) {
        s->chan[ch].regs[coff + i] = (value >> (8 * i)) & 0xff;
    }

    /* Writing CH_CSR with ERQ set kicks the channel. */
    if (coff == CH_CSR) {
        uint32_t csr = ld32(s->chan[ch].regs + CH_CSR);
        if (csr & CH_CSR_ERQ) {
            edma_trigger(s, ch);
        }
    }
}

static const MemoryRegionOps imx95_edma_ops = {
    .read = imx95_edma_read,
    .write = imx95_edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void imx95_edma_reset(DeviceState *dev)
{
    IMX95EdmaState *s = IMX95_EDMA(dev);

    memset(s->mgmt, 0, sizeof(s->mgmt));
    for (int i = 0; i < IMX95_EDMA_MAX_CHANNELS; i++) {
        memset(s->chan[i].regs, 0, sizeof(s->chan[i].regs));
        /*
         * CH_SBR is NOT reset to zero on silicon: it carries the channel's bus
         * master ID (MID) and privileged-access level (PAL). The RM gives
         * eDMA3/eDMA5 a 0x0000_8007 reset (PAL=1, MID=7). This matters because
         * fsl-edma read-modify-writes CH_SBR to set the RD/WR direction bit -
         * so a zero reset here is laundered into the guest's own config as
         * MID=0, non-privileged, wrong on real silicon. Nothing enforces MID
         * in QEMU, so it is silent here - exactly the class this seed fixes.
         */
        st32(s->chan[i].regs + CH_SBR, s->ch_sbr_reset);
        s->chan[i].armed = false;
        s->chan[i].cyclic = false;
        if (i < s->num_channels) {
            qemu_irq_lower(s->irq[i]);
        }
    }
}

static void imx95_edma_realize(DeviceState *dev, Error **errp)
{
    IMX95EdmaState *s = IMX95_EDMA(dev);
    uint64_t region_sz;

    if (s->num_channels == 0 || s->num_channels > IMX95_EDMA_MAX_CHANNELS) {
        error_setg(errp, "imx95.edma3: invalid num-channels %u",
                   s->num_channels);
        return;
    }

    if (s->chan_stride < IMX95_EDMA_CHAN_REGS_SZ) {
        error_setg(errp, "imx95.edma3: invalid chan-stride 0x%x",
                   s->chan_stride);
        return;
    }

    region_sz = IMX95_EDMA_CHAN_OFFSET +
                (uint64_t)s->num_channels * s->chan_stride;
    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_edma_ops, s,
                          TYPE_IMX95_EDMA, region_sz);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    for (int i = 0; i < s->num_channels; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }

    /*
     * Peripheral DMA-request inputs: each drives a cyclic channel one minor
     * loop. Several peripherals can share one eDMA (e.g. edma1 serves both
     * MICFIL capture and SAI1 playback), so expose a few lines; the handler
     * services the active cyclic channel regardless of which line pulsed.
     */
    qdev_init_gpio_in_named(dev, edma_dma_request, "dma-req", 4);
}

static const Property imx95_edma_properties[] = {
    DEFINE_PROP_UINT32("num-channels", IMX95EdmaState, num_channels, 31),
    DEFINE_PROP_UINT32("chan-stride", IMX95EdmaState, chan_stride,
                       IMX95_EDMA_CHAN_STRIDE),
    DEFINE_PROP_BOOL("tcd64", IMX95EdmaState, tcd64, false),
    /*
     * CH_SBR reset value (RM: eDMA3 ch.41 = 0x8007, eDMA5 ch.71 = 0x8007;
     * a 32-ch eDMA5 variant is 0x8015). Per-instance overridable because the
     * MID genuinely differs per engine - one reset-per-name would be a lie.
     */
    DEFINE_PROP_UINT32("ch-sbr-reset", IMX95EdmaState, ch_sbr_reset, 0x8007),
};

static const VMStateDescription vmstate_imx95_edma_chan = {
    .name = "imx95.edma3.chan",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, IMX95EdmaChan, IMX95_EDMA_CHAN_REGS_SZ),
        VMSTATE_BOOL(armed, IMX95EdmaChan),
        VMSTATE_BOOL(cyclic, IMX95EdmaChan),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_imx95_edma = {
    .name = TYPE_IMX95_EDMA,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(mgmt, IMX95EdmaState, IMX95_EDMA_MGMT_REGS),
        VMSTATE_STRUCT_ARRAY(chan, IMX95EdmaState, IMX95_EDMA_MAX_CHANNELS, 3,
                             vmstate_imx95_edma_chan, IMX95EdmaChan),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_edma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_edma_realize;
    dc->vmsd = &vmstate_imx95_edma;
    device_class_set_props(dc, imx95_edma_properties);
    device_class_set_legacy_reset(dc, imx95_edma_reset);
    dc->desc = "i.MX95 eDMA v3 controller";
}

static const TypeInfo imx95_edma_types[] = {
    {
        .name = TYPE_IMX95_EDMA,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95EdmaState),
        .class_init = imx95_edma_class_init,
    },
};

DEFINE_TYPES(imx95_edma_types)
