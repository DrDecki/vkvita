/*
 * VkVita sample: gpu_features
 *
 * Four panels, each checking one texture feature on real hardware:
 *   top left      mip levels uploaded one by one, each level its own color.
 *                 The floor should show bands: white/gray, red, orange,
 *                 yellow, green, ... further away.
 *   top right     the same floor with mipmaps made by vkCmdBlitImage from a
 *                 black and white checker. Far away it turns smooth gray.
 *   bottom left   a cube map on a spinning cube: +X red, -X cyan, +Y green,
 *                 -Y magenta, +Z blue, -Z yellow, each face with a white
 *                 square in its first texel corner.
 *   bottom right  a BC1 (DXT1) texture through a separate image and sampler:
 *                 a smooth 16x16 grid of color blocks with a white block in
 *                 the top left corner.
 * The panel borders are 3 pixel wide lines, the cube is drawn with
 * vkCmdDrawIndexedIndirect, and every 60 frames an occlusion query on the
 * cube is written to the log.
 *
 * The features switch on one after another, three seconds apart, so a crash
 * shows which one caused it: borders, then top left, top right, bottom
 * right, bottom left, and last the occlusion query.
 *
 * Regenerate the shader headers after editing shaders/:
 *   glslangValidator -V --vn k_scene_vert_spv shaders/scene.vert -o scene_vert_spv.h
 *   (same for texture.frag, cube.frag, separate.frag and color.frag)
 *
 * Controls: START quits.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "vkv_sample.h"
#include "scene_vert_spv.h"
#include "texture_frag_spv.h"
#include "cube_frag_spv.h"
#include "separate_frag_spv.h"
#include "color_frag_spv.h"

typedef struct { float pos[3]; float uv[2]; } Vertex;

#define MIP_SIZE   256
#define MIP_LEVELS 9
#define CUBE_SIZE  64
#define BC_SIZE    64

static const uint32_t k_levelColors[MIP_LEVELS] = {
	0, 0xFF2020E0u, 0xFF2090F0u, 0xFF20E0E0u, 0xFF20D020u, 0xFFE0E020u, 0xFFE03020u, 0xFFD020D0u, 0xFFFFFFFFu,
};

/* face colors in RGBA byte order stored little endian: 0xAABBGGRR */
static const uint32_t k_faceColors[6] = {
	0xFF2020E0u, 0xFFE0E020u, 0xFF20D020u, 0xFFD020D0u, 0xFFE03020u, 0xFF20E0E0u,
};

static VkShaderModule make_module(Sample *s, const uint32_t *code, size_t size) {
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = size;
	ci.pCode = code;
	VkShaderModule m;
	VK_CHECK(vkCreateShaderModule(s->device, &ci, NULL, &m));
	return m;
}

static VkImage make_image(Sample *s, VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers,
                          VkImageCreateFlags flags, VkDeviceMemory *mem) {
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.flags = flags;
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = fmt;
	ii.extent = (VkExtent3D){ w, h, 1 };
	ii.mipLevels = levels;
	ii.arrayLayers = layers;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkImage img;
	VK_CHECK(vkCreateImage(s->device, &ii, NULL, &img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s->device, img, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(s->device, &ai, NULL, mem));
	VK_CHECK(vkBindImageMemory(s->device, img, *mem, 0));
	return img;
}

static VkImageView make_view(Sample *s, VkImage img, VkFormat fmt, VkImageViewType type, uint32_t levels,
                             uint32_t layers) {
	VkImageViewCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	ci.image = img;
	ci.viewType = type;
	ci.format = fmt;
	ci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers };
	VkImageView v;
	VK_CHECK(vkCreateImageView(s->device, &ci, NULL, &v));
	return v;
}

