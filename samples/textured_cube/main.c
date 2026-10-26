/*
 * VkVita sample: textured_cube
 *
 * Shows: staging uploads (vkCmdCopyBuffer / vkCmdCopyBufferToImage),
 * a combined image sampler descriptor, push constants used by both shader
 * stages, a per-instance vertex binding, depth testing, a wireframe
 * pipeline, and switching VkVita debug options at runtime.
 *
 * Controls:
 *   left stick  rotate          CROSS     wireframe on/off
 *   SELECT      profile log     TRIANGLE  validation on/off
 *   SQUARE      GPU sync timing CIRCLE    culling off (compat)
 *   START       quit
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "vkv_sample.h"

/* Locations follow declaration order: 0 aPosition, 1 aTexcoord, 2 aColor,
 * 3 aOffset (per instance). Push constants: uMvp = bytes 0..63 (vertex
 * range), uTint = bytes 64..79 (fragment range starts at offset 64). */
static const char *k_vs =
	"void main(float3 aPosition, float2 aTexcoord, float4 aColor, float3 aOffset,\n"
	"          uniform float4x4 uMvp,\n"
	"          float4 out vPosition : POSITION, float2 out vTexcoord : TEXCOORD0, float4 out vColor : COLOR)\n"
	"{\n"
	"    vPosition = mul(float4(aPosition + aOffset, 1.0f), uMvp);\n"
	"    vTexcoord = aTexcoord;\n"
	"    vColor = aColor;\n"
	"}\n";

static const char *k_fs =
	"float4 main(float2 vTexcoord : TEXCOORD0, float4 vColor : COLOR,\n"
	"            uniform sampler2D uTex : TEXUNIT0, uniform float4 uTint) : COLOR\n"
	"{\n"
	"    return tex2D(uTex, vTexcoord) * vColor * uTint;\n"
	"}\n";

typedef struct { float pos[3]; float uv[2]; uint8_t rgba[4]; } Vertex;
typedef struct { float mvp[16]; float tint[4]; } Push;

#define TEX_SIZE 128

static void build_cube(Vertex *v, uint16_t *idx) {
	static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
	static const uint8_t col[6][4] = { { 255, 120, 120, 255 }, { 120, 255, 120, 255 }, { 120, 120, 255, 255 },
	                                   { 255, 255, 120, 255 }, { 120, 255, 255, 255 }, { 255, 120, 255, 255 } };
	for (int f = 0; f < 6; f++) {
		/* two tangent axes for the face; w = n x u keeps every face CCW from outside */
		float u[3];
		if (n[f][0] != 0) { u[0] = 0; u[1] = 0; u[2] = -n[f][0]; }
		else if (n[f][2] != 0) { u[0] = n[f][2]; u[1] = 0; u[2] = 0; }
		else { u[0] = 1; u[1] = 0; u[2] = 0; }
		float w[3] = { n[f][1] * u[2] - n[f][2] * u[1], n[f][2] * u[0] - n[f][0] * u[2], n[f][0] * u[1] - n[f][1] * u[0] };
		for (int i = 0; i < 4; i++) {
			float a = (i == 1 || i == 2) ? 1.0f : -1.0f;
			float b = (i >= 2) ? 1.0f : -1.0f;
			Vertex *p = &v[f * 4 + i];
			for (int k = 0; k < 3; k++) p->pos[k] = 0.5f * (n[f][k] + a * u[k] + b * w[k]);
			p->uv[0] = a > 0 ? 1.0f : 0.0f;
			p->uv[1] = b > 0 ? 1.0f : 0.0f;
			memcpy(p->rgba, col[f], 4);
		}
		uint16_t base = (uint16_t)(f * 4);
		uint16_t *q = &idx[f * 6];
		q[0] = base; q[1] = base + 1; q[2] = base + 2;
		q[3] = base; q[4] = base + 2; q[5] = base + 3;
	}
}

