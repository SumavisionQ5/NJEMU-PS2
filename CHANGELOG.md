# NJEMU PS2 — Changelog

All changes below are relative to the original PSP-only NJEMU codebase
(NJEMU-master / phoe-nix fork). This document lists the net changes only;
iterative debugging work is omitted.

---

## Video

### Rotate Screen: clear side bars + 3:4 TV aspect (v29)
- v28's rotated frame only covered the centred area, so the menu's last
  frame stayed visible in the side bars after Resume. `blit_finish` now
  clears the whole draw buffer before `copyRectRotate` (CPS1 and CPS2).
- v28's uniform pixel scaling ignored the TV's 4:3 pixel aspect ratio
  and rendered the rotated frame too narrow. The rotated frame is now a
  **3:4 portrait** (the 384x224 frame fills a 4:3 TV, so rotated it is
  width = height * 3/4 = 168 of 224, centred).

### 90-degree rotate blit (Rotate Screen)
- `ps2_copyRectRotate()` was a commented-out stub: enabling "Rotate
  Screen" (tate games such as varth) made `blit_finish` draw nothing, so
  the game frame never appeared and the last menu frame stayed on screen
  ("Resume Game" seemed dead).
- Fix: added a textured-triangle primitive
  (`gskit_prim_list_triangle_texture_uv_flat`, GIF layout copied from the
  sprite version with PRIM=TRIANGLE - a GS sprite is axis-aligned, so
  rotation requires triangles) and implemented `ps2_copyRectRotate` as a
  **counter-clockwise 90-degree** rotation (v28; v27 shipped clockwise and
  appeared upside down). The rotated frame is scaled with a **uniform
  scale to preserve the source aspect ratio and centred** in the target
  rect (no full-screen stretch; a 384x224 frame becomes ~130x224 centred,
  bars on the sides).

### Fix visible tearing with vsync ON (vsync decoupled from Frame Limit)
- Two issues fixed:
  1. `update_screen()` only waited for the vsync when the frame finished
     early (`curr < target - 100`); a late frame flipped mid-scan
     (`flipScreen(0)`) and tore.
  2. The vsync branch lived INSIDE the `option_speedlimit` block, so
     turning off Frame Limit silently disabled vsync entirely and tore
     badly on heavy games (dino).
- Fix: when vsync is enabled, always `flipScreen(1)` (wait for the
  vsync), independent of the frame limiter. Heavy frames may run slightly
  below 60fps instead of tearing.

### In-game menu order
- v27: Resume / Reset / Settings / Return to Browser / Exit
- v28: Resume / **Settings** / **Reset** / Return to Browser / Exit
  (Settings and Reset swapped; the earlier Settings / Return to Browser
  swap from v27 is kept)

---

## Input

### Menu input fix: disable edge-latch in menus (eaten navigation)
- Symptom: after the v25 latch restore, quickly moving through the ROM
  list ate inputs.
- Root cause: menus use edge detection (`pad & ~prev`). A latched press
  merged into `pad` while `prev` still held the previous (latched) level
  made the NEXT tap look like a repeated hold, so it was eaten.
- Fix: `ps2_input.c` adds `ps2_input_set_latch()`; `ps2_gui.c` disables
  the latch while `file_browser()` / `showmenu()` are active and
  re-enables it when the game loop runs. Menus poll every vsync
  (~16.7ms), far faster than human taps, so they don't need the latch.
  All nested submenus (Settings / Dip Switch / Key Config / Autofire)
  inherit the disabled state.

### ROM list page-up/page-down (SNESticle style)
- Left / Right on the D-Pad pages the ROM list by one visible page
  (15 rows) in the ROM browser. The footer now reads
  `< >:Page  O:Enter  X:Back`. (L/R shoulder buttons remain Up/Down.)

### Restore edge-latch input sampler (fixes "eaten" inputs)
- Symptom: quick taps (pressed+released entirely between two game frames)
  were lost, most noticeable in fighters on heavy cores where dropped
  frames stretch the poll gap.
- Root cause: v17 replaced the original PS2 port's 4ms edge-latch sampler
  with a plain per-frame poll (SNESticleRevive-style). On the PS2 that
  loses any tap that falls completely between two polls.
