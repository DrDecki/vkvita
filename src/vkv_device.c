/*
 * VkVita: device (GXM init), memory, buffers, images, samplers, sync.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

/* ------------------------------------------------------------------------ */
/* Raw GPU memory                                                           */
/* ------------------------------------------------------------------------ */
bool vkv_gpu_alloc(VkvHeapKind kind, uint32_t size, SceGxmMemoryAttribFlags attr, VkvGpuBlock *out) {
	memset(out, 0, sizeof(*out));
	SceKernelMemBlockType type = kind == VKV_HEAP_CDRAM ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW
	                                                    : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
	uint32_t asz = VKV_ALIGN(size, kind == VKV_HEAP_CDRAM ? VKV_CDRAM_ALIGN : VKV_RAM_ALIGN);
	SceUID uid = sceKernelAllocMemBlock(kind == VKV_HEAP_CDRAM ? "vkv_cdram" : "vkv_ram", type, asz, NULL);
	if (uid < 0 && kind == VKV_HEAP_CDRAM) {
		VKV_INFO("CDRAM exhausted (%u bytes), falling back to main memory", (unsigned)size);
		return vkv_gpu_alloc(VKV_HEAP_RAM, size, attr, out);
	}
	if (uid < 0) {
		VKV_ERROR("sceKernelAllocMemBlock(%u) failed: 0x%08X", (unsigned)asz, (unsigned)uid);
		return false;
	}
	void *base = NULL;
	sceKernelGetMemBlockBase(uid, &base);
	int r = sceGxmMapMemory(base, asz, attr);
	if (r < 0) {
		VKV_ERROR("sceGxmMapMemory failed: 0x%08X", (unsigned)r);
		sceKernelFreeMemBlock(uid);
		return false;
	}
	out->base = base;
	out->uid = uid;
	out->size = asz;
	out->kind = kind;
	if (kind == VKV_HEAP_CDRAM) g_vkv.cdramBytes += asz; else g_vkv.ramBytes += asz;
	return true;
}

void vkv_gpu_free(VkvGpuBlock *blk) {
	if (!blk || !blk->base) return;
	sceGxmUnmapMemory(blk->base);
	sceKernelFreeMemBlock(blk->uid);
	if (blk->kind == VKV_HEAP_CDRAM) g_vkv.cdramBytes -= blk->size; else g_vkv.ramBytes -= blk->size;
	memset(blk, 0, sizeof(*blk));
}

