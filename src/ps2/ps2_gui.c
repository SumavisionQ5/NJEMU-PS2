/******************************************************************************

	ps2_gui.c

	PS2 native menu / UI for NJEMU (CPS/CPS2 arcade emulator).

	Replaces ps2_no_gui.c with a real, SNESticleRevive-style UI:
	  * file_browser()  - ROM set browser; pick a directory, launch the game
	  * showmenu()      - in-game overlay menu (Resume / Reset / Browser / Exit)
	  * ui_init()       - initialise the menu framebuffer

	The menu is rasterised into g_menu_fb (a 512x272 RGBA16 buffer in main
	RAM, see ps2_font.c / ps2_gui.h) and presented each frame via gsKit using
	ps2_present_texture() (defined in ps2_video.c, which reuses the exact
	sprite-draw path the emulator already uses for its scrbitmap).

	ROM layout expected (CPS2):
	  roms/<game>/<chip>.zip      e.g.  roms/sfz2/sz2j.03b.zip
	game_name  = <game>   game_dir = roms/<game>

	Build with:  cmake -DTARGET=CPS2 -DPLATFORM=PS2 -DNO_GUI=OFF

******************************************************************************/

#include "emumain.h"            /* brings in main_ui_draw.h, drivers, Loop, game_name/dir */
#if (EMU_SYSTEM == CPS1)
#include "cps1/inptport.h"      /* P1_*, input_map, af_interval */
#elif (EMU_SYSTEM == CPS2)
#include "cps2/inptport.h"
#elif (EMU_SYSTEM == MVS)
#include "mvs/inptport.h"
#elif (EMU_SYSTEM == NCDZ)
#include "ncdz/inptport.h"
#endif
#include <stdarg.h>             /* va_list for msg_printf */
#include <dirent.h>             /* opendir/readdir/closedir for the ROM browser */
#include <sys/stat.h>           /* stat()/S_ISDIR() for the ROM browser */
#include <sys/types.h>
#include <unistd.h>             /* mkdir() for the roms/ folder */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <kernel.h>
#include <malloc.h>

/* Menus disable the input edge-latch (see ps2_input.c): they poll every
 * vsync and use pad & ~prev edge detection, which a latched press would
 * mis-read as a repeated hold and eat the NEXT tap. */
extern void ps2_input_set_latch(int enable);
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <gsInline.h>
#include <gsCore.h>

#include "ps2/ps2_gui.h"
extern void boot_log(const char *);

/* ps2_present_texture() is defined in ps2_video.c and reuses the
 * already-proven sprite draw path. Declared here (PS2-only TU). */
void ps2_present_texture(GSGLOBAL *gsGlobal, GSTEXTURE *tex, gs_rgbaq color);
void ps2_present_texture_region(GSGLOBAL *gsGlobal, GSTEXTURE *tex, gs_rgbaq color, float uw, float vh);
void ps2_wait_gs_idle(void);


/******************************************************************************
	Shared state
******************************************************************************/

uint32_t g_menu_fb[MENU_TEX_W * MENU_TEX_H];

/* UI palette (declared extern in emumain.h; needed by common code). */
UI_PALETTE ui_palette[UI_PAL_MAX] =
{
	{ 255, 255, 255 },	// UI_PAL_TITLE
	{ 255, 255, 255 },	// UI_PAL_SELECT
	{ 180, 180, 180 },	// UI_PAL_NORMAL
	{ 255, 255,  64 },	// UI_PAL_INFO
	{ 255,  64,  64 },	// UI_PAL_WARNING
	{  48,  48,  48 },	// UI_PAL_BG1
	{   0,   0, 160 },	// UI_PAL_BG2
	{   0,   0,   0 },	// UI_PAL_FRAME
	{  40,  40,  40 },	// UI_PAL_FILESEL1
	{ 120, 120, 120 }	// UI_PAL_FILESEL2
};

int cheat_num = 0;
gamecheat_t* gamecheat[MAX_CHEATS];

/* Menu theme colours (RGBA16). */
#define COL_BG      menu_rgb(0, 0, 0)  /* black (was diag red) */
#define COL_PANEL   menu_rgb(34, 36, 60)
#define COL_PANEL2  menu_rgb(26, 28, 48)
#define COL_BORDER  menu_rgb(90, 96, 140)
#define COL_TITLE   menu_rgb(255, 208, 80)
#define COL_NORMAL  menu_rgb(216, 220, 232)
#define COL_SELECT  menu_rgb(255, 240, 130)
#define COL_DIM     menu_rgb(130, 134, 150)
#define COL_ACCENT  menu_rgb(120, 200, 255)


/******************************************************************************
	Present: upload g_menu_fb as a texture and blit full-screen
******************************************************************************/

static GSTEXTURE *g_menu_tex = NULL;

