/*
 * Minimal VkVita program: cycles the clear color. Needs no shaders, so it
 * runs even without libshacccg.suprx. Start your port from here.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vulkan/vulkan.h>
#include <vkvita/vkvita.h>

#define CHECK(x) do { if ((x) != VK_SUCCESS) { vkvLog(VKV_LOG_ERROR, "%s failed", #x); sceKernelExitProcess(0); } } while (0)

int main(void) {
	/* Debug options can also come from ux0:data/vkvita/config.txt */
	VkvConfig cfg;
	vkvGetDefaultConfig(&cfg);
	cfg.debugFlags |= VKV_DEBUG_VALIDATION | VKV_DEBUG_PROFILE;
	vkvSetConfig(&cfg);

	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkSurfaceKHR surface;
	CHECK(vkvCreateDisplaySurface(inst, &surface));
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
	const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = &ext;
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	sci.surface = surface;
	sci.minImageCount = 3;
	sci.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	sci.imageExtent = (VkExtent2D){ 960, 544 };
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	VkSwapchainKHR sc;
	CHECK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
	uint32_t count = 4;
	VkImage images[4];
	CHECK(vkGetSwapchainImagesKHR(dev, sc, &count, images));

	VkAttachmentDescription att = { 0, sci.imageFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
		VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL };
	VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 0, NULL };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
	VkImageView views[4];
	VkFramebuffer fbs[4];
	for (uint32_t i = 0; i < count; i++) {
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = images[i];
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = sci.imageFormat;
		vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		CHECK(vkCreateImageView(dev, &vi, NULL, &views[i]));
		VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &views[i], 960, 544, 1 };
		CHECK(vkCreateFramebuffer(dev, &fi, NULL, &fbs[i]));
	}

	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
	                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
	                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	for (float t = 0.0f;; t += 0.02f) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_START) break;
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
		CHECK(vkResetFences(dev, 1, &fence));
		uint32_t idx;
		CHECK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &idx));
		CHECK(vkResetCommandBuffer(cb, 0));
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		CHECK(vkBeginCommandBuffer(cb, &bi));
		VkClearValue clear = { .color = { { 0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.1f),
		                                    0.5f + 0.5f * sinf(t + 4.2f), 1.0f } } };
		VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fbs[idx],
		                              { { 0, 0 }, { 960, 544 } }, 1, &clear };
		vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		/* ... bind pipelines and draw here ... */
		vkCmdEndRenderPass(cb);
		CHECK(vkEndCommandBuffer(cb));
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.swapchainCount = 1;
		pi.pSwapchains = &sc;
		pi.pImageIndices = &idx;
		CHECK(vkQueuePresentKHR(q, &pi));
	}
	vkDeviceWaitIdle(dev);
	vkDestroyFence(dev, fence, NULL);
	vkDestroyCommandPool(dev, pool, NULL);
	for (uint32_t i = 0; i < count; i++) {
		vkDestroyFramebuffer(dev, fbs[i], NULL);
		vkDestroyImageView(dev, views[i], NULL);
	}
	vkDestroyRenderPass(dev, rp, NULL);
	vkDestroySwapchainKHR(dev, sc, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroySurfaceKHR(inst, surface, NULL);
	vkDestroyInstance(inst, NULL);
	sceKernelExitProcess(0);
	return 0;
}
