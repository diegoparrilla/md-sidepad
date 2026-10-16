# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make emu, included from the Makefile. It lives here rather than there so
# that changing it is an emulator-only change, which CI does not build or
# release (paths-ignore in .github/workflows).

## Build MD/Sidepad for this computer and run it in Hatari with EmuMD,
## controllers played from PADS (default emu/pads.txt). TOS= a TOS image
## (default: EmuMD's EmuTOS); EMUMD= another EmuMD checkout.
EMUMD ?= emu/emumd
PADS ?= emu/pads.txt
.PHONY: emu
emu:
	$(EMUMD)/tools/mdfw run $(if $(TOS),--tos "$(TOS)") -O pads=$(abspath $(PADS))

## Test it there, headless: four slots, a second pad, rumble out and back
## (emu/test.sh).
.PHONY: emu-test
emu-test:
	MDFW=$(EMUMD)/tools/mdfw emu/test.sh
