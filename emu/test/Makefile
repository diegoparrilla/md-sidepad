# SPDX-License-Identifier: GPL-3.0-or-later
# XPADTEST.PRG, the ST half of emu/test.sh. Build inside
# atarist-toolkit-docker, from the repository root so the xpad submodule
# is inside the mount:  stcmd make -C emu/test
CC      = m68k-atari-mint-gcc
XPAD    = ../../xpad/src
CFLAGS  = -O2 -fomit-frame-pointer -Wall -Wextra -Werror -I $(XPAD)
LDFLAGS = -s

dist/XPADTEST.PRG: xpadtest.c $(XPAD)/xpad.c $(XPAD)/xpad.h
	@mkdir -p dist
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ xpadtest.c $(XPAD)/xpad.c

clean:
	rm -rf dist

.PHONY: clean
