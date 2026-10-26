/*
 * VkVita sample: hello_triangle
 *
 * A complete, explicit Vulkan program (no helpers): instance, device,
 * swapchain, render pass, pipeline, command buffers, fences, present.
 * The only Vita-specific parts are the surface and the shaders, which are
 * Cg source compiled at runtime (needs ur0:data/libshacccg.suprx).
 *
 * Controls: START quits. The triangle spins via a push constant.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <vulkan/vulkan.h>
#include <vkvita/vkvita.h>

#define FRAMES 2
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	vkvLog(VKV_LOG_ERROR, "%s -> %d (line %d)", #x, (int)r_, __LINE__); sceKernelExitProcess(0); } } while (0)

/* Vertex inputs are matched to Vulkan locations in declaration order:
 * aPosition = location 0, aColor = location 1. uAngle is fed from the
 * push constant block (offset 0). */
static const char *k_vertexCg =
	"void main(float2 aPosition, float3 aColor, uniform float uAngle,\n"
	"          float4 out vPosition : POSITION, float4 out vColor : COLOR)\n"
	"{\n"
	"    float s = sin(uAngle), c = cos(uAngle);\n"
	"    float2 p = float2(aPosition.x * c - aPosition.y * s, aPosition.x * s + aPosition.y * c);\n"
	"    vPosition = float4(p.x * 0.57f, p.y, 0.0f, 1.0f);\n" /* 0.57 = 544/960 aspect fix */
	"    vColor = float4(aColor, 1.0f);\n"
	"}\n";

static const char *k_fragmentCg =
	"float4 main(float4 vColor : COLOR) : COLOR\n"
	"{\n"
	"    return vColor;\n"
	"}\n";

typedef struct { float pos[2]; float color[3]; } Vertex;
static const Vertex k_vertices[3] = {
	{ {  0.0f, -0.6f }, { 1.0f, 0.2f, 0.2f } },  /* Vulkan clip space: -Y is up */
	{ {  0.6f,  0.5f }, { 0.2f, 1.0f, 0.2f } },
	{ { -0.6f,  0.5f }, { 0.2f, 0.4f, 1.0f } },
};

