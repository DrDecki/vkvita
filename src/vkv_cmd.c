/*
 * VkVita: command pools, command buffers and vkCmd* recording.
 * Commands are recorded into a byte arena and replayed at vkQueueSubmit
 * (see vkv_queue.c), which is where they become GXM calls.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

/* --- Pools --- */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc, VkCommandPool *pPool) {
	(void)device; (void)ci; (void)alloc;
	VkvCmdPool *p = calloc(1, sizeof(*p));
	if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
	p->obj.magic = VKV_MAGIC_CPOOL;
	*pPool = VKV_H(VkCommandPool, p);
	return VK_SUCCESS;
}

static void vkv_cb_unlink_free(VkvCmdPool *p, struct VkCommandBuffer_T *cb) {
	if (cb->prev) cb->prev->next = cb->next; else p->buffers = cb->next;
	if (cb->next) cb->next->prev = cb->prev;
	free(cb->data);
	cb->obj.magic = VKV_MAGIC_DEAD;
	free(cb);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvCmdPool *p = VKV_ND(VkvCmdPool, pool);
	if (!p) return;
	while (p->buffers) vkv_cb_unlink_free(p, p->buffers);
	p->obj.magic = VKV_MAGIC_DEAD;
	free(p);
}

static void vkv_cb_reset(struct VkCommandBuffer_T *cb, bool release) {
	cb->size = 0;
	if (release) { free(cb->data); cb->data = NULL; cb->cap = 0; }
	cb->state = VKV_CB_INITIAL;
	cb->inRenderPass = false;
	cb->pipeline = NULL;
	cb->rp = NULL;
	cb->sceneCount = 0;
	cb->oom = false;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
	(void)device;
	VkvCmdPool *p = VKV_ND(VkvCmdPool, pool);
	for (struct VkCommandBuffer_T *cb = p->buffers; cb; cb = cb->next)
		vkv_cb_reset(cb, (flags & VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT) != 0);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai,
                                                        VkCommandBuffer *pBuffers) {
	VkvCmdPool *p = VKV_ND(VkvCmdPool, ai->commandPool);
	for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
		struct VkCommandBuffer_T *cb = calloc(1, sizeof(*cb));
		if (!cb) return VK_ERROR_OUT_OF_HOST_MEMORY;
		cb->obj.magic = VKV_MAGIC_CMDBUF;
		cb->device = device;
		cb->pool = p;
		cb->secondary = ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
		cb->next = p->buffers;
		if (p->buffers) p->buffers->prev = cb;
		p->buffers = cb;
		pBuffers[i] = cb;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
                                                const VkCommandBuffer *buffers) {
	(void)device;
	VkvCmdPool *p = VKV_ND(VkvCmdPool, pool);
	for (uint32_t i = 0; i < count; i++)
		if (buffers[i]) vkv_cb_unlink_free(p, buffers[i]);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
	VKV_TRACE("vkBeginCommandBuffer");
	if (VKV_CHECK(VKV_OBJ_OK(cb, VKV_MAGIC_CMDBUF), "invalid command buffer")) return VK_ERROR_UNKNOWN;
	vkv_cb_reset(cb, false);
	cb->state = VKV_CB_RECORDING;
	cb->usage = bi ? bi->flags : 0;
	if (cb->secondary && bi && (bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) && bi->pInheritanceInfo) {
		cb->inRenderPass = true;
		cb->rp = VKV_ND(VkvRenderPass, bi->pInheritanceInfo->renderPass);
		cb->subpass = bi->pInheritanceInfo->subpass;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cb) {
	VKV_TRACE("vkEndCommandBuffer");
	if (VKV_CHECK(cb->state == VKV_CB_RECORDING, "vkEndCommandBuffer on a buffer that is not recording"))
		return VK_ERROR_UNKNOWN;
	if (VKV_CHECK(cb->secondary || !cb->inRenderPass, "vkEndCommandBuffer inside a render pass")) return VK_ERROR_UNKNOWN;
	cb->state = VKV_CB_EXECUTABLE;
	return cb->oom ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
	vkv_cb_reset(cb, (flags & VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT) != 0);
	return VK_SUCCESS;
}

void *vkv_cmd_alloc(struct VkCommandBuffer_T *cb, VkvCmdType type, uint32_t size) {
	size = VKV_ALIGN(size, 8);
	if (cb->state != VKV_CB_RECORDING) {
		if (vkv_validate()) vkv_fail("vkCmd*", "recording into a command buffer that is not in the recording state");
		if (cb->state == VKV_CB_INITIAL) return NULL;
	}
	if (cb->size + size > cb->cap) {
		uint32_t ncap = cb->cap ? cb->cap * 2 : 4096;
		while (ncap < cb->size + size) ncap *= 2;
		uint8_t *n = realloc(cb->data, ncap);
		if (!n) { cb->oom = true; return NULL; }
		cb->data = n;
		cb->cap = ncap;
	}
	VkvCmd *c = (VkvCmd *)(cb->data + cb->size);
	memset(c, 0, size);
	c->type = (uint16_t)type;
	c->size = size;
	cb->size += size;
	return c;
}

#define VKV_CMD(cb, T, type) ((T *)vkv_cmd_alloc((cb), (type), sizeof(T)))

/* --- Render passes --- */
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                VkSubpassContents contents) {
	VKV_TRACE("vkCmdBeginRenderPass");
	(void)contents;
	if (VKV_CHECK(!cb->inRenderPass, "vkCmdBeginRenderPass inside a render pass")) return;
	VkvRenderPass *rp = VKV_ND(VkvRenderPass, bi->renderPass);
	VkvFramebuffer *fb = VKV_ND(VkvFramebuffer, bi->framebuffer);
	if (VKV_CHECK(VKV_OBJ_OK(rp, VKV_MAGIC_RPASS) && VKV_OBJ_OK(fb, VKV_MAGIC_FBUF), "invalid render pass/framebuffer"))
		return;
	VkvCmdBeginRP *c = VKV_CMD(cb, VkvCmdBeginRP, VKV_CMD_BEGIN_RP);
	if (!c) return;
	c->rp = rp;
	c->fb = fb;
	c->area = bi->renderArea;
	c->clearCount = VKV_MIN(bi->clearValueCount, 8);
	for (uint32_t i = 0; i < c->clearCount; i++) c->clears[i] = bi->pClearValues[i];
	cb->inRenderPass = true;
	cb->rp = rp;
	cb->subpass = 0;
	cb->sceneCount++;
}

