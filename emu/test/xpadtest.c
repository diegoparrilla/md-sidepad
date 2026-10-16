/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: xpadtest.c
 * Description: MD/Sidepad's Xpad block, read back on an emulated ST.
 *
 * Run from A:\AUTO by emu/test.sh, once the firmware has left the
 * terminal, with emu/test/pads.txt playing the controllers through
 * EmuMD's Bluetooth stand-in. It checks what a game would see: four
 * slots, rumble on offer, the second controller in slot 1 and gone again
 * when it disconnects. Then it asks for rumble on two pads and stops it,
 * which the script checks from the other end, in the firmware's log.
 *
 * Everything it finds goes to the console, which Hatari's --conout hands
 * to the script; each check prints "ok" or "FAIL", and the last line is
 * XPADTEST-DONE and the number that failed.
 */
#include <stdio.h>
#include <mint/osbind.h>

#include "xpad.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("XPADTEST %-40s %s\r\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static void frames(int n)
{
    while (n-- > 0) Vsync();
}

/* Wait up to `limit` frames for pad i's type to be (or not be) `type`. */
static int wait_type(const XPAD *x, int i, int type, int want, int limit)
{
    XPAD_PAD p;

    while (limit-- > 0)
    {
        if (xpad_read(x, i, &p) && ((p.type == type) == want)) return 1;
        Vsync();
    }
    return 0;
}

int main(void)
{
    const XPAD *x = xpad_find();
    XPAD_REQ *req;
    XPAD_PAD p;
    int i;

    printf("XPADTEST MD/Sidepad under EmuMD\r\n");
    check(x != NULL, "found the XPAD cookie");
    if (x == NULL)
    {
        printf("XPADTEST-DONE %d\r\n", failures);
        return 1;
    }

    printf("XPADTEST provider %s pads=%d caps=%04x\r\n",
           x->provider ? x->provider : "?", x->pad_count, x->caps);
    check(x->pad_count == 4, "four pad slots");
    check((x->caps & XPAD_CAP_RUMBLE) != 0, "rumble on offer");

    /* Both pads connected while the terminal was up. */
    check(wait_type(x, 0, XPAD_TYPE_GAMEPAD, 1, 50), "slot 0 has a gamepad");
    check(wait_type(x, 1, XPAD_TYPE_GAMEPAD, 1, 50), "slot 1 has one too");
    check(xpad_connected(x) == 2, "two pads connected");

    /* The script holds South on slot 1 alone. */
    for (i = 0; i < 50; i++)
    {
        if (xpad_read(x, 1, &p) && (p.buttons & XPAD_SOUTH)) break;
        Vsync();
    }
    printf("XPADTEST pad1 buttons=%08lx type=%d\r\n",
           (unsigned long)p.buttons, p.type);
    check((p.buttons & XPAD_SOUTH) != 0, "slot 1 sees its own button");
    check(xpad_read(x, 0, &p) && !(p.buttons & XPAD_SOUTH),
          "and slot 0 does not");

    /* Rumble: low motor on pad 0, high motor on pad 1, held for a second,
     * then both stopped. Values first, seq last, as xpad.h asks. */
    req = xpad_req(x);
    check(req != NULL, "a request area to write");
    if (req != NULL)
    {
        req->rumble[0][0] = 200;
        req->rumble[1][1] = 90;
        req->seq++;
        frames(50);
        req->rumble[0][0] = 0;
        req->rumble[1][1] = 0;
        req->seq++;
        frames(25);
        printf("XPADTEST rumble requested and stopped\r\n");
    }

    /* The script disconnects slot 1, later; slot 0 stays. */
    check(wait_type(x, 1, XPAD_TYPE_NONE, 1, 1500), "slot 1 empties on disconnect");
    check(wait_type(x, 0, XPAD_TYPE_GAMEPAD, 1, 1), "while slot 0 stays");

    printf("XPADTEST-DONE %d\r\n", failures);
    return failures ? 1 : 0;
}
