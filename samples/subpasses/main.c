/*
 * VkVita sample: subpasses
 *
 * One render pass with three subpasses:
 *   0  a spinning cube with depth into an offscreen color attachment, over a
 *      blue background with a one pixel black and white checker on the left
 *   1  a full screen pass that reads that attachment through subpassLoad and
 *      writes the swapchain image: the left strip turns green where every
 *      pixel came back exactly (red would mean it read a neighbor), the
 *      middle shows the scene as it is and the right side inverted
 *   2  a yellow square in the top right corner, drawn into the same swapchain
 *      attachment without inputs, so it continues the scene of subpass 1
 *
 * Expected picture: green strip, normal cube in the middle that turns into
 * its inverted colors on the right, yellow square top right.
 *
 * Regenerate the shader headers after editing shaders/:
 *   glslangValidator -V --vn k_scene_vert_spv shaders/scene.vert -o scene_vert_spv.h
 *   (same for background.frag, color.frag and post.frag)
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
#include "background_frag_spv.h"
#include "color_frag_spv.h"
#include "post_frag_spv.h"

typedef struct { float pos[3]; float color[3]; } Vertex;

/* vertices: background quad (0), cube (4), screen quad (28), yellow square (32) */
#define V_BG     0
#define V_CUBE   4
#define V_SCREEN 28
#define V_HUD    32
#define V_COUNT  36

static void quad(Vertex *v, float x0, float y0, float x1, float y1, const float top[3], const float bottom[3]) {
	const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
	for (int i = 0; i < 4; i++) {
		v[i].pos[0] = xs[i];
		v[i].pos[1] = ys[i];
		v[i].pos[2] = 0.5f;
		memcpy(v[i].color, i < 2 ? top : bottom, sizeof(v[i].color));
	}
}

static void build_cube(Vertex *v, uint16_t *idx) {
	static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
	static const float col[6][3] = { { 0.95f, 0.35f, 0.3f }, { 0.3f, 0.85f, 0.4f }, { 0.35f, 0.5f, 0.95f },
	                                 { 0.95f, 0.85f, 0.3f }, { 0.3f, 0.9f, 0.9f }, { 0.9f, 0.4f, 0.9f } };
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
			memcpy(p->color, col[f], sizeof(p->color));
		}
		uint16_t base = (uint16_t)(V_CUBE + f * 4); /* v already points at V_CUBE */
		uint16_t *q = &idx[f * 6];
		q[0] = base; q[1] = base + 1; q[2] = base + 2;
		q[3] = base; q[4] = base + 2; q[5] = base + 3;
	}
}

static VkShaderModule make_module(Sample *s, const uint32_t *code, size_t size) {
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	ci.codeSize = size;
	ci.pCode = code;
	VkShaderModule m;
	VK_CHECK(vkCreateShaderModule(s->device, &ci, NULL, &m));
	return m;
}

