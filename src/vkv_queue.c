/*
 * VkVita: queue submission, scenes and the command replay loop.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_replay.h"

/* --- Scenes --- */
void vkv_begin_scene(VkvReplay *R, bool loadDepth, bool clearDepth, float depth, uint32_t stencil) {
	VkvRenderPass *rp = R->rp;
	VkvFramebuffer *fb = R->fb;
	if (R->depthImg && VKV_CHECK(R->depthImg->samples == rp->samples, "depth attachment has %u samples, the render pass %u",
	                             (unsigned)R->depthImg->samples, (unsigned)rp->samples))
		R->depthImg = NULL;
	if (R->depthImg && R->depthImg->pendingDepthClear) {
		if (loadDepth || !clearDepth) {
			loadDepth = false;
			clearDepth = true;
			depth = R->depthImg->clearDepth;
			stencil = R->depthImg->clearStencil;
		}
		R->depthImg->pendingDepthClear = false;
	}
	R->sceneDraws = 0;
	R->sceneClearsDepth = R->depthImg && clearDepth;
	R->sceneDepth = depth;
	R->sceneStencil = stencil;
	if (R->depthImg) {
		const VkvSubpass *sp = &rp->sub[R->subpass];
		const VkvAttachmentDesc *a = &rp->att[sp->depth];
		uint8_t *dptr = R->depthImg->ptr + (size_t)(R->depthView ? R->depthView->baseLayer : 0) * R->depthImg->layerSize;
		sceGxmDepthStencilSurfaceInit(&R->depth, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
		                              R->depthImg->alignedW, dptr, NULL);
		if (clearDepth) {
			sceGxmDepthStencilSurfaceSetBackgroundDepth(&R->depth, depth);
			sceGxmDepthStencilSurfaceSetBackgroundStencil(&R->depth, (unsigned char)stencil);
		}
		sceGxmDepthStencilSurfaceSetForceLoadMode(&R->depth, loadDepth ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED
		                                                                : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
		/* later subpasses in another scene need the depth buffer too */
		bool store = R->splits || rp->lastUse[sp->depth] > (int8_t)sp->runEnd ||
		             a->storeOp == VK_ATTACHMENT_STORE_OP_STORE || a->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE;
		sceGxmDepthStencilSurfaceSetForceStoreMode(&R->depth, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
		                                                            : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
	}
	if (R->colorImg) {
		VkvImage *img = R->colorImg;
		SceGxmColorFormat cf = img->swapchain ? SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR : img->fi->color;
		SceGxmOutputRegisterSize rs = img->fi->bpp == 8 ? SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT
		                                                : SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
		uint32_t level = R->colorView ? R->colorView->baseLevel : 0;
		uint32_t layer = R->colorView ? R->colorView->baseLayer : 0;
		/* with multisampling GXM averages the samples when it writes the tile back */
		SceGxmColorSurfaceScaleMode scale = rp->samples > 1 ? SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE
		                                                    : SCE_GXM_COLOR_SURFACE_SCALE_NONE;
		int r = sceGxmColorSurfaceInit(&R->color, cf, SCE_GXM_COLOR_SURFACE_LINEAR, scale, rs, fb->width, fb->height,
		                               img->levelPitch[level] / img->fi->bpp, vkv_image_level_ptr(img, level, layer));
		if (r < 0) VKV_ERROR("sceGxmColorSurfaceInit failed 0x%08X", (unsigned)r);
		const VkvFormatInfo *vf = R->colorView ? vkv_format(R->colorView->format) : img->fi;
		if (vf && vf->srgb) sceGxmColorSurfaceSetGammaMode(&R->color, SCE_GXM_COLOR_SURFACE_GAMMA_BGR);
	} else {
		sceGxmColorSurfaceInitDisabled(&R->color);
	}
	unsigned int flags = 0;
	if (R->dev->transferPending) {
		flags |= SCE_GXM_SCENE_FRAGMENT_TRANSFER_SYNC;
		R->dev->transferPending = false;
	}
	int r = sceGxmBeginScene(R->ctx, flags, fb->rt, NULL, NULL, R->colorImg ? R->colorImg->sync : NULL, &R->color,
	                         R->depthImg ? &R->depth : NULL);
	if (r < 0) {
		VKV_ERROR("sceGxmBeginScene failed 0x%08X", (unsigned)r);
		return;
	}
	R->inScene = true;
	sceGxmSetViewportEnable(R->ctx, SCE_GXM_VIEWPORT_ENABLED);
	R->pipeDirty = true;
	R->vpVariant = -1;
	R->viewportDirty = true;
	R->scissorDirty = true;
	g_vkv.cur.scenes++;
}

void vkv_end_scene(VkvReplay *R, bool final) {
	if (!R->inScene) return;
	R->transferTail = false; /* the scene waited for earlier transfers, and the fence follows it */
	R->dev->progress.value = ++R->dev->sceneSerial;
	if (final && R->fence && R->scenesLeft == 1) {
		R->fence->serial = R->dev->sceneSerial;
		R->fenceAttached = true;
	}
	int r = sceGxmEndScene(R->ctx, NULL, &R->dev->progress);
	if (r < 0) VKV_ERROR("sceGxmEndScene failed 0x%08X", (unsigned)r);
	R->inScene = false;
	R->dev->gpuBusy = true;
	/* A scene without draws does not write its depth buffer back, so a clear
	 * it was meant to do would be lost. The next scene does it instead. */
	if (R->depthImg && R->sceneClearsDepth && !R->sceneDraws) {
		R->depthImg->pendingDepthClear = true;
		R->depthImg->clearDepth = R->sceneDepth;
		R->depthImg->clearStencil = R->sceneStencil;
		return;
	}
	if (R->depthImg) R->depthImg->written = true;
}

static bool vkv_pass_has_splits(const uint8_t *p, const uint8_t *end) {
	while (p < end) {
		const VkvCmd *c = (const VkvCmd *)p;
		if (c->type == VKV_CMD_END_RP || c->type == VKV_CMD_NEXT_SUBPASS) return false;
		if (c->type == VKV_CMD_CLEAR_ATTACH) return true;
		p += c->size;
	}
	return false;
}

/* Starts the scene of one subpass. Load and clear ops apply where an
 * attachment is first used, everything later loads what came before. */
static void vkv_start_subpass(VkvReplay *R, uint32_t idx, const uint8_t *next, const uint8_t *end) {
	VkvRenderPass *rp = R->rp;
	VkvFramebuffer *fb = R->fb;
	const VkvCmdBeginRP *c = R->begin;
	const VkvSubpass *sp = &rp->sub[idx];
	R->subpass = idx;
	/* a multisampled attachment that gets resolved renders straight into the
	 * resolve target, GXM downscales on the way out */
	R->colorView = sp->resolve >= 0 ? fb->views[sp->resolve] : sp->color >= 0 ? fb->views[sp->color] : NULL;
	R->colorImg = R->colorView ? R->colorView->image : NULL;
	R->depthView = sp->depth >= 0 ? fb->views[sp->depth] : NULL;
	R->depthImg = R->depthView ? R->depthView->image : NULL;
	R->splits = vkv_pass_has_splits(next, end);

	if (R->colorImg && rp->firstUse[sp->color] == (int8_t)idx) {
		const VkvAttachmentDesc *a = &rp->att[sp->color];
		if (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)sp->color < c->clearCount)
			vkv_clear_color_rect(R, R->colorImg, R->colorView->baseLevel, R->colorView->baseLayer,
			                     &c->clears[sp->color].color, (uint32_t)c->area.offset.x,
			                     (uint32_t)c->area.offset.y, c->area.extent.width, c->area.extent.height);
	}
	bool loadDepth = false, clearDepth = false;
	float d = 1.0f;
	uint32_t s = 0;
	if (R->depthImg) {
		const VkvAttachmentDesc *a = &rp->att[sp->depth];
		if (rp->firstUse[sp->depth] != (int8_t)idx) {
			loadDepth = true;
		} else {
			loadDepth = a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && R->depthImg->written;
			if (a->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)sp->depth < c->clearCount) {
				clearDepth = true;
				d = c->clears[sp->depth].depthStencil.depth;
				s = c->clears[sp->depth].depthStencil.stencil;
			}
		}
	}
	vkv_begin_scene(R, loadDepth, clearDepth, d, s);
}

