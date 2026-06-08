/*
 * i.MX 95 DPU 2D blit engine - RGBA <-> NV12 colour-conversion exerciser (G2D).
 *
 * NV12 is 2-plane 4:2:0: plane 0 = Y (W*H bytes), plane 1 = interleaved Cb,Cr at
 * half resolution (W*H/2 bytes). Drives libg2d-dpu through RGBA8888 -> NV12 and
 * back, then checks the round trip reproduces the four solid quadrant colours
 * (4:2:0 subsampling is lossless in flat interiors). Prints "RGB2NV12:",
 * "NV122RGB:" and "G2D-NV12:" PASS/FAIL lines the harness greps.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Minimal libg2d ABI (see g2d_scale_rot.c). */
typedef unsigned long g2d_phys_addr_t;
enum g2d_format { G2D_RGBA8888 = 1, G2D_NV12 = 20 };
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

static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 0xff) - (int)((b >> s) & 0xff);
        d = d < 0 ? -d : d;
        if (d > tol) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    void *handle;
    struct g2d_buf *rgb, *nv12, *out;
    struct g2d_surface s, d;
    uint32_t *src, *dst;
    int rgb2nv12_ok, nv122rgb_ok;

    if (g2d_open(&handle)) {
        printf("g2d_open fail\n");
        return 1;
    }
    rgb = g2d_alloc(W * H * 4, 0);
    nv12 = g2d_alloc(W * H * 3 / 2, 0);     /* Y + half-res interleaved UV */
    out = g2d_alloc(W * H * 4, 0);
    if (!rgb || !nv12 || !out) {
        printf("g2d_alloc fail\n");
        return 1;
    }
    src = rgb->buf_vaddr;
    dst = out->buf_vaddr;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            src[y * W + x] = (x < W / 2)
                ? (y < H / 2 ? RED : BLUE)
                : (y < H / 2 ? GREEN : WHITE);
        }
    }

    /* ---- RGBA8888 -> NV12 (plane 0 = Y, plane 1 = interleaved UV) ---- */
    memset(&s, 0, sizeof(s));
    s.format = G2D_RGBA8888; s.planes[0] = rgb->buf_paddr;
    s.left = 0; s.top = 0; s.right = W; s.bottom = H;
    s.stride = W; s.width = W; s.height = H;
    memset(&d, 0, sizeof(d));
    d.format = G2D_NV12;
    d.planes[0] = nv12->buf_paddr;
    d.planes[1] = nv12->buf_paddr + (unsigned long)W * H;
    d.left = 0; d.top = 0; d.right = W; d.bottom = H;
    d.stride = W; d.width = W; d.height = H;
    memset(nv12->buf_vaddr, 0, W * H * 3 / 2);
    g2d_blit(handle, &s, &d);
    g2d_finish(handle);

    /* ---- NV12 -> RGBA8888 ---- */
    memset(&s, 0, sizeof(s));
    s.format = G2D_NV12;
    s.planes[0] = nv12->buf_paddr;
    s.planes[1] = nv12->buf_paddr + (unsigned long)W * H;
    s.left = 0; s.top = 0; s.right = W; s.bottom = H;
    s.stride = W; s.width = W; s.height = H;
    memset(&d, 0, sizeof(d));
    d.format = G2D_RGBA8888; d.planes[0] = out->buf_paddr;
    d.left = 0; d.top = 0; d.right = W; d.bottom = H;
    d.stride = W; d.width = W; d.height = H;
    memset(out->buf_vaddr, 0, W * H * 4);
    g2d_blit(handle, &s, &d);
    g2d_finish(handle);

    rgb2nv12_ok = 0;
    for (int i = 0; i < W * H * 3 / 8; i++) {
        if (((uint32_t *)nv12->buf_vaddr)[i] != 0) {
            rgb2nv12_ok = 1;
            break;
        }
    }
    printf("RGB2NV12: %s\n", rgb2nv12_ok ? "PASS" : "FAIL");

    nv122rgb_ok =
        near(dst[16 * W + 16], src[16 * W + 16], 14) &&
        near(dst[16 * W + 48], src[16 * W + 48], 14) &&
        near(dst[48 * W + 16], src[48 * W + 16], 14) &&
        near(dst[48 * W + 48], src[48 * W + 48], 14);
    printf("  rt(16,16) %06x->%06x  rt(48,48) %06x->%06x\n",
           src[16 * W + 16] & 0xffffff, dst[16 * W + 16] & 0xffffff,
           src[48 * W + 48] & 0xffffff, dst[48 * W + 48] & 0xffffff);
    printf("NV122RGB: %s\n", nv122rgb_ok ? "PASS" : "FAIL");

    printf("G2D-NV12: %s\n", (rgb2nv12_ok && nv122rgb_ok) ? "PASS" : "FAIL");

    g2d_free(rgb);
    g2d_free(nv12);
    g2d_free(out);
    g2d_close(handle);
    return (rgb2nv12_ok && nv122rgb_ok) ? 0 : 1;
}
