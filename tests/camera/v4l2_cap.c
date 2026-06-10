/*
 * V4L2 streams-aware capture + link_validate oracle for the i.MX95 ISI model.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX95 ISI front end is an 8-channel crossbar that uses the V4L2 streams
 * API (each crossbar source pad is fed by a routed sink stream). When STREAMON
 * fails with -EPIPE the cause is the media-core link_validate, which logs only
 * at dev_dbg - invisible unless CONFIG_DYNAMIC_DEBUG is on (it is not in the
 * BSP). This tool replicates link_validate from userspace: it walks every
 * ENABLED link, reads the source-pad and sink-pad mbus formats, and prints any
 * width/height/code mismatch - so the failing link is named rather than hidden.
 *
 * Modes:
 *   topo  [mdev]         - dump entities, pads, enabled links (like media-ctl)
 *   links [mdev]         - the link_validate oracle (format diff per link)
 *   cap   [video] [mdev] - set up the pipeline + MMAP capture a few frames
 *
 * v4l2-ctl / media-ctl are not in the BSP, hence this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>

#define NBUF   4
#define FRAMES 5
#define TYPE   V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define DEF_CODE 0x200f         /* MEDIA_BUS_FMT_UYVY8_1X16 (ov5640 MIPI) */

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* Map a v4l-subdev minor (major 81) to its /dev path. */
static int subdev_path_for_minor(unsigned minor, char *out, size_t n)
{
    int i;
    for (i = 0; i < 16; i++) {
        struct stat st;
        char p[64];
        snprintf(p, sizeof(p), "/dev/v4l-subdev%d", i);
        if (stat(p, &st) == 0 && (st.st_rdev & 0xff) == (minor & 0xff)) {
            snprintf(out, n, "%s", p);
            return 0;
        }
    }
    return -1;
}

/* Read one subdev pad/stream active mbus format. Returns 0 on success. */
static int read_pad_fmt(unsigned minor, unsigned pad, unsigned stream,
                        struct v4l2_mbus_framefmt *out)
{
    char path[64];
    struct v4l2_subdev_format f;
    int sd, rc;

    if (subdev_path_for_minor(minor, path, sizeof(path)) != 0) {
        return -1;
    }
    sd = open(path, O_RDWR);
    if (sd < 0) {
        return -1;
    }
    memset(&f, 0, sizeof(f));
    f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    f.pad = pad;
    /*
     * The streams-API 'stream' field overlaps reserved[0] in the older UAPI
     * header (same struct size, same ioctl number), so address it that way to
     * build against stock host headers. Only stream 0 is used here.
     */
    f.reserved[0] = stream;
    rc = xioctl(sd, VIDIOC_SUBDEV_G_FMT, &f);
    if (rc == 0) {
        *out = f.format;
    }
    close(sd);
    return rc;
}

/* Dump the media graph: entities, pads, and enabled links - like media-ctl. */
static int media_topo(const char *mdev)
{
    int fd = open(mdev, O_RDWR);
    struct media_device_info info;
    int id;

    if (fd < 0) {
        printf("TOPO: open %s: %s\n", mdev, strerror(errno));
        return 1;
    }
    memset(&info, 0, sizeof(info));
    if (xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0) {
        printf("TOPO: media driver=%s model=%s\n", info.driver, info.model);
    }

    id = 0;
    for (;;) {
        struct media_entity_desc e;
        struct media_links_enum le;
        struct media_pad_desc pads[16];
        struct media_link_desc links[32];
        unsigned p;

        memset(&e, 0, sizeof(e));
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) < 0) {
            break;
        }
        id = e.id;
        printf("TOPO: ent %u '%s' func=0x%x pads=%u links=%u minor=%u\n",
               e.id, e.name, e.type, e.pads, e.links, e.dev.minor);

        memset(&le, 0, sizeof(le));
        memset(pads, 0, sizeof(pads));
        memset(links, 0, sizeof(links));
        le.entity = e.id;
        le.pads = pads;
        le.links = links;
        if (xioctl(fd, MEDIA_IOC_ENUM_LINKS, &le) == 0) {
            for (p = 0; p < e.links && p < 32; p++) {
                struct media_link_desc *l = &links[p];
                printf("TOPO:   link %u/%u -> %u/%u flags=0x%x%s\n",
                       l->source.entity, l->source.index,
                       l->sink.entity, l->sink.index, l->flags,
                       (l->flags & MEDIA_LNK_FL_ENABLED) ? " ENABLED" : "");
            }
        }
    }
    close(fd);
    return 0;
}

