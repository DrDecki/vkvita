/*
 * VkVita: boot screen shown while the app loads, until its first present
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"
#include "vkv_splash_logo.h"
#include "vkv_splash_font.h"

#define VKV_SPLASH_W      960
#define VKV_SPLASH_H      544
#define VKV_SPLASH_BYTES  (2u * 1024 * 1024)

#define VKV_SPLASH_BG     0xFFFFFFFFu
#define VKV_SPLASH_TRACK  0xFFE6E6E6u
#define VKV_SPLASH_FADE_MS 500u

#define VKV_SPLASH_LOGO_X ((VKV_SPLASH_W - VKV_SPLASH_LOGO_W) / 2)
#define VKV_SPLASH_LOGO_Y ((VKV_SPLASH_H - VKV_SPLASH_LOGO_H) / 2 - 24)
#define VKV_SPLASH_BAR_W  160
#define VKV_SPLASH_BAR_H  4
#define VKV_SPLASH_SEG_W  48
#define VKV_SPLASH_BAR_X  ((VKV_SPLASH_W - VKV_SPLASH_BAR_W) / 2)
#define VKV_SPLASH_BAR_Y  (VKV_SPLASH_LOGO_Y + VKV_SPLASH_LOGO_H + 48)
#define VKV_SPLASH_BAR_PERIOD_MS 1600u

static struct {
	SceUID   memUid;
	uint32_t *fb;
	uint8_t  *mask;
	SceUID   thread;
	volatile int stop;
	uint32_t presents;
	uint32_t color;            /* 0x00BBGGRR */
	PFN_vkvSplashDraw draw;    /* custom boot screen, NULL = the VkVita one */
	void    *drawUser;
} s_splash = { .memUid = -1, .thread = -1, .color = VKV_SPLASH_LOGO_COLOR };

void vkvSetSplashDraw(PFN_vkvSplashDraw draw, void *user) {
	s_splash.draw = draw;
	s_splash.drawUser = user;
}

void vkvSplashText(uint32_t *pixels, uint32_t pitch, int x, int y, int scale, uint32_t color, const char *text) {
	if (!pixels || !text) return;
	if (scale < 1) scale = 1;
	int cx = x;
	for (; *text; text++) {
		if (*text == '\n') {
			cx = x;
			y += 16 * scale;
			continue;
		}
		unsigned char ch = (unsigned char)*text;
		const unsigned char *g = k_splashFont + (ch >= 32 && ch < 127 ? ch - 32 : 0) * 16;
		for (int gy = 0; gy < 16 * scale; gy++) {
			int py = y + gy;
			if (py < 0 || py >= VKV_SPLASH_H) continue;
			unsigned bits = g[gy / scale];
			if (!bits) continue;
			for (int gx = 0; gx < 8 * scale; gx++) {
				int px = cx + gx;
				if (px >= 0 && px < VKV_SPLASH_W && (bits & (0x80u >> (gx / scale)))) pixels[py * (int)pitch + px] = color;
			}
		}
		cx += 8 * scale;
	}
}

static uint32_t vkv_splash_mix(uint32_t bg, uint32_t fg, uint32_t a) {
	uint32_t out = 0xFF000000u;
	for (int s = 0; s < 24; s += 8) {
		uint32_t b = (bg >> s) & 0xFF, f = (fg >> s) & 0xFF;
		out |= ((b * (255 - a) + f * a + 127) / 255) << s;
	}
	return out;
}

/* Smoothstep on 0..1024. */
static uint32_t vkv_splash_ease(uint32_t t) {
	if (t >= 1024) return 1024;
	return (t * t * (3 * 1024 - 2 * t)) >> 20;
}

static void vkv_splash_draw_logo(uint32_t *fb, uint32_t pitch, const uint8_t *mask, uint32_t fade) {
	uint32_t color = 0xFF000000u | s_splash.color;
	for (uint32_t y = 0; y < VKV_SPLASH_LOGO_H; y++) {
		uint32_t *row = fb + (VKV_SPLASH_LOGO_Y + y) * pitch + VKV_SPLASH_LOGO_X;
		const uint8_t *m = mask + y * VKV_SPLASH_LOGO_W;
		for (uint32_t x = 0; x < VKV_SPLASH_LOGO_W; x++)
			row[x] = m[x] ? vkv_splash_mix(VKV_SPLASH_BG, color, (m[x] * fade) >> 10) : VKV_SPLASH_BG;
	}
}

static void vkv_splash_draw_bar(uint32_t *fb, uint32_t pitch, uint32_t ms, uint32_t fade) {
	uint32_t color = 0xFF000000u | s_splash.color;
	uint32_t track = vkv_splash_mix(VKV_SPLASH_BG, VKV_SPLASH_TRACK, (255 * fade) >> 10);
	uint32_t seg = vkv_splash_mix(VKV_SPLASH_BG, color, (255 * fade) >> 10);
	uint32_t phase = (ms % VKV_SPLASH_BAR_PERIOD_MS) * 2048 / VKV_SPLASH_BAR_PERIOD_MS;
	uint32_t t = phase < 1024 ? phase : 2048 - phase;
	uint32_t x0 = vkv_splash_ease(t) * (VKV_SPLASH_BAR_W - VKV_SPLASH_SEG_W) >> 10;
	for (uint32_t y = 0; y < VKV_SPLASH_BAR_H; y++) {
		uint32_t *row = fb + (VKV_SPLASH_BAR_Y + y) * pitch + VKV_SPLASH_BAR_X;
		for (uint32_t x = 0; x < VKV_SPLASH_BAR_W; x++)
			row[x] = x >= x0 && x < x0 + VKV_SPLASH_SEG_W ? seg : track;
	}
}