void *vkv_alloc_vertex_usse(uint32_t size, SceUID *uid, unsigned int *offset) {
	size = VKV_ALIGN(size, 4096);
	*uid = sceKernelAllocMemBlock("vkv_vusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
	if (*uid < 0) return NULL;
	void *mem = NULL;
	sceKernelGetMemBlockBase(*uid, &mem);
	if (sceGxmMapVertexUsseMemory(mem, size, offset) < 0) { sceKernelFreeMemBlock(*uid); return NULL; }
	return mem;
}

void *vkv_alloc_fragment_usse(uint32_t size, SceUID *uid, unsigned int *offset) {
	size = VKV_ALIGN(size, 4096);
	*uid = sceKernelAllocMemBlock("vkv_fusse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
	if (*uid < 0) return NULL;
	void *mem = NULL;
	sceKernelGetMemBlockBase(*uid, &mem);
	if (sceGxmMapFragmentUsseMemory(mem, size, offset) < 0) { sceKernelFreeMemBlock(*uid); return NULL; }
	return mem;
}

void vkv_free_vertex_usse(SceUID uid) {
	void *mem = NULL;
	if (uid < 0 || sceKernelGetMemBlockBase(uid, &mem) < 0) return;
	sceGxmUnmapVertexUsseMemory(mem);
	sceKernelFreeMemBlock(uid);
}

void vkv_free_fragment_usse(SceUID uid) {
	void *mem = NULL;
	if (uid < 0 || sceKernelGetMemBlockBase(uid, &mem) < 0) return;
	sceGxmUnmapFragmentUsseMemory(mem);
	sceKernelFreeMemBlock(uid);
}

/* ------------------------------------------------------------------------ */
/* Sub-allocator: first fit over a sorted free list per block               */
/* ------------------------------------------------------------------------ */
static bool vkv_pool_block_alloc(VkvPoolBlock *b, uint32_t size, uint32_t align, uint32_t *outOff) {
	VkvFreeRange **link = &b->free;
	for (VkvFreeRange *r = b->free; r; link = &r->next, r = r->next) {
		uint32_t start = VKV_ALIGN(r->offset, align);
		uint32_t pad = start - r->offset;
		if (r->size < pad + size) continue;
		uint32_t tail = r->size - pad - size;
		if (pad) {
			/* keep the padding as a free range */
			uint32_t oldEnd = r->offset + r->size;
			r->size = pad;
			if (tail) {
				VkvFreeRange *t = malloc(sizeof(*t));
				if (!t) return false;
				t->offset = oldEnd - tail;
				t->size = tail;
				t->next = r->next;
				r->next = t;
			}
		} else if (tail) {
			r->offset += size;
			r->size = tail;
		} else {
			*link = r->next;
			free(r);
		}
		b->used += size;
		*outOff = start;
		return true;
	}
	return false;
}

static void vkv_pool_block_free(VkvPoolBlock *b, uint32_t off, uint32_t size) {
	VkvFreeRange *prev = NULL, *cur = b->free;
	while (cur && cur->offset < off) { prev = cur; cur = cur->next; }
	b->used -= size;
	/* merge with previous */
	if (prev && prev->offset + prev->size == off) {
		prev->size += size;
		if (cur && prev->offset + prev->size == cur->offset) {
			prev->size += cur->size;
			prev->next = cur->next;
			free(cur);
		}
		return;
	}
	/* merge with next */
	if (cur && off + size == cur->offset) {
		cur->offset = off;
		cur->size += size;
		return;
	}
	VkvFreeRange *n = malloc(sizeof(*n));
	if (!n) return; /* leak the range rather than crash */
	n->offset = off;
	n->size = size;
	n->next = cur;
	if (prev) prev->next = n; else b->free = n;
}

static VkvPoolBlock *vkv_pool_new_block(VkvPool *pool, VkvHeapKind kind) {
	VkvPoolBlock *b = calloc(1, sizeof(*b));
	if (!b) return NULL;
	if (!vkv_gpu_alloc(kind, g_vkv.cfg.poolBlockSize, SCE_GXM_MEMORY_ATTRIB_RW, &b->blk)) { free(b); return NULL; }
	b->free = malloc(sizeof(VkvFreeRange));
	b->free->offset = 0;
	b->free->size = b->blk.size;
	b->free->next = NULL;
	/* the block may have landed in RAM after a CDRAM fallback; file it there */
	VkvHeapKind real = b->blk.kind;
	b->next = pool->blocks[real];
	pool->blocks[real] = b;
	return b;
}

static void vkv_pool_destroy(VkvPool *pool) {
	for (int k = 0; k < VKV_HEAP_COUNT; k++) {
		VkvPoolBlock *b = pool->blocks[k];
		while (b) {
			VkvPoolBlock *n = b->next;
			for (VkvFreeRange *r = b->free; r;) { VkvFreeRange *rn = r->next; free(r); r = rn; }
			vkv_gpu_free(&b->blk);
			free(b);
			b = n;
		}
		pool->blocks[k] = NULL;
	}
}

VkResult vkv_memory_alloc(struct VkDevice_T *dev, uint32_t typeIndex, uint32_t size, uint32_t align, VkvMemory *m) {
	memset(m, 0, sizeof(*m));
	m->obj.magic = VKV_MAGIC_MEMORY;
	m->size = size;
	m->typeIndex = typeIndex;
	if (align < 16) align = 16;
	VkvHeapKind kind = typeIndex == VKV_MEMTYPE_CDRAM ? VKV_HEAP_CDRAM : VKV_HEAP_RAM;

	if (size <= g_vkv.cfg.poolMaxAllocSize && align <= 4096) {
		for (VkvPoolBlock *b = dev->pool.blocks[kind]; b; b = b->next) {
			uint32_t off;
			if (vkv_pool_block_alloc(b, VKV_ALIGN(size, 16), align, &off)) {
				m->block = b; m->offset = off; m->ptr = (uint8_t *)b->blk.base + off;
				return VK_SUCCESS;
			}
		}
		VkvPoolBlock *b = vkv_pool_new_block(&dev->pool, kind);
		uint32_t off;
		if (b && vkv_pool_block_alloc(b, VKV_ALIGN(size, 16), align, &off)) {
			m->block = b; m->offset = off; m->ptr = (uint8_t *)b->blk.base + off;
			return VK_SUCCESS;
		}
	}
	if (!vkv_gpu_alloc(kind, size, SCE_GXM_MEMORY_ATTRIB_RW, &m->dedicated))
		return kind == VKV_HEAP_CDRAM ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_OUT_OF_HOST_MEMORY;
	m->ptr = m->dedicated.base;
	return VK_SUCCESS;
}

void vkv_memory_free(struct VkDevice_T *dev, VkvMemory *m) {
	(void)dev;
	if (m->block) vkv_pool_block_free(m->block, m->offset, VKV_ALIGN(m->size, 16));
	else vkv_gpu_free(&m->dedicated);
	m->ptr = NULL;
}

/* ------------------------------------------------------------------------ */
/* Notifications (fences)                                                   */
/* ------------------------------------------------------------------------ */
int32_t vkv_notif_alloc(struct VkDevice_T *dev) {
	for (int32_t i = 0; i < VKV_NOTIF_SLOTS; i++)
		if (!dev->notifUsed[i]) { dev->notifUsed[i] = 1; return i; }
	return -1;
}

void vkv_notif_free(struct VkDevice_T *dev, int32_t slot) {
	if (slot >= 0 && slot < VKV_NOTIF_SLOTS) dev->notifUsed[slot] = 0;
}

/* ------------------------------------------------------------------------ */
/* Device                                                                   */
/* ------------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------------ */
/* Device memory                                                            */
/* ------------------------------------------------------------------------ */
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai,
                                                const VkAllocationCallbacks *alloc, VkDeviceMemory *pMem) {
	VKV_TRACE("vkAllocateMemory");
	(void)alloc;
	if (VKV_CHECK(ai->memoryTypeIndex < VKV_MEMTYPE_COUNT, "invalid memoryTypeIndex %u", (unsigned)ai->memoryTypeIndex))
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (ai->allocationSize > 0xFFFFFFFFull) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	VkvMemory *m = malloc(sizeof(*m));
	if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkResult r = vkv_memory_alloc(device, ai->memoryTypeIndex, (uint32_t)ai->allocationSize, 4096, m);
	if (r != VK_SUCCESS) {
		VKV_ERROR("vkAllocateMemory: %u bytes of type %u failed", (unsigned)ai->allocationSize,
		          (unsigned)ai->memoryTypeIndex);
		free(m);
		return r;
	}
	*pMem = VKV_H(VkDeviceMemory, m);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *alloc) {
	VKV_TRACE("vkFreeMemory");
	(void)alloc;
	VkvMemory *m = VKV_ND(VkvMemory, memory);
	if (!m) return;
	if (VKV_CHECK(m->obj.magic == VKV_MAGIC_MEMORY, "not a VkDeviceMemory (double free?)")) return;
	vkv_memory_free(device, m);
	m->obj.magic = VKV_MAGIC_DEAD;
	free(m);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                           VkDeviceSize size, VkMemoryMapFlags flags, void **ppData) {
	(void)device; (void)flags;
	VkvMemory *m = VKV_ND(VkvMemory, memory);
	if (VKV_CHECK(VKV_OBJ_OK(m, VKV_MAGIC_MEMORY), "invalid memory handle")) return VK_ERROR_MEMORY_MAP_FAILED;
	if (VKV_CHECK(offset < m->size, "map offset %u beyond size %u", (unsigned)offset, (unsigned)m->size))
		return VK_ERROR_MEMORY_MAP_FAILED;
	(void)size;
	*ppData = m->ptr + offset;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory) { (void)device; (void)memory; }

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t n, const VkMappedMemoryRange *r) {
	(void)device; (void)n; (void)r; /* all exposed memory is uncached and coherent */
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t n,
                                                              const VkMappedMemoryRange *r) {
	(void)device; (void)n; (void)r;
	return VK_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Buffers                                                                  */
/* ------------------------------------------------------------------------ */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *ci,
                                              const VkAllocationCallbacks *alloc, VkBuffer *pBuffer) {
	VKV_TRACE("vkCreateBuffer");
	(void)device; (void)alloc;
	if (VKV_CHECK(ci->size > 0, "buffer size is 0")) return VK_ERROR_INITIALIZATION_FAILED;
	if (ci->usage & (VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))
		VKV_UNSUPPORTED("texel buffer usage (buffer created, but texel buffers will not work)");
	VkvBuffer *b = calloc(1, sizeof(*b));
	if (!b) return VK_ERROR_OUT_OF_HOST_MEMORY;
	b->obj.magic = VKV_MAGIC_BUFFER;
	b->size = (uint32_t)ci->size;
	b->usage = ci->usage;
	*pBuffer = VKV_H(VkBuffer, b);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvBuffer *b = VKV_ND(VkvBuffer, buffer);
	if (!b) return;
	b->obj.magic = VKV_MAGIC_DEAD;
	free(b);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer,
                                                         VkMemoryRequirements *req) {
	(void)device;
	VkvBuffer *b = VKV_ND(VkvBuffer, buffer);
	req->size = VKV_ALIGN(b->size, 16);
	req->alignment = 16;
	req->memoryTypeBits = (1u << VKV_MEMTYPE_COUNT) - 1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                                  VkDeviceSize offset) {
	(void)device;
	VkvBuffer *b = VKV_ND(VkvBuffer, buffer);
	VkvMemory *m = VKV_ND(VkvMemory, memory);
	if (VKV_CHECK(VKV_OBJ_OK(b, VKV_MAGIC_BUFFER) && VKV_OBJ_OK(m, VKV_MAGIC_MEMORY), "invalid handle"))
		return VK_ERROR_UNKNOWN;
	if (VKV_CHECK(offset + b->size <= m->size, "buffer (%u bytes at %u) does not fit memory (%u bytes)",
	              (unsigned)b->size, (unsigned)offset, (unsigned)m->size))
		return VK_ERROR_UNKNOWN;
	b->mem = m;
	b->ptr = m->ptr + offset;
	return VK_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Images                                                                   */
/* ------------------------------------------------------------------------ */
static bool vkv_is_pot(uint32_t v) { return v && !(v & (v - 1)); }

static uint32_t vkv_log2(uint32_t v) {
	uint32_t r = 0;
	while (v >>= 1) r++;
	return r;
}

/* Bits of x go to the even positions, bits of y to the odd ones. */
static uint32_t vkv_morton(uint32_t x, uint32_t y) {
	uint32_t r = 0;
	for (uint32_t b = 0; b < 16 && (x >> b || y >> b); b++)
		r |= ((x >> b) & 1u) << (2 * b) | ((y >> b) & 1u) << (2 * b + 1);
	return r;
}

/* GXM swizzled layout for power-of-two sizes: Morton order inside squares
 * of the shorter side, squares placed one after another along the longer one. */
uint32_t vkv_swizzle_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	if (w == h) return vkv_morton(x, y);
	if (w > h) {
		uint32_t k = vkv_log2(h);
		return ((x >> k) << (2 * k)) + vkv_morton(x & (h - 1), y);
	}
	uint32_t k = vkv_log2(w);
	return ((y >> k) << (2 * k)) + vkv_morton(x, y & (w - 1));
}