int main(void) {
	sample_configure("subpasses");
	Sample s;
	sample_init(&s, "subpasses", true);
	const uint32_t W = s.extent.width, H = s.extent.height;

	/* --- geometry --------------------------------------------------------- */
	Vertex verts[V_COUNT];
	uint16_t indices[36 + 6 * 3];
	static const float skyTop[3] = { 0.12f, 0.2f, 0.45f }, skyBottom[3] = { 0.04f, 0.06f, 0.14f };
	static const float white[3] = { 1, 1, 1 }, yellow[3] = { 1.0f, 0.85f, 0.1f };
	memset(verts, 0, sizeof(verts));
	quad(&verts[V_BG], -1.0f, -1.0f, 1.0f, 1.0f, skyTop, skyBottom);
	build_cube(&verts[V_CUBE], indices);
	quad(&verts[V_SCREEN], -1.0f, -1.0f, 1.0f, 1.0f, white, white);
	quad(&verts[V_HUD], 0.72f, -0.9f, 0.9f, -0.58f, yellow, yellow);
	for (int q = 0; q < 3; q++) {
		static const uint16_t bases[3] = { V_BG, V_SCREEN, V_HUD };
		uint16_t *d = &indices[36 + q * 6], b = bases[q];
		d[0] = b; d[1] = b + 1; d[2] = b + 2;
		d[3] = b; d[4] = b + 2; d[5] = b + 3;
	}
	VkBuffer vbuf, ibuf;
	VkDeviceMemory vmem, imem;
	void *p;
	sample_create_buffer(&s, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &vbuf, &vmem, &p);
	memcpy(p, verts, sizeof(verts));
	sample_create_buffer(&s, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &ibuf, &imem, &p);
	memcpy(p, indices, sizeof(indices));

	/* --- offscreen color attachment, read back in subpass 1 ---------------- */
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM;
	ii.extent = (VkExtent3D){ W, H, 1 };
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
	VkImage offImg;
	VK_CHECK(vkCreateImage(s.device, &ii, NULL, &offImg));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s.device, offImg, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(&s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory offMem;
	VK_CHECK(vkAllocateMemory(s.device, &ai, NULL, &offMem));
	VK_CHECK(vkBindImageMemory(s.device, offImg, offMem, 0));
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vci.image = offImg;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ii.format;
	vci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageView offView;
	VK_CHECK(vkCreateImageView(s.device, &vci, NULL, &offView));

	/* --- render pass with three subpasses ---------------------------------- */
	VkAttachmentDescription att[3];
	memset(att, 0, sizeof(att));
	att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	att[0].samples = VK_SAMPLE_COUNT_1_BIT;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	att[1] = att[0];
	att[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
	att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	att[2] = att[0];
	att[2].format = s.format;
	att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[2].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference offColor = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentReference screen = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference offInput = { 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkSubpassDescription sub[3];
	memset(sub, 0, sizeof(sub));
	for (int i = 0; i < 3; i++) sub[i].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub[0].colorAttachmentCount = 1;
	sub[0].pColorAttachments = &offColor;
	sub[0].pDepthStencilAttachment = &depth;
	sub[1].colorAttachmentCount = 1;
	sub[1].pColorAttachments = &screen;
	sub[1].inputAttachmentCount = 1;
	sub[1].pInputAttachments = &offInput;
	sub[2].colorAttachmentCount = 1;
	sub[2].pColorAttachments = &screen;
	VkSubpassDependency deps[2];
	memset(deps, 0, sizeof(deps));
	deps[0].srcSubpass = 0;
	deps[0].dstSubpass = 1;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[0].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
	deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	deps[1].srcSubpass = 1;
	deps[1].dstSubpass = 2;
	deps[1].srcStageMask = deps[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].srcAccessMask = deps[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rpci.attachmentCount = 3;
	rpci.pAttachments = att;
	rpci.subpassCount = 3;
	rpci.pSubpasses = sub;
	rpci.dependencyCount = 2;
	rpci.pDependencies = deps;
	VkRenderPass rp;
	VK_CHECK(vkCreateRenderPass(s.device, &rpci, NULL, &rp));

	VkFramebuffer fbs[SAMPLE_MAX_IMAGES];
	for (uint32_t i = 0; i < s.imageCount; i++) {
		VkImageView views[3] = { offView, s.depthView, s.views[i] };
		VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fci.renderPass = rp;
		fci.attachmentCount = 3;
		fci.pAttachments = views;
		fci.width = W;
		fci.height = H;
		fci.layers = 1;
		VK_CHECK(vkCreateFramebuffer(s.device, &fci, NULL, &fbs[i]));
	}

	/* --- descriptors: the input attachment ---------------------------------- */
	VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dlci.bindingCount = 1;
	dlci.pBindings = &lb;
	VkDescriptorSetLayout setLayout;
	VK_CHECK(vkCreateDescriptorSetLayout(s.device, &dlci, NULL, &setLayout));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1 };
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
	VkDescriptorImageInfo dii = { VK_NULL_HANDLE, offView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	wr.dstSet = set;
	wr.dstBinding = 0;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
	wr.pImageInfo = &dii;
	vkUpdateDescriptorSets(s.device, 1, &wr, 0, NULL);

	/* --- pipelines ---------------------------------------------------------- */
	VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(s.device, &plci, NULL, &layout));

	VkShaderModule vs = make_module(&s, k_scene_vert_spv, sizeof(k_scene_vert_spv));
	VkShaderModule fsBg = make_module(&s, k_background_frag_spv, sizeof(k_background_frag_spv));
	VkShaderModule fsColor = make_module(&s, k_color_frag_spv, sizeof(k_color_frag_spv));
	VkShaderModule fsPost = make_module(&s, k_post_frag_spv, sizeof(k_post_frag_spv));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsBg, "main", NULL },
	};
	VkVertexInputBindingDescription binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attrs[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 },
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
	gp.renderPass = rp;

	VkPipeline bgPipe, cubePipe, postPipe, hudPipe;
	gp.subpass = 0; /* background: no depth test */
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &bgPipe));
	stages[1].module = fsColor; /* cube: depth tested, back faces culled */
	dsi.depthTestEnable = VK_TRUE;
	dsi.depthWriteEnable = VK_TRUE;
	dsi.depthCompareOp = VK_COMPARE_OP_LESS;
	rs.cullMode = VK_CULL_MODE_BACK_BIT;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &cubePipe));
	memset(&dsi, 0, sizeof(dsi));
	dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	rs.cullMode = VK_CULL_MODE_NONE;
	stages[1].module = fsPost;
	gp.subpass = 1;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &postPipe));
	stages[1].module = fsColor;
	gp.subpass = 2;
	VK_CHECK(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gp, NULL, &hudPipe));
	vkDestroyShaderModule(s.device, vs, NULL);
	vkDestroyShaderModule(s.device, fsBg, NULL);
	vkDestroyShaderModule(s.device, fsColor, NULL);
	vkDestroyShaderModule(s.device, fsPost, NULL);

	/* --- loop ---------------------------------------------------------------- */
	float proj[16], back[16], vp[16], ident[16];
	mat4_perspective(proj, 1.0f, (float)W / (float)H, 0.1f, 50.0f);
	mat4_translate(back, 0.0f, 0.0f, -2.6f);
	mat4_mul(vp, proj, back);
	mat4_identity(ident);
	float t = 0.0f;
	uint32_t frameNo = 0;
	SceUInt64 lastFrame = sceKernelGetProcessTimeWide(), worstFrame = 0, sumFrames = 0;

	while (sample_poll_input(&s)) {
		t += 1.0f / 60.0f;
		SceUInt64 now = sceKernelGetProcessTimeWide();
		if (frameNo) {
			if (now - lastFrame > worstFrame) worstFrame = now - lastFrame;
			sumFrames += now - lastFrame;
		}
		lastFrame = now;
		if (frameNo && frameNo % 300 == 0) {
			vkvLog(VKV_LOG_WARN, "subpasses: frames average %.1f ms, longest %.1f ms", sumFrames / 300000.0f,
			       worstFrame / 1000.0f);
			worstFrame = sumFrames = 0;
		}
		VK_CHECK(vkWaitForFences(s.device, 1, &s.fences[s.frame], VK_TRUE, UINT64_MAX));
		VK_CHECK(vkResetFences(s.device, 1, &s.fences[s.frame]));
		VK_CHECK(vkAcquireNextImageKHR(s.device, s.swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE,
		                               &s.imageIndex));
		VkCommandBuffer cb = s.cmd[s.frame];
		VK_CHECK(vkResetCommandBuffer(cb, 0));
		VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		VK_CHECK(vkBeginCommandBuffer(cb, &cbi));

		VkClearValue clears[3];
		memset(clears, 0, sizeof(clears));
		clears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
		VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		rbi.renderPass = rp;
		rbi.framebuffer = fbs[s.imageIndex];
		rbi.renderArea.extent = s.extent;
		rbi.clearValueCount = 3;
		rbi.pClearValues = clears;
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport v = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
		VkRect2D sc = { { 0, 0 }, s.extent };
		vkCmdSetViewport(cb, 0, 1, &v);
		vkCmdSetScissor(cb, 0, 1, &sc);
		VkDeviceSize off = 0;
		vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
		vkCmdBindIndexBuffer(cb, ibuf, 0, VK_INDEX_TYPE_UINT16);

		/* subpass 0: background and cube into the offscreen image */
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bgPipe);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
		vkCmdDrawIndexed(cb, 6, 1, 36, 0, 0);
		float model[16], mvp[16];
		mat4_rotate_xyz(model, t * 0.6f, t, 0.0f);
		mat4_mul(mvp, vp, model);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cubePipe);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
		vkCmdDrawIndexed(cb, 36, 1, 0, 0, 0);

		/* subpass 1: read it back into the swapchain image */
		vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
		vkCmdDrawIndexed(cb, 6, 1, 42, 0, 0);

		/* subpass 2: same attachment, no inputs */
		vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipe);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
		vkCmdDrawIndexed(cb, 6, 1, 48, 0, 0);

		vkCmdEndRenderPass(cb);
		VK_CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		VK_CHECK(vkQueueSubmit(s.queue, 1, &si, s.fences[s.frame]));
		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.swapchainCount = 1;
		pi.pSwapchains = &s.swapchain;
		pi.pImageIndices = &s.imageIndex;
		VK_CHECK(vkQueuePresentKHR(s.queue, &pi));
		s.frame = (s.frame + 1) % SAMPLE_FRAMES;
		frameNo++;
	}

	vkDeviceWaitIdle(s.device);
	vkDestroyPipeline(s.device, bgPipe, NULL);
	vkDestroyPipeline(s.device, cubePipe, NULL);
	vkDestroyPipeline(s.device, postPipe, NULL);
	vkDestroyPipeline(s.device, hudPipe, NULL);
	vkDestroyPipelineLayout(s.device, layout, NULL);
	vkDestroyDescriptorPool(s.device, dpool, NULL);
	vkDestroyDescriptorSetLayout(s.device, setLayout, NULL);
	for (uint32_t i = 0; i < s.imageCount; i++) vkDestroyFramebuffer(s.device, fbs[i], NULL);
	vkDestroyRenderPass(s.device, rp, NULL);
	vkDestroyImageView(s.device, offView, NULL);
	vkDestroyImage(s.device, offImg, NULL);
	vkFreeMemory(s.device, offMem, NULL);
	vkDestroyBuffer(s.device, vbuf, NULL);
	vkDestroyBuffer(s.device, ibuf, NULL);
	vkFreeMemory(s.device, vmem, NULL);
	vkFreeMemory(s.device, imem, NULL);
	sample_shutdown(&s);
	sceKernelExitProcess(0);
	return 0;
}
