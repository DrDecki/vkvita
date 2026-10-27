/*
 * VkVita: draws: pipeline state, uniforms, textures and the GXM draw calls
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_replay.h"

/* --- Draw state --- */
static SceGxmCullMode vkv_cull(const VkvPipeline *p) {
	if (vkv_compat(VKV_COMPAT_DISABLE_CULL) || p->vkCull == VK_CULL_MODE_NONE) return SCE_GXM_CULL_NONE;
	if (p->vkCull == VK_CULL_MODE_FRONT_AND_BACK) return SCE_GXM_CULL_NONE; /* draws are skipped instead */
	/* GXM culls by window-space winding (y down), the same space Vulkan uses. */
	bool cullCW = (p->vkCull == VK_CULL_MODE_BACK_BIT) == (p->vkFront == VK_FRONT_FACE_COUNTER_CLOCKWISE);
	if (vkv_compat(VKV_COMPAT_INVERT_CULL)) cullCW = !cullCW;
	return cullCW ? SCE_GXM_CULL_CW : SCE_GXM_CULL_CCW;
}

static void vkv_apply_pipeline(VkvReplay *R, VkvPipeline *p, int variant) {
	SceGxmContext *ctx = R->ctx;
	SceGxmVertexProgram *vp = vkv_pipeline_vertex_program(p, variant == 1);
	if (R->pipeDirty || R->vpVariant != variant) {
		sceGxmSetVertexProgram(ctx, vp);
		R->vpVariant = variant;
	}
	if (!R->pipeDirty) return;
	R->pipeDirty = false;
	g_vkv.cur.pipelineBinds++;
	sceGxmSetFragmentProgram(ctx, p->fp);
	R->fpScissorOn = false;
	sceGxmSetCullMode(ctx, vkv_cull(p));
	sceGxmSetFrontDepthFunc(ctx, p->depthFunc);
	sceGxmSetBackDepthFunc(ctx, p->depthFunc);
	sceGxmSetFrontDepthWriteEnable(ctx, p->depthWrite);
	sceGxmSetBackDepthWriteEnable(ctx, p->depthWrite);
	sceGxmSetFrontPolygonMode(ctx, p->polyMode);
	sceGxmSetBackPolygonMode(ctx, p->polyMode);

	uint32_t cmp[2], wr[2], ref[2];
	for (int f = 0; f < 2; f++) {
		cmp[f] = (p->dynamicMask & VKV_DYN_STENCIL_CMP) && (R->dynSet & VKV_DYN_STENCIL_CMP) ? R->sCmp[f] : p->sCompare[f];
		wr[f]  = (p->dynamicMask & VKV_DYN_STENCIL_WR)  && (R->dynSet & VKV_DYN_STENCIL_WR)  ? R->sWr[f]  : p->sWrite[f];
		ref[f] = (p->dynamicMask & VKV_DYN_STENCIL_REF) && (R->dynSet & VKV_DYN_STENCIL_REF) ? R->sRef[f] : p->sRef[f];
	}
	sceGxmSetTwoSidedEnable(ctx, p->stencilEnable ? SCE_GXM_TWO_SIDED_ENABLED : SCE_GXM_TWO_SIDED_DISABLED);
	sceGxmSetFrontStencilFunc(ctx, p->sFunc[0], p->sFail[0], p->sDepthFail[0], p->sPass[0], (unsigned char)cmp[0],
	                          (unsigned char)wr[0]);
	sceGxmSetBackStencilFunc(ctx, p->sFunc[1], p->sFail[1], p->sDepthFail[1], p->sPass[1], (unsigned char)cmp[1],
	                         (unsigned char)wr[1]);
	sceGxmSetFrontStencilRef(ctx, ref[0]);
	sceGxmSetBackStencilRef(ctx, ref[1]);

	int factor = 0, units = 0;
	if (p->depthBiasEnable) {
		bool dyn = (p->dynamicMask & VKV_DYN_DEPTH_BIAS) && (R->dynSet & VKV_DYN_DEPTH_BIAS);
		factor = dyn ? (int)R->biasSlope : p->depthBiasFactor;
		units = dyn ? (int)R->biasConst : p->depthBiasUnits;
	}
	sceGxmSetFrontDepthBias(ctx, factor, units);
	sceGxmSetBackDepthBias(ctx, factor, units);

	float lw = (p->dynamicMask & VKV_DYN_LINE_WIDTH) && (R->dynSet & VKV_DYN_LINE_WIDTH) ? R->lineWidth : p->lineWidth;
	unsigned lwi = lw < 1.0f ? 1u : lw > 16.0f ? 16u : (unsigned)(lw + 0.5f);
	sceGxmSetFrontPointLineWidth(ctx, lwi);
	sceGxmSetBackPointLineWidth(ctx, lwi);
	R->viewportDirty = true;
	R->scissorDirty = true;
}

