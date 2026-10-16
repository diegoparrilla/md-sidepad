/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: rumble_test.c
 * Description: Host test for the rumble decisions in rumble.h.
 *
 * The hazard is at both ends. Stop re-arming a held level and the motor
 * gaps; fail to stop and a pad buzzes on after the ST has reset, or a game
 * has unhooked the handler that sends the keepalive. Neither shows up on a
 * bench until it has already happened, so both are checked here. Run it
 * with `make -C rp/test`.
 */
#include <stdio.h>
#include <string.h>

#include "include/rumble.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/* The word the ST sends for one pad: low frequency motor high byte. */
#define W(lo, hi) ((uint16_t)(((lo) << 8) | (hi)))

int main(void)
{
    rumble_logic_t r;
    uint16_t w[RUMBLE_PADS] = {0};
    uint8_t arm, stop;
    uint32_t t;
    int armed, stopped;

    printf("rumble decisions\n\n");

    memset(&r, 0, sizeof(r));

    /* Nothing asked: nothing to do, now or later. */
    arm = rumble_logic_frame(&r, w, 1000);
    check(arm == 0, "an all-zero first frame arms nothing");
    arm = rumble_logic_tick(&r, 5000, &stop);
    check(arm == 0 && stop == 0, "and a quiet pad is never armed or stopped");

    /* A level arrives: armed at once, levels split the right way round. */
    w[1] = W(200, 30);
    arm = rumble_logic_frame(&r, w, 10000);
    check(arm == 0x02, "a new level arms just that pad");
    check(r.lo[1] == 200 && r.hi[1] == 30, "lo is the high byte, hi the low");

    /* Held, with the ST's keepalive arriving: re-armed at the interval and
     * never stopped. */
    armed = 0;
    stopped = 0;
    for (t = 10000; t < 12000; t += 20)
    {
        if (t % 200 == 0) rumble_logic_frame(&r, w, t); /* keepalive */
        arm = rumble_logic_tick(&r, t, &stop);
        if (arm & 0x02) armed++;
        if (stop) stopped++;
    }
    /* Armed by the frame at 10000, then at 10200 ... 11800. */
    check(armed == 9, "a held level re-arms once per interval");
    check(stopped == 0, "and is never stopped while the ST keeps asking");
    check(RUMBLE_BURST_MS > RUMBLE_REARM_MS, "a burst outlasts the re-arm, so no gap");

    /* A keepalive repeating the same level is not a change. */
    arm = rumble_logic_frame(&r, w, 12000);
    check(arm == 0, "an unchanged keepalive arms nothing by itself");

    /* The ST goes quiet: one stop, after the timeout and not before. */
    armed = 0;
    stopped = 0;
    for (t = 12000; t < 14000; t += 20)
    {
        arm = rumble_logic_tick(&r, t, &stop);
        if (stop & 0x02)
        {
            stopped++;
            check(t - 12000 >= RUMBLE_TIMEOUT_MS, "stopped no sooner than the timeout");
            check(t - 12000 < RUMBLE_TIMEOUT_MS + 20, "and no later");
        }
        if (arm & 0x02 && stopped) armed++;
    }
    check(stopped == 1, "a silent ST stops the pad exactly once");
    check(armed == 0, "and it is not re-armed afterwards");
    check(r.lo[1] == 0 && r.hi[1] == 0, "its levels read zero once stopped");

    /* A frame after the stop starts it again. */
    arm = rumble_logic_frame(&r, w, 20000);
    check(arm == 0x02, "the next frame after a stop arms it again");

    /* Zero is a change that arms (stopping it at once), then stays quiet. */
    w[1] = 0;
    arm = rumble_logic_frame(&r, w, 20100);
    check(arm == 0x02, "a zero level arms the pad, which stops it");
    arm = rumble_logic_tick(&r, 25000, &stop);
    check(arm == 0 && stop == 0, "and a stopped pad is never re-armed");

    /* One motor is enough to keep it going. */
    w[3] = W(0, 90);
    rumble_logic_frame(&r, w, 30000);
    arm = rumble_logic_tick(&r, 30000 + RUMBLE_REARM_MS, &stop);
    check(arm == 0x08, "a high-only level re-arms too");

    /* A new session stops what the old one left running. */
    stop = rumble_logic_reset(&r);
    check(stop == 0x08, "a reset stops the pads still running");
    arm = rumble_logic_tick(&r, 31000, &stop);
    check(arm == 0 && stop == 0, "and leaves nothing to re-arm");

    /* A pad that leaves takes its level with it; a frame brings it back. */
    w[3] = W(10, 10);
    rumble_logic_frame(&r, w, 40000);
    rumble_logic_padGone(&r, 3);
    arm = rumble_logic_tick(&r, 40000 + RUMBLE_REARM_MS, &stop);
    check(arm == 0 && stop == 0, "a departed pad is neither armed nor stopped");
    arm = rumble_logic_frame(&r, w, 40300);
    check(arm == 0x08, "the ST's next frame arms the slot again");

    /* The ms clock wraps after 49 days; the arithmetic must not care. */
    memset(&r, 0, sizeof(r));
    w[0] = W(50, 0);
    w[3] = 0;
    rumble_logic_frame(&r, w, 0xFFFFFF00u);
    arm = rumble_logic_tick(&r, 0xFFFFFF00u + RUMBLE_REARM_MS, &stop);
    check(arm == 0x01 && stop == 0, "re-arming survives the clock wrapping");

    printf("\n%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