VKAPI_ATTR void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer cb, VkSubpassContents contents) {
	VKV_TRACE("vkCmdNextSubpass");
	(void)contents;
	if (VKV_CHECK(cb->inRenderPass && cb->rp, "vkCmdNextSubpass outside a render pass")) return;
	if (VKV_CHECK(cb->subpass + 1 < cb->rp->subpassCount, "vkCmdNextSubpass past the last subpass")) return;
	VKV_CMD(cb, VkvCmd, VKV_CMD_NEXT_SUBPASS);
	cb->subpass++;
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer cb) {
	VKV_TRACE("vkCmdEndRenderPass");
	if (VKV_CHECK(cb->inRenderPass, "vkCmdEndRenderPass outside a render pass")) return;
	VKV_CMD(cb, VkvCmd, VKV_CMD_END_RP);
	cb->inRenderPass = false;
}

/* --- State --- */
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipeline) {
	VKV_TRACE("vkCmdBindPipeline");
	if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) { VKV_UNSUPPORTED("compute bind point"); return; }
	VkvPipeline *p = VKV_ND(VkvPipeline, pipeline);
	if (VKV_CHECK(VKV_OBJ_OK(p, VKV_MAGIC_PIPELINE), "invalid pipeline")) return;
	VkvCmdBindPipeline *c = VKV_CMD(cb, VkvCmdBindPipeline, VKV_CMD_BIND_PIPELINE);
	if (!c) return;
	c->p = p;
	cb->pipeline = p;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                                  const VkBuffer *buffers, const VkDeviceSize *offsets) {
	if (VKV_CHECK(first + count <= VKV_MAX_BINDINGS, "vertex binding %u out of range", (unsigned)(first + count)))
		return;
	VkvCmdBindVB *c = VKV_CMD(cb, VkvCmdBindVB, VKV_CMD_BIND_VB);
	if (!c) return;
	c->first = first;
	c->count = count;
	for (uint32_t i = 0; i < count; i++) {
		c->buf[i] = VKV_ND(VkvBuffer, buffers[i]);
		c->off[i] = (uint32_t)offsets[i];
		if (VKV_CHECK(!c->buf[i] || c->buf[i]->ptr, "vertex buffer at binding %u has no memory bound",
		              (unsigned)(first + i)))
			c->buf[i] = NULL;
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                                VkIndexType type) {
	if (type != VK_INDEX_TYPE_UINT16 && type != VK_INDEX_TYPE_UINT32) {
		VKV_UNSUPPORTED("8-bit indices");
		return;
	}
	VkvCmdBindIB *c = VKV_CMD(cb, VkvCmdBindIB, VKV_CMD_BIND_IB);
	if (!c) return;
	c->buf = VKV_ND(VkvBuffer, buffer);
	c->off = (uint32_t)offset;
	c->type = type;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
	uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynCount, const uint32_t *dynOffsets) {
	VKV_TRACE("vkCmdBindDescriptorSets");
	(void)bp; (void)layout;
	if (VKV_CHECK(first + count <= VKV_MAX_SETS, "descriptor set index out of range")) return;
	VkvCmdBindSets *c = VKV_CMD(cb, VkvCmdBindSets, VKV_CMD_BIND_SETS);
	if (!c) return;
	c->first = first;
	c->count = count;
	uint32_t needDyn = 0;
	for (uint32_t i = 0; i < count; i++) {
		c->sets[i] = VKV_ND(VkvSet, sets[i]);
		if (c->sets[i]) needDyn += c->sets[i]->layout->dynamicCount;
	}
	if (VKV_CHECK(dynCount == needDyn, "expected %u dynamic offsets, got %u", (unsigned)needDyn, (unsigned)dynCount))
		dynCount = VKV_MIN(dynCount, needDyn);
	c->dynCount = VKV_MIN(dynCount, VKV_MAX_SETS * VKV_MAX_DYNAMIC);
	for (uint32_t i = 0; i < c->dynCount; i++) c->dyn[i] = dynOffsets[i];
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                              uint32_t offset, uint32_t size, const void *values) {
	(void)layout; (void)stages;
	if (VKV_CHECK(offset + size <= VKV_MAX_PUSH, "push constants [%u..%u) exceed %d bytes", (unsigned)offset,
	              (unsigned)(offset + size), VKV_MAX_PUSH))
		return;
	VkvCmdPush *c = vkv_cmd_alloc(cb, VKV_CMD_PUSH, sizeof(VkvCmdPush) + size);
	if (!c) return;
	c->offset = offset;
	c->size = size;
	memcpy(c->data, values, size);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *vps) {
	if (first != 0 || count < 1) return;
	VkvCmdViewport *c = VKV_CMD(cb, VkvCmdViewport, VKV_CMD_VIEWPORT);
	if (c) c->vp = vps[0];
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *rects) {
	if (first != 0 || count < 1) return;
	VkvCmdScissor *c = VKV_CMD(cb, VkvCmdScissor, VKV_CMD_SCISSOR);
	if (c) c->rect = rects[0];
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope) {
	(void)clamp;
	VkvCmdDepthBias *c = VKV_CMD(cb, VkvCmdDepthBias, VKV_CMD_DEPTH_BIAS);
	if (c) { c->constant = constant; c->slope = slope; }
}

static void vkv_cmd_stencil(VkCommandBuffer cb, uint32_t what, VkStencilFaceFlags faces, uint32_t v) {
	VkvCmdStencil *c = VKV_CMD(cb, VkvCmdStencil, VKV_CMD_STENCIL);
	if (c) { c->what = what; c->faces = faces; c->value = v; }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
	vkv_cmd_stencil(cb, VKV_DYN_STENCIL_CMP, f, m);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
	vkv_cmd_stencil(cb, VKV_DYN_STENCIL_WR, f, m);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t r) {
	vkv_cmd_stencil(cb, VKV_DYN_STENCIL_REF, f, r);
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer cb, float w) {
	VkvCmdLineWidth *c = VKV_CMD(cb, VkvCmdLineWidth, VKV_CMD_LINE_WIDTH);
	if (c) c->width = w;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
	(void)cb; (void)c;
	VKV_UNSUPPORTED("blend constants");
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer cb, float a, float b) {
	(void)cb; (void)a; (void)b;
	VKV_UNSUPPORTED("depth bounds");
}

/* --- Draws --- */
static bool vkv_draw_checks(VkCommandBuffer cb, const char *fn) {
	if (!vkv_validate()) return true;
	if (!cb->inRenderPass) { vkv_fail(fn, "draw outside a render pass"); return false; }
	if (!cb->pipeline) { vkv_fail(fn, "draw without a bound pipeline"); return false; }
	const VkvRenderPass *prp = cb->pipeline->rp, *crp = cb->rp;
	if (prp && crp && cb->subpass < crp->subpassCount && prp->sub[cb->pipeline->subpass].color >= 0 &&
	    crp->sub[cb->subpass].color >= 0) {
		VkFormat a = prp->att[prp->sub[cb->pipeline->subpass].color].format;
		VkFormat b = crp->att[crp->sub[cb->subpass].color].format;
		if (vkv_format(a) && vkv_format(b) && vkv_format(a)->outReg != vkv_format(b)->outReg) {
			vkv_fail(fn, "pipeline was created for an incompatible render pass color format");
			return false;
		}
	}
	if (crp && cb->pipeline->samples != crp->samples) {
		vkv_fail(fn, "pipeline has %u samples, the render pass %u", (unsigned)cb->pipeline->samples,
		         (unsigned)crp->samples);
		return false;
	}
	return true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount,
                                     uint32_t firstVertex, uint32_t firstInstance) {
	VKV_TRACE("vkCmdDraw");
	if (!vertexCount || !instanceCount || !vkv_draw_checks(cb, __func__)) return;
	VkvCmdDraw *c = VKV_CMD(cb, VkvCmdDraw, VKV_CMD_DRAW);
	if (!c) return;
	c->count = vertexCount;
	c->instances = instanceCount;
	c->first = firstVertex;
	c->firstInstance = firstInstance;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount,
                                            uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
	VKV_TRACE("vkCmdDrawIndexed");
	if (!indexCount || !instanceCount || !vkv_draw_checks(cb, __func__)) return;
	VkvCmdDraw *c = VKV_CMD(cb, VkvCmdDraw, VKV_CMD_DRAW_INDEXED);
	if (!c) return;
	c->count = indexCount;
	c->instances = instanceCount;
	c->first = firstIndex;
	c->vertexOffset = vertexOffset;
	c->firstInstance = firstInstance;
}

/* The draw parameters are read from the buffer when the command buffer is submitted. */
static void vkv_record_indirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s, bool indexed,
                                const char *fn) {
	if (!n || !vkv_draw_checks(cb, fn)) return;
	VkvBuffer *buf = VKV_ND(VkvBuffer, b);
	if (VKV_CHECK(VKV_OBJ_OK(buf, VKV_MAGIC_BUFFER) && buf->ptr, "%s: invalid indirect buffer", fn)) return;
	VkvCmdDrawIndirect *c = VKV_CMD(cb, VkvCmdDrawIndirect, VKV_CMD_DRAW_INDIRECT);
	if (!c) return;
	c->buf = buf;
	c->off = (uint32_t)o;
	c->count = n;
	c->stride = n > 1 ? s : (indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand));
	c->indexed = indexed;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
	VKV_TRACE("vkCmdDrawIndirect");
	vkv_record_indirect(cb, b, o, n, s, false, __func__);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n,
                                                    uint32_t s) {
	VKV_TRACE("vkCmdDrawIndexedIndirect");
	vkv_record_indirect(cb, b, o, n, s, true, __func__);
}

