// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cam_isp_chain - the i.MX95 vision pipeline end to end, in one process.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * Raw Bayer arrives over MIPI CSI-2, the ISI DMAs it into a capture buffer in
 * DRAM, and the NeoISP READS THAT SAME DRAM and develops it to colour:
 *
 *   ov5640 -> CSI-2 -> ISI -> [DRAM capture buffer] -> NeoISP -> developed RGB
 *                               ^                      ^
 *                               |                      |
 *                      VIDIOC_EXPBUF (dmabuf)   queued as the ISP's
 *                      exported from the ISI    input0 buffer
 *
 * WHY DMABUF AND NOT A memcpy. There is no ISI->ISP wire on this SoC, and
 * there should not be: on real silicon the ISI writes DRAM and the ISP, a V4L2
 * mem2mem device, reads DRAM. The join IS memory. A userspace memcpy between
 * the two buffers would prove transport but would put our own hands on every
 * byte, so it could not distinguish "the ISP read what the ISI wrote" from
 * "we re-typed what the ISI wrote into a buffer the ISP happened to read".
 * Exporting the ISI's buffer and queueing that exact dmabuf as the ISP's input
 * makes the two devices share one physical buffer, which is the claim.
 *
 * Both queues use vb2_dma_contig_memops, so the export is one physically
 * contiguous region and the ISP's single IMG0_IN_ADDR can address it.
 *
 * Usage: cam_isp_chain <nframes> <W> <H> <outprefix>
 * Prints, per frame, the captured Bayer hash and the developed hash, so the
 * host can tell WHICH staged frame it got (the ISI cycles its host-frame
 * source on its own timer; the guest cannot pick) and correlate the developed
 * output against that frame's own reference.
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
#include <linux/media.h>
#include <linux/v4l2-subdev.h>

#define MAXP      4
#define ISI_NBUF  4
#define CAP_TYPE  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define OUT_TYPE  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE

/*
 * MEDIA_BUS_FMT_SRGGB8_1X8 / V4L2_PIX_FMT_SRGGB8.
 *
 * The Bayer PHASE does not come from the fourcc: the neoisp driver's format
 * table has no phase field (its SRGGB8 and SBGGR8 entries are identical), so
 * DEMOSAIC_CTRL.FMT comes from the params buffer, and a zero-filled params
 * buffer means FMT=0, which the model reads as RGGB. The mosaic we stage is
 * therefore generated RGGB to match. The fourcc still matters for stride and
 * for link validation, just not for which colour sits at pixel (0,0).
 */
#define BAYER_CODE   0x3014
#define BAYER_FOURCC V4L2_PIX_FMT_SRGGB8

/*
 * Overridable so the harness can run a CONTROL: the same program, the same
 * pipeline setup, on a format already known to stream (UYVY). If the control
 * captures and the Bayer run does not, the difference is the format rather
 * than this program - and that is a different bug to go looking for.
 *   CHAIN_CODE=0x200f CHAIN_FOURCC=YUYV CHAIN_CAPTURE_ONLY=1
 */
static unsigned g_code = BAYER_CODE;
static unsigned g_fourcc = BAYER_FOURCC;
static int g_capture_only;
static int g_strict_stride = 1;      /* only 8bpp Bayer has bytesperline == w */

/*
 * Bisect knobs. tests/camera/run.sh streams this same graph, so when this
 * program does not, the difference is in what this program does ON TOP of
 * what that one does. Each knob removes one of those differences:
 *   CHAIN_SET_SOURCES=1   also write source pads (v4l2_cap does not)
 *   CHAIN_SKIP_CROSSBAR=1 leave the crossbar's pads alone
 *   CHAIN_ONE_PIPE=1      set only the ISI pipe this capture uses
 *   CHAIN_NO_CAP_SFMT=1   do not force pixelformat/bytesperline/num_planes
 *   CHAIN_NO_EXPBUF=1     do not export dmabufs
 */
static int g_set_sources;   /* CHAIN_SET_SOURCES=1  also write source pads */
static int g_skip_crossbar; /* CHAIN_SKIP_CROSSBAR=1 leave the crossbar alone */
static int g_one_pipe;      /* CHAIN_ONE_PIPE=1     set only the pipe in use */
static int g_no_cap_sfmt;
static int g_no_expbuf;
static int g_no_sensor_fmt;