static VkViewport vkv_current_viewport(const VkvReplay *R, const VkvPipeline *p) {
	if (p->hasViewport) return p->viewport;
	if (R->hasViewport) return R->viewport;
	return (VkViewport){ 0, 0, (float)R->fb->width, (float)R->fb->height, 0.0f, 1.0f };
}

static void vkv_apply_viewport(VkvReplay *R, const VkvPipeline *p) {
	if (R->viewportDirty) {
		R->viewportDirty = false;
		R->scissorDirty = true; /* the region clip also covers the viewport */
		VkViewport v = vkv_current_viewport(R, p);
		float xs = v.width * 0.5f, xo = v.x + xs;
		float ys = v.height * 0.5f, yo = v.y + ys;
		float zs = v.maxDepth - v.minDepth, zo = v.minDepth;
		if (vkv_compat(VKV_COMPAT_GL_CLIP_SPACE)) {
			ys = -ys;
			zs = (v.maxDepth - v.minDepth) * 0.5f;
			zo = (v.maxDepth + v.minDepth) * 0.5f;
		}
		sceGxmSetViewport(R->ctx, xo, xs, yo, ys, zo, zs);
	}
	if (R->scissorDirty) {
		R->scissorDirty = false;
		int32_t w = (int32_t)R->fb->width, h = (int32_t)R->fb->height;
		/* GXM does not clip to the viewport, so the region clip is viewport and scissor combined */
		VkViewport v = vkv_current_viewport(R, p);
		float vx0 = v.width < 0 ? v.x + v.width : v.x, vy0 = v.height < 0 ? v.y + v.height : v.y;
		int32_t sx = VKV_MAX((int32_t)floorf(vx0), 0), sy = VKV_MAX((int32_t)floorf(vy0), 0);
		int32_t ex = VKV_MIN((int32_t)ceilf(vx0 + fabsf(v.width)), w), ey = VKV_MIN((int32_t)ceilf(vy0 + fabsf(v.height)), h);
		const VkRect2D *s = p->hasScissor ? &p->scissor : R->hasScissor ? &R->scissor : NULL;
		bool scissored = s && !vkv_compat(VKV_COMPAT_IGNORE_SCISSOR);
		R->exactScissor = false;
		if (scissored) {
			int32_t qx = s->offset.x, qy = s->offset.y;
			int32_t qe = qx + (int32_t)s->extent.width, qf = qy + (int32_t)s->extent.height;
			/* the region clip works on 32x32 tiles; anything finer is done by the fragment shader */
			R->exactScissor = ((qx | qy) & 31) || ((qe & 31) && qe < w) || ((qf & 31) && qf < h);
			sx = VKV_MAX(sx, s->offset.x);
			sy = VKV_MAX(sy, s->offset.y);
			ex = VKV_MIN(ex, (int32_t)(s->offset.x + s->extent.width));
			ey = VKV_MIN(ey, (int32_t)(s->offset.y + s->extent.height));
		}
		if (ex <= sx || ey <= sy) { sx = sy = 0; ex = ey = 1; } /* empty: clip almost all */
		R->scissorPx[0] = (float)sx;
		R->scissorPx[1] = (float)sy;
		R->scissorPx[2] = (float)ex;
		R->scissorPx[3] = (float)ey;
		sceGxmSetRegionClip(R->ctx, SCE_GXM_REGION_CLIP_OUTSIDE, (unsigned)sx, (unsigned)sy, (unsigned)(ex - 1),
		                    (unsigned)(ey - 1));
	}
}

