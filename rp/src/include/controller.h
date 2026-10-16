/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: controller.h
 * Description: BLE controller host integration and UI-facing state snapshot.
 */

#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

// Analog triggers (lt/rt, 0..1) count as fire once pulled past this point.
#define CONTROLLER_TRIGGER_THRESHOLD 0.5f

// Controllers connected at once. A pad's slot is its Bluepad32 device index:
// the first free one when it connects, kept while it stays connected. Slot 0
// is player 1, the only pad injected as joystick 1 and the mouse; every slot
// is published to Xpad. Asserted against CONFIG_BLUEPAD32_MAX_DEVICES.
#define CONTROLLER_MAX_PADS 4

typedef struct {
  bool initialized;
  bool paired;
  bool connected;
  bool pairingRequested;
  char pairedAddress[24];
  char pairedAddressType[12];
  char deviceName[32];
  char status[64];

  float lx;
  float ly;
  float rx;
  float ry;
  float lt;
  float rt;

  uint8_t hat;

  bool btnA;
  bool btnB;
  bool btnX;
  bool btnY;
  bool btnLB;
  bool btnRB;
  bool btnView;
  bool btnMenu;
  bool btnLS;
  bool btnRS;
  bool btnGuide;
  bool dpadUp;
  bool dpadDown;
  bool dpadLeft;
  bool dpadRight;

  // Digital, Atari ST style joystick: any stick or D-pad combined into 8
  // directions, plus a single fire button (any face/shoulder button).
  bool anyUp;
  bool anyDown;
  bool anyLeft;
  bool anyRight;
  bool anyButton;

  // Digital directions split by source, for the dual joystick/mouse UI:
  //   pad*    = left stick + D-pad  (shown as the "Joystick" pair)
  //   rstick* = right stick only    (shown as the "Mouse" pair)
  // any* above remains the union of both, so joystick injection is unchanged.
  bool padUp;
  bool padDown;
  bool padLeft;
  bool padRight;
  bool rstickUp;
  bool rstickDown;
  bool rstickLeft;
  bool rstickRight;
} controller_state_t;

int controller_init(void);
void controller_poll(void);
void controller_requestPairing(void);
// Set the status-line text shown in the controller UI. Used to surface
// boot/init progress and errors in the same slot as live controller status.
void controller_setStatus(const char *message);
// Slot 0's state, which is what joystick and mouse injection and the
// visualiser show. The same as controller_getPadState(0, outState).
void controller_getState(controller_state_t *outState);
// One slot's state, 0..CONTROLLER_MAX_PADS-1. An empty slot reads as
// disconnected, with its axes at rest.
void controller_getPadState(int idx, controller_state_t *outState);
// Drive a connected pad's motors for durationMs. Bluepad32 names them weak
// (the small, fast motor) and strong. Returns false, doing nothing, for an
// empty slot or a pad whose driver has no rumble.
bool controller_playRumble(int idx, uint8_t weak, uint8_t strong,
                           uint16_t durationMs);
bool controller_getAndClearRestartRequested(void);

#endif  // CONTROLLER_H
