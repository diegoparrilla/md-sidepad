/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: xpadstate_test.c
 * Description: Host test for the Xpad block writer.
 *
 * The RP is little endian and the m68k is big endian, and the window is
 * served as 16 bit words, so every field has to be placed by hand. That
 * is the part worth checking without hardware, and it is checked the
 * only way that means anything: build the block, then read it back the
 * way the ST would, bytes within each word the other way round.
 *
 * Stub headers in stub/ stand in for the Pico SDK, so this builds with
 * a plain host compiler. Run it with `make -C rp/test`.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "xpad.h"
#include "include/xpadstate.h"
#include "include/controller.h"

#define RELEASE_VERSION "9.9.9"

unsigned char xpadtest_rom[0x10000];

/* xpadstate.c pulls its input from here; stand in for controller.c. */
static controller_state_t gState[CONTROLLER_MAX_PADS];
void controller_getPadState(int idx, controller_state_t *out) { *out = gState[idx]; }

#include "../src/xpadstate.c"

/* The ST's view: bytes within each 16-bit word are the other way round. */
static uint8_t st8(uint32_t off) { return xpadtest_rom[off ^ 1u]; }
static uint16_t st16(uint32_t off) { return (uint16_t)((st8(off) << 8) | st8(off + 1)); }
static uint32_t st32(uint32_t off) { return ((uint32_t)st16(off) << 16) | st16(off + 2); }

static int failures;
static void check(int ok, const char *what)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

#define B XPADSTATE_BLOCK_OFFSET