static void menu_present(void)
{
	static int _mp_log = 0;
	if (_mp_log < 3) { boot_log("[S9] menu_present start"); _mp_log++; }
	GSGLOBAL *gsGlobal = (GSGLOBAL *)video_driver->getNativeObjects(
		video_data, COMMON_GRAPHIC_OBJECTS_GLOBAL_CONTEXT);
	if (!gsGlobal)
		return;

	if (g_menu_tex == NULL)
	{
		g_menu_tex = (GSTEXTURE *)calloc(1, sizeof(GSTEXTURE));
		g_menu_tex->Width  = MENU_TEX_W;
		g_menu_tex->Height = MENU_TEX_H;
		g_menu_tex->PSM    = GS_PSM_CT32;
		g_menu_tex->Filter = GS_FILTER_NEAREST;
		g_menu_tex->Mem    = (void *)g_menu_fb;
		g_menu_tex->VramClut = 0;
		g_menu_tex->Vram = gsKit_vram_alloc(gsGlobal,
			gsKit_texture_size(MENU_TEX_W, MENU_TEX_H, GS_PSM_CT32),
			GSKIT_ALLOC_USERBUFFER);
		gsKit_setup_tbw(g_menu_tex);

	}

	/* Use the NON-inline gsKit_texture_send (the gsKit PNG-example upload
	 * path). With clut != GS_CLUT_TEXTURE it appends TEXFLUSH, which flushes
	 * the GS texture cache - the inline variant never flushed and could
	 * leave stale/corrupt cached texture data. */
	gsKit_texture_send((u32 *)g_menu_fb, MENU_TEX_W, MENU_TEX_H,
		g_menu_tex->Vram, GS_PSM_CT32, g_menu_tex->TBW, GS_CLUT_NONE);

	/* gsKit_texture_send() is an ASYNC dma chain: without waiting for the
	 * GS to consume it, the sprite below sampled a texture whose right-hand
	 * part had not arrived yet -> the panel's right border never showed. */
	ps2_wait_gs_idle();

	/* Draw under the game's DEFAULT test state (ATST=4 EQUAL / AREF=0):
	 * menu pixels now have alpha=0 (see menu_rgb in ps2_gui.h) so they pass
	 * exactly like the game frame does. No gsKit_set_test needed. */
	gs_rgbaq col = color_to_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);
	/* 1:1 blit of the MENU_W x MENU_H view region (menu mode == game mode
	 * resolution in pure 224P/240P builds, so the 8px font is native-sharp
	 * and the full layout - including the bottom hint - stays on screen). */
	ps2_present_texture_region(gsGlobal, g_menu_tex, col,
		(float)MENU_W, (float)MENU_H);

	gsKit_queue_exec(gsGlobal);
	gsKit_sync_flip(gsGlobal);
}

/* Draw a simple double-border panel. */
static void draw_panel(int x, int y, int w, int h)
{
	menu_fill_rect(x,     y,     w,     h,     COL_PANEL);
	menu_fill_rect(x,     y,     w,     2,     COL_BORDER);
	menu_fill_rect(x,     y + h - 2, w, 2,     COL_BORDER);
	menu_fill_rect(x,     y,     2,     h,     COL_BORDER);
	menu_fill_rect(x + w - 2, y, 2,     h,     COL_BORDER);
}


/******************************************************************************
	UI stub functions (required by common code on every platform)
******************************************************************************/

void msg_printf(const char *text, ...) {
	va_list args;
	va_start(args, text);
	vprintf(text, args);
	va_end(args);
}

void show_progress(const char *text) { (void)text; }
void update_progress(void) {}

void msg_screen_clear(void) {}
void show_exit_screen(void) {}
void load_background(int number) { (void)number; }
void show_background(void) {}
int ui_show_popup(int draw) { (void)draw; return 0; }
void ui_popup_reset(void) {}

extern void font_render_into(uint32_t *buf, int buf_w, int x, int y,
                              const char *s, uint32_t color);
extern void ps2_draw_texture_at(GSGLOBAL *gsGlobal, GSTEXTURE *tex,
                                gs_rgbaq color, float dx, float dy,
                                float uw, float vh);
static uint32_t g_fps_fb[256 * 16];
static GSTEXTURE *g_fps_tex = NULL;

/* FPS overlay: draw a small 8x8-font string over the game frame at
 * (sx, sy). Used by show_fps() when "Show FPS" is enabled.
 *
 * v21: the texture is uploaded ONLY when the text actually changes (the
 * FPS value updates a few times per second), and the per-frame
 * ps2_wait_gs_idle() is gone - the upload DMA is queued ahead of the
 * blit in the same GIF FIFO, so it completes together with the frame at
 * flip time. This removes the only blocking GS wait from the main loop. */
