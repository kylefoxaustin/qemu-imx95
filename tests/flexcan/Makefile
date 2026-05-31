# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the static aarch64 cantest self-test binary used by run.sh.
#
#   make            # builds ./cantest with aarch64-linux-gnu-gcc
#   make TOOLCHAIN=aarch64-linux-musl-   # alternate cross prefix

TOOLCHAIN ?= aarch64-linux-gnu-
CC        := $(TOOLCHAIN)gcc
CFLAGS    := -static -O2 -Wall -Wextra

cantest: cantest.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f cantest

.PHONY: clean