static const uint8_t *vkv_uniform_buffer_data(VkvReplay *R, int set, uint32_t binding, uint32_t *range) {
	if (set == VKV_INTERNAL_SET) {
		if (binding == 1) {
			*range = sizeof(R->scissorPx);
			return (const uint8_t *)R->scissorPx;
		}
		*range = sizeof(R->fbInv);
		return (const uint8_t *)R->fbInv;
	}
	if (set < 0 || set >= VKV_MAX_SETS || !R->sets[set]) return NULL;
	VkvSet *s = R->sets[set];
	for (uint32_t b = 0; b < s->layout->bindingCount; b++) {
		const VkvDslBinding *lb = &s->layout->bindings[b];
		if (lb->binding != binding || !lb->count) continue;
		const VkvDescriptor *d = &s->desc[lb->first];
		if (!d->valid || !d->buffer || !d->buffer->ptr) return NULL;
		bool dynamic = d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
		               d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
		if (!dynamic && d->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && d->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
			return NULL;
		uint32_t off = d->offset;
		if (dynamic && lb->dynIndex < VKV_MAX_DYNAMIC)
			off += R->dyn[set][lb->dynIndex];
		*range = d->range;
		return d->buffer->ptr + off;
	}
	return NULL;
}

static void vkv_write_uniforms_one(void *buf, const VkvUniformMap *u, const uint8_t *base) {
	if (u->scalar) {
		float conv[4];
		uint32_t c = VKV_MIN(u->comps, 4u);
		for (uint32_t k = 0; k < c; k++) {
			uint32_t bits;
			memcpy(&bits, base + u->offset + k * 4, 4);
			conv[k] = u->scalar == 1 ? (float)(int32_t)bits : (float)bits;
		}
		sceGxmSetUniformDataF(buf, u->param, 0, c, conv);
	} else if (u->elems <= 1 || u->elemStride == u->comps * 4) {
		sceGxmSetUniformDataF(buf, u->param, 0, u->comps * u->elems, (const float *)(base + u->offset));
	} else {
		for (uint32_t e = 0; e < u->elems; e++)
			sceGxmSetUniformDataF(buf, u->param, e * u->comps, u->comps,
			                      (const float *)(base + u->offset + e * u->elemStride));
	}
}

static void vkv_write_uniforms(VkvReplay *R, void *buf, const VkvUniformMap *map, uint32_t n, const char *shader) {
	for (uint32_t i = 0; i < n; i++) {
		const VkvUniformMap *u = &map[i];
		uint32_t bytes = u->elems > 1 ? u->elemStride * u->elems : u->comps * 4;
		const uint8_t *base;
		if (u->set < 0) {
			if (u->offset + bytes > VKV_MAX_PUSH) continue;
			if (vkv_validate() && u->offset + bytes > R->pushHigh)
				vkv_fail("draw", "shader '%s' uniform '%s' reads push constant bytes [%u..%u) that were never pushed",
				         shader, sceGxmProgramParameterGetName(u->param), (unsigned)u->offset,
				         (unsigned)(u->offset + bytes));
			base = R->push;
		} else {
			uint32_t range = 0;
			base = vkv_uniform_buffer_data(R, u->set, u->binding, &range);
			/* an array (a runtime array of a storage buffer) is fed as far as the buffer goes */
			if (base && u->elems > 1 && u->offset + bytes > range && u->offset + u->elemStride <= range) {
				VkvUniformMap part = *u;
				part.elems = (range - u->offset) / u->elemStride;
				vkv_write_uniforms_one(buf, &part, base);
				continue;
			}
			if (!base || u->offset + bytes > range) {
				if (vkv_validate())
					vkv_fail("draw", "shader '%s' uniform '%s' needs %u bytes at offset %u of the uniform buffer at "
					         "set %d binding %u, %s", shader, sceGxmProgramParameterGetName(u->param), (unsigned)bytes,
					         (unsigned)u->offset, (int)u->set, (unsigned)u->binding,
					         base ? "which is too small" : "but none is bound");
				continue;
			}
		}
		vkv_write_uniforms_one(buf, u, base);
	}
}

static VkvDescriptor *vkv_find_descriptor(VkvReplay *R, int set, uint32_t binding, uint32_t element) {
	if (set < 0 || set >= VKV_MAX_SETS || !R->sets[set]) return NULL;
	VkvSet *s = R->sets[set];
	for (uint32_t b = 0; b < s->layout->bindingCount; b++)
		if (s->layout->bindings[b].binding == binding && element < s->layout->bindings[b].count)
			return &s->desc[s->layout->bindings[b].first + element];
	return NULL;
}

/* Builds the texture for one explicit texture unit from its image and sampler descriptors. */
static bool vkv_resolve_texture(VkvReplay *R, const VkvTexBinding *t, SceGxmTexture *out) {
	VkvDescriptor *img = vkv_find_descriptor(R, t->imageSet, t->imageBinding, t->imageElement);
	if (!img || !img->valid) return false;
	if (img->view) img->view->image->readSerial = R->dev->sceneSerial + 1; /* the scene being recorded */
	if (img->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || img->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
		*out = img->tex;
		return true;
	}
	if (img->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || !img->view) return false;
	*out = img->view->tex;
	if (t->samplerSet >= 0) {
		VkvDescriptor *sd = vkv_find_descriptor(R, t->samplerSet, t->samplerBinding, t->samplerElement);
		if (!sd || !sd->sampler) return false;
		vkv_texture_apply_sampler(out, sd->sampler, img->view->levelCount);
	}
	return true;
}

static void vkv_bind_resources(VkvReplay *R, const VkvPipeline *p) {
	SceGxmContext *ctx = R->ctx;
	uint32_t boundSamplers = 0, boundBuffers = 0;
	bool legacyTex = !p->texExplicit[VKV_STAGE_VERTEX] || !p->texExplicit[VKV_STAGE_FRAGMENT];
	for (uint32_t si = 0; si < VKV_MAX_SETS; si++) {
		VkvSet *s = R->sets[si];
		if (!s) continue;
		for (uint32_t b = 0; b < s->layout->bindingCount; b++) {
			const VkvDslBinding *lb = &s->layout->bindings[b];
			for (uint32_t e = 0; e < lb->count; e++) {
				VkvDescriptor *d = &s->desc[lb->first + e];
				if (!d->valid) continue;
				uint32_t slot = d->binding;
				if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
					if (slot >= SCE_GXM_MAX_UNIFORM_BUFFERS || !d->buffer->ptr) continue;
					uint32_t off = d->offset;
					if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) off += R->dyn[si][lb->dynIndex + e];
					const void *ptr = d->buffer->ptr + off;
					sceGxmSetVertexUniformBuffer(ctx, slot, ptr);
					sceGxmSetFragmentUniformBuffer(ctx, slot, ptr);
					boundBuffers |= 1u << slot;
				} else if (legacyTex && d->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
					if (slot >= SCE_GXM_MAX_TEXTURE_UNITS) continue;
					if (d->view) d->view->image->readSerial = R->dev->sceneSerial + 1;
					if (!p->texExplicit[VKV_STAGE_FRAGMENT]) sceGxmSetFragmentTexture(ctx, slot, &d->tex);
					if (!p->texExplicit[VKV_STAGE_VERTEX] && (p->vSamplerMask & (1u << slot)))
						sceGxmSetVertexTexture(ctx, slot, &d->tex);
					boundSamplers |= 1u << slot;
				}
			}
		}
	}
	for (int st = 0; st < VKV_STAGE_COUNT; st++) {
		if (!p->texExplicit[st]) continue;
		for (uint32_t i = 0; i < p->texCount[st]; i++) {
			const VkvTexBinding *t = &p->texb[st][i];
			SceGxmTexture tex;
			if (!vkv_resolve_texture(R, t, &tex)) {
				if (vkv_validate())
					vkv_fail("draw", "pipeline '%s/%s': texture unit %u needs the image at set %d binding %u%s", p->vname,
					         p->fname, (unsigned)t->unit, (int)t->imageSet, (unsigned)t->imageBinding,
					         t->samplerSet >= 0 ? " and a sampler, but one of them is not bound" : ", which is not bound");
				continue;
			}
			if (st == VKV_STAGE_FRAGMENT) sceGxmSetFragmentTexture(ctx, t->unit, &tex);
			else sceGxmSetVertexTexture(ctx, t->unit, &tex);
			boundSamplers |= 1u << t->unit;
		}
	}
	if (vkv_validate()) {
		uint32_t need = (p->texExplicit[VKV_STAGE_FRAGMENT] ? 0 : p->fSamplerMask) |
		                (p->texExplicit[VKV_STAGE_VERTEX] ? 0 : p->vSamplerMask);
		uint32_t missingS = need & ~boundSamplers;
		uint32_t missingB = p->bufferMask & ~boundBuffers;
		if (missingS) vkv_fail("draw", "pipeline '%s/%s' samples texture units 0x%x with no image bound", p->vname,
		                       p->fname, (unsigned)missingS);
		if (missingB) vkv_fail("draw", "pipeline '%s/%s' reads uniform buffers 0x%x with nothing bound", p->vname,
		                       p->fname, (unsigned)missingB);
	}
}

