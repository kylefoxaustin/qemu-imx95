/*
 * QTest for the i.MX 95 NeoISP - does it actually develop a Bayer frame?
 *
 * Copyright (c) 2026 Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the registers directly - no kernel, no driver - writes a synthetic
 * Bayer mosaic into guest RAM, triggers the block, and checks what came back.
 *
 * The interesting assertion is not "some bytes changed". A debayer that
 * produces A picture rather than THE picture would pass that. So the input is
 * a mosaic sampled from a known RGB image, and the test asserts the developed
 * output RECONSTRUCTS that image within a tolerance - which is a claim about
 * correctness rather than about activity.
 */
#include "qemu/osdep.h"
#include "libqtest-single.h"
#include <math.h>

#define ISP_BASE            0x4ae00000ULL
#define TRIG_CAM0           0x20
#define  TRIG_TRIGGER       (1u << 0)
#define INT_EN0             0x24        /* b0 (v2) offsets */
#define INT_STAT0           0x28
#define  INT_S_FD2          (1u << 3)
#define IMG_CONF_CAM0       0x30
#define IMG_SIZE_CAM0       0x34
#define IMG0_IN_ADDR_CAM0   0x3c
#define OUTCH0_ADDR_CAM0    0x44
#define IMG0_IN_LS_CAM0     0x50
#define OUTCH0_LS_CAM0      0x58
#define DEMOSAIC_CTRL_CAM0  0x1000
#define OB_WB0_R_CTRL       0x204
#define OB_WB0_GR_CTRL      0x208
#define OB_WB0_GB_CTRL      0x20c
#define OB_WB0_B_CTRL       0x210
#define  OBWB(off, gain)    (((gain) << 16) | ((off) & 0xffff))
#define  GAIN_UNITY         256

/* somewhere in DRAM, clear of the kernel/initrd the machine does not load */
#define IN_PA               0x90000000ULL
#define OUT_PA              0x90100000ULL

#define W 32
#define H 32

/*
 * The scene the Bayer mosaic is sampled from. Chosen to DISCRIMINATE, not just
 * to look like something: red and blue are driven in OPPOSITE directions, so
 * the most likely real defect - a wrong Bayer phase, which swaps R and B -
 * cannot hide. A smooth grey-ish scene correlates well even when developed
 * wrongly, which makes it a weak oracle however pretty the picture is.
 *
 * ⚠️ But every channel must remain RECOVERABLE. An earlier version gave green a
 * per-pixel checkerboard, which is Nyquist-frequency content that no debayer
 * can reconstruct from a subsampled mosaic - so the test failed a CORRECT
 * model. A discriminating scene is not the same as a hostile one: the oracle
 * must ask for something achievable, or it measures the algorithm's limits
 * rather than the implementation's correctness.
 */
static void scene_rgb(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (x >= 8 && x < 24 && y >= 8 && y < 24) {
        *r = 240; *g = 120; *b = 16;            /* strongly red-dominant patch */
    } else {
        *r = (uint8_t)(x * 8);                  /* red ramps left -> right */
        *g = (uint8_t)(y * 8);                  /* green ramps top -> bottom */
        *b = (uint8_t)(248 - x * 8);            /* blue ramps the OTHER way */
    }
}

