/*
 * VkVita: query pools (occlusion through the GXM visibility buffer,
 * timestamps taken when the command buffer is submitted) and events.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

/* ------------------------------------------------------------------------ */
/* Query pools                                                              */
/* ------------------------------------------------------------------------ */
/* The visibility buffer is created and set together with the context
 * (vkv_vis_init), before the first scene, so the GPU never sees an unset one. */
bool vkv_vis_init(struct VkDevice_T *dev) {
	if (!vkv_gpu_alloc(VKV_HEAP_RAM, VKV_VIS_STRIDE * VKV_GPU_CORES, SCE_GXM_MEMORY_ATTRIB_RW, &dev->visBuf)) return false;
	memset(dev->visBuf.base, 0, VKV_VIS_STRIDE * VKV_GPU_CORES);
	int r = sceGxmSetVisibilityBuffer(dev->ctx, dev->visBuf.base, VKV_VIS_STRIDE);
	if (r < 0) {
		VKV_WARN("sceGxmSetVisibilityBuffer failed 0x%08X, occlusion queries are not available", (unsigned)r);
		vkv_gpu_free(&dev->visBuf);
		return false;
	}
	dev->visBufSet = true;
	return true;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc, VkQueryPool *pPool) {
	(void)alloc;
	if (ci->queryType != VK_QUERY_TYPE_OCCLUSION && ci->queryType != VK_QUERY_TYPE_TIMESTAMP) {
		VKV_UNSUPPORTED("pipeline statistics queries");
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	VkvQueryPool *p = calloc(1, sizeof(*p));
	if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
	p->obj.magic = VKV_MAGIC_QPOOL;
	p->type = ci->queryType;
	p->count = ci->queryCount;
	p->written = calloc(ci->queryCount ? ci->queryCount : 1, 1);
	if (!p->written) { free(p); return VK_ERROR_OUT_OF_HOST_MEMORY; }
	if (p->type == VK_QUERY_TYPE_TIMESTAMP) {
		p->values = calloc(ci->queryCount ? ci->queryCount : 1, sizeof(uint64_t));
		if (!p->values) { free(p->written); free(p); return VK_ERROR_OUT_OF_HOST_MEMORY; }
	} else {
		p->serial = calloc(ci->queryCount ? ci->queryCount : 1, sizeof(uint32_t));
		if (!p->serial) { free(p->written); free(p); return VK_ERROR_OUT_OF_HOST_MEMORY; }
		/* a contiguous run of free visibility slots */
		uint32_t run = 0, base = 0;
		bool found = false;
		for (uint32_t i = 0; i < VKV_VIS_SLOTS && !found; i++) {
			if (device->visUsed[i]) { run = 0; continue; }
			if (!run) base = i;
			if (++run == p->count) found = true;
		}
		if (!found || !device->visBufSet) {
			VKV_ERROR("vkCreateQueryPool: no room for %u occlusion queries (%d in total)", (unsigned)p->count,
			          VKV_VIS_SLOTS);
			free(p->serial);
			free(p->written);
			free(p);
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		memset(device->visUsed + base, 1, p->count);
		p->visBase = base;
	}
	*pPool = VKV_H(VkQueryPool, p);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks *alloc) {
	(void)alloc;
	VkvQueryPool *p = VKV_ND(VkvQueryPool, pool);
	if (!p) return;
	if (p->type == VK_QUERY_TYPE_OCCLUSION) memset(device->visUsed + p->visBase, 0, p->count);
	free(p->values);
	free(p->serial);
	free(p->written);
	p->obj.magic = VKV_MAGIC_DEAD;
	free(p);
}

/* True when no submitted scene can still write the counters of these queries. */
static bool vkv_queries_done(const struct VkDevice_T *dev, const VkvQueryPool *p, uint32_t first, uint32_t count) {
	for (uint32_t i = first; i < first + count; i++)
		if (p->written[i] && !vkv_serial_done(dev, p->serial[i])) return false;
	return true;
}

void vkv_query_reset(struct VkDevice_T *dev, VkvQueryPool *p, uint32_t first, uint32_t count) {
	if (p->type == VK_QUERY_TYPE_OCCLUSION && dev->gpuBusy && !vkv_queries_done(dev, p, first, count))
		vkv_device_wait_gpu(dev);
	memset(p->written + first, 0, count);
	if (p->type != VK_QUERY_TYPE_OCCLUSION) return;
	for (uint32_t c = 0; c < VKV_GPU_CORES; c++)
		memset((uint8_t *)dev->visBuf.base + c * VKV_VIS_STRIDE + (p->visBase + first) * 4, 0, count * 4);
}

void vkv_query_begin(struct VkDevice_T *dev, SceGxmContext *ctx, VkvQueryPool *p, uint32_t q) {
	p->written[q] = 1;
	if (p->serial) p->serial[q] = dev->sceneSerial + 1; /* the scene being recorded right now */
	if (p->type != VK_QUERY_TYPE_OCCLUSION || !dev->visBufSet) return;
	sceGxmSetFrontVisibilityTestIndex(ctx, p->visBase + q);
	sceGxmSetBackVisibilityTestIndex(ctx, p->visBase + q);
	sceGxmSetFrontVisibilityTestOp(ctx, SCE_GXM_VISIBILITY_TEST_OP_INCREMENT);
	sceGxmSetBackVisibilityTestOp(ctx, SCE_GXM_VISIBILITY_TEST_OP_INCREMENT);
	sceGxmSetFrontVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_ENABLED);
	sceGxmSetBackVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_ENABLED);
}

