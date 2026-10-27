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
 *
 * Expected picture: twelve green tiles.
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
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
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

	/* --- descriptors ---------------------------------------------------------- */
	VkDescriptorSetLayoutBinding lb[7] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	VkDescriptorSetLayoutCreateInfo dlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dlci.bindingCount = 7;
	dlci.pBindings = lb;
	VkDescriptorSetLayout setLayout;
	VK_CHECK(vkCreateDescriptorSetLayout(s.device, &dlci, NULL, &setLayout));
	VkDescriptorPoolSize ps[5] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 },
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
	VkWriteDescriptorSet wr[7];
	memset(wr, 0, sizeof(wr));
	for (int i = 0; i < 7; i++) {
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
	vkUpdateDescriptorSets(s.device, 7, wr, 0, NULL);

	/* --- pipeline: the tile comes from push constants, no vertex buffer ----- */
	VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24 };
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
	VkPipeline pipe;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
	vkDestroyShaderModule(s.device, vs, NULL);
	vkDestroyShaderModule(s.device, fs, NULL);

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
		for (int32_t tile = 0; tile < 12; tile++) {
			struct { int32_t tile, which; float weights[4]; } push = { tile, 1, { 1.0f, 0.5f, 0.25f, 0.125f } };
			vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
			                   &push);
			vkCmdDraw(cb, 6, 1, 0, 0);
		}
		sample_end_frame(&s);
		if (++frameNo == 60) vkvLog(VKV_LOG_WARN, "shader_tests: running, twelve green tiles expected");
	}

	vkDeviceWaitIdle(s.device);
	vkDestroyPipeline(s.device, pipe, NULL);
	vkDestroyPipelineLayout(s.device, layout, NULL);
	vkDestroyDescriptorPool(s.device, dpool, NULL);
	vkDestroyDescriptorSetLayout(s.device, setLayout, NULL);
	vkDestroySampler(s.device, sampler, NULL);
	for (int i = 0; i < 5; i++) destroy_texture(&s, &tex[i]);
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
