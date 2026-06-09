/*
 * Watchdog bite helper for the i.MX 95 WDOG3 functional test.
 *
 * Copyright (c) 2026, Kyle Fox
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Opens /dev/watchdog0 (which starts the hardware watchdog), reports the
 * effective timeout, and then sleeps forever WITHOUT ever issuing a keepalive.
 * With nothing refreshing it, the WDOG3 timeout elapses and the model fires the
 * QEMU watchdog action. Run the VM with -action watchdog=poweroff so the bite
 * cleanly terminates QEMU. Cross-compile static for aarch64.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

int main(void)
{
    int to = 1, fd = open("/dev/watchdog0", O_WRONLY);

    if (fd < 0) {
        printf("WDT-OPEN-FAIL\n");
        return 1;
    }
    ioctl(fd, WDIOC_SETTIMEOUT, &to);
    ioctl(fd, WDIOC_GETTIMEOUT, &to);
    printf("WDT-ARMED timeout=%ds (no keepalive)\n", to);
    fflush(stdout);
    for (;;) {
        sleep(1);   /* never ping -> the watchdog bites */
    }
}