void vkv_splash_draw(uint32_t *fb, uint32_t pitch, const uint8_t *mask, uint32_t ms, bool full) {
	if (full)
		for (uint32_t i = 0; i < pitch * VKV_SPLASH_H; i++) fb[i] = VKV_SPLASH_BG;
	if (full || ms <= VKV_SPLASH_FADE_MS + 40)
		vkv_splash_draw_logo(fb, pitch, mask, vkv_splash_ease(ms * 1024 / VKV_SPLASH_FADE_MS));
	uint32_t barMs = ms > VKV_SPLASH_FADE_MS / 2 ? ms - VKV_SPLASH_FADE_MS / 2 : 0;
	vkv_splash_draw_bar(fb, pitch, barMs, vkv_splash_ease(barMs * 1024 / VKV_SPLASH_FADE_MS));
}

uint8_t *vkv_splash_decode_mask(void) {
	uint8_t *mask = malloc(VKV_SPLASH_LOGO_W * VKV_SPLASH_LOGO_H);
	if (!mask) return NULL;
	uint32_t o = 0;
	for (uint32_t i = 0; i + 1 < sizeof(k_splashLogoRle); i += 2) {
		memset(mask + o, k_splashLogoRle[i + 1], k_splashLogoRle[i]);
		o += k_splashLogoRle[i];
	}
	return mask;
}

static int vkv_splash_thread(SceSize args, void *argp) {
	(void)args; (void)argp;
	SceDisplayFrameBuf fb;
	memset(&fb, 0, sizeof(fb));
	fb.size        = sizeof(fb);
	fb.base        = s_splash.fb;
	fb.pitch       = VKV_SPLASH_W;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width       = VKV_SPLASH_W;
	fb.height      = VKV_SPLASH_H;
	uint64_t start = sceKernelGetProcessTimeWide();
	if (s_splash.draw) {
		memset(s_splash.fb, 0, VKV_SPLASH_W * VKV_SPLASH_H * 4);
		s_splash.draw(s_splash.fb, VKV_SPLASH_W, 0, s_splash.drawUser);
	} else {
		vkv_splash_draw(s_splash.fb, VKV_SPLASH_W, s_splash.mask, 0, true);
	}
	sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
	while (!s_splash.stop) {
		sceDisplayWaitVblankStart();
		uint32_t ms = (uint32_t)((sceKernelGetProcessTimeWide() - start) / 1000);
		if (s_splash.draw) s_splash.draw(s_splash.fb, VKV_SPLASH_W, ms, s_splash.drawUser);
		else vkv_splash_draw(s_splash.fb, VKV_SPLASH_W, s_splash.mask, ms, false);
	}
	return 0;
}

void vkvShowSplash(void) {
	vkv_config_init_once();
	if (!g_vkv.cfg.splash || s_splash.thread >= 0 || s_splash.presents) return;
	s_splash.memUid = sceKernelAllocMemBlock("vkv_splash", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
	                                         VKV_SPLASH_BYTES, NULL);
	if (s_splash.memUid < 0) return;
	sceKernelGetMemBlockBase(s_splash.memUid, (void **)&s_splash.fb);
	uint32_t rgb = g_vkv.cfg.splashColor;
	s_splash.color = rgb ? ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF) : VKV_SPLASH_LOGO_COLOR;
	s_splash.mask = s_splash.draw ? NULL : vkv_splash_decode_mask();
	s_splash.stop = 0;
	s_splash.thread = s_splash.mask || s_splash.draw ? sceKernelCreateThread("vkv_splash", vkv_splash_thread, 0x10000100,
	                                                        0x4000, 0, 0, NULL) : -1;
	if (s_splash.thread < 0 || sceKernelStartThread(s_splash.thread, 0, NULL) < 0) {
		if (s_splash.thread >= 0) sceKernelDeleteThread(s_splash.thread);
		s_splash.thread = -1;
		free(s_splash.mask);
		s_splash.mask = NULL;
		sceKernelFreeMemBlock(s_splash.memUid);
		s_splash.memUid = -1;
	}
}

static void vkv_splash_stop(void) {
	if (s_splash.thread < 0) return;
	s_splash.stop = 1;
	sceKernelWaitThreadEnd(s_splash.thread, NULL, NULL);
	sceKernelDeleteThread(s_splash.thread);
	s_splash.thread = -1;
	free(s_splash.mask);
	s_splash.mask = NULL;
}

static void vkv_splash_free(void) {
	vkv_splash_stop();
	if (s_splash.memUid >= 0) sceKernelFreeMemBlock(s_splash.memUid);
	s_splash.memUid = -1;
	s_splash.fb = NULL;
}

/* The last splash frame stays on screen until the app's first frame is flipped
 * in, so the memory is only released a few presents later. */
void vkv_splash_on_present(void) {
	if (s_splash.presents > 4) return;
	s_splash.presents++;
	if (s_splash.presents == 1) vkv_splash_stop();
	else if (s_splash.presents == 4) vkv_splash_free();
}

void vkv_splash_shutdown(void) {
	vkv_splash_free();
}
