/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Bluepad32 build-time configuration for the Pico W target.
//
// Bluepad32 normally gets these from ESP-IDF's "menuconfig" (sdkconfig.h). On
// the Pico W they are provided here and must be on the include path of both
// libbluepad32 and the app (see rp/src/CMakeLists.txt).

#ifndef SIDEPAD_SDKCONFIG_H
#define SIDEPAD_SDKCONFIG_H

// Up to four controllers at once, one per Xpad pad slot. A controller's slot is
// its Bluepad32 device index: the first free one when it connects, kept for as
// long as it stays connected. Only slot 0 is injected as joystick 1 and the
// mouse, so a second controller can never co-drive them; slots 1-3 reach games
// through Xpad alone. Must equal CONTROLLER_MAX_PADS (asserted in controller.c).
#define CONFIG_BLUEPAD32_MAX_DEVICES 4
#define CONFIG_BLUEPAD32_MAX_ALLOWLIST 4
#define CONFIG_BLUEPAD32_GAP_SECURITY 1
#define CONFIG_BLUEPAD32_ENABLE_BLE_BY_DEFAULT 1

// Sidepad provides its own uni_platform (see controller.c), so use the custom
// platform rather than one of the built-in ESP32 Unijoysticle platforms.
#define CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#define CONFIG_TARGET_PICO_W

// 2 == Info
#define CONFIG_BLUEPAD32_LOG_LEVEL 2

#endif  // SIDEPAD_SDKCONFIG_H