/* --- Transfers, clears, barriers --- */
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n,
                                           const VkBufferCopy *regions) {
	VKV_TRACE("vkCmdCopyBuffer");
	VkvCmdCopyBuffer *c = vkv_cmd_alloc(cb, VKV_CMD_COPY_BUFFER, sizeof(VkvCmdCopyBuffer) + n * sizeof(VkBufferCopy));
	if (!c) return;
	c->src = VKV_ND(VkvBuffer, src);
	c->dst = VKV_ND(VkvBuffer, dst);
	c->n = n;
	memcpy(c->r, regions, n * sizeof(VkBufferCopy));
	for (uint32_t i = 0; i < n; i++)
		if (VKV_CHECK(regions[i].srcOffset + regions[i].size <= c->src->size &&
		              regions[i].dstOffset + regions[i].size <= c->dst->size, "copy region %u out of bounds", (unsigned)i))
			c->r[i].size = 0;
}

static void vkv_record_buf_img(VkCommandBuffer cb, VkvCmdType type, VkBuffer buf, VkImage img, uint32_t n,
                               const VkBufferImageCopy *regions) {
	VkvCmdCopyBufImg *c = vkv_cmd_alloc(cb, type, sizeof(VkvCmdCopyBufImg) + n * sizeof(VkBufferImageCopy));
	if (!c) return;
	c->buf = VKV_ND(VkvBuffer, buf);
	c->img = VKV_ND(VkvImage, img);
	c->n = n;
	memcpy(c->r, regions, n * sizeof(VkBufferImageCopy));
	if (vkv_validate() && c->img->fi->isDepth) vkv_fail(__func__, "copies to/from depth images are not supported");
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
                                                  uint32_t n, const VkBufferImageCopy *regions) {
	VKV_TRACE("vkCmdCopyBufferToImage");
	(void)layout;
	vkv_record_buf_img(cb, VKV_CMD_COPY_BUF_IMG, src, dst, n, regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst,
                                                  uint32_t n, const VkBufferImageCopy *regions) {
	(void)layout;
	vkv_record_buf_img(cb, VKV_CMD_COPY_IMG_BUF, dst, src, n, regions);
}

VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size,
                                             const void *data) {
	VkvCmdUpdateBuffer *c = vkv_cmd_alloc(cb, VKV_CMD_UPDATE_BUFFER, sizeof(VkvCmdUpdateBuffer) + (uint32_t)size);
	if (!c) return;
	c->dst = VKV_ND(VkvBuffer, dst);
	c->off = (uint32_t)off;
	c->size = (uint32_t)size;
	memcpy(c->data, data, size);
}

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size,
                                           uint32_t value) {
	VkvCmdFillBuffer *c = VKV_CMD(cb, VkvCmdFillBuffer, VKV_CMD_FILL_BUFFER);
	if (!c) return;
	c->dst = VKV_ND(VkvBuffer, dst);
	c->off = (uint32_t)off;
	c->size = size == VK_WHOLE_SIZE ? c->dst->size - c->off : (uint32_t)size;
	c->value = value;
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *atts,
                                                 uint32_t nr, const VkClearRect *rects) {
	if (VKV_CHECK(cb->inRenderPass, "vkCmdClearAttachments outside a render pass")) return;
	na = VKV_MIN(na, 8);
	VkvCmdClearAttach *c = vkv_cmd_alloc(cb, VKV_CMD_CLEAR_ATTACH, sizeof(VkvCmdClearAttach) + nr * sizeof(VkClearRect));
	if (!c) return;
	c->na = na;
	c->nr = nr;
	memcpy(c->a, atts, na * sizeof(VkClearAttachment));
	memcpy(c->r, rects, nr * sizeof(VkClearRect));
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                                                const VkClearColorValue *color, uint32_t n,
                                                const VkImageSubresourceRange *ranges) {
	(void)layout;
	if (VKV_CHECK(!cb->inRenderPass, "vkCmdClearColorImage inside a render pass")) return;
	VkvCmdClearColorImage *c = vkv_cmd_alloc(cb, VKV_CMD_CLEAR_COLOR_IMAGE,
	                                         sizeof(VkvCmdClearColorImage) + n * sizeof(VkImageSubresourceRange));
	if (!c) return;
	c->img = VKV_ND(VkvImage, image);
	c->c = *color;
	c->n = n;
	memcpy(c->ranges, ranges, n * sizeof(VkImageSubresourceRange));
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
	const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *ranges) {
	(void)layout; (void)n; (void)ranges;
	if (VKV_CHECK(!cb->inRenderPass, "vkCmdClearDepthStencilImage inside a render pass")) return;
	VkvCmdClearDSImage *c = VKV_CMD(cb, VkvCmdClearDSImage, VKV_CMD_CLEAR_DS_IMAGE);
	if (!c) return;
	c->img = VKV_ND(VkvImage, image);
	c->v = *v;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
	VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb,
	uint32_t ni, const VkImageMemoryBarrier *ib) {
	(void)dep; (void)nm; (void)mb; (void)nb; (void)bb; (void)ni; (void)ib;
	/* Layout transitions are no-ops (all images are linear). A barrier
	 * between GPU writes and transfer reads needs the GPU to finish first. */
	if ((src & (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
	            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) &&
	    (dst & (VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)))
		VKV_CMD(cb, VkvCmd, VKV_CMD_BARRIER);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                          uint32_t n, const VkImageCopy *r) {
	VKV_TRACE("vkCmdCopyImage");
	(void)sl; (void)dl;
	VkvCmdCopyImage *c = vkv_cmd_alloc(cb, VKV_CMD_COPY_IMAGE, sizeof(VkvCmdCopyImage) + n * sizeof(VkImageCopy));
	if (!c) return;
	c->src = VKV_ND(VkvImage, s);
	c->dst = VKV_ND(VkvImage, d);
	c->n = n;
	memcpy(c->r, r, n * sizeof(VkImageCopy));
	if (vkv_validate() && (c->src->fi->bpp != c->dst->fi->bpp || c->src->fi->block != c->dst->fi->block))
		vkv_fail(__func__, "source and destination formats have different texel sizes");
}

/* Multisampled color images hold the resolved pixels already, so resolving
 * is a copy. */
VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                             VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
	VKV_TRACE("vkCmdResolveImage");
	VkImageCopy copies[16];
	while (n) {
		uint32_t k = VKV_MIN(n, 16u);
		for (uint32_t i = 0; i < k; i++) {
			copies[i].srcSubresource = r[i].srcSubresource;
			copies[i].srcOffset = r[i].srcOffset;
			copies[i].dstSubresource = r[i].dstSubresource;
			copies[i].dstOffset = r[i].dstOffset;
			copies[i].extent = r[i].extent;
		}
		vkCmdCopyImage(cb, s, sl, d, dl, k, copies);
		r += k;
		n -= k;
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                          uint32_t n, const VkImageBlit *r, VkFilter f) {
	VKV_TRACE("vkCmdBlitImage");
	(void)sl; (void)dl;
	VkvCmdBlitImage *c = vkv_cmd_alloc(cb, VKV_CMD_BLIT_IMAGE, sizeof(VkvCmdBlitImage) + n * sizeof(VkImageBlit));
	if (!c) return;
	c->src = VKV_ND(VkvImage, s);
	c->dst = VKV_ND(VkvImage, d);
	c->filter = f;
	c->n = n;
	memcpy(c->r, r, n * sizeof(VkImageBlit));
	if (vkv_validate() && (c->src->fi->block || c->dst->fi->block))
		vkv_fail(__func__, "blits between compressed images are not possible");
}

VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs) {
	VKV_TRACE("vkCmdExecuteCommands");
	VkvCmdExecute *c = vkv_cmd_alloc(cb, VKV_CMD_EXECUTE, sizeof(VkvCmdExecute) + n * sizeof(cbs[0]));
	if (!c) return;
	c->n = n;
	for (uint32_t i = 0; i < n; i++) {
		c->cbs[i] = cbs[i];
		if (VKV_CHECK(cbs[i] && cbs[i]->secondary, "vkCmdExecuteCommands: buffer %u is not a secondary command buffer",
		              (unsigned)i))
			c->cbs[i] = NULL;
	}
}

