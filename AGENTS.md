# AGENTS.md — MD/Sidepad

Guidance for any agent (Claude Code, etc.) working in this repo. `CLAUDE.md` just imports this file.

See also: `programming.md` (full shared-region table and budget rules), `README.md` (high-level region/userfw overview), and the sibling apps `md-drives-emulator`, `md-snap`, `md-devops`, `md-js` in the same parent folder — they are proven references for the cartridge/handoff patterns and were leaned on heavily while debugging.

## What this repo is

Template-derived **Sidecartridge Multi-device microfirmware app** targeting Atari ST / STE / MegaST(E). Each "app" is a UF2 image that runs on a Raspberry Pi Pico (RP2040) plugged into the Multi-device cartridge slot, emulating a ROM cartridge for the Atari while also handling networking, SD card I/O, and config. Public build/usage docs: <https://docs.sidecartridge.com/sidecartridge-multidevice/programming/>.

**MD/Sidepad** pairs up to four Bluetooth gamepads with the Atari ST, injects the first as **joystick 1** (optionally right-stick-as-mouse) and publishes all four, with rumble, as an Xpad block. The BLE host is Bluepad32 on the Pico W's CYW43 chip; WiFi is stripped because it shares that chip. UUID `7795c766-c5a8-4607-bc7d-01c55e03d300`; **`pico_w`-only** (CYW43 required for BLE).

## Environment setup

- **ARM GNU Toolchain** — export `PICO_TOOLCHAIN_PATH` to its `arm-none-eabi/bin` dir. The project historically pins **14.2**; the current dev box uses **15.2** (`/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin`) and it builds and runs cleanly. Both work — the last toolchain-sensitive serving bug was removed by the inline hook-publish fix (see the ETV serving lesson below). If you see `arm-none-eabi-gcc not found`, this var is wrong.
- **`atarist-toolkit-docker`** (`stcmd`) — needed for the m68k target. `stcmd` requires a PTY (`pty=true`). `target/atarist/build.sh` exports `STCMD_NO_TTY=1` for every stcmd call it makes; export it yourself only if invoking `stcmd` directly from a non-TTY context (CI, sub-shells).
- **Raspberry Pi Debug Probe / Picoprobe** for the serial console — TX, RX, and **both** GND pins must be connected. The debug UART is the main "verification" channel; the one host test is `make -C rp/test test`.
- **SDK paths** — auto-set from the repo if unset; to set explicitly:
  ```bash
  export PICO_SDK_PATH=$REPO_ROOT/pico-sdk
  export PICO_EXTRAS_PATH=$REPO_ROOT/pico-extras
  export FATFS_SDK_PATH=$REPO_ROOT/fatfs-sdk
  ```
- Optional debugger helpers: `export ARM_GDB_PATH=…/arm-none-eabi/bin`, `export PICO_OPENOCD_PATH=…/openocd/tcl`.

## Build

Top-level build is driven by `build.sh` in the repo root:

```bash
# <board_type> = pico | pico_w | sidecartos_16mb   (MD/Sidepad is pico_w only)
# <build_type> = debug | release   (note: always compiled as MinSizeRel — see below)
# <app_uuid_key> = UUID4 identifying this app, must match desc/app.json
./build.sh pico_w release 7795c766-c5a8-4607-bc7d-01c55e03d300
```

Build flow (orchestrated by `build.sh`):
1. Increments the patch version (`tools/bump_version.sh`) and copies `version.txt` into `rp/` and `target/atarist/`.
2. Builds the Atari ST target (`target/atarist/build.sh`) via `stcmd make`. Enforces an **8 KB hard limit** on `BOOT.BIN` (`CHANDLER_CARTRIDGE_CODE_SIZE` in `rp/src/include/chandler.h`, mirrored as `CARTRIDGE_CODE_SIZE` in `target/atarist/src/main.s`); over-budget aborts with `ERROR: cartridge code is N bytes; limit is 8192`. A separate copy (`FIRMWARE.IMG`) is padded to 64 KB to fill the shared region, and `firmware.py` converts it into `rp/src/include/target_firmware.h` (a C byte array embedded in the RP firmware).
3. Builds the RP firmware (`rp/build.sh`): pins submodule versions (pico-sdk 2.2.0, pico-extras sdk-2.2.0, fatfs-sdk at a specific commit), runs CMake, produces `rp/dist/rp-<board>.uf2`. FatFs config lives at `rp/src/ff/ffconf.h` and shadows the submodule's default via `target_include_directories(... BEFORE PRIVATE)` in `rp/src/CMakeLists.txt`, so the `fatfs-sdk` submodule stays pristine.
4. Computes MD5, renames to `dist/<APP_UUID>-<VERSION>.uf2`, and substitutes UUID/MD5/version into `dist/<APP_UUID>.json` from the `desc/app.json` template.

