/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// inclusw in the C file to avoid multiple definitions
#include "aconfig.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "controller.h"
#include "debug.h"
#include "display.h"
#include "ff.h"
#include "gconfig.h"
#include "include/xpadstate.h"
#include "memfunc.h"
#include "network.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "rumble.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary
#include "term.h"

#define SLEEP_LOOP_MS 50
#define UI_REFRESH_MS 100

// Shared-variable slot (0-59, all app-free) carrying the digital BT joystick
// state to the m68k userfw hooks. The LSB lands at m68k $FA201F.
#define SIDEPAD_BT_JOY_SLOT 3

// Shared-variable slot feeding the m68k boot_gem exit banner. The banner text
// and the VT52 clear live in the cartridge ROM (main.s); the RP only supplies
// the connected flag here (LSB at m68k $FA2023), which boot_gem uses to pick
// between the connected / not-connected banner. The joystick slot (3) never
// overlaps this.
#define SIDEPAD_EXIT_FLAG_SLOT 4

// Shared-variable slot carrying the BT mouse packet to the m68k userfw hook
// when mouse mode is on (right stick -> GEM cursor). Packed big-endian, so the
// m68k reads (at $FA2024): byte0 = enabled, byte1 = buttons (bit1 = left / R3),
// byte2 = signed dx, byte3 = signed dy (per-frame deltas). The resident
// hook reads all four in .di_mouse and drives mousevec with them.
#define SIDEPAD_BT_MOUSE_SLOT 5

// Shared-variable slot carrying the VBL/ETV hook-mode flag to the m68k userfw
// installer, read once when it installs the hook (LSB at m68k $FA202B): 0 = VBL
// ($70), 1 = ETV ($400). Published before the CMD_START burst in
// exitToGemDesktop so it is latched when the installer runs.
#define SIDEPAD_HOOK_MODE_SLOT 6

// Shared-variable slot carrying the joystick-enabled flag to the m68k, so the
// exit banner can list what is actually on (LSB at m68k $FA202F). Banner only,
// unlike slot 5, which the resident hook reads to gate mouse injection: the
// joystick is gated on this side, by publishing an idle packet.
#define SIDEPAD_JOY_ENABLED_SLOT 7

// Shared-variable slot carrying the Xpad-enabled flag to the m68k (LSB at m68k
// $FA2033). Read by the userfw installer, which only puts the XPAD cookie in
// the jar when it is set, and by the exit banner. The block itself is gated on
// this side, by not publishing it at all.
#define SIDEPAD_XPAD_ENABLED_SLOT 8

// ROM3 commands from the resident m68k hook (userfw.s), for Xpad rumble.
// The ids sit well away from the terminal's 0 (ESC) and 1 (keystroke), and
// must match target/atarist/src/inc/sidepad_cmds.s. Both carry the usual
// 4-byte random token first, sent as zero, which chandler skips.
//
// APP_XPAD_REQ_ADDR: one long, the m68k address of the XPAD_REQ area the
//   installer set aside in its RAM block. Sent while it installs, and again
//   after every warm reset; it waits to see the address in the block.
// APP_XPAD_RUMBLE: four words, one per pad, the low frequency motor in the
//   high byte and the high frequency motor in the low byte. The whole state
//   every time, never a change: chandler holds one pending command, so a
//   frame can be lost, and the next one must be enough on its own.
#define APP_XPAD_REQ_ADDR 0x5850
#define APP_XPAD_RUMBLE 0x5851

// Right-stick-as-mouse tuning (analog, proportional). Deadzone around centre
// (0.5) below which the stick is idle, and the cursor's speed at full
// deflection in pixels per injection: 50 injections a second on ETV, and the
// display's rate on VBL. Both are feel parameters to tune on hardware. This
// was 20 while ETV wrongly injected at 12.5 Hz; 5 at 50 Hz keeps that top
// speed, 250 pixels a second.
#define SIDEPAD_MOUSE_DEADZONE 0.15f
#define SIDEPAD_MOUSE_MAX_SPEED 5.0f

enum {
  APP_MODE_SETUP = 255  // Setup
};

// Number of terminal rows used by the controller UI: all of them. (The last
// was once left for a status line the template's line-based prompt used;
// term.c draws nothing there, and MD/Sidepad has no prompt.) The bottom row
// is safe from scrolling because nothing here ever writes its last cell or
// prints a newline: every row is placed with ESC Y, appendLineAt stops one
// short of the last column, and the cursor is only parked in that empty cell.
// Writing into it, or a newline on the bottom row, would scroll the screen.
#define UI_ROW_COUNT TERM_SCREEN_SIZE_Y

// Throttles redraws of the live controller UI to UI_REFRESH_MS.
static absolute_time_t uiRefreshTime;

static void appendFmt(char *buffer, size_t bufferSize, size_t *offset,
                      const char *fmt, ...) {
  if ((buffer == NULL) || (offset == NULL) || (*offset >= bufferSize) ||
      (fmt == NULL)) {
    return;
  }

  va_list args;
  va_start(args, fmt);
  size_t remaining = bufferSize - *offset;
  int written = vsnprintf(buffer + *offset, remaining, fmt, args);
  va_end(args);

  if (written < 0) {
    return;
  }

  size_t writeSize = (size_t)written;
  if (writeSize >= remaining) {
    *offset = bufferSize - 1;
    return;
  }

  *offset += writeSize;
}

static void appendMoveAndClearLine(char *buffer, size_t bufferSize,
                                   size_t *offset, uint8_t row) {
  appendFmt(buffer, bufferSize, offset,
            "\x1B"
            "Y%c%c\x1B"
            "K",
            (char)(TERM_POS_Y + row), (char)(TERM_POS_X));
}

static void appendLineAt(char *buffer, size_t bufferSize, size_t *offset,
                         uint8_t row, const char *text) {
  // One cell short of the full width: the last column is never drawn, so
  // the cursor can be parked there out of the way.
  char line[TERM_SCREEN_SIZE_X + 1] = {0};
  snprintf(line, sizeof(line), "%-*.*s", TERM_SCREEN_SIZE_X - 1,
           TERM_SCREEN_SIZE_X - 1, (text != NULL) ? text : "");
  appendMoveAndClearLine(buffer, bufferSize, offset, row);
  appendFmt(buffer, bufferSize, offset, "%s", line);
}

// Everything that would sit against the left edge starts this far in, where
// the title's text does after its two rule cells. A mono monitor's border is
// always black, so text at column 0 would be right up against it.
#define LEFT_MARGIN 2

// Layout of the digital joystick indicator drawn with text characters.
// A direction box on the left holds a solid block that moves to one of nine
// positions (centre + 8 compass directions); a fire box on the right fills
// solid when any button is pressed.
//
// The terminal is 80 columns of medium resolution, where a character cell is
// twice as tall as it is wide, so everything here is twice as many cells wide
// as it is tall to come out square: the direction box is 18x9 inside, its
// blob 6x3, the fire box 6x3.
#define DIR_BOX_INNER_W 18  // inner playfield width in chars
#define DIR_BOX_INNER_H 9   // inner playfield height in rows
#define DIR_BLOCK_W 6       // direction blob width in chars
#define DIR_BLOCK_H 3       // direction blob height in rows
#define DIR_BOX_ROW 5       // first screen row of the direction box
#define DIR_BOX_COL LEFT_MARGIN  // left screen column of the direction box
#define FIRE_BOX_INNER_W 6
#define FIRE_BOX_INNER_H 3
// Direction box spans cols DIR_BOX_COL..(DIR_BOX_COL + DIR_BOX_INNER_W + 1) =
// 2..21; the fire box starts two empty columns (22, 23) later, at 24, the
// one-column gap of 40-column days at the new width.
#define FIRE_BOX_COL (DIR_BOX_COL + DIR_BOX_INNER_W + 4)

