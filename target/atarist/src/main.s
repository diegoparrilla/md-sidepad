;
; Copyright (C) 2026 Neil Rackett
; SPDX-License-Identifier: GPL-3.0-or-later
;

; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .	 
; bit 30: TOS application .
; bit 31: TTP

ROM4_ADDR			equ $FA0000

; Shared 64 KB region layout (must match rp/src/include/chandler.h).
;
;   $FA0000  CARTRIDGE			m68k header + code (max 8 KB)
;   $FA2000  CMD_MAGIC_SENTINEL_ADDR	4 B
;   $FA2004  RANDOM_TOKEN_ADDR		4 B
;   $FA2008  RANDOM_TOKEN_SEED_ADDR	4 B
;   $FA200C  reserved			4 B
;   $FA2010  SHARED_VARIABLES		240 B (60 x 4-byte slots)
;   $FA2100  APP_BUFFERS_ADDR	       ~40 KB free arena (first 512 B reserved)
;   $FAC180  FRAMEBUFFER_ADDR		16000 B (640x200 mono, at the top)
;   $FAFFFF  end of region

CARTRIDGE_CODE_SIZE	equ $2000	; 8 KB max for cartridge header + code
SHARED_BLOCK_ADDR	equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)		; $FA2000
CMD_MAGIC_SENTINEL_ADDR	equ SHARED_BLOCK_ADDR				; $FA2000

FRAMEBUFFER_SIZE	equ 16000	; 16000 bytes of a 640x200 monochrome screen
FRAMEBUFFER_ADDR	equ (ROM4_ADDR + $10000 - FRAMEBUFFER_SIZE)	; $FAC180
APP_BUFFERS_ADDR	equ (SHARED_BLOCK_ADDR + $100)			; $FA2100

; User firmware entry point. The cartridge image places userfw.s at
; offset $0800 of BOOT.BIN via target/atarist/src/userfw.ld; main.s
; gets the first 2 KB ($0000..$07FF), userfw gets the next 6 KB
; ($0800..$1FFF). The CARTRIDGE_CODE_SIZE = 8 KB cap covers both.
USERFW			equ (ROM4_ADDR + $800)				; $FA0800

SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code
COLS_HIGH			equ 40		; 16-pixel words in a 640-pixel row
ROWS_HIGH			equ 200		; framebuffer rows, each shown twice in high res
BYTES_ROW_HIGH		equ 80		; bytes in a high res screen row
PRE_RESET_WAIT		equ $FFFFF

; If 1, the display will not use the framebuffer and will write directly to the
; display memory. This is useful to reduce the memory usage in the rp2040
; When not using the framebuffer, the endianness swap must be done in the atari ST
DISPLAY_BYPASS_FRAMEBUFFER 	equ 1

CMD_NOP				equ 0		; No operation command
CMD_RESET			equ 1		; Reset command
CMD_BOOT_GEM		equ 2		; Boot GEM command
CMD_TERMINAL		equ 3		; Terminal command
CMD_START			equ 4		; Hand control to the user firmware (USERFW)

_conterm			equ $484	; Conterm device number


; Constants needed for the commands
RANDOM_TOKEN_ADDR:        equ (CMD_MAGIC_SENTINEL_ADDR + 4)  ; $FA2004
RANDOM_TOKEN_SEED_ADDR:   equ (RANDOM_TOKEN_ADDR + 4)        ; $FA2008
; $FA200C: 4-byte slot reserved for future framework use. chandler_init
; zeroes it at boot; apps must not write here.
RESERVED_SLOT_ADDR:       equ (RANDOM_TOKEN_SEED_ADDR + 4)   ; $FA200C
RANDOM_TOKEN_POST_WAIT:   equ $1                             ; Wait cycles after the RNG is ready
COMMAND_TIMEOUT           equ $0000FFFF                      ; Timeout for the command
COMMAND_WRITE_TIMEOUT     equ COMMAND_TIMEOUT                ; Timeout for write commands

SHARED_VARIABLES:         equ (RESERVED_SLOT_ADDR + 4)       ; $FA2010 (60 indexed 4-byte slots)

