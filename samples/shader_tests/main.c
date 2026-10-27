/*
 * VkVita sample: shader_tests
 *
 * Twelve tiles, each one checking a shader feature inside the fragment
 * shader: green when the value it read is the expected one, red when not.
 *
 *   first row   struct members in a uniform block: a vec4 and a float in an
 *               array of structs, int members, a nested struct with an
 *               array inside, a mat4 inside a struct
 *   second row  a sampler array with a constant index, the same array with
 *               an index from a push constant, an array of separate images
 *               with one sampler, and the plain members after the structs
 *   third row   read-only storage buffers: a runtime array of mat4 that is
 *               only partly filled, a tightly packed float array, an array
 *               of structs after a header, and a float array in push
 *               constants (std430, 4 bytes apart)
 *   fourth row  images: a layer of an array image, a layer that a render
 *               pass cleared, an R8 texture swizzled to (1, 1, 1, R) and an
 *               RGBA texture swizzled to BGRA
 *   fifth row   a sampler with a LOD bias of +1, drawn a second time in red
 *               with a scissor that ends 2 pixels left of it: with a pixel
 *               exact scissor no red shows, on 32x32 tiles the left edge
 *               turns red; a red texture with two regions put in by
 *               vkCmdCopyImage on the transfer unit, one from a render
 *               target and one from a texture; an occlusion query result
 *               that vkCmdCopyQueryPoolResults wrote into a storage buffer
 *
 * Expected picture: nineteen green tiles.
 *
 * Regenerate the shader headers after editing shaders/:
 *   glslangValidator -V --vn k_tile_vert_spv shaders/tile.vert -o tile_vert_spv.h
 *   glslangValidator -V --vn k_tile_frag_spv shaders/tile.frag -o tile_frag_spv.h
 *
 * Controls: START quits.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>

#include "vkv_sample.h"
#include "tile_vert_spv.h"
#include "tile_frag_spv.h"

#define TEX_SIZE 8

typedef struct {
	VkImage        image;
	VkDeviceMemory memory;
	VkImageView    view;
} Texture;

/* A small linear texture of one color, written straight into mapped memory. */
static void make_texture(Sample *s, const uint8_t rgba[4], Texture *t) {
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM;
	ii.extent = (VkExtent3D){ TEX_SIZE, TEX_SIZE, 1 };
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_LINEAR;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	VK_CHECK(vkCreateImage(s->device, &ii, NULL, &t->image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s->device, t->image, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	VK_CHECK(vkAllocateMemory(s->device, &ai, NULL, &t->memory));
	VK_CHECK(vkBindImageMemory(s->device, t->image, t->memory, 0));
	VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(s->device, t->image, &sub, &layout);
	uint8_t *p;
	VK_CHECK(vkMapMemory(s->device, t->memory, 0, VK_WHOLE_SIZE, 0, (void **)&p));
	for (uint32_t y = 0; y < TEX_SIZE; y++)
		for (uint32_t x = 0; x < TEX_SIZE; x++) memcpy(p + layout.offset + y * layout.rowPitch + x * 4, rgba, 4);
	vkUnmapMemory(s->device, t->memory);
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vci.image = t->image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ii.format;
	vci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView(s->device, &vci, NULL, &t->view));
}

/* A linear image in mapped memory, each level and layer filled with one color
 * (or one byte for 1 byte formats). */
static void make_image(Sample *s, VkFormat format, uint32_t size, uint32_t levels, uint32_t layers, uint32_t bpp,
                       const uint8_t (*colors)[4], VkImage *image, VkDeviceMemory *memory) {
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = (VkExtent3D){ size, size, 1 };
	ii.mipLevels = levels;
	ii.arrayLayers = layers;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_LINEAR;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	VK_CHECK(vkCreateImage(s->device, &ii, NULL, image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s->device, *image, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	VK_CHECK(vkAllocateMemory(s->device, &ai, NULL, memory));
	VK_CHECK(vkBindImageMemory(s->device, *image, *memory, 0));
	uint8_t *p;
	VK_CHECK(vkMapMemory(s->device, *memory, 0, VK_WHOLE_SIZE, 0, (void **)&p));
	for (uint32_t layer = 0; layer < layers; layer++)
		for (uint32_t level = 0; level < levels; level++) {
			VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer };
			VkSubresourceLayout l;
			vkGetImageSubresourceLayout(s->device, *image, &sub, &l);
			uint32_t w = size >> level;
			const uint8_t *c = colors[layer * levels + level];
			for (uint32_t y = 0; y < w; y++)
				for (uint32_t x = 0; x < w; x++) memcpy(p + l.offset + y * l.rowPitch + x * bpp, c, bpp);
		}
	vkUnmapMemory(s->device, *memory);
}

static VkImageView make_view(Sample *s, VkImage image, VkFormat format, uint32_t levels, uint32_t layer,
                             VkComponentMapping components) {
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format;
	vci.components = components;
	vci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, layer, 1 };
	VkImageView v;
	VK_CHECK(vkCreateImageView(s->device, &vci, NULL, &v));
	return v;
}

static void destroy_texture(Sample *s, Texture *t) {
	vkDestroyImageView(s->device, t->view, NULL);
	vkDestroyImage(s->device, t->image, NULL);
	vkFreeMemory(s->device, t->memory, NULL);
}

static VkShaderModule make_module(Sample *s, const uint32_t *code, size_t size) {
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = size;
	ci.pCode = code;
	VkShaderModule m;
	VK_CHECK(vkCreateShaderModule(s->device, &ci, NULL, &m));
	return m;
}

static void put_f(uint8_t *ubo, uint32_t off, const float *v, uint32_t n) { memcpy(ubo + off, v, n * 4); }
static void put_i(uint8_t *ubo, uint32_t off, int32_t v) { memcpy(ubo + off, &v, 4); }

int main(void) {
	sample_configure("shader_tests");
	Sample s;
	sample_init(&s, "shader_tests", false);

	/* --- the uniform block, laid out by hand with std140 rules -------------- */
	/* lights[i] at 48 * i: pos 0, color 16, intensity 32, kind 36
	 * o at 96: inner.a 96, inner.b[k] 112 + 16 * k, m 144
	 * amb 208, flag 220 */
	VkBuffer ubuf;
	VkDeviceMemory umem;
	uint8_t *ubo;
	sample_create_buffer(&s, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &ubuf, &umem,
	                     (void **)&ubo);
	memset(ubo, 0, 256);
	const float pos0[4] = { 9, 9, 9, 1 }, col0[4] = { 1, 0, 0, 1 }, pos1[4] = { 5, 6, 7, 1 };
	const float col1[4] = { 0.25f, 0.5f, 0.75f, 1.0f }, i0 = 1.5f, i1 = 0.5f;
	put_f(ubo, 0, pos0, 4);
	put_f(ubo, 16, col0, 4);
	put_f(ubo, 32, &i0, 1);
	put_i(ubo, 36, 3);
	put_f(ubo, 48, pos1, 4);
	put_f(ubo, 64, col1, 4);
	put_f(ubo, 80, &i1, 1);
	put_i(ubo, 84, 7);
	const float a[2] = { 1.0f, 2.0f }, b0 = 0.5f, b1 = 2.5f;
	put_f(ubo, 96, a, 2);
	put_f(ubo, 112, &b0, 1);
	put_f(ubo, 128, &b1, 1);
	float m[16];
	mat4_translate(m, 10.0f, 20.0f, 30.0f);
	put_f(ubo, 144, m, 16);
	const float amb[3] = { 0.1f, 0.2f, 0.3f };
	put_f(ubo, 208, amb, 3);
	put_i(ubo, 220, 1);

	/* --- storage buffers (std430) ------------------------------------------- */
	VkBuffer sbuf[3];
	VkDeviceMemory smem[3];
	float *sp[3];
	const VkDeviceSize ssize[3] = { 2 * 64, 8 * 4, 16 + 3 * 32 };
	for (int i = 0; i < 3; i++) {
		sample_create_buffer(&s, ssize[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
		                     &sbuf[i], &smem[i], (void **)&sp[i]);
		memset(sp[i], 0, ssize[i]);
	}
	mat4_identity(sp[0]);
	mat4_translate(sp[0] + 16, 4.0f, 5.0f, 6.0f); /* bones[1]; the shader array has 64, the buffer 2 */
	for (int k = 0; k < 8; k++) sp[1][k] = (float)k + 0.5f;
	const float header[4] = { 1, 2, 3, 4 };
	memcpy(sp[2], header, sizeof(header));
	for (int k = 0; k < 3; k++) sp[2][4 + k * 8 + 4] = 0.25f * (float)(k + 1); /* items[k].b at 16 + 32k + 16 */

	/* --- textures: three for the sampler array, two for the image array ---- */
	static const uint8_t colors[5][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
	                                      { 255, 0, 255, 255 }, { 255, 255, 0, 255 } };
	Texture tex[5];
	for (int i = 0; i < 5; i++) make_texture(&s, colors[i], &tex[i]);
	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler sampler;
	VK_CHECK(vkCreateSampler(s.device, &sci, NULL, &sampler));
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.maxLod = 8.0f;
	sci.mipLodBias = 1.0f;
	VkSampler biasSampler;
	VK_CHECK(vkCreateSampler(s.device, &sci, NULL, &biasSampler));

	/* --- images for the fourth and fifth row ------------------------------------ */
	static const VkComponentMapping ident = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
	static const uint8_t layerColors[3][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 } };
	VkImage arrayImg, alphaImg, redImg, mipImg;
	VkDeviceMemory arrayMem, alphaMem, redMem, mipMem;
	make_image(&s, VK_FORMAT_R8G8B8A8_UNORM, TEX_SIZE, 1, 3, 4, layerColors, &arrayImg, &arrayMem);
	VkImageView layerView = make_view(&s, arrayImg, VK_FORMAT_R8G8B8A8_UNORM, 1, 2, ident);
	static const uint8_t half[1][4] = { { 128, 0, 0, 0 } };
	make_image(&s, VK_FORMAT_R8_UNORM, TEX_SIZE, 1, 1, 1, half, &alphaImg, &alphaMem);
	VkImageView alphaView = make_view(&s, alphaImg, VK_FORMAT_R8_UNORM, 1, 0,
	                                  (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE,
	                                                        VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R });
	static const uint8_t red[1][4] = { { 255, 0, 0, 255 } };
	make_image(&s, VK_FORMAT_R8G8B8A8_UNORM, TEX_SIZE, 1, 1, 4, red, &redImg, &redMem);
	VkImageView swappedView = make_view(&s, redImg, VK_FORMAT_R8G8B8A8_UNORM, 1, 0,
	                                    (VkComponentMapping){ VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
	                                                          VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A });
	static const uint8_t mipColors[4][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
	                                         { 255, 255, 255, 255 } };
	make_image(&s, VK_FORMAT_R8G8B8A8_UNORM, TEX_SIZE, 4, 1, 4, mipColors, &mipImg, &mipMem);
	VkImageView mipView = make_view(&s, mipImg, VK_FORMAT_R8G8B8A8_UNORM, 4, 0, ident);

	/* layer 1 of a two layer render target, cleared to yellow by a render pass */
	VkImageCreateInfo rii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	rii.imageType = VK_IMAGE_TYPE_2D;
	rii.format = VK_FORMAT_R8G8B8A8_UNORM;
	rii.extent = (VkExtent3D){ 64, 64, 1 };
	rii.mipLevels = 1;
	rii.arrayLayers = 2;
	rii.samples = VK_SAMPLE_COUNT_1_BIT;
	rii.tiling = VK_IMAGE_TILING_OPTIMAL;
	rii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkImage targetImg;
	VK_CHECK(vkCreateImage(s.device, &rii, NULL, &targetImg));
	VkMemoryRequirements treq;
	vkGetImageMemoryRequirements(s.device, targetImg, &treq);
	VkMemoryAllocateInfo tai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	tai.allocationSize = treq.size;
	tai.memoryTypeIndex = sample_memory_type(&s, treq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory targetMem;
	VK_CHECK(vkAllocateMemory(s.device, &tai, NULL, &targetMem));
	VK_CHECK(vkBindImageMemory(s.device, targetImg, targetMem, 0));
	VkImageView targetLayer0 = make_view(&s, targetImg, VK_FORMAT_R8G8B8A8_UNORM, 1, 0, ident);
	VkImageView targetLayer1 = make_view(&s, targetImg, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, ident);
	VkAttachmentDescription tatt = { 0 };
	tatt.format = VK_FORMAT_R8G8B8A8_UNORM;
	tatt.samples = VK_SAMPLE_COUNT_1_BIT;
	tatt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	tatt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	tatt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	tatt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	tatt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	tatt.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkAttachmentReference tref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription tsub = { 0 };
	tsub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	tsub.colorAttachmentCount = 1;
	tsub.pColorAttachments = &tref;
	VkRenderPassCreateInfo trp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	trp.attachmentCount = 1;
	trp.pAttachments = &tatt;
	trp.subpassCount = 1;
	trp.pSubpasses = &tsub;
	VkRenderPass targetRp;
	VK_CHECK(vkCreateRenderPass(s.device, &trp, NULL, &targetRp));
	VkFramebuffer targetFb[2];
	VkImageView targetViews[2] = { targetLayer0, targetLayer1 };
	for (int i = 0; i < 2; i++) {
		VkFramebufferCreateInfo tfci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		tfci.renderPass = targetRp;
		tfci.attachmentCount = 1;
		tfci.pAttachments = &targetViews[i];
		tfci.width = tfci.height = 64;
		tfci.layers = 1;
		VK_CHECK(vkCreateFramebuffer(s.device, &tfci, NULL, &targetFb[i]));
	}
	{
		/* layer 0 red, layer 1 yellow: the tile reads layer 1 */
		VkCommandBuffer once = sample_begin_once(&s);
		const VkClearValue clears[2] = { { .color = { { 1, 0, 0, 1 } } }, { .color = { { 1, 1, 0, 1 } } } };
		for (int i = 0; i < 2; i++) {
			VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
			rbi.renderPass = targetRp;
			rbi.framebuffer = targetFb[i];
			rbi.renderArea.extent = (VkExtent2D){ 64, 64 };
			rbi.clearValueCount = 1;
			rbi.pClearValues = &clears[i];
			vkCmdBeginRenderPass(once, &rbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(once);
		}
		sample_end_once(&s, once);
	}

	/* a 16x16 red texture; texels 4 to 11 are copied from the yellow layer of the
	 * render target, 12 to 15 from the green 8x8 texture. Both copies run on the
	 * transfer unit, the first one after the clear above has finished. */
	VkImage copyImg;
	VkDeviceMemory copyMem;
	make_image(&s, VK_FORMAT_R8G8B8A8_UNORM, 16, 1, 1, 4, red, &copyImg, &copyMem);
	VkImageView copyView = make_view(&s, copyImg, VK_FORMAT_R8G8B8A8_UNORM, 1, 0, ident);
	{
		VkCommandBuffer once = sample_begin_once(&s);
		VkImageCopy fromTarget = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1 }, { 4, 4, 0 },
		                           { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 4, 4, 0 }, { 8, 8, 1 } };
		vkCmdCopyImage(once, targetImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copyImg,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &fromTarget);
		VkImageCopy fromTexture = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 2, 2, 0 },
		                            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 12, 12, 0 }, { 4, 4, 1 } };
		vkCmdCopyImage(once, tex[1].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copyImg,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &fromTexture);
		sample_end_once(&s, once);
	}

	/* the occlusion query result lands here, filled further down */
	VkBuffer qbuf;
	VkDeviceMemory qmem;
	uint32_t *qp;
	sample_create_buffer(&s, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &qbuf, &qmem, (void **)&qp);
	memset(qp, 0, 16);

	/* --- descriptors ---------------------------------------------------------- */
	VkDescriptorSetLayoutBinding lb[14] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	VkDescriptorSetLayoutCreateInfo dlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dlci.bindingCount = 14;
	dlci.pBindings = lb;
	VkDescriptorSetLayout setLayout;
	VK_CHECK(vkCreateDescriptorSetLayout(s.device, &dlci, NULL, &setLayout));
	VkDescriptorPoolSize ps[5] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 },
	};
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = 1;
	dpci.poolSizeCount = 5;
	dpci.pPoolSizes = ps;
	VkDescriptorPool dpool;
	VK_CHECK(vkCreateDescriptorPool(s.device, &dpci, NULL, &dpool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsai.descriptorPool = dpool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout;
	VkDescriptorSet set;
	VK_CHECK(vkAllocateDescriptorSets(s.device, &dsai, &set));

	VkDescriptorBufferInfo dbi = { ubuf, 0, 256 };
	VkDescriptorBufferInfo sbi[3];
	for (int i = 0; i < 3; i++) sbi[i] = (VkDescriptorBufferInfo){ sbuf[i], 0, VK_WHOLE_SIZE };
	VkDescriptorImageInfo combined[3], images[2], samp = { sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
	for (int i = 0; i < 3; i++)
		combined[i] = (VkDescriptorImageInfo){ sampler, tex[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	for (int i = 0; i < 2; i++)
		images[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, tex[3 + i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo extra[6] = {
		{ sampler, layerView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ sampler, targetLayer1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ sampler, alphaView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ sampler, swappedView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ biasSampler, mipView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ sampler, copyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkDescriptorBufferInfo qbi = { qbuf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr[14];
	memset(wr, 0, sizeof(wr));
	for (int i = 0; i < 14; i++) {
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = set;
		wr[i].dstBinding = (uint32_t)i;
		wr[i].descriptorType = lb[i].descriptorType;
		wr[i].descriptorCount = lb[i].descriptorCount;
	}
	wr[0].pBufferInfo = &dbi;
	wr[1].pImageInfo = combined;
	wr[2].pImageInfo = images;
	wr[3].pImageInfo = &samp;
	for (int i = 0; i < 3; i++) wr[4 + i].pBufferInfo = &sbi[i];
	for (int i = 0; i < 6; i++) wr[7 + i].pImageInfo = &extra[i];
	wr[13].pBufferInfo = &qbi;
	vkUpdateDescriptorSets(s.device, 14, wr, 0, NULL);

	/* --- pipeline: the tile comes from push constants, no vertex buffer ----- */
	VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 28 };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(s.device, &plci, NULL, &layout));
	VkShaderModule vs = make_module(&s, k_tile_vert_spv, sizeof(k_tile_vert_spv));
	VkShaderModule fs = make_module(&s, k_tile_frag_spv, sizeof(k_tile_frag_spv));
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vps.viewportCount = vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo msi = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	msi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = { 0 };
	ba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cbs.attachmentCount = 1;
	cbs.pAttachments = &ba;
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dyi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dyi.dynamicStateCount = 2;
	dyi.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vps;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &msi;
	gp.pColorBlendState = &cbs;
	gp.pDynamicState = &dyi;
	gp.layout = layout;
	gp.renderPass = s.renderPass;
	VkPipeline pipe, targetPipe;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
	gp.renderPass = targetRp;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &targetPipe));
	vkDestroyShaderModule(s.device, vs, NULL);
	vkDestroyShaderModule(s.device, fs, NULL);

	/* an occlusion query around tile 0 drawn into layer 0 of the render target
	 * (64x64, so about 13x10 pixels), copied into qbuf by the GPU command */
	VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	qci.queryType = VK_QUERY_TYPE_OCCLUSION;
	qci.queryCount = 1;
	VkQueryPool queries;
	VK_CHECK(vkCreateQueryPool(s.device, &qci, NULL, &queries));
	{
		VkCommandBuffer once = sample_begin_once(&s);
		vkCmdResetQueryPool(once, queries, 0, 1);
		const VkClearValue clear = { .color = { { 1, 0, 0, 1 } } };
		VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		rbi.renderPass = targetRp;
		rbi.framebuffer = targetFb[0];
		rbi.renderArea.extent = (VkExtent2D){ 64, 64 };
		rbi.clearValueCount = 1;
		rbi.pClearValues = &clear;
		vkCmdBeginRenderPass(once, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport tv = { 0, 0, 64, 64, 0.0f, 1.0f };
		VkRect2D tsc = { { 0, 0 }, { 64, 64 } };
		vkCmdSetViewport(once, 0, 1, &tv);
		vkCmdSetScissor(once, 0, 1, &tsc);
		vkCmdBindPipeline(once, VK_PIPELINE_BIND_POINT_GRAPHICS, targetPipe);
		vkCmdBindDescriptorSets(once, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
		struct { int32_t tile, which; float weights[4]; float lodScale; } p0 = { 0, 1, { 0 }, 1.0f };
		vkCmdPushConstants(once, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p0), &p0);
		vkCmdBeginQuery(once, queries, 0, 0);
		vkCmdDraw(once, 6, 1, 0, 0);
		vkCmdEndQuery(once, queries, 0);
		vkCmdEndRenderPass(once);
		vkCmdCopyQueryPoolResults(once, queries, 0, 1, qbuf, 0, 8,
		                          VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		sample_end_once(&s, once);
		vkvLog(VKV_LOG_WARN, "shader_tests: occlusion query copied %u samples, available %u", (unsigned)qp[0],
		       (unsigned)qp[1]);
	}

	s.clearColor = (VkClearColorValue){ { 0.08f, 0.09f, 0.13f, 1.0f } };
	uint32_t frameNo = 0;
	while (sample_poll_input(&s)) {
		VkCommandBuffer cb = sample_begin_frame(&s);
		VkViewport v = { 0, 0, (float)s.extent.width, (float)s.extent.height, 0.0f, 1.0f };
		VkRect2D sc = { { 0, 0 }, s.extent };
		vkCmdSetViewport(cb, 0, 1, &v);
		vkCmdSetScissor(cb, 0, 1, &sc);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
		/* about 2.3 texels per pixel on an 8x8 texture over the tile height: mip level 1.2 */
		const float lodScale = 2.3f * 0.33f * 0.5f * (float)s.extent.height / TEX_SIZE;
		for (int32_t tile = 0; tile < 19; tile++) {
			struct { int32_t tile, which; float weights[4]; float lodScale; } push = {
				tile, 1, { 1.0f, 0.5f, 0.25f, 0.125f }, lodScale };
			vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
			                   &push);
			vkCmdDraw(cb, 6, 1, 0, 0);
		}
		/* the last tile again in red, scissored to the 36 pixels left of it: on 32x32 tiles the
		 * scissor would reach to x = 64 and paint the left edge of the tile red */
		struct { int32_t tile, which; float weights[4]; float lodScale; } red = { 16, 7, { 0 }, lodScale };
		VkRect2D left = { { 0, 0 }, { 36, s.extent.height } };
		vkCmdSetScissor(cb, 0, 1, &left);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(red), &red);
		vkCmdDraw(cb, 6, 1, 0, 0);
		sample_end_frame(&s);
		if (++frameNo == 60) vkvLog(VKV_LOG_WARN, "shader_tests: running, nineteen green tiles expected");
	}

	vkDeviceWaitIdle(s.device);
	vkDestroyPipeline(s.device, pipe, NULL);
	vkDestroyPipeline(s.device, targetPipe, NULL);
	vkDestroyQueryPool(s.device, queries, NULL);
	vkDestroyPipelineLayout(s.device, layout, NULL);
	vkDestroyDescriptorPool(s.device, dpool, NULL);
	vkDestroyDescriptorSetLayout(s.device, setLayout, NULL);
	vkDestroySampler(s.device, sampler, NULL);
	vkDestroySampler(s.device, biasSampler, NULL);
	VkImageView views[7] = { layerView, alphaView, swappedView, mipView, targetLayer0, targetLayer1, copyView };
	for (int i = 0; i < 7; i++) vkDestroyImageView(s.device, views[i], NULL);
	for (int i = 0; i < 2; i++) vkDestroyFramebuffer(s.device, targetFb[i], NULL);
	vkDestroyRenderPass(s.device, targetRp, NULL);
	VkImage imgs[6] = { arrayImg, alphaImg, redImg, mipImg, targetImg, copyImg };
	VkDeviceMemory mems[6] = { arrayMem, alphaMem, redMem, mipMem, targetMem, copyMem };
	for (int i = 0; i < 6; i++) {
		vkDestroyImage(s.device, imgs[i], NULL);
		vkFreeMemory(s.device, mems[i], NULL);
	}
	for (int i = 0; i < 5; i++) destroy_texture(&s, &tex[i]);
	vkDestroyBuffer(s.device, qbuf, NULL);
	vkFreeMemory(s.device, qmem, NULL);
	vkDestroyBuffer(s.device, ubuf, NULL);
	vkFreeMemory(s.device, umem, NULL);
	for (int i = 0; i < 3; i++) {
		vkDestroyBuffer(s.device, sbuf[i], NULL);
		vkFreeMemory(s.device, smem[i], NULL);
	}
	sample_shutdown(&s);
	sceKernelExitProcess(0);
	return 0;
}