Successful builds drop UF2s into `dist/` and print the MD5 used in the generated JSON manifest. Verification = `make -C rp/test test` passes (the Xpad block writer's and the rumble decisions' host tests, also run by CI), the build succeeds, the UF2 boots on hardware, and behaviour is confirmed over the serial console.

### Build gotchas
- **CMake always builds with `-DCMAKE_BUILD_TYPE=MinSizeRel`** regardless of `<build_type>`. A full `Release` previously caused breakage (memory/over-optimization); the legacy line is left commented in `rp/build.sh`. `<build_type>` only controls the `DEBUG_MODE` macro and the dist filename.
- **`DEBUG_MODE` → `_DEBUG` in CMake:** `debug` builds keep `DPRINTF` (UART on); `release` compiles them out (silent). This was **broken** for a long time: `rp/src/CMakeLists.txt` had `if (NOT _DEBUG) set(_DEBUG 1)`, and CMake treats the string `"0"` as false, so `release` (DEBUG_MODE=0) was forced back to `_DEBUG=1` — **every** build shipped full UART logging. Fixed to `if (NOT DEFINED _DEBUG OR "${_DEBUG}" STREQUAL "")`. If a release build is ever chatty on UART again, look here first.
- **`firmware.py` odd-length trap:** it trims trailing zeros from the 64 KB image, then aborts with `ValueError` if the result is an **odd** number of bytes. `target/atarist/build.sh` now checks its exit code and fails hard (`exit 6`); before that, a failed run left the previous `rp/src/include/target_firmware.h` in place and the RP build silently embedded a **stale** cartridge → ST boots straight to desktop, no terminal. If you append `dc.b` data to the m68k image and hit this, pad `userfw.s` so the trimmed size stays even (a trailing `dc.w $FFFF` outside the copied block works — see the ETV `userfw.s`).
- `CHARACTER_GAP_MS` must remain defined (700) in `rp/src/include/blink.h` — removing it breaks the RP build.
- Harmless VASM warnings during the m68k build (`target data type overflow`, `trailing garbage after option -D`) can be ignored.
- `stcmd` without a PTY fails with `the input device is not a TTY`; without `STCMD_NO_TTY=1` the m68k build can fail **silently**, the previous `BOOT.BIN` survives, and the RP firmware serves a stale cartridge → garbage on the ST while commands still work. Confirm `target/atarist/dist/BOOT.BIN` was regenerated by the current build (compare timestamps in `dist/`).

### CI / release
- `.github/workflows/pr.yml` runs `make -C rp/test test` and then `make` on every pull request into `main`: the host tests and a build check, nothing else. The tests go first because they need only a C compiler, so a broken block layout fails in seconds rather than after both toolchains have installed. It has a read-only token and no `APP_UUID_KEY`, so the Makefile falls back to the sample UUID and the job still works for PRs from forks.
- `.github/workflows/release.yml` runs on every push to `main` (so a merge releases) and can also be dispatched by hand, refusing to run off `main` either way. It runs `make`, tags `version.txt`, force-moves the lightweight `latest` tag, and replaces the UF2 and JSON on the standing "latest" release with `gh release upload --clobber`. That is a rolling release: the release's title and body are never touched, so they stay hand-written.
- **Merging to `main` publishes.** Bump `version.txt` in the branch if the merge should carry a new version number; merging without a bump re-releases under the existing tag, which is left where it is, and only moves `latest`. Doc-only merges rebuild and republish too.
- The app UUID comes from the `APP_UUID_KEY` repository secret; `uuid.txt` is deliberately not committed, and the workflow fails on its first step if the secret is missing rather than quietly building under the sample UUID.
- `make tag` tags HEAD with the contents of `version.txt` and pushes the tag. It triggers nothing: only a push to `main` releases, and a tag push is not one. The release workflow does its own tagging anyway, so this is now just a manual convenience.
- `upload_s3.sh <file>` is a manual one-off uploader; needs `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY`.

## Architecture

Two-target build: m68k assembly runs on the Atari ST, is compiled into a ROM image, embedded as a C array inside the RP2040 firmware, and served back to the Atari over the cartridge bus that the RP2040 emulates via PIO + DMA.

### Atari ST side (`target/atarist/`)
- `src/main.s` — m68k cartridge boot + dispatch + terminal. Lives at `$FA0000` (ROM4 cartridge region). Defines the cartridge header (`CA_MAGIC`, `CA_INIT`, …), command magic numbers, and the shared-variable layout.
- `src/userfw.s` — **the primary extension point for app-specific m68k code.** `src/userfw.ld` places `main.s` at offset `0x0000` (2 KB budget) and `userfw.s` at `0x0800` (6 KB budget); `main.s` exposes the latter as `USERFW equ (ROM4_ADDR + $800)`. When the RP raises `CMD_START = 4` on the cartridge sentinel, the m68k's vsync-polled `check_commands` dispatches to `rom_function`, which `jmp`s to `USERFW`.
- Adding more m68k modules: add a `.text_<name>` section in `userfw.ld`, mirror the offset with an `equ (ROM4_ADDR + $????)` in `main.s`, and add the `.o` target to `target/atarist/Makefile` (like `gemdrive.ld` in `md-drives-emulator`).
- The cartridge image (header + all `.text_*` sections) must fit in 8 KB; a 64 KB padded copy becomes `target_firmware.h`.