ROMCMD_START_ADDR:        equ $FB0000					  ; We are going to use ROM3 address
CMD_MAGIC_NUMBER    	  equ ($ABCD) 					  ; Magic number header to identify a command
CMD_RETRIES_COUNT	  	  equ 3							  ; Number of retries for the command
CMD_SET_SHARED_VAR		  equ 1							  ; This is a fake command to set the shared variables
														  ; Used to store the system settings
; App commands for the terminal
APP_TERMINAL 				equ $0 ; The terminal app

; App terminal commands
APP_TERMINAL_START   		equ $0 ; Start terminal command
APP_TERMINAL_KEYSTROKE 		equ $1 ; Keystroke command

_dskbufp                equ $4c6                            ; Address of the disk buffer pointer    


	include inc/sidecart_macros.s
	include inc/tos.s



; Macros
; XBIOS Vsync wait
vsync_wait          macro
					move.w #37,-(sp)
					trap #14
					addq.l #2,sp
                    endm    

; XBIOS GetRez
; Return the current screen resolution in D0
get_rez				macro
					move.w #4,-(sp)
					trap #14
					addq.l #2,sp
					endm

; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

; Check the left or right shift key. If pressed, exit.
check_shift_keys	macro
					move.w #-1, -(sp)			; Read all key status
					move.w #$b, -(sp)			; BIOS Get shift key status
					trap #13
					addq.l #4,sp

					btst #1,d0					; Left shift skip and boot GEM
					bne boot_gem

					btst #0,d0					; Right shift skip and boot GEM
					bne boot_gem

					endm

; Check the keys pressed
check_keys			macro

					gemdos	Cconis,2		; Check if a key is pressed
					tst.l d0
					beq .\@no_key

					gemdos	Cnecin,2		; Read the key pressed

					cmp.b #27, d0		; Check if the key is ESC
					beq .\@esc_key	; If it is, send terminal command

					move.l d0, d3
					send_sync APP_TERMINAL_KEYSTROKE, 4

					bra .\@no_key
.\@esc_key:
					send_sync APP_TERMINAL_START, 0

.\@no_key:

					endm

check_commands		macro
					move.l CMD_MAGIC_SENTINEL_ADDR, d6	; Store in the D6 register the remote command value
					cmp.l #CMD_TERMINAL, d6		; Check if the command is a terminal command
					bne.s .\@check_reset

					; Check the keys for the terminal emulation
					check_keys
					bra .\@bypass
.\@check_reset:
					cmp.l #CMD_RESET, d6		; Check if the command is a reset
					beq .reset					; If it is, reset the computer
					cmp.l #CMD_BOOT_GEM, d6		; Check if the command is to boot GEM
					beq boot_gem				; If it is, boot GEM
					cmp.l #CMD_START, d6		; Check if the command hands over to USERFW
					bne.s .\@no_start			; If not, fall through to the NOP/keys path
					bsr rom_function			; Call USERFW (returns after installing hooks)
					bra .\@bypass
.\@no_start:
					; If we are here, the command is a NOP
					; If the command is a NOP, check the shift keys to bypass the command
					; check_shift_keys
					check_keys
.\@bypass:
					endm

	section

;Rom cartridge
; The cartridge image (header + code below) MUST fit in
; CARTRIDGE_CODE_SIZE = $2000 (8 KB). The hard limit is enforced by
; target/atarist/build.sh after vlink emits BOOT.BIN; any direct vasm /
; vlink invocation that bypasses the build script is unchecked, so keep
; an eye on BOOT.BIN's size when iterating outside ./build.sh.

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l 0
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "TERM",0
    even

pre_auto:
; Relocate the content of the cartridge ROM to the RAM

; Get the screen memory address to display
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Move to the work area just after the screen memory
	move.l a2, a3				; Save the relocation destination address in A3
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	jmp (a3)

start_rom_code:
; We assume the screen memory address is in D0 after the get_screen_base call
	move.l d0, a6				; Save the screen memory address in A6

; Enable bconin to return shift key status
	or.b #%1000, _conterm.w

; If the RP already wants GEM on entry (the sentinel stays latched at
; CMD_BOOT_GEM across an ST reset, since the RP never rewrites it), go straight
; to boot_gem before the print loop copies the framebuffer. Otherwise the stale
; terminal/visualiser frame still in the framebuffer flashes up for a frame or
; two before boot_gem clears it. On a cold boot the sentinel is CMD_TERMINAL /
; CMD_NOP, so this falls through to the terminal as normal.
	move.l CMD_MAGIC_SENTINEL_ADDR, d6
	cmp.l #CMD_BOOT_GEM, d6
	beq boot_gem