/* --- Queries and events --- */
static void vkv_record_query(VkCommandBuffer cb, VkvCmdType type, VkQueryPool pool, uint32_t first, uint32_t count) {
	VkvQueryPool *p = VKV_ND(VkvQueryPool, pool);
	if (VKV_CHECK(VKV_OBJ_OK(p, VKV_MAGIC_QPOOL), "invalid query pool")) return;
	if (VKV_CHECK(first + count <= p->count, "query %u out of range", (unsigned)(first + count - 1))) return;
	VkvCmdQuery *c = VKV_CMD(cb, VkvCmdQuery, type);
	if (!c) return;
	c->pool = p;
	c->first = first;
	c->count = count;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query, VkQueryControlFlags f) {
	(void)f;
	if (VKV_CHECK(cb->inRenderPass, "vkCmdBeginQuery outside a render pass (GXM counts inside scenes)")) return;
	vkv_record_query(cb, VKV_CMD_QUERY_BEGIN, pool, query, 1);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool pool, uint32_t query) {
	vkv_record_query(cb, VKV_CMD_QUERY_END, pool, query, 1);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count) {
	vkv_record_query(cb, VKV_CMD_QUERY_RESET, pool, first, count);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool,
                                               uint32_t query) {
	(void)stage;
	vkv_record_query(cb, VKV_CMD_TIMESTAMP, pool, query, 1);
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer cb, VkQueryPool pool, uint32_t first,
	uint32_t count, VkBuffer dst, VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags flags) {
	VKV_TRACE("vkCmdCopyQueryPoolResults");
	VkvQueryPool *p = VKV_ND(VkvQueryPool, pool);
	VkvBuffer *b = VKV_ND(VkvBuffer, dst);
	if (VKV_CHECK(VKV_OBJ_OK(p, VKV_MAGIC_QPOOL), "invalid query pool")) return;
	if (VKV_CHECK(first + count <= p->count, "query %u out of range", (unsigned)(first + count - 1))) return;
	if (VKV_CHECK(!cb->inRenderPass, "vkCmdCopyQueryPoolResults inside a render pass")) return;
	if (VKV_CHECK(b && off < b->size, "vkCmdCopyQueryPoolResults: invalid buffer or offset")) return;
	VkvCmdCopyQuery *c = VKV_CMD(cb, VkvCmdCopyQuery, VKV_CMD_COPY_QUERY);
	if (!c) return;
	c->pool = p;
	c->first = first;
	c->count = count;
	c->dst = b;
	c->off = (uint32_t)off;
	c->stride = (uint32_t)stride;
	c->flags = flags;
}

static void vkv_record_event(VkCommandBuffer cb, VkEvent event, bool value) {
	VkvCmdSetEvent *c = VKV_CMD(cb, VkvCmdSetEvent, VKV_CMD_SET_EVENT);
	if (!c) return;
	c->event = VKV_ND(VkvEvent, event);
	c->value = value;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer cb, VkEvent event, VkPipelineStageFlags stage) {
	(void)stage;
	vkv_record_event(cb, event, true);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer cb, VkEvent event, VkPipelineStageFlags stage) {
	(void)stage;
	vkv_record_event(cb, event, false);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer cb, uint32_t n, const VkEvent *events,
	VkPipelineStageFlags src, VkPipelineStageFlags dst, uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb,
	const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
	(void)n; (void)events;
	/* one in-order queue: waiting on an event is a barrier */
	vkCmdPipelineBarrier(cb, src, dst, 0, nm, mb, nb, bb, ni, ib);
}
