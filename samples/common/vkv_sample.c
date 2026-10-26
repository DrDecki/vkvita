/*
 * Shared sample boilerplate. Plain Vulkan except for sample_configure().
 * SPDX-License-Identifier: MIT
 */
#include "vkv_sample.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

void sample_fatal(const char *what) {
	vkvLog(VKV_LOG_ERROR, "sample aborted: %s", what);
	sceKernelExitProcess(0);
	for (;;) {}
}

void sample_configure(const char *name) {
	static char logPath[96];
	snprintf(logPath, sizeof(logPath), "ux0:data/vkvita/%s.log", name);
	VkvConfig cfg;
	vkvGetDefaultConfig(&cfg);
	cfg.debugFlags = VKV_DEBUG_LOG_INFO | VKV_DEBUG_LOG_SHADERS | VKV_DEBUG_VALIDATION | VKV_DEBUG_PROFILE |
	                 VKV_DEBUG_LOG_UNSUPPORTED;
	cfg.compatFlags = VKV_COMPAT_SHADER_CACHE; /* 2nd launch loads the dumped GXP instead of compiling */
	cfg.logPath = logPath;
	cfg.profileInterval = 300;
	vkvSetConfig(&cfg);
}

uint32_t sample_memory_type(Sample *s, uint32_t typeBits, VkMemoryPropertyFlags props) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(s->phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
	sample_fatal("no suitable memory type");
}