### Shared 64 KB cartridge region
The Atari sees a 64 KB window `$FA0000`–`$FAFFFF`, mirrored RP-side at **`0x20030000`** (the `ROM_IN_RAM` base). **Single source of truth** for cross-target layout — both sides derive every offset symbolically from `rp/src/include/chandler.h` (RP) and `target/atarist/src/main.s` (m68k). **Never hard-code an address inside this region.**

| Offset | Symbol | Size | Purpose |
| --- | --- | --- | --- |
| `$FA0000` | cartridge image | 8 KB | m68k header + all `.text_*` sections (hard limit) |
| `$FA2000` | `CMD_MAGIC_SENTINEL` | 4 B | m68k polls here for NOP/RESET/BOOT_GEM/START/TERMINAL words |
| `$FA2010` | 60 × 4 B indexed shared variables (`SHARED_VARIABLES`) | 240 B | after `RANDOM_TOKEN`/`SEED`/reserved at `$FA2004`–`$FA200F` |
| `$FA2100` | `APP_BUFFERS` / `APP_FREE` | ~40 KB | app arena (first 512 B reserved, once the high-res mask table) |
| `$FAC180` | `FRAMEBUFFER` | 16000 B | 640×200 mono; at the top so an overrun walks off the 64 KB end |

See `programming.md` for the full table and budget rules.

### RP2040 side (`rp/src/`)
- `main.c` — sets clock/voltage, single-sample SELECT-at-boot → Booster hatch, `gconfig_init` (global) then `aconfig_init` (per-app), hands off to `emul_start()`. Config-init failure jumps to Booster via `reset_jump_to_booster()`. **Don't add features to `main.c`** — put them in `emul.c`. (Early-boot recovery hatches like the SELECT check are the deliberate exception; keep them single-sample, no loops/delays.)
- `emul.c` / `emul.h` — the app's main loop and entry point; add features here.
- `romemul.c` / `romemul.pio` — PIO + chained-DMA runtime that serves the cartridge ROM bus autonomously (no CPU/IRQ). The serving base is `&__rom_in_ram_start__ >> 16` pushed to the PIO once; `__rom_in_ram_start__` **must** be 64 KB-aligned (it is, at `0x20030000`).
- `commemul.c` — ROM3 command-receive channel (m68k→RP). Needs `commemul_init()` once at startup (in `emul_start()` after `chandler_init()`). No `Keystroke:` logs → check this first. commemul + romemul share **pio0**; commemul's ring is `__attribute__((aligned(COMM_RING_SIZE_BYTES)))`.
- `gconfig.c` / `aconfig.c` — global vs per-app config in dedicated flash sectors, on top of `settings/`.
- `network.c`, `httpc/`, `download.c` — Wi-Fi/BLE (CYW43, lwIP poll mode), HTTP client, firmware download.
- `sdcard.c`, `hw_config.c` — FatFs over SPI/SDIO.
- `display.c`, `display_term.c`, `term.c`, `u8g2/` — terminal rendered into the Atari framebuffer and/or a local OLED.
- `blink.c`, `select.c`, `reset.c`, `tprotocol.c`, `chandler.c` — LED Morse status, SELECT button, soft reset/jump-to-booster, transport primitives, command handler + shared-var access.

### Memory layout (`rp/src/memmap_rp.ld`)
The RP2040's 2 MB flash and 264 KB SRAM are sliced into named regions; code must not stomp on them.

| Region | Origin | Length | Purpose |
| --- | --- | --- | --- |
| `FLASH` | `0x10000000` | 1024 K | App code |
| `ROM_TEMP` | `0x10100000` | 128 K | Scratch for loaded ROMs |
| `BOOSTER_APP_FLASH` | `0x10120000` | 768 K | Reserved for Booster (do not write) |
| `CONFIG_FLASH` | `0x101E0000` | 120 K | 30 sectors of per-app config |
| `GLOBAL_LOOKUP_FLASH` | `0x101FE000` | 4 K | UUID → config-sector lookup |
| `GLOBAL_CONFIG_FLASH` | `0x101FF000` | 4 K | Global config |
| `RAM` | `0x20000000` | **192 K** | .data/.bss/heap (reclaims the old upper 64 KB) |
| `ROM_IN_RAM` | **`0x20030000`** | **64 K** | cartridge mirror served to the Atari (**not** `0x20020000` — older docs were wrong) |
| `SCRATCH_X` / `SCRATCH_Y` | `0x20040000` / `0x20041000` | 4 K each | core1 / core0 stacks |