static void vkv_exec_begin_rp(VkvReplay *R, const VkvCmdBeginRP *c, const uint8_t *next, const uint8_t *end) {
	VkvFramebuffer *fb = c->fb;
	R->rp = c->rp;
	R->fb = fb;
	R->begin = c;
	R->fbInv[0] = 1.0f / (float)fb->width;
	R->fbInv[1] = 1.0f / (float)fb->height;
	if (vkv_validate() && (c->area.offset.x || c->area.offset.y || c->area.extent.width != fb->width ||
	                       c->area.extent.height != fb->height))
		VKV_UNSUPPORTED("partial render areas (whole framebuffer is rendered)");
	vkv_start_subpass(R, 0, next, end);
}

static void vkv_exec_next_subpass(VkvReplay *R, const uint8_t *next, const uint8_t *end) {
	if (!R->rp || R->subpass + 1 >= R->rp->subpassCount) return;
	uint32_t n = R->subpass + 1;
	if (!R->rp->sub[n].newScene) {
		R->subpass = n;
		return;
	}
	vkv_end_scene(R, false);
	vkv_start_subpass(R, n, next, end);
}


/* --- Replay --- */
static const char *vkv_cmd_name(uint16_t t) {
	static const char *names[] = { "?", "BeginRenderPass", "EndRenderPass", "BindPipeline", "BindVertexBuffers",
		"BindIndexBuffer", "BindDescriptorSets", "PushConstants", "SetViewport", "SetScissor", "SetDepthBias",
		"SetStencil", "Draw", "DrawIndexed", "CopyBuffer", "CopyBufferToImage", "CopyImageToBuffer",
		"UpdateBuffer", "FillBuffer", "ClearAttachments", "ClearColorImage", "Barrier", "FixedFunctionDraw",
		"CopyImage", "BlitImage", "ClearDepthStencilImage", "DrawIndirect", "ExecuteCommands", "SetLineWidth",
		"BeginQuery", "EndQuery", "ResetQueryPool", "WriteTimestamp", "SetEvent", "NextSubpass", "CopyQueryPoolResults" };
	return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

void vkv_replay(VkvReplay *R, struct VkCommandBuffer_T *cb) {
	const uint8_t *p = cb->data, *end = cb->data + cb->size;
	while (p < end) {
		const VkvCmd *h = (const VkvCmd *)p;
		const uint8_t *next = p + h->size;
		if (vkv_dbg(VKV_DEBUG_LOG_CALLS)) vkv_log(VKV_LOG_DEBUG, "replay: %s", vkv_cmd_name(h->type));
		switch ((VkvCmdType)h->type) {
		case VKV_CMD_BEGIN_RP:
			vkv_exec_begin_rp(R, (const VkvCmdBeginRP *)h, next, end);
			break;
		case VKV_CMD_NEXT_SUBPASS:
			vkv_exec_next_subpass(R, next, end);
			break;
		case VKV_CMD_END_RP:
			vkv_end_scene(R, true);
			if (R->scenesLeft) R->scenesLeft--;
			R->rp = NULL;
			R->fb = NULL;
			break;
		case VKV_CMD_BIND_PIPELINE: {
			VkvPipeline *np = ((const VkvCmdBindPipeline *)h)->p;
			if (np != R->pipe) { R->pipe = np; R->pipeDirty = true; }
			break;
		}
		case VKV_CMD_BIND_VB: {
			const VkvCmdBindVB *c = (const VkvCmdBindVB *)h;
			for (uint32_t i = 0; i < c->count; i++) { R->vb[c->first + i] = c->buf[i]; R->vbOff[c->first + i] = c->off[i]; }
			break;
		}
		case VKV_CMD_BIND_IB: {
			const VkvCmdBindIB *c = (const VkvCmdBindIB *)h;
			R->ib = c->buf; R->ibOff = c->off; R->ibType = c->type;
			break;
		}
		case VKV_CMD_BIND_SETS: {
			const VkvCmdBindSets *c = (const VkvCmdBindSets *)h;
			uint32_t d = 0;
			for (uint32_t i = 0; i < c->count; i++) {
				VkvSet *s = c->sets[i];
				R->sets[c->first + i] = s;
				if (!s) continue;
				for (uint32_t k = 0; k < s->layout->dynamicCount && k < VKV_MAX_DYNAMIC; k++)
					R->dyn[c->first + i][k] = d < c->dynCount ? c->dyn[d++] : 0;
			}
			g_vkv.cur.descriptorBinds++;
			break;
		}
		case VKV_CMD_PUSH: {
			const VkvCmdPush *c = (const VkvCmdPush *)h;
			memcpy(R->push + c->offset, c->data, c->size);
			R->pushHigh = VKV_MAX(R->pushHigh, c->offset + c->size);
			break;
		}
		case VKV_CMD_VIEWPORT:
			R->viewport = ((const VkvCmdViewport *)h)->vp;
			R->hasViewport = true;
			R->viewportDirty = true;
			break;
		case VKV_CMD_SCISSOR:
			R->scissor = ((const VkvCmdScissor *)h)->rect;
			R->hasScissor = true;
			R->scissorDirty = true;
			break;
		case VKV_CMD_DEPTH_BIAS: {
			const VkvCmdDepthBias *c = (const VkvCmdDepthBias *)h;
			R->biasConst = c->constant; R->biasSlope = c->slope;
			R->dynSet |= VKV_DYN_DEPTH_BIAS;
			R->pipeDirty = true;
			break;
		}
		case VKV_CMD_STENCIL: {
			const VkvCmdStencil *c = (const VkvCmdStencil *)h;
			uint32_t *dst = c->what == VKV_DYN_STENCIL_CMP ? R->sCmp : c->what == VKV_DYN_STENCIL_WR ? R->sWr : R->sRef;
			if (c->faces & VK_STENCIL_FACE_FRONT_BIT) dst[0] = c->value;
			if (c->faces & VK_STENCIL_FACE_BACK_BIT) dst[1] = c->value;
			R->dynSet |= c->what;
			R->pipeDirty = true;
			break;
		}
		case VKV_CMD_DRAW:
			vkv_exec_draw(R, (const VkvCmdDraw *)h, false);
			break;
		case VKV_CMD_DRAW_INDEXED:
			vkv_exec_draw(R, (const VkvCmdDraw *)h, true);
			break;
		case VKV_CMD_COPY_BUFFER: {
			const VkvCmdCopyBuffer *c = (const VkvCmdCopyBuffer *)h;
			if (!c->src->ptr || !c->dst->ptr) break;
			vkv_transfer_sync(R);
			for (uint32_t i = 0; i < c->n; i++)
				memmove(c->dst->ptr + c->r[i].dstOffset, c->src->ptr + c->r[i].srcOffset, c->r[i].size);
			g_vkv.cur.copies += c->n;
			break;
		}
		case VKV_CMD_COPY_BUF_IMG:
			vkv_exec_copy_buf_img(R, (const VkvCmdCopyBufImg *)h, true);
			break;
		case VKV_CMD_COPY_IMG_BUF:
			if (R->dev->gpuBusy) vkv_device_wait_gpu(R->dev); /* must see rendered pixels */
			vkv_exec_copy_buf_img(R, (const VkvCmdCopyBufImg *)h, false);
			break;
		case VKV_CMD_UPDATE_BUFFER: {
			const VkvCmdUpdateBuffer *c = (const VkvCmdUpdateBuffer *)h;
			if (!c->dst->ptr) break;
			vkv_transfer_sync(R);
			memcpy(c->dst->ptr + c->off, c->data, c->size);
			g_vkv.cur.copies++;
			break;
		}
		case VKV_CMD_FILL_BUFFER: {
			const VkvCmdFillBuffer *c = (const VkvCmdFillBuffer *)h;
			if (!c->dst->ptr) break;
			vkv_transfer_sync(R);
			uint32_t *q = (uint32_t *)(c->dst->ptr + c->off);
			for (uint32_t i = 0; i < c->size / 4; i++) q[i] = c->value;
			break;
		}
		case VKV_CMD_CLEAR_ATTACH:
			vkv_exec_clear_attachments(R, (const VkvCmdClearAttach *)h);
			break;
		case VKV_CMD_CLEAR_COLOR_IMAGE:
			vkv_exec_clear_color_image(R, (const VkvCmdClearColorImage *)h);
			break;
		case VKV_CMD_BARRIER:
			if (R->dev->gpuBusy && !vkv_compat(VKV_COMPAT_FAST_TRANSFERS)) vkv_device_wait_gpu(R->dev);
			break;
		case VKV_CMD_FF_DRAW:
			vkv_exec_ff_draw(R, (const VkvCmdFFDraw *)h);
			break;
		case VKV_CMD_COPY_IMAGE:
			vkv_exec_copy_image(R, (const VkvCmdCopyImage *)h);
			break;
		case VKV_CMD_BLIT_IMAGE:
			vkv_exec_blit_image(R, (const VkvCmdBlitImage *)h);
			break;
		case VKV_CMD_CLEAR_DS_IMAGE: {
			const VkvCmdClearDSImage *c = (const VkvCmdClearDSImage *)h;
			c->img->pendingDepthClear = true;
			c->img->clearDepth = c->v.depth;
			c->img->clearStencil = c->v.stencil;
			break;
		}
		case VKV_CMD_DRAW_INDIRECT:
			vkv_exec_draw_indirect(R, (const VkvCmdDrawIndirect *)h);
			break;
		case VKV_CMD_EXECUTE: {
			const VkvCmdExecute *c = (const VkvCmdExecute *)h;
			for (uint32_t i = 0; i < c->n; i++)
				if (c->cbs[i] && c->cbs[i]->state == VKV_CB_EXECUTABLE) vkv_replay(R, c->cbs[i]);
			R->pipeDirty = true;
			break;
		}
		case VKV_CMD_LINE_WIDTH:
			R->lineWidth = ((const VkvCmdLineWidth *)h)->width;
			R->dynSet |= VKV_DYN_LINE_WIDTH;
			R->pipeDirty = true;
			break;
		case VKV_CMD_QUERY_BEGIN: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			if (R->inScene) vkv_query_begin(R->dev, R->ctx, c->pool, c->first);
			R->queryActive = R->inScene && c->pool->type == VK_QUERY_TYPE_OCCLUSION && R->dev->visBufSet;
			break;
		}
		case VKV_CMD_QUERY_END: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			if (R->inScene) vkv_query_end(R->dev, R->ctx, c->pool, c->first);
			R->queryActive = false;
			break;
		}
		case VKV_CMD_QUERY_RESET: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			vkv_query_reset(R->dev, c->pool, c->first, c->count);
			break;
		}
		case VKV_CMD_TIMESTAMP: {
			const VkvCmdQuery *c = (const VkvCmdQuery *)h;
			if (c->pool->values) c->pool->values[c->first] = vkv_time_us() * 1000u;
			c->pool->written[c->first] = 1;
			break;
		}
		case VKV_CMD_COPY_QUERY: {
			/* The counters are final once the scenes that ended the queries are
			 * done, so wait for those; the buffer is plain memory the CPU writes. */
			const VkvCmdCopyQuery *c = (const VkvCmdCopyQuery *)h;
			if (!c->dst->ptr) break;
			if (c->pool->type == VK_QUERY_TYPE_OCCLUSION && R->dev->gpuBusy) vkv_device_wait_gpu(R->dev);
			vkv_query_results(R->dev, c->pool, c->first, c->count, c->dst->size - c->off, c->dst->ptr + c->off,
			                  c->stride, c->flags);
			break;
		}
		case VKV_CMD_SET_EVENT: {
			const VkvCmdSetEvent *c = (const VkvCmdSetEvent *)h;
			if (c->event) c->event->set = c->value;
			break;
		}
		default:
			VKV_ERROR("corrupt command stream (type %u)", (unsigned)h->type);
			return;
		}
		p = next;
	}
}