void vkv_query_end(struct VkDevice_T *dev, SceGxmContext *ctx, VkvQueryPool *p, uint32_t q) {
	if (p->serial && q < p->count) p->serial[q] = dev->sceneSerial + 1;
	sceGxmSetFrontVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_DISABLED);
	sceGxmSetBackVisibilityTestEnable(ctx, SCE_GXM_VISIBILITY_TEST_DISABLED);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count,
	size_t dataSize, void *data, VkDeviceSize stride, VkQueryResultFlags flags) {
	VkvQueryPool *p = VKV_ND(VkvQueryPool, pool);
	if (VKV_CHECK(VKV_OBJ_OK(p, VKV_MAGIC_QPOOL) && first + count <= p->count, "invalid query pool or range"))
		return VK_ERROR_UNKNOWN;
	/* occlusion results are ready once the scene that ended the query is done;
	 * only VK_QUERY_RESULT_WAIT_BIT blocks for that */
	if (p->type == VK_QUERY_TYPE_OCCLUSION && (flags & VK_QUERY_RESULT_WAIT_BIT) && device->gpuBusy &&
	    !vkv_queries_done(device, p, first, count))
		vkv_device_wait_gpu(device);
	bool is64 = (flags & VK_QUERY_RESULT_64_BIT) != 0;
	VkResult res = VK_SUCCESS;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t q = first + i;
		uint8_t *out = (uint8_t *)data + i * stride;
		if ((size_t)(i * stride + (is64 ? 8 : 4)) > dataSize) break;
		uint64_t v = 0;
		bool avail = p->written[q] != 0;
		if (p->type == VK_QUERY_TYPE_TIMESTAMP) {
			v = p->values[q];
		} else {
			for (uint32_t c = 0; c < VKV_GPU_CORES; c++)
				v += ((const uint32_t *)((const uint8_t *)device->visBuf.base + c * VKV_VIS_STRIDE))[p->visBase + q];
			avail = avail && vkv_serial_done(device, p->serial[q]);
			if (flags & VK_QUERY_RESULT_PARTIAL_BIT) avail = true;
		}
		if (!avail) res = VK_NOT_READY;
		if (avail || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
			if (is64) memcpy(out, &v, 8);
			else { uint32_t v32 = (uint32_t)v; memcpy(out, &v32, 4); }
		}
		if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
			uint64_t a = avail;
			if (is64) memcpy(out + 8, &a, 8);
			else { uint32_t a32 = avail; memcpy(out + 4, &a32, 4); }
		}
	}
	return res;
}

/* ------------------------------------------------------------------------ */
/* Events                                                                   */
/* ------------------------------------------------------------------------ */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(VkDevice device, const VkEventCreateInfo *ci,
                                             const VkAllocationCallbacks *alloc, VkEvent *pEvent) {
	(void)device; (void)ci; (void)alloc;
	VkvEvent *e = calloc(1, sizeof(*e));
	if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
	e->obj.magic = VKV_MAGIC_EVENT;
	*pEvent = VKV_H(VkEvent, e);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvEvent *e = VKV_ND(VkvEvent, event);
	if (!e) return;
	e->obj.magic = VKV_MAGIC_DEAD;
	free(e);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice device, VkEvent event) {
	(void)device;
	return VKV_ND(VkvEvent, event)->set ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent event) {
	(void)device;
	VKV_ND(VkvEvent, event)->set = true;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent event) {
	(void)device;
	VKV_ND(VkvEvent, event)->set = false;
	return VK_SUCCESS;
}