static void read_env(void)
{
    const char *s;

    s = getenv("CHAIN_CODE");
    if (s && *s) {
        g_code = (unsigned)strtoul(s, NULL, 0);
    }
    s = getenv("CHAIN_FOURCC");
    if (s && strlen(s) == 4) {
        g_fourcc = v4l2_fourcc(s[0], s[1], s[2], s[3]);
        g_strict_stride = 0;
    }
    s = getenv("CHAIN_CAPTURE_ONLY");
    if (s && *s == '1') {
        g_capture_only = 1;
    }
    s = getenv("CHAIN_SET_SOURCES");
    g_set_sources = (s && *s == '1');
    s = getenv("CHAIN_SKIP_CROSSBAR");
    g_skip_crossbar = (s && *s == '1');
    s = getenv("CHAIN_ONE_PIPE");
    g_one_pipe = (s && *s == '1');
    s = getenv("CHAIN_NO_CAP_SFMT");
    g_no_cap_sfmt = (s && *s == '1');
    if (g_no_cap_sfmt) {
        /* not forcing the format means the stride is whatever the driver
         * chose, so the Bayer-only bytesperline == width rule cannot apply */
        g_strict_stride = 0;
    }
    s = getenv("CHAIN_NO_EXPBUF");
    g_no_expbuf = (s && *s == '1');
    s = getenv("CHAIN_NO_SENSOR_FMT");
    g_no_sensor_fmt = (s && *s == '1');
    if (g_set_sources || g_skip_crossbar || g_one_pipe || g_no_cap_sfmt ||
        g_no_expbuf || g_no_sensor_fmt) {
        printf("CHAIN BISECT: set_sources=%d skip_crossbar=%d one_pipe=%d "
               "no_cap_sfmt=%d no_expbuf=%d no_sensor_fmt=%d\n",
               g_set_sources, g_skip_crossbar, g_one_pipe, g_no_cap_sfmt,
               g_no_expbuf, g_no_sensor_fmt);
    }
    if (g_code != BAYER_CODE || g_fourcc != BAYER_FOURCC) {
        printf("CHAIN CONTROL MODE: code=0x%04x fourcc=%.4s capture_only=%d\n",
               g_code, (char *)&g_fourcc, g_capture_only);
    }
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static uint64_t fnv1a(const unsigned char *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (n--) {
        h ^= *p++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void node_name(const char *entry, char *out, size_t outn)
{
    char np[320];
    FILE *f;

    out[0] = 0;
    snprintf(np, sizeof np, "/sys/class/video4linux/%s/name", entry);
    f = fopen(np, "r");
    if (!f) {
        return;
    }
    if (fgets(out, outn, f)) {
        out[strcspn(out, "\r\n")] = 0;
    }
    fclose(f);
}

static void node_name_n(int n, char *out, size_t outn)
{
    char e[32];

    snprintf(e, sizeof e, "video%d", n);
    node_name(e, out, outn);
}

/*
 * Open the ISI capture node. Its name is "mxc_isi.<pipe>.capture", one per
 * channel, so match the shape rather than a fixed minor: which /dev/videoN the
 * ISI lands on depends on how many nodes the neoisp registered first (48 of
 * them), and that has changed between kernels.
 */
/* The lowest-numbered ISI pipe, or -1. */
static int isi_find_pipe(void)
{
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    int best = -1;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d))) {
        char nm[64];
        int pipe = -1;

        if (strncmp(e->d_name, "video", 5)) {
            continue;
        }
        node_name(e->d_name, nm, sizeof nm);
        if (sscanf(nm, "mxc_isi.%d.capture", &pipe) != 1 || pipe < 0) {
            continue;
        }
        if (best < 0 || pipe < best) {
            best = pipe;
        }
    }
    closedir(d);
    return best;
}

static int open_isi_capture(char *path, size_t pathn)
{
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    char best_entry[256] = {0};
    int best_pipe = -1;
    int fd = -1;

    if (!d) {
        return -1;
    }
    /*
     * Take the LOWEST-numbered pipe, not the first one readdir happens to
     * yield. The crossbar routes the camera to pipe 0, so grabbing an
     * arbitrary pipe gets a node that opens, accepts a format and exports
     * buffers perfectly - and then fails STREAMON with "Failed to enable
     * pipe 7", because nothing is routed to it.
     */
    while ((e = readdir(d))) {
        char nm[64];
        int pipe = -1;

        if (strncmp(e->d_name, "video", 5)) {
            continue;
        }
        node_name(e->d_name, nm, sizeof nm);
        if (sscanf(nm, "mxc_isi.%d.capture", &pipe) != 1 || pipe < 0) {
            continue;
        }
        if (best_pipe < 0 || pipe < best_pipe) {
            best_pipe = pipe;
            snprintf(best_entry, sizeof best_entry, "%s", e->d_name);
        }
    }
    closedir(d);

    if (best_pipe < 0) {
        return -1;
    }
    snprintf(path, pathn, "/dev/%s", best_entry);
    fd = open(path, O_RDWR);
    printf("CHAIN isi capture node %s (pipe %d)\n", path, best_pipe);
    return fd;
}

static int subdev_path_for_minor(unsigned minor, char *out, size_t outn)
{
    DIR *d = opendir("/sys/class/video4linux");
    struct dirent *e;
    int found = -1;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d))) {
        char np[320], buf[32] = {0};
        FILE *f;
        unsigned m;

        if (strncmp(e->d_name, "v4l-subdev", 10)) {
            continue;
        }
        snprintf(np, sizeof np, "/sys/class/video4linux/%s/dev", e->d_name);
        f = fopen(np, "r");
        if (!f) {
            continue;
        }
        if (fgets(buf, sizeof buf, f) && sscanf(buf, "%*u:%u", &m) == 1 &&
            m == minor) {
            snprintf(out, outn, "/dev/%s", e->d_name);
            found = 0;
        }
        fclose(f);
        if (!found) {
            break;
        }
    }
    closedir(d);
    return found;
}