- Fix: `ps2_input.c` — restored the sampler thread (4ms / ~250Hz, priority
  0x0f; a normal EE thread, NOT the VBLANK ISR which froze the machine)
  feeding an edge accumulator: every 0->1 transition since the last poll
  is latched, then merged into the poll result for exactly one frame
  (`input_merge_latch`, interrupt-safe read+clear). Held buttons never
  accumulate (edge detection only), so menu navigation stays level-based.
  The v17 SNESticleRevive read protections (DISCONN gate, hold-on-no-data,
  reject btns==0/mode==0) are kept. MVS analog poll (raw-bit layout) is
  intentionally not merged.

---

## Audio

### Fix audio stutter with vsync ON (gsKit busy-wait vsync)
- Symptom: with vsync enabled, heavy non-QSOUND CPS1 games (sf2: YM2151 +
  dual OKI6295) stuttered; vsync off was fine, and lighter games (dino)
  were fine in both modes.
- Root cause: gsKit's `gsKit_sync_flip()` → `gsKit_vsync_wait()` polls
  `GS_CSR` in a `while` loop, pinning the EE core for the whole ~16.7ms
  of the vsync wait without ever yielding. The PSP original uses
  `sceDisplayWaitVblankStart` (a sleep). The sound thread (priority 0x08,
  ~33ms/render for YM2151+OKI) only got CPU at interrupt scheduling
  points, so its audio blocks underran and stuttered. Light-load games
  had enough ring-buffer headroom to hide the starvation.
- Fix: `ps2_video.c` — replaced the busy wait with a VBLANK-interrupt
  semaphore: a `vsync_handler` (registered via `gsKit_add_vsync_handler`)
  signals a semaphore; `ps2_flipScreen` mirrors `gsKit_sync_flip()` but
  sleeps on `WaitSema(vsync_sema_id)` (+ `PollSema` drain) instead of
  spinning. `FirstFrame` semantics and the flip itself are unchanged.
  Cleanup added in `ps2_exit`.

### Fix CPS1 audio running at ~2x speed (output block truncated)
- Symptom: sf2/ffight (YM2151 + OKI6295) audio sounded stretched / low-pitched,
  as if a 44.1kHz stream were being played back at half rate. QSOUND games
  (CPS2) and MVS/NCDZ were unaffected.
- Root cause: the PSP original always reserves the hardware audio channel
  as STEREO (`sceAudioSRCChReserve(samples, freq, 2)`), regardless of the
  emulated system's internal channel count. The PS2 port passed
  `sound->channels` (1 for CPS1) to `audsrv_set_format`, so audsrv
  interpreted the block as mono. CPS1's mono render path already outputs
  interleaved L/R-copied stereo frames, so audsrv stretched each block to
  double its real duration.
- Fix: `common/sound.c` — reserve the SRC channel with a fixed channel
  count of 2, matching the PSP original. No change for systems already
  stereo (CPS2/MVS/NCDZ); data layout is unchanged.

### Fix CPS1 audio running at ~2x speed (output block truncated)
- Symptom: after the stereo-channel fix, CPS1 audio no longer sounded
  deep but played back roughly twice as fast; music stuttered with vsync
  on and produced channel-mismatch noise with vsync off.
- Root cause: `sound->channels` stayed 1 for CPS1, so the sound thread
  handed audsrv `samples * channels * 2 = 2944` bytes while the mono
  render path actually emits 1472 interleaved STEREO frames (2944 int16 =
  5888 bytes). audsrv (always stereo) played only the first 736 frames and
  dropped the other half — the music ran at ~2x speed. The same size
  mismatch also made the mute path memset only half the buffer.
- Fix: `common/sound.c` — the sound thread computes the output block size
  as `sound->samples * 2` (fixed stereo output) for memset, output and the
  buffer-size assert. `sound->channels` is left untouched (YM2151's pan
  layout depends on it). CPS2/MVS/NCDZ already used 2 channels and are
  unaffected.

---

## Video Output

### Native progressive 224P/240P modes
- Replaced the original 480i interlaced output with native progressive modes:
  - CPS1 / CPS2: 384×224p
  - MVS / NCDZ: 320×240p
- Avoids the 480i flicker / black-band artifacts on CRT displays.
- Menu mode uses the same progressive mode as the game mode (was 480i before).

### Z-buffer sizing fix (black-block corruption)
- The Z-buffer was allocated at the screen resolution but used as if it
  matched the render-target (offscreen canvas) size. On 224P/240P the depth
  writes overflowed past the Z-buffer and clobbered the offscreen canvas,
  producing black rectangular blocks in the upper part of the picture
  (only 480i builds had a large-enough Z-buffer, which is why the official
  build never showed it).