static void test_neoisp_develop(void)
{
    uint8_t bayer[W * H];
    uint8_t out[W * H * 4];
    int x, y, rx = 0, ry = 0;                    /* RGGB: red at (0,0) */
    double sx[3] = {0}, sy[3] = {0}, sxx[3] = {0}, syy[3] = {0}, sxy[3] = {0};
    int n = 0;
    uint32_t stat;

    qtest_start("-M imx95-19x19-evk -m 2G -display none");

    /* Build an RGGB mosaic: each site carries only its own colour. */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint8_t r, g, b;
            int ox = (x & 1) == rx, oy = (y & 1) == ry;

            scene_rgb(x, y, &r, &g, &b);
            bayer[y * W + x] = (ox && oy) ? r : ((!ox && !oy) ? b : g);
        }
    }
    qtest_memwrite(global_qtest, IN_PA, bayer, sizeof(bayer));
    memset(out, 0xa5, sizeof(out));              /* poison, so "unwritten" fails */
    qtest_memwrite(global_qtest, OUT_PA, out, sizeof(out));

    writel(ISP_BASE + IMG_SIZE_CAM0, (H << 16) | W);
    writel(ISP_BASE + IMG_CONF_CAM0, 8);         /* IBPP0 = 8-bit raw */
    writel(ISP_BASE + DEMOSAIC_CTRL_CAM0, 0 << 4); /* FMT 0 = RGGB */
    writel(ISP_BASE + IMG0_IN_LS_CAM0, W);
    writel(ISP_BASE + OUTCH0_LS_CAM0, W * 4);
    writel(ISP_BASE + IMG0_IN_ADDR_CAM0, (uint32_t)(IN_PA >> 4));
    writel(ISP_BASE + OUTCH0_ADDR_CAM0, (uint32_t)(OUT_PA >> 4));
    /* unity tuning: no black-level offset, gain 1.0 on every channel */
    writel(ISP_BASE + OB_WB0_R_CTRL,  OBWB(0, GAIN_UNITY));
    writel(ISP_BASE + OB_WB0_GR_CTRL, OBWB(0, GAIN_UNITY));
    writel(ISP_BASE + OB_WB0_GB_CTRL, OBWB(0, GAIN_UNITY));
    writel(ISP_BASE + OB_WB0_B_CTRL,  OBWB(0, GAIN_UNITY));
    writel(ISP_BASE + INT_EN0, INT_S_FD2);

    writel(ISP_BASE + TRIG_CAM0, TRIG_TRIGGER);

    /* frame-done must be raised, or the driver would wait forever */
    stat = readl(ISP_BASE + INT_STAT0);
    g_assert_cmphex(stat & INT_S_FD2, ==, INT_S_FD2);

    qtest_memread(global_qtest, OUT_PA, out, sizeof(out));

    /* Correlate PER CHANNEL against the scene. Debayer is lossy at edges, so
     * this asks "is it the same picture", not "is it identical" - but doing it
     * per channel is what makes a red/blue swap impossible to pass. */
    for (y = 1; y < H - 1; y++) {
        for (x = 1; x < W - 1; x++) {
            uint8_t want[3];
            int c;

            scene_rgb(x, y, &want[0], &want[1], &want[2]);
            for (c = 0; c < 3; c++) {
                /* output is packed B,G,R,A */
                double got = out[(y * W + x) * 4 + (2 - c)];
                sx[c] += got;        sy[c] += want[c];
                sxx[c] += got * got; syy[c] += (double)want[c] * want[c];
                sxy[c] += got * want[c];
            }
            n++;
        }
    }
    {
        static const char *cname[3] = { "red", "green", "blue" };
        double worst = 1.0;
        int c;

        for (c = 0; c < 3; c++) {
            double num = n * sxy[c] - sx[c] * sy[c];
            double den = sqrt((n * sxx[c] - sx[c] * sx[c]) *
                              (n * syy[c] - sy[c] * sy[c]));
            double r = den > 0 ? num / den : 0;

            g_test_message("neoisp: %-5s correlation r=%.4f", cname[c], r);
            if (r < worst) {
                worst = r;
            }
        }
        /* A correct debayer lands near 1.0 on every channel. A wrong Bayer
         * phase swaps red and blue and drives those two sharply negative, so
         * gating on the WEAKEST channel is what gives this test its power. */
        g_assert_cmpfloat(worst, >, 0.95);
    }

    /* write-1-to-clear must actually clear */
    writel(ISP_BASE + INT_STAT0, INT_S_FD2);
    g_assert_cmphex(readl(ISP_BASE + INT_STAT0) & INT_S_FD2, ==, 0);

    qtest_end();
}