void sample_create_buffer(Sample *s, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                          VkBuffer *buffer, VkDeviceMemory *memory, void **mapped) {
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = usage;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK(vkCreateBuffer(s->device, &bi, NULL, buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(s->device, *buffer, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(s, req.memoryTypeBits, props);
	VK_CHECK(vkAllocateMemory(s->device, &ai, NULL, memory));
	VK_CHECK(vkBindBufferMemory(s->device, *buffer, *memory, 0));
	if (mapped) VK_CHECK(vkMapMemory(s->device, *memory, 0, size, 0, mapped));
}

VkCommandBuffer sample_begin_once(Sample *s) {
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = s->cmdPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cb;
	VK_CHECK(vkAllocateCommandBuffers(s->device, &ai, &cb));
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

void sample_end_once(Sample *s, VkCommandBuffer cb) {
	VK_CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	VK_CHECK(vkQueueSubmit(s->queue, 1, &si, VK_NULL_HANDLE));
	VK_CHECK(vkQueueWaitIdle(s->queue));
	vkFreeCommandBuffers(s->device, s->cmdPool, 1, &cb);
}

static void sample_create_depth(Sample *s) {
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_D24_UNORM_S8_UINT;
	ii.extent = (VkExtent3D){ s->extent.width, s->extent.height, 1 };
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	VK_CHECK(vkCreateImage(s->device, &ii, NULL, &s->depthImage));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s->device, s->depthImage, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(s->device, &ai, NULL, &s->depthMemory));
	VK_CHECK(vkBindImageMemory(s->device, s->depthImage, s->depthMemory, 0));
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = s->depthImage;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = ii.format;
	vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView(s->device, &vi, NULL, &s->depthView));
}

bool sample_init(Sample *s, const char *appName, bool withDepth) {
	memset(s, 0, sizeof(*s));
	s->withDepth = withDepth;
	s->clearColor = (VkClearColorValue){ { 0.08f, 0.09f, 0.12f, 1.0f } };
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = appName;
	app.apiVersion = VK_API_VERSION_1_0;
	const char *exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = 2;
	ici.ppEnabledExtensionNames = exts;
	VK_CHECK(vkCreateInstance(&ici, NULL, &s->instance));

	uint32_t n = 1;
	VK_CHECK(vkEnumeratePhysicalDevices(s->instance, &n, &s->phys));
	VK_CHECK(vkvCreateDisplaySurface(s->instance, &s->surface));

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qi.queueFamilyIndex = 0;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	const char *devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qi;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = devExts;
	VK_CHECK(vkCreateDevice(s->phys, &dci, NULL, &s->device));
	vkGetDeviceQueue(s->device, 0, 0, &s->queue);

	VkSurfaceCapabilitiesKHR caps;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->phys, s->surface, &caps));
	s->extent = caps.currentExtent;
	s->format = VK_FORMAT_R8G8B8A8_UNORM;
	VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	sci.surface = s->surface;
	sci.minImageCount = 3;
	sci.imageFormat = s->format;
	sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	sci.imageExtent = s->extent;
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sci.clipped = VK_TRUE;
	VK_CHECK(vkCreateSwapchainKHR(s->device, &sci, NULL, &s->swapchain));
	s->imageCount = SAMPLE_MAX_IMAGES;
	VK_CHECK(vkGetSwapchainImagesKHR(s->device, s->swapchain, &s->imageCount, s->images));
	for (uint32_t i = 0; i < s->imageCount; i++) {
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = s->images[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = s->format;
		vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView(s->device, &vi, NULL, &s->views[i]));
	}
	if (withDepth) sample_create_depth(s);

	VkAttachmentDescription att[2];
	memset(att, 0, sizeof(att));
	att[0].format = s->format;
	att[0].samples = VK_SAMPLE_COUNT_1_BIT;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	att[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
	att[1].samples = VK_SAMPLE_COUNT_1_BIT;
	att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub;
	memset(&sub, 0, sizeof(sub));
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &colorRef;
	sub.pDepthStencilAttachment = withDepth ? &depthRef : NULL;
	VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	rpi.attachmentCount = withDepth ? 2 : 1;
	rpi.pAttachments = att;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &sub;
	VK_CHECK(vkCreateRenderPass(s->device, &rpi, NULL, &s->renderPass));

	for (uint32_t i = 0; i < s->imageCount; i++) {
		VkImageView views[2] = { s->views[i], s->depthView };
		VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		fi.renderPass = s->renderPass;
		fi.attachmentCount = withDepth ? 2 : 1;
		fi.pAttachments = views;
		fi.width = s->extent.width;
		fi.height = s->extent.height;
		fi.layers = 1;
		VK_CHECK(vkCreateFramebuffer(s->device, &fi, NULL, &s->framebuffers[i]));
	}

	VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pi.queueFamilyIndex = 0;
	VK_CHECK(vkCreateCommandPool(s->device, &pi, NULL, &s->cmdPool));
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = s->cmdPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = SAMPLE_FRAMES;
	VK_CHECK(vkAllocateCommandBuffers(s->device, &ai, s->cmd));
	for (int i = 0; i < SAMPLE_FRAMES; i++) {
		VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		VK_CHECK(vkCreateFence(s->device, &fci, NULL, &s->fences[i]));
	}
	return true;
}

VkCommandBuffer sample_begin_frame(Sample *s) {
	VK_CHECK(vkWaitForFences(s->device, 1, &s->fences[s->frame], VK_TRUE, UINT64_MAX));
	VK_CHECK(vkResetFences(s->device, 1, &s->fences[s->frame]));
	VK_CHECK(vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE,
	                               &s->imageIndex));
	VkCommandBuffer cb = s->cmd[s->frame];
	VK_CHECK(vkResetCommandBuffer(cb, 0));
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VK_CHECK(vkBeginCommandBuffer(cb, &bi));
	VkClearValue clears[2];
	clears[0].color = s->clearColor;
	clears[1].depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
	VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	rbi.renderPass = s->renderPass;
	rbi.framebuffer = s->framebuffers[s->imageIndex];
	rbi.renderArea.extent = s->extent;
	rbi.clearValueCount = s->withDepth ? 2 : 1;
	rbi.pClearValues = clears;
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport vp = { 0, 0, (float)s->extent.width, (float)s->extent.height, 0.0f, 1.0f };
	VkRect2D sc = { { 0, 0 }, s->extent };
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	return cb;
}