Core 0 owns flash writes (`PICO_FLASH_ASSUME_CORE0_SAFE=1`) and overclocks to 225 MHz at `VREG_VOLTAGE_1_10`. **Caveat:** `__StackLimit` (the heap ceiling) is computed as `ORIGIN(RAM)+LENGTH(RAM)+LENGTH(ROM_IN_RAM)` = `0x20040000`, i.e. the heap is *allowed* to grow through the cartridge mirror. Same as md-snap; md-devops caps it at `ORIGIN(RAM)+LENGTH(RAM)`. It was investigated and ruled out as the cause of the ETV debug-serving bug, but it's a latent fragility if heap use ever gets heavy.

### App identity
`CURRENT_APP_UUID_KEY` (from `APP_UUID_KEY` at CMake time) must match `uuid` in `desc/app.json` and keys into `GLOBAL_LOOKUP_FLASH` to find this app's config sector. Mismatch → jump to Booster.

## Xpad

`xpad` is a **submodule** at the repository root, not a vendored copy:
`rp/src/xpadstate.c` writes that ABI and must not drift from what
consumers are compiled against. Do not edit anything under `xpad/`;
change it upstream in atarist-xpad and bump.

    git submodule update --remote xpad

Two things it is worth knowing before changing anything here.

`xpadstate.c` carries `_Static_assert`s tying its own offsets to the
header, so a bump that moves a field fails the build rather than
producing a block nobody agrees with. Keep them. Two of them guard
`userfw.s` too: `OFF_REQ == 22` (its `XPAD_REQ_PTR_ADDR`) and the
`XPAD_REQ` layout (its hand-laid `res_req`).

`target/atarist/src/userfw.s` includes xpad's generated `xpad.inc`
directly. That works because `target/atarist/build.sh` mounts the
repository root (`stcmd` mounts exactly one directory) and runs make in
the subdirectory, with `-I../../xpad/src` in the Makefile; build.sh
refuses with a clear message if the submodule is missing. The one Xpad
number still spelled out by hand is `XPAD_BLOCK equ $FA2300`, the
block's home in the cartridge window, which is this product's choice
rather than xpad's and is pinned on the RP side by a `_Static_assert`
against `CHANDLER_APP_FREE_OFFSET`. `XPAD_REQ_PTR_ADDR` is derived from
it and `XPAD_HDR_FIXED`, because `xpad.inc` deliberately stops before
the pointer fields.

## MD/Sidepad specifics

The pieces below are hard-won and non-obvious — read before touching the command path, joystick injection, or the booster/desktop exits.

### Keymap (single-key, `sidepadInputCb`)
`ESC` = exit to GEM desktop (installs the joystick hook on the way out), `R` = reset: disconnect every pad and forget every bond (new pads can pair at any time without it; it was `P` for Pair before pairing became always open), `M` = toggle right-stick-as-mouse, `J` = toggle joystick injection, `X` = toggle Xpad (block + cookie), **`H` = toggle VBL (`$70`) / ETV (`$400`) hook mode**, `B` = exit to Booster (full chip reset). The dedicated ST **Help** key toggles a help screen; it carries no useful ASCII, so `sidepadInputCb` dispatches it from the scan code (`SCAN_HELP = 0x62`) in bits 16–23 of the keystroke payload (`TERM_KEYBOARD_SCAN_MASK`). The template terminal is **line-based** (`term_command_cb` buffers until Enter), so MD/Sidepad registers its **own** `chandler_addCB(sidepadInputCb)` to act on the first keypress. `APP_TERMINAL_START` = ESC, `APP_TERMINAL_KEYSTROKE` = any other key. `H`/`M`/`J`/`X` persist via per-app config (`ACONFIG_PARAM_HOOK` / `ACONFIG_PARAM_MOUSE` / `ACONFIG_PARAM_JOYSTICK` / `ACONFIG_PARAM_XPAD`; joystick and Xpad default on when the key is absent), saved in `exitToGemDesktop`.