int main(void)
{
    uint32_t padOff;
    uint16_t seq0;
    uint8_t act0;
    int i, ok;

    printf("xpadstate byte order and layout\n\n");

    check(CHANDLER_APP_FREE_OFFSET == 0x2300, "APP_FREE is where chandler.h says");

    memset(xpadtest_rom, 0xa5, sizeof(xpadtest_rom));
    xpadstate_init();

    check(st32(B + 0) == XPAD_MAGIC, "magic reads as 'XPAD' on the ST");
    check(st16(B + 4) == XPAD_VERSION, "version");
    check(st16(B + 6) == HDR_SIZE, "hdr_size");
    check(st16(B + 8) == 26, "pads_offset");
    check(st16(B + 10) == XPAD_PAD_SIZE_V1, "pad_size");
    check(st16(B + 12) == (XPAD_CAP_ANALOG | XPAD_CAP_HOTPLUG), "caps");
    check(st8(B + 16) == XPADSTATE_PAD_COUNT, "pad_count is a byte at offset 16");
    check(st8(B + 17) == 0, "active is a byte at offset 17");
    check(st32(B + 18) == XPADSTATE_ST_PROVIDER, "provider points into the window");
    check(st32(B + 22) == 0, "req is NULL");
    check(XPADSTATE_PAD_COUNT == 4, "four pads");

    /* Everything xpad_valid() insists on. */
    check(st16(B + 10) >= XPAD_PAD_SIZE_V1, "pad_size clears the v1 floor");
    check(st16(B + 8) >= XPAD_HDR_FIXED, "pads start past the fixed header");
    check(st16(B + 8) <= st16(B + 6), "pads start inside the block");
    check((uint32_t)st8(B + 16) * 2u * st16(B + 10) <=
              (uint32_t)(st16(B + 6) - st16(B + 8)),
          "both pad buffers fit inside hdr_size");

    ok = 1;
    for (i = 0; i < 11; i++)
    {
        char want = "MD/Sidepad "[i];
        if ((char)st8(XPADSTATE_PROVIDER_OFFSET + i) != want) ok = 0;
    }
    check(ok, "provider string reads the right way round");

    /* A pad with something in every field, in slot 0. */
    memset(gState, 0, sizeof(gState));
    gState[0].connected = true;
    gState[0].btnA = true;    /* south */
    gState[0].btnY = true;    /* top -> north, not XPAD_Y */
    gState[0].btnRS = true;   /* right thumb */
    gState[0].padLeft = true;
    gState[0].lx = 1.0f;      /* full right */
    gState[0].ly = 0.0f;      /* full up */
    gState[0].rx = 0.5f;      /* centred */
    gState[0].lt = 1.0f;
    gState[0].rt = 0.0f;

    /* And a second pad in slot 2, holding only East, with slots 1 and 3
     * empty: each must land in its own place and nowhere else. */
    gState[2].connected = true;
    gState[2].btnB = true;
    gState[2].lx = 0.5f;
    gState[2].ly = 0.5f;
    gState[2].rx = 0.5f;
    gState[2].ry = 1.0f;      /* right stick full down */

    seq0 = st16(B + 14);
    act0 = st8(B + 17);
    xpadstate_publish();

    check(st16(B + 14) == (uint16_t)(seq0 + 1), "seq advances on commit");
    check(st8(B + 17) == (act0 ^ 1), "active flips to the other buffer");

    padOff = B + 26 + (uint32_t)st8(B + 17) * XPADSTATE_PAD_COUNT * XPAD_PAD_SIZE_V1;

    check(st32(padOff + 0) == (XPAD_SOUTH | XPAD_NORTH | XPAD_THUMBR | XPAD_LEFT |
                               XPAD_TL2),
          "buttons map by position, with the trigger digitised");
    check((int8_t)st8(padOff + 4) == 127, "lx full right is +127");
    check((int8_t)st8(padOff + 5) == -127, "ly full up is -127");
    check((int8_t)st8(padOff + 6) == 0, "rx centred is 0");
    check(st8(padOff + 8) == 255, "lt fully pulled is 255");
    check(st8(padOff + 9) == 0, "rt released is 0");
    check(st8(padOff + 10) == XPAD_TYPE_GAMEPAD, "type says gamepad");
    check(st8(padOff + 11) == (XPAD_PAD_ANALOG | XPAD_PAD_WIRELESS), "flags");

    /* The slots after it, pad_size apart within the same buffer. */
    check(st8(padOff + 12 + 10) == XPAD_TYPE_NONE, "slot 1 is empty");
    check(st32(padOff + 24) == XPAD_EAST, "slot 2 carries the second pad");
    check(st8(padOff + 24 + 10) == XPAD_TYPE_GAMEPAD, "and says gamepad");
    check((int8_t)st8(padOff + 24 + 7) == 127, "its own right stick, not slot 0's");
    check(st8(padOff + 36 + 10) == XPAD_TYPE_NONE, "slot 3 is empty");

    /* Disconnect: an empty slot, not a pad frozen at its last state. */
    gState[0].connected = false;
    xpadstate_publish();
    padOff = B + 26 + (uint32_t)st8(B + 17) * XPADSTATE_PAD_COUNT * XPAD_PAD_SIZE_V1;
    check(st8(padOff + 10) == XPAD_TYPE_NONE, "disconnecting empties the slot");
    check(st32(padOff + 0) == 0, "and clears the buttons");
    check(st32(padOff + 24) == XPAD_EAST, "while slot 2 carries on");

    /* Both buffers are written in full: the one just made the back buffer
     * must also say slot 0 is empty once it is published again. */
    xpadstate_publish();
    padOff = B + 26 + (uint32_t)st8(B + 17) * XPADSTATE_PAD_COUNT * XPAD_PAD_SIZE_V1;
    check(st8(padOff + 10) == XPAD_TYPE_NONE, "no slot is two commits old");

    /* Buffers must alternate, and never be the one being read. */
    ok = 1;
    for (i = 0; i < 4; i++)
    {
        uint8_t before = st8(B + 17);
        xpadstate_publish();
        if (st8(B + 17) == before) ok = 0;
    }
    check(ok, "every commit swaps buffers");

    check(HDR_SIZE <= XPADSTATE_BLOCK_SIZE, "the block fits its reservation");

    /* The request area the ST hands over, and the capability with it. */
    xpadstate_setReq(0x00012346UL);
    check(st32(B + 22) == 0x00012346UL, "req reads back as the ST's address");
    check(st16(B + 12) == (XPAD_CAP_ANALOG | XPAD_CAP_HOTPLUG | XPAD_CAP_RUMBLE),
          "and rumble is claimed with it");
    xpadstate_setReq(0);
    check(st32(B + 22) == 0 && st16(B + 12) == (XPAD_CAP_ANALOG | XPAD_CAP_HOTPLUG),
          "a NULL req claims no rumble");

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
