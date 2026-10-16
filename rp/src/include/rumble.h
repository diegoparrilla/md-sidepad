/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: rumble.h
 * Description: Drive the controllers' motors from the ST's Xpad requests.
 *
 * A game asks for rumble by writing levels into the XPAD_REQ area in ST RAM.
 * The resident m68k hook (userfw.s) forwards all four pads' levels as one
 * APP_XPAD_RUMBLE frame over ROM3 whenever they change, and again every
 * keepalive (about 200 ms) while any of them is non-zero. A level is held
 * until it is replaced, but Bluepad32 plays rumble for a duration, so a held
 * level is re-armed as a burst slightly longer than the interval it is
 * re-armed at: the motor never gaps.
 *
 * The safety stop is the reason the keepalive exists. A pad whose requests
 * stop arriving for RUMBLE_TIMEOUT_MS is stopped, so an ST that resets, or a
 * game that unhooks the resident handler, cannot leave a motor running.
 * (COMpad, which this follows, had no such timeout, and an ST reset there
 * left the pad buzzing until it disconnected.)
 *
 * The decisions are below as SDK-free inline functions so rp/test can check
 * them on the host. rumble.c turns them into Bluepad32 calls.
 */

#ifndef RUMBLE_H
#define RUMBLE_H

#include <stdbool.h>
#include <stdint.h>

#define RUMBLE_PADS 4
#define RUMBLE_BURST_MS 250    // how long one arm lasts
#define RUMBLE_REARM_MS 200    // how often a held level is armed again
#define RUMBLE_TIMEOUT_MS 600  // silence after which a pad is stopped

// Levels as Xpad names them: lo is the low frequency (strong, big) motor,
// hi the high frequency (weak, small) one.
typedef struct {
  uint8_t lo[RUMBLE_PADS];
  uint8_t hi[RUMBLE_PADS];
  uint32_t armedMs[RUMBLE_PADS];  // when each pad was last armed
  uint32_t lastRxMs;              // when the last frame arrived
} rumble_logic_t;

// A frame arrived. w[i] is pad i's word as the ST sent it: lo in the high
// byte, hi in the low byte, which is XPAD_REQ.rumble[i] read as a big endian
// word. Returns the pads to arm now: those whose levels changed, including to
// zero, which arms them with nothing and so stops them at once.
static inline uint8_t rumble_logic_frame(rumble_logic_t *r, const uint16_t *w,
                                         uint32_t nowMs) {
  uint8_t arm = 0;
  for (int i = 0; i < RUMBLE_PADS; i++) {
    uint8_t lo = (uint8_t)(w[i] >> 8);
    uint8_t hi = (uint8_t)(w[i] & 0xFFu);
    if ((lo != r->lo[i]) || (hi != r->hi[i])) {
      r->lo[i] = lo;
      r->hi[i] = hi;
      r->armedMs[i] = nowMs;
      arm |= (uint8_t)(1u << i);
    }
  }
  r->lastRxMs = nowMs;
  return arm;
}

// Once per app loop. Returns the pads whose held level is due to be armed
// again, and sets *stop to the pads to stop because the ST has gone quiet;
// their levels are zeroed, so each is stopped exactly once.
static inline uint8_t rumble_logic_tick(rumble_logic_t *r, uint32_t nowMs,
                                        uint8_t *stop) {
  uint8_t arm = 0;
  bool quiet = (uint32_t)(nowMs - r->lastRxMs) >= RUMBLE_TIMEOUT_MS;
  *stop = 0;
  for (int i = 0; i < RUMBLE_PADS; i++) {
    if (!r->lo[i] && !r->hi[i]) {
      continue;
    }
    if (quiet) {
      r->lo[i] = 0;
      r->hi[i] = 0;
      *stop |= (uint8_t)(1u << i);
    } else if ((uint32_t)(nowMs - r->armedMs[i]) >= RUMBLE_REARM_MS) {
      r->armedMs[i] = nowMs;
      arm |= (uint8_t)(1u << i);
    }
  }
  return arm;
}

// A new session: the ST has just told us where its request area is, after a
// cold boot or a reset. Returns the pads that were still running, to stop.
static inline uint8_t rumble_logic_reset(rumble_logic_t *r) {
  uint8_t stop = 0;
  for (int i = 0; i < RUMBLE_PADS; i++) {
    if (r->lo[i] || r->hi[i]) {
      stop |= (uint8_t)(1u << i);
    }
    r->lo[i] = 0;
    r->hi[i] = 0;
  }
  return stop;
}

// A pad disconnected. Nothing to stop, since it has gone; just forget its
// levels, so a pad that later takes the slot starts quiet until the ST's next
// frame says otherwise.
static inline void rumble_logic_padGone(rumble_logic_t *r, int idx) {
  if ((idx >= 0) && (idx < RUMBLE_PADS)) {
    r->lo[idx] = 0;
    r->hi[idx] = 0;
  }
}

// rumble.c: the above, applied to the controllers.
void rumble_onFrame(const uint16_t *w);
void rumble_tick(void);
void rumble_reset(void);
void rumble_padGone(int idx);

#endif  // RUMBLE_H