void small_font_print(int sx, int sy, const char *s, int bg) {
	(void)bg;
	GSGLOBAL *gsGlobal = (GSGLOBAL *)video_driver->getNativeObjects(
		video_data, COMMON_GRAPHIC_OBJECTS_GLOBAL_CONTEXT);
	if (!gsGlobal)
		return;

	static char g_fps_cache[64] = "";

	if (strcmp(s, g_fps_cache) != 0)
	{
		strncpy(g_fps_cache, s, sizeof(g_fps_cache) - 1);
		g_fps_cache[sizeof(g_fps_cache) - 1] = 0;

		memset(g_fps_fb, 0, sizeof(g_fps_fb));
		font_render_into(g_fps_fb, 256, 0, 0, s, menu_rgb(255, 255, 64));

		if (g_fps_tex == NULL)
		{
			g_fps_tex = (GSTEXTURE *)calloc(1, sizeof(GSTEXTURE));
			g_fps_tex->Width  = 256;
			g_fps_tex->Height = 16;
			g_fps_tex->PSM    = GS_PSM_CT32;
			g_fps_tex->Filter = GS_FILTER_NEAREST;
			g_fps_tex->Mem    = (void *)g_fps_fb;
			g_fps_tex->VramClut = 0;
			g_fps_tex->Vram = gsKit_vram_alloc(gsGlobal,
				gsKit_texture_size(256, 16, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
			gsKit_setup_tbw(g_fps_tex);
		}
		/* async upload: queued ahead of the blit in the same GIF FIFO */
		gsKit_texture_send((u32 *)g_fps_fb, 256, 16, g_fps_tex->Vram,
			GS_PSM_CT32, g_fps_tex->TBW, GS_CLUT_NONE);
	}

	gs_rgbaq col = color_to_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);
	ps2_draw_texture_at(gsGlobal, g_fps_tex, col,
		(float)sx, (float)sy, (float)(strlen(s) * 8), 8.0f);
}
void small_font_printf(int x, int y, const char *text, ...) {
	(void)x; (void)y; (void)text;
}

void uifont_print_center(int sy, int r, int g, int b, const char *s) {
	(void)sy; (void)r; (void)g; (void)b; (void)s;
}
void uifont_print(int sx, int sy, int r, int g, int b, const char *s) {
	(void)sx; (void)sy; (void)r; (void)g; (void)b; (void)s;
}
void uifont_print_shadow(int sx, int sy, int r, int g, int b, const char *s) {
	(void)sx; (void)sy; (void)r; (void)g; (void)b; (void)s;
}
void uifont_print_shadow_center(int sy, int r, int g, int b, const char *s) {
	(void)sy; (void)r; (void)g; (void)b; (void)s;
}
int uifont_get_string_width(const char *s) { (void)s; return 1; }

void textfont_print(int sx, int sy, int r, int g, int b, const char *s, int flag) {
	(void)sx; (void)sy; (void)r; (void)g; (void)b; (void)s; (void)flag;
}

void small_icon_shadow(int sx, int sy, int r, int g, int b, int no) {
	(void)sx; (void)sy; (void)r; (void)g; (void)b; (void)no;
}
void small_icon(int sx, int sy, int r, int g, int b, int no) {
	(void)sx; (void)sy; (void)r; (void)g; (void)b; (void)no;
}

void boxfill_alpha(int sx, int sy, int ex, int ey, int r, int g, int b, int alpha) {
	(void)sx; (void)sy; (void)ex; (void)ey; (void)r; (void)g; (void)b; (void)alpha;
}
void draw_dialog(int sx, int sy, int ex, int ey) {
	(void)sx; (void)sy; (void)ex; (void)ey;
}
int draw_volume_status(int draw) { (void)draw; return 0; }
int draw_battery_status(int draw) { (void)draw; return 0; }

int save_png(const char *path) { (void)path; return 0; }

void msg_screen_init(int wallpaper, int icon, const char *title) {
	(void)wallpaper; (void)icon; (void)title;
}
void load_gamecfg(const char *name) { (void)name; }
void save_gamecfg(const char *name) { (void)name; }
void delete_files(const char *dirname, const char *pattern) {
	(void)dirname; (void)pattern;
}
void draw_scrollbar(int sx, int sy, int ex, int ey, int disp, int total, int cur) {
	(void)sx; (void)sy; (void)ex; (void)ey; (void)disp; (void)total; (void)cur;
}
int help(int number) { (void)number; return 0; }
void ui_popup(const char *text, ...) { (void)text; }


/******************************************************************************
	UI initialisation
******************************************************************************/


/* Called by the video layer when switching BACK to menu (480i) mode:
 * the previous menu texture VRAM was freed by gsKit_vram_clear(), so the
 * next menu_present() must allocate a fresh texture. */
void ui_menu_texture_reset(void)
{
	if (g_menu_tex) { free(g_menu_tex); g_menu_tex = NULL; }
}

void ui_init(void)
{
	menu_fb_init();
}


/******************************************************************************
	ROM browser
******************************************************************************/

#define BROWSER_MAX 512
#define LIST_X 28
#define LIST_Y 42
#define LINE_H 10
/* Visible rows per page. 224-line menus (CPS1/CPS2) fit 15 rows (15th
 * ends at y=192, panel bottom 200, bottom hint 210). 240-line menus
 * (MVS/NCDZ) have room for 17 rows (17th ends at y=212, panel bottom
 * 216, bottom hint 226). */
#if (EMU_SYSTEM == MVS) || (EMU_SYSTEM == NCDZ)
#define VISIBLE 17
#else
#define VISIBLE 15
#endif

typedef enum { E_DIR = 0, E_UP, E_QUIT, E_FILE } entry_type;

typedef struct {
	char name[256];
	entry_type type;
} disp_entry;

/* Build a flat display list for a directory.
 * Returns number of entries, or -1 on scan failure. */
/* ROM root resolution.
 * The emulator's CWD depends on the loader (OPL / uLE / ps2link ...) and the
 * boot-device driver is chosen from that CWD, so a relative "roms" may not
 * resolve. Probe a few likely roots and keep the first one that opens. */
static char g_rom_root[PATH_MAX] = "roms";

static const char *resolve_rom_root(void)
{
	static const char *cands[] = {
		"roms",
		"mass:/roms",
		"mass0:/roms",
		"mc0:/roms",
		"host:/roms",
		NULL
	};
	int i;

	for (i = 0; cands[i] != NULL; i++)
	{
		DIR *d = opendir(cands[i]);
		if (d != NULL)
		{
			closedir(d);
			strncpy(g_rom_root, cands[i], PATH_MAX - 1);
			g_rom_root[PATH_MAX - 1] = 0;
			return g_rom_root;
		}
	}

	/* Nothing found: fall back to the relative folder and create it. */
	mkdir("roms", 0777);
	strncpy(g_rom_root, "roms", PATH_MAX - 1);
	g_rom_root[PATH_MAX - 1] = 0;
	return g_rom_root;
}

static int build_list(const char *dir, disp_entry *list, int max)
{
	DIR *d = opendir(dir);
	int n = 0;
	struct dirent *de;
	int have_up = (strcmp(dir, g_rom_root) != 0);

	if (!d)
		return -1;

	if (have_up && n < max) {
		strcpy(list[n].name, "[..]  Up");
		list[n].type = E_UP;
		n++;
	}

	while ((de = readdir(d)) != NULL)
	{
		char path[PATH_MAX];
		struct stat st;
		int is_dir = 0;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (n >= max)
			break;

		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &st) == 0)
			is_dir = S_ISDIR(st.st_mode);

		strncpy(list[n].name, de->d_name, 255);
		list[n].name[255] = 0;
		list[n].type = is_dir ? E_DIR : E_FILE;
		n++;
	}
	closedir(d);

	if (n < max) {
		strcpy(list[n].name, "[ Quit Emulator ]");
		list[n].type = E_QUIT;
		n++;
	}
	return n;
}

