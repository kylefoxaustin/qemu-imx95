// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * v4l2_to_fb - capture a frame from the i.MX95 ISI and put it on the LCD.
 *
 * The last link in the vision path: a "smart camera" frame arrives over MIPI
 * CSI-2, the ISI DMAs it into DRAM, this client DQBUFs it and blits it into
 * /dev/fb0, which the DPU scans out to the LVDS panel. That is the whole
 * transport chain end to end, with no ISP in it - the sensor is assumed to emit
 * developed YUV.
 *
 * It prints an FNV-1a hash of the captured bytes so the test can prove the
 * frame arrived intact rather than merely arriving; a picture that looks right
 * is not evidence that every byte is right.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>

#define NBUF 4

static uint64_t fnv1a(const unsigned char *p, size_t n)
{
    /* FNV-1a 64-bit offset basis. Not 1469598103934665603 - that is this
     * constant with a digit missing, which still hashes and still distinguishes
     * inputs, so every internal check passes on numbers nobody outside this
     * program can reproduce. A hash exists to be verified independently. */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* UYVY 4:2:2 -> the framebuffer's 32bpp XRGB, BT.601 full range. */
static void yuyv_to_fb(const unsigned char *src0, int w, int h, int src_stride,
                       unsigned char *fb, int fb_w, int fb_h, int stride, int bpp,
                       int ox, int oy, int yuyv)
{
    for (int y = 0; y < h && y + oy < fb_h; y++) {
        const unsigned char *src = src0 + (size_t)y * src_stride;
        unsigned char *dst = fb + (size_t)(y + oy) * stride;
        for (int x = 0; x < w && x + ox < fb_w; x += 2) {
            int y0, y1, u, v;
            if (yuyv) { y0 = src[0]; u = src[1] - 128; y1 = src[2]; v = src[3] - 128; }
            else      { u = src[0] - 128; y0 = src[1]; v = src[2] - 128; y1 = src[3]; }
            src += 4;
            for (int k = 0; k < 2 && x + k + ox < fb_w; k++) {
                int Y = k ? y1 : y0;
                int r = clamp255(Y + ((91881 * v) >> 16));
                int g = clamp255(Y - ((22554 * u + 46802 * v) >> 16));
                int b = clamp255(Y + ((116130 * u) >> 16));
                unsigned char *px = dst + (size_t)(x + k + ox) * (bpp / 8);
                if (bpp == 32) { px[0] = b; px[1] = g; px[2] = r; px[3] = 0xff; }
                else if (bpp == 16) {
                    unsigned short p = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
                    px[0] = p & 0xff; px[1] = p >> 8;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *vdev = argc > 1 ? argv[1] : "/dev/video0";
    const char *fbdev = argc > 2 ? argv[2] : "/dev/fb0";
    int want_w = argc > 3 ? atoi(argv[3]) : 1280;
    int want_h = argc > 4 ? atoi(argv[4]) : 800;
    const char *dumpfile = argc > 5 ? argv[5] : NULL;

    int vfd = open(vdev, O_RDWR);
    if (vfd < 0) { perror("open video"); return 1; }

    /* Take the format the media graph negotiated. Forcing one here would
     * paper over a pipeline that agreed on something else, which is exactly
     * the failure this test exists to catch. */
    /* The mxc-isi capture node is MULTIPLANAR (V4L2_CAP_VIDEO_CAPTURE_MPLANE),
     * so the single-planar G_FMT returns EINVAL. Use the _MPLANE API. */
    int btype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_format fmt = { .type = btype };
    if (ioctl(vfd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
    int W = fmt.fmt.pix_mp.width, H = fmt.fmt.pix_mp.height;
    size_t frame_sz = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    int src_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    unsigned fourcc = fmt.fmt.pix_mp.pixelformat;
    printf("CAPTURE %dx%d fourcc=%.4s planes=%u bpl=%d sizeimage=%zu\n",
           W, H, (char *)&fmt.fmt.pix_mp.pixelformat,
           fmt.fmt.pix_mp.num_planes, src_stride, frame_sz);
    (void)want_w; (void)want_h;

    struct v4l2_requestbuffers req = { .count = NBUF,
        .type = btype, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(vfd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }

    void *bufs[NBUF]; size_t lens[NBUF];
    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_plane pl[VIDEO_MAX_PLANES];
        struct v4l2_buffer b = { .type = req.type, .memory = req.memory, .index = i };
        memset(pl, 0, sizeof pl);
        b.m.planes = pl; b.length = VIDEO_MAX_PLANES;
        if (ioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
        bufs[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                       vfd, pl[0].m.mem_offset);
        lens[i] = pl[0].length;
        if (bufs[i] == MAP_FAILED) { perror("mmap"); return 1; }
        if (ioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
    }
    int type = req.type;
    if (ioctl(vfd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

    /* Take a few frames; the first can be mid-DMA on a cold pipeline. */
    struct v4l2_buffer b; struct v4l2_plane dpl[VIDEO_MAX_PLANES];
    int got = 0; uint64_t hash = 0; unsigned idx = 0;
    for (int i = 0; i < 5; i++) {
        memset(&b, 0, sizeof b); memset(dpl, 0, sizeof dpl);
        b.type = req.type; b.memory = req.memory;
        b.m.planes = dpl; b.length = VIDEO_MAX_PLANES;
        if (ioctl(vfd, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        idx = b.index; got++;
        hash = fnv1a(bufs[idx], frame_sz < lens[idx] ? frame_sz : lens[idx]);
        printf("FRAME %d bytesused=%u hash=%016llx\n", i, dpl[0].bytesused,
               (unsigned long long)hash);
        if (i < 4 && ioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }
    }
    if (!got) { fprintf(stderr, "no frames\n"); return 1; }

    if (dumpfile) {
        FILE *f = fopen(dumpfile, "wb");
        if (f) { fwrite(bufs[idx], 1, frame_sz, f); fclose(f);
                 printf("DUMPED %s\n", dumpfile); }
    }

    /* Blit onto the panel. */
    int ffd = open(fbdev, O_RDWR);
    if (ffd < 0) { perror("open fb"); return 2; }
    struct fb_var_screeninfo var; struct fb_fix_screeninfo fix;
    if (ioctl(ffd, FBIOGET_VSCREENINFO, &var) < 0) { perror("VSCREENINFO"); return 2; }
    if (ioctl(ffd, FBIOGET_FSCREENINFO, &fix) < 0) { perror("FSCREENINFO"); return 2; }
    printf("FB %ux%u bpp=%u stride=%u\n", var.xres, var.yres,
           var.bits_per_pixel, fix.line_length);
    size_t fbsz = (size_t)fix.line_length * var.yres;
    unsigned char *fb = mmap(NULL, fbsz, PROT_READ | PROT_WRITE, MAP_SHARED, ffd, 0);
    if (fb == MAP_FAILED) { perror("mmap fb"); return 2; }
    memset(fb, 0, fbsz);
    int ox = ((int)var.xres - W) / 2, oy = ((int)var.yres - H) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    yuyv_to_fb(bufs[idx], W, H, src_stride, fb, var.xres, var.yres,
               fix.line_length, var.bits_per_pixel, ox, oy,
               fourcc == V4L2_PIX_FMT_YUYV);
    printf("BLIT %dx%d at +%d+%d (%s)\n", W, H, ox, oy,
           fourcc == V4L2_PIX_FMT_YUYV ? "YUYV" : "UYVY");
    msync(fb, fbsz, MS_SYNC);
    /* nudge the DPU into a fresh scanout */
    var.activate = FB_ACTIVATE_NOW; var.yoffset = 0;
    ioctl(ffd, FBIOPAN_DISPLAY, &var);
    printf("DISPLAYED %dx%d onto %ux%u\n", W, H, var.xres, var.yres);
    printf("CAPTURE_HASH %016llx\n", (unsigned long long)hash);
    return 0;
}