; Get the resolution of the screen. The terminal is 640x200: a mono
; monitor shows it in high resolution with every line doubled, and a colour
; one in medium resolution, which the terminal switches to here.
	get_rez
	cmp.w #2, d0				; Check if the resolution is 640x400 (high resolution)
	beq .print_loop_high		; If it is, print the message in high resolution

; Colour. Remember the resolution TOS chose, and medium's foreground colour
; (3), so boot_gem can put both back before GEM starts. Colour 3 takes
; colour 15's value, low resolution's foreground, so the text is the colour
; it always was. XBIOS leaves a6 alone.
	lea saved_rez(pc), a0
	move.w d0, (a0)
	move.w #1, -(sp)			; medium resolution
	move.l #-1, -(sp)			; physical screen unchanged
	move.l #-1, -(sp)			; logical screen unchanged
	move.w #5, -(sp)			; XBIOS Setscreen
	trap #14
	lea 12(sp), sp
	move.w #-1, -(sp)			; read colour 15, change nothing
	move.w #15, -(sp)
	move.w #7, -(sp)			; XBIOS Setcolor
	trap #14
	addq.l #6, sp
	move.w d0, -(sp)			; colour 3 = colour 15
	move.w #3, -(sp)
	move.w #7, -(sp)
	trap #14
	addq.l #6, sp
	lea saved_color3(pc), a0	; d0 = colour 3 as it was
	move.w d0, (a0)

.print_loop_med:
	vsync_wait

; We must move from the cartridge ROM to the screen memory to display the messages.
; Medium resolution interleaves two planes a word at a time, so each
; framebuffer word goes to both: colour 3 where it is set, 0 where it is not.
	move.l a6, a0				; Set the screen memory address in a0
	move.l #FRAMEBUFFER_ADDR, a1			; Set the cartridge ROM address in a1
	move.l #((FRAMEBUFFER_SIZE / 2) -1), d0			; Set the number of words to copy
.copy_screen_med:
	move.w (a1)+ , d1			; Copy a word from the cartridge ROM
	ifne DISPLAY_BYPASS_FRAMEBUFFER == 1
	rol.w #8, d1				; swap high and low bytes
	endif
	move.w d1, d2				; Copy the word to d2
	swap d2						; Swap the bytes
	move.w d1, d2				; Copy the word to d2
	move.l d2, (a0)+			; Both planes of one 16-pixel group
	dbf d0, .copy_screen_med    ; Loop until all the message is copied

; Check the different commands and the keyboard
	check_commands

	bra .print_loop_med		; Continue printing the message

.print_loop_high:
	vsync_wait

; We must move from the cartridge ROM to the screen memory to display the messages.
; High resolution is 640 wide already, so each framebuffer row is written
; twice, to an even screen line (a1) and the odd one below it (a2).
	move.l a6, a1				; Set the screen memory address in a1
	move.l a6, a2
	lea BYTES_ROW_HIGH(a2), a2	; Move to the next line in the screen
	move.l #FRAMEBUFFER_ADDR, a0		; Set the cartridge ROM address in a0
	move.l #(ROWS_HIGH -1), d0	; Set the number of rows to copy - 1
.copy_screen_row_high:
	move.l #(COLS_HIGH -1), d1	; Set the number of columns to copy - 1 
.copy_screen_col_high:
	move.w (a0)+ , d2			; Copy a word from the cartridge ROM

	ifne DISPLAY_BYPASS_FRAMEBUFFER == 1
	rol.w #8, d2				; swap high and low bytes
	endif

	move.w d2, (a1)+			; Copy the word to the even line
	move.w d2, (a2)+			; and to the odd line below it

	dbf d1, .copy_screen_col_high   ; Loop until all the message is copied

	lea BYTES_ROW_HIGH(a1), a1	; Move to the next line in the screen
	lea BYTES_ROW_HIGH(a2), a2	; Move to the next line in the screen

	dbf d0, .copy_screen_row_high   ; Loop until all the message is copied

