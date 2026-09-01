#include <stdio.h>
#include <stdlib.h>
#include <libpad.h>
#include <libmtap.h>
#include <kernel.h>
#include <ps2_joystick_driver.h>
#include "common/input_driver.h"

#define PS2_MAX_PORT      2 /* each ps2 has 2 ports */
#define PS2_MAX_SLOT      4 /* maximum - 4 slots in one multitap */
#define MAX_CONTROLLERS   (PS2_MAX_PORT * PS2_MAX_SLOT)
#define PS2_ANALOG_STICKS 2
#define PS2_ANALOG_AXIS   2
#define PS2_BUTTONS       16
#define PS2_TOTAL_AXIS    (PS2_ANALOG_STICKS * PS2_ANALOG_AXIS)

#define tolerance 0x30

struct JoyInfo
{
    uint8_t padBuf[256];
    uint16_t btns;
    uint8_t analog_state[PS2_TOTAL_AXIS];
    uint8_t port;
    uint8_t slot;
    int8_t rumble_ready;
    int8_t opened;
} __attribute__((aligned(64)));

struct JoyInfo joyInfo[MAX_CONTROLLERS];

typedef struct ps2_input {
	uint8_t enabled_pads;
} ps2_input_t;

/* ------------------------------------------------------------------ */
/* Input-latch accumulator (fixes "eaten" inputs on the PS2)          */
/*                                                                     */
/* The CPS2 core samples the pad only ONCE per emulated frame          */
/* (cps2/inptport.c -> update_inputport -> poll_gamepad).  When the    */
/* PS2 drops frames -- and CPS2 is the heaviest NJEMU core -- the gap */
/* between two samples grows, so a quick tap that is fully            */
/* pressed+released inside that gap is never observed -> lost input.  */
/*                                                                     */
/* Fix: a 60 Hz vsync callback (registered from ps2_video.c via        */
/* gsKit_add_vsync_handler, the gsKit-safe way to hook VSYNC -- a raw  */
/* SetVBlankHandler would clash with gsKit's own INTC #3 handler)      */
/* feeds this accumulator.  It latches every button that makes a       */
/* 0->1 transition since the previous sample, so a tap is preserved    */
/* until the next poll_gamepad() consumes it.  The game still receives  */
/* the raw held-level every frame via ps2_poll(); the accumulator      */
/* only ADDS the in-between presses.                                   */
/* ------------------------------------------------------------------ */
/* Last valid pad level (SNESticleRevive-style hold): when a poll returns
 * no fresh data (padRead()==0) or obviously invalid data (btns==0 / mode==0),
 * keep the previous level so a held button is never momentarily reported
 * as released. */
static uint32_t g_last_input = 0;

/* ------------------------------------------------------------------ */
/* Edge-latch accumulator (restored v25; fixes "eaten" inputs)        */
/*                                                                     */
/* A pure per-frame poll (v17) loses a quick tap that is pressed and   */
/* released entirely between two game frames (the gap grows on dropped */
/* frames).  A low-priority sampler thread reads the pad at ~250 Hz    */
/* and latches every 0->1 transition into g_input_accum; the game     */
/* still receives the raw held level every frame, and ps2_poll()       */
/* merges the latched presses for exactly one frame.  Held buttons     */
/* never accumulate (edge detection only), so menu navigation stays    */
/* level-based and sane.                                               */
/* ------------------------------------------------------------------ */
static volatile uint32_t g_input_accum = 0;   /* presses latched since last poll */
static volatile uint32_t g_sample_last  = 0;  /* last level seen by the sampler  */

/* Latch merging is only useful inside the game loop (frame rate is
 * unstable there, so taps can fall between frames).  Menus poll every
 * vsync (~16.7ms) - far faster than human taps (~100ms) - and their
 * edge detection (pad & ~prev) would mis-read a latched press as a
 * repeated hold, eating the NEXT tap.  Menus disable the latch. */
static volatile int g_input_latch_enabled = 1;

void ps2_input_set_latch(int enable)
{
	g_input_latch_enabled = enable;
}

static s32 input_sampler_id = -1;
static volatile int input_sampler_alive = 0;

static uint32_t basicPoll(struct padButtonStatus *paddata, bool exclusive);

/* Runs on a NORMAL EE thread (NOT the VBLANK ISR - calling padRead from
 * interrupt context froze the machine).  Latches 0->1 edges. */
int ps2_input_feed_vsync(int v)
{
	struct padButtonStatus paddata;
	uint32_t lvl = basicPoll(&paddata, false);
	uint32_t edge = lvl & ~g_sample_last;

	g_sample_last = lvl;

	/* Atomic merge: the main thread may be consuming the accumulator
	 * (read+clear) in ps2_poll() at the same time. */
	DIntr();
	g_input_accum |= edge;
	EIntr();

	return 0;
}

