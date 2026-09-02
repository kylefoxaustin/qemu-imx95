// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * neoisp_batch - process a FOLDER of camera frames, not one frame.
 *
 * The workflow this exists for: an engineer captures frames on real silicon,
 * drops the folder on a PC, and wants them developed and the results kept.
 * Possibly five thousand of them.
 *
 * Two paths, and they mean different things:
 *   raw    Bayer -> the NeoISP -> a developed image per input
 *   smart  already developed by the sensor -> transport only, no ISP
 *
 * DESIGN NOTES, each of which is a mistake avoided rather than a preference:
 *
 *  - THE DELIVERABLE IS THE OUTPUT FOLDER, not the display. A panel that
 *    samples ~1 frame in N while 5,000 go past lets an operator believe they
 *    watched the run. Blit every Nth with N recorded, and hold the last frame.
 *  - "THE FOLDER HAS FILES" IS NOT SUCCESS. A run that dies at frame 3,200
 *    leaves 3,200 outputs indistinguishable from a smaller successful run. So:
 *    a receipt, and a COMPLETION MARKER WRITTEN LAST. Its absence means
 *    incomplete, unambiguously.
 *  - OUTPUT NAMES DERIVE FROM INPUT NAMES. out/0001 defeats the entire point,
 *    which is diffing a result against the capture it came from.
 *  - THE RECEIPT NAMES ITS FOLDERS. "5,000 processed" without saying processed
 *    FROM WHERE is a count with no sample behind it.
 *  - INPUT IS READ-ONLY at the fsdev. Not a limitation to work around: it is
 *    an engineer's only copy of frames off real silicon.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <linux/fb.h>

#define MAXP 4
#define MAXN 8192

struct node {
    int fd, type;
    void *buf[MAXP];
    size_t len[MAXP];
    unsigned nplanes;
    struct v4l2_format fmt;
};

