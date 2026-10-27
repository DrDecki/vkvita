/*
 * VkVita: images, image views and samplers
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

/* --- Images --- */
static bool vkv_is_pot(uint32_t v) { return v && !(v & (v - 1)); }

static uint32_t vkv_log2(uint32_t v) {
	uint32_t r = 0;
	while (v >>= 1) r++;
	return r;
}

/* Bits of x go to the even positions, bits of y to the odd ones. */
static uint32_t vkv_morton(uint32_t x, uint32_t y) {
	uint32_t r = 0;
	for (uint32_t b = 0; b < 16 && (x >> b || y >> b); b++)
		r |= ((x >> b) & 1u) << (2 * b) | ((y >> b) & 1u) << (2 * b + 1);
	return r;
}

/* GXM swizzled layout for power-of-two sizes: Morton order inside squares
 * of the shorter side, squares placed one after another along the longer one. */
uint32_t vkv_swizzle_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	if (w == h) return vkv_morton(x, y);
	if (w > h) {
		uint32_t k = vkv_log2(h);
		return ((x >> k) << (2 * k)) + vkv_morton(x & (h - 1), y);
	}
	uint32_t k = vkv_log2(w);
	return ((y >> k) << (2 * k)) + vkv_morton(x, y & (w - 1));
}

uint32_t vkv_image_level_width(const VkvImage *img, uint32_t level) { return VKV_MAX(img->width >> level, 1u); }
uint32_t vkv_image_level_height(const VkvImage *img, uint32_t level) { return VKV_MAX(img->height >> level, 1u); }

/* Width/height of a level in elements: texels, or 4x4 blocks for compressed formats. */
void vkv_image_level_elems(const VkvImage *img, uint32_t level, uint32_t *ew, uint32_t *eh) {
	uint32_t w = vkv_image_level_width(img, level), h = vkv_image_level_height(img, level);
	if (img->fi->block) { w = (w + 3) / 4; h = (h + 3) / 4; }
	*ew = w;
	*eh = h;
}

uint8_t *vkv_image_level_ptr(const VkvImage *img, uint32_t level, uint32_t layer) {
	return img->ptr + (size_t)layer * img->layerSize + img->levelOffset[level];
}

/* Address of one element (texel or block) of a level. */
uint8_t *vkv_image_elem(const VkvImage *img, uint32_t level, uint32_t layer, uint32_t ex, uint32_t ey) {
	uint8_t *base = vkv_image_level_ptr(img, level, layer);
	if (img->swizzled) {
		uint32_t ew, eh;
		vkv_image_level_elems(img, level, &ew, &eh);
		return base + (size_t)vkv_swizzle_index(ex, ey, ew, eh) * img->fi->bpp;
	}
	return base + (size_t)ey * img->levelPitch[level] + (size_t)ex * img->fi->bpp;
}

SceGxmMultisampleMode vkv_gxm_multisample(uint32_t samples) {
	return samples == 4 ? SCE_GXM_MULTISAMPLE_4X : samples == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
}

