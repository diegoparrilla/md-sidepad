/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: xpadstate.c
 * Description: Publish the connected controllers as an Xpad block.
 *
 * See xpadstate.h for what this is for. xpad.h is the ABI this writes,
 * and it comes from the xpad submodule at the repository root rather
 * than a copy in this tree, so the layout and the constants cannot
 * drift from what consumers are compiled against. Update it with
 * `git submodule update --remote xpad` and read the diff.
 */

#include "include/xpadstate.h"

#include <stddef.h>
#include <string.h>

#include "chandler.h"
#include "constants.h"
#include "debug.h"
#include "include/controller.h"
#include "xpad.h"

/* ------------------------------------------------------------------ */
/* Byte order                                                          */
/* ------------------------------------------------------------------ */

/*
 * The RP is little endian, the m68k is big endian, and the window is
 * served as 16 bit words. The upshot, and it is the same rule
 * SET_SHARED_VAR already relies on:
 *
 *   - a uint16_t stored here reads back correctly on the ST
 *   - a 32 bit value needs its two halves swapped, high word first
 *   - a *byte* at m68k offset N lives at N ^ 1 on this side
 *
 * The shared variables never hit the third case because they are all
 * whole words. This block has byte fields, so it shows up explicitly.
 */

/* The address userfw.s hardcodes has to be the one the framework leaves
 * free. If APP_FREE ever moves, this stops the build rather than quietly
 * publishing a block into somebody else's memory. */
_Static_assert(XPADSTATE_BLOCK_OFFSET == CHANDLER_APP_FREE_OFFSET,
               "Xpad block is not at the start of APP_FREE");

static uintptr_t rom;
static uint16_t seq;
static uint8_t active;

static void put16(uint32_t off, uint16_t v)
{
  *(volatile uint16_t *)(rom + off) = v;
}

static void put32(uint32_t off, uint32_t v)
{
  put16(off, (uint16_t)(v >> 16));
  put16(off + 2, (uint16_t)(v & 0xffffu));
}