// Mouse mode draws a second pair: an exact clone of the joystick pair shifted
// to the right half (direction box left, button box on its right), starting
// at SECOND_COLUMN so it lines up with the second column of controllers
// above it: direction box cols 42..61, button box cols 64..71. Titles sit one
// blank row below both boxes (box bottom is DIR_BOX_ROW + INNER_H + 1), at
// each pair's left edge.
#define MOUSE_DIR_BOX_COL SECOND_COLUMN
#define MOUSE_FIRE_BOX_COL (MOUSE_DIR_BOX_COL + FIRE_BOX_COL - DIR_BOX_COL)
#define VIS_TITLE_ROW (DIR_BOX_ROW + DIR_BOX_INNER_H + 3)

// The controllers, numbered from 1 as players are, in two columns starting
// two rows below the title: 1 and 2 down the left (rows 2-3), 3 and 4 down
// the right, then a blank row before the boxes at DIR_BOX_ROW. The auto-exit
// countdown sits two rows above the rule, clear of the visualiser titles.
#define PAD_LIST_ROW 2
#define PAD_LIST_ROWS (CONTROLLER_MAX_PADS / 2)
#define COUNTDOWN_ROW (UI_ROW_COUNT - 4)

// The second of two columns, for the controller list and the help page alike:
// as far past halfway as the first column is past the left edge. Entries in
// the first column must end at least two cells short of it.
#define SECOND_COLUMN (TERM_SCREEN_SIZE_X / 2 + LEFT_MARGIN)

// Where text set into a rule ends. appendLineAt renders TERM_SCREEN_SIZE_X - 1
// cells, and two rule cells are left after the text (----vX.Y.Z--). The
// version on the bottom rule and the controller count on the title rule both
// end here, so they line up.
#define RULE_TEXT_END ((size_t)(TERM_SCREEN_SIZE_X - 1) - 2)

// Right-stick-as-mouse toggle ([M] in the controller UI). Persisted to per-app
// config (ACONFIG_PARAM_MOUSE): loaded at startup and saved on exit to desktop,
// so the choice survives reboots.
static bool mouseMode = false;

// Joystick injection on/off, toggled with J and persisted like mouse mode.
// Off does not touch Xpad: a game reading the block is unaffected by whether
// we are also pretending to be a joystick.
static bool joystickMode = true;

// Xpad support on/off ([X] in the controller UI), persisted like the others.
// Off means the block is never published and the m68k installer leaves the
// XPAD cookie out of the jar, so an Xpad-aware program sees nothing at all;
// joystick and mouse injection are untouched either way.
static bool xpadMode = true;

// VBL/ETV hook-mode toggle ([H] in the controller UI). false = VBL ($70), true
// = ETV ($400). Defaults to ETV (ACONFIG_PARAM_HOOK default is "true"); this
// initial value is overwritten from per-app config at
// startup. Published to shared-var slot 6 (inline in exitToGemDesktop) on the
// way out to the desktop so the m68k userfw installer hooks the matching
// vector. ETV survives programs that replace the VBL vector; the m68k
// injects on every etv_timer call, which TOS makes at 50 Hz.
static bool etvMode = true;

// Help-screen toggle (dedicated ST Help key, scan code SCAN_HELP). When true
// the controller visualiser is replaced by a static help page; the title and
// footer rows stay so Help/Esc still work. Debounced so Help-key auto-repeat
// does not flicker the screen.
#define SCAN_HELP 0x62
static bool helpVisible = false;
static absolute_time_t helpLastToggle;
static bool helpToggleValid = false;

// Auto-exit countdown: once a controller connects, the terminal counts down for
// SIDEPAD_AUTOEXIT_SECONDS then exits to the desktop as if ESC was pressed. Any
// key cancels the running countdown for the rest of the session
// (autoExitCancelled is sticky, so a later disconnect/reconnect does not
// restart it). A disconnect alone just stops the current run; reconnecting
// restarts it unless it was key-cancelled.
#define SIDEPAD_AUTOEXIT_SECONDS 10
static bool autoExitActive = false;
static bool autoExitCancelled = false;
static absolute_time_t autoExitDeadline;

// Box-drawing and block glyphs from u8g2_font_amstrad_cpc_extended_8f. Codes
// confirmed by decoding the font bitmaps (see GLYPH debug screen):
//   128 vertical, 129 horizontal, 131/132/133/130 = TL/TR/BR/BL corners,
//   157 = solid filled square.
#define GLYPH_V ((char)128)          // vertical line
#define GLYPH_H ((char)129)          // horizontal line
#define GLYPH_CORNER_TL ((char)131)  // top-left
#define GLYPH_CORNER_TR ((char)132)  // top-right
#define GLYPH_CORNER_BR ((char)133)  // bottom-right
#define GLYPH_CORNER_BL ((char)130)  // bottom-left
#define BLOCK_CHAR ((char)157)       // solid filled square

// Draws a bordered box into lines at the given top-left cell using the
// line-drawing glyphs. The inner area is filled by fillChar; pass '\0' to leave
// it blank. The block, when blockRow/blockCol are >= 0, overwrites a
// blockW x blockH patch of inner cells with the solid glyph.
static void drawBox(char lines[][TERM_SCREEN_SIZE_X + 1], uint8_t topRow,
                    uint8_t leftCol, uint8_t innerW, uint8_t innerH,
                    char fillChar, int blockRow, int blockCol, uint8_t blockW,
                    uint8_t blockH) {
  uint8_t bottomRow = topRow + innerH + 1;
  uint8_t rightCol = leftCol + innerW + 1;

  // Corners.
  lines[topRow][leftCol] = GLYPH_CORNER_TL;
  lines[topRow][rightCol] = GLYPH_CORNER_TR;
  lines[bottomRow][leftCol] = GLYPH_CORNER_BL;
  lines[bottomRow][rightCol] = GLYPH_CORNER_BR;

  // Top and bottom edges.
  for (uint8_t x = 0; x < innerW; x++) {
    lines[topRow][leftCol + 1 + x] = GLYPH_H;
    lines[bottomRow][leftCol + 1 + x] = GLYPH_H;
  }

  // Sides and interior.
  for (uint8_t y = 0; y < innerH; y++) {
    uint8_t row = topRow + 1 + y;
    lines[row][leftCol] = GLYPH_V;
    lines[row][rightCol] = GLYPH_V;
    for (uint8_t x = 0; x < innerW; x++) {
      lines[row][leftCol + 1 + x] = (fillChar != '\0') ? fillChar : ' ';
    }
  }

  if ((blockRow >= 0) && (blockCol >= 0)) {
    for (uint8_t by = 0; by < blockH; by++) {
      for (uint8_t bx = 0; bx < blockW; bx++) {
        lines[topRow + 1 + blockRow + by][leftCol + 1 + blockCol + bx] =
            BLOCK_CHAR;
      }
    }
  }
}