/* Look up an entity's minor by id. Returns minor or 0. */
static unsigned minor_of(int fd, unsigned id)
{
    struct media_entity_desc e;
    memset(&e, 0, sizeof(e));
    e.id = id;
    if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) == 0) {
        return e.dev.minor;
    }
    return 0;
}

/*
 * The link_validate oracle: for every ENABLED subdev<->subdev link, read the
 * source-pad format and the sink-pad format and report whether they agree.
 * This is exactly what v4l2_subdev_link_validate checks (width/height/code) -
 * but at a visible level, so an -EPIPE STREAMON can be attributed to a link.
 */
static int link_oracle(const char *mdev)
{
    int fd = open(mdev, O_RDWR);
    int id, bad = 0;

    if (fd < 0) {
        printf("LINKS: open %s: %s\n", mdev, strerror(errno));
        return 1;
    }
    id = 0;
    for (;;) {
        struct media_entity_desc e;
        struct media_links_enum le;
        struct media_pad_desc pads[16];
        struct media_link_desc links[32];
        unsigned k;

        memset(&e, 0, sizeof(e));
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) < 0) {
            break;
        }
        id = e.id;

        memset(&le, 0, sizeof(le));
        memset(pads, 0, sizeof(pads));
        memset(links, 0, sizeof(links));
        le.entity = e.id;
        le.pads = pads;
        le.links = links;
        if (xioctl(fd, MEDIA_IOC_ENUM_LINKS, &le) < 0) {
            continue;
        }
        for (k = 0; k < e.links && k < 32; k++) {
            struct media_link_desc *l = &links[k];
            struct v4l2_mbus_framefmt sf, kf;
            unsigned smin, kmin;
            int sok, kok;

            if (!(l->flags & MEDIA_LNK_FL_ENABLED)) {
                continue;
            }
            /* Only enumerate each link once (from its source entity). */
            if (l->source.entity != e.id) {
                continue;
            }
            smin = minor_of(fd, l->source.entity);
            kmin = minor_of(fd, l->sink.entity);
            sok = read_pad_fmt(smin, l->source.index, 0, &sf);
            kok = read_pad_fmt(kmin, l->sink.index, 0, &kf);
            if (sok != 0 || kok != 0) {
                /* One side is a video node (no subdev fmt) - skip the diff. */
                printf("LINKS: %u/%u -> %u/%u  (video-node link, skipped)\n",
                       l->source.entity, l->source.index,
                       l->sink.entity, l->sink.index);
                continue;
            }
            int match = sf.width == kf.width && sf.height == kf.height &&
                        sf.code == kf.code;
            printf("LINKS: %u/%u -> %u/%u  src=%ux%u/0x%04x "
                   "sink=%ux%u/0x%04x  %s\n",
                   l->source.entity, l->source.index,
                   l->sink.entity, l->sink.index,
                   sf.width, sf.height, sf.code,
                   kf.width, kf.height, kf.code,
                   match ? "OK" : "*** MISMATCH ***");
            bad += !match;
        }
    }
    close(fd);
    printf("LINKS: %d mismatched link(s)\n", bad);
    return bad ? 1 : 0;
}

/*
 * Bring the sensor->ISI capture pipeline up the way media-ctl would:
 *  - enable the (default-disabled) camera-sensor source link
 *  - read the sensor's source-pad mbus format
 *  - push that same format onto every subdev sink pad along the chain so the
 *    media-core link_validate succeeds at STREAMON.
 */