/* Returns: 1 = launch (game_name/game_dir already set),
 *          0 = quit emulator, 2 = went up a directory. */
static int ends_with_zip(const char *s)
{
	size_t l = strlen(s);
	const char *ext;

	if (l < 5)
		return 0;

	ext = s + l - 4;
	return ext[0] == '.' &&
	       (ext[1] == 'z' || ext[1] == 'Z') &&
	       (ext[2] == 'i' || ext[2] == 'I') &&
	       (ext[3] == 'p' || ext[3] == 'P');
}

static int browser_run(char *cur_dir)
{
	static disp_entry list[BROWSER_MAX];
	int n = build_list(cur_dir, list, BROWSER_MAX);
	int sel = 0, scroll = 0;

	if (n < 0) {
		/* Cannot open directory: show error and bail to quit. */
		menu_clear(COL_BG);
		draw_panel(40, 110, MENU_W - 80, 60);
		menu_draw_text_center(MENU_W / 2, 130, "Cannot open folder:", COL_TITLE);
		menu_draw_text_center(MENU_W / 2, 150, cur_dir, COL_NORMAL);
		menu_present();
		for (volatile int i = 0; i < 30; i++) {} /* brief pause */
		return 0;
	}

	uint32_t prev = poll_gamepad();
	while (1)
	{
		uint32_t pad = poll_gamepad();
		uint32_t pressed = pad & ~prev;
		prev = pad;

		if (pressed & (PLATFORM_PAD_UP | PLATFORM_PAD_L))
			sel = (sel + n - 1) % n;
		else if (pressed & (PLATFORM_PAD_DOWN | PLATFORM_PAD_R))
			sel = (sel + 1) % n;
		else if (pressed & PLATFORM_PAD_RIGHT)
			sel = (sel + VISIBLE < n) ? sel + VISIBLE : n - 1;  /* page down */
		else if (pressed & PLATFORM_PAD_LEFT)
			sel = (sel >= VISIBLE) ? sel - VISIBLE : 0;          /* page up */
		else if (pressed & (PLATFORM_PAD_B1 | PLATFORM_PAD_START))
		{
			disp_entry *e = &list[sel];
			if (e->type == E_DIR)
			{
				snprintf(game_dir, sizeof(game_dir), "%s/%s", cur_dir, e->name);
				strncpy(game_name, e->name, sizeof(game_name) - 1);
				game_name[sizeof(game_name) - 1] = 0;
				parent_name[0] = 0;
				return 1;
			}
			else if (e->type == E_UP)
			{
				char *sl = strrchr(cur_dir, '/');
				if (sl) *sl = 0; else strcpy(cur_dir, g_rom_root);
				return 2;
			}
			else if (e->type == E_QUIT)
			{
				return 0;
			}
			else if (e->type == E_FILE)
			{
				/* Flat layout support: launch a .zip straight out of the
				 * current folder, e.g. mass:/roms/sf2.zip .
				 * game_dir stays the containing folder so loadrom.c can
				 * still resolve the parent zip for clone games. */
				if (ends_with_zip(e->name))
				{
					size_t l = strlen(e->name);
					size_t bn = l - 4;

					if (bn > sizeof(game_name) - 1)
						bn = sizeof(game_name) - 1;

					strncpy(game_dir, cur_dir, sizeof(game_dir) - 1);
					game_dir[sizeof(game_dir) - 1] = 0;
					memcpy(game_name, e->name, bn);
					game_name[bn] = 0;
					parent_name[0] = 0;
					return 1;
				}
				/* not a zip: ignore */
			}
		}
		else if (pressed & (PLATFORM_PAD_B2 | PLATFORM_PAD_SELECT))
		{
			disp_entry *e = &list[sel];
			if (e->type == E_UP) {
				char *sl = strrchr(cur_dir, '/');
				if (sl) *sl = 0; else strcpy(cur_dir, g_rom_root);
				return 2;
			}
		}

		if (sel < scroll) scroll = sel;
		else if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;

		/* ---- render ---- */
		menu_clear(COL_BG);
		/* Panel width: 338 px on the 384-wide CPS framebuffers, 264 px on
		 * the 320-wide MVS/NCDZ ones. Text stays at its original offsets. */
#if (EMU_SYSTEM == MVS) || (EMU_SYSTEM == NCDZ)
		draw_panel(16, 16, 264, MENU_H - 40);
#else
		draw_panel(16, 16, 290, MENU_H - 40);
#endif
		menu_draw_text(28, 26, "NJEMU  -  ROM Browser", COL_TITLE);

		int i;
		for (i = 0; i < VISIBLE && (scroll + i) < n; i++)
		{
			int idx = scroll + i;
			int y = LIST_Y + i * LINE_H;
			uint16_t c = (idx == sel) ? COL_SELECT : COL_NORMAL;
			if (list[idx].type == E_DIR)
				menu_draw_text(LIST_X, y, list[idx].name, c);
			else if (list[idx].type == E_QUIT)
				menu_draw_text(LIST_X, y, list[idx].name, COL_ACCENT);
			else if (list[idx].type == E_UP)
				menu_draw_text(LIST_X, y, list[idx].name, COL_DIM);
			else
				menu_draw_text(LIST_X, y, list[idx].name, COL_DIM);

			if (idx == sel)
				menu_draw_text(LIST_X - 16, y, ">", COL_SELECT);
		}

		menu_draw_text_center(MENU_W / 2, MENU_H - 14,
			"< >:Page  O:Enter  X:Back", COL_DIM);
		menu_present();
	}
}

