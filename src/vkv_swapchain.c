/*
 * VkVita: swapchain and presentation
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_replay.h"

/* --- Swapchain --- */
static void vkv_swapchain_free(struct VkDevice_T *dev, VkvSwapchain *sc) {
	vkv_device_wait_gpu(dev);
	sceGxmDisplayQueueFinish();
	for (uint32_t i = 0; i < sc->imageCount; i++) {
		if (sc->images[i]) {
			if (sc->images[i]->sync) sceGxmSyncObjectDestroy(sc->images[i]->sync);
			sc->images[i]->obj.magic = VKV_MAGIC_DEAD;
			free(sc->images[i]);
		}
		vkv_gpu_free(&sc->mem[i]);
	}
	if (dev->swapchain == sc) dev->swapchain = NULL;
	sc->obj.magic = VKV_MAGIC_DEAD;
	free(sc);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                    const VkAllocationCallbacks *alloc, VkSwapchainKHR *pSwapchain) {
	VKV_TRACE("vkCreateSwapchainKHR");
	(void)alloc;
	if (ci->imageExtent.width != VKV_DISPLAY_WIDTH || ci->imageExtent.height != VKV_DISPLAY_HEIGHT) {
		VKV_ERROR("swapchain extent must be %ux%u (got %ux%u)", VKV_DISPLAY_WIDTH, VKV_DISPLAY_HEIGHT,
		          (unsigned)ci->imageExtent.width, (unsigned)ci->imageExtent.height);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (ci->imageFormat != VK_FORMAT_R8G8B8A8_UNORM && ci->imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
	    ci->imageFormat != VK_FORMAT_R8G8B8A8_SRGB && ci->imageFormat != VK_FORMAT_B8G8R8A8_SRGB) {
		VKV_ERROR("swapchain format %d is not supported", (int)ci->imageFormat);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	VkvSwapchain *old = VKV_ND(VkvSwapchain, ci->oldSwapchain);
	if (device->swapchain && device->swapchain != old) {
		VKV_ERROR("only one swapchain can exist at a time");
		return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
	}

	VkvSwapchain *sc = calloc(1, sizeof(*sc));
	if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
	sc->obj.magic = VKV_MAGIC_SWAPCHAIN;
	sc->device = device;
	sc->imageCount = VKV_MIN(VKV_MAX(ci->minImageCount, 2u), (uint32_t)VKV_MAX_SWAP_IMAGES);
	sc->presentMode = ci->presentMode;
	sc->format = ci->imageFormat;
	sc->width = VKV_DISPLAY_WIDTH;
	sc->height = VKV_DISPLAY_HEIGHT;

	for (uint32_t i = 0; i < sc->imageCount; i++) {
		VkvImage *img = calloc(1, sizeof(*img));
		if (!img) { vkv_swapchain_free(device, sc); return VK_ERROR_OUT_OF_HOST_MEMORY; }
		sc->images[i] = img;
		img->obj.magic = VKV_MAGIC_IMAGE;
		img->format = ci->imageFormat;
		img->fi = vkv_format(ci->imageFormat);
		img->width = sc->width;
		img->height = sc->height;
		img->mipLevels = img->arrayLayers = 1;
		img->usage = ci->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		img->swapchain = true;
		vkv_image_setup_layout(img);
		if (!vkv_gpu_alloc(VKV_HEAP_CDRAM, img->size, SCE_GXM_MEMORY_ATTRIB_RW, &sc->mem[i]) ||
		    vkv_image_ensure_sync(img) != VK_SUCCESS) {
			vkv_swapchain_free(device, sc);
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		memset(sc->mem[i].base, 0, img->size);
		sc->memObj[i].obj.magic = VKV_MAGIC_MEMORY;
		sc->memObj[i].ptr = sc->mem[i].base;
		sc->memObj[i].size = img->size;
		img->mem = &sc->memObj[i];
		img->ptr = sc->mem[i].base;
	}
	sc->acquireIndex = 0;
	sc->frontIndex = sc->imageCount - 1;
	device->swapchain = sc;
	VKV_INFO("swapchain: %u images %ux%u, format %d, present mode %d", (unsigned)sc->imageCount, (unsigned)sc->width,
	         (unsigned)sc->height, (int)sc->format, (int)sc->presentMode);
	*pSwapchain = VKV_H(VkSwapchainKHR, sc);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                 const VkAllocationCallbacks *alloc) {
	VKV_TRACE("vkDestroySwapchainKHR");
	(void)alloc;
	VkvSwapchain *sc = VKV_ND(VkvSwapchain, swapchain);
	if (sc) vkv_swapchain_free(device, sc);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pCount,
                                                       VkImage *pImages) {
	(void)device;
	VkvSwapchain *sc = VKV_ND(VkvSwapchain, swapchain);
	if (!pImages) { *pCount = sc->imageCount; return VK_SUCCESS; }
	uint32_t n = VKV_MIN(*pCount, sc->imageCount);
	for (uint32_t i = 0; i < n; i++) pImages[i] = VKV_H(VkImage, sc->images[i]);
	*pCount = n;
	return n < sc->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                     VkSemaphore semaphore, VkFence fence, uint32_t *pIndex) {
	VKV_TRACE("vkAcquireNextImageKHR");
	(void)device; (void)timeout; (void)semaphore;
	VkvSwapchain *sc = VKV_ND(VkvSwapchain, swapchain);
	if (VKV_CHECK(VKV_OBJ_OK(sc, VKV_MAGIC_SWAPCHAIN), "invalid swapchain")) return VK_ERROR_OUT_OF_DATE_KHR;
	/* Display ordering is enforced on the GPU through each image's sync
	 * object, so the image can be handed out immediately. */
	*pIndex = sc->acquireIndex;
	sc->acquireIndex = (sc->acquireIndex + 1) % sc->imageCount;
	VkvFence *f = VKV_ND(VkvFence, fence);
	if (f) { f->signaled = true; f->pending = false; }
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *ai,
                                                      uint32_t *pIndex) {
	return vkAcquireNextImageKHR(device, ai->swapchain, ai->timeout, ai->semaphore, ai->fence, pIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
	VKV_TRACE("vkQueuePresentKHR");
	struct VkDevice_T *dev = queue->device;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < pi->swapchainCount; i++) {
		VkvSwapchain *sc = VKV_ND(VkvSwapchain, pi->pSwapchains[i]);
		uint32_t idx = pi->pImageIndices[i];
		VkResult r = VK_SUCCESS;
		if (VKV_CHECK(VKV_OBJ_OK(sc, VKV_MAGIC_SWAPCHAIN) && idx < sc->imageCount, "bad swapchain/image index")) {
			r = VK_ERROR_OUT_OF_DATE_KHR;
		} else {
			VkvImage *img = sc->images[idx];
			VkvDisplayData d;
			d.address = img->ptr;
			d.width = sc->width;
			d.height = sc->height;
			d.pitch = img->alignedW;
			d.vsync = sc->presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR;
			if (vkv_compat(VKV_COMPAT_FORCE_VSYNC)) d.vsync = 1;
			if (vkv_compat(VKV_COMPAT_NO_VSYNC)) d.vsync = 0;
			int e = sceGxmDisplayQueueAddEntry(sc->images[sc->frontIndex]->sync, img->sync, &d);
			if (e < 0) {
				VKV_ERROR("sceGxmDisplayQueueAddEntry failed 0x%08X", (unsigned)e);
				r = VK_ERROR_DEVICE_LOST;
			}
			sc->frontIndex = idx;
		}
		if (pi->pResults) pi->pResults[i] = r;
		if (r != VK_SUCCESS) result = r;
	}
	if (vkv_dbg(VKV_DEBUG_GPU_SYNC)) vkv_device_wait_gpu(dev);
	vkv_stats_frame_end();
	return result;
}