; Check the different commands and the keyboard
	check_commands

	bra .print_loop_high		; Continue printing the message
	
.reset:
    move.l #PRE_RESET_WAIT, d6
.wait_me:
    subq.l #1, d6           ; Decrement the outer loop
    bne.s .wait_me          ; Wait for the timeout

	clr.l $420.w			; Invalidate memory system variables
	clr.l $43A.w
	clr.l $51A.w
	move.l $4.w, a0			; Now we can safely jump to the reset vector
	jmp (a0)
	nop

GEMDOS_Cconws		equ 9						; trap #1 -> print null-terminated string
EXIT_FLAG_ADDR		equ (SHARED_VARIABLES + (4 * 4) + 3)	; connected flag, LSB of slot 4 ($FA2023)
MOUSE_ENABLED_ADDR	equ (SHARED_VARIABLES + (5 * 4))		; mouse-mode flag, byte 0 of slot 5 ($FA2024)
JOY_ENABLED_ADDR	equ (SHARED_VARIABLES + (7 * 4) + 3)	; joystick flag, LSB of slot 7 ($FA202F)
XPAD_ENABLED_ADDR	equ (SHARED_VARIABLES + (8 * 4) + 3)	; Xpad flag, LSB of slot 8 ($FA2033)

boot_gem:
	; (Re)install the VBL injection hook before booting to the desktop. boot_gem
	; runs on EVERY boot -- including after an ST reset, where the sentinel is
	; still latched at CMD_BOOT_GEM so check_commands comes straight here and the
	; CMD_START path (which normally installs) never runs. rom_function is
	; idempotent (userfw_installed guard, freshly zero in the relocated RAM copy
	; after a reset), so this installs once per boot and is a no-op on the first
	; boot where CMD_START already installed. bsr/rts is stack-balanced, so the
	; final rts below still unwinds the print loop back to TOS.
	bsr	rom_function
	; Put back the resolution and colour 3 the terminal changed for medium
	; resolution, so the desktop comes up as TOS meant it to and the banner
	; below prints in it. saved_rez is -1 when nothing was changed: a mono
	; monitor, or a warm reset that came straight here.
	lea	saved_rez(pc), a0
	move.w	(a0), d0
	bmi.s	.bg_rez_kept
	move.w	d0, -(sp)
	move.l	#-1, -(sp)
	move.l	#-1, -(sp)
	move.w	#5, -(sp)				; XBIOS Setscreen
	trap	#14
	lea	12(sp), sp
	lea	saved_color3(pc), a0
	move.w	(a0), -(sp)
	move.w	#3, -(sp)
	move.w	#7, -(sp)				; XBIOS Setcolor
	trap	#14
	addq.l	#6, sp
.bg_rez_kept:
	; Print the exit banner via the VT52 console, then return to TOS to continue
	; booting to the GEM desktop. boot_gem is reached via a beq from
	; check_commands inside the print loop, at the same stack level as the
	; CA_INIT return address, so this rts unwinds the print loop -- printing here
	; (not from USERFW, which returns into the loop) means the framebuffer copy
	; stops right after, so the banner persists on screen.
	;
	; All banners (VT52 clear + fixed text) live here in ROM (read from the
	; cartridge, which is readable), so nothing is subject to the unreliable
	; shared-region string transfer; the RP supplies only the flags that
	; pick between them: connected (slot 4), mouse (slot 5), joystick
	; (slot 7) and Xpad (slot 8).
	;
	; The credit banner comes first, after the clear, as MD/Net and MD/DOOM
	; boot with: version.inc, generated by the Makefile from version.txt.
	; The connection message below starts with a blank line to set it off.
	lea	bg_clear(pc), a0
	bsr.s	.bg_write
	lea	banner_txt(pc), a0
	bsr.s	.bg_write
	tst.b	EXIT_FLAG_ADDR				; connected?
	beq.s	.bg_noconn

	; Connected: a fixed header, then the device list on the row below.
	; Each of the three flags contributes its own fragment, separated by
	; commas; d3 holds whether one has been printed yet, so the separator
	; only appears between items. Assembled here rather than picked from
	; eight fixed strings, which is where a whole-string table stops paying.
	; d3 is scratch in this file (check_keys uses it the same way) and GEMDOS
	; preserves d3-d7/a3-a6, so it survives the Cconws calls below.
	lea	bg_msg_head(pc), a0
	bsr.s	.bg_write
	moveq	#0, d3					; nothing printed yet
	tst.b	XPAD_ENABLED_ADDR
	beq.s	.bg_noxpad
	lea	bg_dev_xpad(pc), a0
	bsr.s	.bg_item