static const void *vkv_linear_indices(struct VkDevice_T *dev, uint32_t count, bool *is32) {
	if (count <= dev->linearIdx16Count && dev->linearIdx16.base) { *is32 = false; return dev->linearIdx16.base; }
	if (count > dev->linearIdx32Count) {
		vkv_device_wait_gpu(dev);
		vkv_gpu_free(&dev->linearIdx32);
		uint32_t n = VKV_ALIGN(count, 65536);
		if (!vkv_gpu_alloc(VKV_HEAP_RAM, n * 4, SCE_GXM_MEMORY_ATTRIB_READ, &dev->linearIdx32)) {
			dev->linearIdx32Count = 0;
			return NULL;
		}
		uint32_t *idx = dev->linearIdx32.base;
		for (uint32_t i = 0; i < n; i++) idx[i] = i;
		dev->linearIdx32Count = n;
	}
	*is32 = true;
	return dev->linearIdx32.base;
}

/* Indices for a line strip drawn as a line list: count - 1 vertex pairs. */
static const void *vkv_strip_indices(VkvReplay *R, const void *appIdx, bool *is32, uint32_t count) {
	struct VkDevice_T *d = R->dev;
	uint32_t pairs = count - 1;
	if (!appIdx) {
		if (pairs > VKV_STRIP_PAIRS || !d->stripIdx16.base) {
			VKV_UNSUPPORTED("non-indexed line strips with more than 65536 vertices (skipped)");
			return NULL;
		}
		*is32 = false;
		return d->stripIdx16.base;
	}
	uint32_t isz = *is32 ? 4 : 2, bytes = VKV_ALIGN(pairs * 2 * isz, 16);
	if (bytes > VKV_STRIP_RING) {
		VKV_UNSUPPORTED("indexed line strips with more than 32768 lines (skipped)");
		return NULL;
	}
	if (!d->stripRing.base && !vkv_gpu_alloc(VKV_HEAP_RAM, VKV_STRIP_RING, SCE_GXM_MEMORY_ATTRIB_READ, &d->stripRing))
		return NULL;
	uint32_t scene = d->sceneSerial + 1;
	if (d->stripRingPos + bytes > VKV_STRIP_RING) {
		/* the open scene still reads what it put in the ring */
		if (d->stripRingScene == scene) {
			VKV_UNSUPPORTED("more than 256 KB of indexed line strips in one render pass (skipped)");
			return NULL;
		}
		if (d->gpuBusy) vkv_device_wait_gpu(d);
		d->stripRingPos = 0;
	}
	uint8_t *out = (uint8_t *)d->stripRing.base + d->stripRingPos;
	if (*is32) {
		const uint32_t *in = appIdx;
		uint32_t *o = (uint32_t *)out;
		for (uint32_t k = 0; k < pairs; k++) { o[2 * k] = in[k]; o[2 * k + 1] = in[k + 1]; }
	} else {
		const uint16_t *in = appIdx;
		uint16_t *o = (uint16_t *)out;
		for (uint32_t k = 0; k < pairs; k++) { o[2 * k] = in[k]; o[2 * k + 1] = in[k + 1]; }
	}
	d->stripRingPos += bytes;
	d->stripRingScene = scene;
	return out;
}