/* ===== TEMPORARY DIAGNOSTIC (remove once the cause is known) ===== */
static void diagnostic_screen(void)
{
	static const char *probe[] = {
		"roms",
		"mass:/roms",
		"mass0:/roms",
		"mc0:/roms",
		"host:/roms",
		"mass:/",
		"mass0:/",
		"mc0:/",
		"host:/",
		"cdfs:/",
		NULL
	};
	char cwd[PATH_MAX];
	char line[96];
	uint32_t prev;
	volatile long i;
	int k, y;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		strcpy(cwd, "(getcwd FAILED)");

	y = 14;
	menu_clear(COL_BG);
	menu_draw_text_center(MENU_W / 2, y, "NJEMU DIAGNOSTIC", COL_TITLE);
	y += 18;

	snprintf(line, sizeof(line), "CWD: %.52s", cwd);
	menu_draw_text(6, y, line, COL_NORMAL);
	y += 16;

	for (k = 0; probe[k] != NULL; k++)
	{
		DIR *d = opendir(probe[k]);
		snprintf(line, sizeof(line), "%-13s %s", probe[k], d ? "OPEN" : "fail");
		if (d != NULL)
			closedir(d);
		menu_draw_text(6, y, line, d ? COL_ACCENT : COL_NORMAL);
		y += 14;
	}

	y += 6;
	menu_draw_text_center(MENU_W / 2, y, "Press any button to continue", COL_TITLE);
	menu_present();

	/* Wait for a button (with a generous timeout so we never hard-hang). */
	prev = poll_gamepad();
	for (i = 0; i < 30000000L; i++)
	{
		uint32_t pad = poll_gamepad();
		if (pad & ~prev)
			break;
		prev = pad;
	}
}
/* ===== END TEMPORARY DIAGNOSTIC ===== */

void file_browser(void)
{
	boot_log("[S8] file_browser start");
	char cur_dir[PATH_MAX];

	/* Start in roms/; create it if it doesn't exist so the user can just
	 * drop game-set directories there. */
	strcpy(cur_dir, resolve_rom_root());

	/* Browser is a menu: disable the game-loop edge latch. */
	ps2_input_set_latch(0);

	while (1)
	{
		int r = browser_run(cur_dir);
		if (r == 0) {
			ps2_input_set_latch(1);
			Loop = LOOP_EXIT;
			return;
		}
		if (r == 2) {
			/* went up a directory; re-run browser */
			continue;
		}
		/* r == 1: launch (menu and game share the same 224P/240P video
		 * mode in pure builds - no mode switch needed) */
		ps2_input_set_latch(1);
		Loop = LOOP_EXEC;
		emu_main();
		ps2_input_set_latch(0);

		if (Loop == LOOP_EXIT) {
			ps2_input_set_latch(1);
			return;
		}
		/* returned to browser */
		strcpy(cur_dir, resolve_rom_root());
	}
}


/******************************************************************************
	In-game menu (called from update_inputport while Loop == LOOP_EXEC)
******************************************************************************/

#define MENU_N 5
static const char *menu_items[MENU_N] = {
	"Resume Game",
	"Settings",
	"Reset Game",
	"Return to Browser",
	"Exit Emulator"
};

static void draw_menu(int sel)
{
	int i;
	/* 240 wide on a 320-wide safe panel fits with 40 px each side
	 * (matches the ROM browser panel inset). */
	int box_w = 240;
	int box_x = (MENU_W - box_w) / 2;
	int box_y = 30;
	int box_h = 40 + MENU_N * 26;

	menu_clear(COL_BG);
	/* dim overlay */
	menu_fill_rect(0, 0, MENU_W, MENU_H, menu_rgb(0, 0, 0));

	draw_panel(box_x, box_y, box_w, box_h);
	menu_draw_text_center(MENU_W / 2, box_y + 12, "MENU", COL_TITLE);

	for (i = 0; i < MENU_N; i++)
	{
		int y = box_y + 40 + i * 26;
		uint16_t c = (i == sel) ? COL_SELECT : COL_NORMAL;
		if (i == sel)
			menu_draw_text(box_x + 24, y, ">", COL_SELECT);
		menu_draw_text(box_x + 44, y, menu_items[i], c);
	}

	menu_draw_text_center(MENU_W / 2, box_y + box_h + 16,
		"D-pad:Move  O:Select  X:Resume", COL_DIM);
}

/* =====================================================================
	PS2 settings menu

	Mirrors the per-system options the PSP build exposes (see
	src/psp/menu/*.c and src/psp/config/*.c): option list differs per
	EMU_SYSTEM, values change immediately.  A "Key Config" and an
	"Autofire" submenu let the user rebind buttons / autofire.
	===================================================================== */

/* Option variables that live outside emumain.h. */
extern int cps_raster_enable;
extern int cps_rotate_screen;
extern int neogeo_region;
extern int neogeo_machine_mode;
extern int neogeo_raster_enable;
extern int neogeo_loadscreen;
extern int neogeo_cdspeed_limit;
extern int option_mp3_enable;
extern int option_mp3_volume;