/*
 * Put the whole graph into a Bayer format at WxH.
 *
 * Two steps, and the first one is the one the UYVY capture test did not need:
 * set the SENSOR's source pad to the Bayer code, because everything downstream
 * is then made to agree with it. Then write W/H/code onto every sink pad of
 * every subdev, because v4l2_subdev_link_validate compares width, height AND
 * code across each enabled link and fails STREAMON with -EPIPE on any
 * mismatch - logging only at dev_dbg, which the BSP does not enable.
 */
static int setup_pipeline(const char *mdev, unsigned w, unsigned h,
                          int pipe_id)
{
    struct media_entity_desc e;
    int fd = open(mdev, O_RDWR);
    __u32 id;
    int sensor_id = -1, rc;

    if (fd < 0) {
        printf("CHAIN FAIL: open %s: %s\n", mdev, strerror(errno));
        return -1;
    }

    /* find the camera sensor entity */
    for (id = 0;; ) {
        memset(&e, 0, sizeof e);
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) < 0) {
            break;
        }
        id = e.id;
        /*
         * e.type is the entity FUNCTION, compared whole. Masking it with
         * MEDIA_ENT_TYPE_MASK first (0x00ff0000) strips the low bits of
         * MEDIA_ENT_F_CAM_SENSOR (0x00020001), so the comparison can never be
         * true and the sensor is never found.
         */
        if (e.type == MEDIA_ENT_F_CAM_SENSOR || strstr(e.name, "ov5640")) {
            sensor_id = e.id;
        }
    }
    if (sensor_id < 0) {
        printf("CHAIN FAIL: no camera sensor entity in %s\n", mdev);
        close(fd);
        return -1;
    }

    /* 1. the sensor source pad -> Bayer (skippable, for bisection) */
    memset(&e, 0, sizeof e);
    e.id = sensor_id;
    if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) == 0) {
        char path[288];

        if (subdev_path_for_minor(e.dev.minor, path, sizeof path) == 0) {
            int sd = open(path, O_RDWR);
            struct v4l2_subdev_format sf;

            if (sd >= 0) {
                memset(&sf, 0, sizeof sf);
                sf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
                sf.pad = 0;
                sf.format.width = w;
                sf.format.height = h;
                sf.format.code = g_code;
                sf.format.field = V4L2_FIELD_NONE;
                rc = g_no_sensor_fmt
                   ? xioctl(sd, VIDIOC_SUBDEV_G_FMT, &sf)
                   : xioctl(sd, VIDIOC_SUBDEV_S_FMT, &sf);
                printf("CHAIN sensor '%s' %s S_FMT %ux%u/0x%04x -> %s "
                       "(got %ux%u/0x%04x)\n", e.name, path, w, h, g_code,
                       rc == 0 ? "ok" : strerror(errno),
                       sf.format.width, sf.format.height, sf.format.code);
                if (g_no_sensor_fmt && rc == 0) {
                    g_code = sf.format.code;
                    printf("CHAIN adopting sensor's own %ux%u/0x%04x\n",
                           sf.format.width, sf.format.height, sf.format.code);
                }
                if (!g_no_sensor_fmt && rc == 0 && sf.format.code != g_code) {
                    printf("CHAIN FAIL: sensor refused the Bayer code "
                           "(gave 0x%04x) - it cannot source raw\n",
                           sf.format.code);
                    close(sd);
                    close(fd);
                    return -1;
                }
                /*
                 * The geometry must be EXACT, because everything downstream is
                 * then set to it and link_validate compares width and height
                 * too. The ov5640 driver snaps any 8bpp (raw Bayer) request
                 * below 1280 wide up to 720p:
                 *
                 *   if (bpp == 8 && mode->width < 1280)
                 *           mode = &ov5640_mode_data[OV5640_MODE_720P_1280_720];
                 *
                 * so a 640x480 Bayer request silently becomes 1280x720 and the
                 * only visible symptom is -EPIPE at STREAMON. Say it instead.
                 */
                if (!g_no_sensor_fmt && rc == 0 &&
                    (sf.format.width != w || sf.format.height != h)) {
                    printf("CHAIN FAIL: sensor snapped %ux%u to %ux%u. Raw "
                           "Bayer is 8bpp and this sensor forces >=1280x720 "
                           "for 8bpp; re-run with W=%u H=%u\n",
                           w, h, sf.format.width, sf.format.height,
                           sf.format.width, sf.format.height);
                    close(sd);
                    close(fd);
                    return -1;
                }
                close(sd);
            }
        }
    }

    /*
     * 2. ENABLE the links that ship disabled.
     *
     * The ov5640 -> CSI-2 link is created with flags = 0 (neither ENABLED nor
     * IMMUTABLE), so without this pass media_pipeline_start() cannot build a
     * pipeline and STREAMON returns -EPIPE. There is no message: the pipe
     * never gets as far as mxc_isi_pipe_enable, so even "Failed to enable
     * pipe N" is absent, and -EPIPE looks exactly like a format mismatch
     * while every format in the graph agrees. Enumerate each entity's links
     * and enable the ones it SOURCES.
     */
    for (id = 0;; ) {
        struct media_pad_desc pads[16];
        struct media_link_desc links[32];
        struct media_links_enum le;
        int k;

        memset(&e, 0, sizeof e);
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) < 0) {
            break;
        }
        id = e.id;
        memset(pads, 0, sizeof pads);
        memset(links, 0, sizeof links);
        memset(&le, 0, sizeof le);
        le.entity = e.id;
        le.pads = pads;
        le.links = links;
        if (xioctl(fd, MEDIA_IOC_ENUM_LINKS, &le) < 0) {
            continue;
        }
        for (k = 0; k < e.links && k < 32; k++) {
            struct media_link_desc *l = &links[k];

            if ((l->flags & MEDIA_LNK_FL_ENABLED) ||
                (l->flags & MEDIA_LNK_FL_IMMUTABLE)) {
                continue;
            }
            if (l->source.entity != e.id) {
                continue;
            }
            l->flags |= MEDIA_LNK_FL_ENABLED;
            printf("CHAIN   link %u/%u -> %u/%u enable -> %s\n",
                   l->source.entity, l->source.index,
                   l->sink.entity, l->sink.index,
                   xioctl(fd, MEDIA_IOC_SETUP_LINK, l) == 0
                       ? "ok" : strerror(errno));
        }
    }

    /* 3. propagate onto every sink pad in the graph */
    for (id = 0;; ) {
        struct media_pad_desc pads[16];
        struct media_link_desc links[32];
        struct media_links_enum le;
        char path[288];
        int sd, k;

        memset(&e, 0, sizeof e);
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) < 0) {
            break;
        }
        id = e.id;
        if (e.id == (unsigned)sensor_id) {
            continue;                   /* already set, and it has no sink */
        }
        if (subdev_path_for_minor(e.dev.minor, path, sizeof path) != 0) {
            continue;                   /* not a subdev (a video node) */
        }
        /*
         * EVERY subdev sink pad in the graph, the crossbar's included.
         *
         * The crossbar propagates sink -> source along its ACTIVE ROUTE, so
         * its routed sink pad is what moves its source pad; the other sinks
         * return EINVAL, which is the expected answer and not an error.
         *
         * Skipping the crossbar entirely - which this program did at first,
         * on the theory that touching its pads destroys the route - leaves
         * its source pad at the 1920x1080 power-on default while the ISI sink
         * sits at the negotiated geometry, and STREAMON fails -EPIPE. With
         * CONFIG_DYNAMIC_DEBUG=y the kernel says so itself:
         *
         *   v4l2_subdev_link_validate_default: width does not match
         *                                      (source 1920, sink 640)
         *   link was "crossbar":5 -> "mxc_isi.0":0
         *   Link 'crossbar':5 -> 'mxc_isi.0':0 failed validation: -32
         *
         * With it off that is a bare -EPIPE and nothing else, which is how
         * this cost a day. The userspace link oracle is blind to it too: it
         * reads crossbar:5 through G_FMT, which answers from the route, and
         * sees it agree with the sink it is compared against.
         *
         * What survives of the earlier theory is narrower and still true:
         * writing the crossbar's SOURCE pads is what breaks routing. Sinks
         * only - which is also what tests/camera/v4l2_cap.c does, and that
         * program streams this graph.
         */
        if (g_skip_crossbar && !strcmp(e.name, "crossbar")) {
            continue;
        }
        /*
         * All eight ISI pipes, not only the one in use, again matching
         * v4l2_cap. Setting sink pads alone keeps every pipe's source format
         * equal to what its own video node derived, so the seven unused pipes
         * cost nothing; it is writing their SOURCE pads that invalidates
         * seven video-node links this capture never touches.
         */
        if (g_one_pipe) {
            int this_pipe = -1;

            if (sscanf(e.name, "mxc_isi.%d", &this_pipe) == 1 &&
                this_pipe != pipe_id) {
                continue;
            }
        }
        memset(pads, 0, sizeof pads);
        memset(links, 0, sizeof links);
        memset(&le, 0, sizeof le);
        le.entity = e.id;
        le.pads = pads;
        le.links = links;
        if (xioctl(fd, MEDIA_IOC_ENUM_LINKS, &le) < 0) {
            continue;
        }
        sd = open(path, O_RDWR);
        if (sd < 0) {
            continue;
        }
        for (k = 0; k < e.pads && k < 16; k++) {
            struct v4l2_subdev_format pf;

            /*
             * Sink pads only. The ISI pipe propagates the sink format to its
             * source itself (mxc_isi_pipe_set_fmt: "the sink pad bus format
             * will be propagated to the source pad"), and for RAW it forces
             * source code == info->output, so writing the source pad cannot
             * help a Bayer bypass and can only disagree with what the driver
             * derived.
             */
            if (!g_set_sources && !(pads[k].flags & MEDIA_PAD_FL_SINK)) {
                continue;
            }
            memset(&pf, 0, sizeof pf);
            pf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            pf.pad = pads[k].index;
            pf.format.width = w;
            pf.format.height = h;
            pf.format.code = g_code;
            pf.format.field = V4L2_FIELD_NONE;
            rc = xioctl(sd, VIDIOC_SUBDEV_S_FMT, &pf);
            printf("CHAIN   ent %u '%s' pad %u (%s) S_FMT -> %s\n",
                   e.id, e.name, pads[k].index,
                   (pads[k].flags & MEDIA_PAD_FL_SINK) ? "sink" : "source",
                   rc == 0 ? "ok" : strerror(errno));
        }
        close(sd);
    }

    close(fd);
    return 0;
}