// Draws one direction+button visualiser pair: a direction box (with the blob
// pushed to the active 8-way position) at dirCol and a button box at fireCol.
static void drawJoyPair(char lines[][TERM_SCREEN_SIZE_X + 1], uint8_t dirCol,
                        uint8_t fireCol, bool up, bool down, bool left,
                        bool right, bool fire) {
  int blockRow = (DIR_BOX_INNER_H - DIR_BLOCK_H) / 2;
  int blockCol = (DIR_BOX_INNER_W - DIR_BLOCK_W) / 2;
  if (up) blockRow = 0;
  if (down) blockRow = DIR_BOX_INNER_H - DIR_BLOCK_H;
  if (left) blockCol = 0;
  if (right) blockCol = DIR_BOX_INNER_W - DIR_BLOCK_W;

  drawBox(lines, DIR_BOX_ROW, dirCol, DIR_BOX_INNER_W, DIR_BOX_INNER_H, '\0',
          blockRow, blockCol, DIR_BLOCK_W, DIR_BLOCK_H);
  drawBox(lines, DIR_BOX_ROW, fireCol, FIRE_BOX_INNER_W, FIRE_BOX_INNER_H,
          fire ? BLOCK_CHAR : '\0', -1, -1, 1, 1);
}

// Escape-sequence scratch for one screen refresh, sized once so the helper
// below and the buffer in renderControllerScreen cannot disagree. A full
// refresh of 80 columns is over 2 KB.
#define UPDATES_SIZE 4096

// One visualiser title: "Joystick: on" with the toggle letter in reverse
// video. Emitted as escape sequences rather than into the character grid,
// because ESC p/q are not fixed-width cells and the grid diff counts cells.
//
// "on " keeps the trailing space deliberately: this row is drawn over the
// grid rather than through it, so it gets no clear-to-width and a shorter
// value would leave the "f" of a previous "off" behind.
static void emitToggleTitle(char *updates, size_t *offset, uint8_t dirCol,
                            char key, const char *rest, bool on) {
  appendFmt(updates, UPDATES_SIZE, offset,
            "\x1B"
            "Y%c%c"
            "\x1B"
            "p"
            "%c"
            "\x1B"
            "q"
            "%s: %s",
            (char)(TERM_POS_Y + VIS_TITLE_ROW), (char)(TERM_POS_X + dirCol),
            key, rest, on ? "on " : "off");
}