/* Value string tables (index = option value). */
static const char *s_onoff[]  = { "OFF", "ON" };
static const char *s_noyes[]  = { "NO", "YES" };
static const char *s_dise[]   = { "DISABLE", "ENABLE" };
static const char *s_rate[]   = { "11kHz", "22kHz", "44kHz" };
static const char *s_vol[]    = { "0","10","20","30","40","50","60","70","80","90","100" };
static const char *s_skip[]   = { "OFF","1","2","3","4","5","6","7","8","9","10" };
static const char *s_region_mvs[]  = { "DEFAULT", "JAPAN", "USA", "EUROPE" };
static const char *s_region_ncdz[] = { "JAPAN", "USA", "EUROPE" };
static const char *s_mode[]   = { "DEFAULT", "AES", "MVS" };

typedef struct {
	const char *label;
	int *var;
	const char *const *strs;
	int count;
} sett_item_t;

static sett_item_t sett_items[] = {
#if (EMU_SYSTEM == MVS)
	{ "Region",         &neogeo_region,        s_region_mvs,  4 },
	{ "Machine Mode",   &neogeo_machine_mode,  s_mode,        3 },
	{ "Raster Effects", &neogeo_raster_enable, s_onoff,       2 },
#elif (EMU_SYSTEM == NCDZ)
	{ "Region",         &neogeo_region,        s_region_ncdz, 3 },
	{ "Raster Effects", &neogeo_raster_enable, s_onoff,       2 },
	{ "Load Screen",    &neogeo_loadscreen,    s_noyes,       2 },
	{ "CD Speed",       &neogeo_cdspeed_limit, s_noyes,       2 },
#elif (EMU_SYSTEM == CPS1)
	{ "Raster Effects", &cps_raster_enable,    s_onoff,       2 },
	{ "Rotate Screen",  &cps_rotate_screen,    s_noyes,       2 },
#endif
	{ "Video Sync",     &option_vsync,         s_onoff,       2 },
	{ "Auto Frameskip", &option_autoframeskip, s_dise,        2 },
	{ "Frameskip",      &option_frameskip,     s_skip,        11 },
	{ "Show FPS",       &option_showfps,       s_onoff,       2 },
	{ "Frame Limit",    &option_speedlimit,    s_onoff,       2 },
	{ "Enable Sound",   &option_sound_enable,  s_noyes,       2 },
#if (EMU_SYSTEM == CPS1) || (EMU_SYSTEM == MVS) || (EMU_SYSTEM == NCDZ)
	{ "Sample Rate",    &option_samplerate,    s_rate,        3 },
#endif
	{ "Volume",         &option_sound_volume,  s_vol,         11 },
#if (EMU_SYSTEM == NCDZ)
	{ "Enable CDDA",    &option_mp3_enable,    s_noyes,       2 },
	{ "CDDA Volume",    &option_mp3_volume,    s_vol,         11 },
#endif
};
#define SETT_COUNT ((int)(sizeof(sett_items) / sizeof(sett_items[0])))
#define SETT_ROWS  (SETT_COUNT + 2)   /* + Key Config, Autofire */

/* Button re-config list. */
typedef struct { const char *label; int input_idx; } key_cfg_item_t;
#if (EMU_SYSTEM == MVS)
static const key_cfg_item_t key_items[] = {
	{ "A",      P1_BUTTONA },
	{ "B",      P1_BUTTONB },
	{ "C",      P1_BUTTONC },
	{ "D",      P1_BUTTOND },
	{ "Start",  P1_START },
	{ "Coin",   P1_COIN },
};
#elif (EMU_SYSTEM == NCDZ)
static const key_cfg_item_t key_items[] = {
	{ "A",      P1_BUTTONA },
	{ "B",      P1_BUTTONB },
	{ "C",      P1_BUTTONC },
	{ "D",      P1_BUTTOND },
	{ "Start",  P1_START },
	{ "Select", P1_SELECT },
};
#else
static const key_cfg_item_t key_items[] = {
	{ "Button1", P1_BUTTON1 },
	{ "Button2", P1_BUTTON2 },
	{ "Button3", P1_BUTTON3 },
	{ "Button4", P1_BUTTON4 },
	{ "Start",   P1_START },
	{ "Coin",    P1_COIN },
};
#endif
#define KEY_COUNT ((int)(sizeof(key_items) / sizeof(key_items[0])))

/* Autofire rows: interval + the four action buttons. */
#if (EMU_SYSTEM == MVS) || (EMU_SYSTEM == NCDZ)
#define AF_BTN(i) (P1_BUTTONA + (i))
#define AF_AF(i)  (P1_AF_A + (i))
#else
#define AF_BTN(i) (P1_BUTTON1 + (i))
#define AF_AF(i)  (P1_AF_1 + (i))
#endif
#define AF_COUNT 4

static int menu_panel_w(void)
{
#if (EMU_SYSTEM == MVS) || (EMU_SYSTEM == NCDZ)
	return 264;
#else
	return 290;
#endif
}

static const char *pad_label(uint32_t pad)
{
	switch (pad) {
	case PLATFORM_PAD_UP:     return "D-Pad Up";
	case PLATFORM_PAD_DOWN:   return "D-Pad Down";
	case PLATFORM_PAD_LEFT:   return "D-Pad Left";
	case PLATFORM_PAD_RIGHT:  return "D-Pad Right";
	case PLATFORM_PAD_B1:     return "Circle";
	case PLATFORM_PAD_B2:     return "Cross";
	case PLATFORM_PAD_B3:     return "Square";
	case PLATFORM_PAD_B4:     return "Triangle";
	case PLATFORM_PAD_L:      return "L1";
	case PLATFORM_PAD_R:      return "R1";
	case PLATFORM_PAD_START:  return "Start";
	case PLATFORM_PAD_SELECT: return "Select";
	default:                  return "---";
	}
}