/* ---------------- the ISI capture side ---------------- */

struct isi {
    int fd;
    unsigned nbuf;
    void *mem[ISI_NBUF];
    size_t len[ISI_NBUF];
    int dmafd[ISI_NBUF];
    unsigned bpl, sizeimage;
};

static int isi_start(struct isi *c, unsigned w, unsigned h)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    char path[288];
    unsigned i;
    int t = CAP_TYPE;

    c->fd = open_isi_capture(path, sizeof path);
    if (c->fd < 0) {
        printf("CHAIN FAIL: no mxc_isi capture node\n");
        return -1;
    }

    memset(&fmt, 0, sizeof fmt);
    fmt.type = CAP_TYPE;
    if (xioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0) {
        printf("CHAIN FAIL: isi G_FMT: %s\n", strerror(errno));
        return -1;
    }
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    if (!g_no_cap_sfmt) {
        fmt.fmt.pix_mp.pixelformat = g_fourcc;
        fmt.fmt.pix_mp.num_planes = 1;
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline = w;  /* 8bpp Bayer: 1 B/px */
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = w * h;
    }
    if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("CHAIN FAIL: isi S_FMT: %s\n", strerror(errno));
        return -1;
    }
    c->bpl = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    c->sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    printf("CHAIN isi fmt %ux%u fourcc=%.4s bpl=%u size=%u\n",
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           (char *)&fmt.fmt.pix_mp.pixelformat, c->bpl, c->sizeimage);

    if (!g_no_cap_sfmt && fmt.fmt.pix_mp.pixelformat != g_fourcc) {
        printf("CHAIN FAIL: the ISI would not take a Bayer pixelformat "
               "(gave %.4s)\n", (char *)&fmt.fmt.pix_mp.pixelformat);
        return -1;
    }
    /*
     * The QEMU ISI model derives bytes-per-pixel as pitch/width, so a padded
     * stride would silently make it copy the wrong number of bytes per row.
     * Say so here rather than let the correlation collapse and look like a
     * debayer bug.
     */
    if (g_strict_stride && c->bpl != w) {
        printf("CHAIN FAIL: bytesperline %u != width %u; the ISI model's "
               "bpp=pitch/width would misread the row stride\n", c->bpl, w);
        return -1;
    }

    memset(&req, 0, sizeof req);
    req.count = ISI_NBUF;
    req.type = CAP_TYPE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("CHAIN FAIL: isi REQBUFS: %s\n", strerror(errno));
        return -1;
    }
    c->nbuf = req.count;

    for (i = 0; i < c->nbuf; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane pl[MAXP];
        struct v4l2_exportbuffer eb;

        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = CAP_TYPE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.m.planes = pl;
        b.length = MAXP;
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("CHAIN FAIL: isi QUERYBUF %u: %s\n", i, strerror(errno));
            return -1;
        }
        c->len[i] = pl[0].length;
        c->mem[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, c->fd, pl[0].m.mem_offset);
        if (c->mem[i] == MAP_FAILED) {
            printf("CHAIN FAIL: isi mmap %u: %s\n", i, strerror(errno));
            return -1;
        }

        /* export each capture buffer once; the fd stays valid for the queue */
        if (g_no_expbuf) {
            c->dmafd[i] = -1;
            goto queue_it;
        }
        memset(&eb, 0, sizeof eb);
        eb.type = CAP_TYPE;
        eb.index = i;
        eb.plane = 0;
        if (xioctl(c->fd, VIDIOC_EXPBUF, &eb) < 0) {
            printf("CHAIN FAIL: isi EXPBUF %u: %s - without dmabuf export the "
                   "ISP cannot read the ISI's own buffer\n", i,
                   strerror(errno));
            return -1;
        }
        c->dmafd[i] = eb.fd;