- Fix: re-allocate the Z-buffer to the render-target size after video init
  and point the GS depth-buffer registers at the new address.

### Tile cache eviction hardening
- `*_evict_lru()` now performs a two-pass scan: first it only evicts
  entries that were NOT used in the current frame (never aliasing a VRAM
  slot that this frame's vertices still reference); if the pool is
  exhausted mid-frame it force-evicts the least-recently-used entry so a
  tile is never silently dropped (a dropped tile used to render as a
  black block).

### Texture upload cache flush
- `ps2_uploadMem()` now passes `GS_CLUT_NONE` instead of `GS_CLUT_TEXTURE`
  to `gsKit_texture_send_inline()`, which makes it emit a `GS_TEXFLUSH`
  after each tile upload so the GS texture cache never samples stale data.

### Menu panel / right-edge clipping
- The menu panel is sized to sit inside the GS display window after the
  GS integer-division quantization (2880/384 = 7.5 → 7 leaves a gap that
  pushed the right border off-screen); CPS1/CPS2 panel width is 290,
  MVS/NCDZ 264, centered inside the framebuffer.
- List line height reduced from 18 to 10 px so more ROM entries fit on a
  224-line screen; all panel text was kept at its original coordinates.
- Bottom hint simplified to a single centered line.

## Input

### SNESticleRevive-style pad polling (eaten-input fix)
- The original `padGetState()`-gated polling dropped pads that spend a
  long time in intermediate states (EXECCMD / FINDPAD / FINDCTP1), which
  made presses disappear entirely on real DualShock 2 pads.
- Rewritten following SNESticleRevive's PS2 input layer (a PS2 SFC
  emulator with no eaten-input behaviour):
  - Only `PAD_STATE_DISCONN` skips the read; every other state is polled
    unconditionally.
  - `padRead() == 0` (connected but no fresh data) keeps the previous
    frame's button level — a held button is never momentarily reported
    as released.
  - Invalid reads are rejected: `padStatus.btns == 0` (active-low would
    mean *all* buttons pressed at once) or `padStatus.mode == 0` keeps
    the previous state.
  - The pad snapshot is taken at the START of the emulated frame
    (`update_inputport()` moved before `timer_update_cpu()`), so the
    input read now feeds the same frame's emulation instead of arriving
    one frame late — matching SNESticleRevive's
    `Input Poll → Input Snapshot → ExecuteFrame → Render` model.
  - The previous 4 ms sampler thread and vsync edge-latch accumulator
    were removed: a plain per-frame poll with validated, held reads is
    sufficient (proven by SNESticleRevive), and the extra thread is no
    longer needed.

### Default button mapping
- Default action buttons are □ / × / △ / ○:
  - MVS / NEOGEO CD: A=□ B=× C=△ D=○
  - CPS1 / CPS2: Button1=□ Button2=× Button3=△ Button4=○
  - Start=START, Coin/Select=SELECT
- In-game menu: ○ confirms / selects, × returns / cancels (hints updated
  accordingly).

## Timing / Frame Scheduling

### PS2 microsecond clock fix (root cause of many issues)
- `ps2_currentUs()` used to return `clock()`, which PS2SDK implements as
  `GetTimerSystemTime() >> 8` (i.e. ~576 kHz ticks, not microseconds).
  Every time-based caller (frame limiter, input delay, CD-ROM timing)
  therefore ran ~16× slow, and the frame limiter's `usleep()` grew without
  bound every frame — the main loop effectively fell asleep right after
  the CPS1 self test.
- Returning the raw `GetTimerSystemTime()` value (the EE bus clock,
  ~147.5 MHz) made the deadline check never true, so nothing ever slept
  (unrestricted fast-forward, audio thread starved).
- Fix: convert bus clocks to microseconds (`clocks × 125 / 18432`,
  verified against real-hardware measurements), matching the PSP
  `sceRtcGetCurrentTick()` behavior.

### Frame-limit hardening
- A single frame-limit sleep is capped at 3 frame times so a stale or odd
  deadline can never turn into a multi-hundred-millisecond stall.
- Late frames flip immediately (tearing is preferred over dropping to
  30 fps); vsync is only waited for when the frame is early.

## Compatibility