void vkv_exec_draw(VkvReplay *R, const VkvCmdDraw *c, bool indexed) {
	VkvPipeline *p = R->pipe;
	if (!R->inScene || !p) return;
	if (!p->primSupported || p->vkCull == VK_CULL_MODE_FRONT_AND_BACK) return;

	const void *indices;
	SceGxmIndexFormat ifmt;
	bool is32;
	if (indexed) {
		if (!R->ib || !R->ib->ptr) {
			if (vkv_validate()) vkv_fail("vkCmdDrawIndexed", "no index buffer bound");
			return;
		}
		is32 = R->ibType == VK_INDEX_TYPE_UINT32;
		uint32_t isz = is32 ? 4 : 2;
		if (vkv_validate() && R->ibOff + (c->first + c->count) * isz > R->ib->size) {
			vkv_fail("vkCmdDrawIndexed", "indices [%u..%u) run past the index buffer", (unsigned)c->first,
			         (unsigned)(c->first + c->count));
			return;
		}
		indices = R->ib->ptr + R->ibOff + c->first * isz;
	} else {
		indices = vkv_linear_indices(R->dev, c->count, &is32);
		if (!indices) return;
	}
	uint32_t drawCount = c->count;
	if (p->lineStrip) {
		if (c->count < 2) return;
		indices = vkv_strip_indices(R, indexed ? indices : NULL, &is32, c->count);
		if (!indices) return;
		drawCount = 2 * (c->count - 1);
	}
	ifmt = is32 ? SCE_GXM_INDEX_FORMAT_U32 : SCE_GXM_INDEX_FORMAT_U16;

	vkv_apply_pipeline(R, p, is32 ? 1 : 0);
	vkv_apply_viewport(R, p);
	/* a scissor between tiles: the fragment program variant discards the rest */
	bool exact = R->exactScissor && vkv_pipeline_scissor_program(R->dev, p);
	if (R->exactScissor && !exact) VKV_UNSUPPORTED("pixel-exact scissor for Cg shaders (GXM clips on 32x32 tiles)");
	if (exact != R->fpScissorOn) {
		sceGxmSetFragmentProgram(R->ctx, exact ? p->fpScissor : p->fp);
		R->fpScissorOn = exact;
	}
	vkv_bind_resources(R, p);

	/* base vertex: shift every per-vertex stream */
	int32_t baseVertex = indexed ? c->vertexOffset : (int32_t)c->first;
	for (uint32_t inst = 0; inst < c->instances; inst++) {
		bool ok = true;
		for (uint32_t s = 0; s < p->streamCount; s++) {
			uint32_t b = p->streamBinding[s];
			VkvBuffer *vb = R->vb[b];
			if (!vb || !vb->ptr) {
				if (vkv_validate()) vkv_fail("draw", "pipeline '%s' needs vertex binding %u, nothing bound", p->vname, (unsigned)b);
				ok = false;
				break;
			}
			const uint8_t *base = vb->ptr + R->vbOff[b];
			if (p->streamInstanced[s]) base += (size_t)(c->firstInstance + inst) * p->streamStride[s];
			else base += (intptr_t)baseVertex * p->streamStride[s];
			sceGxmSetVertexStream(R->ctx, s, base);
		}
		if (!ok) return;

		/* push constants -> default uniform buffers (re-reserved per draw) */
		void *ub;
		if (p->vDefaultSize && sceGxmReserveVertexDefaultUniformBuffer(R->ctx, &ub) >= 0)
			vkv_write_uniforms(R, ub, p->vuni, p->vuniCount, p->vname);
		if (exact) {
			if (p->fDefaultSizeScissor && sceGxmReserveFragmentDefaultUniformBuffer(R->ctx, &ub) >= 0)
				vkv_write_uniforms(R, ub, p->funiScissor, p->funiScissorCount, p->fname);
		} else if (p->fDefaultSize && sceGxmReserveFragmentDefaultUniformBuffer(R->ctx, &ub) >= 0) {
			vkv_write_uniforms(R, ub, p->funi, p->funiCount, p->fname);
		}

		R->sceneDraws++;
		int r = sceGxmDraw(R->ctx, p->prim, ifmt, indices, drawCount);
		if (r < 0) VKV_ERROR("sceGxmDraw failed 0x%08X (pipeline '%s')", (unsigned)r, p->vname);
		g_vkv.cur.drawCalls++;
		g_vkv.cur.indices += drawCount;
	}
}