int main(void) {
	/* Optional: turn on logging. Everything here can also be set from
	 * ux0:data/vkvita/config.txt without recompiling. */
	VkvConfig cfg;
	vkvGetDefaultConfig(&cfg);
	cfg.debugFlags |= VKV_DEBUG_LOG_INFO | VKV_DEBUG_LOG_SHADERS | VKV_DEBUG_VALIDATION;
	cfg.logPath = "ux0:data/vkvita/hello_triangle.log";
	vkvSetConfig(&cfg);

	/* Instance + device ------------------------------------------------- */
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "hello_triangle";
	app.apiVersion = VK_API_VERSION_1_0;
	const char *instExts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = 2;
	ici.ppEnabledExtensionNames = instExts;
	VkInstance instance;
	CHECK(vkCreateInstance(&ici, NULL, &instance));

	uint32_t count = 1;
	VkPhysicalDevice phys;
	CHECK(vkEnumeratePhysicalDevices(instance, &count, &phys));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(phys, &props);
	vkvLog(VKV_LOG_INFO, "GPU: %s", props.deviceName);

	/* Standard headless surface = the Vita display. */
	VkHeadlessSurfaceCreateInfoEXT hsi = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
	VkSurfaceKHR surface;
	CHECK(vkCreateHeadlessSurfaceEXT(instance, &hsi, NULL, &surface));

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = devExts;
	VkDevice device;
	CHECK(vkCreateDevice(phys, &dci, NULL, &device));
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);

	/* Swapchain -------------------------------------------------------- */
	VkSurfaceCapabilitiesKHR caps;
	CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
	VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	sci.surface = surface;
	sci.minImageCount = 3;
	sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM; /* the usual desktop choice also works */
	sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	sci.imageExtent = caps.currentExtent;
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.preTransform = caps.currentTransform;
	sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	VkSwapchainKHR swapchain;
	CHECK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain));
	uint32_t imageCount = 4;
	VkImage images[4];
	CHECK(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images));

	/* Render pass + framebuffers --------------------------------------- */
	VkAttachmentDescription color = { 0 };
	color.format = sci.imageFormat;
	color.samples = VK_SAMPLE_COUNT_1_BIT;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { 0 };
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &colorRef;
	VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rpi.attachmentCount = 1;
	rpi.pAttachments = &color;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &sub;
	VkRenderPass renderPass;
	CHECK(vkCreateRenderPass(device, &rpi, NULL, &renderPass));

	VkImageView views[4];
	VkFramebuffer framebuffers[4];
	for (uint32_t i = 0; i < imageCount; i++) {
		VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vci.image = images[i];
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = sci.imageFormat;
		vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vci.subresourceRange.levelCount = 1;
		vci.subresourceRange.layerCount = 1;
		CHECK(vkCreateImageView(device, &vci, NULL, &views[i]));
		VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fci.renderPass = renderPass;
		fci.attachmentCount = 1;
		fci.pAttachments = &views[i];
		fci.width = caps.currentExtent.width;
		fci.height = caps.currentExtent.height;
		fci.layers = 1;
		CHECK(vkCreateFramebuffer(device, &fci, NULL, &framebuffers[i]));
	}

	/* Vertex buffer (host visible, written once) ------------------------ */
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bci.size = sizeof(k_vertices);
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VkBuffer vbuf;
	CHECK(vkCreateBuffer(device, &bci, NULL, &vbuf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, vbuf, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	uint32_t memType = 0;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((req.memoryTypeBits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { memType = i; break; }
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = memType;
	VkDeviceMemory vmem;
	CHECK(vkAllocateMemory(device, &mai, NULL, &vmem));
	CHECK(vkBindBufferMemory(device, vbuf, vmem, 0));
	void *mapped;
	CHECK(vkMapMemory(device, vmem, 0, sizeof(k_vertices), 0, &mapped));
	memcpy(mapped, k_vertices, sizeof(k_vertices));
	vkUnmapMemory(device, vmem);

	/* Shaders + pipeline ----------------------------------------------- */
	if (!vkvShaderCompilerAvailable(device))
		vkvLog(VKV_LOG_ERROR, "libshacccg.suprx missing: install it (see README) or ship GXP shaders");
	VkShaderModule vs, fs;
	CHECK(vkvCreateShaderModuleFromCg(device, k_vertexCg, "triangle_vs", &vs));
	CHECK(vkvCreateShaderModuleFromCg(device, k_fragmentCg, "triangle_fs", &fs));

	VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &push;
	VkPipelineLayout layout;
	CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
	};
	VkVertexInputBindingDescription binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attrs[2] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 2 },
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &binding;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = attrs;
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blend = { 0 };
	blend.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &blend;
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	gpci.stageCount = 2;
	gpci.pStages = stages;
	gpci.pVertexInputState = &vi;
	gpci.pInputAssemblyState = &ia;
	gpci.pViewportState = &vps;
	gpci.pRasterizationState = &rs;
	gpci.pMultisampleState = &ms;
	gpci.pColorBlendState = &cb;
	gpci.pDynamicState = &ds;
	gpci.layout = layout;
	gpci.renderPass = renderPass;
	VkPipeline pipeline;
	CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline));
	/* Allowed by Vulkan: modules can go once pipelines exist. */
	vkDestroyShaderModule(device, vs, NULL);
	vkDestroyShaderModule(device, fs, NULL);

	/* Per-frame command buffers + fences -------------------------------- */
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(device, &cpci, NULL, &pool));
	VkCommandBuffer cmds[FRAMES];
	VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cbai.commandPool = pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = FRAMES;
	CHECK(vkAllocateCommandBuffers(device, &cbai, cmds));
	VkFence fences[FRAMES];
	VkSemaphore acquired[FRAMES], rendered[FRAMES];
	for (int i = 0; i < FRAMES; i++) {
		VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT };
		CHECK(vkCreateFence(device, &fci, NULL, &fences[i]));
		VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		CHECK(vkCreateSemaphore(device, &sem, NULL, &acquired[i]));
		CHECK(vkCreateSemaphore(device, &sem, NULL, &rendered[i]));
	}

	/* Frame loop ------------------------------------------------------- */
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	float angle = 0.0f;
	for (uint32_t frame = 0;; frame = (frame + 1) % FRAMES) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_START) break;

		CHECK(vkWaitForFences(device, 1, &fences[frame], VK_TRUE, UINT64_MAX));
		CHECK(vkResetFences(device, 1, &fences[frame]));
		uint32_t imageIndex;
		CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[frame], VK_NULL_HANDLE, &imageIndex));

		VkCommandBuffer cmd = cmds[frame];
		CHECK(vkResetCommandBuffer(cmd, 0));
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		CHECK(vkBeginCommandBuffer(cmd, &bi));
		VkClearValue clear = { .color = { { 0.05f, 0.05f, 0.08f, 1.0f } } };
		VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		rbi.renderPass = renderPass;
		rbi.framebuffer = framebuffers[imageIndex];
		rbi.renderArea.extent = caps.currentExtent;
		rbi.clearValueCount = 1;
		rbi.pClearValues = &clear;
		vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport = { 0, 0, (float)caps.currentExtent.width, (float)caps.currentExtent.height, 0, 1 };
		VkRect2D scissor = { { 0, 0 }, caps.currentExtent };
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &offset);
		angle += 0.02f;
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &angle);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		CHECK(vkEndCommandBuffer(cmd));

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &acquired[frame];
		si.pWaitDstStageMask = &waitStage;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &rendered[frame];
		CHECK(vkQueueSubmit(queue, 1, &si, fences[frame]));

		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &rendered[frame];
		pi.swapchainCount = 1;
		pi.pSwapchains = &swapchain;
		pi.pImageIndices = &imageIndex;
		CHECK(vkQueuePresentKHR(queue, &pi));
	}

	/* Teardown --------------------------------------------------------- */
	vkDeviceWaitIdle(device);
	for (int i = 0; i < FRAMES; i++) {
		vkDestroyFence(device, fences[i], NULL);
		vkDestroySemaphore(device, acquired[i], NULL);
		vkDestroySemaphore(device, rendered[i], NULL);
	}
	vkDestroyCommandPool(device, pool, NULL);
	vkDestroyPipeline(device, pipeline, NULL);
	vkDestroyPipelineLayout(device, layout, NULL);
	vkDestroyBuffer(device, vbuf, NULL);
	vkFreeMemory(device, vmem, NULL);
	for (uint32_t i = 0; i < imageCount; i++) {
		vkDestroyFramebuffer(device, framebuffers[i], NULL);
		vkDestroyImageView(device, views[i], NULL);
	}
	vkDestroyRenderPass(device, renderPass, NULL);
	vkDestroySwapchainKHR(device, swapchain, NULL);
	vkDestroyDevice(device, NULL);
	vkDestroySurfaceKHR(instance, surface, NULL);
	vkDestroyInstance(instance, NULL);
	sceKernelExitProcess(0);
	return 0;
}
