/*
 * VkVita: queue submission (command replay onto the GXM context),
 * swapchain and presentation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

#include <math.h>

typedef struct VkvReplay {
	struct VkDevice_T        *dev;
	SceGxmContext            *ctx;

	/* render pass */
	bool                      inScene;
	VkvRenderPass            *rp;
	VkvFramebuffer           *fb;
	const struct VkvCmdBeginRP *begin;
	uint32_t                  subpass;
	float                     fbInv[4] __attribute__((aligned(16))); /* 1 / framebuffer size */
	float                     scissorPx[4] __attribute__((aligned(16))); /* exact scissor x0, y0, x1, y1 */
	bool                      exactScissor;   /* the scissor is not on 32x32 tiles */
	bool                      fpScissorOn;    /* the scissor variant of the fragment program is set */
	VkvImage                 *colorImg, *depthImg;
	VkvImageView             *colorView, *depthView;
	SceGxmColorSurface        color;
	SceGxmDepthStencilSurface depth;
	bool                      splits;         /* pass contains mid-pass clears */
	uint32_t                  sceneDraws;     /* draws in the current scene */
	bool                      sceneClearsDepth; /* the current scene clears depth/stencil to these */
	float                     sceneDepth;
	uint32_t                  sceneStencil;
	uint32_t                  scenesLeft;     /* render passes left in this submit */
	VkvFence                 *fence;
	bool                      fenceAttached;
	bool                      transferTail;   /* a transfer was issued after the last scene */
	bool                      queryActive;    /* an occlusion query counts the draws */

	/* bound state */
	VkvPipeline              *pipe;
	bool                      pipeDirty;
	int                       vpVariant;      /* vertex program variant currently set, -1 none */
	VkvBuffer                *vb[VKV_MAX_BINDINGS];
	uint32_t                  vbOff[VKV_MAX_BINDINGS];
	VkvBuffer                *ib;
	uint32_t                  ibOff;
	VkIndexType               ibType;
	VkvSet                   *sets[VKV_MAX_SETS];
	uint32_t                  dyn[VKV_MAX_SETS][VKV_MAX_DYNAMIC];
	uint8_t                   push[VKV_MAX_PUSH] __attribute__((aligned(16)));
	uint32_t                  pushHigh;       /* highest pushed byte, for validation */

	/* dynamic state */
	VkViewport                viewport;
	bool                      hasViewport, viewportDirty;
	VkRect2D                  scissor;
	bool                      hasScissor, scissorDirty;
	uint32_t                  sCmp[2], sWr[2], sRef[2];
	uint32_t                  dynSet;         /* VKV_DYN_* bits set by vkCmdSet* */
	float                     biasConst, biasSlope;
	float                     lineWidth;
} VkvReplay;

/* ------------------------------------------------------------------------ */
/* Clears                                                                   */
/* ------------------------------------------------------------------------ */
static uint32_t vkv_f2u(float f, uint32_t max) {
	if (!(f > 0.0f)) return 0;
	if (f >= 1.0f) return max;
	return (uint32_t)(f * (float)max + 0.5f);
}

static uint16_t vkv_f2h(float f) {
	union { float f; uint32_t u; } v = { f };
	uint32_t s = (v.u >> 16) & 0x8000, e = (v.u >> 23) & 0xFF, m = v.u & 0x7FFFFF;
	if (e == 0xFF) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
	int32_t ne = (int32_t)e - 127 + 15;
	if (ne >= 31) return (uint16_t)(s | 0x7C00);
	if (ne <= 0) return (uint16_t)s;
	return (uint16_t)(s | ((uint32_t)ne << 10) | (m >> 13));
}

/* Packs a clear color into the bytes of one texel. Returns the texel size. */
static uint32_t vkv_pack_color(const VkvImage *img, const VkClearColorValue *c, uint8_t out[16]) {
	const float *f = c->float32;
	uint32_t r8 = vkv_f2u(f[0], 255), g8 = vkv_f2u(f[1], 255), b8 = vkv_f2u(f[2], 255), a8 = vkv_f2u(f[3], 255);
	uint32_t v = 0;
	VkFormat fmt = img->swapchain ? VK_FORMAT_R8G8B8A8_UNORM : img->format;
	switch (fmt) {
	case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
		v = r8 | g8 << 8 | b8 << 16 | a8 << 24; memcpy(out, &v, 4); return 4;
	case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
		v = b8 | g8 << 8 | r8 << 16 | a8 << 24; memcpy(out, &v, 4); return 4;
	case VK_FORMAT_R5G6B5_UNORM_PACK16: {
		uint16_t h = (uint16_t)(vkv_f2u(f[0], 31) << 11 | vkv_f2u(f[1], 63) << 5 | vkv_f2u(f[2], 31));
		memcpy(out, &h, 2); return 2;
	}
	case VK_FORMAT_R8G8_UNORM: { uint16_t h = (uint16_t)(r8 | g8 << 8); memcpy(out, &h, 2); return 2; }
	case VK_FORMAT_R8_UNORM: out[0] = (uint8_t)r8; return 1;
	case VK_FORMAT_R32_SFLOAT: memcpy(out, &f[0], 4); return 4;
	case VK_FORMAT_R16_SFLOAT: { uint16_t h = vkv_f2h(f[0]); memcpy(out, &h, 2); return 2; }
	case VK_FORMAT_R16G16_SFLOAT: {
		uint16_t h[2] = { vkv_f2h(f[0]), vkv_f2h(f[1]) }; memcpy(out, h, 4); return 4;
	}
	case VK_FORMAT_R16G16B16A16_SFLOAT: {
		uint16_t h[4] = { vkv_f2h(f[0]), vkv_f2h(f[1]), vkv_f2h(f[2]), vkv_f2h(f[3]) }; memcpy(out, h, 8); return 8;
	}
	default:
		memset(out, 0, img->fi->bpp);
		return img->fi->bpp;
	}
}

