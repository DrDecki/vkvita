/*
 * VkVita sample: msaa
 *
 * A spinning wheel of thin spokes behind a spinning cube, drawn twice per
 * frame: once into a multisampled render pass that resolves into the
 * swapchain image, then the left half again without multisampling on top.
 * The left half shows the jagged edges, the right half the smoothed ones,
 * with a white line between them.
 *
 * The multisampled pass is the usual Vulkan setup: a color attachment and a
 * depth attachment with 2 or 4 samples, and the swapchain image as the
 * resolve attachment. GXM renders the samples in its tile memory and
 * averages them when it writes the tiles back, so the multisampled color
 * image never holds more than the resolved pixels.
 *
 * Expected picture: the same scene on both sides, with stair steps along
 * the spokes and cube edges on the left and smooth edges on the right.
 *
 * Shaders are the ones of the subpasses sample (scene.vert, color.frag).
 *
 * Controls: CROSS switches between 4x and 2x, START quits.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "vkv_sample.h"
#include "../subpasses/scene_vert_spv.h"
#include "../subpasses/color_frag_spv.h"

typedef struct { float pos[3]; float color[3]; } Vertex;

#define SPOKES   36
#define V_CUBE   0
#define V_WHEEL  24
#define V_LINE   (V_WHEEL + SPOKES * 3)
#define V_COUNT  (V_LINE + 4)
#define I_CUBE   0
#define I_WHEEL  36
#define I_LINE   (I_WHEEL + SPOKES * 3)
#define I_COUNT  (I_LINE + 6)

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
			Vertex *p = &v[V_CUBE + f * 4 + i];
			for (int k = 0; k < 3; k++) p->pos[k] = 0.5f * (n[f][k] + a * u[k] + b * w[k]);
			memcpy(p->color, col[f], sizeof(p->color));
		}
		uint16_t base = (uint16_t)(V_CUBE + f * 4);
		uint16_t *q = &idx[I_CUBE + f * 6];
		q[0] = base; q[1] = base + 1; q[2] = base + 2;
		q[3] = base; q[4] = base + 2; q[5] = base + 3;
	}
}

/* thin wedges from the middle out, far behind the cube */
static void build_wheel(Vertex *v, uint16_t *idx) {
	for (int i = 0; i < SPOKES; i++) {
		float a = (float)i * 6.2831853f / SPOKES, half = 0.012f;
		Vertex *p = &v[V_WHEEL + i * 3];
		float c = (i & 1) ? 1.0f : 0.55f;
		p[0].pos[0] = 0.0f;
		p[0].pos[1] = 0.0f;
		p[1].pos[0] = 0.95f * cosf(a - half);
		p[1].pos[1] = 0.95f * sinf(a - half);
		p[2].pos[0] = 0.95f * cosf(a + half);
		p[2].pos[1] = 0.95f * sinf(a + half);
		for (int k = 0; k < 3; k++) {
			p[k].pos[2] = 0.995f;
			p[k].color[0] = c;
			p[k].color[1] = c;
			p[k].color[2] = (i & 1) ? 0.6f : 1.0f;
			idx[I_WHEEL + i * 3 + k] = (uint16_t)(V_WHEEL + i * 3 + k);
		}
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

static void make_image(Sample *s, VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem, VkImageView *view) {
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = (VkExtent3D){ s->extent.width, s->extent.height, 1 };
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = samples;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	VK_CHECK(vkCreateImage(s->device, &ii, NULL, img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s->device, *img, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(s->device, &ai, NULL, mem));
	VK_CHECK(vkBindImageMemory(s->device, *img, *mem, 0));
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vci.image = *img;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format;
	vci.subresourceRange = (VkImageSubresourceRange){ aspect, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView(s->device, &vci, NULL, view));
}

/* Everything that depends on the sample count. */
typedef struct {
	VkSampleCountFlagBits samples;
	VkImage        colorImg, depthImg;
	VkDeviceMemory colorMem, depthMem;
	VkImageView    colorView, depthView;
	VkRenderPass   rp;
	VkFramebuffer  fbs[SAMPLE_MAX_IMAGES];
	VkPipeline     cube, flat;
} Mode;

static VkPipeline make_pipeline(Sample *s, VkRenderPass rp, VkSampleCountFlagBits samples, VkPipelineLayout layout,
                                VkShaderModule vs, VkShaderModule fs, bool cull, bool depth) {
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
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
	rs.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo msi = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	msi.rasterizationSamples = samples;
	VkPipelineDepthStencilStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	dsi.depthTestEnable = depth;
	dsi.depthWriteEnable = depth;
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
	gp.renderPass = rp;
	VkPipeline p;
	VK_CHECK(vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gp, NULL, &p));
	return p;
}

static void make_mode(Sample *s, Mode *m, VkSampleCountFlagBits samples, VkPipelineLayout layout, VkShaderModule vs,
                      VkShaderModule fs) {
	m->samples = samples;
	make_image(s, s->format, samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
	           VK_IMAGE_ASPECT_COLOR_BIT, &m->colorImg, &m->colorMem, &m->colorView);
	make_image(s, VK_FORMAT_D24_UNORM_S8_UINT, samples,
	           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
	           VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &m->depthImg, &m->depthMem, &m->depthView);

	VkAttachmentDescription att[3];
	memset(att, 0, sizeof(att));
	att[0].format = s->format;
	att[0].samples = samples;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att[1] = att[0];
	att[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
	att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	att[2] = att[0];
	att[2].samples = VK_SAMPLE_COUNT_1_BIT;
	att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentReference resolve = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub;
	memset(&sub, 0, sizeof(sub));
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &color;
	sub.pResolveAttachments = &resolve;
	sub.pDepthStencilAttachment = &depth;
	VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rpci.attachmentCount = 3;
	rpci.pAttachments = att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	VK_CHECK(vkCreateRenderPass(s->device, &rpci, NULL, &m->rp));
	for (uint32_t i = 0; i < s->imageCount; i++) {
		VkImageView views[3] = { m->colorView, m->depthView, s->views[i] };
		VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fci.renderPass = m->rp;
		fci.attachmentCount = 3;
		fci.pAttachments = views;
		fci.width = s->extent.width;
		fci.height = s->extent.height;
		fci.layers = 1;
		VK_CHECK(vkCreateFramebuffer(s->device, &fci, NULL, &m->fbs[i]));
	}
	m->cube = make_pipeline(s, m->rp, samples, layout, vs, fs, true, true);
	m->flat = make_pipeline(s, m->rp, samples, layout, vs, fs, false, true);
}

static void destroy_mode(Sample *s, Mode *m) {
	vkDestroyPipeline(s->device, m->cube, NULL);
	vkDestroyPipeline(s->device, m->flat, NULL);
	for (uint32_t i = 0; i < s->imageCount; i++) vkDestroyFramebuffer(s->device, m->fbs[i], NULL);
	vkDestroyRenderPass(s->device, m->rp, NULL);
	vkDestroyImageView(s->device, m->colorView, NULL);
	vkDestroyImageView(s->device, m->depthView, NULL);
	vkDestroyImage(s->device, m->colorImg, NULL);
	vkDestroyImage(s->device, m->depthImg, NULL);
	vkFreeMemory(s->device, m->colorMem, NULL);
	vkFreeMemory(s->device, m->depthMem, NULL);
}

static void draw_scene(VkCommandBuffer cb, VkPipelineLayout layout, VkPipeline cube, VkPipeline flat,
                       const float wheel[16], const float cubeMvp[16]) {
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, flat);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, wheel);
	vkCmdDrawIndexed(cb, SPOKES * 3, 1, I_WHEEL, 0, 0);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cube);
	vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, cubeMvp);
	vkCmdDrawIndexed(cb, 36, 1, I_CUBE, 0, 0);
}

