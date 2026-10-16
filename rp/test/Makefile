# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests for the Xpad block writer and the rumble decisions. Need
# nothing but a C compiler: stub/ stands in for the Pico SDK headers
# xpadstate.c pulls in, and rumble.h has none.
CC      = cc
# xpad.h comes from the submodule at the repository root, not a copy in
# this tree, so a bump cannot leave the two disagreeing about the layout.
XPAD    = ../../xpad/src
CFLAGS  = -Wall -Wextra -Werror -std=c11 -I stub -I ../src -I ../src/include -I $(XPAD)
BUILD   = build

.PHONY: test clean
test: | $(BUILD)
	$(CC) $(CFLAGS) xpadstate_test.c -o $(BUILD)/xpadstate_test
	@$(BUILD)/xpadstate_test
	$(CC) $(CFLAGS) rumble_test.c -o $(BUILD)/rumble_test
	@$(BUILD)/rumble_test

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