/*
 * Does the tuning knob DO anything?
 *
 * A params interface that is accepted and ignored is worse than one that is
 * absent: an engineer changes white balance, sees no change, and concludes the
 * model is broken - or worse, concludes their tuning is wrong. So this drives
 * the same frame twice, once at unity and once with the red channel's gain
 * halved, and requires the red output to actually fall while green does not.
 */
static void test_neoisp_tuning(void)
{
    uint8_t bayer[W * H], out[W * H * 4];
    int x, y, rx = 0, ry = 0;
    double red_unity = 0, red_half = 0, grn_unity = 0, grn_half = 0;
    int n = 0, pass;

    qtest_start("-M imx95-19x19-evk -m 2G -display none");

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint8_t r, g, b;
            int ox = (x & 1) == rx, oy = (y & 1) == ry;

            scene_rgb(x, y, &r, &g, &b);
            bayer[y * W + x] = (ox && oy) ? r : ((!ox && !oy) ? b : g);
        }
    }
    qtest_memwrite(global_qtest, IN_PA, bayer, sizeof(bayer));

    for (pass = 0; pass < 2; pass++) {
        double sr = 0, sg = 0;

        writel(ISP_BASE + IMG_SIZE_CAM0, (H << 16) | W);
        writel(ISP_BASE + IMG_CONF_CAM0, 6);      /* enc 6 = 8-bit samples */
        writel(ISP_BASE + DEMOSAIC_CTRL_CAM0, 0 << 4);
        writel(ISP_BASE + IMG0_IN_LS_CAM0, W);
        writel(ISP_BASE + OUTCH0_LS_CAM0, W * 4);
        writel(ISP_BASE + IMG0_IN_ADDR_CAM0, (uint32_t)(IN_PA >> 4));
        writel(ISP_BASE + OUTCH0_ADDR_CAM0, (uint32_t)(OUT_PA >> 4));
        /* pass 0: unity everywhere. pass 1: HALVE the red gain only. */
        writel(ISP_BASE + OB_WB0_R_CTRL,
               OBWB(0, pass ? GAIN_UNITY / 2 : GAIN_UNITY));
        writel(ISP_BASE + OB_WB0_GR_CTRL, OBWB(0, GAIN_UNITY));
        writel(ISP_BASE + OB_WB0_GB_CTRL, OBWB(0, GAIN_UNITY));
        writel(ISP_BASE + OB_WB0_B_CTRL,  OBWB(0, GAIN_UNITY));
        writel(ISP_BASE + INT_EN0, INT_S_FD2);

        writel(ISP_BASE + TRIG_CAM0, TRIG_TRIGGER);
        g_assert_cmphex(readl(ISP_BASE + INT_STAT0) & INT_S_FD2, ==, INT_S_FD2);
        writel(ISP_BASE + INT_STAT0, INT_S_FD2);

        qtest_memread(global_qtest, OUT_PA, out, sizeof(out));
        n = 0;
        for (y = 1; y < H - 1; y++) {
            for (x = 1; x < W - 1; x++) {
                sr += out[(y * W + x) * 4 + 2];
                sg += out[(y * W + x) * 4 + 1];
                n++;
            }
        }
        if (pass) { red_half = sr / n; grn_half = sg / n; }
        else      { red_unity = sr / n; grn_unity = sg / n; }
    }

    g_test_message("neoisp tuning: red %.1f -> %.1f, green %.1f -> %.1f",
                   red_unity, red_half, grn_unity, grn_half);
    /* halving the red gain must roughly halve red... */
    g_assert_cmpfloat(red_half, <, red_unity * 0.75);
    /* ...and must NOT move green: that is what makes it WHITE BALANCE rather
     * than a brightness control applied to everything */
    g_assert_cmpfloat(grn_half, >, grn_unity * 0.95);
    g_assert_cmpfloat(grn_half, <, grn_unity * 1.05);

    qtest_end();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx95/neoisp/develop", test_neoisp_develop);
    qtest_add_func("/imx95/neoisp/tuning", test_neoisp_tuning);
    return g_test_run();
}