uint32_t vkv_image_level_width(const VkvImage *img, uint32_t level) { return VKV_MAX(img->width >> level, 1u); }
uint32_t vkv_image_level_height(const VkvImage *img, uint32_t level) { return VKV_MAX(img->height >> level, 1u); }

/* Width/height of a level in elements: texels, or 4x4 blocks for compressed formats. */
void vkv_image_level_elems(const VkvImage *img, uint32_t level, uint32_t *ew, uint32_t *eh) {
	uint32_t w = vkv_image_level_width(img, level), h = vkv_image_level_height(img, level);
	if (img->fi->block) { w = (w + 3) / 4; h = (h + 3) / 4; }
	*ew = w;
	*eh = h;
}

uint8_t *vkv_image_level_ptr(const VkvImage *img, uint32_t level, uint32_t layer) {
	return img->ptr + (size_t)layer * img->layerSize + img->levelOffset[level];
}

/* Address of one element (texel or block) of a level. */
uint8_t *vkv_image_elem(const VkvImage *img, uint32_t level, uint32_t layer, uint32_t ex, uint32_t ey) {
	uint8_t *base = vkv_image_level_ptr(img, level, layer);
	if (img->swizzled) {
		uint32_t ew, eh;
		vkv_image_level_elems(img, level, &ew, &eh);
		return base + (size_t)vkv_swizzle_index(ex, ey, ew, eh) * img->fi->bpp;
	}
	return base + (size_t)ey * img->levelPitch[level] + (size_t)ex * img->fi->bpp;
}