static uint64_t fnv1a(const unsigned char *p, size_t n)
{
    /*
     * The FNV-1a 64-bit offset basis is 14695981039346656037 (0xcbf29ce484222325).
     * This had a digit missing, which still hashes and still distinguishes
     * frames - so every internal check passed - but the receipt's hashes could
     * not be reproduced by anyone using a stock FNV-1a. A hash in a receipt
     * exists to be verified INDEPENDENTLY; one that only this program can
     * reproduce is a checksum of itself.
     */
    uint64_t h = 0xcbf29ce484222325ULL;
    while (n--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

static void node_name(int n, char *out, size_t outn)
{
    char np[96];
    FILE *f;

    out[0] = 0;
    snprintf(np, sizeof np, "/sys/class/video4linux/video%d/name", n);
    f = fopen(np, "r");
    if (!f) return;
    if (fgets(out, outn, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

/* every node-group uses the same names, so a job must come from ONE group */
static int find_group_base(void)
{
    char nm[64], p[64], fr[64];
    int n;

    for (n = 0; n < 256; n++) {
        node_name(n, nm, sizeof nm);
        if (strcmp(nm, "neoisp-input0")) continue;
        node_name(n + 2, p, sizeof p);
        node_name(n + 3, fr, sizeof fr);
        if (!strcmp(p, "neoisp-params") && !strcmp(fr, "neoisp-frame")) return n;
    }
    return -1;
}

static int setup(struct node *n, const char *want, int type, int w, int h,
                 unsigned fourcc, int vidnum)
{
    struct v4l2_requestbuffers req = {0};
    struct v4l2_buffer b = {0};
    struct v4l2_plane pl[MAXP];
    char path[64], nm[64];
    unsigned i;

    node_name(vidnum, nm, sizeof nm);
    if (strcmp(nm, want)) return -1;
    snprintf(path, sizeof path, "/dev/video%d", vidnum);
    n->fd = open(path, O_RDWR);
    if (n->fd < 0) return -1;
    n->type = type;

    memset(&n->fmt, 0, sizeof n->fmt);
    n->fmt.type = type;
    if (ioctl(n->fd, VIDIOC_G_FMT, &n->fmt) < 0) return -1;
    if (w) {
        n->fmt.fmt.pix_mp.width = w;
        n->fmt.fmt.pix_mp.height = h;
        n->fmt.fmt.pix_mp.pixelformat = fourcc;
        if (ioctl(n->fd, VIDIOC_S_FMT, &n->fmt) < 0) return -1;
    }
    req.count = 1; req.type = type; req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(n->fd, VIDIOC_REQBUFS, &req) < 0) return -1;

    memset(pl, 0, sizeof pl);
    b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
    if (type == V4L2_BUF_TYPE_META_OUTPUT || type == V4L2_BUF_TYPE_META_CAPTURE) {
        if (ioctl(n->fd, VIDIOC_QUERYBUF, &b) < 0) return -1;
        n->nplanes = 1;
        n->len[0] = b.length;
        n->buf[0] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         n->fd, b.m.offset);
        return n->buf[0] == MAP_FAILED ? -1 : 0;
    }
    n->nplanes = n->fmt.fmt.pix_mp.num_planes ? n->fmt.fmt.pix_mp.num_planes : 1;
    b.m.planes = pl; b.length = MAXP;
    if (ioctl(n->fd, VIDIOC_QUERYBUF, &b) < 0) return -1;
    for (i = 0; i < n->nplanes; i++) {
        n->len[i] = pl[i].length;
        n->buf[i] = mmap(NULL, pl[i].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         n->fd, pl[i].m.mem_offset);
        if (n->buf[i] == MAP_FAILED) return -1;
    }
    return 0;
}

static int qbuf(struct node *n, size_t used)
{
    struct v4l2_buffer b = {0};
    struct v4l2_plane pl[MAXP];
    unsigned i;

    memset(pl, 0, sizeof pl);
    b.type = n->type; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
    if (n->type == V4L2_BUF_TYPE_META_OUTPUT ||
        n->type == V4L2_BUF_TYPE_META_CAPTURE) {
        b.bytesused = n->len[0]; b.length = n->len[0];
        return ioctl(n->fd, VIDIOC_QBUF, &b);
    }
    b.m.planes = pl; b.length = n->nplanes;
    for (i = 0; i < n->nplanes; i++) {
        pl[i].bytesused = used ? used : n->len[i];
        pl[i].length = n->len[i];
    }
    return ioctl(n->fd, VIDIOC_QBUF, &b);
}

static int dqbuf(struct node *n)
{
    struct v4l2_buffer b = {0};
    struct v4l2_plane pl[MAXP];

    memset(pl, 0, sizeof pl);
    b.type = n->type; b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = pl; b.length = n->nplanes;
    return ioctl(n->fd, VIDIOC_DQBUF, &b);
}

static int stream(struct node *n, int on)
{
    int t = n->type;
    return ioctl(n->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t);
}

/* ---- display: show one designated frame, not a slideshow nobody can watch -- */
static int fb_show(const char *fbdev, const unsigned char *bgr, int w, int h,
                   int bpp_src)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    unsigned char *fb;
    size_t fbsz;
    int fd, y, x, ox, oy;

    fd = open(fbdev, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) { close(fd); return -1; }
    fbsz = (size_t)fix.line_length * var.yres;
    fb = mmap(NULL, fbsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { close(fd); return -1; }
    memset(fb, 0, fbsz);
    ox = ((int)var.xres - w) / 2; if (ox < 0) ox = 0;
    oy = ((int)var.yres - h) / 2; if (oy < 0) oy = 0;
    for (y = 0; y < h && y + oy < (int)var.yres; y++) {
        for (x = 0; x < w && x + ox < (int)var.xres; x++) {
            const unsigned char *s = bgr + ((size_t)y * w + x) * bpp_src;
            unsigned char *d = fb + (size_t)(y + oy) * fix.line_length +
                               (size_t)(x + ox) * (var.bits_per_pixel / 8);
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            if (var.bits_per_pixel == 32) d[3] = 0xff;
        }
    }
    msync(fb, fbsz, MS_SYNC);
    var.activate = FB_ACTIVATE_NOW; var.yoffset = 0;
    ioctl(fd, FBIOPAN_DISPLAY, &var);
    munmap(fb, fbsz); close(fd);
    return 0;
}

static int cmpstr(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static int list_dir(const char *dir, char **names, int max)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    if (!d) return -1;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char *), cmpstr);
    return n;
}

int main(int argc, char **argv)
{
    const char *in_dir = NULL, *out_dir = NULL, *mode = "raw";
    const char *fbdev = "/dev/fb0", *in_tag = "?", *out_tag = "?";
    const char *in_host = "?", *out_host = "?";
    int W = 640, H = 480, blit_every = 0, copy_smart = 0;
    char *names[MAXN];
    int nfiles, i, ok = 0, rejected = 0, blits = 0;
    struct node in0 = {0}, frame = {0}, params = {0}, stats = {0};
    char path[512], opath[512];
    unsigned char *bounce = NULL, *obounce = NULL;
    int trace = 0;
    FILE *rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc) in_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbdev = argv[++i];
        else if (!strcmp(argv[i], "--blit-every") && i + 1 < argc) blit_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--copy-smart")) copy_smart = 1;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--in-tag") && i + 1 < argc) in_tag = argv[++i];
        else if (!strcmp(argv[i], "--out-tag") && i + 1 < argc) out_tag = argv[++i];
        else if (!strcmp(argv[i], "--in-host") && i + 1 < argc) in_host = argv[++i];
        else if (!strcmp(argv[i], "--out-host") && i + 1 < argc) out_host = argv[++i];
    }
    if (!in_dir || !out_dir) {
        fprintf(stderr,
          "usage: %s --in DIR --out DIR [--mode raw|smart] [--width W] [--height H]\n"
          "          [--blit-every N] [--copy-smart] [--fb /dev/fb0] [--trace]\n"
          "          [--in-tag T --out-tag T --in-host P --out-host P]\n", argv[0]);
        return 2;
    }
    int is_raw = !strcmp(mode, "raw");