int main(void) {
	sample_configure("msaa");
	Sample s;
	sample_init(&s, "msaa", true);
	const uint32_t W = s.extent.width, H = s.extent.height;

	/* --- geometry ------------------------------------------------------------ */
	Vertex verts[V_COUNT];
	uint16_t indices[I_COUNT];
	memset(verts, 0, sizeof(verts));
	build_cube(verts, indices);
	build_wheel(verts, indices);
	/* divider, in pixels mapped to clip space below */
	float lx = 1.5f / (float)W;
	const float lxs[4] = { -lx, lx, lx, -lx }, lys[4] = { -1, -1, 1, 1 };
	for (int i = 0; i < 4; i++) {
		Vertex *v = &verts[V_LINE + i];
		v->pos[0] = lxs[i];
		v->pos[1] = lys[i];
		v->pos[2] = 0.0f;
		v->color[0] = v->color[1] = v->color[2] = 1.0f;
	}
	uint16_t *q = &indices[I_LINE];
	q[0] = V_LINE; q[1] = V_LINE + 1; q[2] = V_LINE + 2;
	q[3] = V_LINE; q[4] = V_LINE + 2; q[5] = V_LINE + 3;
	VkBuffer vbuf, ibuf;
	VkDeviceMemory vmem, imem;
	void *p;
	sample_create_buffer(&s, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &vbuf, &vmem, &p);
	memcpy(p, verts, sizeof(verts));
	sample_create_buffer(&s, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &ibuf, &imem, &p);
	memcpy(p, indices, sizeof(indices));

	/* --- what the device offers ---------------------------------------------- */
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(s.phys, &props);
	VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
	if (!(counts & VK_SAMPLE_COUNT_4_BIT) || !(counts & VK_SAMPLE_COUNT_2_BIT)) sample_fatal("no 2x and 4x MSAA");

	VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(s.device, &plci, NULL, &layout));
	VkShaderModule vs = make_module(&s, k_scene_vert_spv, sizeof(k_scene_vert_spv));
	VkShaderModule fs = make_module(&s, k_color_frag_spv, sizeof(k_color_frag_spv));

	Mode modes[2];
	make_mode(&s, &modes[0], VK_SAMPLE_COUNT_4_BIT, layout, vs, fs);
	make_mode(&s, &modes[1], VK_SAMPLE_COUNT_2_BIT, layout, vs, fs);

	/* --- the single-sampled pass for the left half --------------------------- */
	VkAttachmentDescription att[2];
	memset(att, 0, sizeof(att));
	att[0].format = s.format;
	att[0].samples = VK_SAMPLE_COUNT_1_BIT;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	att[1] = att[0];
	att[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
	att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub;
	memset(&sub, 0, sizeof(sub));
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &color;
	sub.pDepthStencilAttachment = &depth;
	VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rpci.attachmentCount = 2;
	rpci.pAttachments = att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	VkRenderPass plainRp;
	VK_CHECK(vkCreateRenderPass(s.device, &rpci, NULL, &plainRp));
	VkFramebuffer plainFbs[SAMPLE_MAX_IMAGES];
	for (uint32_t i = 0; i < s.imageCount; i++) {
		VkImageView views[2] = { s.views[i], s.depthView };
		VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fci.renderPass = plainRp;
		fci.attachmentCount = 2;
		fci.pAttachments = views;
		fci.width = W;
		fci.height = H;
		fci.layers = 1;
		VK_CHECK(vkCreateFramebuffer(s.device, &fci, NULL, &plainFbs[i]));
	}
	VkPipeline plainCube = make_pipeline(&s, plainRp, VK_SAMPLE_COUNT_1_BIT, layout, vs, fs, true, true);
	VkPipeline plainFlat = make_pipeline(&s, plainRp, VK_SAMPLE_COUNT_1_BIT, layout, vs, fs, false, true);
	vkDestroyShaderModule(s.device, vs, NULL);
	vkDestroyShaderModule(s.device, fs, NULL);

	/* --- loop ---------------------------------------------------------------- */
	float proj[16], back[16], vp[16], ident[16];
	mat4_perspective(proj, 1.0f, (float)W / (float)H, 0.1f, 50.0f);
	mat4_translate(back, 0.0f, 0.0f, -2.6f);
	mat4_mul(vp, proj, back);
	mat4_identity(ident);
	float t = 0.0f;
	uint32_t frameNo = 0, mode = 0;
	SceUInt64 lastFrame = sceKernelGetProcessTimeWide(), worstFrame = 0, sumFrames = 0;
	vkvLog(VKV_LOG_WARN, "msaa: right half %ux", (unsigned)modes[mode].samples);

	while (sample_poll_input(&s)) {
		if (s.pressed & SCE_CTRL_CROSS) {
			mode ^= 1;
			vkvLog(VKV_LOG_WARN, "msaa: right half %ux", (unsigned)modes[mode].samples);
		}
		t += 1.0f / 60.0f;
		SceUInt64 now = sceKernelGetProcessTimeWide();
		if (frameNo) {
			if (now - lastFrame > worstFrame) worstFrame = now - lastFrame;
			sumFrames += now - lastFrame;
		}
		lastFrame = now;
		if (frameNo && frameNo % 300 == 0) {
			vkvLog(VKV_LOG_WARN, "msaa: %ux, frames average %.1f ms, longest %.1f ms", (unsigned)modes[mode].samples,
			       sumFrames / 300000.0f, worstFrame / 1000.0f);
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

		float rot[16], aspect[16], wheel[16], model[16], cubeMvp[16];
		mat4_rotate_xyz(rot, 0.0f, 0.0f, t * 0.25f);
		mat4_identity(aspect);
		aspect[0] = (float)H / (float)W;
		mat4_mul(wheel, aspect, rot);
		mat4_rotate_xyz(model, t * 0.6f, t, 0.0f);
		mat4_mul(cubeMvp, vp, model);
		VkViewport v = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
		VkRect2D full = { { 0, 0 }, s.extent };
		VkDeviceSize off = 0;

		/* the whole screen multisampled, resolved into the swapchain image */
		Mode *m = &modes[mode];
		VkClearValue clears[3];
		memset(clears, 0, sizeof(clears));
		clears[0].color = (VkClearColorValue){ { 0.06f, 0.07f, 0.12f, 1.0f } };
		clears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
		VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		rbi.renderPass = m->rp;
		rbi.framebuffer = m->fbs[s.imageIndex];
		rbi.renderArea.extent = s.extent;
		rbi.clearValueCount = 3;
		rbi.pClearValues = clears;
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdSetViewport(cb, 0, 1, &v);
		vkCmdSetScissor(cb, 0, 1, &full);
		vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
		vkCmdBindIndexBuffer(cb, ibuf, 0, VK_INDEX_TYPE_UINT16);
		draw_scene(cb, layout, m->cube, m->flat, wheel, cubeMvp);
		vkCmdEndRenderPass(cb);

		/* the left half again without multisampling */
		VkClearValue plainClears[2];
		memset(plainClears, 0, sizeof(plainClears));
		plainClears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
		rbi.renderPass = plainRp;
		rbi.framebuffer = plainFbs[s.imageIndex];
		rbi.clearValueCount = 2;
		rbi.pClearValues = plainClears;
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdSetViewport(cb, 0, 1, &v);
		VkRect2D left = { { 0, 0 }, { W / 2, H } };
		VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = clears[0].color } };
		VkClearRect cr = { left, 0, 1 };
		vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
		vkCmdSetScissor(cb, 0, 1, &left);
		vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
		vkCmdBindIndexBuffer(cb, ibuf, 0, VK_INDEX_TYPE_UINT16);
		draw_scene(cb, layout, plainCube, plainFlat, wheel, cubeMvp);
		vkCmdSetScissor(cb, 0, 1, &full);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, plainFlat);
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, ident);
		vkCmdDrawIndexed(cb, 6, 1, I_LINE, 0, 0);
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
	destroy_mode(&s, &modes[0]);
	destroy_mode(&s, &modes[1]);
	vkDestroyPipeline(s.device, plainCube, NULL);
	vkDestroyPipeline(s.device, plainFlat, NULL);
	for (uint32_t i = 0; i < s.imageCount; i++) vkDestroyFramebuffer(s.device, plainFbs[i], NULL);
	vkDestroyRenderPass(s.device, plainRp, NULL);
	vkDestroyPipelineLayout(s.device, layout, NULL);
	vkDestroyBuffer(s.device, vbuf, NULL);
	vkDestroyBuffer(s.device, ibuf, NULL);
	vkFreeMemory(s.device, vmem, NULL);
	vkFreeMemory(s.device, imem, NULL);
	sample_shutdown(&s);
	sceKernelExitProcess(0);
	return 0;
}