SceGxmMultisampleMode vkv_gxm_multisample(uint32_t samples) {
	return samples == 4 ? SCE_GXM_MULTISAMPLE_4X : samples == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
}

void vkv_image_setup_layout(VkvImage *img) {
	const VkvFormatInfo *fi = img->fi;
	img->levels = 1;
	img->layers = 1;
	if (fi->isDepth) {
		img->layers = VKV_MAX(img->arrayLayers, 1u);
		/* GXM keeps every sample of a depth buffer: 2x doubles the rows,
		 * 4x doubles rows and columns */
		img->alignedW = VKV_ALIGN(img->width, SCE_GXM_TILE_SIZEX) * (img->samples == 4 ? 2 : 1);
		img->alignedH = VKV_ALIGN(img->height, SCE_GXM_TILE_SIZEY) * (img->samples > 1 ? 2 : 1);
		img->rowPitch = img->alignedW * 4;
		img->levelPitch[0] = img->rowPitch;
		img->levelOffset[0] = 0;
		img->layerSize = img->alignedW * img->alignedH * 4;
		img->size = img->layerSize * img->layers;
		img->alignment = 4096;
		return;
	}
	/* every layer is stored; a cube keeps its six faces below */
	img->layers = VKV_MAX(img->arrayLayers, 1u);
	bool pot = vkv_is_pot(img->width) && vkv_is_pot(img->height);
	uint32_t maxLevels = vkv_log2(VKV_MAX(img->width, img->height)) + 1;
	img->levels = VKV_MIN(VKV_MAX(img->mipLevels, 1u), VKV_MIN(maxLevels, (uint32_t)VKV_MAX_LEVELS));
	if (img->levels > 1 && !pot) {
		VKV_UNSUPPORTED("mipmaps on non-power-of-two images (only level 0 is stored)");
		img->levels = 1;
	}
	if (img->cube) {
		img->layers = 6;
		if (img->width != img->height || !pot) {
			VKV_UNSUPPORTED("cube maps that are not square power-of-two sizes");
		}
		img->swizzled = true;
	}
	if (fi->block) {
		if (!pot) VKV_UNSUPPORTED("compressed textures that are not power-of-two sized");
		img->swizzled = true;
	}

	/* Level sizes follow GXM: levels are packed back to back, linear rows use an
	 * 8 texel stride, swizzled levels have no padding and compressed levels take
	 * whole 4x4 blocks. */
	uint32_t chainLevels = img->cube && img->levels > 1 ? maxLevels : img->levels;
	uint32_t total = 0, used = 0;
	for (uint32_t l = 0; l < chainLevels; l++) {
		uint32_t w = vkv_image_level_width(img, l), h = vkv_image_level_height(img, l), size, pitch;
		if (fi->block) {
			uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
			pitch = bw * fi->bpp;
			size = bw * bh * fi->bpp;
		} else if (img->swizzled) {
			pitch = w * fi->bpp;
			size = w * h * fi->bpp;
		} else {
			/* GXM linear textures and color surfaces use an 8 texel aligned stride */
			uint32_t stride = img->swapchain ? VKV_ALIGN(w, 64) : VKV_ALIGN(w, 8);
			pitch = stride * fi->bpp;
			size = pitch * h;
		}
		if (l < img->levels) {
			img->levelPitch[l] = pitch;
			img->levelOffset[l] = total;
			used = total + size;
		}
		total += size;
	}
	if (img->cube) {
		/* A cube with a mip chain gives every face room for the full chain down to
		 * 1x1, and large faces start on 2048 byte boundaries. Without a chain the
		 * faces follow each other directly. */
		uint32_t faceAlign = 4;
		if (img->levels > 1) {
			uint32_t bits = fi->block ? 0 : fi->bpp * 8, w = img->width, h = img->height;
			if ((w >= 32 && h >= 32 && (fi->block || bits <= 8)) || (w >= 16 && h >= 16 && (bits == 16 || bits == 32)) ||
			    (w >= 8 && h >= 8 && bits == 64))
				faceAlign = 2048;
		}
		img->layerSize = VKV_ALIGN(total, faceAlign);
	} else {
		/* layers of render targets start on page boundaries like the image itself */
		img->layerSize = img->layers > 1 && (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VKV_ALIGN(used, 4096)
		                                                                                   : VKV_ALIGN(used, 16);
	}
	img->alignedW = fi->block ? img->width : img->levelPitch[0] / fi->bpp;
	img->alignedH = img->height;
	img->rowPitch = img->levelPitch[0];
	img->size = img->layerSize * img->layers;
	img->alignment = (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? 4096 : 256;
}

VkResult vkv_image_ensure_sync(VkvImage *img) {
	if (img->sync) return VK_SUCCESS;
	if (sceGxmSyncObjectCreate(&img->sync) < 0) {
		img->sync = NULL;
		VKV_ERROR("sceGxmSyncObjectCreate failed");
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *ci,
                                             const VkAllocationCallbacks *alloc, VkImage *pImage) {
	VKV_TRACE("vkCreateImage");
	(void)device; (void)alloc;
	const VkvFormatInfo *fi = vkv_format(ci->format);
	if (!fi) {
		VKV_ERROR("vkCreateImage: format %d is not supported", (int)ci->format);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (ci->imageType != VK_IMAGE_TYPE_2D) {
		VKV_UNSUPPORTED("1D/3D images");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	uint32_t samples = (uint32_t)ci->samples;
	if (samples != 1 && samples != 2 && samples != 4) {
		VKV_ERROR("vkCreateImage: %u samples, GXM can do 1, 2 or 4", (unsigned)samples);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (samples > 1 && (ci->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)))
		VKV_UNSUPPORTED("reading the samples of a multisampled image (it holds the resolved pixels)");
	if (samples > 1 && (ci->mipLevels > 1 || ci->arrayLayers > 1)) {
		VKV_ERROR("vkCreateImage: multisampled images have one level and one layer");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	bool cube = (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && ci->arrayLayers == 6;
	if (ci->arrayLayers > 256) {
		VKV_ERROR("vkCreateImage: %u array layers, at most 256", (unsigned)ci->arrayLayers);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (fi->block && (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
		VKV_ERROR("vkCreateImage: compressed formats cannot be render targets");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (cube && (ci->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
		VKV_UNSUPPORTED("rendering into cube map faces");
	if (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT && ci->arrayLayers > 6)
		VKV_UNSUPPORTED("cube map arrays (only the first cube is stored)");
	if (VKV_CHECK(ci->extent.width && ci->extent.height && ci->extent.width <= 4096 && ci->extent.height <= 4096,
	              "bad image extent %ux%u", (unsigned)ci->extent.width, (unsigned)ci->extent.height))
		return VK_ERROR_INITIALIZATION_FAILED;

	VkvImage *img = calloc(1, sizeof(*img));
	if (!img) return VK_ERROR_OUT_OF_HOST_MEMORY;
	img->obj.magic = VKV_MAGIC_IMAGE;
	img->format = ci->format;
	img->fi = fi;
	img->width = ci->extent.width;
	img->height = ci->extent.height;
	img->mipLevels = ci->mipLevels;
	img->arrayLayers = ci->arrayLayers;
	img->samples = samples;
	img->usage = ci->usage;
	img->tiling = ci->tiling;
	img->cube = cube;
	vkv_image_setup_layout(img);
	*pImage = VKV_H(VkImage, img);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvImage *img = VKV_ND(VkvImage, image);
	if (!img) return;
	if (VKV_CHECK(!img->swapchain, "swapchain images must not be destroyed by the app")) return;
	if (img->sync) sceGxmSyncObjectDestroy(img->sync);
	img->obj.magic = VKV_MAGIC_DEAD;
	free(img);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *req) {
	(void)device;
	VkvImage *img = VKV_ND(VkvImage, image);
	req->size = img->size;
	req->alignment = img->alignment;
	req->memoryTypeBits = (1u << VKV_MEMTYPE_COUNT) - 1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                 VkDeviceSize offset) {
	(void)device;
	VkvImage *img = VKV_ND(VkvImage, image);
	VkvMemory *m = VKV_ND(VkvMemory, memory);
	if (VKV_CHECK(VKV_OBJ_OK(img, VKV_MAGIC_IMAGE) && VKV_OBJ_OK(m, VKV_MAGIC_MEMORY), "invalid handle"))
		return VK_ERROR_UNKNOWN;
	if (VKV_CHECK(offset + img->size <= m->size, "image does not fit its memory")) return VK_ERROR_UNKNOWN;
	if (VKV_CHECK((((uintptr_t)m->ptr + offset) & (img->alignment - 1)) == 0, "image memory is misaligned"))
		return VK_ERROR_UNKNOWN;
	img->mem = m;
	img->ptr = m->ptr + offset;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice device, VkImage image,
                                                       const VkImageSubresource *sub, VkSubresourceLayout *l) {
	(void)device;
	VkvImage *img = VKV_ND(VkvImage, image);
	uint32_t level = VKV_MIN(sub->mipLevel, img->levels - 1);
	uint32_t layer = VKV_MIN(sub->arrayLayer, img->layers - 1);
	if (img->swizzled) VKV_UNSUPPORTED("mapping swizzled (cube or compressed) images directly");
	l->offset = (size_t)layer * img->layerSize + img->levelOffset[level];
	l->rowPitch = img->levelPitch[level];
	l->size = level + 1 < img->levels ? img->levelOffset[level + 1] - img->levelOffset[level]
	                                  : img->layerSize - img->levelOffset[level];
	l->arrayPitch = img->layerSize;
	l->depthPitch = l->size;
}

/* ------------------------------------------------------------------------ */
/* Image views and samplers                                                 */
/* ------------------------------------------------------------------------ */
/* Component swizzles: GXM bakes the swizzle into the texture format. The
 * letters of a GXM swizzle name the channels from the highest byte down, so
 * each entry here is the source of r, g, b, a: 0-3 are the bytes from the
 * lowest up, 4 is zero, 5 is one. */
enum { SW_ZERO = 4, SW_ONE = 5 };
static const uint8_t k_swizzle4[8][4] = {
	{ 0, 1, 2, 3 },           /* ABGR */
	{ 2, 1, 0, 3 },           /* ARGB */
	{ 3, 2, 1, 0 },           /* RGBA */
	{ 1, 2, 3, 0 },           /* BGRA */
	{ 0, 1, 2, SW_ONE },      /* 1BGR */
	{ 2, 1, 0, SW_ONE },      /* 1RGB */
	{ 3, 2, 1, SW_ONE },      /* RGB1 */
	{ 1, 2, 3, SW_ONE },      /* BGR1 */
};
static const uint8_t k_swizzle1[8][4] = {
	{ 0, SW_ZERO, SW_ZERO, SW_ONE }, /* R, sampled the Vulkan way */
	{ 0, SW_ZERO, SW_ZERO, SW_ZERO }, /* 000R */
	{ 0, SW_ONE, SW_ONE, SW_ONE },   /* 111R */
	{ 0, 0, 0, 0 },                  /* RRRR */
	{ 0, 0, 0, SW_ZERO },            /* 0RRR */
	{ 0, 0, 0, SW_ONE },             /* 1RRR */
	{ SW_ZERO, SW_ZERO, SW_ZERO, 0 }, /* R000 */
	{ SW_ONE, SW_ONE, SW_ONE, 0 },   /* R111 */
};

static SceGxmTextureFormat vkv_view_swizzle(const VkvFormatInfo *fi, const VkComponentMapping *cm, bool *ok) {
	*ok = true;
	const VkComponentSwizzle want[4] = { cm->r, cm->g, cm->b, cm->a };
	bool identity = true;
	for (int k = 0; k < 4; k++)
		if (want[k] != VK_COMPONENT_SWIZZLE_IDENTITY && want[k] != (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + k))
			identity = false;
	if (identity || !vkv_format_sampleable(fi)) return fi->tex;
	uint32_t base = (uint32_t)fi->tex & 0x9F000000u, swz = ((uint32_t)fi->tex >> 12) & 7;
	const uint8_t (*table)[4];
	switch (base) {
	case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8: case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
	case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5: case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16:
	case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1: case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2: case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3:
		table = k_swizzle4;
		break;
	case SCE_GXM_TEXTURE_BASE_FORMAT_U8: case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
		table = k_swizzle1;
		break;
	default:
		*ok = false;
		return fi->tex;
	}
	/* where each Vulkan channel lives in memory, then where each output must come from */
	const uint8_t *id = table[swz];
	uint8_t need[4];
	for (int k = 0; k < 4; k++) {
		VkComponentSwizzle c = want[k] == VK_COMPONENT_SWIZZLE_IDENTITY ? (VkComponentSwizzle)(VK_COMPONENT_SWIZZLE_R + k)
		                                                               : want[k];
		if (c == VK_COMPONENT_SWIZZLE_ZERO) need[k] = SW_ZERO;
		else if (c == VK_COMPONENT_SWIZZLE_ONE) need[k] = SW_ONE;
		else need[k] = id[c - VK_COMPONENT_SWIZZLE_R];
	}
	for (uint32_t s = 0; s < 8; s++)
		if (!memcmp(table[s], need, 4)) return (SceGxmTextureFormat)(base | (s << 12));
	*ok = false;
	return fi->tex;
}

bool vkv_view_build_texture(VkvImageView *v) {
	const VkvFormatInfo *fi = vkv_format(v->format);
	VkvImage *img = v->image;
	v->texValid = false;
	if (!vkv_format_sampleable(fi) || !img->ptr) return false;
	if (fi->bpp != img->fi->bpp || fi->block != img->fi->block) {
		VKV_WARN("image view format %d has a different texel size than its image", (int)v->format);
		return false;
	}
	uint32_t w = vkv_image_level_width(img, v->baseLevel), h = vkv_image_level_height(img, v->baseLevel);
	void *data = vkv_image_level_ptr(img, v->baseLevel, v->cube ? 0 : v->baseLayer);
	int r;
	/* mipCount 0 tells GXM the cube has no mip chain, so the faces follow each other
	 * directly; any other count makes every face span a full chain, which is how
	 * mipmapped cubes are stored */
	if (v->cube && img->levels > 1 && v->baseLevel) {
		VKV_UNSUPPORTED("cube views that skip the first mip levels of a mipmapped cube");
		return false;
	}
	SceGxmTextureFormat tf = v->texFormat ? v->texFormat : fi->tex;
	if (v->cube) r = sceGxmTextureInitCube(&v->tex, data, tf, w, h, img->levels > 1 ? v->levelCount : 0);
	else if (img->swizzled) r = sceGxmTextureInitSwizzled(&v->tex, data, tf, w, h, v->levelCount);
	else r = sceGxmTextureInitLinear(&v->tex, data, tf, w, h, v->levelCount);
	if (r < 0) {
		VKV_ERROR("texture init (%ux%u fmt %d, %u levels%s) failed: 0x%08X", (unsigned)w, (unsigned)h, (int)v->format,
		          (unsigned)v->levelCount, v->cube ? ", cube" : "", (unsigned)r);
		return false;
	}
	if (fi->srgb) sceGxmTextureSetGammaMode(&v->tex, SCE_GXM_TEXTURE_GAMMA_BGR);
	sceGxmTextureSetMinFilter(&v->tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
	sceGxmTextureSetMagFilter(&v->tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
	v->texValid = true;
	return true;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc, VkImageView *pView) {
	VKV_TRACE("vkCreateImageView");
	(void)device; (void)alloc;
	VkvImage *img = VKV_ND(VkvImage, ci->image);
	if (VKV_CHECK(VKV_OBJ_OK(img, VKV_MAGIC_IMAGE), "invalid image")) return VK_ERROR_UNKNOWN;
	if (VKV_CHECK(img->ptr != NULL, "image has no memory bound")) return VK_ERROR_UNKNOWN;
	VkvImageView *v = calloc(1, sizeof(*v));
	if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
	v->obj.magic = VKV_MAGIC_VIEW;
	v->image = img;
	v->format = ci->format;
	v->baseLevel = VKV_MIN(ci->subresourceRange.baseMipLevel, img->levels - 1);
	uint32_t count = ci->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS
	                     ? img->levels - v->baseLevel : ci->subresourceRange.levelCount;
	v->levelCount = VKV_MAX(VKV_MIN(count, img->levels - v->baseLevel), 1u);
	v->baseLayer = VKV_MIN(ci->subresourceRange.baseArrayLayer, img->layers - 1);
	v->cube = img->cube && (ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE || ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
	uint32_t layerCount = ci->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS
	                          ? img->layers - v->baseLayer : ci->subresourceRange.layerCount;
	if (ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE && !img->cube)
		VKV_UNSUPPORTED("cube views of images created without CUBE_COMPATIBLE and 6 layers");
	else if (ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && layerCount > 6)
		VKV_UNSUPPORTED("cube array views (the first cube is used)");
	else if ((ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY || ci->viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY) && layerCount > 1)
		VKV_UNSUPPORTED("array views with several layers (GXM has no array textures, the first layer is used)");
	else if (ci->viewType == VK_IMAGE_VIEW_TYPE_1D || ci->viewType == VK_IMAGE_VIEW_TYPE_3D)
		VKV_UNSUPPORTED("1D and 3D views (treated as 2D)");
	bool swOk;
	v->texFormat = vkv_view_swizzle(vkv_format(ci->format) ? vkv_format(ci->format) : img->fi, &ci->components, &swOk);
	if (!swOk) VKV_UNSUPPORTED("this component swizzle (GXM has a fixed set per format, ignored)");
	if (!img->fi->isDepth) vkv_view_build_texture(v);
	*pView = VKV_H(VkImageView, v);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvImageView *v = VKV_ND(VkvImageView, view);
	if (!v) return;
	v->obj.magic = VKV_MAGIC_DEAD;
	free(v);
}

static SceGxmTextureAddrMode vkv_addr_mode(VkSamplerAddressMode m) {
	switch (m) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:               return SCE_GXM_TEXTURE_ADDR_REPEAT;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:      return SCE_GXM_TEXTURE_ADDR_MIRROR;
	case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		VKV_UNSUPPORTED("CLAMP_TO_BORDER (using CLAMP_TO_EDGE)");
		return SCE_GXM_TEXTURE_ADDR_CLAMP;
	default:                                           return SCE_GXM_TEXTURE_ADDR_CLAMP;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *ci,
                                               const VkAllocationCallbacks *alloc, VkSampler *pSampler) {
	(void)device; (void)alloc;
	VkvSampler *s = calloc(1, sizeof(*s));
	if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
	s->obj.magic = VKV_MAGIC_SAMPLER;
	s->minFilter = ci->minFilter == VK_FILTER_NEAREST ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
	s->magFilter = ci->magFilter == VK_FILTER_NEAREST ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
	s->mipFilter = ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
	                                                               : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
	s->addrU = vkv_addr_mode(ci->addressModeU);
	s->addrV = vkv_addr_mode(ci->addressModeV);
	/* maxLod limits how many levels are sampled; 0.25 is the usual "no mipmaps" value */
	float maxLod = ci->maxLod < 0.0f ? 0.0f : ci->maxLod;
	s->maxLevels = maxLod >= (float)VKV_MAX_LEVELS ? VKV_MAX_LEVELS : (uint32_t)maxLod + 1;
	if (ci->anisotropyEnable) VKV_UNSUPPORTED("anisotropic filtering");
	if (ci->compareEnable) VKV_UNSUPPORTED("depth compare samplers");
	if (ci->minLod > 0.0f) VKV_UNSUPPORTED("sampler minLod");
	/* GXM stores the bias in eighths around 31 (-3.875 to +4) */
	float bias = ci->mipLodBias * 8.0f + 31.0f;
	s->lodBias = bias <= 0.0f ? 0 : bias >= 63.0f ? 63 : (uint32_t)(bias + 0.5f);
	if (ci->mipLodBias < -3.875f || ci->mipLodBias > 4.0f) VKV_UNSUPPORTED("LOD bias beyond -3.875..4 (clamped)");
	*pSampler = VKV_H(VkSampler, s);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvSampler *s = VKV_ND(VkvSampler, sampler);
	if (!s) return;
	s->obj.magic = VKV_MAGIC_DEAD;
	free(s);
}

void vkv_texture_apply_sampler(SceGxmTexture *tex, const VkvSampler *s, uint32_t levels) {
	if (!s) return;
	sceGxmTextureSetMinFilter(tex, s->minFilter);
	sceGxmTextureSetMagFilter(tex, s->magFilter);
	sceGxmTextureSetUAddrMode(tex, s->addrU);
	sceGxmTextureSetVAddrMode(tex, s->addrV);
	if (s->lodBias != 31) sceGxmTextureSetLodBias(tex, s->lodBias);
	if (levels > 1) {
		sceGxmTextureSetMipFilter(tex, s->mipFilter);
		if (s->maxLevels < levels) sceGxmTextureSetMipmapCount(tex, s->maxLevels);
	}
}

/* ------------------------------------------------------------------------ */
/* Fences and semaphores                                                    */
/* ------------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------------ */
/* Pipeline cache (accepted, not persisted)                                 */
/* ------------------------------------------------------------------------ */
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