/*
 * Setup markers always print - there are eight of them and they are what
 * localised the hang that cost this file most of its debugging. PER-FRAME
 * markers are behind --trace: five lines x 5,000 frames is 25,000 lines of
 * console for a run whose useful output is the receipt.
 */
#define STAGE(...) do { printf("BATCH-STAGE " __VA_ARGS__); putchar('\n'); \
                        fflush(stdout); } while (0)
#define FSTAGE(...) do { if (trace) { STAGE(__VA_ARGS__); } } while (0)

    nfiles = list_dir(in_dir, names, MAXN);
    if (nfiles < 0) { fprintf(stderr, "cannot read %s: %s\n", in_dir, strerror(errno)); return 1; }
    printf("BATCH start mode=%s files=%d %dx%d\n", mode, nfiles, W, H);

    STAGE("listed %d files", nfiles);
    snprintf(path, sizeof path, "%s/receipt.txt", out_dir);
    rc = fopen(path, "w");
    STAGE("receipt opened");
    if (!rc) { fprintf(stderr, "cannot write receipt: %s\n", strerror(errno)); return 1; }
    /* Name the folders. A count with no provenance is a count with no sample. */
    fprintf(rc, "# neoisp batch receipt\n");
    fprintf(rc, "mode %s\ngeometry %dx%d\n", mode, W, H);
    fprintf(rc, "in_mount_tag %s\nin_host_path %s\nin_guest_dir %s\n", in_tag, in_host, in_dir);
    fprintf(rc, "out_mount_tag %s\nout_host_path %s\nout_guest_dir %s\n", out_tag, out_host, out_dir);
    fprintf(rc, "blit_every %d\n", blit_every);
    fprintf(rc, "files_found %d\n", nfiles);
    fflush(rc);
    STAGE("receipt flushed (first 9p write)");

    if (is_raw) {
        STAGE("finding node group");
        int base = find_group_base();
        if (base < 0) { fprintf(rc, "FATAL no neoisp node group\n"); fclose(rc); return 1; }
        STAGE("node group base=%d", base);
        fprintf(rc, "neoisp_group /dev/video%d\n", base);
        if (setup(&in0, "neoisp-input0", V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                  W, H, V4L2_PIX_FMT_SBGGR8, base) < 0 ||
            setup(&frame, "neoisp-frame", V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                  W, H, V4L2_PIX_FMT_RGB32, base + 3) < 0) {
            fprintf(rc, "FATAL node setup failed\n"); fclose(rc); return 1;
        }
        setup(&params, "neoisp-params", V4L2_BUF_TYPE_META_OUTPUT, 0, 0, 0, base + 2);
        setup(&stats,  "neoisp-stats",  V4L2_BUF_TYPE_META_CAPTURE, 0, 0, 0, base + 5);
        STAGE("nodes set up, in0.len[0]=%zu", (size_t)in0.len[0]);
        bounce = malloc(in0.len[0]);
        obounce = malloc(frame.len[0]);
        if (!bounce || !obounce) { fprintf(rc, "FATAL no memory\n"); fclose(rc); return 1; }
        /*
         * PRE-FAULT THE BOUNCE BUFFER. 9p reads go through netfs, which pins
         * the destination pages with get_user_pages; a fresh malloc is mapped
         * but not yet populated, and the pin comes back EFAULT on pages that
         * have never been touched. Writing to it once forces them in.
         */
        memset(bounce, 0, in0.len[0]);
        memset(obounce, 0, frame.len[0]);
        STAGE("bounces allocated + prefaulted");
        /*
         * STREAM ON BEFORE ANY BUFFER IS QUEUED, once, outside the loop.
         * vb2 hands pre-queued buffers to the driver only AFTER
         * start_streaming returns, and this driver schedules a job from inside
         * it - so a buffer queued first is invisible to that job and DQBUF
         * blocks forever with no error. The same mistake cost a run in
         * tests/isp-develop; making it twice is what moved it into a comment.
         */
        if (params.fd > 0) stream(&params, 1);
        if (stats.fd > 0)  stream(&stats, 1);
        stream(&in0, 1);
        stream(&frame, 1);
        STAGE("all four nodes streaming");
        fprintf(rc, "out_format %.4s stride %u sizeimage %u\n",
                (char *)&frame.fmt.fmt.pix_mp.pixelformat,
                frame.fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
                frame.fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
        fflush(rc);
    }

    for (i = 0; i < nfiles; i++) {
        size_t got;
        FILE *f;
        uint64_t hin, hout = 0;

        snprintf(path, sizeof path, "%s/%s", in_dir, names[i]);
        FSTAGE("frame %d: opening %s", i, names[i]);
        f = fopen(path, "rb");
        if (!f) { fprintf(rc, "REJECT %s unreadable\n", names[i]); rejected++; continue; }
        FSTAGE("frame %d: opened", i);

        if (!is_raw) {
            /* transport only: the sensor already developed these. The identity
             * check is a HASH, not a 9 GB copy - same oracle, no disk. */
            static unsigned char sbuf[4 << 20];
            got = fread(sbuf, 1, sizeof sbuf, f);
            fclose(f);
            hin = fnv1a(sbuf, got);
            if (copy_smart) {
                snprintf(opath, sizeof opath, "%s/%s", out_dir, names[i]);
                FILE *o = fopen(opath, "wb");
                if (o) { fwrite(sbuf, 1, got, o); fclose(o); }
            }
            fprintf(rc, "OK %s path=smart bytes=%zu hash=%016llx copied=%d\n",
                    names[i], got, (unsigned long long)hin, copy_smart);
            ok++;
            if (blit_every && (i % blit_every) == 0) {
                fb_show(fbdev, sbuf, W, H, 2); blits++;
            }
            continue;
        }

        /*
         * Read into ORDINARY MEMORY, then copy into the V4L2 buffer.
         *
         * Reading a 9p file directly into an mmap'd V4L2 DMA buffer returns
         * zero bytes: the same fread from the initramfs works, so it is the
         * combination of that filesystem and a device mapping as the
         * destination, not the file and not the buffer. A bounce buffer costs
         * one memcpy per 300 KB frame and removes the question entirely -
         * which is worth more here than being right about whose bug it is.
         */
        /*
         * A 9p TRANSFER MAY NOT USE AN mmap'd V4L2 BUFFER AS ITS USER-MEMORY
         * ENDPOINT, IN EITHER DIRECTION. That is the single root cause behind
         * both halves of this function, and it took a stage trace to see:
         *
         *   read INTO frame buffer  -> returns 0 bytes, silently
         *   write FROM frame buffer -> "netfs: Couldn't get user pages
         *                              (rc=-14)" retried forever, a hang
         *
         * netfs pins the user pages with get_user_pages; a V4L2 mapping is
         * VM_IO/VM_PFNMAP and cannot be pinned, so the transfer fails at the
         * mapping, NOT at the file, the mount, or the size of the request.
         * I twice blamed something else that merely correlated - the source
         * filesystem, then the read size - and both survived a plausible
         * argument and died on the trace. Bounce through ordinary memory in
         * BOTH directions and the whole class disappears.
         *
         * The zero-progress guard stays regardless: a read that stops
         * advancing must END this frame, not spin. A hang reports nothing;
         * a REJECT names the file.
         */
        got = 0;
        while (got < in0.len[0]) {
            size_t want = in0.len[0] - got;
            size_t n;
            if (want > (64u << 10)) want = 64u << 10;
            n = fread(bounce + got, 1, want, f);
            if (n == 0) break;          /* EOF or error - both end the frame */
            got += n;
        }
        if (ferror(f)) {
            fprintf(rc, "REJECT %s read error: %s\n", names[i], strerror(errno));
            fclose(f); rejected++; continue;
        }
        fclose(f);
        if (got < in0.len[0]) {
            fprintf(rc, "REJECT %s short read %zu of %zu\n", names[i], got, in0.len[0]);
            rejected++; continue;
        }
        FSTAGE("frame %d: read %zu bytes", i, got);
        memcpy(in0.buf[0], bounce, got);
        FSTAGE("frame %d: copied into V4L2 buffer", i);
        hin = fnv1a(in0.buf[0], got);

        /*
         * QBUF failures are REPORTED, not ignored. Silently dropping them is
         * what hid this bug: the metadata re-queue was failing every frame
         * after the first and the only symptom was a DQBUF that never
         * returned, three layers away from the cause.
         */
        if (params.fd > 0) {
            memset(params.buf[0], 0, params.len[0]);
            if (qbuf(&params, 0) < 0)
                fprintf(rc, "WARN %s params QBUF: %s\n", names[i], strerror(errno));
        }
        if (stats.fd > 0 && qbuf(&stats, 0) < 0)
            fprintf(rc, "WARN %s stats QBUF: %s\n", names[i], strerror(errno));
        qbuf(&frame, 0);
        qbuf(&in0, got);
        FSTAGE("frame %d: queued, waiting on DQBUF", i);
        if (dqbuf(&frame) < 0) {
            fprintf(rc, "REJECT %s develop failed: %s\n", names[i], strerror(errno));
            rejected++; continue;
        }
        FSTAGE("frame %d: DQBUF returned", i);
        dqbuf(&in0);
        /*
         * DEQUEUE THE METADATA NODES TOO, EVERY FRAME.
         *
         * params and stats are per-job buffers like the image ones. Queue them
         * and never take them back and the driver still owns them on the next
         * frame: the re-queue fails, the job has no parameter buffer, and
         * DQBUF blocks forever. Frame 0 succeeds, frame 1 hangs - which reads
         * like an intermittent fault and is in fact a missing dequeue.
         */
        if (params.fd > 0) dqbuf(&params);
        if (stats.fd > 0) dqbuf(&stats);
        FSTAGE("frame %d: metadata dequeued", i);
        hout = fnv1a(frame.buf[0], frame.len[0]);

        /* output name derives from the input name: diffing against the capture
         * is the entire point of processing someone's frames */
        snprintf(opath, sizeof opath, "%s/%s.bgr", out_dir, names[i]);
        f = fopen(opath, "wb");
        if (!f) { fprintf(rc, "REJECT %s cannot write output\n", names[i]); rejected++; continue; }
        /* out of the V4L2 mapping first - see the endpoint note above */
        memcpy(obounce, frame.buf[0], frame.len[0]);
        fwrite(obounce, 1, frame.len[0], f);
        fclose(f);
        fprintf(rc, "OK %s path=raw->isp in_hash=%016llx out=%s.bgr out_hash=%016llx\n",
                names[i], (unsigned long long)hin, names[i], (unsigned long long)hout);
        ok++;
        if (blit_every && (i % blit_every) == 0) {
            fb_show(fbdev, frame.buf[0], W, H, 3); blits++;
        }
        /* progress on the console: a stalled batch should look stalled, not
         * merely quiet, and at 5,000 frames nobody watches a silent run */
        printf("BATCH %d/%d %s\n", i + 1, nfiles, names[i]);
        fflush(stdout);
        if ((i % 100) == 0) fflush(rc);
    }

    fprintf(rc, "counts in=%d ok=%d rejected=%d blitted=%d\n",
            nfiles, ok, rejected, blits);
    /* THE COMPLETION MARKER IS WRITTEN LAST. Its absence means the run did not
     * finish - which a folder full of outputs cannot tell you by itself. */
    fprintf(rc, "COMPLETE\n");
    fclose(rc);
    printf("BATCH done in=%d ok=%d rejected=%d blitted=%d\n", nfiles, ok, rejected, blits);
    printf("BATCH COMPLETE\n");
    return rejected ? 1 : 0;
}