void vkv_exec_ff_draw(VkvReplay *R, const VkvCmdFFDraw *c) {
	if (!R->inScene) return;
	VkvPipeline *p = c->pipe;
	R->pipeDirty = true; /* FF pipelines change state behind the app's back */
	vkv_apply_pipeline(R, p, 0);
	vkv_apply_viewport(R, p);
	sceGxmSetVertexStream(R->ctx, 0, c->vertices);
	void *ub;
	if (p->vDefaultSize && sceGxmReserveVertexDefaultUniformBuffer(R->ctx, &ub) >= 0 && c->mvpParam)
		sceGxmSetUniformDataF(ub, c->mvpParam, 0, 16, c->mvp);
	if (p->fDefaultSize && sceGxmReserveFragmentDefaultUniformBuffer(R->ctx, &ub) >= 0 && c->alphaRefParam)
		sceGxmSetUniformDataF(ub, c->alphaRefParam, 0, 1, &c->alphaRef);
	if (c->textured) sceGxmSetFragmentTexture(R->ctx, 0, &c->tex);
	R->sceneDraws++;
	sceGxmDraw(R->ctx, c->prim, SCE_GXM_INDEX_FORMAT_U16, R->dev->linearIdx16.base, c->count);
	g_vkv.cur.drawCalls++;
	g_vkv.cur.ffDraws++;
	g_vkv.cur.indices += c->count;
	R->pipeDirty = true;
}

void vkv_exec_draw_indirect(VkvReplay *R, const VkvCmdDrawIndirect *c) {
	for (uint32_t i = 0; i < c->count; i++) {
		uint32_t off = c->off + i * c->stride;
		VkvCmdDraw d;
		memset(&d, 0, sizeof(d));
		if (c->indexed) {
			if (off + sizeof(VkDrawIndexedIndirectCommand) > c->buf->size) break;
			VkDrawIndexedIndirectCommand cmd;
			memcpy(&cmd, c->buf->ptr + off, sizeof(cmd));
			d.count = cmd.indexCount;
			d.instances = cmd.instanceCount;
			d.first = cmd.firstIndex;
			d.vertexOffset = cmd.vertexOffset;
			d.firstInstance = cmd.firstInstance;
		} else {
			if (off + sizeof(VkDrawIndirectCommand) > c->buf->size) break;
			VkDrawIndirectCommand cmd;
			memcpy(&cmd, c->buf->ptr + off, sizeof(cmd));
			d.count = cmd.vertexCount;
			d.instances = cmd.instanceCount;
			d.first = cmd.firstVertex;
			d.firstInstance = cmd.firstInstance;
		}
		if (d.count && d.instances) vkv_exec_draw(R, &d, c->indexed);
	}
}