static void renderControllerScreen(const controller_state_t *state,
                                   bool forceFullRefresh) {
  // All three static: at 80 columns they come to about 6 KB, more than the
  // core 0 stack can spare, and a refresh never runs inside another one.
  // Every row of currentLines is rewritten below, and updates is
  // restarted, so nothing carries over between calls.
  static char previousLines[UI_ROW_COUNT][TERM_SCREEN_SIZE_X + 1] = {{0}};
  static char currentLines[UI_ROW_COUNT][TERM_SCREEN_SIZE_X + 1];
  static char updates[UPDATES_SIZE];

  if (state == NULL) {
    return;
  }

  // Pad every row to full width so stale characters are always overwritten.
  for (uint8_t row = 0; row < UI_ROW_COUNT; row++) {
    memset(currentLines[row], ' ', TERM_SCREEN_SIZE_X);
    currentLines[row][TERM_SCREEN_SIZE_X] = '\0';
  }

  // Title row: two leading rule cells, "MD/Sidepad", then a rule to the edge
  // (--MD/Sidepad------). The rule cells are the left margin.
  const char *title = "MD/Sidepad";
  size_t titleLen = strlen(title);
  const size_t titleLead = LEFT_MARGIN;
  memset(currentLines[0], GLYPH_H, titleLead);
  memcpy(currentLines[0] + titleLead, title, titleLen);
  memset(currentLines[0] + titleLead + titleLen, GLYPH_H,
         TERM_SCREEN_SIZE_X - titleLead - titleLen);

  // The connected controllers, one place per slot: "1 Xbox Wireless
  // Controller". Pad 1 is the joystick and mouse; every pad is in Xpad. An
  // empty slot is left blank, so a pad's place never moves. With nothing
  // connected at all, the first row carries the status message instead
  // (scanning, pairing, start-up errors).
  int connectedCount = 0;
  for (int i = 0; i < CONTROLLER_MAX_PADS; i++) {
    controller_state_t pad = {0};
    controller_getPadState(i, &pad);
    if (!pad.connected) {
      continue;
    }
    connectedCount++;
    char line[TERM_SCREEN_SIZE_X + 1];
    int n = snprintf(line, sizeof(line), "%d %s", i + 1,
                     (pad.deviceName[0] != '\0') ? pad.deviceName
                                                  : "Controller");
    size_t lineLen = (n < 0) ? 0 : (size_t)n;
    // Down the left column, then the right; each kept inside its column.
    size_t col = (i < PAD_LIST_ROWS) ? LEFT_MARGIN : SECOND_COLUMN;
    size_t room = ((i < PAD_LIST_ROWS) ? SECOND_COLUMN - 1
                                       : TERM_SCREEN_SIZE_X - 1) - col;
    if (lineLen > room) lineLen = room;
    memcpy(currentLines[PAD_LIST_ROW + (i % PAD_LIST_ROWS)] + col, line,
           lineLen);
  }
  if (connectedCount == 0) {
    const char *status = (state->status[0] != '\0') ? state->status : "N/A";
    size_t statusLen = strlen(status);
    if (statusLen > TERM_SCREEN_SIZE_X - 1 - LEFT_MARGIN) {
      statusLen = TERM_SCREEN_SIZE_X - 1 - LEFT_MARGIN;
    }
    memcpy(currentLines[PAD_LIST_ROW] + LEFT_MARGIN, status, statusLen);
  }

  // How many are connected, set into the top right of the title rule
  // (--MD/Sidepad-----Connected:2--), ending where the version does on the
  // bottom rule. Part of the grid, so the diff redraws it when it changes.
  {
    char count[16];
    int n = snprintf(count, sizeof(count), "Connected:%d", connectedCount);
    size_t countLen = (n < 0) ? 0 : (size_t)n;
    if (countLen < RULE_TEXT_END - titleLead - titleLen) {
      memcpy(currentLines[0] + RULE_TEXT_END - countLen, count, countLen);
    }
  }

  // Auto-exit countdown message, below the visualiser titles. Shown only
  // while the countdown is running; when it is not, the row stays blank and
  // the line diff clears any previous message.
  if (autoExitActive) {
    int64_t remUs =
        absolute_time_diff_us(get_absolute_time(), autoExitDeadline);
    int secs = (int)((remUs + 999999) / 1000000);  // ceil to whole seconds
    if (secs < 1) secs = 1;  // never display 0; loop exits
    char countdown[TERM_SCREEN_SIZE_X + 1];
    int n = snprintf(countdown, sizeof(countdown),
                     "Exit in %ds (any key to cancel)", secs);
    size_t countdownLen = (n < 0) ? 0 : (size_t)n;
    if (countdownLen > TERM_SCREEN_SIZE_X - 1 - LEFT_MARGIN) {
      countdownLen = TERM_SCREEN_SIZE_X - 1 - LEFT_MARGIN;
    }
    memcpy(currentLines[COUNTDOWN_ROW] + LEFT_MARGIN, countdown,
           countdownLen);
  }

  // Visualiser. Both titles are always on screen and carry their own state,
  // so nothing moves when you toggle; only an enabled half draws boxes, and
  // an empty column reads as off rather than as a fault.
  //
  // With mouse mode on, the joystick half narrows to the left stick and D-pad
  // and drops R3 from its fire condition, because R3 is then the mouse button.
  // That mirrors what writeBtJoyState() actually injects.
  //
  // Titles are emitted after the grid diff (see below) because their toggle
  // letters are in reverse video, which the plain-char grid cannot carry.
  {
    bool joyFire = state->btnA || state->btnB || state->btnX || state->btnY ||
                   state->btnLB || state->btnRB || state->btnLS ||
                   state->lt > CONTROLLER_TRIGGER_THRESHOLD ||
                   state->rt > CONTROLLER_TRIGGER_THRESHOLD;
    // With mouse mode off the right stick is not a cursor, so the joystick
    // half keeps using any* and reacts to both sticks as it always has.
    bool jUp = mouseMode ? state->padUp : state->anyUp;
    bool jDown = mouseMode ? state->padDown : state->anyDown;
    bool jLeft = mouseMode ? state->padLeft : state->anyLeft;
    bool jRight = mouseMode ? state->padRight : state->anyRight;
    bool jFire = mouseMode ? joyFire : state->anyButton;

    // Only the enabled half gets boxes. The titles stay put either way and
    // carry the state, so nothing moves when you toggle: an empty column is
    // "off", not a rendering fault.
    if (joystickMode) {
      drawJoyPair(currentLines, DIR_BOX_COL, FIRE_BOX_COL, jUp, jDown, jLeft,
                  jRight, jFire);
    }
    if (mouseMode) {
      drawJoyPair(currentLines, MOUSE_DIR_BOX_COL, MOUSE_FIRE_BOX_COL,
                  state->rstickUp, state->rstickDown, state->rstickLeft,
                  state->rstickRight, state->btnRS);
    }
  }

  // Help page: when the Help key is toggled on, replace the controller content
  // area (everything between the title row and the bottom rule) with the static
  // help text. Drawn last so it overwrites the status/visualiser written above;
  // the shared title and footer rows are untouched. Content area is rows
  // 1..UI_ROW_COUNT-3 (21 rows), 80 columns. A row has a left half and an
  // optional right half starting at SECOND_COLUMN, which is how the keys sit
  // in two columns, read down the left one and then the right; keep the left
  // half short of SECOND_COLUMN. The keys are in the footer's order (Help,
  // Esc, Booster, Reset, Hook, Xpad), then the two the visualiser titles show.
  if (helpVisible) {
    static const struct {
      const char *left;
      const char *right;
    } helpLines[] = {
        {"", NULL},
        {"Pair up to 4 Bluetooth gamepads. Pad 1 is your ST joystick", NULL},
        {"and/or mouse, and all 4 work in games that support Xpad", NULL},
        {"", NULL},
        {"Help Show or hide this page",
         "H    Toggle joystick hook (see below)"},
        {"ESC  Exit to desktop and play!", "X    Xpad on/off"},
        {"B    Return to Booster", "J    Joystick on/off"},
        {"R    Reset: forget all pads", "M    Right stick as mouse on/off"},
        {"", NULL},
        {"", NULL},
        {"Choose between 2 joystick hooks:", NULL},
        {"", NULL},
        {"ETV  Works with most apps and games", NULL},
        {"VBL  Great for GEM and TOS apps", NULL},
    };
    // On the row the main screen's countdown uses, rather than counted down
    // from the top, so the two stay level whatever the list above holds.
    static const char helpWeb[] = "Web  neilrackett.com/atarist";
    for (uint8_t row = 1; row <= UI_ROW_COUNT - 3; row++) {
      memset(currentLines[row], ' ', TERM_SCREEN_SIZE_X);
    }
    for (size_t i = 0; i < sizeof(helpLines) / sizeof(helpLines[0]); i++) {
      if (1 + i > (size_t)(UI_ROW_COUNT - 3)) break;  // don't stomp the footer
      size_t len = strlen(helpLines[i].left);
      if (len > TERM_SCREEN_SIZE_X - 1 - LEFT_MARGIN) {
        len = TERM_SCREEN_SIZE_X - 1 - LEFT_MARGIN;
      }
      memcpy(currentLines[1 + i] + LEFT_MARGIN, helpLines[i].left, len);
      if (helpLines[i].right != NULL) {
        // The last column is never drawn, so the right half stops short of it.
        len = strlen(helpLines[i].right);
        if (len > TERM_SCREEN_SIZE_X - 1 - SECOND_COLUMN) {
          len = TERM_SCREEN_SIZE_X - 1 - SECOND_COLUMN;
        }
        memcpy(currentLines[1 + i] + SECOND_COLUMN, helpLines[i].right, len);
      }
    }
    memcpy(currentLines[COUNTDOWN_ROW] + LEFT_MARGIN, helpWeb,
           sizeof(helpWeb) - 1);
  }

  // Bottom two rows: a horizontal rule then the controls. Each
  // currentLines[row] is one independent screen line, so they go in separate
  // rows (not one string with \n). Use the box-drawing horizontal glyph, which
  // sits mid-cell, rather than '_' (cell bottom) so the rule clears the
  // controls text below it.
  memset(currentLines[UI_ROW_COUNT - 2], GLYPH_H, TERM_SCREEN_SIZE_X);
  // Overlay the build version near the right end of the rule (----vX.Y.Z--),
  // ending at RULE_TEXT_END like the controller count on the title rule.
  const char *version = RELEASE_VERSION;
  size_t versionLen = strlen(version);
  if (versionLen < RULE_TEXT_END) {
    memcpy(currentLines[UI_ROW_COUNT - 2] + RULE_TEXT_END - versionLen,
           version, versionLen);
  }
  // The controls footer (bottom row) is emitted separately below with the key
  // names in reverse video, so it is not part of this plain-char grid (the
  // ESC p/q codes are not fixed-width cells).

  size_t offset = 0;
  updates[0] = '\0';

  if (forceFullRefresh) {
    appendFmt(updates, sizeof(updates), &offset,
              "\x1B"
              "E");
  }

  for (uint8_t row = 0; row < UI_ROW_COUNT; row++) {
    if (forceFullRefresh ||
        (strcmp(currentLines[row], previousLines[row]) != 0)) {
      appendLineAt(updates, sizeof(updates), &offset, row, currentLines[row]);
      snprintf(previousLines[row], sizeof(previousLines[row]), "%s",
               currentLines[row]);
    }
  }

  // Visualiser titles, with the toggle letter in reverse video: "Joystick"
  // and "Mouse (RS)" with J and M highlighted, so the keys that turn each
  // half on and off are visible without consulting the footer.
  //
  // Emitted here rather than through the grid for the same reason as the
  // footer: ESC p/q are not fixed-width cells, so the plain-char diff cannot
  // hold them. The grid leaves this row blank and clears it on a full
  // refresh, and we draw over it immediately after. Suppressed while the help
  // page is up, since that overwrites the whole content area.
  if (forceFullRefresh && !helpVisible) {
    emitToggleTitle(updates, &offset, DIR_BOX_COL, 'J', "oystick",
                    joystickMode);
    emitToggleTitle(updates, &offset, MOUSE_DIR_BOX_COL, 'M', "ouse (RS)",
                    mouseMode);
  }

  // Controls footer with reverse-video key names (VT52 ESC p = on, ESC q =
  // off). Emitted outside the plain-char grid because the escape codes are not
  // fixed-width cells. Only on a full refresh: the row is static, so it
  // persists across partial refreshes (the grid never repaints this row).
  if (forceFullRefresh) {
    appendMoveAndClearLine(updates, sizeof(updates), &offset, UI_ROW_COUNT - 1);
    appendFmt(updates, sizeof(updates), &offset, "%*s", LEFT_MARGIN, "");
    appendFmt(updates, sizeof(updates), &offset,
              "\x1B"
              "p"
              "Help"
              "\x1B"
              "q"
              " "
              "\x1B"
              "p"
              "Esc"
              "\x1B"
              "q"
              " "
              "\x1B"
              "p"
              "B"
              "\x1B"
              "q"
              "ooster "
              "\x1B"
              "p"
              "R"
              "\x1B"
              "q"
              "eset "
              "\x1B"
              "p"
              "H"
              "\x1B"
              "q"
              "ook:%s "
              "\x1B"
              "p"
              "X"
              "\x1B"
              "q"
              "pad:%s",
              etvMode ? "ETV" : "VBL", xpadMode ? "on" : "off");
  }

  // Park the cursor block in the empty bottom-right cell so it doesn't sit on
  // top of the footer text. (The terminal always draws a block cursor; this
  // keeps it out of the way without changing the shared terminal code.)
  appendFmt(updates, sizeof(updates), &offset,
            "\x1B"
            "Y%c%c",
            (char)(TERM_POS_Y + (UI_ROW_COUNT - 1)),
            (char)(TERM_POS_X + (TERM_SCREEN_SIZE_X - 1)));
  term_printString(updates);
}