### CPS1 DIP-switch defaults (freeze / self-test hang fix)
- The PSP original initialises `cps1_dipswitch[0..2]` to `0xff` (factory
  all-off) via its `load_settings()` config system. The PS2 port had no
  such system, so the switches stayed 0, which sets the DIP_C bit3
  "Freeze" bit to the frozen state. Games using that bit (sf2, cawing,
  dino, punisher, …) hung / went black right after the self test.
- Fix: `input_init()` now initialises the DIP switches to `0xff`,
  matching the PSP defaults; all previously stuck games pass the self
  test and boot normally.

## Audio

### In-game menu mute without stream teardown
- The menu used to call `audsrv_stop_audio()`; audsrv has no
  `start_audio()`, so the stream could never be restarted and
  `audsrv_wait_audio()` blocked forever — returning to the game was
  silent and re-opening the menu deadlocked the emulator.
- Fix: the menu now mutes by setting the audsrv volume to 0 and restores
  it to 100 on exit; the stream itself is never stopped.

## Menu / UI

### PS2 settings menu (mirrors the PSP option set, per-system)
- In-game menu gained a "Settings" entry (per PSP options):
  - CPS1: Raster Effects, Rotate Screen, Video Sync, Auto Frameskip,
    Frameskip (0–10), Show FPS, Frame Limit, Enable Sound, Sample Rate,
    Sound Volume.
  - CPS2: same minus CPS1-only items (no Raster / Rotate / Sample Rate).
  - MVS: Region, Machine Mode, Raster Effects + the common items.
  - NCDZ: Region, Raster Effects, Emulate Load Screen, CD-ROM Speed
    Limit, CDDA on/off & volume + the common items.
- Key Config sub-menu (rebind A/B/C/D or Buttons 1–4, Start, Coin/Select)
  and Autofire sub-menu (interval + per-button on/off).
- Values apply immediately; changes are held in memory (not persisted,
  same as the PSP's runtime behavior).

### MVS Region / Machine Mode now apply on "Reset Game"
- The Region / Machine Mode patch is written into the loaded BIOS memory
  only while the BIOS is being loaded (`load_rom_user1()`). The in-game
  menu's "Reset Game" calls `neogeo_reset()` which does NOT reload the
  BIOS, so a Region (or Machine Mode) changed in Settings never took
  effect on reset. (NCDZ was unaffected — its reset rewrites the region
  registers directly.)
- Fix: the BIOS region / machine-mode patch was extracted into
  `neogeo_apply_bios_patch()` and `neogeo_reset()` now re-applies it to
  the loaded BIOS in memory. Changing Region (DEFAULT / JAPAN / USA /
  EUROPE) or Machine Mode in Settings now takes effect after Reset Game,
  matching the PSP build's CFG_RESTART semantics.

### CPS1 DIP Switch sub-menu (Settings)
- The PSP build exposes a per-game DIP switch editor
  (`load_dipswitch()` / `save_dipswitch()`, cps1/dipsw.c) but the PS2
  settings menu never had an entry for it.
- Fix: CPS1 Settings gained a "Dip Switch" entry. It lists the current
  game's switches (coinage, difficulty, lives, free play, freeze, flip
  screen, demo sounds, game mode, …) in the same layout as the PSP
  editor; L/R changes the selected switch, × / ○ writes the new values
  back to `cps1_dipswitch[]`. The 68000 I/O-port handlers read the DIP
  bits on every access (cps1/driver.c), so changes apply immediately —
  no reset needed. The factory defaults remain 0xff (all off).

### VSync defaults per system
- CPS1 / CPS2: vsync ON by default.
- MVS / NCDZ: vsync OFF by default.

### FPS overlay (Show FPS)
- Implemented the PS2 small-font renderer (the original was a stub), so
  the Show FPS option actually draws `fskp n pct fps` in the top-right
  corner of the game picture.

### Menu entry/exit debounce
- SELECT+START now waits for both buttons to be released before the
  emulator leaves the in-game menu, preventing an instant re-entry that
  made "Resume Game" appear to do nothing.

## Platform / Misc

- `zip_open()` no longer returns a truncated 64-bit pointer as an int
  (was an intermittent "File not found" on 64-bit desktop builds).
- All diagnostic logging that wrote `njemu_boot.txt` / `njemu_diag.txt` /
  `njemu_dbg.txt` / `njemu_video.txt` to the USB stick has been disabled
  (boot_log / dbg_printf are now no-ops; the self-test blocks are
  compiled out) — the emulator no longer writes any log files.
