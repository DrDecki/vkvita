/*
 * VkVita: queue submission (command replay onto the GXM context),
 * swapchain and presentation.
 * SPDX-License-Identifier: MIT
 */
#include "vkv_internal.h"

typedef struct VkvReplay {
	struct VkDevice_T        *dev;
	SceGxmContext            *ctx;

	/* render pass */
	bool                      inScene;
	VkvRenderPass            *rp;
	VkvFramebuffer           *fb;
	VkvImage                 *colorImg, *depthImg;
	SceGxmColorSurface        color;
	SceGxmDepthStencilSurface depth;
	bool                      splits;         /* pass contains mid-pass clears */
	uint32_t                  scenesLeft;     /* render passes left in this submit */
	VkvFence                 *fence;
	bool                      fenceAttached;

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

/* Fills a rectangle of a color image, outside of any scene. */
static void vkv_clear_color_rect(VkvReplay *R, VkvImage *img, const VkClearColorValue *c, uint32_t x, uint32_t y,
                                 uint32_t w, uint32_t h) {
	if (!img->ptr || !w || !h) return;
	x = VKV_MIN(x, img->width); y = VKV_MIN(y, img->height);
	w = VKV_MIN(w, img->width - x); h = VKV_MIN(h, img->height - y);
	uint8_t texel[16];
	uint32_t bpp = vkv_pack_color(img, c, texel);
	g_vkv.cur.clears++;

	SceGxmTransferFormat xf = img->fi->xfer;
	if (!vkv_compat(VKV_COMPAT_CPU_CLEAR) && img->sync && (bpp == 4 || bpp == 2) && xf) {
		uint32_t value = 0;
		memcpy(&value, texel, bpp);
		int r = sceGxmTransferFill(value, xf, img->ptr, x, y, w, h, (int32_t)img->rowPitch, img->sync,
		                           SCE_GXM_TRANSFER_FRAGMENT_SYNC, NULL);
		if (r >= 0) {
			R->dev->usedTransfer = true;
			R->dev->transferPending = true;
			return;
		}
		VKV_WARN("sceGxmTransferFill failed 0x%08X, falling back to CPU clear", (unsigned)r);
	}
	/* CPU path: the GPU must not be using the image */
	if (R->dev->gpuBusy) vkv_device_wait_gpu(R->dev);
	for (uint32_t row = 0; row < h; row++) {
		uint8_t *p = img->ptr + (y + row) * img->rowPitch + x * bpp;
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
	if (R->depthImg) {
		const VkvAttachmentDesc *a = &rp->att[rp->depthIndex];
		sceGxmDepthStencilSurfaceInit(&R->depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
		                              R->depthImg->alignedW, R->depthImg->ptr, NULL);
		if (clearDepth) {
			sceGxmDepthStencilSurfaceSetBackgroundDepth(&R->depth, depth);
			sceGxmDepthStencilSurfaceSetBackgroundStencil(&R->depth, (unsigned char)stencil);
		}
		sceGxmDepthStencilSurfaceSetForceLoadMode(&R->depth, loadDepth ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED
		                                                                : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
		bool store = R->splits || a->storeOp == VK_ATTACHMENT_STORE_OP_STORE ||
		             a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
		sceGxmDepthStencilSurfaceSetForceStoreMode(&R->depth, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
		                                                            : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
	}
	if (R->colorImg) {
		VkvImage *img = R->colorImg;
		SceGxmColorFormat cf = img->swapchain ? SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR : img->fi->color;
		SceGxmOutputRegisterSize rs = img->fi->bpp == 8 ? SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT
		                                                : SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
		int r = sceGxmColorSurfaceInit(&R->color, cf, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
		                               rs, fb->width, fb->height, img->alignedW, img->ptr);
		if (r < 0) VKV_ERROR("sceGxmColorSurfaceInit failed 0x%08X", (unsigned)r);
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
	const SceGxmNotification *notif = NULL;
	if (final && R->fence && R->scenesLeft == 1) {
		notif = &R->fence->notif;
		R->fenceAttached = true;
	}
	int r = sceGxmEndScene(R->ctx, NULL, notif);
	if (r < 0) VKV_ERROR("sceGxmEndScene failed 0x%08X", (unsigned)r);
	R->inScene = false;
	R->dev->gpuBusy = true;
	if (R->depthImg) R->depthImg->written = true;
}

static bool vkv_pass_has_splits(const uint8_t *p, const uint8_t *end) {
	while (p < end) {
		const VkvCmd *c = (const VkvCmd *)p;
		if (c->type == VKV_CMD_END_RP) return false;
		if (c->type == VKV_CMD_CLEAR_ATTACH) return true;
		p += c->size;
	}
	return false;
}

static void vkv_exec_begin_rp(VkvReplay *R, const VkvCmdBeginRP *c, const uint8_t *next, const uint8_t *end) {
	VkvRenderPass *rp = c->rp;
	VkvFramebuffer *fb = c->fb;
	R->rp = rp;
	R->fb = fb;
	R->colorImg = rp->colorIndex >= 0 ? fb->views[rp->colorIndex]->image : NULL;
	R->depthImg = rp->depthIndex >= 0 ? fb->views[rp->depthIndex]->image : NULL;
	R->splits = vkv_pass_has_splits(next, end);

	if (R->colorImg) {
		const VkvAttachmentDesc *a = &rp->att[rp->colorIndex];
		if (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)rp->colorIndex < c->clearCount)
			vkv_clear_color_rect(R, R->colorImg, &c->clears[rp->colorIndex].color, (uint32_t)c->area.offset.x,
			                     (uint32_t)c->area.offset.y, c->area.extent.width, c->area.extent.height);
	}
	bool loadDepth = false, clearDepth = false;
	float d = 1.0f;
	uint32_t s = 0;
	if (R->depthImg) {
		const VkvAttachmentDesc *a = &rp->att[rp->depthIndex];
		loadDepth = a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && R->depthImg->written;
		if (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)rp->depthIndex < c->clearCount) {
			clearDepth = true;
			d = c->clears[rp->depthIndex].depthStencil.depth;
			s = c->clears[rp->depthIndex].depthStencil.stencil;
		}
	}
	if (vkv_validate() && (c->area.offset.x || c->area.offset.y || c->area.extent.width != fb->width ||
	                       c->area.extent.height != fb->height))
		VKV_UNSUPPORTED("partial render areas (whole framebuffer is rendered)");
	vkv_begin_scene(R, loadDepth, clearDepth, d, s);
}

static void vkv_exec_clear_attachments(VkvReplay *R, const VkvCmdClearAttach *c) {
	if (!R->inScene) return;
	bool clearDepth = false;
	float d = 1.0f;
	uint32_t s = 0;
	vkv_end_scene(R, false);
	for (uint32_t i = 0; i < c->na; i++) {
		const VkClearAttachment *a = &c->a[i];
		if ((a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && R->colorImg) {
			for (uint32_t k = 0; k < c->nr; k++)
				vkv_clear_color_rect(R, R->colorImg, &a->clearValue.color, (uint32_t)c->r[k].rect.offset.x,
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
	R->viewportDirty = true;
	R->scissorDirty = true;
}

static void vkv_apply_viewport(VkvReplay *R, const VkvPipeline *p) {
	if (R->viewportDirty) {
		R->viewportDirty = false;
		VkViewport v;
		if (p->hasViewport) v = p->viewport;
		else if (R->hasViewport) v = R->viewport;
		else v = (VkViewport){ 0, 0, (float)R->fb->width, (float)R->fb->height, 0.0f, 1.0f };
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
		uint32_t w = R->fb->width, h = R->fb->height;
		uint32_t x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
		const VkRect2D *s = p->hasScissor ? &p->scissor : R->hasScissor ? &R->scissor : NULL;
		if (s && !vkv_compat(VKV_COMPAT_IGNORE_SCISSOR)) {
			int32_t sx = VKV_MAX(s->offset.x, 0), sy = VKV_MAX(s->offset.y, 0);
			int32_t ex = VKV_MIN((int32_t)(s->offset.x + s->extent.width), (int32_t)w);
			int32_t ey = VKV_MIN((int32_t)(s->offset.y + s->extent.height), (int32_t)h);
			if (ex <= sx || ey <= sy) { sx = sy = 0; ex = ey = 1; } /* empty scissor: clip almost all */
			x0 = sx; y0 = sy; x1 = ex - 1; y1 = ey - 1;
			if ((sx | sy) & 31 || (((uint32_t)ex & 31) && (uint32_t)ex != w) || (((uint32_t)ey & 31) && (uint32_t)ey != h))
				VKV_UNSUPPORTED("pixel-exact scissor (GXM region clip works on 32x32 tiles)");
		}
		sceGxmSetRegionClip(R->ctx, SCE_GXM_REGION_CLIP_OUTSIDE, x0, y0, x1, y1);
	}
}

static void vkv_write_uniforms(void *buf, const VkvUniformMap *map, uint32_t n, const uint8_t *push, uint32_t pushHigh,
                               const char *shader) {
	for (uint32_t i = 0; i < n; i++) {
		const VkvUniformMap *u = &map[i];
		uint32_t bytes = u->elems > 1 ? u->elemStride * u->elems : u->comps * 4;
		if (u->offset + bytes > VKV_MAX_PUSH) continue;
		if (vkv_validate() && u->offset + bytes > pushHigh)
			vkv_fail("draw", "shader '%s' uniform '%s' reads push constant bytes [%u..%u) that were never pushed", shader,
			         sceGxmProgramParameterGetName(u->param), (unsigned)u->offset, (unsigned)(u->offset + bytes));
		const float *src = (const float *)(push + u->offset);
		if (u->elems <= 1 || u->elemStride == u->comps * 4) {
			sceGxmSetUniformDataF(buf, u->param, 0, u->comps * u->elems, src);
		} else {
			for (uint32_t e = 0; e < u->elems; e++)
				sceGxmSetUniformDataF(buf, u->param, e * u->comps, u->comps,
				                      (const float *)(push + u->offset + e * u->elemStride));
		}
	}
}

static void vkv_bind_resources(VkvReplay *R, const VkvPipeline *p) {
	SceGxmContext *ctx = R->ctx;
	uint32_t boundSamplers = 0, boundBuffers = 0;
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
				} else if (d->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
					if (slot >= SCE_GXM_MAX_TEXTURE_UNITS) continue;
					sceGxmSetFragmentTexture(ctx, slot, &d->tex);
					if (p->vSamplerMask & (1u << slot)) sceGxmSetVertexTexture(ctx, slot, &d->tex);
					boundSamplers |= 1u << slot;
				}
			}
		}
	}
	if (vkv_validate()) {
		uint32_t missingS = (p->fSamplerMask | p->vSamplerMask) & ~boundSamplers;
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
	ifmt = is32 ? SCE_GXM_INDEX_FORMAT_U32 : SCE_GXM_INDEX_FORMAT_U16;

	vkv_apply_pipeline(R, p, is32 ? 1 : 0);
	vkv_apply_viewport(R, p);
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
			vkv_write_uniforms(ub, p->vuni, p->vuniCount, R->push, R->pushHigh, p->vname);
		if (p->fDefaultSize && sceGxmReserveFragmentDefaultUniformBuffer(R->ctx, &ub) >= 0)
			vkv_write_uniforms(ub, p->funi, p->funiCount, R->push, R->pushHigh, p->fname);

		int r = sceGxmDraw(R->ctx, p->prim, ifmt, indices, c->count);
		if (r < 0) VKV_ERROR("sceGxmDraw failed 0x%08X (pipeline '%s')", (unsigned)r, p->vname);
		g_vkv.cur.drawCalls++;
		g_vkv.cur.indices += c->count;
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
	uint32_t bpp = img->fi->bpp;
	for (uint32_t i = 0; i < c->n; i++) {
		const VkBufferImageCopy *r = &c->r[i];
		if (r->imageSubresource.mipLevel || r->imageSubresource.baseArrayLayer) {
			VKV_UNSUPPORTED("copies to mip levels/layers above 0 (skipped)");
			continue;
		}
		uint32_t rowLen = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
		uint32_t rows = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
		(void)rows;
		uint32_t x = (uint32_t)r->imageOffset.x, y = (uint32_t)r->imageOffset.y;
		uint32_t w = VKV_MIN(r->imageExtent.width, img->width - VKV_MIN(x, img->width));
		uint32_t h = VKV_MIN(r->imageExtent.height, img->height - VKV_MIN(y, img->height));
		uint32_t srcPitch = rowLen * bpp;
		if (vkv_validate() && r->bufferOffset + (uint64_t)srcPitch * (h ? h - 1 : 0) + w * bpp > buf->size) {
			vkv_fail("vkCmdCopyBufferToImage", "region %u reads past the buffer", (unsigned)i);
			continue;
		}
		for (uint32_t row = 0; row < h; row++) {
			uint8_t *ip = img->ptr + (y + row) * img->rowPitch + x * bpp;
			uint8_t *bp = buf->ptr + r->bufferOffset + row * srcPitch;
			if (toImage) memcpy(ip, bp, w * bpp); else memcpy(bp, ip, w * bpp);
		}
		g_vkv.cur.copies++;
	}
}

static void vkv_exec_clear_color_image(VkvReplay *R, const VkvCmdClearColorImage *c) {
	if (!c->img->sync) vkv_image_ensure_sync(c->img);
	vkv_clear_color_rect(R, c->img, &c->c, 0, 0, c->img->width, c->img->height);
}

/* ------------------------------------------------------------------------ */
/* Replay                                                                   */
/* ------------------------------------------------------------------------ */
static const char *vkv_cmd_name(uint16_t t) {
	static const char *names[] = { "?", "BeginRenderPass", "EndRenderPass", "BindPipeline", "BindVertexBuffers",
		"BindIndexBuffer", "BindDescriptorSets", "PushConstants", "SetViewport", "SetScissor", "SetDepthBias",
		"SetStencil", "Draw", "DrawIndexed", "CopyBuffer", "CopyBufferToImage", "CopyImageToBuffer",
		"UpdateBuffer", "FillBuffer", "ClearAttachments", "ClearColorImage", "Barrier", "FixedFunctionDraw" };
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
		fence->notif.value = ++dev->notifCounter;
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