static void input_sampler_thread(void *arg)
{
	(void)arg;
	while (input_sampler_alive)
	{
		/* 4 ms (~250 Hz) edge detection: fast enough to catch any tap
		 * that falls between game frames, without saturating the SIO
		 * bus (~100-200us per pad read -> ~4% of a core at 4ms). */
		DelayThread(4000);
		ps2_input_feed_vsync(0);
	}
	ExitThread();
}

static void *ps2_init(void) {
	ps2_input_t *ps2 = (ps2_input_t*)calloc(1, sizeof(ps2_input_t));

	uint32_t port = 0;
    uint32_t slot = 0;

    if (init_joystick_driver(true) < 0) {
		free(ps2);
        return NULL;
    }

    for (port = 0; port < PS2_MAX_PORT; port++) {
        mtapPortOpen(port);
    }
    /* it can fail - we dont care, we will check it more strictly when padPortOpen */

    for (slot = 0; slot < PS2_MAX_SLOT; slot++) {
        for (port = 0; port < PS2_MAX_PORT; port++) {
            /* 2 main controller ports acts the same with and without multitap
            Port 0,0 -> Connector 1 - the same as Port 0
            Port 1,0 -> Connector 2 - the same as Port 1
            Port 0,1 -> Connector 3
            Port 1,1 -> Connector 4
            Port 0,2 -> Connector 5
            Port 1,2 -> Connector 6
            Port 0,3 -> Connector 7
            Port 1,3 -> Connector 8
            */

            struct JoyInfo *info = &joyInfo[ps2->enabled_pads];
            if (padPortOpen(port, slot, (void *)info->padBuf) > 0) {
                info->port = (uint8_t)port;
                info->slot = (uint8_t)slot;
                info->opened = 1;
                ps2->enabled_pads++;
            }
        }
	}

	/* Start the input sampler thread (fixes eaten inputs; a normal EE
	 * thread, NOT the VBLANK ISR - that froze the machine). */
	{
		ee_thread_t eth;
		extern void *_gp;
		eth.attr = 0;
		eth.option = 0;
		eth.func = input_sampler_thread;
		eth.stack = malloc(0x2000);
		eth.stack_size = 0x2000;
		eth.gp_reg = &_gp;
		/* 0x0f: high enough to never starve, low enough not to hog the
		 * main thread (a pad read over SIO is ~100-200us; at 4ms that's
		 * ~4% of one core). 0x05 plus a tight loop visibly stuttered the
		 * emulator. */
		eth.initial_priority = 0x0f;
		input_sampler_id = CreateThread(&eth);
		if (input_sampler_id >= 0)
		{
			input_sampler_alive = 1;
			StartThread(input_sampler_id, NULL);
		}
	}

	return ps2;
}

static void ps2_free(void *data) {
	ps2_input_t *ps2 = (ps2_input_t*)data;
	uint32_t i = 0;

	if (input_sampler_id >= 0)
	{
		input_sampler_alive = 0;
		DeleteThread(input_sampler_id);
		input_sampler_id = -1;
	}

	for (i = 0; i < MAX_CONTROLLERS; i++) {
		struct JoyInfo *info = &joyInfo[i];
		if (info->opened) {
			padPortClose(info->port, info->slot);
		}
	}

	deinit_joystick_driver(true);

	free(ps2);
}

static struct  JoyInfo *getFirstJoyInfo(uint32_t pad){
	uint32_t i;
	struct JoyInfo *info = NULL;

	for (i = 0; i < MAX_CONTROLLERS; i++) {
		info = &joyInfo[i];
		if (info->opened) {
				return info;
		}
	}

	return NULL;	
}

static inline int16_t convert_u8_to_s16(uint8_t val)
{
    if (val == 0) {
        return -0x7fff;
    }
    return val * 0x0101 - 0x8000;
}

