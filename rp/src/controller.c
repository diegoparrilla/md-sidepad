/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: controller.c
 * Description: Bluetooth controller integration via Bluepad32.
 *
 * Bluepad32 (https://github.com/ricardoquesada/bluepad32, vendored as a
 * submodule) handles Bluetooth Classic + BLE discovery, per-model HID quirks
 * (Xbox One/Series, DualShock, Switch, ...) and hands us a normalized
 * uni_gamepad_t. This file is the Bluepad32 "platform": it registers the
 * callbacks, maps the virtual gamepad onto controller_state_t, and exposes the
 * same controller_* API the UI already consumes.
 *
 * Bluepad32 runs on top of the Pico SDK's BTstack. We do NOT call
 * btstack_run_loop_execute() (which blocks); instead the existing app loop
 * keeps calling controller_poll() -> network_safePoll() -> cyw43_arch_poll(),
 * which services BTstack and therefore fires the Bluepad32 callbacks below.
 */

#include "include/controller.h"

#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "hardware/sync.h"
#include "include/network.h"
#include "include/rumble.h"
#include "pico/time.h"
#include "uni.h"

// Sanity check: we provide our own (custom) platform.
#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Pico W must use CONFIG_BLUEPAD32_PLATFORM_CUSTOM (see ble/sdkconfig.h)"
#endif

// Collapsed-joystick deadzone: analog axes are normalized 0..1 with 0.5 at
// centre, so a band either side of centre reads as "off" for the derived
// Atari-style 8-way directions.
#define CONTROLLER_DEADZONE 0.30f

// One controller slot. The slot number is the Bluepad32 device index, so a
// pad keeps its slot (and its Xpad pad number) for as long as it stays
// connected, and a new pad takes the first free one.
typedef struct {
  bool connected;   // a controller is connected and ready
  bool connecting;  // a device connected but is not ready yet
  char deviceName[32];
  // Input fields of the latest decoded report. Only the gamepad-derived members
  // of controller_state_t are populated here; meta fields are filled in
  // controller_getPadState().
  controller_state_t input;
} controller_pad_t;

typedef struct {
  bool initialized;
  bool pairingRequested;
  char status[64];
  controller_pad_t pads[CONTROLLER_MAX_PADS];
} controller_runtime_t;

static controller_runtime_t gRuntime = {0};

// Bluepad32's device table is what hands out the slot numbers, so the two must
// agree: a bigger table would give us indices with nowhere to go.
_Static_assert(CONTROLLER_MAX_PADS == CONFIG_BLUEPAD32_MAX_DEVICES,
               "one controller slot per Bluepad32 device");

// Clear the input snapshot to its REST state, which is not all-zero: the
// stick axes are normalised with 0.5 at centre, so memsetting them to 0
// reads as full deflection up and left. That is not academic. With mouse
// mode on, the right stick at 0.0/0.0 makes mouseAxisDelta() return the
// maximum negative delta on both axes, and the m68k hook injects that every
// frame: the GEM cursor crawls into the top-left corner and fights the real
// mouse, until the first stick report replaces the zeros with centred
// values and it inexplicably starts working.
//
// Triggers stay at zero, which really is their rest value.
static void resetInputToRest(controller_pad_t *pad) {
  memset(&pad->input, 0, sizeof(pad->input));
  pad->input.lx = 0.5f;
  pad->input.ly = 0.5f;
  pad->input.rx = 0.5f;
  pad->input.ry = 0.5f;
}

// Empty a slot: not connected, no name, sticks at rest.
static void clearPad(controller_pad_t *pad) {
  pad->connected = false;
  pad->connecting = false;
  pad->deviceName[0] = '\0';
  resetInputToRest(pad);
}

// The slot a Bluepad32 device occupies, or -1 if its index is out of range,
// which the assertion above says cannot happen.
static int slotFor(const uni_hid_device_t *d) {
  int idx = (d != NULL) ? uni_hid_device_get_idx_for_instance(d) : -1;
  return ((idx >= 0) && (idx < CONTROLLER_MAX_PADS)) ? idx : -1;
}

