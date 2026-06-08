/*
 * i.MX 95 DPU 2D blit engine - RGB<->YUV colour-conversion exerciser (NXP G2D).
 *
 * Drives libg2d-dpu through an RGBA8888 -> YUYV conversion and back
 * (YUYV -> RGBA8888), then checks the round trip reproduces the source. The
 * source is four solid colour quadrants so the 4:2:2 chroma subsampling is
 * lossless in flat interiors; the check allows a small tolerance for the
 * RGB<->YUV rounding. Prints "RGB2YUV: PASS/FAIL", "YUV2RGB: PASS/FAIL" and a
 * final "G2D-CONVERT: PASS/FAIL" the harness greps.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* Minimal libg2d ABI (see g2d_scale_rot.c). */
typedef unsigned long g2d_phys_addr_t;
enum g2d_format { G2D_RGBA8888 = 1, G2D_YUYV = 24 };  /* bytes Y0,U,Y1,V */
enum g2d_blend_func { G2D_BLEND_ZERO = 0 };
enum g2d_rotation { G2D_ROTATION_0 = 0 };
struct g2d_surface {
    enum g2d_format format;
    g2d_phys_addr_t planes[3];
    int left, top, right, bottom;
    int stride, width, height;
    enum g2d_blend_func blendfunc;
    int global_alpha;
    int clrcolor;
    enum g2d_rotation rot;
};
struct g2d_buf {
    void *buf_handle;
    void *buf_vaddr;
    g2d_phys_addr_t buf_paddr;
    int buf_size;
};
int g2d_open(void **handle);
int g2d_close(void *handle);
int g2d_blit(void *handle, struct g2d_surface *src, struct g2d_surface *dst);
int g2d_finish(void *handle);
struct g2d_buf *g2d_alloc(int size, int cacheable);
int g2d_free(struct g2d_buf *buf);

#define W 64
#define H 64
#define RGBA(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(a) << 24))
#define RED   RGBA(200, 40, 40, 255)
#define GREEN RGBA(40, 200, 40, 255)
#define BLUE  RGBA(40, 40, 200, 255)
#define WHITE RGBA(200, 200, 200, 255)

static void fill_quadrants(uint32_t *p)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            p[y * W + x] = (x < W / 2)
                ? (y < H / 2 ? RED : BLUE)
                : (y < H / 2 ? GREEN : WHITE);
        }
    }
}

static void init_surface(struct g2d_surface *s, enum g2d_format f,
                         g2d_phys_addr_t pa, int stride)
{
    memset(s, 0, sizeof(*s));
    s->format = f;
    s->planes[0] = pa;
    s->left = 0;
    s->top = 0;
    s->right = W;
    s->bottom = H;
    s->stride = stride;
    s->width = W;
    s->height = H;
}

/* per-channel difference within tol? (interior pixels avoid 4:2:2 edge bleed) */
static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 0xff) - (int)((b >> s) & 0xff);
        if (d < 0) {
            d = -d;
        }
        if (d > tol) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    void *handle;
    struct g2d_buf *rgb, *yuv, *out;
    struct g2d_surface s, d;
    uint32_t *src, *dst;
    int rgb2yuv_ok, yuv2rgb_ok;

    if (g2d_open(&handle)) {
        printf("g2d_open fail\n");
        return 1;
    }
    rgb = g2d_alloc(W * H * 4, 0);
    yuv = g2d_alloc(W * H * 2, 0);          /* YUYV: 2 bytes/pixel */
    out = g2d_alloc(W * H * 4, 0);
    if (!rgb || !yuv || !out) {
        printf("g2d_alloc fail\n");
        return 1;
    }
    fill_quadrants(rgb->buf_vaddr);
    src = rgb->buf_vaddr;
    dst = out->buf_vaddr;

    /* ---- RGBA8888 -> YUYV ---- */
    init_surface(&s, G2D_RGBA8888, rgb->buf_paddr, W);
    init_surface(&d, G2D_YUYV, yuv->buf_paddr, W);
    memset(yuv->buf_vaddr, 0, W * H * 2);
    g2d_blit(handle, &s, &d);
    g2d_finish(handle);

    /* ---- YUYV -> RGBA8888 ---- */
    init_surface(&s, G2D_YUYV, yuv->buf_paddr, W);
    init_surface(&d, G2D_RGBA8888, out->buf_paddr, W);
    memset(out->buf_vaddr, 0, W * H * 4);
    g2d_blit(handle, &s, &d);
    g2d_finish(handle);

    /* RGB2YUV produced a non-zero YUYV buffer (conversion ran at all). */
    rgb2yuv_ok = 0;
    for (int i = 0; i < W * H / 2; i++) {
        if (((uint32_t *)yuv->buf_vaddr)[i] != 0) {
            rgb2yuv_ok = 1;
            break;
        }
    }
    printf("RGB2YUV: %s\n", rgb2yuv_ok ? "PASS" : "FAIL");

    /* Round trip reproduces the four quadrant colours (interior samples). */
    yuv2rgb_ok =
        near(dst[16 * W + 16],  src[16 * W + 16],  12) &&
        near(dst[16 * W + 48],  src[16 * W + 48],  12) &&
        near(dst[48 * W + 16],  src[48 * W + 16],  12) &&
        near(dst[48 * W + 48],  src[48 * W + 48],  12);
    printf("  rt(16,16) %06x->%06x  rt(48,16) %06x->%06x\n",
           src[16 * W + 16] & 0xffffff, dst[16 * W + 16] & 0xffffff,
           src[16 * W + 48] & 0xffffff, dst[16 * W + 48] & 0xffffff);
    printf("YUV2RGB: %s\n", yuv2rgb_ok ? "PASS" : "FAIL");

    printf("G2D-CONVERT: %s\n", (rgb2yuv_ok && yuv2rgb_ok) ? "PASS" : "FAIL");

    g2d_free(rgb);
    g2d_free(yuv);
    g2d_free(out);
    g2d_close(handle);
    return (rgb2yuv_ok && yuv2rgb_ok) ? 0 : 1;
}
