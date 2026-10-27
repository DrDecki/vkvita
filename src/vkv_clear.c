/*
 * VkVita: clears: transfer fills outside scenes, quads inside them
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_replay.h"

/* --- Clears --- */
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
void vkv_clear_color_rect(VkvReplay *R, VkvImage *img, uint32_t level, uint32_t layer, const VkClearColorValue *c,
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


/* --- Clears inside a scene --- */
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

void vkv_exec_clear_attachments(VkvReplay *R, const VkvCmdClearAttach *c) {
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

void vkv_exec_clear_color_image(VkvReplay *R, const VkvCmdClearColorImage *c) {
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

