/*
 * VkVita: GPU memory blocks, the sub-allocator, device memory and buffers
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

/* --- Raw GPU memory --- */
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

/* --- Sub-allocator: first fit over a sorted free list per block --- */
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

void vkv_pool_destroy(VkvPool *pool) {
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


/* --- Device memory --- */
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

/* --- Buffers --- */
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