### Joystick injection (`target/atarist/src/userfw.s`)
- Hook the **VBL autovector `$70`**, *not* a `_vblqueue` slot — GEM reclaims free `_vblqueue` slots when the desktop loads, silently de-linking the hook. `$70` is the OS VBL entry; GEM only adds/removes `_vblqueue` entries (which the `$70` handler walks), so a hook there survives into the desktop. Chain to the saved original.
- **VBL vs ETV (selectable, implemented and hardware-verified):** two resident entry points share one `do_inject` routine and the same `push orig_hook; rts` chain (valid for the `$70` autovector — orig rte's the interrupt frame — **and** the `$400` etv_timer subroutine — orig rts's back to TOS' MFP ISR which rte's; confirmed against `md-devops` runner.s). The installer reads the hook-mode flag from shared-var **slot 6** (`HOOK_FLAG_ADDR = $FA202B`) and hooks `$70` or `resident_etv_start` at `$400`. `etv_timer` is called at **50 Hz** (every fourth tick of the 200 Hz Timer C, not every tick), the same as a PAL VBL, so the ETV entry injects on every call. It used to divide by four again, on the belief that `etv_timer` ran at 200 Hz, which left ETV mode, the default, injecting at 12.5 Hz; measured under TOS 1.62, TOS 2.06 and EmuTOS in Hatari, it is 50 Hz. Don't reintroduce a divider. **Default = ETV** (`ACONFIG_PARAM_HOOK` default `"true"`); `[H]` switches to VBL. ETV survives programs that replace the VBL vector, so it's the more compatible default (esp. games).
- The cartridge region is emulated **read-only**: fetches/reads work, **stores bus-error**. The resident handler keeps writable state (prev sample, packet buffer, saved joyvec, orig hook, rumble state) in RAM — the installer `Malloc`s a block (`GEMDOS_Malloc` 72) and copies a position-independent handler in. Reads of the shared region still work, so it reads the controller byte in place.
- Read **joyvec live each frame** from KBDVECS (`XBIOS Kbdvbase` 34, joyvec at **offset 24** — not 20). A reader typically installs its own joyvec *after* us; a snapshot would bypass it. Same live-read for mousevec (offset 16).
- Synthesise a TOS joystick packet `[$FF, joy0, joy1]` with the `$FF` header at an **odd** address (consumers read `move.w 1(a0)`). Our state → **joy1**. IKBD encoding: bit0=Up, bit1=Down, bit2=Left, bit3=Right, **bit7=Fire**. The RP packs fire into bit6; userfw moves it to bit7.

