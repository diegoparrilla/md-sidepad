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
; the installer runs: 0 = VBL, 1 = ETV. VBL ($70) fires every vsync (50, 60 or
; 71 Hz with the display). ETV (etv_timer, $400) is a TOS subroutine vector,
; and the default: it survives programs that replace the VBL vector. TOS calls
; it from the MFP Timer C interrupt, but only on every fourth of Timer C's
; 200 Hz ticks, so 50 Hz, the IKBD's own rate, and the ETV entry injects on
; every call. (It once divided by four again on the belief that etv_timer ran
; at 200 Hz, which left ETV mode injecting at 12.5 Hz. Measured under TOS
; 1.62, 2.06 and EmuTOS: 50 Hz.) Both entries share one injection routine and
; the same `push orig; rts` chain, valid for the $70 autovector (the saved
; original rte's the interrupt frame) and the $400 subroutine (the saved original
; rts's back to TOS' MFP ISR, which rte's).
;
;
; Xpad rumble
; -----------
; A game asks for rumble by writing levels into an XPAD_REQ area and bumping
; its seq. The block lives in the read-only cartridge window, so the request
; area cannot: it is res_req, inside the RAM block below. The installer sends
; its address to the RP (APP_XPAD_REQ_ADDR) and waits to see it come back as
; the block's req pointer before it puts the cookie in, so no consumer can
; find the block without it. From then on the resident hook forwards all four
; pads' levels as one APP_XPAD_RUMBLE frame when seq moves, and again every
; RQ_KEEPALIVE (200 ms) while any motor is on. The RP stops a pad whose frames
; stop arriving, which is what keeps an ST reset, or a game that unhooks us,
; from leaving a motor running. See rp/src/include/rumble.h.
;
; Frames go to the RP the way the terminal's send_sync sends them, as reads
; in ROM3 whose low address bits are the data, but nothing is waited for:
; this runs in an interrupt, and the next frame repairs a lost one.
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
ETV_TIMER_VECTOR    equ $400        ; .l  etv_timer subroutine vector (every 4th Timer C tick, 50 Hz)
BT_JOY_BYTE         equ $FA201F     ; LSB of shared-var slot 3 (RP writes it)
HOOK_FLAG_ADDR      equ $FA202B     ; LSB of shared-var slot 6: 0 = VBL ($70), 1 = ETV ($400)
XPAD_FLAG_ADDR      equ $FA2033     ; LSB of shared-var slot 8: 1 = publish the Xpad cookie
; Mouse packet in shared-var slot 5 ($FA2024), one byte each (RP writes them).
; move.l reads the slot big-endian, so byte0 is at the lowest address:
BT_MOUSE_ENABLED    equ $FA2024     ; mouse mode on (1) / off (0)
BT_MOUSE_BTN        equ $FA2025     ; IKBD button bits (bit1 = left button / R3)
BT_MOUSE_DX         equ $FA2026     ; signed per-frame dx
BT_MOUSE_DY         equ $FA2027     ; signed per-frame dy

; ROM3 commands to the RP, for Xpad rumble. Must match APP_XPAD_* in
; rp/src/emul.c, which describes the payloads; well clear of the terminal's
; 0 (ESC) and 1 (keystroke).
APP_XPAD_REQ_ADDR   equ $5850       ; payload: XPAD_REQ address, low word first
APP_XPAD_RUMBLE     equ $5851       ; payload: four words, lo << 8 | hi per pad
ROM3_CMD_ADDR       equ $FB8000     ; middle of ROM3: (a0,dN.w) reaches all 64 KB
CMD_HEADER          equ $ABCD       ; starts every command, as in main.s
FRCLOCK             equ $466        ; _frclock: VBLs since boot (supervisor)
HZ200               equ $4BA        ; _hz_200: 5 ms ticks since boot (supervisor)

; Rumble timing is measured on _hz_200, not in hook ticks: the hook runs at
; the display's rate on VBL (50, 60 or 71 Hz) and at etv_timer's 50 Hz on
; ETV, and the RP's timeout is in real time.
RQ_MIN_GAP          equ 6           ; 30 ms between two different frames
RQ_KEEPALIVE        equ 40          ; 200 ms between keepalives
HS_FRAMES           equ 50          ; frames to wait for the RP (~1 s)
HS_RESEND           equ 8           ; resend the address every 8 frames (2^n)
HS_SPIN             equ 4000        ; cap on one frame's wait, ~15 ms at 8 MHz

; Xpad block, published by the RP into APP_FREE. Consumers find it through
; the cookie jar and read it straight out of the window: no copy, no hook,
; and nothing for a game to clobber. Must match XPADSTATE_ST_BLOCK in
; rp/src/include/xpadstate.h.
; XPAD_COOKIE, XPAD_JAR and the rest of the ABI come from the xpad
; submodule, generated from xpad.h by `make inc` there, so they cannot
; be transcribed wrongly here. See the -I in the Makefile.
    include "xpad.inc"

XPAD_BLOCK          equ $FA2300     ; the block itself, in the cartridge window
; The block's req pointer, which the RP fills in from APP_XPAD_REQ_ADDR.
; xpad.inc stops at XPAD_HDR_FIXED because pointer sizes differ between
; machines; on the m68k provider then req follow it, 4 bytes each. Pinned
; on the RP side by a _Static_assert on OFF_REQ in xpadstate.c.
XPAD_REQ_PTR_ADDR   equ (XPAD_BLOCK+XPAD_HDR_FIXED+4)

; -----------------------------------------------------------------------
; Installer — runs from ROM at USERFW ($FA0800). Only writes to RAM.
; -----------------------------------------------------------------------
; a3 holds the RAM block base across the trap calls: GEMDOS/XBIOS may clobber
; d0-d2/a0-a2, but preserve d3-d7/a3-a6, so a3 survives the traps below.
userfw:
    movem.l d0-d4/a0-a3, -(sp)

    ; Malloc a RAM block for the resident handler + its state.
    move.l  #(resident_end-resident_start), -(sp)
    move.w  #GEMDOS_Malloc, -(sp)
    trap    #1
    addq.l  #6, sp
    tst.l   d0
    beq     .uf_cookie_only         ; out of memory -> no hook, no rumble
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

    ; Publish the Xpad cookie, after telling the RP where our request area
    ; is: a consumer finding the block must find its req pointer filled in.
    ; Both are skipped when the RP has Xpad turned off (slot 8): it
    ; publishes no block then, so a cookie would point at empty window.
    tst.b   XPAD_FLAG_ADDR
    beq.s   .uf_hook
    bsr     req_handshake
    bsr     install_xpad_cookie
.uf_hook:

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
    bra.s   .uf_exit

.uf_cookie_only:
    ; No RAM block, so no hook and nowhere for a request area, but the block
    ; itself is in the cartridge window and still worth publishing: req stays
    ; NULL, so no consumer is offered rumble.
    tst.b   XPAD_FLAG_ADDR
    beq.s   .uf_exit
    bsr     install_xpad_cookie

.uf_exit:
    movem.l (sp)+, d0-d4/a0-a3
    rts

; -----------------------------------------------------------------------
; Tell the RP where the XPAD_REQ area in the RAM block is, and wait until
; the block's req pointer says so: the RP writes it from the command, and
; seeing it there is the only acknowledgement there is. On success the
; resident starts forwarding rumble (RQ_ON); on a timeout it never does,
; and req stays as the RP last had it.
;
; Waits a frame at a time on _frclock, resending every HS_RESEND frames in
; case one was lost, for at most HS_FRAMES frames. Each frame's wait is
; also capped (HS_SPIN), so a stopped VBL can slow the boot down but never
; hang it. In: a3 = RAM block. Clobbers d0-d4/a0-a1.
; -----------------------------------------------------------------------
req_handshake:
    move.l  a3, d4
    add.l   #(res_req-resident_start), d4   ; d4 = our XPAD_REQ's address
    lea     (res_ad_frame-resident_start+8)(a3), a1
    move.w  d4, (a1)                ; payload: low word first,
    swap    d4
    move.w  d4, 2(a1)               ; then the high word
    swap    d4
    moveq   #0, d3                  ; frames waited
.hs_send:
    lea     (res_ad_frame-resident_start)(a3), a1
    bsr     res_send                ; the ROM copy: it only reads, so fine
.hs_wait:
    move.l  FRCLOCK.w, d0
    move.w  #HS_SPIN, d1
.hs_vbl:
    cmp.l   FRCLOCK.w, d0
    dbne    d1, .hs_vbl             ; until the next VBL, or the cap
    cmp.l   XPAD_REQ_PTR_ADDR, d4
    beq.s   .hs_ok                  ; the RP has it
    addq.w  #1, d3
    cmp.w   #HS_FRAMES, d3
    bhs.s   .hs_done                ; give up: no rumble this session
    move.w  d3, d0
    and.w   #(HS_RESEND-1), d0
    beq.s   .hs_send
    bra.s   .hs_wait
.hs_ok:
    move.b  #1, (res_rq-resident_start+RQ_ON)(a3)
.hs_done:
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
; VBL autovector ($70) entry: fires every vsync. Inject, then chain.
resident_start:
    ; Save the FULL register set, not just our scratch: we jsr into the system
    ; joyvec/mousevec, and mousevec (line-A / AES cursor machinery) can clobber
    ; d3-d7/a3-a6. Restoring only d0-d2/a0-a2 would leak that corruption back into
    ; the interrupted program -> intermittent bombs.
    movem.l d0-d7/a0-a6, -(sp)
    bsr     do_inject
    bra     res_chain

    even
; etv_timer ($400) entry: TOS calls this at 50 Hz, from every fourth Timer C
; tick. Inject, then fall through to the chain.
resident_etv_start:
    movem.l d0-d7/a0-a6, -(sp)
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
    bsr     do_rumble
    rts

    even
; do_rumble — forward the request area to the RP. Sends the whole state, all
; four pads, never a change, because the RP holds one pending command and can
; drop a frame: the next one must be enough on its own. Sends when seq moves
; (at most every RQ_MIN_GAP, longer than the RP's 20 ms poll, so two
; different frames cannot land in one poll and lose the second), and every
; RQ_KEEPALIVE while any motor is on, plus once more after they all go off in
; case that stop was the frame that got lost. Nothing at all until a consumer
; first bumps seq, so the terminal and the desktop see no ROM3 traffic from
; here. Free to clobber registers: do_inject's callers saved them all.
do_rumble:
    lea     res_rq(pc), a2
    tst.b   RQ_ON(a2)
    beq.s   .dr_done                ; no request area confirmed: nothing to do
    move.l  HZ200.w, d4             ; now
    move.l  d4, d5
    sub.l   RQ_LAST(a2), d5         ; 5 ms ticks since the last frame
    lea     res_req(pc), a3
    move.b  2(a3), d3               ; req.seq, read before the levels
    cmp.b   RQ_SEEN(a2), d3
    beq.s   .dr_keepalive
    cmp.l   #RQ_MIN_GAP, d5
    bhs.s   .dr_send                ; changed, and far enough from the last
.dr_keepalive:
    cmp.l   #RQ_KEEPALIVE, d5
    blo.s   .dr_done                ; not due yet
    tst.b   RQ_NZ(a2)
    bne.s   .dr_send                ; a motor is on: keep it on
    tst.b   RQ_QUIET(a2)
    beq.s   .dr_done                ; all off, and the stop already repeated
.dr_send:
    move.b  d3, RQ_SEEN(a2)
    move.l  d4, RQ_LAST(a2)
    lea     4(a3), a0               ; req.rumble[0][0]
    lea     res_rq_frame+8(pc), a1  ; the frame's four payload words
    moveq   #0, d0
    move.l  (a0)+, d1               ; pads 0 and 1
    move.l  d1, (a1)+
    or.l    d1, d0
    move.l  (a0), d1                ; pads 2 and 3
    move.l  d1, (a1)
    or.l    d1, d0
    sne     d0                      ; d0.b = $FF if any motor is on
    move.b  RQ_NZ(a2), d1           ; what the last frame said
    move.b  d0, RQ_NZ(a2)
    not.b   d0
    and.b   d0, d1                  ; on before, all off now: owe one repeat
    move.b  d1, RQ_QUIET(a2)
    lea     res_rq_frame(pc), a1
    bsr.s   res_send
.dr_done:
    rts

    even
; res_send — one command to the RP over ROM3: header, command, size, the
; payload words, then the checksum (command + size + every payload word).
; Each word is the low 16 bits of a read's address, which the RP's PIO
; captures; the read itself returns nothing of interest. Nothing is waited
; for. Position independent, and only reads memory, so the installer can
; call the copy in ROM. In: a1 -> command.w, size.w, then size/2 words.
; Clobbers d0-d2/a0-a1.
res_send:
    move.l  #ROM3_CMD_ADDR, a0
    move.w  #CMD_HEADER, d0
    tst.b   (a0,d0.w)               ; header, not in the checksum
    move.w  (a1)+, d0               ; command
    move.w  d0, d2
    tst.b   (a0,d0.w)
    move.w  (a1)+, d0               ; payload size in bytes, token included
    add.w   d0, d2
    tst.b   (a0,d0.w)
    move.w  d0, d1
    lsr.w   #1, d1
    subq.w  #1, d1                  ; words - 1, for dbf
.rs_word:
    move.w  (a1)+, d0
    add.w   d0, d2
    tst.b   (a0,d0.w)
    dbf     d1, .rs_word
    tst.b   (a0,d2.w)               ; checksum
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
; The Xpad request area consumers write, found through the block's req
; pointer once the RP has it. 16 bytes, laid out as xpad.h's XPAD_REQ, which
; holds no pointers so is the same on every machine (asserted in xpadstate.c).
res_req:
    dc.w    16                      ; size: xpad_req() refuses anything smaller
    dc.b    0, 0                    ; seq, reserved
    dc.b    0,0, 0,0, 0,0, 0,0      ; rumble[4][2]: low, high frequency motor
    dc.b    0,0,0,0                 ; led[4]: not honoured, no XPAD_CAP_LED
    even
res_rq_frame:
    dc.w    APP_XPAD_RUMBLE, 12, 0, 0   ; command, size, token (unused)
    dc.w    0, 0, 0, 0                  ; pads 0-3, filled in by do_rumble
    even
res_ad_frame:
    dc.w    APP_XPAD_REQ_ADDR, 8, 0, 0  ; command, size, token (unused)
    dc.w    0, 0                        ; address, filled in by req_handshake
    even
; do_rumble's state, at these offsets from res_rq.
RQ_ON               equ 0           ; byte: the RP has our request area
RQ_SEEN             equ 1           ; byte: req.seq at the last frame
RQ_NZ               equ 2           ; byte: the last frame had a motor on ($FF)
RQ_QUIET            equ 3           ; byte: an all-off frame owes a repeat
RQ_LAST             equ 4           ; long: _hz_200 at the last frame
res_rq:
    dc.b    0, 0, 0, 0
    dc.l    0
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
