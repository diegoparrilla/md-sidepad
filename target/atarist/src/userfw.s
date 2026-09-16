;
; Copyright (C) 2026 Neil Rackett
; SPDX-License-Identifier: GPL-3.0-or-later
;

; Sidepad user firmware: inject the BT controller as Atari ST joystick 1, and
; (when mouse mode is on) the right stick as the GEM mouse via the IKBD mousevec.
;
; Called once (via main.s rom_function) when the RP raises CMD_START.
;
; Mechanism
; ---------
; The RP writes a packed controller state byte to shared-variable slot 3
; every loop (bit0=Up, bit1=Down, bit2=Left, bit3=Right, bit6=Fire - the standard
; IKBD encoding, confirmed on hardware via JOYMOUT). A handler hooked onto the VBL autovector
; ($70) watches that byte and, when it changes, synthesises a TOS joystick packet
; [$FF, joy0, joy1] (our state in the joy1 byte) and calls the system joyvec. The
; real joyvec is left in place, so a physical joystick on port 1 keeps working
; alongside the controller — whichever one is moved drives joystick 1.
;
; Why hook $70 and not _vblqueue
; ------------------------------
; A free _vblqueue slot works during boot but GEM reclaims it for its own VBL
; routines when the desktop loads, silently de-linking us. The $70 autovector is
; the OS's VBL entry point and GEM does not replace it (it only adds/removes
; _vblqueue entries, which the $70 handler walks), so a hook there survives into
; the desktop. We chain to the saved original so the OS VBL processing still runs.
;
; VBL vs ETV hook (selectable at install time)
; --------------------------------------------
; The RP publishes a hook-mode flag to shared-var slot 6 (HOOK_FLAG_ADDR) before
; the installer runs: 0 = VBL, 1 = ETV. VBL ($70) fires every vsync (~50 Hz) and
; is the default. ETV (etv_timer, $400) is a TOS subroutine vector called from the
; MFP Timer C interrupt at ~200 Hz; it survives programs that replace the VBL
; vector, but we only need ~50 Hz so the ETV entry injects on every ETV_DIVISOR'th
; tick (4 -> ~50 Hz) and chains the rest. Both entries share one injection routine
; and the same `push orig; rts` chain, valid for the $70 autovector (the saved
; original rte's the interrupt frame) and the $400 subroutine (the saved original
; rts's back to TOS' MFP ISR, which rte's).
;
; Why copy to RAM
; ---------------
; The whole cartridge region ($FA0000-$FAFFFF) is emulated read-only to the
; ST: code can be fetched/read from it, but stores bus-error. The resident
; handler needs writable state (saved joyvec, previous sample, packet
; buffer), so the installer Malloc's a RAM block and copies the resident,
; position-independent handler into it. Reads of the shared region (the
; controller byte) still work, so the handler reads slot 3 in place.

    section text

GEMDOS_Malloc       equ 72          ; trap #1
XBIOS_Kbdvbase      equ 34          ; trap #14 -> KBDVECS pointer in d0
KBDVECS_JOYVEC_OFF  equ 24          ; joyvec field within KBDVECS (offset 24!)
KBDVECS_MOUSEVEC_OFF equ 16         ; mousevec field within KBDVECS
VBL_VECTOR          equ $70         ; .l  level-4 (VBL) autovector
ETV_TIMER_VECTOR    equ $400        ; .l  etv_timer subroutine vector (MFP Timer C, ~200 Hz)
ETV_DIVISOR         equ 4           ; ETV ticks per injection: ~200 Hz / 4 = ~50 Hz (match VBL)
BT_JOY_BYTE         equ $FA201F     ; LSB of shared-var slot 3 (RP writes it)
HOOK_FLAG_ADDR      equ $FA202B     ; LSB of shared-var slot 6: 0 = VBL ($70), 1 = ETV ($400)
XPAD_FLAG_ADDR      equ $FA2033     ; LSB of shared-var slot 8: 1 = publish the Xpad cookie
; Mouse packet in shared-var slot 5 ($FA2024), one byte each (RP writes them).
; move.l reads the slot big-endian, so byte0 is at the lowest address:
BT_MOUSE_ENABLED    equ $FA2024     ; mouse mode on (1) / off (0)
BT_MOUSE_BTN        equ $FA2025     ; IKBD button bits (bit1 = left button / R3)
BT_MOUSE_DX         equ $FA2026     ; signed per-frame dx
BT_MOUSE_DY         equ $FA2027     ; signed per-frame dy

; Xpad block, published by the RP into APP_FREE. Consumers find it through
; the cookie jar and read it straight out of the window: no copy, no hook,
; and nothing for a game to clobber. Must match XPADSTATE_ST_BLOCK in
; rp/src/include/xpadstate.h.
; XPAD_COOKIE, XPAD_JAR and the rest of the ABI come from the xpad
; submodule, generated from xpad.h by `make inc` there, so they cannot
; be transcribed wrongly here. See the -I in the Makefile.
    include "xpad.inc"

XPAD_BLOCK          equ $FA2300     ; the block itself, in the cartridge window

; -----------------------------------------------------------------------
; Installer — runs from ROM at USERFW ($FA0800). Only writes to RAM.
; -----------------------------------------------------------------------
; a3 holds the RAM block base across the trap calls: GEMDOS/XBIOS may clobber
; d0-d2/a0-a2, but preserve d3-d7/a3-a6, so a3 survives the traps below.
userfw:
    movem.l d0-d2/a0-a3, -(sp)

    ; Publish the Xpad cookie first. It is independent of everything below:
    ; the block lives in the cartridge window, not in the RAM block, so it
    ; should go in even if the Malloc fails and no hook gets installed.
    ; Skipped entirely when the RP has Xpad turned off (slot 8): it publishes
    ; no block then, so a cookie here would point at empty window.
    tst.b   XPAD_FLAG_ADDR
    beq.s   .uf_no_xpad
    bsr     install_xpad_cookie
.uf_no_xpad:

    ; Malloc a RAM block for the resident handler + its state.
    move.l  #(resident_end-resident_start), -(sp)
    move.w  #GEMDOS_Malloc, -(sp)
    trap    #1
    addq.l  #6, sp
    tst.l   d0
    beq.s   .uf_exit                ; out of memory -> install nothing
    move.l  d0, a3                  ; a3 = RAM block base (survives traps)

    ; Copy the resident code+data (in ROM) into the RAM block.
    lea     resident_start(pc), a0  ; PC in ROM -> ROM source address
    move.l  a3, a1                  ; a1 = destination
    move.w  #(resident_end-resident_start-1), d1
.uf_copy:
    move.b  (a0)+, (a1)+
    dbf     d1, .uf_copy

    ; Save the ADDRESS of the KBDVECS joyvec slot (kbdvecs+24) into the block.
    ; The VBL handler dereferences this live each frame, so it always chains to
    ; whatever joyvec is currently installed -- a joystick reader (game, test
    ; tool) typically installs its own joyvec AFTER us, and a saved snapshot
    ; would bypass it.
    move.w  #XBIOS_Kbdvbase, -(sp)
    trap    #14
    addq.l  #2, sp                  ; d0 = KBDVECS pointer
    move.l  d0, a0
    lea     KBDVECS_JOYVEC_OFF(a0), a0
    move.l  a0, (res_joyslot-resident_start)(a3)
    ; Save the mousevec slot address too (right-stick-as-mouse). Same live-read
    ; rationale as joyvec: a reader may install its own mousevec after us.
    move.l  d0, a0
    lea     KBDVECS_MOUSEVEC_OFF(a0), a0
    move.l  a0, (res_mouseslot-resident_start)(a3)

    ; Hook either the VBL autovector ($70) or the etv_timer subroutine vector
    ; ($400), per the hook-mode flag the RP published to slot 6 (HOOK_FLAG_ADDR):
    ; 0 = VBL, 1 = ETV. Save the original at the chosen vector into the block; the
    ; resident chains back to it. Mask interrupts across the swap so a tick can't
    ; fire mid-update. Both vectors are supervisor-only; the installer runs
    ; supervisor at boot, so the access is legal.
    moveq   #0, d0
    move.b  HOOK_FLAG_ADDR, d0
    cmp.b   #1, d0
    beq.s   .uf_hook_etv

    ; VBL: point $70 at resident_start (block offset 0).
    move.w  sr, -(sp)
    ori.w   #$0700, sr             ; raise IPL to 7 (block interrupts)
    move.l  VBL_VECTOR.w, (orig_hook-resident_start)(a3)
    move.l  a3, VBL_VECTOR.w
    move.w  (sp)+, sr
    bra.s   .uf_exit

.uf_hook_etv:
    ; ETV: point $400 at the resident ETV entry (a known offset into the block).
    move.l  a3, d0
    add.l   #(resident_etv_start-resident_start), d0
    move.w  sr, -(sp)
    ori.w   #$0700, sr             ; raise IPL to 7 (block interrupts)
    move.l  ETV_TIMER_VECTOR.w, (orig_hook-resident_start)(a3)
    move.l  d0, ETV_TIMER_VECTOR.w
    move.w  (sp)+, sr

    ; The exit banner is printed later by main.s boot_gem (from the RP-composed
    ; shared-region string), not here -- printing from the installer would land
    ; back in the terminal print loop and be repainted over. This routine only
    ; installs the joystick hook.

.uf_exit:
    movem.l (sp)+, d0-d2/a0-a3
    rts

; -----------------------------------------------------------------------
; Point the XPAD cookie at the block in the window, so any Xpad-aware
; program can find the controller without going near joyvec.
;
; The jar is pairs of (id, value) ending in a (0, slots_remaining)
; terminator. Replace an existing entry, otherwise append if there is room.
; Runs from the installer, which is supervisor at boot, so the system
; variable at $5A0 is legal to touch here.
;
; Silently does nothing when there is no jar at all, which is TOS 1.0 and
; nothing else. Clobbers d0/a0, both saved by the caller.
; -----------------------------------------------------------------------
install_xpad_cookie:
    move.l  XPAD_JAR.w, d0
    beq.s   .xc_done                ; no jar: nothing we can do from here
    move.l  d0, a0

.xc_scan:
    move.l  (a0), d0
    beq.s   .xc_append              ; terminator reached
    cmp.l   #XPAD_COOKIE, d0
    beq.s   .xc_replace
    addq.l  #8, a0                  ; next (id, value) pair
    bra.s   .xc_scan

.xc_replace:
    move.l  #XPAD_BLOCK, 4(a0)
    rts

.xc_append:
    move.l  4(a0), d0               ; the terminator carries the free count
    cmp.l   #2, d0
    blt.s   .xc_done                ; jar full: the caller must enlarge it
    ; Lay the new terminator down before overwriting this pair, so a
    ; consumer scanning concurrently never runs off the end of the jar.
    clr.l   8(a0)
    subq.l  #1, d0
    move.l  d0, 12(a0)
    move.l  #XPAD_COOKIE, (a0)
    move.l  #XPAD_BLOCK, 4(a0)

.xc_done:
    rts

; -----------------------------------------------------------------------
; Resident block — copied verbatim into RAM and run from there. Must be
; position independent: it references its own data/subroutines PC-relative and
; the shared region by absolute address. Two entry points share one injection
; routine: resident_start (offset 0) goes on the VBL autovector ($70);
; resident_etv_start goes on etv_timer ($400). Only one is hooked per session
; (the installer picks from HOOK_FLAG_ADDR). Both preserve every register they
; touch and tail-chain into the saved original handler.
; -----------------------------------------------------------------------
    even
; VBL autovector ($70) entry: fires every vsync (~50 Hz). Inject, then chain.
resident_start:
    ; Save the FULL register set, not just our scratch: we jsr into the system
    ; joyvec/mousevec, and mousevec (line-A / AES cursor machinery) can clobber
    ; d3-d7/a3-a6. Restoring only d0-d2/a0-a2 would leak that corruption back into
    ; the interrupted program -> intermittent bombs.
    movem.l d0-d7/a0-a6, -(sp)
    bsr     do_inject
    bra     res_chain

    even
; etv_timer ($400) entry: TOS' MFP Timer C calls this ~200 Hz. We only want
; ~50 Hz, so inject on every ETV_DIVISOR'th tick and chain the rest.
resident_etv_start:
    movem.l d0-d7/a0-a6, -(sp)
    lea     etv_div(pc), a0
    subq.b  #1, (a0)
    bgt     res_chain               ; not the Nth tick yet -> skip injection
    move.b  #ETV_DIVISOR, (a0)      ; reload the divider for the next group
    bsr     do_inject

; Shared chain tail: restore registers and jump to the saved original handler.
; `push orig; rts` is valid for the $70 autovector (orig rte's the interrupt
; frame) and the $400 subroutine (orig rts's back to TOS' MFP ISR, which rte's).
res_chain:
    movem.l (sp)+, d0-d7/a0-a6
    move.l  orig_hook(pc), -(sp)
    rts

    even
; do_inject — the joystick + (optional) mouse injection shared by both entries.
; The caller has already saved d0-d7/a0-a6, so this is free to clobber registers.
; Returns via rts.
do_inject:
    move.b  BT_JOY_BYTE, d0         ; current controller state (read OK)
    lea     res_prev(pc), a1
    cmp.b   (a1), d0
    beq.s   .di_mouse               ; unchanged -> skip joystick, try mouse
    move.b  d0, (a1)                ; remember new state

    ; Translate to an IKBD joystick byte: the direction bits (0-3) already match
    ; the IKBD encoding and pass through unchanged; move our fire (bit 6) to the
    ; IKBD fire position (bit 7).
    move.b  d0, d1
    and.b   #$0F, d1
    btst    #6, d0
    beq.s   .di_nofire
    bset    #7, d1
.di_nofire:
    ; Build a real IKBD joystick packet and call the CURRENT joyvec (read live
    ; from the KBDVECS slot). TOS delivers joystick packets as three bytes
    ; [$FF, joystick0, joystick1] with the $FF header at an ODD address, so a
    ; consumer that reads `move.w 1(a0)` (e.g. PP's JOYMOUT tester) gets joy0/joy1
    ; as an aligned word. We drive joystick 1, so our state goes in the joy1 byte
    ; and joy0 is left idle. res_pkt is even-aligned, so res_pkt+1 is the odd
    ; header byte we hand to the vector.
    lea     res_pkt(pc), a0
    move.b  #$FF, 1(a0)             ; header at odd address (res_pkt+1)
    clr.b   2(a0)                   ; joystick 0 = idle
    move.b  d1, 3(a0)              ; joystick 1 = our state
    move.l  res_joyslot(pc), a1     ; address of the KBDVECS joyvec slot
    move.l  (a1), d2                ; current joyvec (live read)
    beq.s   .di_mouse               ; no handler installed -> skip to mouse
    move.l  d2, a1
    addq.l  #1, a0                  ; a0 -> $FF header (odd address)
    ; Block the IKBD ACIA interrupt (IPL 6) across the call, same as the mousevec
    ; call below: driving joyvec leaves the ACIA unmasked, so a real IKBD packet
    ; could otherwise re-enter joyvec on top of us.
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    jsr     (a1)                    ; joyvec convention: a0 -> [$FF, joy0, joy1]
    move.w  (sp)+, sr

.di_mouse:
    ; --- Mouse: while enabled, inject a relative IKBD mouse packet every frame
    ; the stick is deflected (relative deltas accumulate) or the button changed.
    ; Calls the live mousevec, so GEM's cursor (read via graf_mkstate) moves.
    tst.b   BT_MOUSE_ENABLED
    beq.s   .di_done                ; mouse mode off -> done
    move.b  BT_MOUSE_BTN, d0        ; IKBD buttons (bit1 = left = R3)
    move.b  BT_MOUSE_DX, d1         ; signed dx
    move.b  BT_MOUSE_DY, d2         ; signed dy
    tst.b   d1
    bne.s   .di_domouse             ; moving in x -> inject
    tst.b   d2
    bne.s   .di_domouse             ; moving in y -> inject
    lea     res_mprev(pc), a1       ; idle: inject only if the button changed
    cmp.b   (a1), d0
    beq.s   .di_done
.di_domouse:
    lea     res_mprev(pc), a1
    move.b  d0, (a1)                ; remember button state
    or.b    #$F8, d0               ; relative-mouse header: $F8 | buttons
    lea     res_mpkt(pc), a0
    move.b  d0, (a0)               ; header
    move.b  d1, 1(a0)             ; dx
    move.b  d2, 2(a0)             ; dy
    move.l  res_mouseslot(pc), a1   ; address of the KBDVECS mousevec slot
    move.l  (a1), d0                ; current mousevec (live read)
    beq.s   .di_done                ; no handler installed -> skip
    move.l  d0, a1
    ; Block the IKBD ACIA interrupt (IPL 6) across the call: driving mousevec
    ; leaves the ACIA unmasked, so a real IKBD packet could otherwise re-enter
    ; mousevec on top of us and corrupt its state.
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    jsr     (a1)                    ; mousevec convention: a0 -> [header, dx, dy]
    move.w  (sp)+, sr
.di_done:
    rts

    even
res_prev:
    dc.b    0
    even
res_pkt:
    dc.b    0,0,0,0
    even
res_joyslot:
    dc.l    0
    even
res_mprev:
    dc.b    0
    even
res_mpkt:
    dc.b    0,0,0
    even
res_mouseslot:
    dc.l    0
    even
etv_div:
    dc.b    1                      ; ETV tick divider (1 -> inject on first tick)
    even
orig_hook:
    dc.l    0                      ; saved original handler at the hooked vector
resident_end:
    even
; Pad the cartridge image so its byte count stays even after firmware.py trims
; trailing zeros: the resident block ends in an all-zero dc.l (orig_hook) that
; gets trimmed, which can leave the last real byte at an even offset -> odd
; length -> firmware.py aborts and the build silently embeds a STALE cartridge.
; A non-zero word tail (outside the copied block) forces the trimmed size even.
    dc.w    $FFFF