static int setup_pipeline(const char *mdev, uint32_t *w, uint32_t *h,
                          uint32_t *code)
{
    int fd = open(mdev, O_RDWR);
    int id, sensor_id = -1;
    struct v4l2_subdev_format sfmt;
    int rc;

    if (fd < 0) {
        printf("SETUP: open %s: %s\n", mdev, strerror(errno));
        return -1;
    }

    /* Pass 1: enable every disabled, non-immutable link out of a CAM sensor. */
    id = 0;
    for (;;) {
        struct media_entity_desc e;
        struct media_links_enum le;
        struct media_pad_desc pads[16];
        struct media_link_desc links[32];
        unsigned k;

        memset(&e, 0, sizeof(e));
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) < 0) {
            break;
        }
        id = e.id;
        if (e.type == MEDIA_ENT_F_CAM_SENSOR) {
            sensor_id = e.id;
        }

        memset(&le, 0, sizeof(le));
        memset(pads, 0, sizeof(pads));
        memset(links, 0, sizeof(links));
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
            if (l->source.entity == e.id) {
                l->flags |= MEDIA_LNK_FL_ENABLED;
                if (xioctl(fd, MEDIA_IOC_SETUP_LINK, l) < 0) {
                    printf("SETUP: enable link %u/%u->%u/%u: %s\n",
                           l->source.entity, l->source.index,
                           l->sink.entity, l->sink.index, strerror(errno));
                } else {
                    printf("SETUP: enabled link %u/%u -> %u/%u\n",
                           l->source.entity, l->source.index,
                           l->sink.entity, l->sink.index);
                }
            }
        }
    }

    if (sensor_id < 0) {
        printf("SETUP: no camera sensor entity found\n");
        close(fd);
        return -1;
    }

    /* Read the sensor source-pad format from its subdev node. */
    *w = 640; *h = 480; *code = 0;
    {
        struct media_entity_desc e;
        char path[64];
        int sd = -1;
        memset(&e, 0, sizeof(e));
        e.id = sensor_id;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) == 0 &&
            subdev_path_for_minor(e.dev.minor, path, sizeof(path)) == 0) {
            sd = open(path, O_RDWR);
        }
        if (sd >= 0) {
            memset(&sfmt, 0, sizeof(sfmt));
            sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            sfmt.pad = 0;
            if (xioctl(sd, VIDIOC_SUBDEV_G_FMT, &sfmt) == 0) {
                *w = sfmt.format.width;
                *h = sfmt.format.height;
                *code = sfmt.format.code;
                printf("SETUP: sensor %s fmt %ux%u code=0x%04x\n", path,
                       *w, *h, *code);
            }
            close(sd);
        }
    }

    /*
     * Pass 2: push that format onto every subdev sink pad in the graph. The
     * crossbar/pipe propagate sink->source along the active route, so only the
     * connected crossbar sink accepts it (the others return EINVAL).
     */
    id = 0;
    for (;;) {
        struct media_entity_desc e;
        struct media_pad_desc pads[16];
        struct media_links_enum le;
        struct media_link_desc links[32];
        char path[64];
        int sd;
        unsigned k;

        memset(&e, 0, sizeof(e));
        e.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) < 0) {
            break;
        }
        id = e.id;
        if (subdev_path_for_minor(e.dev.minor, path, sizeof(path)) != 0) {
            continue;
        }
        memset(&le, 0, sizeof(le));
        memset(pads, 0, sizeof(pads));
        memset(links, 0, sizeof(links));
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
            if (!(pads[k].flags & MEDIA_PAD_FL_SINK)) {
                continue;       /* sink pads drive propagation */
            }
            memset(&pf, 0, sizeof(pf));
            pf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            pf.pad = pads[k].index;
            pf.format.width = *w;
            pf.format.height = *h;
            pf.format.code = *code ? *code : DEF_CODE;
            pf.format.field = V4L2_FIELD_NONE;
            rc = xioctl(sd, VIDIOC_SUBDEV_S_FMT, &pf);
            printf("SETUP: ent %u '%s' pad %u S_FMT %ux%u/0x%04x -> %s\n",
                   e.id, e.name, pads[k].index, *w, *h,
                   *code ? *code : DEF_CODE,
                   rc == 0 ? "ok" : strerror(errno));
        }
        close(sd);
    }

    close(fd);
    return 0;
}