static void put8(uint32_t off, uint8_t v)
{
  *(volatile uint8_t *)(rom + (off ^ 1u)) = v;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/*
 * Field offsets are written out rather than taken with offsetof,
 * because this builds the m68k layout and we are compiling for ARM: the
 * two pointer fields are 2-aligned there and 4-aligned here, so
 * everything from provider onwards sits at a different offset. The
 * fixed header before them is identical on both, so those offsets are
 * asserted against the submodule's header below and cannot drift.
 */
#define OFF_MAGIC 0
#define OFF_VERSION 4
#define OFF_HDR_SIZE 6
#define OFF_PADS_OFFSET 8
#define OFF_PAD_SIZE 10
#define OFF_CAPS 12
#define OFF_SEQ 14
#define OFF_PAD_COUNT 16
#define OFF_ACTIVE 17
/* Past the fixed header the block holds two m68k pointers, so these
 * cannot be asserted against offsetof: on ARM the same fields sit at
 * 20/24/28. Derive them from the one constant that IS asserted below,
 * plus the single fact that makes them m68k-specific, so there is one
 * hand-written number here rather than three. */
#define M68K_PTR_SIZE 4
#define OFF_PROVIDER XPAD_HDR_FIXED
#define OFF_REQ (OFF_PROVIDER + M68K_PTR_SIZE)
#define OFF_PADS (OFF_REQ + M68K_PTR_SIZE)

/* userfw.s spells this address out as XPAD_REQ_PTR_ADDR, block + 22, and
 * polls it during its handshake, so it cannot be allowed to move. */
_Static_assert(OFF_REQ == 22, "userfw.s XPAD_REQ_PTR_ADDR assumes req at 22");

/* The request area itself is userfw.s's res_req, laid out by hand. XPAD_REQ
 * holds no pointers, so these hold on the m68k as well as here. */
_Static_assert(sizeof(XPAD_REQ) == 16, "userfw.s res_req is 16 bytes");
_Static_assert(offsetof(XPAD_REQ, seq) == 2, "userfw.s reads seq at 2");
_Static_assert(offsetof(XPAD_REQ, rumble) == 4, "userfw.s reads rumble at 4");

_Static_assert(OFF_MAGIC == offsetof(XPAD, magic), "magic moved");
_Static_assert(OFF_VERSION == offsetof(XPAD, version), "version moved");
_Static_assert(OFF_HDR_SIZE == offsetof(XPAD, hdr_size), "hdr_size moved");
_Static_assert(OFF_PADS_OFFSET == offsetof(XPAD, pads_offset),
               "pads_offset moved");
_Static_assert(OFF_PAD_SIZE == offsetof(XPAD, pad_size), "pad_size moved");
_Static_assert(OFF_CAPS == offsetof(XPAD, caps), "caps moved");
_Static_assert(OFF_SEQ == offsetof(XPAD, seq), "seq moved");
_Static_assert(OFF_PAD_COUNT == offsetof(XPAD, pad_count), "pad_count moved");
_Static_assert(OFF_ACTIVE == offsetof(XPAD, active), "active moved");
_Static_assert(OFF_ACTIVE + 1 == XPAD_HDR_FIXED,
               "the fixed header is not what Xpad says it is");

/* XPAD_PAD holds no pointers, so its layout is the same on both sides
 * and every offset can be asserted. */
#define PAD_BUTTONS 0
#define PAD_LX 4
#define PAD_RX 6
#define PAD_LT 8
#define PAD_TYPE 10

_Static_assert(PAD_BUTTONS == offsetof(XPAD_PAD, buttons), "buttons moved");
_Static_assert(PAD_LX == offsetof(XPAD_PAD, lx), "lx moved");
_Static_assert(PAD_RX == offsetof(XPAD_PAD, rx), "rx moved");
_Static_assert(PAD_LT == offsetof(XPAD_PAD, lt), "lt moved");
_Static_assert(PAD_TYPE == offsetof(XPAD_PAD, type), "type moved");
_Static_assert(sizeof(XPAD_PAD) == XPAD_PAD_SIZE_V1, "XPAD_PAD is not v1");

/* Room for the maximum, so raising XPADSTATE_PAD_COUNT later moves
 * nothing. xpad_valid() only requires the pads in use to fit. */
#define HDR_SIZE (OFF_PADS + 2 * XPAD_MAX_PADS * XPAD_PAD_SIZE_V1)

_Static_assert(HDR_SIZE <= XPADSTATE_BLOCK_SIZE,
               "the block does not fit in the space reserved for it");

/* A slot per controller, and no more slots than Xpad allows. */
_Static_assert(XPADSTATE_PAD_COUNT == CONTROLLER_MAX_PADS,
               "one Xpad pad per controller slot");
_Static_assert(XPADSTATE_PAD_COUNT <= XPAD_MAX_PADS, "too many pads for Xpad");

/* Analogue because the sticks are real, hotplug because a Bluetooth pad
 * comes and goes. Rumble is added by xpadstate_setReq(), once there is a
 * request area to ask through. */
#define CAPS_BASE (XPAD_CAP_ANALOG | XPAD_CAP_HOTPLUG)

#define BLK(off) (XPADSTATE_BLOCK_OFFSET + (uint32_t)(off))

/* ------------------------------------------------------------------ */
/* Conversion                                                          */
/* ------------------------------------------------------------------ */

/* Bluepad32 axes arrive normalised to 0..1 with 0.5 at rest. Xpad wants
 * signed -127..127, screen oriented, which is the same sense: a stick
 * pushed down reads positive on both. */
static int8_t axisFrom(float v)
{
  float centred = (v - 0.5f) * 254.0f;
  int n = (int)(centred < 0.0f ? centred - 0.5f : centred + 0.5f);

  if (n > 127) n = 127;
  if (n < -127) n = -127;

  return (int8_t)n;
}

static uint8_t triggerFrom(float v)
{
  int n = (int)(v * 255.0f + 0.5f);

  if (n > 255) n = 255;
  if (n < 0) n = 0;

  return (uint8_t)n;
}

static uint32_t buttonsFrom(const controller_state_t *st)
{
  uint32_t b = 0;

  /*
   * Directions come from the D-pad and left stick, which controller.c
   * has already combined and deadzoned. The right stick is deliberately
   * excluded: it is the aim stick, and in mouse mode it is the cursor,
   * so folding it into the d-pad would make a consumer see movement
   * nobody asked for.
   */
  if (st->padUp) b |= XPAD_UP;
  if (st->padDown) b |= XPAD_DOWN;
  if (st->padLeft) b |= XPAD_LEFT;
  if (st->padRight) b |= XPAD_RIGHT;

  /*
   * Map by physical position, never by the letter printed on the pad.
   * Xpad follows the kernel, where BTN_X aliases north and BTN_Y aliases
   * west, while Bluepad32's btnX and btnY are the Xbox positions: left
   * and top. Going through the positional names keeps that straight.
   */
  if (st->btnA) b |= XPAD_SOUTH; /* bottom */
  if (st->btnB) b |= XPAD_EAST;  /* right  */
  if (st->btnX) b |= XPAD_WEST;  /* left   */
  if (st->btnY) b |= XPAD_NORTH; /* top    */

  if (st->btnLB) b |= XPAD_TL;
  if (st->btnRB) b |= XPAD_TR;

  if (st->lt > CONTROLLER_TRIGGER_THRESHOLD) b |= XPAD_TL2;
  if (st->rt > CONTROLLER_TRIGGER_THRESHOLD) b |= XPAD_TR2;

  if (st->btnView) b |= XPAD_SELECT;
  if (st->btnMenu) b |= XPAD_START;
  if (st->btnGuide) b |= XPAD_MODE;
  if (st->btnLS) b |= XPAD_THUMBL;
  if (st->btnRS) b |= XPAD_THUMBR;

  return b;
}

/* ------------------------------------------------------------------ */
/* Publishing                                                          */
/* ------------------------------------------------------------------ */

static void writeProvider(void)
{
  const char *name = "MD/Sidepad " RELEASE_VERSION;
  uint32_t i;

  for (i = 0; i < XPADSTATE_PROVIDER_SIZE; i++)
  {
    char c = (i + 1 < XPADSTATE_PROVIDER_SIZE) ? name[i] : '\0';

    put8(XPADSTATE_PROVIDER_OFFSET + i, (uint8_t)c);

    if (!c) break; /* the loop above already stops one short of the end */
  }
}

void xpadstate_init(void)
{
  uint32_t i;

  rom = (uintptr_t)&__rom_in_ram_start__;
  seq = 0;
  active = 0;

  /* Both buffers start empty, and so does anything reserved but unused. */
  for (i = 0; i < XPADSTATE_BLOCK_SIZE; i += 2) put16(BLK(i), 0);

  put32(BLK(OFF_MAGIC), XPAD_MAGIC);
  put16(BLK(OFF_VERSION), XPAD_VERSION);
  put16(BLK(OFF_HDR_SIZE), HDR_SIZE);
  put16(BLK(OFF_PADS_OFFSET), OFF_PADS);
  put16(BLK(OFF_PAD_SIZE), XPAD_PAD_SIZE_V1);

  /* No rumble or LED yet: req is NULL until the ST says where its request
   * area is, and claiming a capability with nowhere to write it would be a
   * lie. */
  put16(BLK(OFF_CAPS), CAPS_BASE);

  put16(BLK(OFF_SEQ), 0);
  put8(BLK(OFF_PAD_COUNT), XPADSTATE_PAD_COUNT);
  put8(BLK(OFF_ACTIVE), 0);

  writeProvider();
  put32(BLK(OFF_PROVIDER), XPADSTATE_ST_PROVIDER);
  put32(BLK(OFF_REQ), 0);

  DPRINTF("Xpad block at m68k %08lx, provider at %08lx\n",
          (unsigned long)XPADSTATE_ST_BLOCK,
          (unsigned long)XPADSTATE_ST_PROVIDER);
}

void xpadstate_publish(void)
{
  uint8_t back = active ^ 1u;
  int i;

  /*
   * Every slot, every commit, whether it changed or not. Consumers read
   * the whole front buffer, so a slot left alone here would show what it
   * held two commits ago once the buffers flip back.
   */
  for (i = 0; i < XPADSTATE_PAD_COUNT; i++)
  {
    controller_state_t st = {0};
    uint32_t padOff;
    uint32_t buttons;
    uint8_t type, flags;

    controller_getPadState(i, &st);

    padOff = BLK(OFF_PADS) +
             ((uint32_t)back * XPADSTATE_PAD_COUNT + (uint32_t)i) *
                 XPAD_PAD_SIZE_V1;

    if (st.connected)
    {
      buttons = buttonsFrom(&st);
      type = XPAD_TYPE_GAMEPAD; /* Bluepad32 knows the family; we do not
                                 * carry it through yet */
      flags = XPAD_PAD_ANALOG | XPAD_PAD_WIRELESS;
    }
    else
    {
      /* Nothing in this slot: empty, not a pad stuck at its last state. */
      buttons = 0;
      type = XPAD_TYPE_NONE;
      flags = 0;
    }

    put32(padOff + PAD_BUTTONS, buttons);

    if (st.connected)
    {
      put16(padOff + PAD_LX,
            (uint16_t)(((uint8_t)axisFrom(st.lx) << 8) | (uint8_t)axisFrom(st.ly)));
      put16(padOff + PAD_RX,
            (uint16_t)(((uint8_t)axisFrom(st.rx) << 8) | (uint8_t)axisFrom(st.ry)));
      put16(padOff + PAD_LT,
            (uint16_t)((triggerFrom(st.lt) << 8) | triggerFrom(st.rt)));
    }
    else
    {
      put16(padOff + PAD_LX, 0);
      put16(padOff + PAD_RX, 0);
      put16(padOff + PAD_LT, 0);
    }

    put16(padOff + PAD_TYPE, (uint16_t)(((uint16_t)type << 8) | flags));
  }

  /*
   * Commit, in the order the ABI requires: the back buffer is complete,
   * then seq moves, then active flips. A consumer rechecks both after
   * copying, and that recheck only means anything if seq leads.
   */
  seq++;
  put16(BLK(OFF_SEQ), seq);
  put8(BLK(OFF_ACTIVE), back);

  active = back;
}

void xpadstate_setReq(uint32_t m68kAddr)
{
  /*
   * The pointer goes in first, then the capability: a consumer that sees
   * XPAD_CAP_RUMBLE must also see somewhere to write. Both are single
   * fields the ST reads whole, and the installer does not put the cookie
   * in the jar until it has seen this address, so no consumer can be
   * mid-read of either.
   */
  put32(BLK(OFF_REQ), m68kAddr);
  put16(BLK(OFF_CAPS), m68kAddr ? (CAPS_BASE | XPAD_CAP_RUMBLE) : CAPS_BASE);
}
