/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: rumble.c
 * Description: Apply the ST's rumble requests to the controllers.
 *
 * See rumble.h. Everything here runs on the app loop, the same thread that
 * polls BTstack, so it can call Bluepad32 directly.
 */

#include "include/rumble.h"

#include "debug.h"
#include "include/controller.h"
#include "pico/time.h"

static rumble_logic_t gRumble;

static uint32_t nowMs(void) { return to_ms_since_boot(get_absolute_time()); }

// Arm each pad in `arm` with its held levels (zero levels stop it), and stop
// each pad in `stop`. Bluepad32 calls the motors weak and strong; Xpad calls
// them high and low frequency, the same pair the other way round.
static void apply(uint8_t arm, uint8_t stop) {
  for (int i = 0; i < RUMBLE_PADS; i++) {
    uint8_t bit = (uint8_t)(1u << i);
    if (stop & bit) {
      controller_playRumble(i, 0, 0, RUMBLE_BURST_MS);
    } else if (arm & bit) {
      controller_playRumble(i, gRumble.hi[i], gRumble.lo[i], RUMBLE_BURST_MS);
    }
  }
}

void rumble_onFrame(const uint16_t *w) {
  uint8_t arm = rumble_logic_frame(&gRumble, w, nowMs());
  for (int i = 0; i < RUMBLE_PADS; i++) {
    if (arm & (1u << i)) {
      DPRINTF("Rumble: pad %d lo=%u hi=%u\n", i, gRumble.lo[i],
              gRumble.hi[i]);
    }
  }
  apply(arm, 0);
}

void rumble_tick(void) {
  uint8_t stop;
  uint8_t arm = rumble_logic_tick(&gRumble, nowMs(), &stop);
  if (stop) {
    DPRINTF("Rumble: ST went quiet, stopping 0x%02X\n", stop);
  }
  apply(arm, stop);
}

void rumble_reset(void) {
  // A frame before this one would otherwise count as recent.
  gRumble.lastRxMs = nowMs();
  apply(0, rumble_logic_reset(&gRumble));
}

void rumble_padGone(int idx) { rumble_logic_padGone(&gRumble, idx); }