.bg_noxpad:
	tst.b	JOY_ENABLED_ADDR
	beq.s	.bg_nojoy
	lea	bg_dev_joy(pc), a0
	bsr.s	.bg_item
.bg_nojoy:
	tst.b	MOUSE_ENABLED_ADDR
	beq.s	.bg_nomouse
	lea	bg_dev_mouse(pc), a0
	bsr.s	.bg_item
.bg_nomouse:
	tst.w	d3
	bne.s	.bg_tail
	lea	bg_dev_none(pc), a0			; connected, but every output turned off
	bsr.s	.bg_write
.bg_tail:
	lea	bg_list_end(pc), a0			; close the list line, then a blank one
	bra.s	.bg_print
.bg_noconn:
	lea	bg_msg_noconn(pc), a0			; blank line, "MD/Sidepad not connected", blank line
.bg_print:
	bsr.s	.bg_write
	rts
; One device fragment, preceded by a separator when it is not the first.
.bg_item:
	tst.w	d3
	beq.s	.bg_item_first
	move.l	a0, -(sp)
	lea	bg_list_sep(pc), a0
	bsr.s	.bg_write
	move.l	(sp)+, a0
.bg_item_first:
	moveq	#1, d3
	bsr.s	.bg_write
	rts
.bg_write:
	move.l	a0, -(sp)
	move.w	#GEMDOS_Cconws, -(sp)
	trap	#1
	addq.l	#6, sp
	rts
bg_clear:
	dc.b	27,"E",0
	include	"version.inc"			; banner_txt: the credit banner
bg_msg_head:
	dc.b	13,10,"MD/Sidepad connected:",13,10,0
bg_dev_xpad:
	dc.b	"Xpad",0
bg_dev_joy:
	dc.b	"joystick",0
bg_dev_mouse:
	dc.b	"mouse",0
bg_dev_none:
	dc.b	"nothing (all inputs off)",0
bg_list_sep:
	dc.b	", ",0
bg_list_end:
	dc.b	13,10,13,10,0
bg_msg_noconn:
	dc.b	13,10,"MD/Sidepad not connected",13,10,13,10,0
	even

; Dispatcher for the user firmware module. Reached on CMD_START via the
; sentinel poll in check_commands. The cartridge image places userfw.s
; at offset $0800 (USERFW = $FA0800) through target/atarist/src/userfw.ld;
; main.s simply hands control over with a one-way jmp. Apps that want
; to chain multiple modules can change this to a sequence of jsr / jmp
; the same way md-drives-emulator's rom_function dispatches into
; GEMDRIVE/FLOPPYEMUL/ACSIEMUL/RTCEMUL.
rom_function:
    ; Run the userfw installer exactly once. The CMD_START sentinel stays set
    ; for several frames (the RP holds it, then switches back to CMD_TERMINAL),
    ; so guard against re-installing. The flag lives in this relocated-to-RAM
    ; code, reached PC-relative so the write lands in RAM (the cartridge ROM
    ; region is read-only to the ST); an absolute write would bus-error.
    lea     userfw_installed(pc), a0
    tst.l   (a0)
    bne.s   .rf_done
    move.l  #1, (a0)
    movem.l d0-d2/a0-a2, -(sp)
    jsr     USERFW              ; installer executes from ROM (fetch/reads OK)
    movem.l (sp)+, d0-d2/a0-a2
.rf_done:
    rts

userfw_installed:
    dc.l    0

; What the terminal changed for medium resolution, for boot_gem to put back.
; In the relocated RAM copy, so writable, and fresh on every boot.
saved_rez:
    dc.w    -1                  ; -1: resolution not changed
saved_color3:
    dc.w    0

; Shared functions included at the end of the file
; Don't forget to include the macros for the shared functions at the top of file
    include "inc/sidecart_functions.s"


end_rom_code:
end_pre_auto:
	even
	dc.l 0