static int do_capture(const char *dev, uint32_t cap_w, uint32_t cap_h)
{
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    void *mem[NBUF];
    uint32_t len[NBUF];
    uint32_t nplanes;
    int fd, i, got = 0;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        printf("CAMERA-CAP[%s]: FAIL (open: %s)\n", dev, strerror(errno));
        return 1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        printf("CAMERA-CAP[%s]: cap driver=%s card=%s caps=0x%08x\n", dev,
               cap.driver, cap.card, cap.device_caps);
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = TYPE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        printf("CAMERA-CAP[%s]: FAIL (G_FMT: %s)\n", dev, strerror(errno));
        return 1;
    }
    if (cap_w && cap_h) {
        fmt.fmt.pix_mp.width = cap_w;
        fmt.fmt.pix_mp.height = cap_h;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            printf("CAMERA-CAP[%s]: S_FMT %ux%u: %s\n", dev, cap_w, cap_h,
                   strerror(errno));
        }
    }
    nplanes = fmt.fmt.pix_mp.num_planes;
    printf("CAMERA-CAP[%s]: fmt %ux%u fourcc=%c%c%c%c planes=%u "
           "bpl=%u size=%u\n", dev,
           fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
           fmt.fmt.pix_mp.pixelformat & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
           (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff, nplanes,
           fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
           fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    memset(&req, 0, sizeof(req));
    req.count = NBUF;
    req.type = TYPE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("CAMERA-CAP[%s]: FAIL (REQBUFS: %s)\n", dev, strerror(errno));
        return 1;
    }

    for (i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&b, 0, sizeof(b));
        memset(planes, 0, sizeof(planes));
        b.type = TYPE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = nplanes;
        b.m.planes = planes;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("CAMERA-CAP[%s]: FAIL (QUERYBUF: %s)\n", dev,
                   strerror(errno));
            return 1;
        }
        len[i] = planes[0].length;
        mem[i] = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, planes[0].m.mem_offset);
        if (mem[i] == MAP_FAILED) {
            printf("CAMERA-CAP[%s]: FAIL (mmap: %s)\n", dev, strerror(errno));
            return 1;
        }
        memset(mem[i], 0, planes[0].length);
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            printf("CAMERA-CAP[%s]: FAIL (QBUF: %s)\n", dev, strerror(errno));
            return 1;
        }
    }

    enum v4l2_buf_type t = TYPE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
        printf("CAMERA-CAP[%s]: FAIL (STREAMON: %s)\n", dev, strerror(errno));
        return 1;
    }

    for (i = 0; i < FRAMES; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        fd_set fds;
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            printf("CAMERA-CAP[%s]: frame %d timeout\n", dev, i);
            break;
        }
        memset(&b, 0, sizeof(b));
        memset(planes, 0, sizeof(planes));
        b.type = TYPE;
        b.memory = V4L2_MEMORY_MMAP;
        b.length = nplanes;
        b.m.planes = planes;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            printf("CAMERA-CAP[%s]: frame %d DQBUF: %s\n", dev, i,
                   strerror(errno));
            break;
        }
        const uint8_t *p = mem[b.index];
        uint32_t used = planes[0].bytesused, nz = 0, j;
        for (j = 0; j < used && j < 65536; j++) {
            nz += p[j] != 0;
        }
        printf("CAMERA-CAP[%s]: frame %d buf=%u used=%u nz(64k)=%u "
               "p[0..3]=%02x%02x%02x%02x\n", dev, i, b.index, used, nz,
               p[0], p[1], p[2], p[3]);
        got += (used > 0 && nz > 0);
        xioctl(fd, VIDIOC_QBUF, &b);
    }

    xioctl(fd, VIDIOC_STREAMOFF, &t);
    for (i = 0; i < (int)req.count; i++) {
        munmap(mem[i], len[i]);
    }
    close(fd);

    printf("CAMERA-CAP[%s]: %s (%d/%d frames)\n", dev,
           got >= 3 ? "PASS" : "FAIL", got, FRAMES);
    return got >= 3 ? 0 : 1;
}

int main(int argc, char **argv)
{
    uint32_t cap_w = 0, cap_h = 0, cap_code = 0;

    if (argc > 1 && strcmp(argv[1], "topo") == 0) {
        return media_topo(argc > 2 ? argv[2] : "/dev/media0");
    }
    if (argc > 1 && strcmp(argv[1], "links") == 0) {
        setup_pipeline(argc > 2 ? argv[2] : "/dev/media0",
                       &cap_w, &cap_h, &cap_code);
        return link_oracle(argc > 2 ? argv[2] : "/dev/media0");
    }
    if (argc > 1 && strcmp(argv[1], "cap") == 0) {
        const char *dev;
        setup_pipeline("/dev/media0", &cap_w, &cap_h, &cap_code);
        dev = argc > 2 ? argv[2] : "/dev/video0";
        return do_capture(dev, cap_w, cap_h);
    }
    printf("usage: %s topo|links|cap [args]\n", argv[0]);
    return 2;
}
