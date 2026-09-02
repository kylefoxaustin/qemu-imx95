// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * neoisp_m2m - push a raw Bayer frame through the i.MX95 NeoISP and get a
 * developed image back, through the real V4L2 mem2mem path.
 *
 * The qtest proves the MODEL develops a frame when its registers are poked
 * directly. This proves the DRIVER reaches that path: node discovery, format
 * negotiation, buffer allocation, the queue dance and the completion
 * interrupt, exactly as a real application would drive it.
 *
 * The block needs input0 streaming, plus any node whose media link is enabled
 * (params, frame). Rather than perform link surgery we simply feed all of
 * them - a zero-filled params buffer is enough, since tuning parameters do not
 * change whether a frame is produced.
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
#include <linux/videodev2.h>

#define MAXP 4

struct node {
    int fd;
    char name[32];
    int type;                   /* buffer type */
    void *buf[MAXP];
    size_t len[MAXP];
    unsigned nplanes;
    struct v4l2_format fmt;
};

static uint64_t fnv1a(const unsigned char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    while (n--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

/*
 * Read the sysfs name of /dev/videoN, or "" if there is none.
 */
static void node_name(int n, char *out, size_t outn)
{
    char np[96];
    FILE *f;

    out[0] = 0;
    snprintf(np, sizeof np, "/sys/class/video4linux/video%d/name", n);
    f = fopen(np, "r");
    if (!f) {
        return;
    }
    if (fgets(out, outn, f)) {
        out[strcspn(out, "\r\n")] = 0;
    }
    fclose(f);
}

/*
 * Find the FIRST node-group: the driver registers EIGHT groups of six nodes
 * each (input0, input1, params, frame, ir, stats) and every group uses the
 * SAME names. Matching on the name alone therefore picks an arbitrary group
 * per node - which is how input0 ended up in one group, params in a second and
 * frame in a third, leaving no group with a complete set and the job never
 * becoming ready. A job must be assembled from ONE group.
 */
static int find_group_base(void)
{
    char nm[64];
    int n;

    for (n = 0; n < 256; n++) {
        node_name(n, nm, sizeof nm);
        if (!strcmp(nm, "neoisp-input0")) {
            /* confirm the siblings sit where the group layout says they do */
            char f[64], p[64];
            node_name(n + 2, p, sizeof p);
            node_name(n + 3, f, sizeof f);
            if (!strcmp(p, "neoisp-params") && !strcmp(f, "neoisp-frame")) {
                return n;
            }
        }
    }
    return -1;
}

/*
 * Find /dev/videoN by NODE name, e.g. "neoisp-input0".
 *
 * Not by VIDIOC_QUERYCAP card: every node of this device reports the same card
 * string, so matching on it would open an arbitrary one of the 48 and then fail
 * confusingly downstream. The per-node name lives in sysfs, which is where the
 * driver publishes the distinction that matters here.
 */
static int open_named(const char *want, char *out, size_t outn)
{
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    int fd = -1;

    if (!d) return -1;
    while ((e = readdir(d))) {
        char np[320], path[288], nm[64] = {0};
        FILE *f;

        if (strncmp(e->d_name, "video", 5)) continue;
        snprintf(np, sizeof np, "/sys/class/video4linux/%s/name", e->d_name);
        f = fopen(np, "r");
        if (!f) continue;
        if (fgets(nm, sizeof nm, f)) {
            nm[strcspn(nm, "\r\n")] = 0;
        }
        fclose(f);
        if (strcmp(nm, want)) continue;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        fd = open(path, O_RDWR);
        if (fd >= 0 && out) {
            snprintf(out, outn, "%s", path);
        }
        break;
    }
    closedir(d);
    return fd;
}

static int setup(struct node *n, const char *name, int type,
                 int w, int h, unsigned fourcc, int vidnum)
{
    struct v4l2_requestbuffers req = {0};
    struct v4l2_buffer b = {0};
    struct v4l2_plane pl[MAXP];
    char path[288];
    unsigned i;

    if (vidnum >= 0) {
        char nm[64];
        node_name(vidnum, nm, sizeof nm);
        if (strcmp(nm, name)) {
            fprintf(stderr, "video%d is '%s', expected '%s'\n", vidnum, nm, name);
            return -1;
        }
        snprintf(path, sizeof path, "/dev/video%d", vidnum);
        n->fd = open(path, O_RDWR);
    } else {
        n->fd = open_named(name, path, sizeof path);
    }
    if (n->fd < 0) { fprintf(stderr, "no node '%s'\n", name); return -1; }
    snprintf(n->name, sizeof n->name, "%s", name);
    n->type = type;

    memset(&n->fmt, 0, sizeof n->fmt);
    n->fmt.type = type;
    if (ioctl(n->fd, VIDIOC_G_FMT, &n->fmt) < 0) {
        fprintf(stderr, "%s: G_FMT: %s\n", name, strerror(errno));
        return -1;
    }
    if (w) {                        /* image nodes: ask for our geometry */
        n->fmt.fmt.pix_mp.width = w;
        n->fmt.fmt.pix_mp.height = h;
        n->fmt.fmt.pix_mp.pixelformat = fourcc;
        if (ioctl(n->fd, VIDIOC_S_FMT, &n->fmt) < 0) {
            fprintf(stderr, "%s: S_FMT: %s\n", name, strerror(errno));
            return -1;
        }
    }
    if (type == V4L2_BUF_TYPE_META_OUTPUT ||
        type == V4L2_BUF_TYPE_META_CAPTURE) {
        /* a metadata node describes itself in fmt.meta - reading pix_mp here
         * yields garbage geometry and then a confusing failure downstream */
        n->nplanes = 1;
        printf("%-15s %s meta fourcc=%.4s size=%u\n", name, path,
               (char *)&n->fmt.fmt.meta.dataformat, n->fmt.fmt.meta.buffersize);
    } else {
        n->nplanes = n->fmt.fmt.pix_mp.num_planes ?
                     n->fmt.fmt.pix_mp.num_planes : 1;
        printf("%-15s %s %ux%u fourcc=%.4s planes=%u size=%u\n", name, path,
               n->fmt.fmt.pix_mp.width, n->fmt.fmt.pix_mp.height,
               (char *)&n->fmt.fmt.pix_mp.pixelformat, n->nplanes,
               n->fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    }

    req.count = 1; req.type = type; req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(n->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "%s: REQBUFS: %s\n", name, strerror(errno));
        return -1;
    }
    memset(pl, 0, sizeof pl);
    b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
    if (type == V4L2_BUF_TYPE_META_OUTPUT || type == V4L2_BUF_TYPE_META_CAPTURE) {
        if (ioctl(n->fd, VIDIOC_QUERYBUF, &b) < 0) {
            fprintf(stderr, "%s: QUERYBUF(meta): %s\n", name, strerror(errno));
            return -1;
        }
        n->len[0] = b.length;
        n->buf[0] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         n->fd, b.m.offset);
        if (n->buf[0] == MAP_FAILED) { perror("mmap meta"); return -1; }
        return 0;
    }
    b.m.planes = pl; b.length = MAXP;
    if (ioctl(n->fd, VIDIOC_QUERYBUF, &b) < 0) {
        fprintf(stderr, "%s: QUERYBUF: %s\n", name, strerror(errno));
        return -1;
    }
    for (i = 0; i < n->nplanes; i++) {
        n->len[i] = pl[i].length;
        n->buf[i] = mmap(NULL, pl[i].length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, n->fd, pl[i].m.mem_offset);
        if (n->buf[i] == MAP_FAILED) { perror("mmap"); return -1; }
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
        b.bytesused = n->len[0];
        b.length = n->len[0];
        return ioctl(n->fd, VIDIOC_QBUF, &b);
    }
    b.m.planes = pl; b.length = n->nplanes;
    for (i = 0; i < n->nplanes; i++) {
        pl[i].bytesused = used ? used : n->len[i];
        pl[i].length = n->len[i];
    }
    return ioctl(n->fd, VIDIOC_QBUF, &b);
}

static int stream_on(struct node *n)
{
    int t = n->type;
    return ioctl(n->fd, VIDIOC_STREAMON, &t);
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

int main(int argc, char **argv)
{
    const char *bayer_file = argc > 1 ? argv[1] : "/bayer.raw";
    const char *out_file = argc > 2 ? argv[2] : "/developed.raw";
    int W = argc > 3 ? atoi(argv[3]) : 640;
    int H = argc > 4 ? atoi(argv[4]) : 480;
    struct node in0 = {0}, params = {0}, frame = {0};
    FILE *f;
    size_t got;
    int base;

    printf("NEOISP-M2M start %dx%d\n", W, H);

    base = find_group_base();
    if (base < 0) { fprintf(stderr, "no complete neoisp node group\n"); return 1; }
    printf("NEOISP-M2M node group base=/dev/video%d\n", base);

    if (setup(&in0, "neoisp-input0", V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
              W, H, V4L2_PIX_FMT_SBGGR8, base) < 0) return 1;
    if (setup(&frame, "neoisp-frame", V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
              W, H, V4L2_PIX_FMT_RGB32, base + 3) < 0) return 1;
    /* params is a metadata node: take whatever format it reports */
    if (setup(&params, "neoisp-params", V4L2_BUF_TYPE_META_OUTPUT, 0, 0, 0,
              base + 2) < 0) {
        printf("NEOISP-M2M: no params node in use\n");
    }

    /* load the Bayer frame into the input buffer */
    f = fopen(bayer_file, "rb");
    if (!f) { perror("open bayer"); return 1; }
    got = fread(in0.buf[0], 1, in0.len[0], f);
    fclose(f);
    printf("NEOISP-M2M loaded %zu bytes of Bayer (buffer %zu)\n", got, in0.len[0]);
    printf("NEOISP-M2M bayer-hash %016llx\n",
           (unsigned long long)fnv1a(in0.buf[0], got));

    if (params.fd > 0) {
        memset(params.buf[0], 0, params.len[0]);   /* default tuning */
        if (qbuf(&params, 0) < 0) perror("params QBUF");
    }
    if (qbuf(&in0, got) < 0) { perror("input0 QBUF"); return 1; }
    if (qbuf(&frame, 0) < 0) { perror("frame QBUF"); return 1; }

    if (params.fd > 0 && stream_on(&params) < 0) perror("params STREAMON");
    if (stream_on(&in0) < 0) { perror("input0 STREAMON"); return 1; }
    if (stream_on(&frame) < 0) { perror("frame STREAMON"); return 1; }

    if (dqbuf(&frame) < 0) { perror("frame DQBUF"); return 1; }
    printf("NEOISP-M2M developed-hash %016llx\n",
           (unsigned long long)fnv1a(frame.buf[0], frame.len[0]));

    f = fopen(out_file, "wb");
    if (f) { fwrite(frame.buf[0], 1, frame.len[0], f); fclose(f); }
    printf("NEOISP-M2M wrote %s (%zu bytes)\n", out_file, frame.len[0]);
    printf("NEOISP-M2M PASS\n");
    return 0;
}
