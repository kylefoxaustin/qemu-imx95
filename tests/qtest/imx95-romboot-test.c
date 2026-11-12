/*
 * QTest for the i.MX 95 emulated boot ROM (hw/arm/fsl-imx95.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * With no -kernel, the machine stands in for the boot ROM: it walks the AHAB
 * container set at 32 KiB on uSDHC1 and loads each executable image to the
 * address its entry names. Two cases, because the second is what keeps every
 * pre-existing boot flow working:
 *
 *   loads      an EXEC image for the A55 lands in DRAM, byte for byte, and
 *              the walk steps over the header-only container in front of it
 *   ignores    the same image with hab_flags = 0 (image type 0, not EXEC) is
 *              left alone. That is exactly the shape of the media our older
 *              tests build, where SPL and the SM arrive via -device loader,
 *              so the ROM read must be a no-op on it.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* CONTAINER_HDR_MMCSD_OFFSET: where the ROM looks on SD/eMMC. */
#define SET_OFF         0x8000
/* ALIGN(16, container_hdr_alignment()) past it, as the ROM and SPL step. */
#define CNT_OFF         0x8400
#define HDR_SIZE        0x10
#define IMG_ENTRY_SIZE  0x80
#define PAYLOAD_OFF     0xc00           /* within CNT_OFF */

#define IMG_TYPE_EXEC   0x3
#define CORE_A55        0x2
#define FLAGS_A55_EXEC  (IMG_TYPE_EXEC | (CORE_A55 << 4))

/* Somewhere in DRAM that nothing else touches on a bare machine. */
#define LOAD_ADDR       0x92000000ULL
#define PAYLOAD_LEN     256
#define IMAGE_LEN       (2 * 1024 * 1024)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;  p[1] = v >> 8;  p[2] = v >> 16;  p[3] = v >> 24;
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, v);
    put32(p + 4, v >> 32);
}

/* A container set with one image, whose hab_flags the caller chooses. */
static char *make_image(uint32_t hab_flags, const uint8_t *payload)
{
    g_autofree uint8_t *img = g_malloc0(IMAGE_LEN);
    uint8_t *hdr, *ent;
    g_autoptr(GError) err = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("imx95-romboot-XXXXXX.img", &path, &err);

    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);

    /* Header-only container, so the walk has to step to the next one. */
    hdr = img + SET_OFF;
    hdr[0x00] = 0;                      /* version    */
    hdr[0x01] = HDR_SIZE;               /* length_lsb */
    hdr[0x03] = 0x87;                   /* tag        */
    hdr[0x0b] = 0;                      /* num_images */

    hdr = img + CNT_OFF;
    hdr[0x00] = 0;
    hdr[0x01] = HDR_SIZE + IMG_ENTRY_SIZE;
    hdr[0x03] = 0x87;
    hdr[0x0b] = 1;

    ent = hdr + HDR_SIZE;
    put32(ent + 0x00, PAYLOAD_OFF);     /* offset, from this header */
    put32(ent + 0x04, PAYLOAD_LEN);     /* size   */
    put64(ent + 0x08, LOAD_ADDR);       /* dst    */
    put64(ent + 0x10, LOAD_ADDR);       /* entry  */
    put32(ent + 0x18, hab_flags);

    memcpy(img + CNT_OFF + PAYLOAD_OFF, payload, PAYLOAD_LEN);

    g_assert_cmpint(write(fd, img, IMAGE_LEN), ==, IMAGE_LEN);
    close(fd);
    return path;
}

static QTestState *boot_with(const char *path)
{
    return qtest_initf("-machine imx95-19x19-evk -accel qtest -m 2G "
                       "-drive if=none,format=raw,file=%s,id=mmc0 "
                       "-device emmc,drive=mmc0", path);
}

static void test_rom_loads_exec_image(void)
{
    uint8_t payload[PAYLOAD_LEN], got[PAYLOAD_LEN];
    g_autofree char *path = NULL;
    QTestState *qts;
    int i;

    for (i = 0; i < PAYLOAD_LEN; i++) {
        payload[i] = 0x40 + (i & 0x3f);
    }
    path = make_image(FLAGS_A55_EXEC, payload);

    qts = boot_with(path);
    qtest_memread(qts, LOAD_ADDR, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), payload, sizeof(payload));
    qtest_quit(qts);

    unlink(path);
}

static void test_rom_ignores_non_exec_image(void)
{
    uint8_t payload[PAYLOAD_LEN], got[PAYLOAD_LEN];
    g_autofree char *path = NULL;
    QTestState *qts;
    int i;

    for (i = 0; i < PAYLOAD_LEN; i++) {
        payload[i] = 0x40 + (i & 0x3f);
    }
    path = make_image(0, payload);

    qts = boot_with(path);
    qtest_memread(qts, LOAD_ADDR, got, sizeof(got));
    for (i = 0; i < PAYLOAD_LEN; i++) {
        g_assert_cmphex(got[i], ==, 0);
    }
    qtest_quit(qts);

    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/romboot/loads-exec-image",
                   test_rom_loads_exec_image);
    qtest_add_func("imx95/romboot/ignores-non-exec-image",
                   test_rom_ignores_non_exec_image);
    return g_test_run();
}