/* Fills a rectangle of one level of a color image, outside of any scene. */
static void vkv_clear_color_rect(VkvReplay *R, VkvImage *img, uint32_t level, uint32_t layer, const VkClearColorValue *c,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	if (!img->ptr || !w || !h || img->fi->block || level >= img->levels || layer >= img->layers) return;
	uint32_t lw = vkv_image_level_width(img, level), lh = vkv_image_level_height(img, level);
	x = VKV_MIN(x, lw); y = VKV_MIN(y, lh);
	w = VKV_MIN(w, lw - x); h = VKV_MIN(h, lh - y);
	if (!w || !h) return;
	uint8_t texel[16];
	uint32_t bpp = vkv_pack_color(img, c, texel);
	uint8_t *base = vkv_image_level_ptr(img, level, layer);
	uint32_t pitch = img->levelPitch[level];
	g_vkv.cur.clears++;

	/* Transfers only wait for scenes that render into the image, not for scenes
	 * that still sample it, so a clear right after such a scene would overwrite
	 * pixels it has not read yet. */
	if (img->readSerial && R->dev->gpuBusy && !vkv_serial_done(R->dev, img->readSerial)) {
		static bool s_told;
		if (!s_told) {
			s_told = true;
			VKV_INFO("clearing an image the GPU is still sampling waits for the GPU first");
		}
		vkv_device_wait_gpu(R->dev);
	}

	static bool s_fillBroken;
	if (!vkv_compat(VKV_COMPAT_CPU_CLEAR) && !s_fillBroken && img->sync && bpp == 4 && !img->swizzled) {
		uint32_t value = 0;
		memcpy(&value, texel, bpp);
		int r = sceGxmTransferFill(value, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, base, x, y, w, h, (int32_t)pitch,
		                           img->sync, SCE_GXM_TRANSFER_FRAGMENT_SYNC, NULL);
		if (r >= 0) {
			R->dev->usedTransfer = true;
			R->dev->transferPending = true;
			R->transferTail = true;
			return;
		}
		s_fillBroken = true;
		VKV_WARN("sceGxmTransferFill failed 0x%08X, using CPU clears from now on", (unsigned)r);
	}
	/* CPU path: the GPU must not be using the image */
	if (R->dev->gpuBusy) vkv_device_wait_gpu(R->dev);
	if (img->swizzled) {
		for (uint32_t row = 0; row < h; row++)
			for (uint32_t i = 0; i < w; i++) memcpy(vkv_image_elem(img, level, layer, x + i, y + row), texel, bpp);
		return;
	}
	for (uint32_t row = 0; row < h; row++) {
		uint8_t *p = base + (y + row) * pitch + x * bpp;
		if (bpp == 4) {
			uint32_t v; memcpy(&v, texel, 4);
			uint32_t *q = (uint32_t *)p;
			for (uint32_t i = 0; i < w; i++) q[i] = v;
		} else {
			for (uint32_t i = 0; i < w; i++) memcpy(p + i * bpp, texel, bpp);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Scenes                                                                   */
/* ------------------------------------------------------------------------ */
static void vkv_begin_scene(VkvReplay *R, bool loadDepth, bool clearDepth, float depth, uint32_t stencil) {
	VkvRenderPass *rp = R->rp;
	VkvFramebuffer *fb = R->fb;
	if (R->depthImg && VKV_CHECK(R->depthImg->samples == rp->samples, "depth attachment has %u samples, the render pass %u",
	                             (unsigned)R->depthImg->samples, (unsigned)rp->samples))
		R->depthImg = NULL;
	if (R->depthImg && R->depthImg->pendingDepthClear) {
		if (loadDepth || !clearDepth) {
			loadDepth = false;
			clearDepth = true;
			depth = R->depthImg->clearDepth;
			stencil = R->depthImg->clearStencil;
		}
		R->depthImg->pendingDepthClear = false;
	}
	R->sceneDraws = 0;
	R->sceneClearsDepth = R->depthImg && clearDepth;
	R->sceneDepth = depth;
	R->sceneStencil = stencil;
	if (R->depthImg) {
		const VkvSubpass *sp = &rp->sub[R->subpass];
		const VkvAttachmentDesc *a = &rp->att[sp->depth];
		uint8_t *dptr = R->depthImg->ptr + (size_t)(R->depthView ? R->depthView->baseLayer : 0) * R->depthImg->layerSize;
		sceGxmDepthStencilSurfaceInit(&R->depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
		                              R->depthImg->alignedW, dptr, NULL);
		if (clearDepth) {
			sceGxmDepthStencilSurfaceSetBackgroundDepth(&R->depth, depth);
			sceGxmDepthStencilSurfaceSetBackgroundStencil(&R->depth, (unsigned char)stencil);
		}
		sceGxmDepthStencilSurfaceSetForceLoadMode(&R->depth, loadDepth ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED
		                                                                : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
		/* later subpasses in another scene need the depth buffer too */
		bool store = R->splits || rp->lastUse[sp->depth] > (int8_t)sp->runEnd ||
		             a->storeOp == VK_ATTACHMENT_STORE_OP_STORE || a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
		sceGxmDepthStencilSurfaceSetForceStoreMode(&R->depth, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
		                                                            : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
	}
	if (R->colorImg) {
		VkvImage *img = R->colorImg;
		SceGxmColorFormat cf = img->swapchain ? SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR : img->fi->color;
		SceGxmOutputRegisterSize rs = img->fi->bpp == 8 ? SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT
		                                                : SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
		uint32_t level = R->colorView ? R->colorView->baseLevel : 0;
		uint32_t layer = R->colorView ? R->colorView->baseLayer : 0;
		/* with multisampling GXM averages the samples when it writes the tile back */
		SceGxmColorSurfaceScaleMode scale = rp->samples > 1 ? SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE
		                                                    : SCE_GXM_COLOR_SURFACE_SCALE_NONE;
		int r = sceGxmColorSurfaceInit(&R->color, cf, SCE_GXM_COLOR_SURFACE_LINEAR, scale, rs, fb->width, fb->height,
		                               img->levelPitch[level] / img->fi->bpp, vkv_image_level_ptr(img, level, layer));
		if (r < 0) VKV_ERROR("sceGxmColorSurfaceInit failed 0x%08X", (unsigned)r);
		const VkvFormatInfo *vf = R->colorView ? vkv_format(R->colorView->format) : img->fi;
		if (vf && vf->srgb) sceGxmColorSurfaceSetGammaMode(&R->color, SCE_GXM_COLOR_SURFACE_GAMMA_BGR);
	} else {
		sceGxmColorSurfaceInitDisabled(&R->color);
	}
	unsigned int flags = 0;
	if (R->dev->transferPending) {
		flags |= SCE_GXM_SCENE_FRAGMENT_TRANSFER_SYNC;
		R->dev->transferPending = false;
	}
	int r = sceGxmBeginScene(R->ctx, flags, fb->rt, NULL, NULL, R->colorImg ? R->colorImg->sync : NULL, &R->color,
	                         R->depthImg ? &R->depth : NULL);
	if (r < 0) {
		VKV_ERROR("sceGxmBeginScene failed 0x%08X", (unsigned)r);
		return;
	}
	R->inScene = true;
	sceGxmSetViewportEnable(R->ctx, SCE_GXM_VIEWPORT_ENABLED);
	R->pipeDirty = true;
	R->vpVariant = -1;
	R->viewportDirty = true;
	R->scissorDirty = true;
	g_vkv.cur.scenes++;
}

static void vkv_end_scene(VkvReplay *R, bool final) {
	if (!R->inScene) return;
	R->transferTail = false; /* the scene waited for earlier transfers, and the fence follows it */
	R->dev->progress.value = ++R->dev->sceneSerial;
	if (final && R->fence && R->scenesLeft == 1) {
		R->fence->serial = R->dev->sceneSerial;
		R->fenceAttached = true;
	}
	int r = sceGxmEndScene(R->ctx, NULL, &R->dev->progress);
	if (r < 0) VKV_ERROR("sceGxmEndScene failed 0x%08X", (unsigned)r);
	R->inScene = false;
	R->dev->gpuBusy = true;
	/* A scene without draws does not write its depth buffer back, so a clear
	 * it was meant to do would be lost. The next scene does it instead. */
	if (R->depthImg && R->sceneClearsDepth && !R->sceneDraws) {
		R->depthImg->pendingDepthClear = true;
		R->depthImg->clearDepth = R->sceneDepth;
		R->depthImg->clearStencil = R->sceneStencil;
		return;
	}
	if (R->depthImg) R->depthImg->written = true;
}

static bool vkv_pass_has_splits(const uint8_t *p, const uint8_t *end) {
	while (p < end) {
		const VkvCmd *c = (const VkvCmd *)p;
		if (c->type == VKV_CMD_END_RP || c->type == VKV_CMD_NEXT_SUBPASS) return false;
		if (c->type == VKV_CMD_CLEAR_ATTACH) return true;
		p += c->size;
	}
	return false;
}

/* Starts the scene of one subpass. Load and clear ops apply where an
 * attachment is first used, everything later loads what came before. */
static void vkv_start_subpass(VkvReplay *R, uint32_t idx, const uint8_t *next, const uint8_t *end) {
	VkvRenderPass *rp = R->rp;
	VkvFramebuffer *fb = R->fb;
	const VkvCmdBeginRP *c = R->begin;
	const VkvSubpass *sp = &rp->sub[idx];
	R->subpass = idx;
	/* a multisampled attachment that gets resolved renders straight into the
	 * resolve target, GXM downscales on the way out */
	R->colorView = sp->resolve >= 0 ? fb->views[sp->resolve] : sp->color >= 0 ? fb->views[sp->color] : NULL;
	R->colorImg = R->colorView ? R->colorView->image : NULL;
	R->depthView = sp->depth >= 0 ? fb->views[sp->depth] : NULL;
	R->depthImg = R->depthView ? R->depthView->image : NULL;
	R->splits = vkv_pass_has_splits(next, end);

	if (R->colorImg && rp->firstUse[sp->color] == (int8_t)idx) {
		const VkvAttachmentDesc *a = &rp->att[sp->color];
		if (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)sp->color < c->clearCount)
			vkv_clear_color_rect(R, R->colorImg, R->colorView->baseLevel, R->colorView->baseLayer,
			                     &c->clears[sp->color].color, (uint32_t)c->area.offset.x,
			                     (uint32_t)c->area.offset.y, c->area.extent.width, c->area.extent.height);
	}
	bool loadDepth = false, clearDepth = false;
	float d = 1.0f;
	uint32_t s = 0;
	if (R->depthImg) {
		const VkvAttachmentDesc *a = &rp->att[sp->depth];
		if (rp->firstUse[sp->depth] != (int8_t)idx) {
			loadDepth = true;
		} else {
			loadDepth = a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && R->depthImg->written;
			if (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)sp->depth < c->clearCount) {
				clearDepth = true;
				d = c->clears[sp->depth].depthStencil.depth;
				s = c->clears[sp->depth].depthStencil.stencil;
			}
		}
	}
	vkv_begin_scene(R, loadDepth, clearDepth, d, s);
}

static void vkv_exec_begin_rp(VkvReplay *R, const VkvCmdBeginRP *c, const uint8_t *next, const uint8_t *end) {
	VkvFramebuffer *fb = c->fb;
	R->rp = c->rp;
	R->fb = fb;
	R->begin = c;
	R->fbInv[0] = 1.0f / (float)fb->width;
	R->fbInv[1] = 1.0f / (float)fb->height;
	if (vkv_validate() && (c->area.offset.x || c->area.offset.y || c->area.extent.width != fb->width ||
	                       c->area.extent.height != fb->height))
		VKV_UNSUPPORTED("partial render areas (whole framebuffer is rendered)");
	vkv_start_subpass(R, 0, next, end);
}

static void vkv_exec_next_subpass(VkvReplay *R, const uint8_t *next, const uint8_t *end) {
	if (!R->rp || R->subpass + 1 >= R->rp->subpassCount) return;
	uint32_t n = R->subpass + 1;
	if (!R->rp->sub[n].newScene) {
		R->subpass = n;
		return;
	}
	vkv_end_scene(R, false);
	vkv_start_subpass(R, n, next, end);
}

/* ------------------------------------------------------------------------ */
/* Clears inside a scene                                                    */
/* ------------------------------------------------------------------------ */
/* vkCmdClearAttachments draws a quad per rectangle with the clear color and
 * depth, so the scene keeps going and depth and stencil rectangles are exact.
 * The corners come from INDEX, so no vertex buffer is needed. */
static const char k_clearVs[] =
	"void main(int idx : INDEX, uniform float4 uRect, uniform float uDepth, float4 out vPosition : POSITION)\n"
	"{\n"
	"    float x = (idx == 1 || idx == 3) ? uRect.z : uRect.x;\n"
	"    float y = (idx >= 2) ? uRect.w : uRect.y;\n"
	"    vPosition = float4(x, y, uDepth, 1.0f);\n"
	"}\n";
static const char k_clearFs[] =
	"float4 main(uniform float4 uColor) : COLOR\n"
	"{\n"
	"    return uColor;\n"
	"}\n";

void vkv_clear_programs_free(struct VkDevice_T *d) {
	if (!d->patcher) return;
	for (uint32_t i = 0; i < d->clearFpCount; i++) sceGxmShaderPatcherReleaseFragmentProgram(d->patcher, d->clearFp[i].fp);
	d->clearFpCount = 0;
	if (d->clearVp) sceGxmShaderPatcherReleaseVertexProgram(d->patcher, d->clearVp);
	d->clearVp = NULL;
	for (int s = 0; s < 2; s++) {
		if (d->clearId[s]) sceGxmShaderPatcherUnregisterProgram(d->patcher, d->clearId[s]);
		d->clearId[s] = NULL;
		free(d->clearProg[s]);
		d->clearProg[s] = NULL;
	}
	d->clearState = 0;
}

static bool vkv_clear_programs(struct VkDevice_T *d) {
	if (d->clearState) return d->clearState > 0;
	d->clearState = -1;
	const char *src[2] = { k_clearVs, k_clearFs };
	for (int s = 0; s < 2; s++) {
		uint32_t size = 0;
		d->clearProg[s] = vkv_shark_compile(d, src[s], s ? VKV_STAGE_FRAGMENT : VKV_STAGE_VERTEX,
		                                    s ? "vkv_clear_fs" : "vkv_clear_vs", s ? 0xC1EA0F02u : 0xC1EA0F01u, &size);
		if (!d->clearProg[s] || sceGxmShaderPatcherRegisterProgram(d->patcher, d->clearProg[s], &d->clearId[s]) < 0) {
			VKV_INFO("clears inside render passes split the scene (no clear shaders)");
			vkv_clear_programs_free(d);
			d->clearState = -1;
			return false;
		}
	}
	if (sceGxmShaderPatcherCreateVertexProgram(d->patcher, d->clearId[0], NULL, 0, NULL, 0, &d->clearVp) < 0) {
		vkv_clear_programs_free(d);
		d->clearState = -1;
		return false;
	}
	d->clearRect = sceGxmProgramFindParameterByName(d->clearProg[0], "uRect");
	d->clearDepth = sceGxmProgramFindParameterByName(d->clearProg[0], "uDepth");
	d->clearColor = sceGxmProgramFindParameterByName(d->clearProg[1], "uColor");
	d->clearState = d->clearRect && d->clearDepth && d->clearColor ? 1 : -1;
	if (d->clearState < 0) vkv_clear_programs_free(d), d->clearState = -1;
	return d->clearState > 0;
}

static SceGxmFragmentProgram *vkv_clear_fp(struct VkDevice_T *d, SceGxmOutputRegisterFormat fmt,
                                           SceGxmMultisampleMode ms, bool color) {
	uint32_t key = (uint32_t)fmt | ((uint32_t)ms << 8) | (color ? 1u << 16 : 0);
	for (uint32_t i = 0; i < d->clearFpCount; i++)
		if (d->clearFp[i].key == key) return d->clearFp[i].fp;
	if (d->clearFpCount == 16) return NULL;
	SceGxmBlendInfo mask;
	memset(&mask, 0, sizeof(mask));
	mask.colorMask = SCE_GXM_COLOR_MASK_NONE;
	mask.colorFunc = mask.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
	mask.colorSrc = mask.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
	mask.colorDst = mask.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	SceGxmFragmentProgram *fp = NULL;
	if (sceGxmShaderPatcherCreateFragmentProgram(d->patcher, d->clearId[1], fmt, ms, color ? NULL : &mask,
	                                             d->clearProg[0], &fp) < 0)
		return NULL;
	d->clearFp[d->clearFpCount].key = key;
	d->clearFp[d->clearFpCount].fp = fp;
	d->clearFpCount++;
	return fp;
}

/* Returns false when the quads could not be drawn and the caller has to
 * clear the old way. */
static bool vkv_clear_with_quads(VkvReplay *R, const VkvCmdClearAttach *c) {
	struct VkDevice_T *d = R->dev;
	if (vkv_compat(VKV_COMPAT_CPU_CLEAR) || !vkv_clear_programs(d)) return false;
	bool color = false, depth = false, stencil = false;
	VkClearValue cv;
	memset(&cv, 0, sizeof(cv));
	VkClearDepthStencilValue ds = { 1.0f, 0 };
	for (uint32_t i = 0; i < c->na; i++) {
		const VkClearAttachment *a = &c->a[i];
		if ((a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && R->colorImg) { color = true; cv = a->clearValue; }
		if ((a->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && R->depthImg) { depth = true; ds.depth = a->clearValue.depthStencil.depth; }
		if ((a->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && R->depthImg) {
			stencil = true;
			ds.stencil = a->clearValue.depthStencil.stencil;
		}
	}
	if (!color && !depth && !stencil) return true;
	const VkvFormatInfo *fi = R->colorImg ? R->colorImg->fi : NULL;
	SceGxmFragmentProgram *fp = vkv_clear_fp(d, fi ? fi->outReg : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
	                                         vkv_gxm_multisample(R->rp->samples), color);
	if (!fp) return false;

	SceGxmContext *ctx = R->ctx;
	sceGxmSetVertexProgram(ctx, d->clearVp);
	sceGxmSetFragmentProgram(ctx, fp);
	sceGxmSetCullMode(ctx, SCE_GXM_CULL_NONE);
	sceGxmSetTwoSidedEnable(ctx, SCE_GXM_TWO_SIDED_DISABLED);
	sceGxmSetFrontPolygonMode(ctx, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	sceGxmSetFrontDepthBias(ctx, 0, 0);
	sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(ctx, depth ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetFrontStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
	                          stencil ? SCE_GXM_STENCIL_OP_REPLACE : SCE_GXM_STENCIL_OP_KEEP, 0xFF, stencil ? 0xFF : 0);
	sceGxmSetFrontStencilRef(ctx, ds.stencil & 0xFF);
	/* clip space is the whole framebuffer, z goes straight to depth */
	float fw = (float)R->fb->width, fh = (float)R->fb->height;
	sceGxmSetViewport(ctx, fw * 0.5f, fw * 0.5f, fh * 0.5f, fh * 0.5f, 0.0f, 1.0f);
	sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, R->fb->width - 1, R->fb->height - 1);

	float col[4] = { cv.color.float32[0], cv.color.float32[1], cv.color.float32[2], cv.color.float32[3] };
	/* clears do not count toward an occlusion query */
	if (R->queryActive) {
		sceGxmSetFrontVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_DISABLED);
		sceGxmSetBackVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_DISABLED);
	}
	for (uint32_t k = 0; k < c->nr; k++) {
		const VkRect2D *r = &c->r[k].rect;
		float rect[4] = { r->offset.x / fw * 2.0f - 1.0f, r->offset.y / fh * 2.0f - 1.0f,
		                  (r->offset.x + (float)r->extent.width) / fw * 2.0f - 1.0f,
		                  (r->offset.y + (float)r->extent.height) / fh * 2.0f - 1.0f };
		float z = ds.depth;
		void *ub;
		if (sceGxmReserveVertexDefaultUniformBuffer(ctx, &ub) < 0) return false;
		sceGxmSetUniformDataF(ub, d->clearRect, 0, 4, rect);
		sceGxmSetUniformDataF(ub, d->clearDepth, 0, 1, &z);
		if (sceGxmReserveFragmentDefaultUniformBuffer(ctx, &ub) < 0) return false;
		sceGxmSetUniformDataF(ub, d->clearColor, 0, 4, col);
		R->sceneDraws++;
		sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, d->linearIdx16.base, 4);
	}
	if (R->queryActive) {
		sceGxmSetFrontVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_ENABLED);
		sceGxmSetBackVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_ENABLED);
	}
	if (depth || stencil) g_vkv.cur.clears++;
	/* the next draw sets its own state again */
	R->pipeDirty = true;
	R->vpVariant = -1;
	R->viewportDirty = true;
	R->scissorDirty = true;
	return true;
}

static void vkv_exec_clear_attachments(VkvReplay *R, const VkvCmdClearAttach *c) {
	if (!R->inScene) return;
	if (vkv_clear_with_quads(R, c)) return;
	bool clearDepth = false;
	float d = 1.0f;
	uint32_t s = 0;
	vkv_end_scene(R, false);
	for (uint32_t i = 0; i < c->na; i++) {
		const VkClearAttachment *a = &c->a[i];
		if ((a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && R->colorImg) {
			for (uint32_t k = 0; k < c->nr; k++)
				vkv_clear_color_rect(R, R->colorImg, R->colorView->baseLevel, R->colorView->baseLayer,
				                     &a->clearValue.color, (uint32_t)c->r[k].rect.offset.x,
				                     (uint32_t)c->r[k].rect.offset.y, c->r[k].rect.extent.width,
				                     c->r[k].rect.extent.height);
		}
		if ((a->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && R->depthImg) {
			clearDepth = true;
			d = a->clearValue.depthStencil.depth;
			s = a->clearValue.depthStencil.stencil;
			for (uint32_t k = 0; k < c->nr; k++)
				if (c->r[k].rect.extent.width < R->fb->width || c->r[k].rect.extent.height < R->fb->height)
					VKV_UNSUPPORTED("partial depth clears (the whole depth buffer is cleared)");
			g_vkv.cur.clears++;
		}
	}
	vkv_begin_scene(R, R->depthImg && !clearDepth, clearDepth, d, s);
}

/* ------------------------------------------------------------------------ */
/* Draw state                                                               */
/* ------------------------------------------------------------------------ */
static SceGxmCullMode vkv_cull(const VkvPipeline *p) {
	if (vkv_compat(VKV_COMPAT_DISABLE_CULL) || p->vkCull == VK_CULL_MODE_NONE) return SCE_GXM_CULL_NONE;
	if (p->vkCull == VK_CULL_MODE_FRONT_AND_BACK) return SCE_GXM_CULL_NONE; /* draws are skipped instead */
	/* GXM culls by window-space winding (y down), the same space Vulkan uses. */
	bool cullCW = (p->vkCull == VK_CULL_MODE_BACK_BIT) == (p->vkFront == VK_FRONT_FACE_COUNTER_CLOCKWISE);
	if (vkv_compat(VKV_COMPAT_INVERT_CULL)) cullCW = !cullCW;
	return cullCW ? SCE_GXM_CULL_CW : SCE_GXM_CULL_CCW;
}

static void vkv_apply_pipeline(VkvReplay *R, VkvPipeline *p, int variant) {
	SceGxmContext *ctx = R->ctx;
	SceGxmVertexProgram *vp = vkv_pipeline_vertex_program(p, variant == 1);
	if (R->pipeDirty || R->vpVariant != variant) {
		sceGxmSetVertexProgram(ctx, vp);
		R->vpVariant = variant;
	}
	if (!R->pipeDirty) return;
	R->pipeDirty = false;
	g_vkv.cur.pipelineBinds++;
	sceGxmSetFragmentProgram(ctx, p->fp);
	R->fpScissorOn = false;
	sceGxmSetCullMode(ctx, vkv_cull(p));
	sceGxmSetFrontDepthFunc(ctx, p->depthFunc);
	sceGxmSetBackDepthFunc(ctx, p->depthFunc);
	sceGxmSetFrontDepthWriteEnable(ctx, p->depthWrite);
	sceGxmSetBackDepthWriteEnable(ctx, p->depthWrite);
	sceGxmSetFrontPolygonMode(ctx, p->polyMode);
	sceGxmSetBackPolygonMode(ctx, p->polyMode);

	uint32_t cmp[2], wr[2], ref[2];
	for (int f = 0; f < 2; f++) {
		cmp[f] = (p->dynamicMask & VKV_DYN_STENCIL_CMP) && (R->dynSet & VKV_DYN_STENCIL_CMP) ? R->sCmp[f] : p->sCompare[f];
		wr[f]  = (p->dynamicMask & VKV_DYN_STENCIL_WR)  && (R->dynSet & VKV_DYN_STENCIL_WR)  ? R->sWr[f]  : p->sWrite[f];
		ref[f] = (p->dynamicMask & VKV_DYN_STENCIL_REF) && (R->dynSet & VKV_DYN_STENCIL_REF) ? R->sRef[f] : p->sRef[f];
	}
	sceGxmSetTwoSidedEnable(ctx, p->stencilEnable ? SCE_GXM_TWO_SIDED_ENABLED : SCE_GXM_TWO_SIDED_DISABLED);
	sceGxmSetFrontStencilFunc(ctx, p->sFunc[0], p->sFail[0], p->sDepthFail[0], p->sPass[0], (unsigned char)cmp[0],
	                          (unsigned char)wr[0]);
	sceGxmSetBackStencilFunc(ctx, p->sFunc[1], p->sFail[1], p->sDepthFail[1], p->sPass[1], (unsigned char)cmp[1],
	                         (unsigned char)wr[1]);
	sceGxmSetFrontStencilRef(ctx, ref[0]);
	sceGxmSetBackStencilRef(ctx, ref[1]);

	int factor = 0, units = 0;
	if (p->depthBiasEnable) {
		bool dyn = (p->dynamicMask & VKV_DYN_DEPTH_BIAS) && (R->dynSet & VKV_DYN_DEPTH_BIAS);
		factor = dyn ? (int)R->biasSlope : p->depthBiasFactor;
		units = dyn ? (int)R->biasConst : p->depthBiasUnits;
	}
	sceGxmSetFrontDepthBias(ctx, factor, units);
	sceGxmSetBackDepthBias(ctx, factor, units);

	float lw = (p->dynamicMask & VKV_DYN_LINE_WIDTH) && (R->dynSet & VKV_DYN_LINE_WIDTH) ? R->lineWidth : p->lineWidth;
	unsigned lwi = lw < 1.0f ? 1u : lw > 16.0f ? 16u : (unsigned)(lw + 0.5f);
	sceGxmSetFrontPointLineWidth(ctx, lwi);
	sceGxmSetBackPointLineWidth(ctx, lwi);
	R->viewportDirty = true;
	R->scissorDirty = true;
}

static VkViewport vkv_current_viewport(const VkvReplay *R, const VkvPipeline *p) {
	if (p->hasViewport) return p->viewport;
	if (R->hasViewport) return R->viewport;
	return (VkViewport){ 0, 0, (float)R->fb->width, (float)R->fb->height, 0.0f, 1.0f };
}

static void vkv_apply_viewport(VkvReplay *R, const VkvPipeline *p) {
	if (R->viewportDirty) {
		R->viewportDirty = false;
		R->scissorDirty = true; /* the region clip also covers the viewport */
		VkViewport v = vkv_current_viewport(R, p);
		float xs = v.width * 0.5f, xo = v.x + xs;
		float ys = v.height * 0.5f, yo = v.y + ys;
		float zs = v.maxDepth - v.minDepth, zo = v.minDepth;
		if (vkv_compat(VKV_COMPAT_GL_CLIP_SPACE)) {
			ys = -ys;
			zs = (v.maxDepth - v.minDepth) * 0.5f;
			zo = (v.maxDepth + v.minDepth) * 0.5f;
		}
		sceGxmSetViewport(R->ctx, xo, xs, yo, ys, zo, zs);
	}
	if (R->scissorDirty) {
		R->scissorDirty = false;
		int32_t w = (int32_t)R->fb->width, h = (int32_t)R->fb->height;
		/* GXM does not clip to the viewport, so the region clip is viewport and scissor combined */
		VkViewport v = vkv_current_viewport(R, p);
		float vx0 = v.width < 0 ? v.x + v.width : v.x, vy0 = v.height < 0 ? v.y + v.height : v.y;
		int32_t sx = VKV_MAX((int32_t)floorf(vx0), 0), sy = VKV_MAX((int32_t)floorf(vy0), 0);
		int32_t ex = VKV_MIN((int32_t)ceilf(vx0 + fabsf(v.width)), w), ey = VKV_MIN((int32_t)ceilf(vy0 + fabsf(v.height)), h);
		const VkRect2D *s = p->hasScissor ? &p->scissor : R->hasScissor ? &R->scissor : NULL;
		bool scissored = s && !vkv_compat(VKV_COMPAT_IGNORE_SCISSOR);
		R->exactScissor = false;
		if (scissored) {
			int32_t qx = s->offset.x, qy = s->offset.y;
			int32_t qe = qx + (int32_t)s->extent.width, qf = qy + (int32_t)s->extent.height;
			/* the region clip works on 32x32 tiles; anything finer is done by the fragment shader */
			R->exactScissor = ((qx | qy) & 31) || ((qe & 31) && qe < w) || ((qf & 31) && qf < h);
			sx = VKV_MAX(sx, s->offset.x);
			sy = VKV_MAX(sy, s->offset.y);
			ex = VKV_MIN(ex, (int32_t)(s->offset.x + s->extent.width));
			ey = VKV_MIN(ey, (int32_t)(s->offset.y + s->extent.height));
		}
		if (ex <= sx || ey <= sy) { sx = sy = 0; ex = ey = 1; } /* empty: clip almost all */
		R->scissorPx[0] = (float)sx;
		R->scissorPx[1] = (float)sy;
		R->scissorPx[2] = (float)ex;
		R->scissorPx[3] = (float)ey;
		sceGxmSetRegionClip(R->ctx, SCE_GXM_REGION_CLIP_OUTSIDE, (unsigned)sx, (unsigned)sy, (unsigned)(ex - 1),
		                    (unsigned)(ey - 1));
	}
}

static const uint8_t *vkv_uniform_buffer_data(VkvReplay *R, int set, uint32_t binding, uint32_t *range) {
	if (set == VKV_INTERNAL_SET) {
		if (binding == 1) {
			*range = sizeof(R->scissorPx);
			return (const uint8_t *)R->scissorPx;
		}
		*range = sizeof(R->fbInv);
		return (const uint8_t *)R->fbInv;
	}
	if (set < 0 || set >= VKV_MAX_SETS || !R->sets[set]) return NULL;
	VkvSet *s = R->sets[set];
	for (uint32_t b = 0; b < s->layout->bindingCount; b++) {
		const VkvDslBinding *lb = &s->layout->bindings[b];
		if (lb->binding != binding || !lb->count) continue;
		const VkvDescriptor *d = &s->desc[lb->first];
		if (!d->valid || !d->buffer || !d->buffer->ptr) return NULL;
		bool dynamic = d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
		               d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (!dynamic && d->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && d->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			return NULL;
		uint32_t off = d->offset;
		if (dynamic && lb->dynIndex < VKV_MAX_DYNAMIC)
			off += R->dyn[set][lb->dynIndex];
		*range = d->range;
		return d->buffer->ptr + off;
	}
	return NULL;
}

static void vkv_write_uniforms_one(void *buf, const VkvUniformMap *u, const uint8_t *base) {
	if (u->scalar) {
		float conv[4];
		uint32_t c = VKV_MIN(u->comps, 4u);
		for (uint32_t k = 0; k < c; k++) {
			uint32_t bits;
			memcpy(&bits, base + u->offset + k * 4, 4);
			conv[k] = u->scalar == 1 ? (float)(int32_t)bits : (float)bits;
		}
		sceGxmSetUniformDataF(buf, u->param, 0, c, conv);
	} else if (u->elems <= 1 || u->elemStride == u->comps * 4) {
		sceGxmSetUniformDataF(buf, u->param, 0, u->comps * u->elems, (const float *)(base + u->offset));
	} else {
		for (uint32_t e = 0; e < u->elems; e++)
			sceGxmSetUniformDataF(buf, u->param, e * u->comps, u->comps,
			                      (const float *)(base + u->offset + e * u->elemStride));
	}
}

static void vkv_write_uniforms(VkvReplay *R, void *buf, const VkvUniformMap *map, uint32_t n, const char *shader) {
	for (uint32_t i = 0; i < n; i++) {
		const VkvUniformMap *u = &map[i];
		uint32_t bytes = u->elems > 1 ? u->elemStride * u->elems : u->comps * 4;
		const uint8_t *base;
		if (u->set < 0) {
			if (u->offset + bytes > VKV_MAX_PUSH) continue;
			if (vkv_validate() && u->offset + bytes > R->pushHigh)
				vkv_fail("draw", "shader '%s' uniform '%s' reads push constant bytes [%u..%u) that were never pushed",
				         shader, sceGxmProgramParameterGetName(u->param), (unsigned)u->offset,
				         (unsigned)(u->offset + bytes));
			base = R->push;
		} else {
			uint32_t range = 0;
			base = vkv_uniform_buffer_data(R, u->set, u->binding, &range);
			/* an array (a runtime array of a storage buffer) is fed as far as the buffer goes */
			if (base && u->elems > 1 && u->offset + bytes > range && u->offset + u->elemStride <= range) {
				VkvUniformMap part = *u;
				part.elems = (range - u->offset) / u->elemStride;
				vkv_write_uniforms_one(buf, &part, base);
				continue;
			}
			if (!base || u->offset + bytes > range) {
				if (vkv_validate())
					vkv_fail("draw", "shader '%s' uniform '%s' needs %u bytes at offset %u of the uniform buffer at "
					         "set %d binding %u, %s", shader, sceGxmProgramParameterGetName(u->param), (unsigned)bytes,
					         (unsigned)u->offset, (int)u->set, (unsigned)u->binding,
					         base ? "which is too small" : "but none is bound");
				continue;
			}
		}
		vkv_write_uniforms_one(buf, u, base);
	}
}

static VkvDescriptor *vkv_find_descriptor(VkvReplay *R, int set, uint32_t binding, uint32_t element) {
	if (set < 0 || set >= VKV_MAX_SETS || !R->sets[set]) return NULL;
	VkvSet *s = R->sets[set];
	for (uint32_t b = 0; b < s->layout->bindingCount; b++)
		if (s->layout->bindings[b].binding == binding && element < s->layout->bindings[b].count)
			return &s->desc[s->layout->bindings[b].first + element];
	return NULL;
}

/* Builds the texture for one explicit texture unit from its image and sampler descriptors. */
static bool vkv_resolve_texture(VkvReplay *R, const VkvTexBinding *t, SceGxmTexture *out) {
	VkvDescriptor *img = vkv_find_descriptor(R, t->imageSet, t->imageBinding, t->imageElement);
	if (!img || !img->valid) return false;
	if (img->view) img->view->image->readSerial = R->dev->sceneSerial + 1; /* the scene being recorded */
	if (img->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || img->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
		*out = img->tex;
		return true;
	}
	if (img->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || !img->view) return false;
	*out = img->view->tex;
	if (t->samplerSet >= 0) {
		VkvDescriptor *sd = vkv_find_descriptor(R, t->samplerSet, t->samplerBinding, t->samplerElement);
		if (!sd || !sd->sampler) return false;
		vkv_texture_apply_sampler(out, sd->sampler, img->view->levelCount);
	}
	return true;
}

static void vkv_bind_resources(VkvReplay *R, const VkvPipeline *p) {
	SceGxmContext *ctx = R->ctx;
	uint32_t boundSamplers = 0, boundBuffers = 0;
	bool legacyTex = !p->texExplicit[VKV_STAGE_VERTEX] || !p->texExplicit[VKV_STAGE_FRAGMENT];
	for (uint32_t si = 0; si < VKV_MAX_SETS; si++) {
		VkvSet *s = R->sets[si];
		if (!s) continue;
		for (uint32_t b = 0; b < s->layout->bindingCount; b++) {
			const VkvDslBinding *lb = &s->layout->bindings[b];
			for (uint32_t e = 0; e < lb->count; e++) {
				VkvDescriptor *d = &s->desc[lb->first + e];
				if (!d->valid) continue;
				uint32_t slot = d->binding;
				if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
					if (slot >= SCE_GXM_MAX_UNIFORM_BUFFERS || !d->buffer->ptr) continue;
					uint32_t off = d->offset;
					if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) off += R->dyn[si][lb->dynIndex + e];
					const void *ptr = d->buffer->ptr + off;
					sceGxmSetVertexUniformBuffer(ctx, slot, ptr);
					sceGxmSetFragmentUniformBuffer(ctx, slot, ptr);
					boundBuffers |= 1u << slot;
				} else if (legacyTex && d->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
					if (slot >= SCE_GXM_MAX_TEXTURE_UNITS) continue;
					if (d->view) d->view->image->readSerial = R->dev->sceneSerial + 1;
					if (!p->texExplicit[VKV_STAGE_FRAGMENT]) sceGxmSetFragmentTexture(ctx, slot, &d->tex);
					if (!p->texExplicit[VKV_STAGE_VERTEX] && (p->vSamplerMask & (1u << slot)))
						sceGxmSetVertexTexture(ctx, slot, &d->tex);
					boundSamplers |= 1u << slot;
				}
			}
		}
	}
	for (int st = 0; st < VKV_STAGE_COUNT; st++) {
		if (!p->texExplicit[st]) continue;
		for (uint32_t i = 0; i < p->texCount[st]; i++) {
			const VkvTexBinding *t = &p->texb[st][i];
			SceGxmTexture tex;
			if (!vkv_resolve_texture(R, t, &tex)) {
				if (vkv_validate())
					vkv_fail("draw", "pipeline '%s/%s': texture unit %u needs the image at set %d binding %u%s", p->vname,
					         p->fname, (unsigned)t->unit, (int)t->imageSet, (unsigned)t->imageBinding,
					         t->samplerSet >= 0 ? " and a sampler, but one of them is not bound" : ", which is not bound");
				continue;
			}
			if (st == VKV_STAGE_FRAGMENT) sceGxmSetFragmentTexture(ctx, t->unit, &tex);
			else sceGxmSetVertexTexture(ctx, t->unit, &tex);
			boundSamplers |= 1u << t->unit;
		}
	}
	if (vkv_validate()) {
		uint32_t need = (p->texExplicit[VKV_STAGE_FRAGMENT] ? 0 : p->fSamplerMask) |
		                (p->texExplicit[VKV_STAGE_VERTEX] ? 0 : p->vSamplerMask);
		uint32_t missingS = need & ~boundSamplers;
		uint32_t missingB = p->bufferMask & ~boundBuffers;
		if (missingS) vkv_fail("draw", "pipeline '%s/%s' samples texture units 0x%x with no image bound", p->vname,
		                       p->fname, (unsigned)missingS);
		if (missingB) vkv_fail("draw", "pipeline '%s/%s' reads uniform buffers 0x%x with nothing bound", p->vname,
		                       p->fname, (unsigned)missingB);
	}
}

static const void *vkv_linear_indices(struct VkDevice_T *dev, uint32_t count, bool *is32) {
	if (count <= dev->linearIdx16Count && dev->linearIdx16.base) { *is32 = false; return dev->linearIdx16.base; }
	if (count > dev->linearIdx32Count) {
		vkv_device_wait_gpu(dev);
		vkv_gpu_free(&dev->linearIdx32);
		uint32_t n = VKV_ALIGN(count, 65536);
		if (!vkv_gpu_alloc(VKV_HEAP_RAM, n * 4, SCE_GXM_MEMORY_ATTRIB_READ, &dev->linearIdx32)) {
			dev->linearIdx32Count = 0;
			return NULL;
		}
		uint32_t *idx = dev->linearIdx32.base;
		for (uint32_t i = 0; i < n; i++) idx[i] = i;
		dev->linearIdx32Count = n;
	}
	*is32 = true;
	return dev->linearIdx32.base;
}

/* Indices for a line strip drawn as a line list: count - 1 vertex pairs. */
static const void *vkv_strip_indices(VkvReplay *R, const void *appIdx, bool *is32, uint32_t count) {
	struct VkDevice_T *d = R->dev;
	uint32_t pairs = count - 1;
	if (!appIdx) {
		if (pairs > VKV_STRIP_PAIRS || !d->stripIdx16.base) {
			VKV_UNSUPPORTED("non-indexed line strips with more than 65536 vertices (skipped)");
			return NULL;
		}
		*is32 = false;
		return d->stripIdx16.base;
	}
	uint32_t isz = *is32 ? 4 : 2, bytes = VKV_ALIGN(pairs * 2 * isz, 16);
	if (bytes > VKV_STRIP_RING) {
		VKV_UNSUPPORTED("indexed line strips with more than 32768 lines (skipped)");
		return NULL;
	}
	if (!d->stripRing.base && !vkv_gpu_alloc(VKV_HEAP_RAM, VKV_STRIP_RING, SCE_GXM_MEMORY_ATTRIB_READ, &d->stripRing))
		return NULL;
	uint32_t scene = d->sceneSerial + 1;
	if (d->stripRingPos + bytes > VKV_STRIP_RING) {
		/* the open scene still reads what it put in the ring */
		if (d->stripRingScene == scene) {
			VKV_UNSUPPORTED("more than 256 KB of indexed line strips in one render pass (skipped)");
			return NULL;
		}
		if (d->gpuBusy) vkv_device_wait_gpu(d);
		d->stripRingPos = 0;
	}
	uint8_t *out = (uint8_t *)d->stripRing.base + d->stripRingPos;
	if (*is32) {
		const uint32_t *in = appIdx;
		uint32_t *o = (uint32_t *)out;
		for (uint32_t k = 0; k < pairs; k++) { o[2 * k] = in[k]; o[2 * k + 1] = in[k + 1]; }
	} else {
		const uint16_t *in = appIdx;
		uint16_t *o = (uint16_t *)out;
		for (uint32_t k = 0; k < pairs; k++) { o[2 * k] = in[k]; o[2 * k + 1] = in[k + 1]; }
	}
	d->stripRingPos += bytes;
	d->stripRingScene = scene;
	return out;
}

static void vkv_exec_draw(VkvReplay *R, const VkvCmdDraw *c, bool indexed) {
	VkvPipeline *p = R->pipe;
	if (!R->inScene || !p) return;
	if (!p->primSupported || p->vkCull == VK_CULL_MODE_FRONT_AND_BACK) return;

	const void *indices;
	SceGxmIndexFormat ifmt;
	bool is32;
	if (indexed) {
		if (!R->ib || !R->ib->ptr) {
			if (vkv_validate()) vkv_fail("vkCmdDrawIndexed", "no index buffer bound");
			return;
		}
		is32 = R->ibType == VK_INDEX_TYPE_UINT32;
		uint32_t isz = is32 ? 4 : 2;
		if (vkv_validate() && R->ibOff + (c->first + c->count) * isz > R->ib->size) {
			vkv_fail("vkCmdDrawIndexed", "indices [%u..%u) run past the index buffer", (unsigned)c->first,
			         (unsigned)(c->first + c->count));
			return;
		}
		indices = R->ib->ptr + R->ibOff + c->first * isz;
	} else {
		indices = vkv_linear_indices(R->dev, c->count, &is32);
		if (!indices) return;
	}
	uint32_t drawCount = c->count;
	if (p->lineStrip) {
		if (c->count < 2) return;
		indices = vkv_strip_indices(R, indexed ? indices : NULL, &is32, c->count);
		if (!indices) return;
		drawCount = 2 * (c->count - 1);
	}
	ifmt = is32 ? SCE_GXM_INDEX_FORMAT_U32 : SCE_GXM_INDEX_FORMAT_U16;

	vkv_apply_pipeline(R, p, is32 ? 1 : 0);
	vkv_apply_viewport(R, p);
	/* a scissor between tiles: the fragment program variant discards the rest */
	bool exact = R->exactScissor && vkv_pipeline_scissor_program(R->dev, p);
	if (R->exactScissor && !exact) VKV_UNSUPPORTED("pixel-exact scissor for Cg shaders (GXM clips on 32x32 tiles)");
	if (exact != R->fpScissorOn) {
		sceGxmSetFragmentProgram(R->ctx, exact ? p->fpScissor : p->fp);
		R->fpScissorOn = exact;
	}
	vkv_bind_resources(R, p);

	/* base vertex: shift every per-vertex stream */
	int32_t baseVertex = indexed ? c->vertexOffset : (int32_t)c->first;
	for (uint32_t inst = 0; inst < c->instances; inst++) {
		bool ok = true;
		for (uint32_t s = 0; s < p->streamCount; s++) {
			uint32_t b = p->streamBinding[s];
			VkvBuffer *vb = R->vb[b];
			if (!vb || !vb->ptr) {
				if (vkv_validate()) vkv_fail("draw", "pipeline '%s' needs vertex binding %u, nothing bound", p->vname, (unsigned)b);
				ok = false;
				break;
			}
			const uint8_t *base = vb->ptr + R->vbOff[b];
			if (p->streamInstanced[s]) base += (size_t)(c->firstInstance + inst) * p->streamStride[s];
			else base += (intptr_t)baseVertex * p->streamStride[s];
			sceGxmSetVertexStream(R->ctx, s, base);
		}
		if (!ok) return;

		/* push constants -> default uniform buffers (re-reserved per draw) */
		void *ub;
		if (p->vDefaultSize && sceGxmReserveVertexDefaultUniformBuffer(R->ctx, &ub) >= 0)
			vkv_write_uniforms(R, ub, p->vuni, p->vuniCount, p->vname);
		if (exact) {
			if (p->fDefaultSizeScissor && sceGxmReserveFragmentDefaultUniformBuffer(R->ctx, &ub) >= 0)
				vkv_write_uniforms(R, ub, p->funiScissor, p->funiScissorCount, p->fname);
		} else if (p->fDefaultSize && sceGxmReserveFragmentDefaultUniformBuffer(R->ctx, &ub) >= 0) {
			vkv_write_uniforms(R, ub, p->funi, p->funiCount, p->fname);
		}

		R->sceneDraws++;
		int r = sceGxmDraw(R->ctx, p->prim, ifmt, indices, drawCount);
		if (r < 0) VKV_ERROR("sceGxmDraw failed 0x%08X (pipeline '%s')", (unsigned)r, p->vname);
		g_vkv.cur.drawCalls++;
		g_vkv.cur.indices += drawCount;
	}
}

static void vkv_exec_ff_draw(VkvReplay *R, const VkvCmdFFDraw *c) {
	if (!R->inScene) return;
	VkvPipeline *p = c->pipe;
	R->pipeDirty = true; /* FF pipelines change state behind the app's back */
	vkv_apply_pipeline(R, p, 0);
	vkv_apply_viewport(R, p);
	sceGxmSetVertexStream(R->ctx, 0, c->vertices);
	void *ub;
	if (p->vDefaultSize && sceGxmReserveVertexDefaultUniformBuffer(R->ctx, &ub) >= 0 && c->mvpParam)
		sceGxmSetUniformDataF(ub, c->mvpParam, 0, 16, c->mvp);
	if (p->fDefaultSize && sceGxmReserveFragmentDefaultUniformBuffer(R->ctx, &ub) >= 0 && c->alphaRefParam)
		sceGxmSetUniformDataF(ub, c->alphaRefParam, 0, 1, &c->alphaRef);
	if (c->textured) sceGxmSetFragmentTexture(R->ctx, 0, &c->tex);
	R->sceneDraws++;
	sceGxmDraw(R->ctx, c->prim, SCE_GXM_INDEX_FORMAT_U16, R->dev->linearIdx16.base, c->count);
	g_vkv.cur.drawCalls++;
	g_vkv.cur.ffDraws++;
	g_vkv.cur.indices += c->count;
	R->pipeDirty = true;
}

/* ------------------------------------------------------------------------ */
/* Transfers (executed by the CPU at submit time)                           */
/* ------------------------------------------------------------------------ */
static void vkv_transfer_sync(VkvReplay *R) {
	if (R->dev->gpuBusy && !vkv_compat(VKV_COMPAT_FAST_TRANSFERS)) vkv_device_wait_gpu(R->dev);
}

static void vkv_exec_copy_buf_img(VkvReplay *R, const VkvCmdCopyBufImg *c, bool toImage) {
	VkvImage *img = c->img;
	VkvBuffer *buf = c->buf;
	if (!img->ptr || !buf->ptr) return;
	vkv_transfer_sync(R);
	const uint32_t es = img->fi->bpp, bd = img->fi->block ? 4 : 1; /* element size, texels per element side */
	for (uint32_t i = 0; i < c->n; i++) {
		const VkBufferImageCopy *r = &c->r[i];
		uint32_t level = r->imageSubresource.mipLevel;
		if (level >= img->levels) {
			VKV_UNSUPPORTED("copies to mip levels that are not stored (skipped)");
			continue;
		}
		uint32_t ew, eh;
		vkv_image_level_elems(img, level, &ew, &eh);
		uint32_t rowLen = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
		uint32_t rows = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
		uint32_t ex = (uint32_t)r->imageOffset.x / bd, ey = (uint32_t)r->imageOffset.y / bd;
		uint32_t w = (r->imageExtent.width + bd - 1) / bd, h = (r->imageExtent.height + bd - 1) / bd;
		w = VKV_MIN(w, ew - VKV_MIN(ex, ew));
		h = VKV_MIN(h, eh - VKV_MIN(ey, eh));
		uint32_t srcPitch = (rowLen + bd - 1) / bd * es;
		uint32_t layerStride = srcPitch * ((rows + bd - 1) / bd);
		uint32_t layers = r->imageSubresource.layerCount ? r->imageSubresource.layerCount : 1;
		for (uint32_t L = 0; L < layers; L++) {
			uint32_t layer = r->imageSubresource.baseArrayLayer + L;
			if (layer >= img->layers) {
				VKV_UNSUPPORTED("copies to array layers that are not stored (skipped)");
				break;
			}
			uint64_t srcBase = r->bufferOffset + (uint64_t)L * layerStride;
			if (vkv_validate() && h && srcBase + (uint64_t)srcPitch * (h - 1) + w * es > buf->size) {
				vkv_fail(toImage ? "vkCmdCopyBufferToImage" : "vkCmdCopyImageToBuffer", "region %u reads past the buffer",
				         (unsigned)i);
				break;
			}
			for (uint32_t row = 0; row < h; row++) {
				uint8_t *bp = buf->ptr + srcBase + row * srcPitch;
				if (!img->swizzled) {
					uint8_t *ip = vkv_image_elem(img, level, layer, ex, ey + row);
					if (toImage) memcpy(ip, bp, w * es); else memcpy(bp, ip, w * es);
					continue;
				}
				for (uint32_t x = 0; x < w; x++) {
					uint8_t *ip = vkv_image_elem(img, level, layer, ex + x, ey + row);
					if (toImage) memcpy(ip, bp + x * es, es); else memcpy(bp + x * es, ip, es);
				}
			}
		}
		g_vkv.cur.copies++;
	}
}

/* Transfer formats that move texels of a given size unchanged. */
static bool vkv_transfer_format(uint32_t bpp, SceGxmTransferFormat *f) {
	switch (bpp) {
	case 1: *f = SCE_GXM_TRANSFER_FORMAT_U8_R; return true;
	case 2: *f = SCE_GXM_TRANSFER_FORMAT_U8U8_GR; return true;
	case 3: *f = SCE_GXM_TRANSFER_FORMAT_U8U8U8_BGR; return true;
	case 4: *f = SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR; return true;
	case 8: *f = SCE_GXM_TRANSFER_FORMAT_RAW64; return true;
	case 16: *f = SCE_GXM_TRANSFER_FORMAT_RAW128; return true;
	default: return false;
	}
}

/* Copies between linear images on the transfer unit, so the CPU neither waits
 * for the GPU nor touches the pixels. The transfer can wait for one sync
 * object: the source's when it is a render target (its pixels may still be
 * coming), else the destination's. When both are render targets the scenes
 * that draw into the source are waited for on the CPU. Returns false when
 * the CPU has to copy instead. */
static bool vkv_gpu_copy_image(VkvReplay *R, const VkvCmdCopyImage *c) {
	static uint32_t s_brokenSizes; /* texel sizes the transfer unit refused */
	VkvImage *src = c->src, *dst = c->dst;
	SceGxmTransferFormat tf;
	if ((s_brokenSizes & (1u << src->fi->bpp)) || vkv_compat(VKV_COMPAT_CPU_COPY) || src->swizzled || dst->swizzled || src->fi->block ||
	    src->fi->isDepth || dst->fi->isDepth || !vkv_transfer_format(src->fi->bpp, &tf))
		return false;
	/* every region must be one the transfer can do in full */
	for (uint32_t i = 0; i < c->n; i++) {
		const VkImageCopy *r = &c->r[i];
		if (r->srcSubresource.mipLevel >= src->levels || r->dstSubresource.mipLevel >= dst->levels) return false;
		if (r->srcOffset.x < 0 || r->srcOffset.y < 0 || r->dstOffset.x < 0 || r->dstOffset.y < 0) return false;
		if (r->srcSubresource.baseArrayLayer + VKV_MAX(r->srcSubresource.layerCount, 1u) > src->layers ||
		    r->dstSubresource.baseArrayLayer + VKV_MAX(r->srcSubresource.layerCount, 1u) > dst->layers)
			return false;
	}
	SceGxmSyncObject *sync = src->sync ? src->sync : dst->sync;
	if (src->sync && dst->sync && src != dst && R->dev->gpuBusy) vkv_device_wait_gpu(R->dev);
	/* scenes that still sample the destination must finish reading it first */
	if (dst->readSerial && R->dev->gpuBusy && !vkv_serial_done(R->dev, dst->readSerial)) vkv_device_wait_gpu(R->dev);
	for (uint32_t i = 0; i < c->n; i++) {
		const VkImageCopy *r = &c->r[i];
		uint32_t sl = r->srcSubresource.mipLevel, dl = r->dstSubresource.mipLevel;
		uint32_t sw, sh, dw, dh;
		vkv_image_level_elems(src, sl, &sw, &sh);
		vkv_image_level_elems(dst, dl, &dw, &dh);
		uint32_t sx = (uint32_t)r->srcOffset.x, sy = (uint32_t)r->srcOffset.y;
		uint32_t dx = (uint32_t)r->dstOffset.x, dy = (uint32_t)r->dstOffset.y;
		if (sx >= sw || sy >= sh || dx >= dw || dy >= dh) continue;
		uint32_t w = VKV_MIN(r->extent.width, VKV_MIN(sw - sx, dw - dx));
		uint32_t h = VKV_MIN(r->extent.height, VKV_MIN(sh - sy, dh - dy));
		uint32_t layers = VKV_MAX(r->srcSubresource.layerCount, 1u);
		for (uint32_t L = 0; L < layers; L++) {
			const uint8_t *sp = vkv_image_level_ptr(src, sl, r->srcSubresource.baseArrayLayer + L);
			uint8_t *dp = vkv_image_level_ptr(dst, dl, r->dstSubresource.baseArrayLayer + L);
			int res = sceGxmTransferCopy(w, h, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE, tf, SCE_GXM_TRANSFER_LINEAR, sp, sx,
			                             sy, (int32_t)src->levelPitch[sl], tf, SCE_GXM_TRANSFER_LINEAR, dp, dx, dy,
			                             (int32_t)dst->levelPitch[dl], sync, SCE_GXM_TRANSFER_FRAGMENT_SYNC, NULL);
			if (res < 0) {
				s_brokenSizes |= 1u << src->fi->bpp;
				VKV_WARN("sceGxmTransferCopy failed 0x%08X, copying %u byte texels with the CPU from now on",
				         (unsigned)res, (unsigned)src->fi->bpp);
				/* regions already issued are copied again by the CPU, which is harmless */
				vkv_device_wait_gpu(R->dev);
				return false;
			}
		}
		g_vkv.cur.copies++;
	}
	static bool s_told;
	if (!s_told) {
		s_told = true;
		VKV_INFO("image copies run on the transfer unit");
	}
	R->dev->usedTransfer = true;
	R->dev->transferPending = true;
	R->dev->gpuBusy = true; /* CPU access to either image now waits for the transfer */
	R->transferTail = true;
	return true;
}

static void vkv_exec_copy_image(VkvReplay *R, const VkvCmdCopyImage *c) {
	VkvImage *src = c->src, *dst = c->dst;
	if (!src->ptr || !dst->ptr || src->fi->bpp != dst->fi->bpp || src->fi->block != dst->fi->block) return;
	if (vkv_gpu_copy_image(R, c)) return;
	if (R->dev->gpuBusy) vkv_device_wait_gpu(R->dev); /* the source may have been rendered to */
	const uint32_t es = src->fi->bpp, bd = src->fi->block ? 4 : 1;
	for (uint32_t i = 0; i < c->n; i++) {
		const VkImageCopy *r = &c->r[i];
		uint32_t sl = r->srcSubresource.mipLevel, dl = r->dstSubresource.mipLevel;
		if (sl >= src->levels || dl >= dst->levels) continue;
		uint32_t sw, sh, dw, dh;
		vkv_image_level_elems(src, sl, &sw, &sh);
		vkv_image_level_elems(dst, dl, &dw, &dh);
		uint32_t sx = (uint32_t)r->srcOffset.x / bd, sy = (uint32_t)r->srcOffset.y / bd;
		uint32_t dx = (uint32_t)r->dstOffset.x / bd, dy = (uint32_t)r->dstOffset.y / bd;
		uint32_t w = (r->extent.width + bd - 1) / bd, h = (r->extent.height + bd - 1) / bd;
		if (sx >= sw || sy >= sh || dx >= dw || dy >= dh) continue;
		w = VKV_MIN(w, VKV_MIN(sw - sx, dw - dx));
		h = VKV_MIN(h, VKV_MIN(sh - sy, dh - dy));
		uint32_t layers = VKV_MAX(r->srcSubresource.layerCount, 1u);
		for (uint32_t L = 0; L < layers; L++) {
			uint32_t sa = r->srcSubresource.baseArrayLayer + L, da = r->dstSubresource.baseArrayLayer + L;
			if (sa >= src->layers || da >= dst->layers) break;
			for (uint32_t y = 0; y < h; y++) {
				if (!src->swizzled && !dst->swizzled) {
					memmove(vkv_image_elem(dst, dl, da, dx, dy + y), vkv_image_elem(src, sl, sa, sx, sy + y), w * es);
					continue;
				}
				for (uint32_t x = 0; x < w; x++)
					memcpy(vkv_image_elem(dst, dl, da, dx + x, dy + y), vkv_image_elem(src, sl, sa, sx + x, sy + y), es);
			}
		}
		g_vkv.cur.copies++;
	}
}

/* Formats whose texels are 1 to 4 unsigned bytes can be filtered byte by byte. */
static bool vkv_byte_channels(VkFormat f) {
	switch (f) {
	case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8_UNORM:
		return true;
	default:
		return false;
	}
}

static void vkv_blit_region(const VkvImage *src, const VkvImage *dst, const VkImageBlit *r, bool linear) {
	uint32_t sl = r->srcSubresource.mipLevel, dl = r->dstSubresource.mipLevel;
	if (sl >= src->levels || dl >= dst->levels) return;
	int32_t sw = (int32_t)vkv_image_level_width(src, sl), shh = (int32_t)vkv_image_level_height(src, sl);
	int32_t dw = (int32_t)vkv_image_level_width(dst, dl), dh = (int32_t)vkv_image_level_height(dst, dl);
	float sx0 = (float)r->srcOffsets[0].x, sy0 = (float)r->srcOffsets[0].y;
	float sx1 = (float)r->srcOffsets[1].x, sy1 = (float)r->srcOffsets[1].y;
	int32_t dx0 = r->dstOffsets[0].x, dy0 = r->dstOffsets[0].y, dx1 = r->dstOffsets[1].x, dy1 = r->dstOffsets[1].y;
	if (dx0 == dx1 || dy0 == dy1) return;
	float kx = (sx1 - sx0) / (float)(dx1 - dx0), ky = (sy1 - sy0) / (float)(dy1 - dy0);
	int32_t xa = VKV_MAX(VKV_MIN(dx0, dx1), 0), xb = VKV_MIN(VKV_MAX(dx0, dx1), dw);
	int32_t ya = VKV_MAX(VKV_MIN(dy0, dy1), 0), yb = VKV_MIN(VKV_MAX(dy0, dy1), dh);
	uint32_t bpp = dst->fi->bpp;
	uint32_t layers = VKV_MAX(r->srcSubresource.layerCount, 1u);
	for (uint32_t L = 0; L < layers; L++) {
		uint32_t sa = r->srcSubresource.baseArrayLayer + L, da = r->dstSubresource.baseArrayLayer + L;
		if (sa >= src->layers || da >= dst->layers) break;
		for (int32_t y = ya; y < yb; y++) {
			float v = sy0 + ((float)(y - dy0) + 0.5f) * ky - 0.5f;
			for (int32_t x = xa; x < xb; x++) {
				float u = sx0 + ((float)(x - dx0) + 0.5f) * kx - 0.5f;
				uint8_t *out = vkv_image_elem(dst, dl, da, (uint32_t)x, (uint32_t)y);
				if (!linear) {
					int32_t ix = VKV_MIN(VKV_MAX((int32_t)floorf(u + 0.5f), 0), sw - 1);
					int32_t iy = VKV_MIN(VKV_MAX((int32_t)floorf(v + 0.5f), 0), shh - 1);
					memcpy(out, vkv_image_elem(src, sl, sa, (uint32_t)ix, (uint32_t)iy), bpp);
					continue;
				}
				float fu = floorf(u), fv = floorf(v), ax = u - fu, ay = v - fv;
				int32_t x0 = VKV_MIN(VKV_MAX((int32_t)fu, 0), sw - 1), x1 = VKV_MIN(x0 + 1, sw - 1);
				int32_t y0 = VKV_MIN(VKV_MAX((int32_t)fv, 0), shh - 1), y1 = VKV_MIN(y0 + 1, shh - 1);
				if (fu < 0) { x0 = x1 = 0; }
				if (fv < 0) { y0 = y1 = 0; }
				const uint8_t *p00 = vkv_image_elem(src, sl, sa, (uint32_t)x0, (uint32_t)y0);
				const uint8_t *p10 = vkv_image_elem(src, sl, sa, (uint32_t)x1, (uint32_t)y0);
				const uint8_t *p01 = vkv_image_elem(src, sl, sa, (uint32_t)x0, (uint32_t)y1);
				const uint8_t *p11 = vkv_image_elem(src, sl, sa, (uint32_t)x1, (uint32_t)y1);
				for (uint32_t k = 0; k < bpp; k++) {
					float top = p00[k] + (p10[k] - p00[k]) * ax, bot = p01[k] + (p11[k] - p01[k]) * ax;
					out[k] = (uint8_t)(top + (bot - top) * ay + 0.5f);
				}
			}
		}
	}
}

static void vkv_exec_blit_image(VkvReplay *R, const VkvCmdBlitImage *c) {
	VkvImage *src = c->src, *dst = c->dst;
	if (!src->ptr || !dst->ptr || src->fi->block || dst->fi->block) return;
	if (src->fi->bpp != dst->fi->bpp) {
		VKV_UNSUPPORTED("blits that convert between formats of different sizes (skipped)");
		return;
	}
	if (R->dev->gpuBusy) vkv_device_wait_gpu(R->dev);
	bool linear = c->filter == VK_FILTER_LINEAR && vkv_byte_channels(src->format) && src->format == dst->format;
	if (c->filter == VK_FILTER_LINEAR && !linear) VKV_UNSUPPORTED("linear blits of non 8-bit formats (using nearest)");
	for (uint32_t i = 0; i < c->n; i++) vkv_blit_region(src, dst, &c->r[i], linear);
	g_vkv.cur.copies += c->n;
}

static void vkv_exec_draw_indirect(VkvReplay *R, const VkvCmdDrawIndirect *c) {
	for (uint32_t i = 0; i < c->count; i++) {
		uint32_t off = c->off + i * c->stride;
		VkvCmdDraw d;
		memset(&d, 0, sizeof(d));
		if (c->indexed) {
			if (off + sizeof(VkDrawIndexedIndirectCommand) > c->buf->size) break;
			VkDrawIndexedIndirectCommand cmd;
			memcpy(&cmd, c->buf->ptr + off, sizeof(cmd));
			d.count = cmd.indexCount;
			d.instances = cmd.instanceCount;
			d.first = cmd.firstIndex;
			d.vertexOffset = cmd.vertexOffset;
			d.firstInstance = cmd.firstInstance;
		} else {
			if (off + sizeof(VkDrawIndirectCommand) > c->buf->size) break;
			VkDrawIndirectCommand cmd;
			memcpy(&cmd, c->buf->ptr + off, sizeof(cmd));
			d.count = cmd.vertexCount;
			d.instances = cmd.instanceCount;
			d.first = cmd.firstVertex;
			d.firstInstance = cmd.firstInstance;
		}
		if (d.count && d.instances) vkv_exec_draw(R, &d, c->indexed);
	}
}

static void vkv_exec_clear_color_image(VkvReplay *R, const VkvCmdClearColorImage *c) {
	VkvImage *img = c->img;
	if (!img->sync) vkv_image_ensure_sync(img);
	for (uint32_t i = 0; i < c->n; i++) {
		const VkImageSubresourceRange *sr = &c->ranges[i];
		uint32_t lc = sr->levelCount == VK_REMAINING_MIP_LEVELS ? img->levels : sr->levelCount;
		uint32_t ac = sr->layerCount == VK_REMAINING_ARRAY_LAYERS ? img->layers : sr->layerCount;
		for (uint32_t l = sr->baseMipLevel; l < VKV_MIN(sr->baseMipLevel + lc, img->levels); l++)
			for (uint32_t a = sr->baseArrayLayer; a < VKV_MIN(sr->baseArrayLayer + ac, img->layers); a++)
				vkv_clear_color_rect(R, img, l, a, &c->c, 0, 0, vkv_image_level_width(img, l),
				                     vkv_image_level_height(img, l));
	}
}

/* ------------------------------------------------------------------------ */
/* Replay                                                                   */
/* ------------------------------------------------------------------------ */
static const char *vkv_cmd_name(uint16_t t) {
	static const char *names[] = { "?", "BeginRenderPass", "EndRenderPass", "BindPipeline", "BindVertexBuffers",
		"BindIndexBuffer", "BindDescriptorSets", "PushConstants", "SetViewport", "SetScissor", "SetDepthBias",
		"SetStencil", "Draw", "DrawIndexed", "CopyBuffer", "CopyBufferToImage", "CopyImageToBuffer",
		"UpdateBuffer", "FillBuffer", "ClearAttachments", "ClearColorImage", "Barrier", "FixedFunctionDraw",
		"CopyImage", "BlitImage", "ClearDepthStencilImage", "DrawIndirect", "ExecuteCommands", "SetLineWidth",
		"BeginQuery", "EndQuery", "ResetQueryPool", "WriteTimestamp", "SetEvent", "NextSubpass", "CopyQueryPoolResults" };
	return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

static void vkv_replay(VkvReplay *R, struct VkCommandBuffer_T *cb) {
	const uint8_t *p = cb->data, *end = cb->data + cb->size;
	while (p < end) {
		const VkvCmd *h = (const VkvCmd *)p;
		const uint8_t *next = p + h->size;
		if (vkv_dbg(VKV_DEBUG_LOG_CALLS)) vkv_log(VKV_LOG_DEBUG, "replay: %s", vkv_cmd_name(h->type));
		switch ((VkvCmdType)h->type) {
		case VKV_CMD_BEGIN_RP:
			vkv_exec_begin_rp(R, (const VkvCmdBeginRP *)h, next, end);
			break;
		case VKV_CMD_NEXT_SUBPASS:
			vkv_exec_next_subpass(R, next, end);
			break;
		case VKV_CMD_END_RP:
			vkv_end_scene(R, true);
			if (R->scenesLeft) R->scenesLeft--;
			R->rp = NULL;
			R->fb = NULL;
			break;
		case VKV_CMD_BIND_PIPELINE: {
			VkvPipeline *np = ((const VkvCmdBindPipeline *)h)->p;
			if (np != R->pipe) { R->pipe = np; R->pipeDirty = true; }
			break;
		}
		case VKV_CMD_BIND_VB: {
			const VkvCmdBindVB *c = (const VkvCmdBindVB *)h;
			for (uint32_t i = 0; i < c->count; i++) { R->vb[c->first + i] = c->buf[i]; R->vbOff[c->first + i] = c->off[i]; }
			break;
		}
		case VKV_CMD_BIND_IB: {
			const VkvCmdBindIB *c = (const VkvCmdBindIB *)h;
			R->ib = c->buf; R->ibOff = c->off; R->ibType = c->type;
			break;
		}
		case VKV_CMD_BIND_SETS: {
			const VkvCmdBindSets *c = (const VkvCmdBindSets *)h;
			uint32_t d = 0;
			for (uint32_t i = 0; i < c->count; i++) {
				VkvSet *s = c->sets[i];
				R->sets[c->first + i] = s;
				if (!s) continue;
				for (uint32_t k = 0; k < s->layout->dynamicCount && k < VKV_MAX_DYNAMIC; k++)
					R->dyn[c->first + i][k] = d < c->dynCount ? c->dyn[d++] : 0;
			}
			g_vkv.cur.descriptorBinds++;
			break;
		}
		case VKV_CMD_PUSH: {
			const VkvCmdPush *c = (const VkvCmdPush *)h;
			memcpy(R->push + c->offset, c->data, c->size);
			R->pushHigh = VKV_MAX(R->pushHigh, c->offset + c->size);
			break;
		}
		case VKV_CMD_VIEWPORT:
			R->viewport = ((const VkvCmdViewport *)h)->vp;
			R->hasViewport = true;
			R->viewportDirty = true;
			break;
		case VKV_CMD_SCISSOR:
			R->scissor = ((const VkvCmdScissor *)h)->rect;
			R->hasScissor = true;
			R->scissorDirty = true;
			break;
		case VKV_CMD_DEPTH_BIAS: {
			const VkvCmdDepthBias *c = (const VkvCmdDepthBias *)h;
			R->biasConst = c->constant; R->biasSlope = c->slope;
			R->dynSet |= VKV_DYN_DEPTH_BIAS;
			R->pipeDirty = true;
			break;
		}
		case VKV_CMD_STENCIL: {
			const VkvCmdStencil *c = (const VkvCmdStencil *)h;
			uint32_t *dst = c->what == VKV_DYN_STENCIL_CMP ? R->sCmp : c->what == VKV_DYN_STENCIL_WR ? R->sWr : R->sRef;
			if (c->faces & VK_STENCIL_FACE_FRONT_BIT) dst[0] = c->value;
			if (c->faces & VK_STENCIL_FACE_BACK_BIT) dst[1] = c->value;
			R->dynSet |= c->what;
			R->pipeDirty = true;
			break;
		}
		case VKV_CMD_DRAW:
			vkv_exec_draw(R, (const VkvCmdDraw *)h, false);
			break;
		case VKV_CMD_DRAW_INDEXED:
			vkv_exec_draw(R, (const VkvCmdDraw *)h, true);
			break;
		case VKV_CMD_COPY_BUFFER: {
			const VkvCmdCopyBuffer *c = (const VkvCmdCopyBuffer *)h;
			if (!c->src->ptr || !c->dst->ptr) break;
			vkv_transfer_sync(R);
			for (uint32_t i = 0; i < c->n; i++)
				memmove(c->dst->ptr + c->r[i].dstOffset, c->src->ptr + c->r[i].srcOffset, c->r[i].size);
			g_vkv.cur.copies += c->n;
			break;
		}
		case VKV_CMD_COPY_BUF_IMG:
			vkv_exec_copy_buf_img(R, (const VkvCmdCopyBufImg *)h, true);
			break;
		case VKV_CMD_COPY_IMG_BUF:
			if (R->dev->gpuBusy) vkv_device_wait_gpu(R->dev); /* must see rendered pixels */
			vkv_exec_copy_buf_img(R, (const VkvCmdCopyBufImg *)h, false);
			break;
		case VKV_CMD_UPDATE_BUFFER: {
			const VkvCmdUpdateBuffer *c = (const VkvCmdUpdateBuffer *)h;
			if (!c->dst->ptr) break;
			vkv_transfer_sync(R);
			memcpy(c->dst->ptr + c->off, c->data, c->size);
			g_vkv.cur.copies++;
			break;
		}
		case VKV_CMD_FILL_BUFFER: {
			const VkvCmdFillBuffer *c = (const VkvCmdFillBuffer *)h;
			if (!c->dst->ptr) break;
			vkv_transfer_sync(R);
			uint32_t *q = (uint32_t *)(c->dst->ptr + c->off);
			for (uint32_t i = 0; i < c->size / 4; i++) q[i] = c->value;
			break;
		}
		case VKV_CMD_CLEAR_ATTACH:
			vkv_exec_clear_attachments(R, (const VkvCmdClearAttach *)h);
			break;
		case VKV_CMD_CLEAR_COLOR_IMAGE:
			vkv_exec_clear_color_image(R, (const VkvCmdClearColorImage *)h);
			break;
		case VKV_CMD_BARRIER:
			if (R->dev->gpuBusy && !vkv_compat(VKV_COMPAT_FAST_TRANSFERS)) vkv_device_wait_gpu(R->dev);
			break;
		case VKV_CMD_FF_DRAW:
			vkv_exec_ff_draw(R, (const VkvCmdFFDraw *)h);
			break;
		case VKV_CMD_COPY_IMAGE:
			vkv_exec_copy_image(R, (const VkvCmdCopyImage *)h);
			break;
		case VKV_CMD_BLIT_IMAGE:
			vkv_exec_blit_image(R, (const VkvCmdBlitImage *)h);
			break;
		case VKV_CMD_CLEAR_DS_IMAGE: {
			const VkvCmdClearDSImage *c = (const VkvCmdClearDSImage *)h;
			c->img->pendingDepthClear = true;
			c->img->clearDepth = c->v.depth;
			c->img->clearStencil = c->v.stencil;
			break;
		}
		case VKV_CMD_DRAW_INDIRECT:
			vkv_exec_draw_indirect(R, (const VkvCmdDrawIndirect *)h);
			break;
		case VKV_CMD_EXECUTE: {
			const VkvCmdExecute *c = (const VkvCmdExecute *)h;
			for (uint32_t i = 0; i < c->n; i++)
				if (c->cbs[i] && c->cbs[i]->state == VKV_CB_EXECUTABLE) vkv_replay(R, c->cbs[i]);
			R->pipeDirty = true;
			break;
		}
		case VKV_CMD_LINE_WIDTH:
			R->lineWidth = ((const VkvCmdLineWidth *)h)->width;
			R->dynSet |= VKV_DYN_LINE_WIDTH;
			R->pipeDirty = true;
			break;
		case VKV_CMD_QUERY_BEGIN: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			if (R->inScene) vkv_query_begin(R->dev, R->ctx, c->pool, c->first);
			R->queryActive = R->inScene && c->pool->type == VK_QUERY_TYPE_OCCLUSION && R->dev->visBufSet;
			break;
		}
		case VKV_CMD_QUERY_END: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			if (R->inScene) vkv_query_end(R->dev, R->ctx, c->pool, c->first);
			R->queryActive = false;
			break;
		}
		case VKV_CMD_QUERY_RESET: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			vkv_query_reset(R->dev, c->pool, c->first, c->count);
			break;
		}
		case VKV_CMD_TIMESTAMP: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			if (c->pool->values) c->pool->values[c->first] = vkv_time_us() * 1000u;
			c->pool->written[c->first] = 1;
			break;
		}
		case VKV_CMD_COPY_QUERY: {
			/* The counters are final once the scenes that ended the queries are
			 * done, so wait for those; the buffer is plain memory the CPU writes. */
			const VkvCmdCopyQuery *c = (const VkvCmdCopyQuery *)h;
			if (!c->dst->ptr) break;
			if (c->pool->type == VK_QUERY_TYPE_OCCLUSION && R->dev->gpuBusy) vkv_device_wait_gpu(R->dev);
			vkv_query_results(R->dev, c->pool, c->first, c->count, c->dst->size - c->off, c->dst->ptr + c->off,
			                  c->stride, c->flags);
			break;
		}
		case VKV_CMD_SET_EVENT: {
			const VkvCmdSetEvent *c = (const VkvCmdSetEvent *)h;
			if (c->event) c->event->set = c->value;
			break;
		}
		default:
			VKV_ERROR("corrupt command stream (type %u)", (unsigned)h->type);
			return;
		}
		p = next;
	}
}

