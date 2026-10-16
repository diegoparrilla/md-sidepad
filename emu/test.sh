#!/bin/sh
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# MD/Sidepad in Hatari, with EmuMD (emu/emumd), the whole way: the real
# firmware, controller.c included, with emu/test/pads.txt playing the
# controllers through EmuMD's Bluetooth stand-in. Player 1 connecting
# starts the auto-exit, so the firmware leaves the terminal by itself and
# the cartridge installs the resident hook and the cookie. The ST then
# boots from a floppy whose AUTO folder runs XPADTEST.PRG (emu/test),
# which reads the block and asks for rumble; the firmware's log says what
# reached the controllers.
#
# There is no harddrive, because a GEMDOS drive takes the first 1 KB of
# the cartridge, where MD/Sidepad's boot code lives. Needs stcmd, mtools
# (mformat, mmd, mcopy), libslirp and python3.
#
#   emu/test.sh          (MDFW=.../tools/mdfw for another EmuMD)

set -eu
cd "$(dirname "$0")/.."
MDFW=${MDFW:-emu/emumd/tools/mdfw}

$MDFW build
env STCMD_NO_TTY=1 ST_WORKING_FOLDER="$PWD" stcmd make -s -C emu/test >/dev/null

# Kept after the run, so a failure can be looked into: the console in
# out.txt, the firmware's log in hatari.log.
WORK=build/emu-test
rm -rf "$WORK"
mkdir -p "$WORK"

mformat -i "$WORK/test.st" -f 720 -C ::
mmd -i "$WORK/test.st" ::AUTO
mcopy -i "$WORK/test.st" emu/test/dist/XPADTEST.PRG ::AUTO/

# No -V: the firmware's debug output would land in the middle of the ST's
# console lines. What is checked below is logged without it.
$MDFW run --headless --frames 3000 --timeout 300 --log "$WORK/hatari.log" \
    -O "pads=$PWD/emu/test/pads.txt" -- --disk-a "$WORK/test.st" --conout 2 \
    > "$WORK/out.txt" 2>&1 || { tail -40 "$WORK/out.txt"; exit 1; }
cat "$WORK/out.txt" "$WORK/hatari.log" | tr -d '\r' > "$WORK/all.txt"

fail=0
expect() { # TEXT
    grep -qF -- "$1" "$WORK/all.txt" || { echo "test: expected $1"; fail=1; }
}
# The last rumble line for PAD, which must be a stop. Counted in the log
# alone: the firmware's lines reach the console output as well.
last_rumble() {
    grep -F "Bluepad32: pad $1 rumble" "$WORK/hatari.log" | tail -1
}

grep -F XPADTEST "$WORK/out.txt" | tr -d '\r' || true

# The firmware's side.
expect "Bluepad32: pad 0 ready"
expect "Bluepad32: pad 1 ready"
expect "Bluepad32: pad 0 leds=0x01"
expect "Bluepad32: pad 1 leds=0x02"
expect "Bluepad32: pad 0 rumble weak=0 strong=200"
expect "Bluepad32: pad 1 rumble weak=90 strong=0"
[ "$(grep -cF 'Bluepad32: pad 0 rumble weak=0 strong=200' "$WORK/hatari.log")" -ge 2 ] ||
    { echo "test: expected pad 0's rumble re-armed while held"; fail=1; }
for pad in 0 1; do
    case "$(last_rumble $pad)" in
    *"weak=0 strong=0"*) ;;
    *) echo "test: pad $pad was not left stopped: $(last_rumble $pad)"; fail=1 ;;
    esac
    # Stopped once, by XPADTEST: a second stop means the RP's safety
    # timeout cut in while the ST was still holding the level, so the
    # keepalive is not arriving often enough.
    stops=$(grep -cF "Bluepad32: pad $pad rumble weak=0 strong=0" "$WORK/hatari.log")
    [ "$stops" = 1 ] || { echo "test: pad $pad was stopped $stops times, not once"; fail=1; }
done
expect "Bluepad32: pad 1 disconnected"

# The ST's side.
expect "pads=4 caps=000b"
expect "XPADTEST pad1 buttons=00000010 type=2"
expect "XPADTEST-DONE 0"
if grep -q "XPADTEST.*FAIL" "$WORK/all.txt"; then fail=1; fi

[ $fail = 0 ] && echo "test: all good"
exit $fail