static float normalize_axis(int32_t value) {
  // Bluepad32 axes are signed, range -AXIS_NORMALIZE_RANGE/2 .. +range/2-1
  // (i.e. -512..511). Map to 0..1 with 0.5 at centre.
  float normalized = ((float)value + (float)(AXIS_NORMALIZE_RANGE / 2)) /
                     (float)AXIS_NORMALIZE_RANGE;
  if (normalized < 0.0f) normalized = 0.0f;
  if (normalized > 1.0f) normalized = 1.0f;
  return normalized;
}

static float normalize_pedal(int32_t value) {
  // Brake / throttle are 0..1023.
  float normalized = (float)value / 1023.0f;
  if (normalized < 0.0f) normalized = 0.0f;
  if (normalized > 1.0f) normalized = 1.0f;
  return normalized;
}

// Map a Bluepad32 virtual gamepad onto the input fields of controller_state_t.
static void map_gamepad(const uni_gamepad_t *gp, controller_state_t *state) {
  state->lx = normalize_axis(gp->axis_x);
  state->ly = normalize_axis(gp->axis_y);
  state->rx = normalize_axis(gp->axis_rx);
  state->ry = normalize_axis(gp->axis_ry);
  state->lt = normalize_pedal(gp->brake);
  state->rt = normalize_pedal(gp->throttle);

  // Bluepad32 reports a D-pad bitmask, not a HID hat value.
  state->hat = 0xFFu;
  state->dpadUp = (gp->dpad & DPAD_UP) != 0;
  state->dpadDown = (gp->dpad & DPAD_DOWN) != 0;
  state->dpadLeft = (gp->dpad & DPAD_LEFT) != 0;
  state->dpadRight = (gp->dpad & DPAD_RIGHT) != 0;

  state->btnA = (gp->buttons & BUTTON_A) != 0;
  state->btnB = (gp->buttons & BUTTON_B) != 0;
  state->btnX = (gp->buttons & BUTTON_X) != 0;
  state->btnY = (gp->buttons & BUTTON_Y) != 0;
  state->btnLB = (gp->buttons & BUTTON_SHOULDER_L) != 0;
  state->btnRB = (gp->buttons & BUTTON_SHOULDER_R) != 0;
  state->btnLS = (gp->buttons & BUTTON_THUMB_L) != 0;
  state->btnRS = (gp->buttons & BUTTON_THUMB_R) != 0;
  state->btnView = (gp->misc_buttons & MISC_BUTTON_SELECT) != 0;
  state->btnMenu = (gp->misc_buttons & MISC_BUTTON_START) != 0;
  state->btnGuide = (gp->misc_buttons & MISC_BUTTON_SYSTEM) != 0;

  // Digitize each analog stick into 8-way directions past the deadzone, kept
  // separate so the UI can show a joystick pair (left stick + D-pad) and a
  // mouse pair (right stick) independently.
  bool lUp = state->ly < (0.5f - CONTROLLER_DEADZONE);
  bool lDown = state->ly > (0.5f + CONTROLLER_DEADZONE);
  bool lLeft = state->lx < (0.5f - CONTROLLER_DEADZONE);
  bool lRight = state->lx > (0.5f + CONTROLLER_DEADZONE);
  state->rstickUp = state->ry < (0.5f - CONTROLLER_DEADZONE);
  state->rstickDown = state->ry > (0.5f + CONTROLLER_DEADZONE);
  state->rstickLeft = state->rx < (0.5f - CONTROLLER_DEADZONE);
  state->rstickRight = state->rx > (0.5f + CONTROLLER_DEADZONE);

  // Left stick + D-pad: shown as the joystick pair.
  state->padUp = state->dpadUp || lUp;
  state->padDown = state->dpadDown || lDown;
  state->padLeft = state->dpadLeft || lLeft;
  state->padRight = state->dpadRight || lRight;

  // Union of everything: the classic Atari ST 8-way joystick that is actually
  // injected. Unchanged from before the split, so hardware behaviour is identical.
  state->anyUp = state->padUp || state->rstickUp;
  state->anyDown = state->padDown || state->rstickDown;
  state->anyLeft = state->padLeft || state->rstickLeft;
  state->anyRight = state->padRight || state->rstickRight;

  // Single fire button: any face or shoulder button, or a pulled trigger.
  state->anyButton = state->btnA || state->btnB || state->btnX || state->btnY ||
                     state->btnLB || state->btnRB || state->btnLS ||
                     state->btnRS ||
                     state->lt > CONTROLLER_TRIGGER_THRESHOLD ||
                     state->rt > CONTROLLER_TRIGGER_THRESHOLD;
}