static void sett_draw_row(int idx, int y, const char *label, const char *val, int sel)
{
	uint16_t c = sel ? COL_SELECT : COL_NORMAL;
	if (sel)
		menu_draw_text(LIST_X - 16, y, ">", COL_SELECT);
	menu_draw_text(LIST_X, y, label, c);
	if (val)
	{
		int vw = menu_text_width(val);
		menu_draw_text(16 + menu_panel_w() - 12 - vw, y, val, c);
	}
}

/* ------------------------------------------------------------------
	Autofire submenu
	------------------------------------------------------------------ */
static void autofire_menu(void)
{
	int rows = 1 + AF_COUNT;
	int sel = 0;
	uint32_t prev = poll_gamepad();
	int done = 0;

	while (!done)
	{
		uint32_t pad = poll_gamepad();
		uint32_t pressed = pad & ~prev;
		prev = pad;

		if (pressed & (PLATFORM_PAD_UP | PLATFORM_PAD_L))
			sel = (sel + rows - 1) % rows;
		else if (pressed & (PLATFORM_PAD_DOWN | PLATFORM_PAD_R))
			sel = (sel + 1) % rows;
		else if (pressed & (PLATFORM_PAD_LEFT | PLATFORM_PAD_RIGHT))
		{
			if (sel == 0)
			{
				extern int af_interval;
				int d = (pressed & PLATFORM_PAD_LEFT) ? -1 : 1;
				af_interval += d;
				if (af_interval < 0) af_interval = 0;
				if (af_interval > 10) af_interval = 10;
			}
			else
			{
				int i = sel - 1;
				extern int input_map[];
				int af_idx = AF_AF(i);
				if (input_map[af_idx])
					input_map[af_idx] = 0;
				else
					input_map[af_idx] = input_map[AF_BTN(i)];
			}
		}
		else if (pressed & PLATFORM_PAD_B2)
			done = 1;

		/* render */
		menu_clear(COL_BG);
		draw_panel(16, 16, menu_panel_w(), MENU_H - 40);
		menu_draw_text(28, 26, "AUTOFIRE", COL_TITLE);
		{
			extern int af_interval;
			int y = LIST_Y;
			char buf[32];
			snprintf(buf, sizeof(buf), "%d", af_interval);
			sett_draw_row(0, y, "Interval", buf, sel == 0);
			for (int i = 0; i < AF_COUNT; i++)
			{
				extern int input_map[];
				int af_idx = AF_AF(i);
				const char *lbl = (EMU_SYSTEM == MVS || EMU_SYSTEM == NCDZ)
						? (i == 0 ? "A" : i == 1 ? "B" : i == 2 ? "C" : "D")
						: (i == 0 ? "Button1" : i == 1 ? "Button2" : i == 2 ? "Button3" : "Button4");
				sett_draw_row(0, y + (i + 1) * LINE_H, lbl,
						input_map[af_idx] ? "ON" : "OFF", sel == i + 1);
			}
		}
		menu_draw_text_center(MENU_W / 2, MENU_H - 14, "L/R:Toggle  X:Back", COL_DIM);
		menu_present();
	}
}

/* ------------------------------------------------------------------
	Key config submenu
	------------------------------------------------------------------ */
static void key_config_menu(void)
{
	int sel = 0;
	int binding = -1;
	uint32_t prev = poll_gamepad();
	int done = 0;

	while (!done)
	{
		uint32_t pad = poll_gamepad();
		uint32_t pressed = pad & ~prev;
		prev = pad;

		if (binding >= 0)
		{
			/* wait for a fresh non-d-pad press */
			uint32_t btns = pad & (PLATFORM_PAD_B1 | PLATFORM_PAD_B2 |
					       PLATFORM_PAD_B3 | PLATFORM_PAD_B4 |
					       PLATFORM_PAD_L | PLATFORM_PAD_R |
					       PLATFORM_PAD_START | PLATFORM_PAD_SELECT);
			if (btns)
			{
				uint32_t key = 0;
				for (int i = 4; i < 12; i++)
				{
					if (btns & (1u << i))
					{
						key = (1u << i);
						break;
					}
				}
				extern int input_map[];
				input_map[key_items[binding].input_idx] = key;
				binding = -1;
			}
		}
		else
		{
			if (pressed & (PLATFORM_PAD_UP | PLATFORM_PAD_L))
				sel = (sel + KEY_COUNT - 1) % KEY_COUNT;
			else if (pressed & (PLATFORM_PAD_DOWN | PLATFORM_PAD_R))
				sel = (sel + 1) % KEY_COUNT;
			else if (pressed & PLATFORM_PAD_B1)
				binding = sel;   /* Circle: bind */
			else if (pressed & PLATFORM_PAD_B2)
				done = 1;        /* Cross: back */
		}

		/* render */
		menu_clear(COL_BG);
		draw_panel(16, 16, menu_panel_w(), MENU_H - 40);
		menu_draw_text(28, 26, "KEY CONFIG", COL_TITLE);
		{
			extern int input_map[];
			int y = LIST_Y;
			for (int i = 0; i < KEY_COUNT; i++)
			{
				char val[32];
				snprintf(val, sizeof(val), "[%s]",
					 pad_label((uint32_t)input_map[key_items[i].input_idx]));
				if (binding == i)
					sett_draw_row(0, y, key_items[i].label,
						      "[PRESS KEY]", 1);
				else
					sett_draw_row(0, y, key_items[i].label, val,
						      sel == i);
				y += LINE_H;
			}
		}
		menu_draw_text_center(MENU_W / 2, MENU_H - 14,
			binding >= 0 ? "Press new button..." : "O:Bind  X:Back", COL_DIM);
		menu_present();
	}
}