VkResult vkv_queue_execute(struct VkDevice_T *dev, uint32_t cbCount, const VkCommandBuffer *cbs, VkvFence *fence) {
	VkvReplay R;
	memset(&R, 0, sizeof(R));
	R.dev = dev;
	R.ctx = dev->ctx;
	R.vpVariant = -1;
	R.fence = fence;
	for (uint32_t i = 0; i < cbCount; i++) R.scenesLeft += cbs[i]->sceneCount;
	if (fence) {
		fence->signaled = false;
		fence->pending = R.scenesLeft > 0;
		if (!fence->pending) fence->signaled = true;
	}
	for (uint32_t i = 0; i < cbCount; i++) {
		struct VkCommandBuffer_T *cb = cbs[i];
		if (VKV_CHECK(VKV_OBJ_OK(cb, VKV_MAGIC_CMDBUF), "invalid command buffer in submit") ||
		    VKV_CHECK(cb->state == VKV_CB_EXECUTABLE, "submitting a command buffer that was not ended")) {
			R.scenesLeft -= VKV_MIN(R.scenesLeft, cb->sceneCount);
			continue;
		}
		/* state does not carry across command buffers in Vulkan */
		R.pipe = NULL;
		R.pipeDirty = true;
		memset(R.vb, 0, sizeof(R.vb));
		R.ib = NULL;
		memset(R.sets, 0, sizeof(R.sets));
		R.hasViewport = R.hasScissor = false;
		R.dynSet = 0;
		R.pushHigh = 0;
		vkv_replay(&R, cb);
		if (R.inScene) {
			VKV_ERROR("command buffer ended inside a render pass; closing the scene");
			vkv_end_scene(&R, true);
		}
		if (cb->usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) cb->state = VKV_CB_INITIAL;
	}
	/* no scene carried the fence: signal it on the CPU */
	/* transfers after the fenced scene are not covered by it */
	if (fence && R.transferTail) sceGxmTransferFinish();
	if (fence && fence->pending && !R.fenceAttached) {
		VKV_WARN("fence could not be attached to a scene; signaling after a full GPU wait");
		vkv_device_wait_gpu(dev);
		fence->pending = false;
		fence->signaled = true;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
	VKV_TRACE("vkQueueSubmit");
	struct VkDevice_T *dev = queue->device;
	uint64_t t0 = vkv_time_us();
	VkvFence *f = VKV_ND(VkvFence, fence);
	if (f && VKV_CHECK(VKV_OBJ_OK(f, VKV_MAGIC_FENCE), "invalid fence")) f = NULL;
	if (f && VKV_CHECK(!f->pending && !f->signaled, "fence passed to vkQueueSubmit is not reset")) { /* continue */ }

	/* Gather every command buffer so the fence lands on the very last scene. */
	uint32_t total = 0;
	for (uint32_t i = 0; i < count; i++) total += submits[i].commandBufferCount;
	VkCommandBuffer stackCbs[16] = { 0 };
	VkCommandBuffer *cbs = total <= 16 ? stackCbs : malloc(total * sizeof(VkCommandBuffer));
	if (!cbs) return VK_ERROR_OUT_OF_HOST_MEMORY;
	uint32_t n = 0;
	for (uint32_t i = 0; i < count; i++)
		for (uint32_t j = 0; j < submits[i].commandBufferCount; j++) cbs[n++] = submits[i].pCommandBuffers[j];
	VkResult r = vkv_queue_execute(dev, n, cbs, f);
	if (cbs != stackCbs) free(cbs);

	g_vkv.cur.submitUs += vkv_time_us() - t0;
	if (vkv_dbg(VKV_DEBUG_GPU_SYNC)) {
		uint64_t t1 = vkv_time_us();
		vkv_device_wait_gpu(dev);
		vkv_log(VKV_LOG_DEBUG, "gpu_sync: submit of %u command buffers took %.2f ms CPU + %.2f ms GPU", (unsigned)n,
		        (t1 - t0) / 1000.0f, (vkv_time_us() - t1) / 1000.0f);
	}
	return r;
}

