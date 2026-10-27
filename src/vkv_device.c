/*
 * VkVita: device creation (GXM init), notifications, fences and semaphores
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

/* --- Notifications (fences) --- */
int32_t vkv_notif_alloc(struct VkDevice_T *dev) {
	for (int32_t i = 0; i < VKV_NOTIF_SLOTS; i++)
		if (!dev->notifUsed[i]) { dev->notifUsed[i] = 1; return i; }
	return -1;
}

void vkv_notif_free(struct VkDevice_T *dev, int32_t slot) {
	if (slot >= 0 && slot < VKV_NOTIF_SLOTS) dev->notifUsed[slot] = 0;
}

/* --- Device --- */
static void vkv_display_callback(const void *data) {
	const VkvDisplayData *d = data;
	SceDisplayFrameBuf fb;
	memset(&fb, 0, sizeof(fb));
	fb.size        = sizeof(fb);
	fb.base        = d->address;
	fb.pitch       = d->pitch;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width       = d->width;
	fb.height      = d->height;
	sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
	if (d->vsync) sceDisplayWaitVblankStart();
}

static void *vkv_patcher_host_alloc(void *user, SceSize size) { (void)user; return malloc(size); }
static void  vkv_patcher_host_free(void *user, void *mem) { (void)user; free(mem); }

#define VKV_PATCHER_BUFFER_SIZE (64 * 1024)
#define VKV_PATCHER_VUSSE_SIZE  (64 * 1024)
#define VKV_PATCHER_FUSSE_SIZE  (64 * 1024)

static bool s_gxmInitialized;

