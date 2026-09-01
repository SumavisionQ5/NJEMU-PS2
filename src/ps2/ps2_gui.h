/******************************************************************************

	ps2_gui.h

	PS2 menu / UI drawing API (CPU framebuffer based).

	The menu is drawn into a plain 16-bit (GS_PSM_CT16 / RGBA16) framebuffer
	in main RAM. ps2_gui.c uploads that buffer as a single texture and blits
	it full-screen via gsKit, mirroring how the emulator presents its own
	scrbitmap. This keeps all menu drawing in portable C (no GS calls) and
	limits PS2-specific code to one present call.

******************************************************************************/

#ifndef PS2_GUI_H
#define PS2_GUI_H

#include <stdint.h>

/* Menu VIEW size = native framebuffer size (1:1, no scaling):
 *   CPS1/CPS2 -> 384x224 (224p), MVS/NCDZ -> 320x240 (240p).
 * The 8px font renders native-sharp. The raster buffer is 512x256
 * (power of two, required by GS TEX0 TW/TH); only the MENU_W x MENU_H
 * view-sized sub-region is blitted 1:1. Layout constants below are all
 * relative to MENU_W/MENU_H so nothing is clipped. */
#if (EMU_SYSTEM == MVS) || (EMU_SYSTEM == NCDZ)
#define MENU_W 320
#define MENU_H 240
#else
#define MENU_W 384
#define MENU_H 224
#endif

/* Texture/buffer size MUST stay a power of two (GS TEX0 TW/TH are log2). */
#define MENU_TEX_W 512
#define MENU_TEX_H 256

/* RGBA8888 (CT32) framebuffer the menu is rasterised into. */
extern uint32_t g_menu_fb[MENU_TEX_W * MENU_TEX_H];

/* Pack 0-255 R,G,B into a GS CT16 pixel (5R:5G:5B:1A).
 * NOTE: alpha bit = 0 (NOT 1!). The game renderer leaves the alpha test at
 * ATST=4 (EQUAL) / AREF=0, i.e. only pixels with alpha==0 are drawn. The game
 * frame passes because its pixels have alpha=0; the menu must do the same or
 * every menu pixel is filtered out -> black screen. */
static inline uint32_t menu_rgb(int r, int g, int b)
{
	/* CT32 (RGBA8888), alpha = 0 so the menu passes the game renderer's
	 * default alpha test (ATST=4 EQUAL / AREF=0) exactly like the game
	 * frame does. 32bpp linear textures are the gsKit PNG-proven path. */
	return ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r);
}

void menu_fb_init(void);
void menu_clear(uint32_t color);
void menu_fill_rect(int x, int y, int w, int h, uint32_t color);
void menu_draw_char(int x, int y, char c, uint32_t color);
void menu_draw_text(int x, int y, const char *s, uint32_t color);
void menu_draw_text_center(int cx, int y, const char *s, uint32_t color);
int  menu_text_width(const char *s);

#endif /* PS2_GUI_H */