void vkv_image_setup_layout(VkvImage *img) {
	const VkvFormatInfo *fi = img->fi;
	img->levels = 1;
	img->layers = 1;
	if (fi->isDepth) {
		img->layers = VKV_MAX(img->arrayLayers, 1u);
		/* GXM keeps every sample of a depth buffer: 2x doubles the rows,
		 * 4x doubles rows and columns */
		img->alignedW = VKV_ALIGN(img->width, SCE_GXM_TILE_SIZEX) * (img->samples == 4 ? 2 : 1);
		img->alignedH = VKV_ALIGN(img->height, SCE_GXM_TILE_SIZEY) * (img->samples > 1 ? 2 : 1);
		img->rowPitch = img->alignedW * 4;
		img->levelPitch[0] = img->rowPitch;
		img->levelOffset[0] = 0;
		img->layerSize = img->alignedW * img->alignedH * 4;
		img->size = img->layerSize * img->layers;
		img->alignment = 4096;
		return;
	}
	/* every layer is stored; a cube keeps its six faces below */
	img->layers = VKV_MAX(img->arrayLayers, 1u);
	bool pot = vkv_is_pot(img->width) && vkv_is_pot(img->height);
	uint32_t maxLevels = vkv_log2(VKV_MAX(img->width, img->height)) + 1;
	img->levels = VKV_MIN(VKV_MAX(img->mipLevels, 1u), VKV_MIN(maxLevels, (uint32_t)VKV_MAX_LEVELS));
	if (img->levels > 1 && !pot) {
		VKV_UNSUPPORTED("mipmaps on non-power-of-two images (only level 0 is stored)");
		img->levels = 1;
	}
	if (img->cube) {
		img->layers = 6;
		if (img->width != img->height || !pot) {
			VKV_UNSUPPORTED("cube maps that are not square power-of-two sizes");
		}
		img->swizzled = true;
	}
	if (fi->block) {
		if (!pot) VKV_UNSUPPORTED("compressed textures that are not power-of-two sized");
		img->swizzled = true;
	}

	/* Level sizes follow GXM: levels are packed back to back, linear rows use an
	 * 8 texel stride, swizzled levels have no padding and compressed levels take
	 * whole 4x4 blocks. */
	uint32_t chainLevels = img->cube && img->levels > 1 ? maxLevels : img->levels;
	uint32_t total = 0, used = 0;
	for (uint32_t l = 0; l < chainLevels; l++) {
		uint32_t w = vkv_image_level_width(img, l), h = vkv_image_level_height(img, l), size, pitch;
		if (fi->block) {
			uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
			pitch = bw * fi->bpp;
			size = bw * bh * fi->bpp;
		} else if (img->swizzled) {
			pitch = w * fi->bpp;
			size = w * h * fi->bpp;
		} else {
			/* GXM linear textures and color surfaces use an 8 texel aligned stride */
			uint32_t stride = img->swapchain ? VKV_ALIGN(w, 64) : VKV_ALIGN(w, 8);
			pitch = stride * fi->bpp;
			size = pitch * h;
		}
		if (l < img->levels) {
			img->levelPitch[l] = pitch;
			img->levelOffset[l] = total;
			used = total + size;
		}
		total += size;
	}
	if (img->cube) {
		/* A cube with a mip chain gives every face room for the full chain down to
		 * 1x1, and large faces start on 2048 byte boundaries. Without a chain the
		 * faces follow each other directly. */
		uint32_t faceAlign = 4;
		if (img->levels > 1) {
			uint32_t bits = fi->block ? 0 : fi->bpp * 8, w = img->width, h = img->height;
			if ((w >= 32 && h >= 32 && (fi->block || bits <= 8)) || (w >= 16 && h >= 16 && (bits == 16 || bits == 32)) ||
			    (w >= 8 && h >= 8 && bits == 64))
				faceAlign = 2048;
		}
		img->layerSize = VKV_ALIGN(total, faceAlign);
	} else {
		/* layers of render targets start on page boundaries like the image itself */
		img->layerSize = img->layers > 1 && (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VKV_ALIGN(used, 4096)
		                                                                                   : VKV_ALIGN(used, 16);
	}
	img->alignedW = fi->block ? img->width : img->levelPitch[0] / fi->bpp;
	img->alignedH = img->height;
	img->rowPitch = img->levelPitch[0];
	img->size = img->layerSize * img->layers;
	img->alignment = (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? 4096 : 256;
}

VkResult vkv_image_ensure_sync(VkvImage *img) {
	if (img->sync) return VK_SUCCESS;
	if (sceGxmSyncObjectCreate(&img->sync) < 0) {
		img->sync = NULL;
		VKV_ERROR("sceGxmSyncObjectCreate failed");
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *ci,
                                             const VkAllocationCallbacks *alloc, VkImage *pImage) {
	VKV_TRACE("vkCreateImage");
	(void)device; (void)alloc;
	const VkvFormatInfo *fi = vkv_format(ci->format);
	if (!fi) {
		VKV_ERROR("vkCreateImage: format %d is not supported", (int)ci->format);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (ci->imageType != VK_IMAGE_TYPE_2D) {
		VKV_UNSUPPORTED("1D/3D images");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	uint32_t samples = (uint32_t)ci->samples;
	if (samples != 1 && samples != 2 && samples != 4) {
		VKV_ERROR("vkCreateImage: %u samples, GXM can do 1, 2 or 4", (unsigned)samples);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (samples > 1 && (ci->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)))
		VKV_UNSUPPORTED("reading the samples of a multisampled image (it holds the resolved pixels)");
	if (samples > 1 && (ci->mipLevels > 1 || ci->arrayLayers > 1)) {
		VKV_ERROR("vkCreateImage: multisampled images have one level and one layer");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	bool cube = (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && ci->arrayLayers == 6;
	if (ci->arrayLayers > 256) {
		VKV_ERROR("vkCreateImage: %u array layers, at most 256", (unsigned)ci->arrayLayers);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (fi->block && (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
		VKV_ERROR("vkCreateImage: compressed formats cannot be render targets");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (cube && (ci->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
		VKV_UNSUPPORTED("rendering into cube map faces");
	if (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT && ci->arrayLayers > 6)
		VKV_UNSUPPORTED("cube map arrays (only the first cube is stored)");
	if (VKV_CHECK(ci->extent.width && ci->extent.height && ci->extent.width <= 4096 && ci->extent.height <= 4096,
	              "bad image extent %ux%u", (unsigned)ci->extent.width, (unsigned)ci->extent.height))
		return VK_ERROR_INITIALIZATION_FAILED;

	VkvImage *img = calloc(1, sizeof(*img));
	if (!img) return VK_ERROR_OUT_OF_HOST_MEMORY;
	img->obj.magic = VKV_MAGIC_IMAGE;
	img->format = ci->format;
	img->fi = fi;
	img->width = ci->extent.width;
	img->height = ci->extent.height;
	img->mipLevels = ci->mipLevels;
	img->arrayLayers = ci->arrayLayers;
	img->samples = samples;
	img->usage = ci->usage;
	img->tiling = ci->tiling;
	img->cube = cube;
	vkv_image_setup_layout(img);
	*pImage = VKV_H(VkImage, img);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvImage *img = VKV_ND(VkvImage, image);
	if (!img) return;
	if (VKV_CHECK(!img->swapchain, "swapchain images must not be destroyed by the app")) return;
	if (img->sync) sceGxmSyncObjectDestroy(img->sync);
	img->obj.magic = VKV_MAGIC_DEAD;
	free(img);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *req) {
	(void)device;
	VkvImage *img = VKV_ND(VkvImage, image);
	req->size = img->size;
	req->alignment = img->alignment;
	req->memoryTypeBits = (1u << VKV_MEMTYPE_COUNT) - 1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                 VkDeviceSize offset) {
	(void)device;
	VkvImage *img = VKV_ND(VkvImage, image);
	VkvMemory *m = VKV_ND(VkvMemory, memory);
	if (VKV_CHECK(VKV_OBJ_OK(img, VKV_MAGIC_IMAGE) && VKV_OBJ_OK(m, VKV_MAGIC_MEMORY), "invalid handle"))
		return VK_ERROR_UNKNOWN;
	if (VKV_CHECK(offset + img->size <= m->size, "image does not fit its memory")) return VK_ERROR_UNKNOWN;
	if (VKV_CHECK((((uintptr_t)m->ptr + offset) & (img->alignment - 1)) == 0, "image memory is misaligned"))
		return VK_ERROR_UNKNOWN;
	img->mem = m;
	img->ptr = m->ptr + offset;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device, VkImage image,
                                                       const VkImageSubresource *sub, VkSubresourceLayout *l) {
	(void)device;
	VkvImage *img = VKV_ND(VkvImage, image);
	uint32_t level = VKV_MIN(sub->mipLevel, img->levels - 1);
	uint32_t layer = VKV_MIN(sub->arrayLayer, img->layers - 1);
	if (img->swizzled) VKV_UNSUPPORTED("mapping swizzled (cube or compressed) images directly");
	l->offset = (size_t)layer * img->layerSize + img->levelOffset[level];
	l->rowPitch = img->levelPitch[level];
	l->size = level + 1 < img->levels ? img->levelOffset[level + 1] - img->levelOffset[level]
	                                  : img->layerSize - img->levelOffset[level];
	l->arrayPitch = img->layerSize;
	l->depthPitch = l->size;
}

/* --- Image views and samplers --- */
/* GXM bakes component swizzles into the texture format. Each entry is the
 * source of r, g, b, a: 0-3 are bytes from the lowest up, 4 is zero, 5 is one. */
enum { SW_ZERO = 4, SW_ONE = 5 };
static const uint8_t k_swizzle4[8][4] = {
	{ 0, 1, 2, 3 },           /* ABGR */
	{ 2, 1, 0, 3 },           /* ARGB */
	{ 3, 2, 1, 0 },           /* RGBA */
	{ 1, 2, 3, 0 },           /* BGRA */
	{ 0, 1, 2, SW_ONE },      /* 1BGR */
	{ 2, 1, 0, SW_ONE },      /* 1RGB */
	{ 3, 2, 1, SW_ONE },      /* RGB1 */
	{ 1, 2, 3, SW_ONE },      /* BGR1 */
};
static const uint8_t k_swizzle1[8][4] = {
	{ 0, SW_ZERO, SW_ZERO, SW_ONE }, /* R, sampled the Vulkan way */
	{ 0, SW_ZERO, SW_ZERO, SW_ZERO }, /* 000R */
	{ 0, SW_ONE, SW_ONE, SW_ONE },   /* 111R */
	{ 0, 0, 0, 0 },                  /* RRRR */
	{ 0, 0, 0, SW_ZERO },            /* 0RRR */
	{ 0, 0, 0, SW_ONE },             /* 1RRR */
	{ SW_ZERO, SW_ZERO, SW_ZERO, 0 }, /* R000 */
	{ SW_ONE, SW_ONE, SW_ONE, 0 },   /* R111 */
};

static SceGxmTextureFormat vkv_view_swizzle(const VkvFormatInfo *fi, const VkComponentMapping *cm, bool *ok) {
	*ok = true;
	const VkComponentSwizzle want[4] = { cm->r, cm->g, cm->b, cm->a };
	bool identity = true;
	for (int k = 0; k < 4; k++)
		if (want[k] != VK_COMPONENT_SWIZZLE_IDENTITY && want[k] != (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + k))
			identity = false;
	if (identity || !vkv_format_sampleable(fi)) return fi->tex;
	uint32_t base = (uint32_t)fi->tex & 0x9F000000u, swz = ((uint32_t)fi->tex >> 12) & 7;
	const uint8_t (*table)[4];
	switch (base) {
	case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8: case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
	case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5: case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16:
	case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1: case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2: case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3:
		table = k_swizzle4;
		break;
	case SCE_GXM_TEXTURE_BASE_FORMAT_U8: case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
		table = k_swizzle1;
		break;
	default:
		*ok = false;
		return fi->tex;
	}
	/* where each Vulkan channel lives in memory, then where each output must come from */
	const uint8_t *id = table[swz];
	uint8_t need[4];
	for (int k = 0; k < 4; k++) {
		VkComponentSwizzle c = want[k] == VK_COMPONENT_SWIZZLE_IDENTITY ? (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + k)
		                                                               : want[k];
		if (c == VK_COMPONENT_SWIZZLE_ZERO) need[k] = SW_ZERO;
		else if (c == VK_COMPONENT_SWIZZLE_ONE) need[k] = SW_ONE;
		else need[k] = id[c - VK_COMPONENT_SWIZZLE_R];
	}
	for (uint32_t s = 0; s < 8; s++)
		if (!memcmp(table[s], need, 4)) return (SceGxmTextureFormat)(base | (s << 12));
	*ok = false;
	return fi->tex;
}

bool vkv_view_build_texture(VkvImageView *v) {
	const VkvFormatInfo *fi = vkv_format(v->format);
	VkvImage *img = v->image;
	v->texValid = false;
	if (!vkv_format_sampleable(fi) || !img->ptr) return false;
	if (fi->bpp != img->fi->bpp || fi->block != img->fi->block) {
		VKV_WARN("image view format %d has a different texel size than its image", (int)v->format);
		return false;
	}
	uint32_t w = vkv_image_level_width(img, v->baseLevel), h = vkv_image_level_height(img, v->baseLevel);
	void *data = vkv_image_level_ptr(img, v->baseLevel, v->cube ? 0 : v->baseLayer);
	int r;
	/* mipCount 0 tells GXM the cube has no mip chain, so the faces follow each other
	 * directly; any other count makes every face span a full chain, which is how
	 * mipmapped cubes are stored */
	if (v->cube && img->levels > 1 && v->baseLevel) {
		VKV_UNSUPPORTED("cube views that skip the first mip levels of a mipmapped cube");
		return false;
	}
	SceGxmTextureFormat tf = v->texFormat ? v->texFormat : fi->tex;
	if (v->cube) r = sceGxmTextureInitCube(&v->tex, data, tf, w, h, img->levels > 1 ? v->levelCount : 0);
	else if (img->swizzled) r = sceGxmTextureInitSwizzled(&v->tex, data, tf, w, h, v->levelCount);
	else r = sceGxmTextureInitLinear(&v->tex, data, tf, w, h, v->levelCount);
	if (r < 0) {
		VKV_ERROR("texture init (%ux%u fmt %d, %u levels%s) failed: 0x%08X", (unsigned)w, (unsigned)h, (int)v->format,
		          (unsigned)v->levelCount, v->cube ? ", cube" : "", (unsigned)r);
		return false;
	}
	if (fi->srgb) sceGxmTextureSetGammaMode(&v->tex, SCE_GXM_TEXTURE_GAMMA_BGR);
	sceGxmTextureSetMinFilter(&v->tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
	sceGxmTextureSetMagFilter(&v->tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
	v->texValid = true;
	return true;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc, VkImageView *pView) {
	VKV_TRACE("vkCreateImageView");
	(void)device; (void)alloc;
	VkvImage *img = VKV_ND(VkvImage, ci->image);
	if (VKV_CHECK(VKV_OBJ_OK(img, VKV_MAGIC_IMAGE), "invalid image")) return VK_ERROR_UNKNOWN;
	if (VKV_CHECK(img->ptr != NULL, "image has no memory bound")) return VK_ERROR_UNKNOWN;
	VkvImageView *v = calloc(1, sizeof(*v));
	if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
	v->obj.magic = VKV_MAGIC_VIEW;
	v->image = img;
	v->format = ci->format;
	v->baseLevel = VKV_MIN(ci->subresourceRange.baseMipLevel, img->levels - 1);
	uint32_t count = ci->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS
	                     ? img->levels - v->baseLevel : ci->subresourceRange.levelCount;
	v->levelCount = VKV_MAX(VKV_MIN(count, img->levels - v->baseLevel), 1u);
	v->baseLayer = VKV_MIN(ci->subresourceRange.baseArrayLayer, img->layers - 1);
	v->cube = img->cube && (ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE || ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
	uint32_t layerCount = ci->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS
	                          ? img->layers - v->baseLayer : ci->subresourceRange.layerCount;
	if (ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE && !img->cube)
		VKV_UNSUPPORTED("cube views of images created without CUBE_COMPATIBLE and 6 layers");
	else if (ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && layerCount > 6)
		VKV_UNSUPPORTED("cube array views (the first cube is used)");
	else if ((ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY || ci->viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY) && layerCount > 1)
		VKV_UNSUPPORTED("array views with several layers (GXM has no array textures, the first layer is used)");
	else if (ci->viewType == VK_IMAGE_VIEW_TYPE_1D || ci->viewType == VK_IMAGE_VIEW_TYPE_3D)
		VKV_UNSUPPORTED("1D and 3D views (treated as 2D)");
	bool swOk;
	v->texFormat = vkv_view_swizzle(vkv_format(ci->format) ? vkv_format(ci->format) : img->fi, &ci->components, &swOk);
	if (!swOk) VKV_UNSUPPORTED("this component swizzle (GXM has a fixed set per format, ignored)");
	if (!img->fi->isDepth) vkv_view_build_texture(v);
	*pView = VKV_H(VkImageView, v);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvImageView *v = VKV_ND(VkvImageView, view);
	if (!v) return;
	v->obj.magic = VKV_MAGIC_DEAD;
	free(v);
}

static SceGxmTextureAddrMode vkv_addr_mode(VkSamplerAddressMode m) {
	switch (m) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:               return SCE_GXM_TEXTURE_ADDR_REPEAT;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:      return SCE_GXM_TEXTURE_ADDR_MIRROR;
	case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		VKV_UNSUPPORTED("CLAMP_TO_BORDER (using CLAMP_TO_EDGE)");
		return SCE_GXM_TEXTURE_ADDR_CLAMP;
	default:                                           return SCE_GXM_TEXTURE_ADDR_CLAMP;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *ci,
                                               const VkAllocationCallbacks *alloc, VkSampler *pSampler) {
	(void)device; (void)alloc;
	VkvSampler *s = calloc(1, sizeof(*s));
	if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
	s->obj.magic = VKV_MAGIC_SAMPLER;
	s->minFilter = ci->minFilter == VK_FILTER_NEAREST ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
	s->magFilter = ci->magFilter == VK_FILTER_NEAREST ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
	s->mipFilter = ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
	                                                               : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
	s->addrU = vkv_addr_mode(ci->addressModeU);
	s->addrV = vkv_addr_mode(ci->addressModeV);
	/* maxLod limits how many levels are sampled; 0.25 is the usual "no mipmaps" value */
	float maxLod = ci->maxLod < 0.0f ? 0.0f : ci->maxLod;
	s->maxLevels = maxLod >= (float)VKV_MAX_LEVELS ? VKV_MAX_LEVELS : (uint32_t)maxLod + 1;
	if (ci->anisotropyEnable) VKV_UNSUPPORTED("anisotropic filtering");
	if (ci->compareEnable) VKV_UNSUPPORTED("depth compare samplers");
	if (ci->minLod > 0.0f) VKV_UNSUPPORTED("sampler minLod");
	/* GXM stores the bias in eighths around 31 (-3.875 to +4) */
	float bias = ci->mipLodBias * 8.0f + 31.0f;
	s->lodBias = bias <= 0.0f ? 0 : bias >= 63.0f ? 63 : (uint32_t)(bias + 0.5f);
	if (ci->mipLodBias < -3.875f || ci->mipLodBias > 4.0f) VKV_UNSUPPORTED("LOD bias beyond -3.875..4 (clamped)");
	*pSampler = VKV_H(VkSampler, s);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvSampler *s = VKV_ND(VkvSampler, sampler);
	if (!s) return;
	s->obj.magic = VKV_MAGIC_DEAD;
	free(s);
}

void vkv_texture_apply_sampler(SceGxmTexture *tex, const VkvSampler *s, uint32_t levels) {
	if (!s) return;
	sceGxmTextureSetMinFilter(tex, s->minFilter);
	sceGxmTextureSetMagFilter(tex, s->magFilter);
	sceGxmTextureSetUAddrMode(tex, s->addrU);
	sceGxmTextureSetVAddrMode(tex, s->addrV);
	if (s->lodBias != 31) sceGxmTextureSetLodBias(tex, s->lodBias);
	if (levels > 1) {
		sceGxmTextureSetMipFilter(tex, s->mipFilter);
		if (s->maxLevels < levels) sceGxmTextureSetMipmapCount(tex, s->maxLevels);
	}
}