### The terminal is 640x200 (`display.c`, `term.h`, `main.s`, `emul.c`)
- The framebuffer is 640×200, one bit a pixel, 80 columns of 8×8 text. `main.s` shows it in **medium resolution** on a colour monitor and **high resolution** on a mono one. Medium writes each framebuffer word to both planes (colour 3). High res writes each row twice, and needs no pixel doubling, so the high-res mask table is gone.
- Switching to medium changes TOS's state, so `main.s` saves the resolution and palette colour 3 in its relocated RAM copy (`saved_rez`, `saved_color3`). It sets colour 3 to colour 15's value (low res's foreground). `boot_gem` puts both back before the banner and GEM. `saved_rez` stays -1 on mono, or after a warm reset that skips the terminal, and nothing is restored then.
- A medium-res cell is twice as tall as it is wide, so the visualiser is drawn twice as many cells wide as tall to stay square: an 18×9 box with a 6×3 blob. Anything new in that area should follow suit.
- The full copy is 8000 words through a `rol.w #8` byte swap. That is about 64 ms on an 8 MHz ST, so the terminal redraws at about 15 Hz, which is faster than the RP's 100 ms UI refresh.
- `renderControllerScreen`'s buffers (`currentLines`, `updates`, 4 KB) are **static**: at 80 columns they are about 6 KB, too much for core 0's 4 KB stack. That is safe because a refresh never runs inside another.
- `main.s` uses about 1894 of its 2048 bytes.
- `make emu` shows colour; add `-- --monitor mono` to `mdfw run` for mono.

### Multiple pads (`controller.c`, `xpadstate.c`)
- A pad's **slot is its Bluepad32 device index**: the first free one when it connects, kept while it stays connected. `CONFIG_BLUEPAD32_MAX_DEVICES` (`rp/src/bt/sdkconfig.h`), `CONTROLLER_MAX_PADS` and `XPADSTATE_PAD_COUNT` are all 4 and asserted equal. **Slot 0 alone** is joystick 1 and the mouse (`controller_getState()` is slot 0, so the injection, banner, auto-exit and visualiser code reads only it); every slot is an Xpad pad, and `xpadstate_publish()` writes all four every commit so no slot is ever two commits old. `pad_count` is immutable for a published block, so empty slots are there from the start as `XPAD_TYPE_NONE`.
- Pairing is always open: Bluepad32 scans from `on_init_complete` and never stops, so a pad in pairing mode joins whenever a slot is free, desktop included. `R` (Reset) disconnects every slot and deletes all keys.
- On ready, a pad's player LEDs are set to `1 << slot` where the driver has them.
- `X` (Xpad off) stays, deliberately, even though pads 2-4 and rumble only reach games through Xpad: with it off, MD/Sidepad leaves the cookie alone, so another Xpad provider (COMpad, atarist-xpad's example drivers) can own it. That is how those get tested on an ST with the Multi-device fitted. Don't remove it as redundant.
- `MAX_NR_L2CAP_CHANNELS` is 10, not the 6 Bluepad32's example ships: a Classic HID pad holds two channels, so 6 only fits three Classic pads.
- **RAM:** `g_devices` is ~23 KB for four devices. `.bss` now ends at `0x20027574`, ~35 KB below `ROM_IN_RAM` (was ~53 KB). The heap may legally grow into the cartridge mirror (see the `__StackLimit` caveat), so watch heap use.
- **Unverified risk:** BTstack's link-key TLV defaults to `PICO_FLASH_BANK_STORAGE_OFFSET` = the last 8 KB of flash (`0x101FE000`), which is exactly `GLOBAL_LOOKUP_FLASH` + `GLOBAL_CONFIG_FLASH` in `memmap_rp.ld`. Nothing overrides it. More bonds mean more writes there. After pairing four pads, power-cycle and check the app still boots; if not, the likely fix is a `PICO_FLASH_BANK_STORAGE_OFFSET` in an unused region, but decide that deliberately.

### Rumble (`userfw.s`, `rumble.h`/`rumble.c`, `emul.c`)
- The block is in the read-only window, so the **`XPAD_REQ` area is `res_req` in the resident's Malloc'd RAM block**, and only the RP can point the block's `req` at it. The installer (if the Xpad flag is set) sends `APP_XPAD_REQ_ADDR` with the address and polls `XPAD_REQ_PTR_ADDR` (`$FA2316`) until the RP has written it back, resending every 8 frames, giving up after ~50 frames (each frame's `_frclock` wait is spin-capped so boot can never hang). Only then does it install the cookie, then the hook. On a timeout rumble stays off for the session (`RQ_ON` clear). Malloc failure: cookie only, `req` NULL. A warm reset re-Mallocs and handshakes again.
- `xpadstate_setReq()` writes `req`, then claims `XPAD_CAP_RUMBLE`. Rumble is claimed whenever there is a request area, not only while a pad with motors is connected: consumers read caps once, pads hot-plug, and a pad without motors ignores it.
- **ROM3 commands from the resident**, ids clear of the terminal's 0/1, defined in both `emul.c` and `userfw.s`: `APP_XPAD_REQ_ADDR $5850` (payload after the zero token: address low word, high word) and `APP_XPAD_RUMBLE $5851` (four words, `rumble[i][0] << 8 | rumble[i][1]`, i.e. the big-endian words at `req+4`). Same framing and checksum as `send_sync` (`$ABCD`, cmd, size incl. token, words, cmd+size+Σwords), but sent by `res_send` in the resident, fire and forget: it runs in an interrupt, and nothing waits on the token echo. Nothing else issues ROM3 reads after `boot_gem`, so there is nothing to interleave with.
- **State, never events:** chandler holds one pending command and drops a *different* one that arrives meanwhile, so every rumble frame carries all four pads. `do_rumble` sends when `req.seq` moves, at most every 30 ms (`RQ_MIN_GAP`, so two different frames cannot share one 20 ms RP drain), every 200 ms (`RQ_KEEPALIVE`) while any motor is on, and once more after they all go off. Nothing at all until a consumer first bumps `seq`. Both are timed on `_hz_200`, never in hook ticks: the hook's rate depends on VBL/ETV mode and the display, and the RP's timeout is real time. Counting ticks is how the first version tripped its own safety stop under ETV (see below).
- **Safety stop:** the RP re-arms a held level as a 250 ms burst every 200 ms, but only while frames keep arriving: a pad with no frame for 600 ms is stopped once (`rumble_logic_tick`). That is what stops a motor when the ST resets or a game unhooks `$70`/`$400`. COMpad lacked it. `APP_XPAD_REQ_ADDR` also stops everything (a new session), and a disconnect forgets the slot's levels.
- **Both burst loops in `exitToGemDesktop()` call `chandler_loop()`**, because the installer's handshake runs inside the `CMD_START` burst. Don't remove them.
- The decisions are SDK-free in `rumble.h` and host tested by `rp/test/rumble_test.c`.

### RP→m68k handoff (`emul.c` exit path + `main.s`)
- **Inject only on exit-to-desktop, never during the terminal loop.** Installing the hook while the terminal is live races the VBL handler's cartridge-bus reads against the terminal's `send_sync` protocol and corrupts keystroke delivery.
- Exit sequence (`exitToGemDesktop`): publish the hook-mode flag (slot 6) **inline** first, then send `DISPLAY_COMMAND_START` (`CMD_START`=4) for several frames so the m68k installs the hook (idempotent), then `DISPLAY_COMMAND_CONTINUE` (`CMD_BOOT_GEM`=2) for several frames, then loop forever pumping `controller_poll` + `writeBtJoyState`. **Burst both commands** — a single sentinel write can race the m68k's vsync-paced `check_commands` and be dropped (symptom: firmware-load message flashes, then the terminal returns).
- `main.s` `check_commands`: `CMD_START` does `bne .no_start / bsr rom_function / bra bypass`. `rom_function` runs the installer **once** via a **PC-relative** `userfw_installed` guard — an absolute sentinel write here would bus-error. `boot_gem` is just `rts`; the cartridge runs as `CA_INIT` (header bit 27), the stack is balanced through `pre_auto`→relocation→print-loop, so `rts` returns to TOS and continues to the desktop. `main.s` also has an early CA_INIT `cmp #CMD_BOOT_GEM; beq boot_gem` so a warm ST reset (RP keeps the sentinel latched) goes straight to the desktop with the pad. `emul_start` **zeroes the sentinel before `init_romemul`** so a stale `CMD_BOOT_GEM` in SRAM can't make the first boot skip the terminal (`chandler_init` zeroes only the reserved slot, not the sentinel).

### Shared-variable byte order
`firmware.py` packs little-endian words and the cartridge bus per-word swap cancels at the word-**value** level, so `SET_SHARED_VAR(idx, V)` makes the m68k `move.l SHARED_VARIABLES+idx*4` read back `V`. Slots used: **3** = joystick byte (LSB at `$FA201F`), **4** = connected flag (`$FA2023`), **5** = mouse packet (`$FA2024`), **6** = VBL/ETV hook flag (`$FA202B`), **7** = joystick-enabled flag (`$FA202F`, exit banner only), **8** = Xpad-enabled flag (`$FA2033`, read by the userfw cookie installer and the exit banner). A direct inline store must match SET_SHARED_VAR's layout: low word at `slot+2`, high word at `slot+0`.

### "No terminal on boot" — disambiguate three distinct causes (healthy UART each)
1. **Desktop loads but broken/corrupt** (cursor jumps, unusable) → **mouse-flood**: `writeBtMouseState` must gate on `st.connected` (has regressed twice).
2. **Clean desktop, no terminal, cartridge appears absent** → **served too late**: TOS scans `$FA0000` very early; anything delaying `emul_start` before `init_romemul` (or heavy blocking pre-serve UART on debug builds) pushes serving past the scan window. Keep `main.c` and pre-`init_romemul` `emul_start` lean; the per-entry `aconfig` lookup dump and the `gconfig`/`aconfig` `settings_print(NULL)` dumps were removed for this reason. The CMake `_DEBUG` bug above made *every* build pay this UART cost.
3. **Clean desktop, no terminal, RAM mirror byte-perfect** → the **serving-path layout bug**: see below.

### The ETV serving lesson (function vs inline)
Publishing the hook-mode flag via a `static void writeHookMode()` **helper function** deterministically broke ROM serving on **debug** builds (ST boots to a clean GEM desktop, no terminal) while release builds were fine. Proven — over ~15 hardware images + UART probes — **not** to be timing (a build with *less* pre-serve logging still failed), heap growth (capping the heap didn't help), mirror corruption (probes: header + sentinel stay byte-perfect through the app loop), the sentinel, or the toolchain (**14.2 and 15.2 fail identically**). Root cause never fully explained — a binary-layout sensitivity on the autonomous PIO/DMA serving path. **The fix that works everywhere: publish the flag inline (direct `volatile uint16_t` stores) instead of via a single-use function.** If a tiny addition to `emul.c` ever breaks serving in a debug build again, suspect this class and prefer inlining over a new helper. (Full write-up in the agent's memory: `sidepad-debug-serving-gotchas`.)

### Other gotchas
- Bluepad32's CMake defaults to `pico_cyw43_arch_none`; `rp/src/CMakeLists.txt` patches it at configure time to `pico_cyw43_arch_lwip_poll` so it coexists with lwIP without modifying the submodule.
- Once a controller connects, the terminal counts down `SIDEPAD_AUTOEXIT_SECONDS` (10) then exits to the desktop as if ESC was pressed (any key/controller input cancels, stickily).

### EmuMD (`mdfw.ini`, `emu/`)
`make emu` runs the real firmware in Hatari through [EmuMD](https://github.com/neilrackett/emumd) (submodule `emu/emumd`; `EMUMD=../emumd` for a checkout); `make emu-test` (`emu/test.sh`) is the end-to-end test of everything above: four slots, a second pad, rumble out and back, the safety stop not cutting in while held. EmuMD stands in for the template's hardware and, with `[bluetooth]`, for Bluepad32: controllers come from a script (`-O pads=FILE`, format in `emu/test/pads.txt`), and every rumble and LED call is logged as `Bluepad32: pad N ...`. Things that are easy to break:
- **No harddrive.** A GEMDOS drive takes the cartridge's first 1 KB, where `main.s` boots. ST programs go on a floppy (`emu/test.sh` builds one with mtools); `XPADTEST.PRG` runs from its `AUTO` folder once the auto-exit has left the terminal.
- **No `-V` in the test.** DPRINTF output lands in the middle of the ST's console lines. Everything the test checks is logged without it; use `-V` by hand to see `Xpad: req at` and `Rumble:` lines.
- **One `-O` only**: several break on an EmuMD/Hatari option-joining bug.
- **Host builds need host-safe code:** cartridge-window addresses are `uintptr_t`, not `uint32_t` (`chandler.c`, `display.c`, `emul.c`): the same width on the RP2040, whole on a 64-bit host. `tprotocol.h`'s `strh` is behind `__arm__`. `network_scan()` has no nested functions. `u8g2_d_setup.c` is left out of the host build (it names drivers this u8g2 lacks; the device's linker drops it). Offsets computed as `(uint32_t)&sym - XIP_BASE` are fine: they wrap back to the right value.
- `controller_init()`'s `initialized` guard survives an EmuMD reboot (statics persist there), so a reboot that is not to the Booster would leave Bluetooth down in the emulator. Real hardware resets the chip, so this is emulator-only.

### Mouse speed is per injection
`SIDEPAD_MOUSE_MAX_SPEED_VBL` and `_ETV` are pixels per injection at full deflection, so the cursor's real speed is that times the hook's rate: 50 Hz on ETV, the display's rate on VBL (50, 60 or 71 Hz). Both were tuned by hand at 20 while ETV wrongly injected at 12.5 Hz (see above). VBL stays 20 (1000 pixels a second at 50 Hz). ETV is now 5, the same 250 pixels a second it was tuned at. They are separate because they were tuned separately. Fractions of a pixel are carried from frame to frame (`mouseAxisDelta`), because at 50 Hz whole pixels alone would make the slowest speed 50 pixels a second. Retune on hardware if it feels wrong; anything timed in real seconds should use `_hz_200`, as rumble does.

### Known limitation (by design)
Injection rides the IKBD `joyvec` path. Games/demos that read the joystick straight from the IKBD ACIA interrupt (e.g. the Blood Money demo) bypass `joyvec` entirely and **will not see the pad**; tools that go through `joyvec` (PP's JOYMOUT, most games — Duke Nukem works in ETV mode) do. Separately, joystick-1 fire is physically wired to the **right mouse button** at the IKBD, so synthesised fire also shows as a right-click — expected ST hardware behaviour.

## Troubleshooting

| Symptom | Fix / cause |
| --- | --- |
| `the input device is not a TTY` from `stcmd` | Run from a PTY, or `export STCMD_NO_TTY=1` before invoking `stcmd` directly. |
| `arm-none-eabi-gcc not found` | `PICO_TOOLCHAIN_PATH` is wrong — point it at the toolchain `bin` dir. |
| Missing `CHARACTER_GAP_MS` build error | Re-add `#define CHARACTER_GAP_MS 700` to `rp/src/include/blink.h`. |
| `ERROR: cartridge code is N bytes; limit is 8192` | m68k cartridge > 8 KB. Trim `main.s`/includes or move data into APP_BUFFERS / SHARED_VARIABLES instead of the cartridge image. |
| `firmware.py` fails / build exits 6 | Trimmed cartridge image is odd-length. Pad `userfw.s` (trailing `dc.w $FFFF` outside the copied block) so it stays even. |
| Final steps fail copying UF2 | Upstream compile failed — scroll back to the first error. |
| ST shows garbage but commands work | `target_firmware.h` is stale — `stcmd make` likely failed silently. Confirm `target/atarist/dist/BOOT.BIN` timestamp matches the rest of `dist/`. |
| Release build is noisy on UART | The CMake `if(NOT _DEBUG)` silent-release bug — see Build gotchas. |
| Clean desktop, no terminal | Three causes — see "No terminal on boot" above (mouse-flood / served-too-late / serving-path layout). |
| `No Keystroke:` logs | `commemul_init()` not called at startup. |

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/` — pinned submodules, re-pinned every build. Change FatFs config via `rp/src/ff/ffconf.h` (project-owned override; the include path makes it win).
- Don't touch `main.c` for feature work — start in `emul.c`. (Early-boot recovery hatches are the only exception; keep them single-sample.)
- Match the existing C style (`.clang-format`, `.clang-tidy`, wired via CMake when the binaries are on `PATH`).
- Keep the pre-`init_romemul` path lean; don't add blocking work (busy-waits, heavy UART) ahead of serving.

## Working style

Bias toward caution over speed. For trivial tasks, use judgment.

1. **Think before coding** — state assumptions; if multiple interpretations exist, present them; if a simpler approach exists, say so; if something's unclear, stop and ask.
2. **Simplicity first** — minimum code that solves the problem, nothing speculative. No abstractions for single-use code (the ETV bug is a live reminder: a single-use helper broke serving where an inline store did not). No unrequested flexibility, no error handling for impossible scenarios. If 200 lines could be 50, rewrite.
3. **Surgical changes** — touch only what you must; don't "improve" adjacent code/formatting; match existing style; mention unrelated dead code rather than deleting it; remove only what your change orphans. Every changed line should trace to the request.
4. **Goal-driven execution** — define success criteria and loop until verified. "Verified" here means: `make -C rp/test test` passes, the build succeeds, it boots on hardware, and the behaviour is confirmed over the serial console. State a brief plan with a verification check per step for multi-step work.
5. **No AI attribution** — never add AI-tool attribution to commits, PRs, code comments, docs, or any artifact. No `Co-Authored-By: Claude …`, no "Generated with Claude Code / ChatGPT", no "AI-assisted" notes. Write everything as the human author.

Keep this file current as the process evolves so every agent starts with the latest tribal knowledge.
