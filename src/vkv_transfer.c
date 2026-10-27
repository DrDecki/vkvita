/*
 * VkVita: buffer and image copies, blits
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_replay.h"

/* --- Transfers (executed by the CPU at submit time) --- */
void vkv_transfer_sync(VkvReplay *R) {
	if (R->dev->gpuBusy && !vkv_compat(VKV_COMPAT_FAST_TRANSFERS)) vkv_device_wait_gpu(R->dev);
}

void vkv_exec_copy_buf_img(VkvReplay *R, const VkvCmdCopyBufImg *c, bool toImage) {
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

/* Linear image copies on the transfer unit. A transfer can wait on one sync
 * object, so it takes the source's if it is a render target, else the
 * destination's. Returns false when the CPU has to copy. */
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

void vkv_exec_copy_image(VkvReplay *R, const VkvCmdCopyImage *c) {
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

void vkv_exec_blit_image(VkvReplay *R, const VkvCmdBlitImage *c) {
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