static uint32_t basicPoll(struct padButtonStatus *paddata, bool exclusive) {
	uint32_t data = 0;
	int32_t state, pressed_buttons, ret;
	struct JoyInfo *info = NULL;

	info = getFirstJoyInfo(0);
	if (info == NULL) {
		return data;
	}
	state = padGetState(info->port, info->slot);
	/* SNESticleRevive-style: only a hard DISCONN skips the read. A real
	 * DualShock 2 can sit in EXECCMD/FINDPAD/FINDCTP1 for long stretches;
	 * gating on those states (old code did) made the pad unresponsive for
	 * the whole session - the "eaten inputs" bug. */
	if (state == PAD_STATE_DISCONN) {
		return 0;
	}
	ret = padRead(info->port, info->slot, paddata); // port, slot, buttons
	if (ret == 0) {
		/* Connected but no fresh data yet (still negotiating / rumble busy):
		 * hold the previous level so a held button is never momentarily
		 * reported as released. Same pattern as SNESticle/picodrive. */
		return g_last_input;
	}
	/* Reject obviously invalid pad data: btns==0 (active-low would mean
	 * ALL buttons pressed at once) or mode==0 - treat as a failed read
	 * and hold the previous state (SNESticleRevive input.cpp). */
	if (paddata->btns == 0 || paddata->mode == 0) {
		return g_last_input;
	}

	/* Buttons */
	pressed_buttons = 0xffff ^ paddata->btns;

	data |= (pressed_buttons & PAD_UP) ? PLATFORM_PAD_UP : 0;
	data |= (pressed_buttons & PAD_DOWN) ? PLATFORM_PAD_DOWN : 0;
	data |= (pressed_buttons & PAD_LEFT) ? PLATFORM_PAD_LEFT : 0;
	data |= (pressed_buttons & PAD_RIGHT) ? PLATFORM_PAD_RIGHT : 0;

	data |= (pressed_buttons & PAD_CIRCLE) ? PLATFORM_PAD_B1 : 0;
	data |= (pressed_buttons & PAD_CROSS) ? PLATFORM_PAD_B2 : 0;
	data |= (pressed_buttons & PAD_SQUARE) ? PLATFORM_PAD_B3 : 0;
	data |= (pressed_buttons & PAD_TRIANGLE) ? PLATFORM_PAD_B4 : 0;

	data |= (pressed_buttons & PAD_L1) ? PLATFORM_PAD_L : 0;
	data |= (pressed_buttons & PAD_R1) ? PLATFORM_PAD_R : 0;

	data |= (pressed_buttons & PAD_START) ? PLATFORM_PAD_START : 0;
	data |= (pressed_buttons & PAD_SELECT) ? PLATFORM_PAD_SELECT : 0;

	/* Analog */
	if (paddata->ljoy_h || paddata->ljoy_v || paddata->rjoy_h || paddata->rjoy_v) {
		if ((convert_u8_to_s16(paddata->ljoy_v) < 0) && !(exclusive && (pressed_buttons & PAD_UP))) data |=  PLATFORM_PAD_DOWN;
		if ((convert_u8_to_s16(paddata->ljoy_v) > 0) && !(exclusive && (pressed_buttons & PAD_DOWN))) data |=  PLATFORM_PAD_UP;
		if ((convert_u8_to_s16(paddata->ljoy_h) < 0) && !(exclusive && (pressed_buttons & PAD_LEFT))) data |=  PLATFORM_PAD_LEFT;
		if ((convert_u8_to_s16(paddata->ljoy_h) > 0) && !(exclusive && (pressed_buttons & PAD_RIGHT))) data |=  PLATFORM_PAD_RIGHT;
	}

	g_last_input = data;
	return data;
}


/* (Edge-latch sampler restored in v25 - a pure per-frame poll loses
 * taps that fall between frames; see the accumulator above.) */

/* Merge presses the sampler latched since the previous poll, then
 * consume the accumulator atomically (the sampler thread may be OR-ing
 * a new edge into it right now).  A tap that was fully pressed+released
 * between two game frames therefore reaches the core for exactly one
 * frame.  Shared by all poll entry points (MVS analog / fatfursp too). */
static inline uint32_t input_merge_latch(uint32_t btnsData)
{
	DIntr();
	btnsData |= g_input_accum;
	g_input_accum = 0;
	EIntr();
	return btnsData;
}

static uint32_t ps2_poll(void *data) {
	ps2_input_t *ps2 = (ps2_input_t*)data;
	struct padButtonStatus paddata;
	uint32_t btnsData = 0;

	if (ps2->enabled_pads == 0) {
		return btnsData;
	}

	btnsData = basicPoll(&paddata, false);

	if (g_input_latch_enabled)
		return input_merge_latch(btnsData);
	return btnsData;
}

#if (EMU_SYSTEM == MVS)
static uint32_t ps2_pollFatfursp(void *data) {
	struct padButtonStatus paddata;
	uint32_t btnsData = 0;

	btnsData = basicPoll(&paddata, true);

	if (g_input_latch_enabled)
		return input_merge_latch(btnsData);
	return btnsData;
}

static uint32_t ps2_pollAnalog(void *data) {
	uint32_t btnsData;
	struct padButtonStatus paddata = {0};

	/* NOTE: returns the raw pad bit layout (btns + analog sticks), not
	 * PLATFORM_PAD_*; only used by the MVS stick games (irrmaze /
	 * popbounc).  The latch bits use the PLATFORM_PAD_* layout and are
	 * intentionally NOT merged here. */
	btnsData = basicPoll(&paddata, false);

	btnsData  = paddata.btns & 0xffff;
	btnsData |= paddata.ljoy_h << 16;
	btnsData |= paddata.ljoy_v << 24;

	return btnsData;
}
#endif


input_driver_t input_ps2 = {
	"ps2",
	ps2_init,
	ps2_free,
	ps2_poll,
#if (EMU_SYSTEM == MVS)
	ps2_pollFatfursp,
	ps2_pollAnalog,
#endif
};