/* ------------------------------------------------------------------
	Main settings menu
	------------------------------------------------------------------ */
static void settings_menu(void)
{
	int sel = 0, scroll = 0;
	uint32_t prev = poll_gamepad();
	int done = 0;

	while (!done)
	{
		uint32_t pad = poll_gamepad();
		uint32_t pressed = pad & ~prev;
		prev = pad;

		if (pressed & (PLATFORM_PAD_UP | PLATFORM_PAD_L))
			sel = (sel + SETT_ROWS - 1) % SETT_ROWS;
		else if (pressed & (PLATFORM_PAD_DOWN | PLATFORM_PAD_R))
			sel = (sel + 1) % SETT_ROWS;
		else if (pressed & (PLATFORM_PAD_LEFT | PLATFORM_PAD_RIGHT))
		{
			if (sel < SETT_COUNT)
			{
				int d = (pressed & PLATFORM_PAD_LEFT) ? -1 : 1;
				int nv = (*sett_items[sel].var) + d;
				if (nv < 0) nv = sett_items[sel].count - 1;
				if (nv >= sett_items[sel].count) nv = 0;
				*sett_items[sel].var = nv;
			}
		}
		else if (pressed & PLATFORM_PAD_B1)      /* Circle: enter */
		{
			if (sel == SETT_COUNT)
				key_config_menu();
			else if (sel == SETT_COUNT + 1)
				autofire_menu();
		}
		else if (pressed & PLATFORM_PAD_B2)      /* Cross: back */
			done = 1;

		if (sel < scroll) scroll = sel;
		else if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;

		/* render */
		menu_clear(COL_BG);
		draw_panel(16, 16, menu_panel_w(), MENU_H - 40);
		menu_draw_text(28, 26, "SETTINGS", COL_TITLE);
		for (int i = 0; i < VISIBLE && (scroll + i) < SETT_ROWS; i++)
		{
			int idx = scroll + i;
			int y = LIST_Y + i * LINE_H;
			if (idx < SETT_COUNT)
			{
				const sett_item_t *it = &sett_items[idx];
				int v = *it->var;
				if (v < 0) v = 0;
				if (v >= it->count) v = it->count - 1;
				sett_draw_row(0, y, it->label, it->strs[v], idx == sel);
			}
			else if (idx == SETT_COUNT)
				sett_draw_row(0, y, "Key Config", NULL, idx == sel);
			else
				sett_draw_row(0, y, "Autofire", NULL, idx == sel);
		}
		menu_draw_text_center(MENU_W / 2, MENU_H - 14,
			"L/R:Change  O:Enter  X:Back", COL_DIM);
		menu_present();
	}
}

void showmenu(void)
{
	uint32_t prev = poll_gamepad(); /* seed: don't treat the opening
	                                    Start+Select as a new press */
	int sel = 0;
	int done = 0;

	/* In-game menu is a menu: disable the edge latch (see ps2_input.c). */
	ps2_input_set_latch(0);

	/* Mute the game audio while the in-game menu is open.
	 * sound_mute() only stops the emulator's mix thread; the PS2 audsrv
	 * DMA buffer keeps playing already-queued audio, so we must also stop
	 * the audsrv stream (and flush its ring). */
	extern void sound_mute(int mute);
	extern int audsrv_set_volume(int volume);
	sound_mute(1);
	audsrv_set_volume(0);

	while (!done)
	{
		uint32_t pad = poll_gamepad();
		uint32_t pressed = pad & ~prev;
		prev = pad;

		if (pressed & (PLATFORM_PAD_B1 | PLATFORM_PAD_START))
		{
			switch (sel)
			{
				case 0: done = 1; break;                       /* Resume */
				case 1: settings_menu(); sel = 0;
					prev = poll_gamepad();         break; /* Settings: absorb stray X */
				case 2: Loop = LOOP_RESET;    done = 1; break; /* Reset  */
				case 3: Loop = LOOP_BROWSER; done = 1; break; /* Browser*/
				case 4: Loop = LOOP_EXIT;     done = 1; break; /* Exit   */
			}
		}
		else if (pressed & PLATFORM_PAD_B2)
		{
			done = 1; /* resume */
		}
		else if (pressed & (PLATFORM_PAD_UP | PLATFORM_PAD_L))
		{
			sel = (sel + MENU_N - 1) % MENU_N;
		}
		else if (pressed & (PLATFORM_PAD_DOWN | PLATFORM_PAD_R))
		{
			sel = (sel + 1) % MENU_N;
		}

		draw_menu(sel);
		menu_present();
	}

	/* Restore game audio when leaving the in-game menu: unmute the
	 * emulator mix thread and bring the audsrv volume back up.
	 * The audsrv stream itself is never stopped - audsrv has no
	 * start_audio(), and while it is stopped audsrv_wait_audio() never
	 * returns (that was the no-sound-then-hard-lock bug). */
	extern int audsrv_set_volume(int volume);
	sound_mute(0);
	audsrv_set_volume(100);

	/* Back to the game loop: re-enable the edge latch. */
	ps2_input_set_latch(1);

	/* Clear the last menu frame from the display: the game frame below
	 * is rendered into the *other* buffer and its own flip will not
	 * necessarily repaint this one, leaving the MENU UI on screen while
	 * the game runs (audio keeps playing). Forcing one vsync'd flip here
	 * advances the display cleanly before the core resumes. */
	video_driver->flipScreen(video_data, 1);
}