static void vkv_device_teardown(struct VkDevice_T *d) {
	if (d->ctx) sceGxmFinish(d->ctx);
	if (s_gxmInitialized) sceGxmDisplayQueueFinish();
	vkv_clear_programs_free(d);
	vkv_shark_term(d);
	if (d->patcher) sceGxmShaderPatcherDestroy(d->patcher);
	vkv_gpu_free(&d->patcherBuf);
	vkv_free_vertex_usse(d->patcherVUsseUid);
	vkv_free_fragment_usse(d->patcherFUsseUid);
	if (d->ctx) sceGxmDestroyContext(d->ctx);
	vkv_gpu_free(&d->vdmRing);
	vkv_gpu_free(&d->vertexRing);
	vkv_gpu_free(&d->fragmentRing);
	vkv_free_fragment_usse(d->fragUsseRingUid);
	vkv_gpu_free(&d->linearIdx16);
	vkv_gpu_free(&d->linearIdx32);
	vkv_gpu_free(&d->stripIdx16);
	vkv_gpu_free(&d->stripRing);
	vkv_gpu_free(&d->visBuf);
	vkv_pool_destroy(&d->pool);
	free(d->ctxHostMem);
	if (s_gxmInitialized) { sceGxmTerminate(); s_gxmInitialized = false; }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                              const VkAllocationCallbacks *alloc, VkDevice *pDevice) {
	(void)alloc;
	VKV_TRACE("vkCreateDevice");
	if (s_gxmInitialized) {
		VKV_ERROR("only one VkDevice is supported (GXM is already initialized)");
		return VK_ERROR_TOO_MANY_OBJECTS;
	}
	for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
		if (strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
			VKV_ERROR("device extension %s is not supported", ci->ppEnabledExtensionNames[i]);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}
	}
	if (ci->pEnabledFeatures) {
		const VkPhysicalDeviceFeatures *f = ci->pEnabledFeatures;
		if (f->geometryShader || f->tessellationShader || f->multiViewport || f->samplerAnisotropy ||
		    f->shaderFloat64 || f->largePoints)
			VKV_WARN("vkCreateDevice: some requested features are not available and will be ignored");
	}

	struct VkDevice_T *d = calloc(1, sizeof(*d));
	if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
	d->obj.magic = VKV_MAGIC_DEVICE;
	d->instance = pd->instance;
	d->queue.obj.magic = VKV_MAGIC_QUEUE;
	d->queue.device = d;
	d->fragUsseRingUid = d->patcherVUsseUid = d->patcherFUsseUid = -1;

	SceGxmInitializeParams ip;
	memset(&ip, 0, sizeof(ip));
	ip.flags                        = 0;
	ip.displayQueueMaxPendingCount  = g_vkv.cfg.displayQueueDepth;
	ip.displayQueueCallback         = vkv_display_callback;
	ip.displayQueueCallbackDataSize = sizeof(VkvDisplayData);
	ip.parameterBufferSize          = g_vkv.cfg.parameterBufferSize;
	int r = sceGxmInitialize(&ip);
	if (r < 0) {
		VKV_ERROR("sceGxmInitialize failed: 0x%08X", (unsigned)r);
		free(d);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	s_gxmInitialized = true;

	/* context */
	d->ctxHostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
	unsigned int fragUsseOffset = 0;
	void *fragUsse = vkv_alloc_fragment_usse(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
	                                         &d->fragUsseRingUid, &fragUsseOffset);
	if (!d->ctxHostMem || !fragUsse ||
	    !vkv_gpu_alloc(VKV_HEAP_RAM, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, &d->vdmRing) ||
	    !vkv_gpu_alloc(VKV_HEAP_RAM, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, &d->vertexRing) ||
	    !vkv_gpu_alloc(VKV_HEAP_RAM, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, &d->fragmentRing)) {
		VKV_ERROR("failed to allocate GXM ring buffers");
		vkv_device_teardown(d);
		free(d);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	SceGxmContextParams cp;
	memset(&cp, 0, sizeof(cp));
	cp.hostMem                       = d->ctxHostMem;
	cp.hostMemSize                   = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
	cp.vdmRingBufferMem              = d->vdmRing.base;
	cp.vdmRingBufferMemSize          = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
	cp.vertexRingBufferMem           = d->vertexRing.base;
	cp.vertexRingBufferMemSize       = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
	cp.fragmentRingBufferMem         = d->fragmentRing.base;
	cp.fragmentRingBufferMemSize     = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
	cp.fragmentUsseRingBufferMem     = fragUsse;
	cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
	cp.fragmentUsseRingBufferOffset  = fragUsseOffset;
	r = sceGxmCreateContext(&cp, &d->ctx);
	if (r < 0) {
		VKV_ERROR("sceGxmCreateContext failed: 0x%08X", (unsigned)r);
		d->ctx = NULL;
		vkv_device_teardown(d);
		free(d);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* shader patcher */
	unsigned int vOff = 0, fOff = 0;
	void *vusse = vkv_alloc_vertex_usse(VKV_PATCHER_VUSSE_SIZE, &d->patcherVUsseUid, &vOff);
	void *fusse = vkv_alloc_fragment_usse(VKV_PATCHER_FUSSE_SIZE, &d->patcherFUsseUid, &fOff);
	if (!vusse || !fusse ||
	    !vkv_gpu_alloc(VKV_HEAP_RAM, VKV_PATCHER_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, &d->patcherBuf)) {
		VKV_ERROR("failed to allocate shader patcher memory");
		vkv_device_teardown(d);
		free(d);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	SceGxmShaderPatcherParams pp;
	memset(&pp, 0, sizeof(pp));
	pp.hostAllocCallback  = vkv_patcher_host_alloc;
	pp.hostFreeCallback   = vkv_patcher_host_free;
	pp.bufferMem          = d->patcherBuf.base;
	pp.bufferMemSize      = VKV_PATCHER_BUFFER_SIZE;
	pp.vertexUsseMem      = vusse;
	pp.vertexUsseMemSize  = VKV_PATCHER_VUSSE_SIZE;
	pp.vertexUsseOffset   = vOff;
	pp.fragmentUsseMem    = fusse;
	pp.fragmentUsseMemSize= VKV_PATCHER_FUSSE_SIZE;
	pp.fragmentUsseOffset = fOff;
	r = sceGxmShaderPatcherCreate(&pp, &d->patcher);
	if (r < 0) {
		VKV_ERROR("sceGxmShaderPatcherCreate failed: 0x%08X", (unsigned)r);
		d->patcher = NULL;
		vkv_device_teardown(d);
		free(d);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	d->notifRegion = sceGxmGetNotificationRegion();
	d->progressSlot = vkv_notif_alloc(d);
	d->progress.address = &d->notifRegion[d->progressSlot];
	d->progress.value = 0;
	*d->progress.address = 0;
	d->sceneSerial = 0;
	vkv_vis_init(d);

	/* 0..65535 index list for vkCmdDraw */
	d->linearIdx16Count = 65536;
	if (vkv_gpu_alloc(VKV_HEAP_RAM, d->linearIdx16Count * 2, SCE_GXM_MEMORY_ATTRIB_READ, &d->linearIdx16)) {
		uint16_t *idx = d->linearIdx16.base;
		for (uint32_t i = 0; i < d->linearIdx16Count; i++) idx[i] = (uint16_t)i;
	}
	/* line strips become line lists: vertex pairs (k, k + 1) */
	if (vkv_gpu_alloc(VKV_HEAP_RAM, VKV_STRIP_PAIRS * 4, SCE_GXM_MEMORY_ATTRIB_READ, &d->stripIdx16)) {
		uint16_t *idx = d->stripIdx16.base;
		for (uint32_t k = 0; k < VKV_STRIP_PAIRS; k++) {
			idx[2 * k] = (uint16_t)k;
			idx[2 * k + 1] = (uint16_t)(k + 1);
		}
	}

	VKV_INFO("device ready: GXM initialized, parameter buffer %u KB, display queue depth %u",
	         (unsigned)(g_vkv.cfg.parameterBufferSize / 1024), (unsigned)g_vkv.cfg.displayQueueDepth);
	*pDevice = d;
	return VK_SUCCESS;
}

bool vkv_serial_done(const struct VkDevice_T *dev, uint32_t serial) {
	return (int32_t)(*dev->progress.address - serial) >= 0;
}

void vkv_device_wait_gpu(struct VkDevice_T *dev) {
	uint64_t t0 = vkv_time_us();
	sceGxmFinish(dev->ctx);
	dev->gpuBusy = false;
	if (dev->usedTransfer) {
		sceGxmTransferFinish();
		dev->usedTransfer = false;
	}
	g_vkv.cur.gpuWaitUs += vkv_time_us() - t0;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
	(void)alloc;
	if (!device) return;
	VKV_TRACE("vkDestroyDevice");
	vkv_device_teardown(device);
	device->obj.magic = VKV_MAGIC_DEAD;
	free(device);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *pQueue) {
	(void)family; (void)index;
	*pQueue = &device->queue;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
	VKV_TRACE("vkDeviceWaitIdle");
	vkv_device_wait_gpu(device);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
	vkv_device_wait_gpu(queue->device);
	return VK_SUCCESS;
}


/* --- Fences and semaphores --- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice device, const VkFenceCreateInfo *ci,
                                             const VkAllocationCallbacks *alloc, VkFence *pFence) {
	(void)alloc;
	VkvFence *f = calloc(1, sizeof(*f));
	if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
	f->obj.magic = VKV_MAGIC_FENCE;
	f->device = device;
	f->signaled = (ci->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
	*pFence = VKV_H(VkFence, f);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *alloc) {
	(void)alloc;
	VkvFence *f = VKV_ND(VkvFence, fence);
	if (!f) return;
	(void)device;
	f->obj.magic = VKV_MAGIC_DEAD;
	free(f);
}

static bool vkv_fence_poll(VkvFence *f) {
	if (f->signaled) return true;
	if (f->pending && vkv_serial_done(f->device, f->serial)) {
		f->pending = false;
		f->signaled = true;
		return true;
	}
	return false;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t count, const VkFence *fences) {
	(void)device;
	for (uint32_t i = 0; i < count; i++) {
		VkvFence *f = VKV_ND(VkvFence, fences[i]);
		if (VKV_CHECK(VKV_OBJ_OK(f, VKV_MAGIC_FENCE), "invalid fence")) continue;
		if (VKV_CHECK(!f->pending || vkv_fence_poll(f), "resetting a fence that is still in flight")) continue;
		f->signaled = false;
		f->pending = false;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence) {
	(void)device;
	return vkv_fence_poll(VKV_ND(VkvFence, fence)) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences,
                                               VkBool32 waitAll, uint64_t timeout) {
	VKV_TRACE("vkWaitForFences");
	uint64_t start = vkv_time_us();
	uint64_t limitUs = timeout == UINT64_MAX ? UINT64_MAX : timeout / 1000;
	for (;;) {
		uint32_t done = 0;
		for (uint32_t i = 0; i < count; i++) {
			VkvFence *f = VKV_ND(VkvFence, fences[i]);
			if (VKV_CHECK(VKV_OBJ_OK(f, VKV_MAGIC_FENCE), "invalid fence")) return VK_ERROR_DEVICE_LOST;
			if (VKV_CHECK(f->signaled || f->pending, "waiting on a fence that was never submitted (deadlock)"))
				return VK_TIMEOUT;
			if (vkv_fence_poll(f)) done++;
		}
		if ((waitAll && done == count) || (!waitAll && done)) break;
		uint64_t elapsed = vkv_time_us() - start;
		if (elapsed >= limitUs) {
			g_vkv.cur.gpuWaitUs += elapsed;
			return VK_TIMEOUT;
		}
		sceKernelDelayThread(100);
	}
	g_vkv.cur.gpuWaitUs += vkv_time_us() - start;
	(void)device;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc, VkSemaphore *pSem) {
	(void)device; (void)ci; (void)alloc;
	/* One in-order queue: semaphores carry no work, they only exist for API shape. */
	VkvSemaphore *s = calloc(1, sizeof(*s));
	if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
	s->obj.magic = VKV_MAGIC_SEMAPHORE;
	*pSem = VKV_H(VkSemaphore, s);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore sem, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvSemaphore *s = VKV_ND(VkvSemaphore, sem);
	if (!s) return;
	s->obj.magic = VKV_MAGIC_DEAD;
	free(s);
}

/* --- Pipeline cache (accepted, not persisted) --- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *ci,
                                                     const VkAllocationCallbacks *alloc, VkPipelineCache *pCache) {
	(void)device; (void)ci; (void)alloc;
	VkvPipelineCache *c = calloc(1, sizeof(*c));
	if (!c) return VK_ERROR_OUT_OF_HOST_MEMORY;
	c->obj.magic = VKV_MAGIC_PCACHE;
	*pCache = VKV_H(VkPipelineCache, c);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache cache,
                                                  const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	free(VKV_ND(VkvPipelineCache, cache));
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache, size_t *size,
                                                      void *data) {
	(void)device; (void)cache; (void)data;
	*size = 0;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice device, VkPipelineCache dst, uint32_t n,
                                                     const VkPipelineCache *src) {
	(void)device; (void)dst; (void)n; (void)src;
	return VK_SUCCESS;
}
