/*
 * i.MX 95 DPU 2D blit engine - scale + rotate exerciser (real NXP G2D).
 *
 * Drives libg2d-dpu with two transforming blits the basic test never does:
 *   1. a 2x upscale  (src 64x64 -> dst 128x128)
 *   2. a 90-degree rotation (G2D_ROTATION_90)
 * and self-checks the result by reading back the destination. The source is
 * four solid colour quadrants (TL red, TR green, BL blue, BR white) so the
 * checks land in flat interior regions where nearest-neighbour and the HW's
 * bilinear filter agree. Prints "SCALE: PASS/FAIL", "ROTATE: PASS/FAIL", and a
 * final "G2D-XFORM: PASS/FAIL" the harness greps.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/*
 * Minimal libg2d ABI (matches NXP's g2d.h struct/enum layout) so the test is
 * self-contained — it compiles against the BSP rootfs (libg2d.so.2 + libc) with
 * no g2d dev-header/sysroot dependency. Only the bits this exerciser uses.
 */
typedef unsigned long g2d_phys_addr_t;
enum g2d_format { G2D_RGBA8888 = 1 };
enum g2d_blend_func { G2D_BLEND_ZERO = 0 };
enum g2d_rotation {
    G2D_ROTATION_0 = 0, G2D_ROTATION_90 = 1, G2D_ROTATION_180 = 2,
    G2D_ROTATION_270 = 3, G2D_FLIP_H = 4, G2D_FLIP_V = 5
};
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

/* G2D_RGBA8888 in memory is bytes R,G,B,A -> little-endian word below. */
#define RGBA(r, g, b, a) \
    ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(a) << 24))
#define RED   RGBA(255, 0, 0, 255)
#define GREEN RGBA(0, 255, 0, 255)
#define BLUE  RGBA(0, 0, 255, 255)
#define WHITE RGBA(255, 255, 255, 255)

static void fill_quadrants(uint32_t *p, int w, int h, int stride)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = (x < w / 2)
                ? (y < h / 2 ? RED : BLUE)
                : (y < h / 2 ? GREEN : WHITE);
            p[y * stride + x] = c;
        }
    }
}

static void init_surface(struct g2d_surface *s, g2d_phys_addr_t pa,
                         int w, int h)
{
    memset(s, 0, sizeof(*s));
    s->format = G2D_RGBA8888;
    s->planes[0] = pa;
    s->left = 0;
    s->top = 0;
    s->right = w;
    s->bottom = h;
    s->stride = w;
    s->width = w;
    s->height = h;
}

static const char *cname(uint32_t c)
{
    switch (c) {
    case RED:   return "red";
    case GREEN: return "green";
    case BLUE:  return "blue";
    case WHITE: return "white";
    default:    return "??";
    }
}

static int chk(uint32_t *p, int stride, int x, int y, uint32_t want,
               const char *tag)
{
    uint32_t got = p[y * stride + x];

    printf("  %s (%d,%d): got %s (0x%08x) want %s\n", tag, x, y,
           cname(got), got, cname(want));
    return got == want;
}

int main(void)
{
    void *handle;
    struct g2d_buf *sbuf, *dbuf;
    struct g2d_surface src, dst;
    uint32_t *dp;
    int scale_ok, rot_ok;

    if (g2d_open(&handle)) {
        printf("g2d_open fail\n");
        return 1;
    }

    sbuf = g2d_alloc(W * H * 4, 0);
    dbuf = g2d_alloc((2 * W) * (2 * H) * 4, 0);     /* fits the 2x upscale */
    if (!sbuf || !dbuf) {
        printf("g2d_alloc fail\n");
        return 1;
    }
    fill_quadrants(sbuf->buf_vaddr, W, H, W);
    dp = dbuf->buf_vaddr;

    /* ---- 1. 2x upscale: 64x64 -> 128x128 ---- */
    init_surface(&src, sbuf->buf_paddr, W, H);
    init_surface(&dst, dbuf->buf_paddr, 2 * W, 2 * H);
    memset(dbuf->buf_vaddr, 0, (2 * W) * (2 * H) * 4);
    g2d_blit(handle, &src, &dst);
    g2d_finish(handle);

    printf("---- scale 64x64 -> 128x128 ----\n");
    scale_ok = chk(dp, 2 * W, 16,  16,  RED,   "scale") &
               chk(dp, 2 * W, 112, 16,  GREEN, "scale") &
               chk(dp, 2 * W, 16,  112, BLUE,  "scale") &
               chk(dp, 2 * W, 112, 112, WHITE, "scale");
    printf("SCALE: %s\n", scale_ok ? "PASS" : "FAIL");

    /* ---- 2. 90-degree rotation: 64x64 -> 64x64 ---- */
    init_surface(&src, sbuf->buf_paddr, W, H);
    init_surface(&dst, dbuf->buf_paddr, W, H);
    dst.rot = G2D_ROTATION_90;
    memset(dbuf->buf_vaddr, 0, W * H * 4);
    g2d_blit(handle, &src, &dst);
    g2d_finish(handle);

    /*
     * 90-degree clockwise: source TL(red)->dst TR, TR(green)->BR, BR(white)->BL,
     * BL(blue)->dst TL. Check the four dst corner interiors.
     */
    printf("---- rotate 90 ----\n");
    rot_ok = chk(dp, W, 16,      16,      BLUE,  "rot") &
             chk(dp, W, W - 16,  16,      RED,   "rot") &
             chk(dp, W, W - 16,  H - 16,  GREEN, "rot") &
             chk(dp, W, 16,      H - 16,  WHITE, "rot");
    printf("ROTATE: %s\n", rot_ok ? "PASS" : "FAIL");

    printf("G2D-XFORM: %s\n", (scale_ok && rot_ok) ? "PASS" : "FAIL");

    g2d_free(sbuf);
    g2d_free(dbuf);
    g2d_close(handle);
    return (scale_ok && rot_ok) ? 0 : 1;
}