void sample_end_frame(Sample *s) {
	VkCommandBuffer cb = s->cmd[s->frame];
	vkCmdEndRenderPass(cb);
	VK_CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	VK_CHECK(vkQueueSubmit(s->queue, 1, &si, s->fences[s->frame]));
	VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	pi.swapchainCount = 1;
	pi.pSwapchains = &s->swapchain;
	pi.pImageIndices = &s->imageIndex;
	VK_CHECK(vkQueuePresentKHR(s->queue, &pi));
	s->frame = (s->frame + 1) % SAMPLE_FRAMES;
}

bool sample_poll_input(Sample *s) {
	SceCtrlData pad;
	memset(&pad, 0, sizeof(pad));
	sceCtrlPeekBufferPositive(0, &pad, 1);
	s->pressed = pad.buttons & ~s->buttons;
	s->buttons = pad.buttons;
	s->lx = (int)pad.lx - 128;
	s->ly = (int)pad.ly - 128;
	if (s->lx > -16 && s->lx < 16) s->lx = 0;
	if (s->ly > -16 && s->ly < 16) s->ly = 0;
	return !(s->pressed & SCE_CTRL_START);
}

void sample_shutdown(Sample *s) {
	vkDeviceWaitIdle(s->device);
	for (int i = 0; i < SAMPLE_FRAMES; i++) vkDestroyFence(s->device, s->fences[i], NULL);
	vkDestroyCommandPool(s->device, s->cmdPool, NULL);
	for (uint32_t i = 0; i < s->imageCount; i++) {
		vkDestroyFramebuffer(s->device, s->framebuffers[i], NULL);
		vkDestroyImageView(s->device, s->views[i], NULL);
	}
	vkDestroyRenderPass(s->device, s->renderPass, NULL);
	if (s->withDepth) {
		vkDestroyImageView(s->device, s->depthView, NULL);
		vkDestroyImage(s->device, s->depthImage, NULL);
		vkFreeMemory(s->device, s->depthMemory, NULL);
	}
	vkDestroySwapchainKHR(s->device, s->swapchain, NULL);
	vkDestroyDevice(s->device, NULL);
	vkDestroySurfaceKHR(s->instance, s->surface, NULL);
	vkDestroyInstance(s->instance, NULL);
}

/* ------------------------------------------------------------------------ */
void mat4_identity(float m[16]) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_mul(float out[16], const float a[16], const float b[16]) {
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++)
			r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] +
			                 a[12 + row] * b[c * 4 + 3];
	memcpy(out, r, sizeof(r));
}

void mat4_perspective(float m[16], float fovy, float aspect, float zn, float zf) {
	float f = 1.0f / tanf(fovy * 0.5f);
	memset(m, 0, 16 * sizeof(float));
	m[0] = f / aspect;
	m[5] = -f;                 /* Vulkan clip space: y points down */
	m[10] = zf / (zn - zf);    /* depth 0..1 */
	m[11] = -1.0f;
	m[14] = zn * zf / (zn - zf);
}

void mat4_rotate_xyz(float m[16], float x, float y, float z) {
	float cx = cosf(x), sx = sinf(x), cy = cosf(y), sy = sinf(y), cz = cosf(z), sz = sinf(z);
	float rx[16], ry[16], rz[16], t[16];
	mat4_identity(rx); rx[5] = cx; rx[6] = sx; rx[9] = -sx; rx[10] = cx;
	mat4_identity(ry); ry[0] = cy; ry[2] = -sy; ry[8] = sy; ry[10] = cy;
	mat4_identity(rz); rz[0] = cz; rz[1] = sz; rz[4] = -sz; rz[5] = cz;
	mat4_mul(t, ry, rx);
	mat4_mul(m, rz, t);
}

void mat4_translate(float m[16], float x, float y, float z) {
	mat4_identity(m);
	m[12] = x; m[13] = y; m[14] = z;
}
