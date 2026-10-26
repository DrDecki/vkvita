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
		    f->textureCompressionBC || f->shaderFloat64 || f->wideLines || f->largePoints)
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
	if (ci->usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
	                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT))
		VKV_UNSUPPORTED("storage/texel/indirect buffer usage (buffer created, but those uses will not work)");
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
void vkv_image_setup_layout(VkvImage *img) {
	if (img->fi->isDepth) {
		img->alignedW = VKV_ALIGN(img->width, SCE_GXM_TILE_SIZEX);
		img->alignedH = VKV_ALIGN(img->height, SCE_GXM_TILE_SIZEY);
		img->rowPitch = img->alignedW * 4;
		img->size = img->alignedW * img->alignedH * 4;
		img->alignment = 4096;
	} else {
		/* GXM linear textures and color surfaces use an 8 texel aligned stride */
		uint32_t stride = img->swapchain ? VKV_ALIGN(img->width, 64) : VKV_ALIGN(img->width, 8);
		img->alignedW = stride;
		img->alignedH = img->height;
		img->rowPitch = stride * img->fi->bpp;
		img->size = VKV_ALIGN(img->rowPitch * img->height, 16);
		img->alignment = (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? 4096 : 256;
	}
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
	if (ci->samples != VK_SAMPLE_COUNT_1_BIT) VKV_UNSUPPORTED("multisampled images (rendered single-sampled)");
	if (ci->mipLevels > 1) VKV_UNSUPPORTED("mipmapped images (only level 0 is stored and sampled)");
	if (ci->arrayLayers > 1) VKV_UNSUPPORTED("array/cube images (only layer 0 is stored)");
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
	img->usage = ci->usage;
	img->tiling = ci->tiling;
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
	(void)device; (void)sub;
	VkvImage *img = VKV_ND(VkvImage, image);
	l->offset = 0;
	l->size = img->size;
	l->rowPitch = img->rowPitch;
	l->arrayPitch = img->size;
	l->depthPitch = img->size;
}

/* ------------------------------------------------------------------------ */
/* Image views and samplers                                                 */
/* ------------------------------------------------------------------------ */
bool vkv_view_build_texture(VkvImageView *v) {
	const VkvFormatInfo *fi = vkv_format(v->format);
	VkvImage *img = v->image;
	v->texValid = false;
	if (!fi || !fi->tex || !img->ptr) return false;
	if (fi->bpp != img->fi->bpp) {
		VKV_WARN("image view format %d has a different texel size than its image", (int)v->format);
		return false;
	}
	int r = sceGxmTextureInitLinear(&v->tex, img->ptr, fi->tex, img->width, img->height, 0);
	if (r < 0) {
		VKV_ERROR("sceGxmTextureInitLinear(%ux%u fmt %d) failed: 0x%08X", (unsigned)img->width,
		          (unsigned)img->height, (int)v->format, (unsigned)r);
		return false;
	}
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
	if (ci->viewType != VK_IMAGE_VIEW_TYPE_2D) VKV_UNSUPPORTED("non-2D image views (treated as 2D)");
	if (ci->subresourceRange.baseMipLevel || ci->subresourceRange.baseArrayLayer)
		VKV_UNSUPPORTED("image views of mip levels/layers other than 0");
	VkvImageView *v = calloc(1, sizeof(*v));
	if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
	v->obj.magic = VKV_MAGIC_VIEW;
	v->image = img;
	v->format = ci->format;
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
	s->addrU = vkv_addr_mode(ci->addressModeU);
	s->addrV = vkv_addr_mode(ci->addressModeV);
	if (ci->anisotropyEnable) VKV_UNSUPPORTED("anisotropic filtering");
	if (ci->compareEnable) VKV_UNSUPPORTED("depth compare samplers");
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

void vkv_texture_apply_sampler(SceGxmTexture *tex, const VkvSampler *s) {
	if (!s) return;
	sceGxmTextureSetMinFilter(tex, s->minFilter);
	sceGxmTextureSetMagFilter(tex, s->magFilter);
	sceGxmTextureSetUAddrMode(tex, s->addrU);
	sceGxmTextureSetVAddrMode(tex, s->addrV);
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
	f->slot = vkv_notif_alloc(device);
	if (f->slot < 0) {
		VKV_ERROR("out of GXM notification slots (%d fences max)", VKV_NOTIF_SLOTS);
		free(f);
		return VK_ERROR_TOO_MANY_OBJECTS;
	}
	f->notif.address = &device->notifRegion[f->slot];
	f->notif.value = 0;
	*f->notif.address = 0;
	f->signaled = (ci->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
	*pFence = VKV_H(VkFence, f);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *alloc) {
	(void)alloc;
	VkvFence *f = VKV_ND(VkvFence, fence);
	if (!f) return;
	vkv_notif_free(device, f->slot);
	f->obj.magic = VKV_MAGIC_DEAD;
	free(f);
}

static bool vkv_fence_poll(VkvFence *f) {
	if (f->signaled) return true;
	if (f->pending && (int32_t)(*f->notif.address - f->notif.value) >= 0) {
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