queue_it:
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = CAP_TYPE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.m.planes = pl;
        b.length = 1;
        if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0) {
            printf("CHAIN FAIL: isi QBUF %u: %s\n", i, strerror(errno));
            return -1;
        }
    }
    printf("CHAIN isi exported %u dmabuf capture buffers\n", c->nbuf);

    if (xioctl(c->fd, VIDIOC_STREAMON, &t) < 0) {
        printf("CHAIN FAIL: isi STREAMON: %s (-EPIPE here means a link "
               "format mismatch; run v4l2_cap links for the offender)\n",
               strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------- the NeoISP mem2mem side ---------------- */

struct node {
    int fd;
    int type;
    int memory;
    void *buf[MAXP];
    size_t len[MAXP];
    unsigned nplanes;
};

/*
 * The driver registers EIGHT groups of six identically-named nodes
 * (input0, input1, params, frame, ir, stats). A job must be assembled from ONE
 * group: picking per-name across groups leaves no group with a complete set,
 * prepare_job never becomes ready, and DQBUF blocks forever with no error
 * anywhere. Find the base of a group whose siblings sit where the layout says.
 */
static int find_group_base(void)
{
    char nm[64], p[64], f[64];
    int n;

    for (n = 0; n < 256; n++) {
        node_name_n(n, nm, sizeof nm);
        if (strcmp(nm, "neoisp-input0")) {
            continue;
        }
        node_name_n(n + 2, p, sizeof p);
        node_name_n(n + 3, f, sizeof f);
        if (!strcmp(p, "neoisp-params") && !strcmp(f, "neoisp-frame")) {
            return n;
        }
    }
    return -1;
}

static int isp_node(struct node *n, int vidnum, const char *want, int type,
                    int memory, unsigned w, unsigned h, unsigned fourcc)
{
    struct v4l2_requestbuffers req;
    struct v4l2_format fmt;
    char nm[64], path[64];
    unsigned i;

    node_name_n(vidnum, nm, sizeof nm);
    if (strcmp(nm, want)) {
        printf("CHAIN FAIL: video%d is '%s', expected '%s'\n",
               vidnum, nm, want);
        return -1;
    }
    snprintf(path, sizeof path, "/dev/video%d", vidnum);
    n->fd = open(path, O_RDWR);
    if (n->fd < 0) {
        printf("CHAIN FAIL: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    n->type = type;
    n->memory = memory;

    memset(&fmt, 0, sizeof fmt);
    fmt.type = type;
    if (xioctl(n->fd, VIDIOC_G_FMT, &fmt) < 0) {
        printf("CHAIN FAIL: %s G_FMT: %s\n", want, strerror(errno));
        return -1;
    }
    if (w) {
        fmt.fmt.pix_mp.width = w;
        fmt.fmt.pix_mp.height = h;
        fmt.fmt.pix_mp.pixelformat = fourcc;
        if (xioctl(n->fd, VIDIOC_S_FMT, &fmt) < 0) {
            printf("CHAIN FAIL: %s S_FMT: %s\n", want, strerror(errno));
            return -1;
        }
        n->nplanes = fmt.fmt.pix_mp.num_planes ?
                     fmt.fmt.pix_mp.num_planes : 1;
        printf("CHAIN %-14s %s %ux%u fourcc=%.4s bpl=%u size=%u\n", want, path,
               fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
               (char *)&fmt.fmt.pix_mp.pixelformat,
               fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
               fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    } else {
        n->nplanes = 1;
        printf("CHAIN %-14s %s meta size=%u\n", want, path,
               fmt.fmt.meta.buffersize);
    }

    memset(&req, 0, sizeof req);
    req.count = 1;
    req.type = type;
    req.memory = memory;
    if (xioctl(n->fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("CHAIN FAIL: %s REQBUFS(%s): %s\n", want,
               memory == V4L2_MEMORY_DMABUF ? "DMABUF" : "MMAP",
               strerror(errno));
        return -1;
    }

    if (memory == V4L2_MEMORY_DMABUF) {
        return 0;               /* no mapping: the ISI owns the memory */
    }

    {
        struct v4l2_buffer b;
        struct v4l2_plane pl[MAXP];
        int meta = (type == V4L2_BUF_TYPE_META_OUTPUT ||
                    type == V4L2_BUF_TYPE_META_CAPTURE);

        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = 0;
        if (!meta) {
            b.m.planes = pl;
            b.length = MAXP;
        }
        if (xioctl(n->fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("CHAIN FAIL: %s QUERYBUF: %s\n", want, strerror(errno));
            return -1;
        }
        if (meta) {
            n->len[0] = b.length;
            n->buf[0] = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, n->fd, b.m.offset);
        } else {
            for (i = 0; i < n->nplanes; i++) {
                n->len[i] = pl[i].length;
                n->buf[i] = mmap(NULL, pl[i].length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, n->fd, pl[i].m.mem_offset);
            }
        }
        if (n->buf[0] == MAP_FAILED) {
            printf("CHAIN FAIL: %s mmap: %s\n", want, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int isp_qbuf_mmap(struct node *n)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[MAXP];
    unsigned i;
    int meta = (n->type == V4L2_BUF_TYPE_META_OUTPUT ||
                n->type == V4L2_BUF_TYPE_META_CAPTURE);

    memset(&b, 0, sizeof b);
    memset(pl, 0, sizeof pl);
    b.type = n->type;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = 0;
    if (meta) {
        b.bytesused = n->len[0];
        b.length = n->len[0];
        return xioctl(n->fd, VIDIOC_QBUF, &b);
    }
    b.m.planes = pl;
    b.length = n->nplanes;
    for (i = 0; i < n->nplanes; i++) {
        pl[i].bytesused = n->len[i];
        pl[i].length = n->len[i];
    }
    return xioctl(n->fd, VIDIOC_QBUF, &b);
}

/* Queue the ISI's exported buffer as the ISP's input. This is the join. */
static int isp_qbuf_dmabuf(struct node *n, int dmafd, size_t used)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[MAXP];

    memset(&b, 0, sizeof b);
    memset(pl, 0, sizeof pl);
    b.type = n->type;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = 0;
    b.m.planes = pl;
    b.length = 1;
    pl[0].m.fd = dmafd;
    pl[0].bytesused = used;
    pl[0].length = used;
    return xioctl(n->fd, VIDIOC_QBUF, &b);
}

static int isp_dqbuf(struct node *n)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[MAXP];
    int meta = (n->type == V4L2_BUF_TYPE_META_OUTPUT ||
                n->type == V4L2_BUF_TYPE_META_CAPTURE);

    memset(&b, 0, sizeof b);
    memset(pl, 0, sizeof pl);
    b.type = n->type;
    b.memory = n->memory;
    if (!meta) {
        b.m.planes = pl;
        b.length = n->nplanes;
    }
    return xioctl(n->fd, VIDIOC_DQBUF, &b);
}

static int stream_on(struct node *n)
{
    int t = n->type;

    return xioctl(n->fd, VIDIOC_STREAMON, &t);
}

int main(int argc, char **argv)
{
    unsigned nframes = argc > 1 ? (unsigned)atoi(argv[1]) : 2;
    unsigned W = argc > 2 ? (unsigned)atoi(argv[2]) : 640;
    unsigned H = argc > 3 ? (unsigned)atoi(argv[3]) : 480;
    const char *prefix = argc > 4 ? argv[4] : "/developed";
    struct node in0 = {0}, frame = {0}, params = {0}, stats = {0};
    struct isi cap = {0};
    unsigned i, ok = 0;
    int base;

    int pipe_id;

    read_env();
    printf("CHAIN start nframes=%u %ux%u\n", nframes, W, H);

    pipe_id = isi_find_pipe();
    if (pipe_id < 0) {
        printf("CHAIN FAIL: no mxc_isi capture node\n");
        return 1;
    }
    printf("CHAIN using ISI pipe %d\n", pipe_id);

    if (setup_pipeline("/dev/media0", W, H, pipe_id) < 0) {
        return 1;
    }
    if (isi_start(&cap, W, H) < 0) {
        return 1;
    }

    if (g_capture_only) {
        struct v4l2_buffer b;
        struct v4l2_plane pl[MAXP];
        unsigned n;

        for (n = 0; n < nframes; n++) {
            memset(&b, 0, sizeof b);
            memset(pl, 0, sizeof pl);
            b.type = CAP_TYPE;
            b.memory = V4L2_MEMORY_MMAP;
            b.m.planes = pl;
            b.length = MAXP;
            if (xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0) {
                printf("CHAIN FAIL: control DQBUF %u: %s\n", n,
                       strerror(errno));
                return 1;
            }
            printf("CHAIN control frame=%u used=%u hash=%016llx\n", n,
                   pl[0].bytesused,
                   (unsigned long long)fnv1a(cap.mem[b.index],
                                             cap.len[b.index]));
            b.length = 1;
            xioctl(cap.fd, VIDIOC_QBUF, &b);
        }
        printf("CHAIN CONTROL PASS: capture streams on this format\n");
        return 0;
    }

    base = find_group_base();
    if (base < 0) {
        printf("CHAIN FAIL: no complete neoisp node group\n");
        return 1;
    }
    printf("CHAIN neoisp group base=/dev/video%d\n", base);

    /* input0 takes the ISI's memory; everything else is ours */
    if (isp_node(&in0, base, "neoisp-input0", OUT_TYPE, V4L2_MEMORY_DMABUF,
                 W, H, g_fourcc) < 0) {
        return 1;
    }
    if (isp_node(&frame, base + 3, "neoisp-frame", CAP_TYPE, V4L2_MEMORY_MMAP,
                 W, H, V4L2_PIX_FMT_RGB32) < 0) {
        return 1;
    }
    /*
     * params AND stats must both be fed. The driver marks input0, params,
     * frame and stats MEDIA_LNK_FL_ENABLED by default and prepare_job refuses
     * to schedule while any enabled node lacks a queued buffer - with no error
     * raised, so a missing one reads as a dead device rather than an unmet
     * precondition.
     */
    if (isp_node(&params, base + 2, "neoisp-params", V4L2_BUF_TYPE_META_OUTPUT,
                 V4L2_MEMORY_MMAP, 0, 0, 0) < 0) {
        return 1;
    }
    if (isp_node(&stats, base + 5, "neoisp-stats", V4L2_BUF_TYPE_META_CAPTURE,
                 V4L2_MEMORY_MMAP, 0, 0, 0) < 0) {
        return 1;
    }

    /*
     * STREAMON before any QBUF. vb2 hands pre-queued buffers to the driver
     * only after start_streaming returns, but this driver schedules a job from
     * inside start_streaming - so a buffer queued first is invisible, the job
     * runs with a NULL output buffer, and the hardware is programmed with
     * output address 0: one trigger, no frame.
     */
    if (stream_on(&params) < 0 || stream_on(&stats) < 0 ||
        stream_on(&in0) < 0 || stream_on(&frame) < 0) {
        printf("CHAIN FAIL: neoisp STREAMON: %s\n", strerror(errno));
        return 1;
    }

    /* heap staging for the 9p write; see the copy below for why it exists */
    unsigned char *stage = NULL;
    size_t stage_len = 0;

    for (i = 0; i < nframes; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane pl[MAXP];
        unsigned idx;
        FILE *f;
        char out[128];

        /* --- 1. the ISI has DMAed a Bayer frame into DRAM --- */
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = CAP_TYPE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = MAXP;
        if (xioctl(cap.fd, VIDIOC_DQBUF, &b) < 0) {
            printf("CHAIN FAIL: isi DQBUF %u: %s\n", i, strerror(errno));
            return 1;
        }
        idx = b.index;
        printf("CHAIN frame=%u cap-index=%u cap-bytes=%u cap-hash=%016llx\n",
               i, idx, pl[0].bytesused,
               (unsigned long long)fnv1a(cap.mem[idx], (size_t)W * H));

        /* --- 2. the ISP reads THAT buffer, by dmabuf --- */
        if (isp_qbuf_mmap(&params) < 0) {
            printf("CHAIN FAIL: params QBUF: %s\n", strerror(errno));
            return 1;
        }
        if (isp_qbuf_mmap(&stats) < 0) {
            printf("CHAIN FAIL: stats QBUF: %s\n", strerror(errno));
            return 1;
        }
        if (isp_qbuf_mmap(&frame) < 0) {
            printf("CHAIN FAIL: frame QBUF: %s\n", strerror(errno));
            return 1;
        }
        if (isp_qbuf_dmabuf(&in0, cap.dmafd[idx], (size_t)W * H) < 0) {
            printf("CHAIN FAIL: input0 QBUF(dmabuf fd=%d): %s\n",
                   cap.dmafd[idx], strerror(errno));
            return 1;
        }

        if (isp_dqbuf(&frame) < 0) {
            printf("CHAIN FAIL: frame DQBUF: %s\n", strerror(errno));
            return 1;
        }
        printf("CHAIN frame=%u dev-hash=%016llx\n", i,
               (unsigned long long)fnv1a(frame.buf[0], frame.len[0]));

        /* recycle the ISP's own buffers, or iteration 2 has none to queue */
        isp_dqbuf(&in0);
        isp_dqbuf(&params);
        isp_dqbuf(&stats);

        /*
         * Copy to the heap before writing to the 9p share.
         *
         * frame.buf[0] is a V4L2 mmap of dma-contig memory, so its VMA is
         * VM_IO|VM_PFNMAP and has no struct pages to pin. 9p over netfs pins
         * the source pages for a direct write, gets -EFAULT, and RETRIES
         * forever - "netfs: Couldn't get user pages (rc=-14)" at thousands of
         * lines a second (a 29 MB serial log in one run) while the write never
         * completes and never returns an error either. A heap staging buffer
         * is ordinary anonymous memory and writes normally.
         *
         * This does not weaken the claim: what is being proved is that the
         * ISP read the buffer the ISI wrote, and that already happened above,
         * in the kernel, through the exported dmabuf. The developed frame now
         * only has to get off the machine intact, and it is hashed from the
         * ISP's own buffer before the copy.
         */
        if (!stage) {
            stage = malloc(frame.len[0]);
            if (!stage) {
                printf("CHAIN FAIL: no memory to stage %zu bytes\n",
                       frame.len[0]);
                return 1;
            }
            stage_len = frame.len[0];
        }
        if (frame.len[0] > stage_len) {
            printf("CHAIN FAIL: developed frame grew (%zu > %zu)\n",
                   frame.len[0], stage_len);
            return 1;
        }
        memcpy(stage, frame.buf[0], frame.len[0]);

        snprintf(out, sizeof out, "%s-%u.raw", prefix, i);
        f = fopen(out, "wb");
        if (!f) {
            printf("CHAIN FAIL: open %s: %s\n", out, strerror(errno));
            return 1;
        }
        {
            size_t wrote = fwrite(stage, 1, frame.len[0], f);

            if (wrote != frame.len[0] || fflush(f) != 0) {
                printf("CHAIN FAIL: wrote %zu of %zu bytes to %s: %s\n",
                       wrote, frame.len[0], out, strerror(errno));
                fclose(f);
                return 1;
            }
            fclose(f);
            printf("CHAIN frame=%u wrote %s (%zu bytes)\n", i, out,
                   frame.len[0]);
        }

        /* give the capture buffer back to the ISI */
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = CAP_TYPE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = idx;
        b.m.planes = pl;
        b.length = 1;
        if (xioctl(cap.fd, VIDIOC_QBUF, &b) < 0) {
            printf("CHAIN warn: isi re-QBUF %u: %s\n", idx, strerror(errno));
        }
        ok++;
    }

    printf("CHAIN developed %u/%u frames\n", ok, nframes);
    if (ok == nframes) {
        printf("CHAIN PASS\n");
        return 0;
    }
    return 1;
}