// Render the controller UI from the live controller.c snapshot.
static void refreshControllerUI(bool forceFullRefresh) {
  controller_state_t state = {0};
  controller_getState(&state);
  renderControllerScreen(&state, forceFullRefresh);
}

// Keep active loop or exit
static bool keepActive = true;

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// Set when the user chooses [ESC] Desktop: leave the terminal loop, install the
// m68k joystick hook, and continue booting to the GEM desktop instead of
// resetting / returning to Booster.
static bool exitToDesktop = false;

// Number of CMD_START frames to hold while the m68k installs the joystick hook
// on the way out to the desktop (the installer is idempotent).
#define SIDEPAD_HOOK_START_TICKS 10
// Number of CMD_BOOT_GEM frames to hold so the m68k reliably latches the boot
// handoff. A single sentinel write can race the m68k's vsync-paced
// check_commands poll and be dropped, leaving it stuck redrawing the terminal;
// repeating it guarantees boot_gem runs. Once boot_gem rts's, the m68k leaves
// the print loop and stops reading the sentinel, so the extra writes are inert.
#define SIDEPAD_BOOT_GEM_TICKS 10
// Joystick state refresh interval once a game is running (~50 Hz).
#define SIDEPAD_JOY_POLL_MS 20

// Pack the controller's 8-way + fire signals into the IKBD joystick byte the
// m68k VBL hook consumes and publish it to the shared-variable slot the hook
// polls. Direction bits use the standard IKBD encoding, confirmed on hardware
// via JOYMOUT: bit0=Up, bit1=Down, bit2=Left, bit3=Right. Fire is carried in
// bit6 here and moved to the IKBD fire bit (bit7) by userfw.s.
//
// In mouse mode the right stick drives the cursor, so the joystick uses left
// stick + D-pad only (pad*) and fire excludes the right-stick click (R3, now
// the mouse button). Mouse mode off = the union of both sticks + D-pad, as
// before.
static void writeBtJoyState(void) {
  controller_state_t btState = {0};
  controller_getState(&btState);
  uint8_t bt = 0;
  if (mouseMode) {
    if (btState.padUp) bt |= 0x01;
    if (btState.padDown) bt |= 0x02;
    if (btState.padLeft) bt |= 0x04;
    if (btState.padRight) bt |= 0x08;
    // Fire = any face/shoulder/left-thumb button or pulled trigger, but NOT R3
    // (the mouse click).
    if (btState.btnA || btState.btnB || btState.btnX || btState.btnY ||
        btState.btnLB || btState.btnRB || btState.btnLS ||
        btState.lt > CONTROLLER_TRIGGER_THRESHOLD ||
        btState.rt > CONTROLLER_TRIGGER_THRESHOLD) {
      bt |= 0x40;
    }
  } else {
    if (btState.anyUp) bt |= 0x01;
    if (btState.anyDown) bt |= 0x02;
    if (btState.anyLeft) bt |= 0x04;
    if (btState.anyRight) bt |= 0x08;
    if (btState.anyButton) bt |= 0x40;
  }
  // Joystick injection off: publish an idle packet rather than skipping the
  // write, so the m68k hook sees "nothing held" instead of the last state
  // before the toggle. Xpad is untouched by this; it has its own path.
  if (!joystickMode) bt = 0;

  uintptr_t romBase = (uintptr_t)&__rom_in_ram_start__;
  SET_SHARED_VAR(SIDEPAD_BT_JOY_SLOT, (uint32_t)bt, romBase,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
}

// Map a centred analog axis (0..1, 0.5 = rest) to a signed per-frame cursor
// delta: zero inside the deadzone, then a linear+quadratic blend up to
// SIDEPAD_MOUSE_MAX_SPEED at full deflection. The blend (0.5*t + 0.5*t^2)
// halves the slope near the deadzone edge -> half the minimum speed for finer
// slow control, while full deflection still reaches the same max.
//
// The IKBD moves in whole pixels, and at 50 frames a second even one pixel a
// frame is 50 pixels a second, so the fraction a frame does not move is
// carried into the next: a gentle push creeps rather than lurching. The carry
// is dropped inside the deadzone, so letting go stops the cursor dead rather
// than letting it coast a pixel.
static float mouseCarryX = 0.0f;
static float mouseCarryY = 0.0f;

static int8_t mouseAxisDelta(float axis, float *carry) {
  float off = axis - 0.5f;  // -0.5 .. +0.5
  float mag = off < 0.0f ? -off : off;
  if (mag <= SIDEPAD_MOUSE_DEADZONE) {
    *carry = 0.0f;
    return 0;
  }
  float t = (mag - SIDEPAD_MOUSE_DEADZONE) / (0.5f - SIDEPAD_MOUSE_DEADZONE);
  if (t > 1.0f) t = 1.0f;  // clamp to full deflection
  float scaled = (0.5f * t + 0.5f * t * t) * SIDEPAD_MOUSE_MAX_SPEED;
  float v = *carry + (off < 0.0f ? -scaled : scaled);
  int delta = (int)v;  // toward zero; the rest is carried
  *carry = v - (float)delta;
  return (int8_t)delta;
}

// Publish the right-stick-as-mouse packet to the shared region for the m68k VBL
// hook: enabled flag, left-button (R3) state, and signed per-frame dx/dy. When
// mouse mode is off, publish a disabled/idle packet so the hook does nothing.
// The resident hook consumes this every tick while mouse mode is on.
static void writeBtMouseState(void) {
  controller_state_t st = {0};
  controller_getState(&st);
  uint8_t enabled = 0;
  uint8_t buttons = 0;
  int8_t dx = 0;
  int8_t dy = 0;
  // Nothing to inject without a pad. The axes are centred at rest so this
  // would publish zeros anyway, but saying it explicitly means a future
  // change to the rest state cannot quietly start moving the cursor.
  if (mouseMode && st.connected) {
    enabled = 1;
    dx = mouseAxisDelta(st.rx, &mouseCarryX);
    dy = mouseAxisDelta(st.ry, &mouseCarryY);
    if (st.btnRS) buttons |= 0x02;  // IKBD left button = bit 1 ($FA header)
  } else {
    mouseCarryX = 0.0f;
    mouseCarryY = 0.0f;
  }
  uint32_t v = ((uint32_t)enabled << 24) | ((uint32_t)buttons << 16) |
               ((uint32_t)(uint8_t)dx << 8) | (uint32_t)(uint8_t)dy;
  uintptr_t romBase = (uintptr_t)&__rom_in_ram_start__;
  SET_SHARED_VAR(SIDEPAD_BT_MOUSE_SLOT, v, romBase,
                 CHANDLER_SHARED_VARIABLES_OFFSET);

  // Log only when the packed packet changes, so the serial
  // console shows the analog mapping without flooding at the poll rate.
  static uint32_t prevV = 0;
  if (v != prevV) {
    prevV = v;
    DPRINTF("Mouse slot: en=%u btn=0x%02X dx=%d dy=%d\n", (unsigned)enabled,
            (unsigned)buttons, (int)dx, (int)dy);
  }
}

// Publish the connection state to the shared region so the m68k boot_gem
// handler can pick which fixed banner ("Sidepad connected" / "Sidepad not
// connected") to print from ROM on its way out to the desktop. Must run before
// CMD_BOOT_GEM so the flag is in place when the m68k reads it.
static void writeExitMessage(void) {
  controller_state_t st = {0};
  controller_getState(&st);
  uintptr_t romBase = (uintptr_t)&__rom_in_ram_start__;

  // Connection state -> slot 4 (m68k boot_gem picks the banner from ROM).
  SET_SHARED_VAR(SIDEPAD_EXIT_FLAG_SLOT, st.connected ? 1u : 0u, romBase,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
}

// Read one persisted bool. `absentDefault` is what an upgrade gets: a config
// written before the key existed has no opinion, and for some flags "no
// opinion" must not mean "off".
static bool loadBoolSetting(const char *key, bool absentDefault) {
  SettingsConfigEntry *entry = settings_find_entry(aconfig_getContext(), key);

  if (entry == NULL) {
    return absentDefault;
  }
  return strcmp(entry->value, "true") == 0;
}

// Stage one bool for the next save. Deliberately does NOT flush: settings_save
// erases and reprograms a whole flash sector, so the toggles are staged and
// committed once, in saveSettings(), rather than a sector's worth of erase
// apiece on the way out to the desktop.
static void putBoolSetting(const char *key, bool value) {
  settings_put_bool(aconfig_getContext(), key, value);
}

// Commit everything staged above. One erase+program, not three.
static void saveSettings(void) { settings_save(aconfig_getContext(), true); }

// Leave the terminal for the GEM desktop and never return: install the m68k
// joystick hook, publish the exit banner, hand off to boot_gem, then loop
// forever republishing the joystick byte the resident VBL hook reads. Invoked
// once the main loop exits with exitToDesktop set.
static void exitToGemDesktop(void) {
  // Persist the mouse-mode and hook-mode choices now, on the way out to the
  // desktop: the main terminal loop has just exited and the m68k handoff bursts
  // have not started, so this is the one quiescent point for the flash write
  // (the Atari reads ROM from RAM, so the bus is unaffected by the brief
  // write).
  putBoolSetting(ACONFIG_PARAM_MOUSE, mouseMode);
  putBoolSetting(ACONFIG_PARAM_JOYSTICK, joystickMode);
  putBoolSetting(ACONFIG_PARAM_HOOK, etvMode);
  putBoolSetting(ACONFIG_PARAM_XPAD, xpadMode);
  saveSettings();

  // The joystick injection hook only matters once a game is running, so install
  // it now, as we leave the terminal for the desktop. Installing it earlier
  // would race the VBL handler's cartridge-bus reads against the terminal's
  // send_sync protocol and corrupt keystroke delivery. The m68k terminal loop
  // is still alive here to handle CMD_START. Publish the hook-mode flag first
  // so it is latched when the installer reads it (the installer runs once, on
  // the first CMD_START, and picks the VBL $70 or ETV $400 vector from it).
  // Written inline as a direct shared-var store (same byte layout as
  // SET_SHARED_VAR: low word at slot+2, high word at slot+0), avoiding a
  // single-use helper.
  {
    uintptr_t hookSlot = (uintptr_t)&__rom_in_ram_start__ +
                         CHANDLER_SHARED_VARIABLES_OFFSET +
                         (SIDEPAD_HOOK_MODE_SLOT * 4);
    *((volatile uint16_t *)(hookSlot + 2)) = etvMode ? 1u : 0u;
    *((volatile uint16_t *)(hookSlot)) = 0u;
  }
  // Same idea for the joystick-enabled flag the exit banner lists. Latched
  // once here rather than rewritten every tick: J can only be pressed in the
  // terminal, which has just exited, and the banner is not read until the
  // m68k's first CMD_START after this.
  SET_SHARED_VAR(SIDEPAD_JOY_ENABLED_SLOT, joystickMode ? 1u : 0u,
                 (uintptr_t)&__rom_in_ram_start__,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  // And the Xpad flag, latched here for the same reason: the installer reads
  // it on the first CMD_START below to decide whether to put the cookie in the
  // jar, and X can only be pressed in the terminal we have just left.
  SET_SHARED_VAR(SIDEPAD_XPAD_ENABLED_SLOT, xpadMode ? 1u : 0u,
                 (uintptr_t)&__rom_in_ram_start__,
                 CHANDLER_SHARED_VARIABLES_OFFSET);
  // Lay down the Xpad block before the m68k installer runs, because the same
  // CMD_START burst that installs the hook also installs the cookie pointing
  // at it. A consumer that looked before this ran would find a valid cookie
  // aimed at zeroes. With Xpad off nothing is published at all, and the
  // installer leaves the cookie out to match, so the window stays as the
  // cartridge image left it: zeroes, which no consumer accepts as a block.
  if (xpadMode) {
    xpadstate_init();
    xpadstate_publish();
  }
  for (int i = 0; i < SIDEPAD_HOOK_START_TICKS; i++) {
    // The installer this starts sends APP_XPAD_REQ_ADDR and waits to see it
    // answered before it puts the cookie in, so commands must be heard here,
    // not only once the forever loop below starts. The same goes for the
    // burst after this one, which a warm reset can land in.
    chandler_loop();
    SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
    writeBtJoyState();
    writeBtMouseState();  // publish before the hook starts reading slot 5
    if (xpadMode) xpadstate_publish();
    controller_poll();
    sleep_ms(SLEEP_LOOP_MS);
  }
  // Publish the exit banner before handing off so it is in place when the m68k
  // boot_gem handler prints it. boot_gem prints it as the last thing before
  // rts'ing out of the print loop, so it persists on screen (the print loop
  // stops repainting the framebuffer over it).
  writeExitMessage();
  // Let the m68k continue booting to the GEM desktop. Hold CMD_BOOT_GEM for a
  // few frames rather than a single write: one write can race the m68k's
  // vsync-paced check_commands poll and be dropped, leaving it stuck redrawing
  // the terminal (the symptom: the firmware-load message flashes, then the
  // terminal returns). Repeating it guarantees boot_gem latches.
  for (int i = 0; i < SIDEPAD_BOOT_GEM_TICKS; i++) {
    chandler_loop();
    SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
    writeBtJoyState();
    writeBtMouseState();
    if (xpadMode) xpadstate_publish();
    controller_poll();
    sleep_ms(SLEEP_LOOP_MS);
  }
  // Now keep pumping the BLE host and republishing the joystick byte (and, when
  // mouse mode is on, the mouse packet) the resident VBL hook reads.
  while (true) {
    chandler_loop();
    rumble_tick();
    controller_poll();
    writeBtJoyState();
    writeBtMouseState();
    if (xpadMode) xpadstate_publish();
    sleep_ms(SIDEPAD_JOY_POLL_MS);
  }
}

static void showTitle() {
  term_printString(
      "\x1B"
      "E"
      "MD/Sidepad - " RELEASE_VERSION "\n");
}

// Handle a single key shortcut from the controller screen. Sidepad uses the
// terminal transport purely as a key-input path, so shortcuts act on the first
// keypress (no Enter, unlike the template's line-based command REPL).
static void handleUiKeystroke(char keystroke) {
  DPRINTF("UI keystroke: '%c' (0x%02X)\n", keystroke, (uint8_t)keystroke);
  // Any actionable key leaves the help screen and returns to the controller UI.
  if (helpVisible) {
    helpVisible = false;
    refreshControllerUI(true);
  }
  switch (tolower((unsigned char)keystroke)) {
    case 'r':
      // Reset: disconnect every pad and forget every bond. Not needed to add
      // a pad, since pairing is always open; this is for starting again.
      DPRINTF("Reset requested: forgetting every pad\n");
      controller_requestPairing();
      break;
    case 'm':
      // Toggle right-stick-as-mouse. Persisted on exit to desktop (see
      // exitToGemDesktop), not here, to avoid a flash write on every keypress.
      // Force a full redraw so the second pair / titles appear or disappear.
      mouseMode = !mouseMode;
      DPRINTF("Mouse mode: %s\n", mouseMode ? "on" : "off");
      refreshControllerUI(true);
      break;
    case 'j':
      // Toggle joystick injection, the same shape as M. Xpad keeps being
      // published either way, so this only affects software reading the
      // joystick port.
      joystickMode = !joystickMode;
      DPRINTF("Joystick mode: %s\n", joystickMode ? "on" : "off");
      refreshControllerUI(true);
      break;
    case 'x':
      // Toggle Xpad, the same shape as M and J. Joystick and mouse injection
      // are unaffected; this only decides whether the block and its cookie go
      // out at all. Full redraw so the footer's Xpad label updates.
      xpadMode = !xpadMode;
      DPRINTF("Xpad mode: %s\n", xpadMode ? "on" : "off");
      refreshControllerUI(true);
      break;
    case 'h':
      // Toggle the VBL/ETV hook mode. Persisted on exit to desktop (like mouse
      // mode), and acted on: the installer reads slot 6 and hooks $70 or $400
      // accordingly. Force a full redraw so the footer's Hook label updates.
      etvMode = !etvMode;
      DPRINTF("Hook mode: %s\n", etvMode ? "ETV" : "VBL");
      refreshControllerUI(true);
      break;
    case 'b':
      // Return to Booster via the clean chip-reset path (loop tail).
      resetDeviceAtBoot = false;
      keepActive = false;
      exitToDesktop = false;
      break;
    default:
      break;
  }
}

// Toggle the help screen on the dedicated ST Help key. Debounced so the key's
// auto-repeat does not flicker the screen. Called from the keystroke callback,
// the same context handleUiKeystroke runs in, so rendering here is safe.
static void toggleHelp(void) {
  absolute_time_t now = get_absolute_time();
  if (helpToggleValid && absolute_time_diff_us(helpLastToggle, now) < 350000) {
    return;
  }
  helpLastToggle = now;
  helpToggleValid = true;
  helpVisible = !helpVisible;
  DPRINTF("Help screen: %s\n", helpVisible ? "on" : "off");
  refreshControllerUI(true);
}

// chandler callback: route the m68k's terminal keystrokes straight to the
// Sidepad single-key handler instead of the line-based command REPL.
static void sidepadInputCb(TransmissionProtocol *protocol,
                           uint16_t *payloadPtr) {
  if (protocol == NULL) {
    return;
  }
  // Any key cancels a running auto-exit countdown, and keeps it cancelled for
  // the rest of the session. Only mark cancelled when one was actually running,
  // so keys pressed before the first connect (e.g. [R] to reset) don't suppress
  // the first countdown. Keys with actions (p/m/x/esc) still perform them
  // below. Only the terminal's own commands are keys: the Xpad ones come from
  // the resident hook, once the terminal has gone.
  if (autoExitActive && ((protocol->command_id == APP_TERMINAL_START) ||
                         (protocol->command_id == APP_TERMINAL_KEYSTROKE))) {
    autoExitActive = false;
    autoExitCancelled = true;
  }
  switch (protocol->command_id) {
    case APP_TERMINAL_START:
      // ESC ([ESC] Desktop): leave the terminal loop and continue booting to
      // the GEM desktop, installing the joystick hook on the way out.
      keepActive = false;
      exitToDesktop = true;
      break;
    case APP_TERMINAL_KEYSTROKE: {
      if (payloadPtr == NULL) {
        return;
      }
      uint32_t payload32 = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
      // The dedicated ST Help key produces no useful ASCII, so dispatch it by
      // scan code (carried in bits 16-23 of the payload) before the ASCII path.
      uint8_t scanCode = (uint8_t)((payload32 & TERM_KEYBOARD_SCAN_MASK) >>
                                   TERM_KEYBOARD_SCAN_SHIFT);
      if (scanCode == SCAN_HELP) {
        toggleHelp();
        break;
      }
      handleUiKeystroke((char)(payload32 & TERM_KEYBOARD_KEY_MASK));
      break;
    }
    case APP_XPAD_REQ_ADDR:
      // With Xpad off there is no block to point into, and the installer
      // does not ask; ignore a stray one rather than write into the window.
      if (xpadMode && (payloadPtr != NULL) && (protocol->payload_size >= 8)) {
        uint32_t addr = TPROTO_GET_PAYLOAD_PARAM32(payloadPtr);
        xpadstate_setReq(addr);
        // A new request area means a new session, after a cold boot or a
        // reset: stop whatever the last one left running.
        rumble_reset();
        DPRINTF("Xpad: req at 0x%08lX\n", (unsigned long)addr);
      }
      break;
    case APP_XPAD_RUMBLE:
      if ((payloadPtr != NULL) && (protocol->payload_size >= 12)) {
        rumble_onFrame(payloadPtr);
      }
      break;
    default:
      break;
  }
}

// This section contains the functions that are called from the main loop

static bool getKeepActive() { return keepActive; }

static bool getResetDevice() { return resetDeviceAtBoot; }

static void preinit() {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString("Loading, please wait...\n");

  display_refresh();
}

static void init(void) {
  // Clear the screen
  term_clearScreen();

  // Draw the Sidepad controller screen from the live controller state.
  refreshControllerUI(true);
  uiRefreshTime = make_timeout_time_ms(UI_REFRESH_MS);

  display_refresh();
}

void emul_start() {
  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = APP_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF(
        "APP_MODE_SETUP not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  // In this example, the flow will always start the configuration app first
  // The ROM Emulator app for example will check here if the start directly
  // in emulation mode is needed or not

  // 3. If we are here, it means the app is not in emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Clear the command sentinel in the freshly-copied mirror before the bus
  // comes up. COPY_FIRMWARE only writes the zero-trimmed cartridge (~2.4 KB),
  // and chandler_init zeroes only the reserved slot, not the sentinel, so
  // $FA2000 would otherwise hold stale SRAM the m68k could read as a spurious
  // command. Matches md-snap.
  *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CHANDLER_CMD_SENTINEL_OFFSET)) = 0;

  // Initialize the cartridge ROM4 read engine. ROM4 reads are served entirely
  // by chained DMAs feeding the PIO TX FIFO — no CPU/IRQ involvement.
  // Without this engine the cartridge image is unreadable from the m68k,
  // so a failure here is fatal: panic instead of stumbling on with a half-
  // configured PIO/DMA setup.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and the
  // command handler that polls the ring, parses the protocol, and dispatches
  // each command to the registered callbacks. commemul is similarly load-
  // bearing — without it the m68k can issue commands but the RP never sees
  // them, so any non-OK return is fatal.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(sidepadInputCb);

  // After this point, the remote computer can execute the code

  // 4. During the setup/configuration mode, the driver code must interact
  // with the user to configure the device. To simplify the process, the
  // terminal emulator is used to interact with the user.
  // The terminal emulator is a simple text-based interface that allows the
  // user to configure the device using text commands.
  // If you want to use a custom app in the remote computer, you can do it.
  // But it's easier to debug and code in the rp2040

  // Initialize the display
  display_setupU8g2();

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM, floppies, even
  // full hard disk files, configuration files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app continues and reports SD status in the terminal menu.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.
  // If there is no folder in the micro SD card, the app will create it.

  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/test";  // MODIFY THIS TO YOUR FOLDER NAME
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // Pre-init the stuff
  // In this example it only prints the please wait message, but can be used as
  // a place to put other code that needs to be run before the network is
  // initialized
  preinit();

  // Restore the persisted mouse-mode and hook-mode choices so the first UI
  // render reflects them without needing the user to toggle.
  mouseMode = loadBoolSetting(ACONFIG_PARAM_MOUSE, false);
  // Absent means a config predating the toggle: keep the joystick.
  joystickMode = loadBoolSetting(ACONFIG_PARAM_JOYSTICK, true);
  etvMode = loadBoolSetting(ACONFIG_PARAM_HOOK, false);
  // Absent means a config predating the toggle: keep publishing Xpad.
  xpadMode = loadBoolSetting(ACONFIG_PARAM_XPAD, true);

  // 6. Bring up the BLE controller host.
  // Sidepad does not use WiFi: the CYW43 chip is shared between WiFi and
  // Bluetooth, and we bring it up in BT-only mode via controller_init()
  // (which calls network_initChipOnly() then starts Bluepad32). The terminal
  // already shows the controller screen; surface init progress/errors there.
  controller_setStatus("Starting BLE controller host...");
  refreshControllerUI(true);
  int controllerErr = controller_init();
  if (controllerErr != 0) {
    DPRINTF("controller_init failed: %i\n", controllerErr);
    controller_setStatus("BLE init failed - check Pico W / CYW43");
  }
  refreshControllerUI(true);

  // 7. Configure the SELECT button so menu status can show it immediately.
  select_configure();

  // 8. Now complete the terminal emulator initialization
  // The terminal emulator is used to interact with the user to configure the
  // device.
  init();

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. Start the main loop
  // The main loop is the core of the app. It is responsible for running the
  // app, handling the user input, and performing the tasks of the app.
  // The main loop runs until the user decides to exit.
  // For testing purposes, this app only shows commands to manage the settings
  DPRINTF("Start the app loop here\n");
  while (getKeepActive()) {
    // Drain the ROM3 command ring → dispatch to registered callbacks
    // (sidepadInputCb handles single-key shortcuts).
    chandler_loop();

    // Drive the BLE controller host (CYW43 + BTstack poll, status update).
    controller_poll();

    // Auto-exit countdown. Start it when a controller connects (rising edge);
    // cancel it if the controller drops. When it elapses, exit to the desktop
    // exactly as ESC would (a keypress cancels it via sidepadInputCb).
    static bool prevConnected = false;
    controller_state_t connState = {0};
    controller_getState(&connState);
    if (connState.connected && !prevConnected && !autoExitCancelled) {
      autoExitActive = true;
      autoExitDeadline = make_timeout_time_ms(SIDEPAD_AUTOEXIT_SECONDS * 1000);
    } else if (!connState.connected) {
      autoExitActive = false;
    }
    prevConnected = connState.connected;
    // Any controller input also cancels the running countdown (sticky, like a
    // keypress). Use the deadzoned directions + buttons; exclude the Guide
    // button (the wake/connect press) so connecting doesn't self-cancel.
    if (autoExitActive &&
        (connState.anyUp || connState.anyDown || connState.anyLeft ||
         connState.anyRight || connState.anyButton || connState.btnView ||
         connState.btnMenu)) {
      DPRINTF("Auto-exit cancelled by controller input\n");
      autoExitActive = false;
      autoExitCancelled = true;
    }
    if (autoExitActive &&
        absolute_time_diff_us(get_absolute_time(), autoExitDeadline) <= 0) {
      DPRINTF("Auto-exit countdown elapsed; exiting to desktop\n");
      autoExitActive = false;
      keepActive = false;
      exitToDesktop = true;
    }

    // Redraw the live controller UI on a fixed cadence.
    if (absolute_time_diff_us(get_absolute_time(), uiRefreshTime) <= 0) {
      refreshControllerUI(false);
      uiRefreshTime = make_timeout_time_ms(UI_REFRESH_MS);
    }

    sleep_ms(SLEEP_LOOP_MS);
  }

  if (exitToDesktop) {
    exitToGemDesktop();  // never returns
  }

  // 10. Send RESET computer command
  // Ok, so we are done with the setup but we want to reset the computer to
  // reboot in the same microfirmware app or start the booster app

  sleep_ms(SLEEP_LOOP_MS);
  // We must reset the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  sleep_ms(SLEEP_LOOP_MS);
  if (getResetDevice()) {
    // Reset the device
    reset_device();
  } else {
    // Before jumping to the booster app, let's clean the settings
    // Set emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                         APP_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Return to Booster via a full chip reset rather than an in-place jump: the
    // reset clears the pio0/DMA state this app brought up (romemul + commemul),
    // so Booster starts on pristine hardware. An in-place jump leaves those
    // running and corrupts Booster (garbled screen). main() routes back to
    // Booster on the clean restart.
    DPRINTF("Rebooting to the booster app...\n");
    reset_reboot_to_booster();
  }
}