int main(void) {
	sample_configure("textured_cube");
	Sample s;
	sample_init(&s, "textured_cube", true);

	/* --- geometry: built on the CPU, uploaded through a staging buffer --- */
	Vertex verts[24];
	uint16_t indices[36];
	build_cube(verts, indices);
	float offsets[9][3];
	for (int i = 0; i < 9; i++) {
		offsets[i][0] = (float)(i % 3 - 1) * 1.6f;
		offsets[i][1] = (float)(i / 3 - 1) * 1.6f;
		offsets[i][2] = 0.0f;
	}
	VkDeviceSize vsize = sizeof(verts), isize = sizeof(indices), osize = sizeof(offsets);
	VkDeviceSize texBytes = TEX_SIZE * TEX_SIZE * 4;

	VkBuffer staging;
	VkDeviceMemory stagingMem;
	uint8_t *st;
	sample_create_buffer(&s, vsize + isize + osize + texBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &staging, &stagingMem, (void **)&st);
	memcpy(st, verts, vsize);
	memcpy(st + vsize, indices, isize);
	memcpy(st + vsize + isize, offsets, osize);
	uint32_t *tex = (uint32_t *)(st + vsize + isize + osize);
	for (int y = 0; y < TEX_SIZE; y++)
		for (int x = 0; x < TEX_SIZE; x++) {
			int c = ((x / 16) ^ (y / 16)) & 1;
			tex[y * TEX_SIZE + x] = c ? 0xFFFFFFFFu : 0xFF404040u; /* ABGR in memory = RGBA bytes */
		}

	VkBuffer vbuf, ibuf, obuf;
	VkDeviceMemory vmem, imem, omem;
	sample_create_buffer(&s, vsize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vbuf, &vmem, NULL);
	sample_create_buffer(&s, isize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ibuf, &imem, NULL);
	sample_create_buffer(&s, osize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &obuf, &omem, NULL);

	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM;
	ii.extent = (VkExtent3D){ TEX_SIZE, TEX_SIZE, 1 };
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImage image;
	VK_CHECK(vkCreateImage(s.device, &ii, NULL, &image));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s.device, image, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(&s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory imageMem;
	VK_CHECK(vkAllocateMemory(s.device, &ai, NULL, &imageMem));
	VK_CHECK(vkBindImageMemory(s.device, image, imageMem, 0));

	VkCommandBuffer up = sample_begin_once(&s);
	VkBufferCopy c0 = { 0, 0, vsize }, c1 = { vsize, 0, isize }, c2 = { vsize + isize, 0, osize };
	vkCmdCopyBuffer(up, staging, vbuf, 1, &c0);
	vkCmdCopyBuffer(up, staging, ibuf, 1, &c1);
	vkCmdCopyBuffer(up, staging, obuf, 1, &c2);
	VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.image = image;
	toDst.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
	                     1, &toDst);
	VkBufferImageCopy bic = { 0 };
	bic.bufferOffset = vsize + isize + osize;
	bic.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	bic.imageExtent = (VkExtent3D){ TEX_SIZE, TEX_SIZE, 1 };
	vkCmdCopyBufferToImage(up, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	VkImageMemoryBarrier toRead = toDst;
	toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
	                     NULL, 1, &toRead);
	sample_end_once(&s, up);
	vkDestroyBuffer(s.device, staging, NULL);
	vkFreeMemory(s.device, stagingMem, NULL);

	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vci.image = image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ii.format;
	vci.subresourceRange = toDst.subresourceRange;
	VkImageView view;
	VK_CHECK(vkCreateImageView(s.device, &vci, NULL, &view));
	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VkSampler sampler;
	VK_CHECK(vkCreateSampler(s.device, &sci, NULL, &sampler));

	/* --- descriptors: binding 0 = TEXUNIT0 ------------------------------ */
	VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
	                                    NULL };
	VkDescriptorSetLayoutCreateInfo dlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dlci.bindingCount = 1;
	dlci.pBindings = &lb;
	VkDescriptorSetLayout setLayout;
	VK_CHECK(vkCreateDescriptorSetLayout(s.device, &dlci, NULL, &setLayout));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpci.maxSets = 1;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes = &ps;
	VkDescriptorPool dpool;
	VK_CHECK(vkCreateDescriptorPool(s.device, &dpci, NULL, &dpool));
	VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsai.descriptorPool = dpool;
	dsai.descriptorSetCount = 1;
	dsai.pSetLayouts = &setLayout;
	VkDescriptorSet set;
	VK_CHECK(vkAllocateDescriptorSets(s.device, &dsai, &set));
	VkDescriptorImageInfo dii = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	w.dstSet = set;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &dii;
	vkUpdateDescriptorSets(s.device, 1, &w, 0, NULL);

	/* --- pipelines ------------------------------------------------------ */
	VkShaderModule vs, fs;
	VK_CHECK(vkvCreateShaderModuleFromCg(s.device, k_vs, "cube_vs", &vs));
	VK_CHECK(vkvCreateShaderModuleFromCg(s.device, k_fs, "cube_fs", &fs));
	VkPushConstantRange ranges[2] = { { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 }, { VK_SHADER_STAGE_FRAGMENT_BIT, 64, 16 } };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 2;
	plci.pPushConstantRanges = ranges;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(s.device, &plci, NULL, &layout));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
	};
	VkVertexInputBindingDescription bindings[2] = {
		{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_INSTANCE },
	};
	VkVertexInputAttributeDescription attrs[4] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
		{ 2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20 },
		{ 3, 1, VK_FORMAT_R32G32B32_SFLOAT, 0 },
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = 2;
	vi.pVertexBindingDescriptions = bindings;
	vi.vertexAttributeDescriptionCount = 4;
	vi.pVertexAttributeDescriptions = attrs;
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vps.viewportCount = vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_BACK_BIT;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; /* faces are wound CCW seen from outside */
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
	gp.pDepthStencilState = &dsi;
	gp.pColorBlendState = &cbs;
	gp.pDynamicState = &dyi;
	gp.layout = layout;
	gp.renderPass = s.renderPass;
	VkPipeline solid, wire;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &solid));
	rs.polygonMode = VK_POLYGON_MODE_LINE;
	rs.cullMode = VK_CULL_MODE_NONE;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &wire));
	vkDestroyShaderModule(s.device, vs, NULL);
	vkDestroyShaderModule(s.device, fs, NULL);

	/* --- loop ----------------------------------------------------------- */
	float rx = 0.4f, ry = 0.0f, t = 0.0f;
	bool wireframe = false;
	float proj[16], view4[16], vp[16];
	mat4_perspective(proj, 1.0f, (float)s.extent.width / (float)s.extent.height, 0.1f, 50.0f);
	mat4_translate(view4, 0.0f, 0.0f, -6.0f);
	mat4_mul(vp, proj, view4);

	while (sample_poll_input(&s)) {
		VkvConfig cfg;
		vkvGetConfig(&cfg);
		if (s.pressed & SCE_CTRL_SELECT) {
			vkvSetDebugFlags(cfg.debugFlags ^ VKV_DEBUG_PROFILE);
			vkvLog(VKV_LOG_INFO, "profile log %s", (cfg.debugFlags & VKV_DEBUG_PROFILE) ? "off" : "on");
		}
		if (s.pressed & SCE_CTRL_TRIANGLE) {
			vkvSetDebugFlags(cfg.debugFlags ^ VKV_DEBUG_VALIDATION);
			vkvLog(VKV_LOG_INFO, "validation %s", (cfg.debugFlags & VKV_DEBUG_VALIDATION) ? "off" : "on");
		}
		if (s.pressed & SCE_CTRL_SQUARE) {
			vkvSetDebugFlags(cfg.debugFlags ^ VKV_DEBUG_GPU_SYNC);
			vkvLog(VKV_LOG_INFO, "gpu sync timing %s", (cfg.debugFlags & VKV_DEBUG_GPU_SYNC) ? "off" : "on");
		}
		if (s.pressed & SCE_CTRL_CIRCLE) {
			vkvSetCompatFlags(cfg.compatFlags ^ VKV_COMPAT_DISABLE_CULL);
			vkvLog(VKV_LOG_INFO, "culling %s", (cfg.compatFlags & VKV_COMPAT_DISABLE_CULL) ? "on" : "off");
		}
		if (s.pressed & SCE_CTRL_CROSS) wireframe = !wireframe;

		t += 1.0f / 60.0f;
		ry += 0.01f + s.lx * 0.0004f;
		rx += s.ly * 0.0004f;

		VkCommandBuffer cb = sample_begin_frame(&s);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe ? wire : solid);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
		VkBuffer vbs[2] = { vbuf, obuf };
		VkDeviceSize offs[2] = { 0, 0 };
		vkCmdBindVertexBuffers(cb, 0, 2, vbs, offs);
		vkCmdBindIndexBuffer(cb, ibuf, 0, VK_INDEX_TYPE_UINT16);

		Push push;
		float model[16];
		mat4_rotate_xyz(model, rx, ry, 0.0f);
		mat4_mul(push.mvp, vp, model);
		push.tint[0] = 0.75f + 0.25f * sinf(t);
		push.tint[1] = push.tint[2] = push.tint[3] = 1.0f;
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, push.mvp);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 64, 16, push.tint);
		vkCmdDrawIndexed(cb, 36, 9, 0, 0, 0);
		sample_end_frame(&s);
	}

	vkDeviceWaitIdle(s.device);
	vkDestroyPipeline(s.device, solid, NULL);
	vkDestroyPipeline(s.device, wire, NULL);
	vkDestroyPipelineLayout(s.device, layout, NULL);
	vkDestroyDescriptorPool(s.device, dpool, NULL);
	vkDestroyDescriptorSetLayout(s.device, setLayout, NULL);
	vkDestroySampler(s.device, sampler, NULL);
	vkDestroyImageView(s.device, view, NULL);
	vkDestroyImage(s.device, image, NULL);
	vkFreeMemory(s.device, imageMem, NULL);
	vkDestroyBuffer(s.device, vbuf, NULL);
	vkDestroyBuffer(s.device, ibuf, NULL);
	vkDestroyBuffer(s.device, obuf, NULL);
	vkFreeMemory(s.device, vmem, NULL);
	vkFreeMemory(s.device, imem, NULL);
	vkFreeMemory(s.device, omem, NULL);
	sample_shutdown(&s);
	sceKernelExitProcess(0);
	return 0;
}
