/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Exercise the I3C master's legacy-I2C datapath from userspace.
 *
 * The svc-i3c-master driver registers an I2C adapter for legacy-I2C devices on
 * the I3C bus; the tmp105 we declared under i3c2 lands on it. This helper scans
 * every /dev/i2c-N, and on each tries a SMBus round-trip to the tmp105 at 0x48:
 * write 0x60 to the config register (pointer 0x01), read it back, expect 0x60.
 * A successful round-trip means a real transfer traversed the I3C master
 * (MCTRL START/addr -> MWDATAB -> repeated-START -> MRDATAB), proving the
 * legacy-I2C path end to end. Prints I3C_I2C_OK on success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define TMP105_ADDR     0x48
#define TMP105_CONFIG   0x01
#define TEST_VAL        0x60

static int smbus(int fd, char rw, uint8_t cmd, int size,
                 union i2c_smbus_data *data)
{
    struct i2c_smbus_ioctl_data a = {
        .read_write = rw, .command = cmd, .size = size, .data = data,
    };
    return ioctl(fd, I2C_SMBUS, &a);
}

static int try_bus(const char *path)
{
    union i2c_smbus_data d;
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, TMP105_ADDR) < 0) {
        close(fd);
        return -1;
    }
    d.byte = TEST_VAL;
    if (smbus(fd, I2C_SMBUS_WRITE, TMP105_CONFIG,
              I2C_SMBUS_BYTE_DATA, &d) < 0) {
        close(fd);
        return -1;          /* NACK: no tmp105 on this bus */
    }
    memset(&d, 0, sizeof(d));
    if (smbus(fd, I2C_SMBUS_READ, TMP105_CONFIG,
              I2C_SMBUS_BYTE_DATA, &d) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    printf("%s: tmp105@0x48 config wrote 0x%02x read 0x%02x\n",
           path, TEST_VAL, d.byte & 0xff);
    return (d.byte & 0xff) == TEST_VAL ? 0 : -2;
}

int main(void)
{
    DIR *dir = opendir("/dev");
    struct dirent *e;
    char path[280];
    int ok = -1;

    if (!dir) {
        printf("I3C_I2C_FAIL: cannot open /dev\n");
        return 1;
    }
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "i2c-", 4) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        if (try_bus(path) == 0) {
            ok = 0;
            break;
        }
    }
    closedir(dir);

    if (ok == 0) {
        printf("I3C_I2C_OK\n");
        return 0;
    }
    printf("I3C_I2C_FAIL: no tmp105 round-trip on any i2c bus\n");
    return 1;
}
