/******************************************************************************

	ps2_png.c

	PS2 implementation of load_png() for the NJEMU NCDZ target.

	The upstream tree only ships load_png() for the PSP port (src/psp/png.c).
	NCDZ's cdrom.c (show_loading_image) calls it to draw the "loading.png"
	splash while the CD image is being accessed. This file provides a real
	PS2 implementation:

	  1. read the PNG file from the PS2 filesystem,
	  2. decode it with zlib (inflate) + the PNG unfilter step,
	  3. convert the pixels to GS_PSM_CT16 (ABGR1555),
	  4. upload the texture via gsKit and present it full-screen.

	On any failure it returns non-zero so the caller falls back to the plain
	"Now Loading..." text splash.

******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "common/video_driver.h"

#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <gsInline.h>

/* Defined in ps2_video.c and used by the menu; declare it here so this
   translation unit can call it. */
void ps2_present_texture(GSGLOBAL *gsGlobal, GSTEXTURE *tex, gs_rgbaq color);

/* ---- Paeth predictor (PNG filter type 4) ---- */
static int paeth(int a, int b, int c)
{
	int p = a + b - c;
	int pa = abs(p - a);
	int pb = abs(p - b);
	int pc = abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	if (pb <= pc) return b;
	return c;
}

static uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

int load_png(const char *name, int number)
{
	(void)number;

	FILE *fp = fopen(name, "rb");
	if (!fp) return 1;

	fseek(fp, 0, SEEK_END);
	long fsz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (fsz <= 0) { fclose(fp); return 1; }

	uint8_t *file = (uint8_t *)malloc((size_t)fsz);
	if (!file) { fclose(fp); return 1; }
	if (fread(file, 1, (size_t)fsz, fp) != (size_t)fsz) {
		free(file); fclose(fp); return 1;
	}
	fclose(fp);

	/* PNG signature */
	static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	if ((size_t)fsz < 8 || memcmp(file, sig, 8) != 0) { free(file); return 1; }

	uint32_t width = 0, height = 0;
	int bit_depth = 0, color_type = 0, has_ihdr = 0;
	uint8_t *idat = NULL; size_t idat_len = 0;
	uint8_t *plte = NULL; int plte_len = 0;

	size_t off = 8;
	while (off + 8 <= (size_t)fsz) {
		uint32_t len = read_be32(&file[off]);
		char type[5];
		memcpy(type, &file[off + 4], 4);
		type[4] = '\0';
		uint8_t *data = &file[off + 8];

		if (strcmp(type, "IHDR") == 0 && len >= 13) {
			width = read_be32(&data[0]);
			height = read_be32(&data[4]);
			bit_depth = data[8];
			color_type = data[9];
			has_ihdr = 1;
		} else if (strcmp(type, "PLTE") == 0) {
			free(plte);
			plte = (uint8_t *)malloc(len);
			if (plte) { memcpy(plte, data, len); plte_len = (int)len; }
		} else if (strcmp(type, "IDAT") == 0) {
			uint8_t *nidat = (uint8_t *)realloc(idat, idat_len + len);
			if (!nidat) { /* keep going with what we have */ }
			else {
				idat = nidat;
				memcpy(idat + idat_len, data, len);
				idat_len += len;
			}
		} else if (strcmp(type, "IEND") == 0) {
			break;
		}

		if (len > (size_t)fsz - (off + 12)) break;
		off += 12 + len;
	}

	if (!has_ihdr || !idat || width == 0 || height == 0) {
		free(idat); free(plte); free(file); return 1;
	}

	/* Inflate the IDAT stream (PNG uses standard zlib). */
	uLongf out_sz = (uLongf)(width * height * 4 + 4096);
	uint8_t *raw = (uint8_t *)malloc(out_sz);
	if (!raw) { free(idat); free(plte); free(file); return 1; }
	int zr = uncompress(raw, &out_sz, idat, (uLong)idat_len);
	free(idat);
	if (zr != Z_OK) { free(raw); free(plte); free(file); return 1; }

	int channels;
	switch (color_type) {
		case 0: channels = 1; break; /* grayscale      */
		case 2: channels = 3; break; /* RGB            */
		case 3: channels = 1; break; /* palette index  */
		case 4: channels = 2; break; /* gray + alpha   */
		case 6: channels = 4; break; /* RGBA           */
		default: free(raw); free(plte); free(file); return 1;
	}
	if (bit_depth != 8) { free(raw); free(plte); free(file); return 1; }

	uint32_t stride = width * (uint32_t)channels;
	uint8_t *image = (uint8_t *)malloc((size_t)height * stride);
	if (!image) { free(raw); free(plte); free(file); return 1; }

	/* Unfilter the scanlines. */
	uint8_t *prev = (uint8_t *)calloc(stride, 1);
	uint8_t *cur  = (uint8_t *)malloc(stride);
	size_t p = 0;
	int err = 0;
	for (uint32_t y = 0; y < height; y++) {
		if (p >= out_sz) { err = 1; break; }
		int ft = raw[p++];
		memcpy(cur, &raw[p], stride);
		p += stride;
		switch (ft) {
			case 0: /* None */ break;
			case 1: /* Sub */
				for (uint32_t i = 0; i < stride; i++) {
					uint8_t a = (i >= (uint32_t)channels) ? cur[i - channels] : 0;
					cur[i] = (uint8_t)(cur[i] + a);
				}
				break;
			case 2: /* Up */
				for (uint32_t i = 0; i < stride; i++)
					cur[i] = (uint8_t)(cur[i] + prev[i]);
				break;
			case 3: /* Average */
				for (uint32_t i = 0; i < stride; i++) {
					uint8_t a = (i >= (uint32_t)channels) ? cur[i - channels] : 0;
					cur[i] = (uint8_t)(cur[i] + ((a + prev[i]) >> 1));
				}
				break;
			case 4: /* Paeth */
				for (uint32_t i = 0; i < stride; i++) {
					uint8_t a = (i >= (uint32_t)channels) ? cur[i - channels] : 0;
					uint8_t c = (i >= (uint32_t)channels) ? prev[i - channels] : 0;
					cur[i] = (uint8_t)(cur[i] + (uint8_t)paeth(a, prev[i], c));
				}
				break;
			default: err = 1; break;
		}
		if (err) break;
		memcpy(image + (size_t)y * stride, cur, stride);
		memcpy(prev, cur, stride);
	}
	free(prev); free(cur); free(raw);
	if (err) { free(image); free(plte); free(file); return 1; }

	/* Convert to GS_PSM_CT16 (ABGR1555: R in low 5 bits). */
	uint16_t *fb = (uint16_t *)malloc((size_t)width * height * 2);
	if (!fb) { free(image); free(plte); free(file); return 1; }

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = image + (size_t)y * stride;
		for (uint32_t x = 0; x < width; x++) {
			int r, g, b;
			const uint8_t *px = row + (size_t)x * channels;
			switch (color_type) {
				case 0: r = g = b = px[0]; break;
				case 2: r = px[0]; g = px[1]; b = px[2]; break;
				case 4: r = g = b = px[0]; break; /* alpha px[1] ignored */
				case 6: r = px[0]; g = px[1]; b = px[2]; break;
				case 3: {
					int idx = px[0] * 3;
					if (plte && idx + 2 < plte_len) {
						r = plte[idx]; g = plte[idx + 1]; b = plte[idx + 2];
					} else { r = g = b = 0; }
					break;
				}
				default: r = g = b = 0; break;
			}
			fb[(size_t)y * width + x] =
				(uint16_t)(0x8000 |
				           ((r >> 3) << 0) |
				           ((g >> 3) << 5) |
				           ((b >> 3) << 10));
		}
	}
	free(image);
	free(plte);
	free(file);

	/* Upload + present via gsKit (mirrors the menu path in ps2_gui.c). */
	GSGLOBAL *gsGlobal = (GSGLOBAL *)video_driver->getNativeObjects(
		video_data, COMMON_GRAPHIC_OBJECTS_GLOBAL_CONTEXT);
	if (!gsGlobal) { free(fb); return 1; }

	GSTEXTURE *tex = (GSTEXTURE *)calloc(1, sizeof(GSTEXTURE));
	if (!tex) { free(fb); return 1; }
	tex->Width  = (int)width;
	tex->Height = (int)height;
	tex->PSM    = GS_PSM_CT16;
	tex->Filter = GS_FILTER_NEAREST;
	tex->Mem    = (void *)fb;
	tex->Vram = gsKit_vram_alloc(gsGlobal,
		gsKit_texture_size((int)width, (int)height, GS_PSM_CT16),
		GSKIT_ALLOC_USERBUFFER);
	gsKit_setup_tbw(tex);
	gsKit_texture_send((u32 *)fb, (int)width, (int)height,
		tex->Vram, GS_PSM_CT16, tex->TBW, 0);

	gs_rgbaq col = color_to_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);
	ps2_present_texture(gsGlobal, tex, col);

	/* free() of fb/tex is intentionally skipped: the GS queued the draw and
	   may still reference the buffer until the next flip. It is a one-shot
	   splash, so the small leak is harmless. */

	return 0;
}