//
// Bluepad32 platform callbacks
//

static void sidepad_platform_init(int argc, const char **argv) {
  ARG_UNUSED(argc);
  ARG_UNUSED(argv);
  DPRINTF("bluepad32: platform init\n");
}

static void sidepad_platform_on_init_complete(void) {
  DPRINTF("bluepad32: init complete, scanning\n");
  // We are on the BT thread here, so use the "unsafe" variants. Do NOT delete
  // stored keys: a previously paired controller should reconnect automatically.
  uni_bt_start_scanning_and_autoconnect_unsafe();
}

static uni_error_t sidepad_platform_on_device_discovered(bd_addr_t addr,
                                                      const char *name,
                                                      uint16_t cod,
                                                      uint8_t rssi) {
  ARG_UNUSED(addr);
  ARG_UNUSED(name);
  ARG_UNUSED(rssi);
  // Ignore keyboards; we only want game controllers / joysticks.
  if (((cod & UNI_BT_COD_MINOR_MASK) & UNI_BT_COD_MINOR_KEYBOARD) ==
      UNI_BT_COD_MINOR_KEYBOARD) {
    return UNI_ERROR_IGNORE_DEVICE;
  }
  return UNI_ERROR_SUCCESS;
}

// Bluepad32 hands us an unresolved/garbage name buffer for some controllers
// (non-zero, non-printable bytes that differ per connection). Accept a name only
// if it is entirely printable ASCII; otherwise leave dst empty so callers fall
// back to a generic label rather than pushing binary garbage to the ST screen.
static void captureControllerName(char *dst, size_t dstSize, const char *src) {
  if (dstSize == 0) {
    return;
  }
  dst[0] = '\0';
  if (src == NULL) {
    return;
  }
  size_t n = 0;
  while ((src[n] != '\0') && (n < dstSize - 1)) {
    unsigned char c = (unsigned char)src[n];
    if ((c < 0x20) || (c > 0x7E)) {
      return;  // non-printable byte -> reject the whole name
    }
    n++;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void sidepad_platform_on_device_connected(uni_hid_device_t *d) {
  int idx = slotFor(d);
  if (idx < 0) {
    return;
  }
  uint32_t irqState = save_and_disable_interrupts();
  gRuntime.pads[idx].connecting = true;
  gRuntime.pads[idx].connected = false;
  restore_interrupts(irqState);
  DPRINTF("bluepad32: device connected %p (slot %d)\n", (void *)d, idx);
}

static void sidepad_platform_on_device_disconnected(uni_hid_device_t *d) {
  int idx = slotFor(d);
  if (idx < 0) {
    return;
  }
  uint32_t irqState = save_and_disable_interrupts();
  clearPad(&gRuntime.pads[idx]);
  restore_interrupts(irqState);
  // Whatever it was asked to rumble at goes with it: a pad that later takes
  // this slot starts quiet, until the ST asks again.
  rumble_padGone(idx);
  DPRINTF("bluepad32: device disconnected %p (slot %d)\n", (void *)d, idx);
}

static uni_error_t sidepad_platform_on_device_ready(uni_hid_device_t *d) {
  int idx = slotFor(d);
  if (idx < 0) {
    return UNI_ERROR_NO_SLOTS;
  }
  uint32_t irqState = save_and_disable_interrupts();
  gRuntime.pads[idx].connected = true;
  gRuntime.pads[idx].connecting = false;
  captureControllerName(gRuntime.pads[idx].deviceName,
                        sizeof(gRuntime.pads[idx].deviceName), d->name);
  restore_interrupts(irqState);
  // Light the player number, on pads that have player LEDs, so it is plain
  // which pad is player 1 and therefore the joystick.
  if (d->report_parser.set_player_leds != NULL) {
    d->report_parser.set_player_leds(d, (uint8_t)(1u << idx));
  }
  DPRINTF("bluepad32: device ready %p (slot %d, %s)\n", (void *)d, idx,
          d->name);
  return UNI_ERROR_SUCCESS;
}

static void sidepad_platform_on_controller_data(uni_hid_device_t *d,
                                             uni_controller_t *ctl) {
  if ((ctl == NULL) || (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)) {
    return;
  }
  int idx = slotFor(d);
  if (idx < 0) {
    return;
  }

  controller_state_t mapped = {0};
  map_gamepad(&ctl->gamepad, &mapped);

  controller_pad_t *pad = &gRuntime.pads[idx];
  uint32_t irqState = save_and_disable_interrupts();
  pad->input = mapped;
  // The remote BT name can resolve after on_device_ready, so capture it here
  // (once) as soon as a valid printable name is available — input only flows
  // from a ready device. captureControllerName rejects garbage buffers.
  if (pad->deviceName[0] == '\0') {
    captureControllerName(pad->deviceName, sizeof(pad->deviceName), d->name);
  }
  restore_interrupts(irqState);
}

static const uni_property_t *sidepad_platform_get_property(
    uni_property_idx_t idx) {
  ARG_UNUSED(idx);
  return NULL;
}

static void sidepad_platform_on_oob_event(uni_platform_oob_event_t event,
                                       void *data) {
  ARG_UNUSED(data);
  DPRINTF("bluepad32: oob event 0x%04x\n", event);
}

static struct uni_platform *get_sidepad_platform(void) {
  static struct uni_platform plat = {
      .name = "Sidepad",
      .init = sidepad_platform_init,
      .on_init_complete = sidepad_platform_on_init_complete,
      .on_device_discovered = sidepad_platform_on_device_discovered,
      .on_device_connected = sidepad_platform_on_device_connected,
      .on_device_disconnected = sidepad_platform_on_device_disconnected,
      .on_device_ready = sidepad_platform_on_device_ready,
      .on_oob_event = sidepad_platform_on_oob_event,
      .on_controller_data = sidepad_platform_on_controller_data,
      .get_property = sidepad_platform_get_property,
  };
  return &plat;
}

//
// Public API
//

void controller_setStatus(const char *message) {
  if (message == NULL) {
    return;
  }
  snprintf(gRuntime.status, sizeof(gRuntime.status), "%s", message);
}

int controller_init(void) {
  if (gRuntime.initialized) {
    return 0;
  }

  // Before anything can read the state: gRuntime is a zero-initialised
  // static, and zero is not rest for the stick axes.
  for (int i = 0; i < CONTROLLER_MAX_PADS; i++) {
    clearPad(&gRuntime.pads[i]);
  }

  // Bring up the CYW43 chip. With CYW43_ENABLE_BLUETOOTH=1 this also powers the
  // Bluetooth controller, which Bluepad32 then drives via BTstack.
  int chipErr = network_initChipOnly();
  if (chipErr != 0) {
    snprintf(gRuntime.status, sizeof(gRuntime.status), "BLE init failed");
    DPRINTF("controller_init: network_initChipOnly failed: %d\n", chipErr);
    return -1;
  }

  // Must be called before uni_init().
  uni_platform_set_custom(get_sidepad_platform());
  uni_init(0, NULL);

  gRuntime.initialized = true;
  gRuntime.pairingRequested = false;
  snprintf(gRuntime.status, sizeof(gRuntime.status), "Scanning for controller...");

  return 0;
}

void controller_poll(void) {
  if (!gRuntime.initialized) {
    return;
  }

  // Drives the cyw43/BTstack poll backend, which in turn fires the Bluepad32
  // callbacks above.
  network_safePoll();

  int connected = 0;
  bool connecting = false;
  bool pairing;
  uint32_t irqState = save_and_disable_interrupts();
  for (int i = 0; i < CONTROLLER_MAX_PADS; i++) {
    if (gRuntime.pads[i].connected) connected++;
    if (gRuntime.pads[i].connecting) connecting = true;
  }
  pairing = gRuntime.pairingRequested;
  if (pairing && (connected > 0)) {
    gRuntime.pairingRequested = false;
    pairing = false;
  }
  restore_interrupts(irqState);

  if (connected == 1) {
    snprintf(gRuntime.status, sizeof(gRuntime.status), "Controller connected");
  } else if (connected > 1) {
    snprintf(gRuntime.status, sizeof(gRuntime.status),
             "%d controllers connected", connected);
  } else if (connecting) {
    snprintf(gRuntime.status, sizeof(gRuntime.status),
             "Connecting to controller...");
  } else if (pairing) {
    snprintf(gRuntime.status, sizeof(gRuntime.status),
             "Pairing cleared, scanning...");
  } else {
    snprintf(gRuntime.status, sizeof(gRuntime.status),
             "Scanning for controller...");
  }
}

void controller_requestPairing(void) {
  if (!gRuntime.initialized) {
    return;
  }

  // Called from the app loop (not the BT thread), so use the "safe" variants.
  // Drop every live controller first: uni_bt_del_keys_safe() only clears
  // stored bonding keys, it does NOT tear down an active link. Without the
  // explicit disconnect a previously-paired controller stays connected (and
  // re-bonds). Disconnecting frees its slot and, with its key now gone, it
  // won't auto-reconnect - only a controller actively in pairing mode will be
  // picked up by the scan.
  for (int i = 0; i < CONTROLLER_MAX_PADS; i++) {
    uint32_t irqState = save_and_disable_interrupts();
    bool live = gRuntime.pads[i].connected || gRuntime.pads[i].connecting;
    restore_interrupts(irqState);
    if (live) {
      uni_bt_disconnect_device_safe(i);
    }
  }

  // Forget stored keys and (re)start scanning so a fresh controller can pair.
  uni_bt_del_keys_safe();
  uni_bt_start_scanning_and_autoconnect_safe();

  uint32_t irqState = save_and_disable_interrupts();
  gRuntime.pairingRequested = true;
  for (int i = 0; i < CONTROLLER_MAX_PADS; i++) {
    clearPad(&gRuntime.pads[i]);
  }
  restore_interrupts(irqState);

  snprintf(gRuntime.status, sizeof(gRuntime.status),
           "Pairing cleared, scanning...");
}

void controller_getState(controller_state_t *outState) {
  controller_getPadState(0, outState);
}

void controller_getPadState(int idx, controller_state_t *outState) {
  if ((outState == NULL) || (idx < 0) || (idx >= CONTROLLER_MAX_PADS)) {
    return;
  }

  const controller_pad_t *pad = &gRuntime.pads[idx];
  controller_state_t inputSnapshot;
  bool connected;
  char deviceName[sizeof(pad->deviceName)];

  uint32_t irqState = save_and_disable_interrupts();
  inputSnapshot = pad->input;
  connected = pad->connected;
  memcpy(deviceName, pad->deviceName, sizeof(deviceName));
  restore_interrupts(irqState);

  // Start from the decoded input fields, then overlay meta fields.
  *outState = inputSnapshot;
  outState->initialized = gRuntime.initialized;
  outState->connected = connected;
  // Bluepad32 only delivers input from a connected, ready controller, so
  // "paired" is reported as "currently connected".
  outState->paired = connected;
  outState->pairingRequested = gRuntime.pairingRequested;
  outState->pairedAddress[0] = '\0';
  outState->pairedAddressType[0] = '\0';
  snprintf(outState->deviceName, sizeof(outState->deviceName), "%s", deviceName);
  snprintf(outState->status, sizeof(outState->status), "%s", gRuntime.status);
}

bool controller_playRumble(int idx, uint8_t weak, uint8_t strong,
                           uint16_t durationMs) {
  if (!gRuntime.initialized || (idx < 0) || (idx >= CONTROLLER_MAX_PADS) ||
      !gRuntime.pads[idx].connected) {
    return false;
  }
  // Safe from the app loop: with the lwIP poll architecture BTstack runs
  // inside cyw43_arch_poll() on this same thread, so nothing else is in
  // Bluepad32 while this sends the report.
  uni_hid_device_t *d = uni_hid_device_get_instance_for_idx(idx);
  if ((d == NULL) || (d->report_parser.play_dual_rumble == NULL)) {
    return false;
  }
  d->report_parser.play_dual_rumble(d, 0, durationMs, weak, strong);
  return true;
}

bool controller_getAndClearRestartRequested(void) { return false; }