static void barrier(VkCommandBuffer cb, VkImage img, uint32_t level, uint32_t levels, uint32_t layers,
                    VkImageLayout from, VkImageLayout to) {
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.image = img;
	b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, layers };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT |
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) {
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void build_cube(Vertex *v, uint16_t *idx) {
	static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
	for (int f = 0; f < 6; f++) {
		float u[3];
		if (n[f][0] != 0) { u[0] = 0; u[1] = 0; u[2] = -n[f][0]; }
		else if (n[f][2] != 0) { u[0] = n[f][2]; u[1] = 0; u[2] = 0; }
		else { u[0] = 1; u[1] = 0; u[2] = 0; }
		float w[3] = { n[f][1] * u[2] - n[f][2] * u[1], n[f][2] * u[0] - n[f][0] * u[2], n[f][0] * u[1] - n[f][1] * u[0] };
		for (int i = 0; i < 4; i++) {
			float a = (i == 1 || i == 2) ? 1.0f : -1.0f, b = (i >= 2) ? 1.0f : -1.0f;
			Vertex *p = &v[f * 4 + i];
			for (int k = 0; k < 3; k++) p->pos[k] = 0.5f * (n[f][k] + a * u[k] + b * w[k]);
			p->uv[0] = a > 0 ? 1.0f : 0.0f;
			p->uv[1] = b > 0 ? 1.0f : 0.0f;
		}
		uint16_t base = (uint16_t)(f * 4);
		uint16_t *q = &idx[f * 6];
		q[0] = base; q[1] = base + 1; q[2] = base + 2;
		q[3] = base; q[4] = base + 2; q[5] = base + 3;
	}
}

int main(void) {
	sample_configure("gpu_features");
	Sample s;
	sample_init(&s, "gpu_features", true);

	/* --- geometry: floor (4), cube (24), quad (4), border lines (8) ----- */
	Vertex verts[40];
	uint16_t indices[48];
	const float floorUV = 24.0f;
	Vertex floorV[4] = {
		{ { -4.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } }, { { 4.0f, 0.0f, 0.0f }, { floorUV * 0.25f, 0.0f } },
		{ { 4.0f, 0.0f, -32.0f }, { floorUV * 0.25f, floorUV } }, { { -4.0f, 0.0f, -32.0f }, { 0.0f, floorUV } },
	};
	memcpy(verts, floorV, sizeof(floorV));
	build_cube(verts + 4, indices + 6);
	for (int i = 0; i < 36; i++) indices[6 + i] += 4;
	Vertex quadV[4] = {
		{ { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } }, { { 1.0f, -1.0f, 0.0f }, { 1.0f, 0.0f } },
		{ { 1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } }, { { -1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
	};
	memcpy(verts + 28, quadV, sizeof(quadV));
	const float e = 0.995f;
	Vertex lineV[8] = {
		{ { -e, -e, 0 }, { 0, 0 } }, { { e, -e, 0 }, { 0, 0 } }, { { e, -e, 0 }, { 0, 0 } }, { { e, e, 0 }, { 0, 0 } },
		{ { e, e, 0 }, { 0, 0 } }, { { -e, e, 0 }, { 0, 0 } }, { { -e, e, 0 }, { 0, 0 } }, { { -e, -e, 0 }, { 0, 0 } },
	};
	memcpy(verts + 32, lineV, sizeof(lineV));
	uint16_t floorIdx[6] = { 0, 1, 2, 0, 2, 3 };
	memcpy(indices, floorIdx, sizeof(floorIdx));
	uint16_t quadIdx[6] = { 28, 29, 30, 28, 30, 31 };
	memcpy(indices + 42, quadIdx, sizeof(quadIdx));

	VkBuffer vbuf, ibuf, indirect;
	VkDeviceMemory vmem, imem, indMem;
	void *p;
	sample_create_buffer(&s, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &vbuf, &vmem, &p);
	memcpy(p, verts, sizeof(verts));
	sample_create_buffer(&s, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &ibuf, &imem, &p);
	memcpy(p, indices, sizeof(indices));
	VkDrawIndexedIndirectCommand cubeDraw = { 36, 1, 6, 0, 0 };
	sample_create_buffer(&s, sizeof(cubeDraw), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &indirect, &indMem, &p);
	memcpy(p, &cubeDraw, sizeof(cubeDraw));

	/* --- staging data --------------------------------------------------- */
	VkDeviceSize mipBytes = 0;
	VkDeviceSize mipOffset[MIP_LEVELS];
	for (int l = 0; l < MIP_LEVELS; l++) {
		uint32_t d = MIP_SIZE >> l;
		mipOffset[l] = mipBytes;
		mipBytes += (VkDeviceSize)d * d * 4;
	}
	VkDeviceSize checkerOff = mipBytes, cubeOff = checkerOff + MIP_SIZE * MIP_SIZE * 4;
	VkDeviceSize bcOff = cubeOff + 6 * CUBE_SIZE * CUBE_SIZE * 4;
	VkDeviceSize stagingSize = bcOff + (BC_SIZE / 4) * (BC_SIZE / 4) * 8;
	VkBuffer staging;
	VkDeviceMemory stagingMem;
	uint8_t *st;
	sample_create_buffer(&s, stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &staging, &stagingMem, (void **)&st);
	for (int l = 0; l < MIP_LEVELS; l++) {
		uint32_t d = MIP_SIZE >> l, *px = (uint32_t *)(st + mipOffset[l]);
		for (uint32_t y = 0; y < d; y++)
			for (uint32_t x = 0; x < d; x++)
				px[y * d + x] = l ? k_levelColors[l] : (((x / 16) ^ (y / 16)) & 1) ? 0xFFFFFFFFu : 0xFF808080u;
	}
	uint32_t *checker = (uint32_t *)(st + checkerOff);
	for (uint32_t y = 0; y < MIP_SIZE; y++)
		for (uint32_t x = 0; x < MIP_SIZE; x++)
			checker[y * MIP_SIZE + x] = (((x / 4) ^ (y / 4)) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
	for (int f = 0; f < 6; f++) {
		uint32_t *px = (uint32_t *)(st + cubeOff) + f * CUBE_SIZE * CUBE_SIZE;
		for (uint32_t y = 0; y < CUBE_SIZE; y++)
			for (uint32_t x = 0; x < CUBE_SIZE; x++)
				px[y * CUBE_SIZE + x] = (x < 16 && y < 16) ? 0xFFFFFFFFu : k_faceColors[f];
	}
	uint8_t *bc = st + bcOff;
	for (uint32_t by = 0; by < BC_SIZE / 4; by++)
		for (uint32_t bx = 0; bx < BC_SIZE / 4; bx++) {
			uint16_t c = (bx == 0 && by == 0) ? 0xFFFF : rgb565(bx * 16, by * 16, 255 - bx * 8);
			uint8_t *blk = bc + (by * (BC_SIZE / 4) + bx) * 8;
			blk[0] = (uint8_t)c; blk[1] = (uint8_t)(c >> 8); /* color0 */
			blk[2] = (uint8_t)c; blk[3] = (uint8_t)(c >> 8); /* color1 */
			memset(blk + 4, 0, 4);                             /* every texel uses color0 */
		}

	/* --- images --------------------------------------------------------- */
	VkDeviceMemory mipMem, blitMem, cubeMem, bcMem;
	VkImage mipImg = make_image(&s, VK_FORMAT_R8G8B8A8_UNORM, MIP_SIZE, MIP_SIZE, MIP_LEVELS, 1, 0, &mipMem);
	VkImage blitImg = make_image(&s, VK_FORMAT_R8G8B8A8_UNORM, MIP_SIZE, MIP_SIZE, MIP_LEVELS, 1, 0, &blitMem);
	VkImage cubeImg = make_image(&s, VK_FORMAT_R8G8B8A8_UNORM, CUBE_SIZE, CUBE_SIZE, 1, 6,
	                             VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, &cubeMem);
	VkImage bcImg = make_image(&s, VK_FORMAT_BC1_RGB_UNORM_BLOCK, BC_SIZE, BC_SIZE, 1, 1, 0, &bcMem);

	VkCommandBuffer up = sample_begin_once(&s);
	barrier(up, mipImg, 0, MIP_LEVELS, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	barrier(up, blitImg, 0, MIP_LEVELS, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	barrier(up, cubeImg, 0, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	barrier(up, bcImg, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy mipCopies[MIP_LEVELS];
	memset(mipCopies, 0, sizeof(mipCopies));
	for (int l = 0; l < MIP_LEVELS; l++) {
		mipCopies[l].bufferOffset = mipOffset[l];
		mipCopies[l].imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, 1 };
		mipCopies[l].imageExtent = (VkExtent3D){ MIP_SIZE >> l, MIP_SIZE >> l, 1 };
	}
	vkCmdCopyBufferToImage(up, staging, mipImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MIP_LEVELS, mipCopies);
	VkBufferImageCopy one = { 0 };
	one.bufferOffset = checkerOff;
	one.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	one.imageExtent = (VkExtent3D){ MIP_SIZE, MIP_SIZE, 1 };
	vkCmdCopyBufferToImage(up, staging, blitImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &one);
	one.bufferOffset = cubeOff;
	one.imageSubresource.layerCount = 6;
	one.imageExtent = (VkExtent3D){ CUBE_SIZE, CUBE_SIZE, 1 };
	vkCmdCopyBufferToImage(up, staging, cubeImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &one);
	one.bufferOffset = bcOff;
	one.imageSubresource.layerCount = 1;
	one.imageExtent = (VkExtent3D){ BC_SIZE, BC_SIZE, 1 };
	vkCmdCopyBufferToImage(up, staging, bcImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &one);
	/* the usual mipmap generation loop */
	for (uint32_t l = 1; l < MIP_LEVELS; l++) {
		barrier(up, blitImg, l - 1, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit b = { 0 };
		b.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1 };
		b.srcOffsets[1] = (VkOffset3D){ (int32_t)(MIP_SIZE >> (l - 1)), (int32_t)(MIP_SIZE >> (l - 1)), 1 };
		b.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 };
		b.dstOffsets[1] = (VkOffset3D){ (int32_t)(MIP_SIZE >> l), (int32_t)(MIP_SIZE >> l), 1 };
		vkCmdBlitImage(up, blitImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, blitImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		               1, &b, VK_FILTER_LINEAR);
	}
	barrier(up, mipImg, 0, MIP_LEVELS, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	barrier(up, blitImg, 0, MIP_LEVELS, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	barrier(up, cubeImg, 0, 1, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	barrier(up, bcImg, 0, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	sample_end_once(&s, up);
	vkDestroyBuffer(s.device, staging, NULL);
	vkFreeMemory(s.device, stagingMem, NULL);

	VkImageView mipView = make_view(&s, mipImg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, MIP_LEVELS, 1);
	VkImageView blitView = make_view(&s, blitImg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, MIP_LEVELS, 1);
	VkImageView cubeView = make_view(&s, cubeImg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_CUBE, 1, 6);
	VkImageView bcView = make_view(&s, bcImg, VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_IMAGE_VIEW_TYPE_2D, 1, 1);

	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = 16.0f;
	VkSampler trilinear, nearest;
	VK_CHECK(vkCreateSampler(s.device, &sci, NULL, &trilinear));
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = 0.25f;
	VK_CHECK(vkCreateSampler(s.device, &sci, NULL, &nearest));

	/* --- descriptors ---------------------------------------------------- */
	VkDescriptorSetLayoutBinding combined = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
	                                          VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutBinding separate[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dl.bindingCount = 1;
	dl.pBindings = &combined;
	VkDescriptorSetLayout combinedLayout, separateLayout;
	VK_CHECK(vkCreateDescriptorSetLayout(s.device, &dl, NULL, &combinedLayout));
	dl.bindingCount = 2;
	dl.pBindings = separate;
	VK_CHECK(vkCreateDescriptorSetLayout(s.device, &dl, NULL, &separateLayout));
	VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 },
	                               { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 } };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = 4;
	dpci.poolSizeCount = 3;
	dpci.pPoolSizes = ps;
	VkDescriptorPool dpool;
	VK_CHECK(vkCreateDescriptorPool(s.device, &dpci, NULL, &dpool));
	VkDescriptorSetLayout setLayouts[4] = { combinedLayout, combinedLayout, combinedLayout, separateLayout };
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsai.descriptorPool = dpool;
	dsai.descriptorSetCount = 4;
	dsai.pSetLayouts = setLayouts;
	VkDescriptorSet sets[4]; /* mips, blit, cube, bc */
	VK_CHECK(vkAllocateDescriptorSets(s.device, &dsai, sets));
	VkDescriptorImageInfo infos[5] = {
		{ trilinear, mipView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ trilinear, blitView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ trilinear, cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ VK_NULL_HANDLE, bcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ nearest, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED },
	};
	VkWriteDescriptorSet w[5];
	memset(w, 0, sizeof(w));
	for (int i = 0; i < 5; i++) {
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[i].descriptorCount = 1;
		w[i].pImageInfo = &infos[i];
		w[i].dstSet = sets[i < 3 ? i : 3];
		w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
		                    : i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
		w[i].dstBinding = i == 4 ? 1 : 0;
	}
	vkUpdateDescriptorSets(s.device, 5, w, 0, NULL);

	/* --- pipelines ------------------------------------------------------ */
	VkPushConstantRange ranges[2] = { { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 }, { VK_SHADER_STAGE_FRAGMENT_BIT, 64, 16 } };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &combinedLayout;
	plci.pushConstantRangeCount = 2;
	plci.pPushConstantRanges = ranges;
	VkPipelineLayout combinedPL, separatePL;
	VK_CHECK(vkCreatePipelineLayout(s.device, &plci, NULL, &combinedPL));
	plci.pSetLayouts = &separateLayout;
	VK_CHECK(vkCreatePipelineLayout(s.device, &plci, NULL, &separatePL));

	VkShaderModule vs = make_module(&s, k_scene_vert_spv, sizeof(k_scene_vert_spv));
	VkShaderModule fsTex = make_module(&s, k_texture_frag_spv, sizeof(k_texture_frag_spv));
	VkShaderModule fsCube = make_module(&s, k_cube_frag_spv, sizeof(k_cube_frag_spv));
	VkShaderModule fsSep = make_module(&s, k_separate_frag_spv, sizeof(k_separate_frag_spv));
	VkShaderModule fsColor = make_module(&s, k_color_frag_spv, sizeof(k_color_frag_spv));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsTex, "main", NULL },
	};
	VkVertexInputBindingDescription binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attrs[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &binding;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = attrs;
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vps.viewportCount = vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo msi = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	msi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	dsi.depthTestEnable = VK_TRUE;
	dsi.depthWriteEnable = VK_TRUE;
	dsi.depthCompareOp = VK_COMPARE_OP_LESS;
	VkPipelineColorBlendAttachmentState ba = { 0 };
	ba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cbs.attachmentCount = 1;
	cbs.pAttachments = &ba;
	VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH };
	VkPipelineDynamicStateCreateInfo dyi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dyi.dynamicStateCount = 3;
	dyi.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vps;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &msi;
	gp.pDepthStencilState = &dsi;
	gp.pColorBlendState = &cbs;
	gp.pDynamicState = &dyi;
	gp.layout = combinedPL;
	gp.renderPass = s.renderPass;
	VkPipeline texPipe, cubePipe, sepPipe, linePipe;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &texPipe));
	stages[1].module = fsCube;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &cubePipe));
	stages[1].module = fsSep;
	gp.layout = separatePL;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &sepPipe));
	stages[1].module = fsColor;
	gp.layout = combinedPL;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	dsi.depthTestEnable = VK_FALSE;
	dsi.depthWriteEnable = VK_FALSE;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &linePipe));
	vkDestroyShaderModule(s.device, vs, NULL);
	vkDestroyShaderModule(s.device, fsTex, NULL);
	vkDestroyShaderModule(s.device, fsCube, NULL);
	vkDestroyShaderModule(s.device, fsSep, NULL);
	vkDestroyShaderModule(s.device, fsColor, NULL);

	VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	qci.queryType = VK_QUERY_TYPE_OCCLUSION;
	qci.queryCount = 1;
	VkQueryPool queries;
	VK_CHECK(vkCreateQueryPool(s.device, &qci, NULL, &queries));

	/* --- loop ----------------------------------------------------------- */
	const float pw = (float)s.extent.width * 0.5f, ph = (float)s.extent.height * 0.5f;
	float proj[16], view4[16], floorMvp[16], quadMvp[16], ident[16];
	mat4_perspective(proj, 1.0f, pw / ph, 0.1f, 100.0f);
	mat4_identity(ident);
	float rot[16], lift[16];
	mat4_rotate_xyz(rot, 0.45f, 0.0f, 0.0f);
	mat4_translate(lift, 0.0f, -1.0f, -0.5f);
	mat4_mul(view4, rot, lift);
	mat4_mul(floorMvp, proj, view4);
	float flip[16];
	mat4_identity(flip);
	flip[0] = 0.55f;
	flip[5] = 0.95f;
	memcpy(quadMvp, flip, sizeof(flip));
	float t = 0.0f;
	uint32_t frameNo = 0;
	bool queryIssued = false;

	while (sample_poll_input(&s)) {
		t += 1.0f / 60.0f;
		uint32_t phase = frameNo / 180;
		if (frameNo % 180 == 0 && phase <= 5) vkvLog(VKV_LOG_WARN, "gpu_features: phase %u", (unsigned)phase);
		static const int k_panelPhase[4] = { 1, 2, 4, 3 };
		bool runQuery = phase >= 5 && (frameNo % 60) == 0;
		if (runQuery) {
			if (queryIssued) {
				uint64_t samples = 0;
				if (vkGetQueryPoolResults(s.device, queries, 0, 1, sizeof(samples), &samples, sizeof(samples),
				                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
					vkvLog(VKV_LOG_WARN, "occlusion query: the cube covered %llu samples", (unsigned long long)samples);
			}
			VkCommandBuffer rc = sample_begin_once(&s);
			vkCmdResetQueryPool(rc, queries, 0, 1);
			sample_end_once(&s, rc);
		}

		VkCommandBuffer cb = sample_begin_frame(&s);
		VkBuffer vbs[1] = { vbuf };
		VkDeviceSize offs[1] = { 0 };
		vkCmdBindVertexBuffers(cb, 0, 1, vbs, offs);
		vkCmdBindIndexBuffer(cb, ibuf, 0, VK_INDEX_TYPE_UINT16);

		for (int panel = 0; panel < 4; panel++) {
			VkViewport v = { (panel & 1) * pw, (panel >> 1) * ph, pw, ph, 0.0f, 1.0f };
			vkCmdSetViewport(cb, 0, 1, &v);
			if (phase < (uint32_t)k_panelPhase[panel]) {
			} else if (panel < 2) {
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, texPipe);
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, combinedPL, 0, 1, &sets[panel], 0, NULL);
				vkCmdPushConstants(cb, combinedPL, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, floorMvp);
				vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
			} else if (panel == 2) {
				float model[16], cubeVp[16], cubeMvp[16], back[16];
				mat4_rotate_xyz(model, t * 0.7f, t, 0.0f);
				mat4_translate(back, 0.0f, 0.0f, -2.2f);
				mat4_mul(cubeVp, proj, back);
				mat4_mul(cubeMvp, cubeVp, model);
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cubePipe);
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, combinedPL, 0, 1, &sets[2], 0, NULL);
				vkCmdPushConstants(cb, combinedPL, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, cubeMvp);
				if (runQuery) vkCmdBeginQuery(cb, queries, 0, 0);
				vkCmdDrawIndexedIndirect(cb, indirect, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
				if (runQuery) vkCmdEndQuery(cb, queries, 0);
			} else {
				vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sepPipe);
				vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, separatePL, 0, 1, &sets[3], 0, NULL);
				vkCmdPushConstants(cb, separatePL, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, quadMvp);
				vkCmdDrawIndexed(cb, 6, 1, 42, 0, 0);
			}
			float color[4] = { 0.9f, 0.9f, 0.9f, 1.0f };
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipe);
			vkCmdSetLineWidth(cb, 3.0f);
			vkCmdPushConstants(cb, combinedPL, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
			vkCmdPushConstants(cb, combinedPL, VK_SHADER_STAGE_FRAGMENT_BIT, 64, 16, color);
			vkCmdDraw(cb, 8, 1, 32, 0);
		}
		sample_end_frame(&s);
		if (runQuery) queryIssued = true;
		frameNo++;
	}

	vkDeviceWaitIdle(s.device);
	vkDestroyQueryPool(s.device, queries, NULL);
	vkDestroyPipeline(s.device, texPipe, NULL);
	vkDestroyPipeline(s.device, cubePipe, NULL);
	vkDestroyPipeline(s.device, sepPipe, NULL);
	vkDestroyPipeline(s.device, linePipe, NULL);
	vkDestroyPipelineLayout(s.device, combinedPL, NULL);
	vkDestroyPipelineLayout(s.device, separatePL, NULL);
	vkDestroyDescriptorPool(s.device, dpool, NULL);
	vkDestroyDescriptorSetLayout(s.device, combinedLayout, NULL);
	vkDestroyDescriptorSetLayout(s.device, separateLayout, NULL);
	vkDestroySampler(s.device, trilinear, NULL);
	vkDestroySampler(s.device, nearest, NULL);
	VkImageView views[4] = { mipView, blitView, cubeView, bcView };
	VkImage images[4] = { mipImg, blitImg, cubeImg, bcImg };
	VkDeviceMemory mems[4] = { mipMem, blitMem, cubeMem, bcMem };
	for (int i = 0; i < 4; i++) {
		vkDestroyImageView(s.device, views[i], NULL);
		vkDestroyImage(s.device, images[i], NULL);
		vkFreeMemory(s.device, mems[i], NULL);
	}
	vkDestroyBuffer(s.device, vbuf, NULL);
	vkDestroyBuffer(s.device, ibuf, NULL);
	vkDestroyBuffer(s.device, indirect, NULL);
	vkFreeMemory(s.device, vmem, NULL);
	vkFreeMemory(s.device, imem, NULL);
	vkFreeMemory(s.device, indMem, NULL);
	sample_shutdown(&s);
	sceKernelExitProcess(0);
	return 0;
}