VkResult vkv_queue_execute(struct VkDevice_T *dev, uint32_t cbCount, const VkCommandBuffer *cbs, VkvFence *fence) {
	VkvReplay R;
	memset(&R, 0, sizeof(R));
	R.dev = dev;
	R.ctx = dev->ctx;
	R.vpVariant = -1;
	R.fence = fence;
	for (uint32_t i = 0; i < cbCount; i++) R.scenesLeft += cbs[i]->sceneCount;
	if (fence) {
		fence->signaled = false;
		fence->pending = R.scenesLeft > 0;
		if (!fence->pending) fence->signaled = true;
	}
	for (uint32_t i = 0; i < cbCount; i++) {
		struct VkCommandBuffer_T *cb = cbs[i];
		if (VKV_CHECK(VKV_OBJ_OK(cb, VKV_MAGIC_CMDBUF), "invalid command buffer in submit") ||
		    VKV_CHECK(cb->state == VKV_CB_EXECUTABLE, "submitting a command buffer that was not ended")) {
			R.scenesLeft -= VKV_MIN(R.scenesLeft, cb->sceneCount);
			continue;
		}
		/* state does not carry across command buffers in Vulkan */
		R.pipe = NULL;
		R.pipeDirty = true;
		memset(R.vb, 0, sizeof(R.vb));
		R.ib = NULL;
		memset(R.sets, 0, sizeof(R.sets));
		R.hasViewport = R.hasScissor = false;
		R.dynSet = 0;
		R.pushHigh = 0;
		vkv_replay(&R, cb);
		if (R.inScene) {
			VKV_ERROR("command buffer ended inside a render pass; closing the scene");
			vkv_end_scene(&R, true);
		}
		if (cb->usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) cb->state = VKV_CB_INITIAL;
	}
	/* A scene that failed to begin (or was skipped) never carried the
	 * notification: wait for the GPU and signal on the CPU instead of
	 * leaving the app blocked in vkWaitForFences forever. */
	/* Transfers after the scene that carries the fence, or in a submit without
	 * scenes, are not covered by it: finish them before the fence counts as
	 * signaled. They are short copies and fills. */
	if (fence && R.transferTail) sceGxmTransferFinish();
	if (fence && fence->pending && !R.fenceAttached) {
		VKV_WARN("fence could not be attached to a scene; signaling after a full GPU wait");
		vkv_device_wait_gpu(dev);
		fence->pending = false;
		fence->signaled = true;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
	VKV_TRACE("vkQueueSubmit");
	struct VkDevice_T *dev = queue->device;
	uint64_t t0 = vkv_time_us();
	VkvFence *f = VKV_ND(VkvFence, fence);
	if (f && VKV_CHECK(VKV_OBJ_OK(f, VKV_MAGIC_FENCE), "invalid fence")) f = NULL;
	if (f && VKV_CHECK(!f->pending && !f->signaled, "fence passed to vkQueueSubmit is not reset")) { /* continue */ }

	/* Gather every command buffer so the fence lands on the very last scene. */
	uint32_t total = 0;
	for (uint32_t i = 0; i < count; i++) total += submits[i].commandBufferCount;
	VkCommandBuffer stackCbs[16];
	VkCommandBuffer *cbs = total <= 16 ? stackCbs : malloc(total * sizeof(VkCommandBuffer));
	if (!cbs) return VK_ERROR_OUT_OF_HOST_MEMORY;
	uint32_t n = 0;
	for (uint32_t i = 0; i < count; i++)
		for (uint32_t j = 0; j < submits[i].commandBufferCount; j++) cbs[n++] = submits[i].pCommandBuffers[j];
	VkResult r = vkv_queue_execute(dev, n, cbs, f);
	if (cbs != stackCbs) free(cbs);

	g_vkv.cur.submitUs += vkv_time_us() - t0;
	if (vkv_dbg(VKV_DEBUG_GPU_SYNC)) {
		uint64_t t1 = vkv_time_us();
		vkv_device_wait_gpu(dev);
		vkv_log(VKV_LOG_DEBUG, "gpu_sync: submit of %u command buffers took %.2f ms CPU + %.2f ms GPU", (unsigned)n,
		        (t1 - t0) / 1000.0f, (vkv_time_us() - t1) / 1000.0f);
	}
	return r;
}

/* ------------------------------------------------------------------------ */
/* Swapchain                                                                */
/* ------------------------------------------------------------------------ */
static void vkv_swapchain_free(struct VkDevice_T *dev, VkvSwapchain *sc) {
	vkv_device_wait_gpu(dev);
	sceGxmDisplayQueueFinish();
	for (uint32_t i = 0; i < sc->imageCount; i++) {
		if (sc->images[i]) {
			if (sc->images[i]->sync) sceGxmSyncObjectDestroy(sc->images[i]->sync);
			sc->images[i]->obj.magic = VKV_MAGIC_DEAD;
			free(sc->images[i]);
		}
		vkv_gpu_free(&sc->mem[i]);
	}
	if (dev->swapchain == sc) dev->swapchain = NULL;
	sc->obj.magic = VKV_MAGIC_DEAD;
	free(sc);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                    const VkAllocationCallbacks *alloc, VkSwapchainKHR *pSwapchain) {
	VKV_TRACE("vkCreateSwapchainKHR");
	(void)alloc;
	if (ci->imageExtent.width != VKV_DISPLAY_WIDTH || ci->imageExtent.height != VKV_DISPLAY_HEIGHT) {
		VKV_ERROR("swapchain extent must be %ux%u (got %ux%u)", VKV_DISPLAY_WIDTH, VKV_DISPLAY_HEIGHT,
		          (unsigned)ci->imageExtent.width, (unsigned)ci->imageExtent.height);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (ci->imageFormat != VK_FORMAT_R8G8B8A8_UNORM && ci->imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
	    ci->imageFormat != VK_FORMAT_R8G8B8A8_SRGB && ci->imageFormat != VK_FORMAT_B8G8R8A8_SRGB) {
		VKV_ERROR("swapchain format %d is not supported", (int)ci->imageFormat);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	VkvSwapchain *old = VKV_ND(VkvSwapchain, ci->oldSwapchain);
	if (device->swapchain && device->swapchain != old) {
		VKV_ERROR("only one swapchain can exist at a time");
		return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
	}

	VkvSwapchain *sc = calloc(1, sizeof(*sc));
	if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
	sc->obj.magic = VKV_MAGIC_SWAPCHAIN;
	sc->device = device;
	sc->imageCount = VKV_MIN(VKV_MAX(ci->minImageCount, 2u), (uint32_t)VKV_MAX_SWAP_IMAGES);
	sc->presentMode = ci->presentMode;
	sc->format = ci->imageFormat;
	sc->width = VKV_DISPLAY_WIDTH;
	sc->height = VKV_DISPLAY_HEIGHT;

	for (uint32_t i = 0; i < sc->imageCount; i++) {
		VkvImage *img = calloc(1, sizeof(*img));
		if (!img) { vkv_swapchain_free(device, sc); return VK_ERROR_OUT_OF_HOST_MEMORY; }
		sc->images[i] = img;
		img->obj.magic = VKV_MAGIC_IMAGE;
		img->format = ci->imageFormat;
		img->fi = vkv_format(ci->imageFormat);
		img->width = sc->width;
		img->height = sc->height;
		img->mipLevels = img->arrayLayers = 1;
		img->usage = ci->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		img->swapchain = true;
		vkv_image_setup_layout(img);
		if (!vkv_gpu_alloc(VKV_HEAP_CDRAM, img->size, SCE_GXM_MEMORY_ATTRIB_RW, &sc->mem[i]) ||
		    vkv_image_ensure_sync(img) != VK_SUCCESS) {
			vkv_swapchain_free(device, sc);
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		memset(sc->mem[i].base, 0, img->size);
		sc->memObj[i].obj.magic = VKV_MAGIC_MEMORY;
		sc->memObj[i].ptr = sc->mem[i].base;
		sc->memObj[i].size = img->size;
		img->mem = &sc->memObj[i];
		img->ptr = sc->mem[i].base;
	}
	sc->acquireIndex = 0;
	sc->frontIndex = sc->imageCount - 1;
	device->swapchain = sc;
	VKV_INFO("swapchain: %u images %ux%u, format %d, present mode %d", (unsigned)sc->imageCount, (unsigned)sc->width,
	         (unsigned)sc->height, (int)sc->format, (int)sc->presentMode);
	*pSwapchain = VKV_H(VkSwapchainKHR, sc);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                 const VkAllocationCallbacks *alloc) {
	VKV_TRACE("vkDestroySwapchainKHR");
	(void)alloc;
	VkvSwapchain *sc = VKV_ND(VkvSwapchain, swapchain);
	if (sc) vkv_swapchain_free(device, sc);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pCount,
                                                       VkImage *pImages) {
	(void)device;
	VkvSwapchain *sc = VKV_ND(VkvSwapchain, swapchain);
	if (!pImages) { *pCount = sc->imageCount; return VK_SUCCESS; }
	uint32_t n = VKV_MIN(*pCount, sc->imageCount);
	for (uint32_t i = 0; i < n; i++) pImages[i] = VKV_H(VkImage, sc->images[i]);
	*pCount = n;
	return n < sc->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                     VkSemaphore semaphore, VkFence fence, uint32_t *pIndex) {
	VKV_TRACE("vkAcquireNextImageKHR");
	(void)device; (void)timeout; (void)semaphore;
	VkvSwapchain *sc = VKV_ND(VkvSwapchain, swapchain);
	if (VKV_CHECK(VKV_OBJ_OK(sc, VKV_MAGIC_SWAPCHAIN), "invalid swapchain")) return VK_ERROR_OUT_OF_DATE_KHR;
	/* Display ordering is enforced on the GPU through each image's sync
	 * object, so the image can be handed out immediately. */
	*pIndex = sc->acquireIndex;
	sc->acquireIndex = (sc->acquireIndex + 1) % sc->imageCount;
	VkvFence *f = VKV_ND(VkvFence, fence);
	if (f) { f->signaled = true; f->pending = false; }
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *ai,
                                                      uint32_t *pIndex) {
	return vkAcquireNextImageKHR(device, ai->swapchain, ai->timeout, ai->semaphore, ai->fence, pIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
	VKV_TRACE("vkQueuePresentKHR");
	struct VkDevice_T *dev = queue->device;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < pi->swapchainCount; i++) {
		VkvSwapchain *sc = VKV_ND(VkvSwapchain, pi->pSwapchains[i]);
		uint32_t idx = pi->pImageIndices[i];
		VkResult r = VK_SUCCESS;
		if (VKV_CHECK(VKV_OBJ_OK(sc, VKV_MAGIC_SWAPCHAIN) && idx < sc->imageCount, "bad swapchain/image index")) {
			r = VK_ERROR_OUT_OF_DATE_KHR;
		} else {
			VkvImage *img = sc->images[idx];
			VkvDisplayData d;
			d.address = img->ptr;
			d.width = sc->width;
			d.height = sc->height;
			d.pitch = img->alignedW;
			d.vsync = sc->presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR;
			if (vkv_compat(VKV_COMPAT_FORCE_VSYNC)) d.vsync = 1;
			if (vkv_compat(VKV_COMPAT_NO_VSYNC)) d.vsync = 0;
			int e = sceGxmDisplayQueueAddEntry(sc->images[sc->frontIndex]->sync, img->sync, &d);
			if (e < 0) {
				VKV_ERROR("sceGxmDisplayQueueAddEntry failed 0x%08X", (unsigned)e);
				r = VK_ERROR_DEVICE_LOST;
			}
			sc->frontIndex = idx;
		}
		if (pi->pResults) pi->pResults[i] = r;
		if (r != VK_SUCCESS) result = r;
	}
	if (vkv_dbg(VKV_DEBUG_GPU_SYNC)) vkv_device_wait_gpu(dev);
	vkv_stats_frame_end();
	return result;
}
