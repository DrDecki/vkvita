/*
 * VkVita: descriptor sets, pipeline layouts, render passes, framebuffers
 * and graphics pipelines (translated to GXM patched programs + state).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

#include <stdio.h>

/* ------------------------------------------------------------------------ */
/* Descriptor set layouts                                                   */
/* ------------------------------------------------------------------------ */
static int vkv_cmp_binding(const void *a, const void *b) {
	const VkvDslBinding *x = a, *y = b;
	return (int)x->binding - (int)y->binding;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                           const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *pOut) {
	(void)device; (void)alloc;
	VkvSetLayout *l = calloc(1, sizeof(*l));
	if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
	l->obj.magic = VKV_MAGIC_DSLAYOUT;
	l->bindingCount = ci->bindingCount;
	l->bindings = calloc(ci->bindingCount ? ci->bindingCount : 1, sizeof(VkvDslBinding));
	for (uint32_t i = 0; i < ci->bindingCount; i++) {
		const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
		l->bindings[i].binding = b->binding;
		l->bindings[i].type = b->descriptorType;
		l->bindings[i].count = b->descriptorCount;
		switch (b->descriptorType) {
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:         /* read-only use in shaders */
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			break;
		default:
			VKV_UNSUPPORTED("storage image and texel buffer descriptors");
			break;
		}
		if (b->pImmutableSamplers && b->descriptorCount &&
		    (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
			l->bindings[i].immutable = calloc(b->descriptorCount, sizeof(VkvSampler *));
			for (uint32_t e = 0; l->bindings[i].immutable && e < b->descriptorCount; e++)
				l->bindings[i].immutable[e] = VKV_ND(VkvSampler, b->pImmutableSamplers[e]);
		}
		if (b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= SCE_GXM_MAX_TEXTURE_UNITS)
			VKV_WARN("sampler binding %u is beyond the 16 GXM texture units", (unsigned)b->binding);
		if ((b->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
		     b->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) && b->binding >= SCE_GXM_MAX_UNIFORM_BUFFERS)
			VKV_WARN("uniform buffer binding %u is beyond the 14 GXM buffer slots", (unsigned)b->binding);
	}
	qsort(l->bindings, l->bindingCount, sizeof(VkvDslBinding), vkv_cmp_binding);
	uint32_t first = 0, dyn = 0;
	for (uint32_t i = 0; i < l->bindingCount; i++) {
		l->bindings[i].first = first;
		first += l->bindings[i].count;
		/* dynamic offsets are given in binding order for both kinds */
		if (l->bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
		    l->bindings[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
			l->bindings[i].dynIndex = dyn;
			dyn += l->bindings[i].count;
		}
	}
	l->descriptorCount = first;
	l->dynamicCount = dyn;
	*pOut = VKV_H(VkDescriptorSetLayout, l);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
                                                        const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvSetLayout *l = VKV_ND(VkvSetLayout, layout);
	if (!l) return;
	for (uint32_t i = 0; i < l->bindingCount; i++) free(l->bindings[i].immutable);
	free(l->bindings);
	l->obj.magic = VKV_MAGIC_DEAD;
	free(l);
}

/* ------------------------------------------------------------------------ */
/* Descriptor pools and sets                                                */
/* ------------------------------------------------------------------------ */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *ci,
                                                      const VkAllocationCallbacks *alloc, VkDescriptorPool *pPool) {
	(void)device; (void)ci; (void)alloc;
	VkvDescPool *p = calloc(1, sizeof(*p));
	if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
	p->obj.magic = VKV_MAGIC_DPOOL;
	*pPool = VKV_H(VkDescriptorPool, p);
	return VK_SUCCESS;
}

static void vkv_pool_free_set(VkvDescPool *p, VkvSet *s) {
	if (s->prev) s->prev->next = s->next; else p->sets = s->next;
	if (s->next) s->next->prev = s->prev;
	s->obj.magic = VKV_MAGIC_DEAD;
	free(s);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                     VkDescriptorPoolResetFlags flags) {
	(void)device; (void)flags;
	VkvDescPool *p = VKV_ND(VkvDescPool, pool);
	while (p->sets) vkv_pool_free_set(p, p->sets);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool pool,
                                                   const VkAllocationCallbacks *alloc) {
	(void)alloc;
	VkvDescPool *p = VKV_ND(VkvDescPool, pool);
	if (!p) return;
	vkResetDescriptorPool(device, pool, 0);
	p->obj.magic = VKV_MAGIC_DEAD;
	free(p);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai,
                                                        VkDescriptorSet *pSets) {
	(void)device;
	VkvDescPool *p = VKV_ND(VkvDescPool, ai->descriptorPool);
	for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
		VkvSetLayout *l = VKV_ND(VkvSetLayout, ai->pSetLayouts[i]);
		VkvSet *s = calloc(1, sizeof(*s) + l->descriptorCount * sizeof(VkvDescriptor));
		if (!s) {
			for (uint32_t j = 0; j < i; j++) vkv_pool_free_set(p, VKV_ND(VkvSet, pSets[j]));
			return VK_ERROR_OUT_OF_POOL_MEMORY;
		}
		s->obj.magic = VKV_MAGIC_DSET;
		s->layout = l;
		s->pool = p;
		s->count = l->descriptorCount;
		for (uint32_t b = 0; b < l->bindingCount; b++)
			for (uint32_t e = 0; e < l->bindings[b].count; e++) {
				VkvDescriptor *d = &s->desc[l->bindings[b].first + e];
				d->type = l->bindings[b].type;
				d->binding = l->bindings[b].binding + e;
				if (l->bindings[b].immutable) {
					d->sampler = l->bindings[b].immutable[e];
					d->valid = d->type == VK_DESCRIPTOR_TYPE_SAMPLER;
				}
			}
		s->next = p->sets;
		if (p->sets) p->sets->prev = s;
		p->sets = s;
		pSets[i] = VKV_H(VkDescriptorSet, s);
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t count,
                                                    const VkDescriptorSet *sets) {
	(void)device;
	VkvDescPool *p = VKV_ND(VkvDescPool, pool);
	for (uint32_t i = 0; i < count; i++)
		if (sets[i]) vkv_pool_free_set(p, VKV_ND(VkvSet, sets[i]));
	return VK_SUCCESS;
}

static VkvDescriptor *vkv_set_find(VkvSet *s, uint32_t binding, uint32_t element) {
	VkvSetLayout *l = s->layout;
	for (uint32_t b = 0; b < l->bindingCount; b++) {
		if (l->bindings[b].binding != binding) continue;
		uint32_t idx = l->bindings[b].first + element;
		return idx < s->count ? &s->desc[idx] : NULL;
	}
	return NULL;
}

static VkvSampler *vkv_set_immutable(const VkvSet *s, uint32_t binding, uint32_t element) {
	const VkvSetLayout *l = s->layout;
	for (uint32_t b = 0; b < l->bindingCount; b++)
		if (l->bindings[b].binding == binding)
			return l->bindings[b].immutable && element < l->bindings[b].count ? l->bindings[b].immutable[element] : NULL;
	return NULL;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t writeCount,
	const VkWriteDescriptorSet *writes, uint32_t copyCount, const VkCopyDescriptorSet *copies) {
	VKV_TRACE("vkUpdateDescriptorSets");
	(void)device;
	for (uint32_t w = 0; w < writeCount; w++) {
		const VkWriteDescriptorSet *wr = &writes[w];
		VkvSet *s = VKV_ND(VkvSet, wr->dstSet);
		if (VKV_CHECK(VKV_OBJ_OK(s, VKV_MAGIC_DSET), "invalid descriptor set")) continue;
		for (uint32_t i = 0; i < wr->descriptorCount; i++) {
			VkvDescriptor *d = vkv_set_find(s, wr->dstBinding, wr->dstArrayElement + i);
			if (VKV_CHECK(d != NULL, "binding %u[%u] not in layout", (unsigned)wr->dstBinding,
			              (unsigned)(wr->dstArrayElement + i)))
				continue;
			if (!d) continue;
			if (VKV_CHECK(d->type == wr->descriptorType, "binding %u: descriptor type mismatch",
			              (unsigned)wr->dstBinding))
				continue;
			switch (wr->descriptorType) {
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
				const VkDescriptorBufferInfo *bi = &wr->pBufferInfo[i];
				d->buffer = VKV_ND(VkvBuffer, bi->buffer);
				d->offset = (uint32_t)bi->offset;
				d->range = bi->range == VK_WHOLE_SIZE ? d->buffer->size - d->offset : (uint32_t)bi->range;
				d->valid = d->buffer != NULL;
				if (VKV_CHECK(!d->buffer || (d->offset & 15) == 0, "uniform buffer offset %u not 16-byte aligned",
				              (unsigned)d->offset))
					d->valid = false;
				break;
			}
			case VK_DESCRIPTOR_TYPE_SAMPLER: {
				VkvSampler *imm = vkv_set_immutable(s, wr->dstBinding, wr->dstArrayElement + i);
				d->sampler = imm ? imm : VKV_ND(VkvSampler, wr->pImageInfo[i].sampler);
				d->valid = d->sampler != NULL;
				break;
			}
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
				d->view = VKV_ND(VkvImageView, wr->pImageInfo[i].imageView);
				d->valid = false;
				if (VKV_CHECK(VKV_OBJ_OK(d->view, VKV_MAGIC_VIEW), "invalid image view")) break;
				if (!d->view->texValid && !vkv_view_build_texture(d->view)) {
					VKV_WARN("binding %u: image view format %d cannot be sampled", (unsigned)wr->dstBinding,
					         (int)d->view->format);
					break;
				}
				d->tex = d->view->tex;
				d->valid = true;
				break;
			}
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
				/* read by lowered SPIR-V as a texture at the current pixel: point sampling */
				d->view = VKV_ND(VkvImageView, wr->pImageInfo[i].imageView);
				d->valid = false;
				if (VKV_CHECK(VKV_OBJ_OK(d->view, VKV_MAGIC_VIEW), "invalid image view")) break;
				if (!d->view->texValid && !vkv_view_build_texture(d->view)) {
					VKV_WARN("binding %u: input attachment format %d cannot be read", (unsigned)wr->dstBinding,
					         (int)d->view->format);
					break;
				}
				d->tex = d->view->tex;
				sceGxmTextureSetMinFilter(&d->tex, SCE_GXM_TEXTURE_FILTER_POINT);
				sceGxmTextureSetMagFilter(&d->tex, SCE_GXM_TEXTURE_FILTER_POINT);
				sceGxmTextureSetUAddrMode(&d->tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
				sceGxmTextureSetVAddrMode(&d->tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
				d->valid = true;
				break;
			}
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
				const VkDescriptorImageInfo *ii = &wr->pImageInfo[i];
				VkvSampler *imm = vkv_set_immutable(s, wr->dstBinding, wr->dstArrayElement + i);
				d->view = VKV_ND(VkvImageView, ii->imageView);
				d->sampler = imm ? imm : VKV_ND(VkvSampler, ii->sampler);
				d->valid = false;
				if (VKV_CHECK(VKV_OBJ_OK(d->view, VKV_MAGIC_VIEW), "invalid image view")) break;
				if (!d->view->texValid && !vkv_view_build_texture(d->view)) {
					VKV_WARN("binding %u: image view format %d cannot be sampled", (unsigned)wr->dstBinding,
					         (int)d->view->format);
					break;
				}
				d->tex = d->view->tex;
				vkv_texture_apply_sampler(&d->tex, d->sampler, d->view->levelCount);
				d->valid = true;
				break;
			}
			default:
				VKV_UNSUPPORTED("writing this descriptor type");
				break;
			}
		}
	}
	for (uint32_t c = 0; c < copyCount; c++) {
		const VkCopyDescriptorSet *cp = &copies[c];
		VkvSet *src = VKV_ND(VkvSet, cp->srcSet), *dst = VKV_ND(VkvSet, cp->dstSet);
		for (uint32_t i = 0; i < cp->descriptorCount; i++) {
			VkvDescriptor *s = vkv_set_find(src, cp->srcBinding, cp->srcArrayElement + i);
			VkvDescriptor *d = vkv_set_find(dst, cp->dstBinding, cp->dstArrayElement + i);
			if (s && d) {
				uint32_t binding = d->binding;
				*d = *s;
				d->binding = binding;
			}
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Pipeline layouts                                                         */
/* ------------------------------------------------------------------------ */
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci,
                                                      const VkAllocationCallbacks *alloc, VkPipelineLayout *pOut) {
	(void)device; (void)alloc;
	if (VKV_CHECK(ci->setLayoutCount <= VKV_MAX_SETS, "too many set layouts (%u > %d)", (unsigned)ci->setLayoutCount,
	              VKV_MAX_SETS))
		return VK_ERROR_INITIALIZATION_FAILED;
	VkvPipelineLayout *l = calloc(1, sizeof(*l));
	if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
	l->obj.magic = VKV_MAGIC_PLAYOUT;
	l->setCount = VKV_MIN(ci->setLayoutCount, VKV_MAX_SETS);
	for (uint32_t i = 0; i < l->setCount; i++) l->sets[i] = VKV_ND(VkvSetLayout, ci->pSetLayouts[i]);
	l->rangeCount = VKV_MIN(ci->pushConstantRangeCount, 8);
	for (uint32_t i = 0; i < l->rangeCount; i++) {
		l->ranges[i] = ci->pPushConstantRanges[i];
		l->pushSize = VKV_MAX(l->pushSize, l->ranges[i].offset + l->ranges[i].size);
	}
	if (VKV_CHECK(l->pushSize <= VKV_MAX_PUSH, "push constant block %u bytes > %d", (unsigned)l->pushSize, VKV_MAX_PUSH))
		l->pushSize = VKV_MAX_PUSH;
	*pOut = VKV_H(VkPipelineLayout, l);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout,
                                                   const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvPipelineLayout *l = VKV_ND(VkvPipelineLayout, layout);
	if (!l) return;
	l->obj.magic = VKV_MAGIC_DEAD;
	free(l);
}

/* ------------------------------------------------------------------------ */
/* Render passes and framebuffers                                           */
/* ------------------------------------------------------------------------ */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                  const VkAllocationCallbacks *alloc, VkRenderPass *pOut) {
	VKV_TRACE("vkCreateRenderPass");
	(void)device; (void)alloc;
	if (!ci->subpassCount || ci->subpassCount > VKV_MAX_SUBPASSES) {
		VKV_ERROR("vkCreateRenderPass: 1 to %d subpasses are supported (got %u)", VKV_MAX_SUBPASSES,
		          (unsigned)ci->subpassCount);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	for (uint32_t i = 0; i < ci->subpassCount; i++) {
		const VkSubpassDescription *sp = &ci->pSubpasses[i];
		if (sp->colorAttachmentCount > 1) {
			VKV_ERROR("vkCreateRenderPass: GXM has one color output (subpass %u has %u color attachments)", (unsigned)i,
			          (unsigned)sp->colorAttachmentCount);
			return VK_ERROR_FEATURE_NOT_PRESENT;
		}
		if (sp->inputAttachmentCount > VKV_MAX_INPUTS) {
			VKV_ERROR("vkCreateRenderPass: at most %d input attachments per subpass (subpass %u has %u)", VKV_MAX_INPUTS,
			          (unsigned)i, (unsigned)sp->inputAttachmentCount);
			return VK_ERROR_FEATURE_NOT_PRESENT;
		}
	}
	if (VKV_CHECK(ci->attachmentCount <= 8, "too many attachments")) return VK_ERROR_FEATURE_NOT_PRESENT;

	VkvRenderPass *rp = calloc(1, sizeof(*rp));
	if (!rp) return VK_ERROR_OUT_OF_HOST_MEMORY;
	rp->obj.magic = VKV_MAGIC_RPASS;
	rp->attachmentCount = ci->attachmentCount;
	for (uint32_t i = 0; i < ci->attachmentCount; i++) {
		rp->att[i].format = ci->pAttachments[i].format;
		rp->att[i].loadOp = ci->pAttachments[i].loadOp;
		rp->att[i].storeOp = ci->pAttachments[i].storeOp;
		rp->att[i].stencilLoadOp = ci->pAttachments[i].stencilLoadOp;
		rp->att[i].stencilStoreOp = ci->pAttachments[i].stencilStoreOp;
		rp->att[i].samples = (uint32_t)ci->pAttachments[i].samples;
	}
	rp->samples = 0;
	memset(rp->firstUse, -1, sizeof(rp->firstUse));
	memset(rp->lastUse, -1, sizeof(rp->lastUse));
	rp->subpassCount = ci->subpassCount;
	for (uint32_t i = 0; i < ci->subpassCount; i++) {
		const VkSubpassDescription *sp = &ci->pSubpasses[i];
		VkvSubpass *s = &rp->sub[i];
		s->color = s->depth = s->resolve = -1;
		if (sp->colorAttachmentCount && sp->pColorAttachments[0].attachment != VK_ATTACHMENT_UNUSED)
			s->color = (int32_t)sp->pColorAttachments[0].attachment;
		if (sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
			s->depth = (int32_t)sp->pDepthStencilAttachment->attachment;
		if (s->color >= 0 && sp->pResolveAttachments && sp->pResolveAttachments[0].attachment != VK_ATTACHMENT_UNUSED &&
		    sp->pResolveAttachments[0].attachment < ci->attachmentCount)
			s->resolve = (int32_t)sp->pResolveAttachments[0].attachment;
		/* GXM has one sample count per render target */
		int32_t sa[2] = { s->color, s->depth };
		for (int k = 0; k < 2; k++) {
			if (sa[k] < 0 || sa[k] >= (int32_t)ci->attachmentCount) continue;
			uint32_t n = rp->att[sa[k]].samples;
			if (!rp->samples) rp->samples = n;
			else if (rp->samples != n) {
				VKV_ERROR("vkCreateRenderPass: all color and depth attachments need the same sample count "
				          "(%u and %u)", (unsigned)rp->samples, (unsigned)n);
				free(rp);
				return VK_ERROR_FEATURE_NOT_PRESENT;
			}
		}
		if (s->resolve >= 0 && rp->att[s->color].samples > 1 && rp->att[s->color].storeOp == VK_ATTACHMENT_STORE_OP_STORE)
			VKV_UNSUPPORTED("keeping a multisampled attachment that is also resolved (only the resolve target is written)");
		for (uint32_t k = 0; k < sp->inputAttachmentCount; k++) {
			uint32_t a = sp->pInputAttachments[k].attachment;
			if (a == VK_ATTACHMENT_UNUSED || a >= ci->attachmentCount) continue;
			const VkvFormatInfo *fi = vkv_format(rp->att[a].format);
			if (fi && fi->isDepth) {
				VKV_UNSUPPORTED("depth/stencil input attachments (GXM depth buffers cannot be sampled)");
				continue;
			}
			if ((int32_t)a == s->color) VKV_UNSUPPORTED("reading and writing the same attachment in one subpass");
			s->input[s->inputCount++] = (int32_t)a;
		}
		int32_t used[2 + VKV_MAX_INPUTS], nu = 0;
		used[nu++] = s->color;
		used[nu++] = s->depth;
		for (uint32_t k = 0; k < s->inputCount; k++) used[nu++] = s->input[k];
		for (int32_t k = 0; k < nu; k++) {
			if (used[k] < 0 || used[k] >= 8) continue;
			if (rp->firstUse[used[k]] < 0) rp->firstUse[used[k]] = (int8_t)i;
			rp->lastUse[used[k]] = (int8_t)i;
		}
		/* a subpass keeps the scene of the one before when it renders to the same
		 * attachments and reads none of them back */
		s->newScene = i == 0 || s->color != rp->sub[i - 1].color || s->depth != rp->sub[i - 1].depth || s->inputCount;
	}
	for (int32_t i = (int32_t)rp->subpassCount - 1; i >= 0; i--) {
		bool last = i + 1 == (int32_t)rp->subpassCount || rp->sub[i + 1].newScene;
		rp->sub[i].runEnd = last ? (uint32_t)i : rp->sub[i + 1].runEnd;
	}
	if (!rp->samples) rp->samples = 1;
	if (rp->samples != 1 && rp->samples != 2 && rp->samples != 4) {
		VKV_ERROR("vkCreateRenderPass: %u samples, GXM can do 1, 2 or 4", (unsigned)rp->samples);
		free(rp);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	rp->colorIndex = rp->sub[0].color;
	rp->depthIndex = rp->sub[0].depth;
	*pOut = VKV_H(VkRenderPass, rp);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks *alloc) {
	(void)device; (void)alloc;
	VkvRenderPass *rp = VKV_ND(VkvRenderPass, pass);
	if (!rp) return;
	rp->obj.magic = VKV_MAGIC_DEAD;
	free(rp);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc, VkFramebuffer *pOut) {
	VKV_TRACE("vkCreateFramebuffer");
	(void)device; (void)alloc;
	VkvRenderPass *rp = VKV_ND(VkvRenderPass, ci->renderPass);
	if (VKV_CHECK(VKV_OBJ_OK(rp, VKV_MAGIC_RPASS), "invalid render pass")) return VK_ERROR_UNKNOWN;
	if (VKV_CHECK(ci->attachmentCount == rp->attachmentCount, "attachment count mismatch")) return VK_ERROR_UNKNOWN;
	VkvFramebuffer *fb = calloc(1, sizeof(*fb));
	if (!fb) return VK_ERROR_OUT_OF_HOST_MEMORY;
	fb->obj.magic = VKV_MAGIC_FBUF;
	fb->rp = rp;
	fb->width = ci->width;
	fb->height = ci->height;
	fb->viewCount = VKV_MIN(ci->attachmentCount, 8);
	for (uint32_t i = 0; i < fb->viewCount; i++) fb->views[i] = VKV_ND(VkvImageView, ci->pAttachments[i]);

	for (uint32_t i = 0; i < fb->viewCount; i++) {
		VkvImage *img = fb->views[i]->image;
		if (VKV_CHECK(img->width >= fb->width && img->height >= fb->height,
		              "attachment %u (%ux%u) is smaller than the framebuffer (%ux%u)", (unsigned)i,
		              (unsigned)img->width, (unsigned)img->height, (unsigned)fb->width, (unsigned)fb->height)) {
			free(fb);
			return VK_ERROR_UNKNOWN;
		}
		if (!img->fi->isDepth && vkv_image_ensure_sync(img) != VK_SUCCESS) { free(fb); return VK_ERROR_OUT_OF_HOST_MEMORY; }
	}

	SceGxmRenderTargetParams p;
	memset(&p, 0, sizeof(p));
	p.flags = 0;
	p.width = (uint16_t)fb->width;
	p.height = (uint16_t)fb->height;
	p.scenesPerFrame = (uint16_t)g_vkv.cfg.scenesPerFrame;
	p.multisampleMode = vkv_gxm_multisample(rp->samples);
	p.multisampleLocations = 0;
	p.driverMemBlock = -1;
	int r = sceGxmCreateRenderTarget(&p, &fb->rt);
	if (r < 0) {
		VKV_ERROR("sceGxmCreateRenderTarget(%ux%u) failed: 0x%08X", (unsigned)fb->width, (unsigned)fb->height,
		          (unsigned)r);
		free(fb);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	*pOut = VKV_H(VkFramebuffer, fb);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                                const VkAllocationCallbacks *alloc) {
	(void)alloc;
	VkvFramebuffer *fb = VKV_ND(VkvFramebuffer, framebuffer);
	if (!fb) return;
	/* the GPU may still reference the render target */
	vkv_device_wait_gpu(device);
	sceGxmDestroyRenderTarget(fb->rt);
	fb->obj.magic = VKV_MAGIC_DEAD;
	free(fb);
}

/* ------------------------------------------------------------------------ */
/* Graphics pipelines                                                       */
/* ------------------------------------------------------------------------ */
static SceGxmBlendFactor vkv_blend_factor(VkBlendFactor f) {
	switch (f) {
	case VK_BLEND_FACTOR_ZERO:                     return SCE_GXM_BLEND_FACTOR_ZERO;
	case VK_BLEND_FACTOR_ONE:                      return SCE_GXM_BLEND_FACTOR_ONE;
	case VK_BLEND_FACTOR_SRC_COLOR:                return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:      return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case VK_BLEND_FACTOR_DST_COLOR:                return SCE_GXM_BLEND_FACTOR_DST_COLOR;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:      return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case VK_BLEND_FACTOR_SRC_ALPHA:                return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:      return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case VK_BLEND_FACTOR_DST_ALPHA:                return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
	case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:      return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:       return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	default:
		VKV_UNSUPPORTED("constant / dual-source blend factors (using ONE)");
		return SCE_GXM_BLEND_FACTOR_ONE;
	}
}

static SceGxmBlendFunc vkv_blend_op(VkBlendOp op) {
	switch (op) {
	case VK_BLEND_OP_ADD:              return SCE_GXM_BLEND_FUNC_ADD;
	case VK_BLEND_OP_SUBTRACT:         return SCE_GXM_BLEND_FUNC_SUBTRACT;
	case VK_BLEND_OP_REVERSE_SUBTRACT: return SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
	case VK_BLEND_OP_MIN:              return SCE_GXM_BLEND_FUNC_MIN;
	case VK_BLEND_OP_MAX:              return SCE_GXM_BLEND_FUNC_MAX;
	default:
		VKV_UNSUPPORTED("advanced blend ops (using ADD)");
		return SCE_GXM_BLEND_FUNC_ADD;
	}
}

static SceGxmDepthFunc vkv_depth_func(VkCompareOp op) {
	switch (op) {
	case VK_COMPARE_OP_NEVER:            return SCE_GXM_DEPTH_FUNC_NEVER;
	case VK_COMPARE_OP_LESS:             return SCE_GXM_DEPTH_FUNC_LESS;
	case VK_COMPARE_OP_EQUAL:            return SCE_GXM_DEPTH_FUNC_EQUAL;
	case VK_COMPARE_OP_LESS_OR_EQUAL:    return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
	case VK_COMPARE_OP_GREATER:          return SCE_GXM_DEPTH_FUNC_GREATER;
	case VK_COMPARE_OP_NOT_EQUAL:        return SCE_GXM_DEPTH_FUNC_NOT_EQUAL;
	case VK_COMPARE_OP_GREATER_OR_EQUAL: return SCE_GXM_DEPTH_FUNC_GREATER_EQUAL;
	default:                             return SCE_GXM_DEPTH_FUNC_ALWAYS;
	}
}

static SceGxmStencilFunc vkv_stencil_func(VkCompareOp op) {
	switch (op) {
	case VK_COMPARE_OP_NEVER:            return SCE_GXM_STENCIL_FUNC_NEVER;
	case VK_COMPARE_OP_LESS:             return SCE_GXM_STENCIL_FUNC_LESS;
	case VK_COMPARE_OP_EQUAL:            return SCE_GXM_STENCIL_FUNC_EQUAL;
	case VK_COMPARE_OP_LESS_OR_EQUAL:    return SCE_GXM_STENCIL_FUNC_LESS_EQUAL;
	case VK_COMPARE_OP_GREATER:          return SCE_GXM_STENCIL_FUNC_GREATER;
	case VK_COMPARE_OP_NOT_EQUAL:        return SCE_GXM_STENCIL_FUNC_NOT_EQUAL;
	case VK_COMPARE_OP_GREATER_OR_EQUAL: return SCE_GXM_STENCIL_FUNC_GREATER_EQUAL;
	default:                             return SCE_GXM_STENCIL_FUNC_ALWAYS;
	}
}

static SceGxmStencilOp vkv_stencil_op(VkStencilOp op) {
	switch (op) {
	case VK_STENCIL_OP_ZERO:                return SCE_GXM_STENCIL_OP_ZERO;
	case VK_STENCIL_OP_REPLACE:             return SCE_GXM_STENCIL_OP_REPLACE;
	case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return SCE_GXM_STENCIL_OP_INCR;
	case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return SCE_GXM_STENCIL_OP_DECR;
	case VK_STENCIL_OP_INVERT:              return SCE_GXM_STENCIL_OP_INVERT;
	case VK_STENCIL_OP_INCREMENT_AND_WRAP:  return SCE_GXM_STENCIL_OP_INCR_WRAP;
	case VK_STENCIL_OP_DECREMENT_AND_WRAP:  return SCE_GXM_STENCIL_OP_DECR_WRAP;
	default:                                return SCE_GXM_STENCIL_OP_KEEP;
	}
}

static void vkv_stencil_face(VkvPipeline *p, int face, const VkStencilOpState *s) {
	p->sFunc[face] = vkv_stencil_func(s->compareOp);
	p->sFail[face] = vkv_stencil_op(s->failOp);
	p->sDepthFail[face] = vkv_stencil_op(s->depthFailOp);
	p->sPass[face] = vkv_stencil_op(s->passOp);
	p->sCompare[face] = (uint8_t)s->compareMask;
	p->sWrite[face] = (uint8_t)s->writeMask;
	p->sRef[face] = s->reference;
}

static VkResult vkv_create_vertex_program(VkvPipeline *p, int variant) {
	SceGxmVertexStream streams[SCE_GXM_MAX_VERTEX_STREAMS];
	memcpy(streams, p->streams, sizeof(streams));
	for (uint32_t i = 0; i < p->streamCount; i++)
		streams[i].indexSource = variant ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	int r = sceGxmShaderPatcherCreateVertexProgram(p->device->patcher, p->vpid, p->attrs, p->attrCount, streams,
	                                               p->streamCount, &p->vp[variant]);
	if (r < 0) {
		VKV_ERROR("pipeline '%s': sceGxmShaderPatcherCreateVertexProgram failed 0x%08X (%u attributes, %u streams)",
		          p->vname, (unsigned)r, (unsigned)p->attrCount, (unsigned)p->streamCount);
		p->vp[variant] = NULL;
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return VK_SUCCESS;
}

SceGxmVertexProgram *vkv_pipeline_vertex_program(VkvPipeline *p, bool index32) {
	int v = index32 ? 1 : 0;
	if (!p->vp[v]) vkv_create_vertex_program(p, v);
	return p->vp[v];
}

static uint32_t vkv_push_base(const VkvPipelineLayout *l, VkShaderStageFlags stage) {
	if (!l) return 0;
	for (uint32_t i = 0; i < l->rangeCount; i++)
		if (l->ranges[i].stageFlags & stage) return l->ranges[i].offset;
	return 0;
}

SceGxmFragmentProgram *vkv_pipeline_scissor_program(struct VkDevice_T *dev, VkvPipeline *p) {
	if (p->scissorState) return p->scissorState > 0 ? p->fpScissor : NULL;
	p->scissorState = -1;
	const SceGxmProgram *prog;
	SceGxmShaderPatcherId pid;
	if (!p->fmod || !vkv_shader_get_scissor_program(dev, p->fmod, &prog, &pid)) return NULL;
	if (sceGxmShaderPatcherCreateFragmentProgram(dev->patcher, pid, p->outFmt, vkv_gxm_multisample(p->samples),
	                                             p->hasBlend ? &p->blend : NULL, p->vprog, &p->fpScissor) < 0) {
		p->fpScissor = NULL;
		return NULL;
	}
	p->funiScissor = malloc(VKV_MAX_UNIFORM_MAP * sizeof(VkvUniformMap));
	if (!p->funiScissor) {
		sceGxmShaderPatcherReleaseFragmentProgram(dev->patcher, p->fpScissor);
		p->fpScissor = NULL;
		return NULL;
	}
	p->funiScissorCount = vkv_build_uniform_map(p->fmod, prog, vkv_push_base(p->layout, VK_SHADER_STAGE_FRAGMENT_BIT),
	                                            p->funiScissor, VKV_MAX_UNIFORM_MAP);
	p->fDefaultSizeScissor = sceGxmProgramGetDefaultUniformBufferSize(prog);
	p->scissorState = 1;
	return p->fpScissor;
}

void vkv_destroy_pipeline(struct VkDevice_T *dev, VkvPipeline *p) {
	if (!p) return;
	if (p->vp[0]) sceGxmShaderPatcherReleaseVertexProgram(dev->patcher, p->vp[0]);
	if (p->vp[1]) sceGxmShaderPatcherReleaseVertexProgram(dev->patcher, p->vp[1]);
	if (p->fp) sceGxmShaderPatcherReleaseFragmentProgram(dev->patcher, p->fp);
	if (p->fpScissor) sceGxmShaderPatcherReleaseFragmentProgram(dev->patcher, p->fpScissor);
	free(p->funiScissor);
	if (p->vmod) vkv_shader_release(p->vmod);
	if (p->fmod) vkv_shader_release(p->fmod);
	p->obj.magic = VKV_MAGIC_DEAD;
	free(p);
}

VkResult vkv_create_pipeline(struct VkDevice_T *dev, const VkGraphicsPipelineCreateInfo *ci, VkvPipeline **out) {
	VkvShaderModule *vmod = NULL, *fmod = NULL;
	for (uint32_t i = 0; i < ci->stageCount; i++) {
		const VkPipelineShaderStageCreateInfo *s = &ci->pStages[i];
		if (s->stage == VK_SHADER_STAGE_VERTEX_BIT) vmod = VKV_ND(VkvShaderModule, s->module);
		else if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fmod = VKV_ND(VkvShaderModule, s->module);
		else {
			VKV_ERROR("pipeline: only vertex and fragment stages exist on GXM");
			return VK_ERROR_FEATURE_NOT_PRESENT;
		}
		if (s->pSpecializationInfo) VKV_UNSUPPORTED("specialization constants (ignored)");
	}
	if (!vmod || !fmod) {
		VKV_ERROR("pipeline: a vertex and a fragment shader are both required");
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	VkvPipeline *p = calloc(1, sizeof(*p));
	if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
	p->obj.magic = VKV_MAGIC_PIPELINE;
	p->device = dev;
	p->layout = VKV_ND(VkvPipelineLayout, ci->layout);
	p->rp = VKV_ND(VkvRenderPass, ci->renderPass);
	p->subpass = p->rp && ci->subpass < p->rp->subpassCount ? ci->subpass : 0;
	if (p->rp && ci->subpass >= p->rp->subpassCount) VKV_WARN("pipeline uses subpass %u of a render pass with %u",
	                                                         (unsigned)ci->subpass, (unsigned)p->rp->subpassCount);
	snprintf(p->vname, sizeof(p->vname), "%s", vmod->name);
	snprintf(p->fname, sizeof(p->fname), "%s", fmod->name);

	VkResult res = vkv_shader_get_program(dev, vmod, VKV_STAGE_VERTEX, &p->vprog, &p->vpid);
	if (res == VK_SUCCESS) res = vkv_shader_get_program(dev, fmod, VKV_STAGE_FRAGMENT, &p->fprog, &p->fpid);
	if (res != VK_SUCCESS) { free(p); return res; }
	p->vmod = vmod;
	p->fmod = fmod;
	vkv_shader_retain(vmod);
	vkv_shader_retain(fmod);

	/* --- vertex input ---------------------------------------------------- */
	const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
	uint32_t bindingToStream[64];
	memset(bindingToStream, 0xFF, sizeof(bindingToStream));
	if (vi) {
		for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount && p->streamCount < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
			const VkVertexInputBindingDescription *b = &vi->pVertexBindingDescriptions[i];
			if (b->binding >= 64 || b->binding >= VKV_MAX_BINDINGS) {
				VKV_WARN("pipeline: vertex binding %u out of range", (unsigned)b->binding);
				continue;
			}
			uint32_t s = p->streamCount++;
			bindingToStream[b->binding] = s;
			p->streamBinding[s] = b->binding;
			p->streamInstanced[s] = b->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
			p->streamStride[s] = (uint16_t)b->stride;
			/* per-instance data is emulated: stride 0, pointer advanced per instance */
			p->streams[s].stride = p->streamInstanced[s] ? 0 : (uint16_t)b->stride;
			p->streams[s].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
			if (p->streamInstanced[s]) p->streams[s].stride = 0;
		}
		uint32_t covered = 0;
		for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
			const VkVertexInputAttributeDescription *a = &vi->pVertexAttributeDescriptions[i];
			const VkvFormatInfo *fi = vkv_format(a->format);
			if (!fi || !fi->attribComps) {
				VKV_ERROR("pipeline '%s': vertex format %d (location %u) is not supported by GXM", p->vname,
				          (int)a->format, (unsigned)a->location);
				vkv_destroy_pipeline(dev, p);
				return VK_ERROR_FORMAT_NOT_SUPPORTED;
			}
			const SceGxmProgramParameter *pp = vkv_shader_find_attribute(vmod, p->vprog, a->location);
			if (!pp) {
				VKV_INFO("pipeline '%s': location %u has no matching shader attribute (optimized out?)", p->vname,
				         (unsigned)a->location);
				continue;
			}
			if (a->binding >= 64 || bindingToStream[a->binding] == 0xFFFFFFFFu) {
				VKV_ERROR("pipeline '%s': location %u uses undeclared binding %u", p->vname, (unsigned)a->location,
				          (unsigned)a->binding);
				vkv_destroy_pipeline(dev, p);
				return VK_ERROR_INITIALIZATION_FAILED;
			}
			SceGxmVertexAttribute *ga = &p->attrs[p->attrCount++];
			ga->streamIndex = (uint16_t)bindingToStream[a->binding];
			ga->offset = (uint16_t)a->offset;
			ga->format = (uint8_t)fi->attrib;
			ga->componentCount = fi->attribComps;
			ga->regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(pp);
			covered++;
			if (vkv_dbg(VKV_DEBUG_LOG_SHADERS))
				vkv_log(VKV_LOG_INFO, "pipeline '%s': location %u -> attribute '%s' (reg %u, stream %u, +%u)",
				        p->vname, (unsigned)a->location, sceGxmProgramParameterGetName(pp), (unsigned)ga->regIndex,
				        (unsigned)ga->streamIndex, (unsigned)ga->offset);
		}
		if (vkv_validate()) {
			unsigned n = sceGxmProgramGetParameterCount(p->vprog), attrs = 0;
			for (unsigned i = 0; i < n; i++) {
				const SceGxmProgramParameter *pp = sceGxmProgramGetParameter(p->vprog, i);
				if (sceGxmProgramParameterGetCategory(pp) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE &&
				    !vkv_shader_is_system_input(pp))
					attrs++;
			}
			if (attrs > covered)
				vkv_fail(__func__, "pipeline '%s': vertex shader has %u attributes but only %u are fed", p->vname,
				         attrs, (unsigned)covered);
		}
	}
	if (vkv_create_vertex_program(p, 0) != VK_SUCCESS) { vkv_destroy_pipeline(dev, p); return VK_ERROR_INITIALIZATION_FAILED; }

	/* --- fragment program (output format + blending) ---------------------- */
	SceGxmOutputRegisterFormat outFmt = SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4;
	if (p->rp && p->rp->sub[p->subpass].color >= 0) {
		const VkvFormatInfo *fi = vkv_format(p->rp->att[p->rp->sub[p->subpass].color].format);
		if (fi) outFmt = fi->outReg;
	}
	SceGxmBlendInfo blend;
	memset(&blend, 0, sizeof(blend));
	const SceGxmBlendInfo *blendPtr = NULL;
	const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
	if (cb && cb->attachmentCount) {
		const VkPipelineColorBlendAttachmentState *a = &cb->pAttachments[0];
		uint8_t mask = 0;
		if (a->colorWriteMask & VK_COLOR_COMPONENT_R_BIT) mask |= SCE_GXM_COLOR_MASK_R;
		if (a->colorWriteMask & VK_COLOR_COMPONENT_G_BIT) mask |= SCE_GXM_COLOR_MASK_G;
		if (a->colorWriteMask & VK_COLOR_COMPONENT_B_BIT) mask |= SCE_GXM_COLOR_MASK_B;
		if (a->colorWriteMask & VK_COLOR_COMPONENT_A_BIT) mask |= SCE_GXM_COLOR_MASK_A;
		blend.colorMask = mask;
		if (a->blendEnable) {
			blend.colorFunc = vkv_blend_op(a->colorBlendOp);
			blend.alphaFunc = vkv_blend_op(a->alphaBlendOp);
			blend.colorSrc = vkv_blend_factor(a->srcColorBlendFactor);
			blend.colorDst = vkv_blend_factor(a->dstColorBlendFactor);
			blend.alphaSrc = vkv_blend_factor(a->srcAlphaBlendFactor);
			blend.alphaDst = vkv_blend_factor(a->dstAlphaBlendFactor);
		} else {
			blend.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
			blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
			blend.colorSrc = SCE_GXM_BLEND_FACTOR_ONE;
			blend.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
			blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
			blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
		}
		if (a->blendEnable || mask != SCE_GXM_COLOR_MASK_ALL) blendPtr = &blend;
		if (cb->logicOpEnable) VKV_UNSUPPORTED("logic ops");
		if (cb->attachmentCount > 1) VKV_UNSUPPORTED("more than one blend attachment");
	}
	/* the fragment program has to match the sample count of the render target */
	const VkPipelineMultisampleStateCreateInfo *ms = ci->pMultisampleState;
	p->samples = ms ? (uint32_t)ms->rasterizationSamples : 1;
	if (p->rp && p->samples != p->rp->samples) {
		VKV_WARN("pipeline '%s': %u samples for a render pass with %u, using the render pass", p->fname,
		         (unsigned)p->samples, (unsigned)p->rp->samples);
		p->samples = p->rp->samples;
	}
	if (ms && ms->sampleShadingEnable) VKV_UNSUPPORTED("sample shading (shaded once per pixel)");
	if (ms && ms->alphaToCoverageEnable) VKV_UNSUPPORTED("alpha to coverage");
	if (ms && ms->pSampleMask && (ms->pSampleMask[0] & ((1u << p->samples) - 1)) != ((1u << p->samples) - 1))
		VKV_UNSUPPORTED("sample masks");
	p->outFmt = outFmt;
	p->hasBlend = blendPtr != NULL;
	if (blendPtr) p->blend = *blendPtr;
	int r = sceGxmShaderPatcherCreateFragmentProgram(dev->patcher, p->fpid, outFmt, vkv_gxm_multisample(p->samples),
	                                                 blendPtr, p->vprog, &p->fp);
	if (r < 0) {
		VKV_ERROR("pipeline '%s': sceGxmShaderPatcherCreateFragmentProgram failed 0x%08X", p->fname, (unsigned)r);
		p->fp = NULL;
		vkv_destroy_pipeline(dev, p);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* --- push constants -> default uniforms ------------------------------- */
	p->vuniCount = vkv_build_uniform_map(vmod, p->vprog, vkv_push_base(p->layout, VK_SHADER_STAGE_VERTEX_BIT), p->vuni,
	                                     VKV_MAX_UNIFORM_MAP);
	p->funiCount = vkv_build_uniform_map(fmod, p->fprog, vkv_push_base(p->layout, VK_SHADER_STAGE_FRAGMENT_BIT),
	                                     p->funi, VKV_MAX_UNIFORM_MAP);
	p->vDefaultSize = sceGxmProgramGetDefaultUniformBufferSize(p->vprog);
	p->fDefaultSize = sceGxmProgramGetDefaultUniformBufferSize(p->fprog);
	p->texb[VKV_STAGE_VERTEX] = vmod->texb[VKV_STAGE_VERTEX];
	p->texCount[VKV_STAGE_VERTEX] = vmod->texCount[VKV_STAGE_VERTEX];
	p->texb[VKV_STAGE_FRAGMENT] = fmod->texb[VKV_STAGE_FRAGMENT];
	p->texCount[VKV_STAGE_FRAGMENT] = fmod->texCount[VKV_STAGE_FRAGMENT];
	p->texExplicit[VKV_STAGE_VERTEX] = vmod->texExplicit[VKV_STAGE_VERTEX];
	p->texExplicit[VKV_STAGE_FRAGMENT] = fmod->texExplicit[VKV_STAGE_FRAGMENT];

	/* --- reflection used by draw-time validation -------------------------- */
	for (int st = 0; st < 2; st++) {
		const SceGxmProgram *prog = st ? p->fprog : p->vprog;
		unsigned n = sceGxmProgramGetParameterCount(prog);
		for (unsigned i = 0; i < n; i++) {
			const SceGxmProgramParameter *pp = sceGxmProgramGetParameter(prog, i);
			SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(pp);
			unsigned res = sceGxmProgramParameterGetResourceIndex(pp);
			if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER && res < 32) {
				if (st) p->fSamplerMask |= 1u << res; else p->vSamplerMask |= 1u << res;
			} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER && res < 32) {
				p->bufferMask |= 1u << res;
			} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM) {
				unsigned c = sceGxmProgramParameterGetContainerIndex(pp);
				if (c < SCE_GXM_MAX_UNIFORM_BUFFERS) p->bufferMask |= 1u << c;
			}
		}
	}

	/* --- primitive -------------------------------------------------------- */
	p->primSupported = true;
	p->polyMode = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
	VkPrimitiveTopology topo = ci->pInputAssemblyState ? ci->pInputAssemblyState->topology
	                                                   : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	switch (topo) {
	/* the UV variant generates SPRITECOORD, the plain point mode leaves it at zero */
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:     p->prim = SCE_GXM_PRIMITIVE_POINTS; p->polyMode = SCE_GXM_POLYGON_MODE_POINT_01UV; break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:      p->prim = SCE_GXM_PRIMITIVE_LINES;  p->polyMode = SCE_GXM_POLYGON_MODE_LINE; break;
	/* GXM has no line strips: draws hand it the vertex pairs as a line list */
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:     p->prim = SCE_GXM_PRIMITIVE_LINES;  p->polyMode = SCE_GXM_POLYGON_MODE_LINE;
	                                           p->lineStrip = true; break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  p->prim = SCE_GXM_PRIMITIVE_TRIANGLES; break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: p->prim = SCE_GXM_PRIMITIVE_TRIANGLE_STRIP; break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:   p->prim = SCE_GXM_PRIMITIVE_TRIANGLE_FAN; break;
	default:
		VKV_WARN("pipeline '%s': topology %d is not supported by GXM; draws will be skipped", p->vname, (int)topo);
		p->primSupported = false;
		p->prim = SCE_GXM_PRIMITIVE_TRIANGLES;
		break;
	}
	if (ci->pInputAssemblyState && ci->pInputAssemblyState->primitiveRestartEnable)
		VKV_UNSUPPORTED("primitive restart");

	/* --- rasterization ---------------------------------------------------- */
	const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
	p->vkCull = rs ? rs->cullMode : VK_CULL_MODE_NONE;
	p->vkFront = rs ? rs->frontFace : VK_FRONT_FACE_COUNTER_CLOCKWISE;
	if (rs && p->prim != SCE_GXM_PRIMITIVE_POINTS && p->prim != SCE_GXM_PRIMITIVE_LINES) {
		if (rs->polygonMode == VK_POLYGON_MODE_LINE) p->polyMode = SCE_GXM_POLYGON_MODE_TRIANGLE_LINE;
		else if (rs->polygonMode == VK_POLYGON_MODE_POINT) p->polyMode = SCE_GXM_POLYGON_MODE_TRIANGLE_POINT;
	}
	if (rs && rs->depthClampEnable) VKV_UNSUPPORTED("depth clamp");
	if (rs && rs->rasterizerDiscardEnable) VKV_UNSUPPORTED("rasterizer discard");
	p->lineWidth = rs ? rs->lineWidth : 1.0f;
	p->depthBiasEnable = rs && rs->depthBiasEnable;
	if (p->depthBiasEnable) {
		p->depthBiasFactor = (int)rs->depthBiasSlopeFactor;
		p->depthBiasUnits = (int)rs->depthBiasConstantFactor;
	}

	/* --- depth / stencil -------------------------------------------------- */
	const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;
	bool hasDepth = p->rp && p->rp->sub[p->subpass].depth >= 0;
	p->depthFunc = SCE_GXM_DEPTH_FUNC_ALWAYS;
	p->depthWrite = SCE_GXM_DEPTH_WRITE_DISABLED;
	if (ds && hasDepth && ds->depthTestEnable) {
		p->depthFunc = vkv_depth_func(ds->depthCompareOp);
		p->depthWrite = ds->depthWriteEnable ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
	}
	if (ds && ds->depthBoundsTestEnable) VKV_UNSUPPORTED("depth bounds test");
	p->stencilEnable = ds && hasDepth && ds->stencilTestEnable;
	if (p->stencilEnable) {
		vkv_stencil_face(p, 0, &ds->front);
		vkv_stencil_face(p, 1, &ds->back);
	} else {
		for (int f = 0; f < 2; f++) {
			p->sFunc[f] = SCE_GXM_STENCIL_FUNC_ALWAYS;
			p->sFail[f] = p->sDepthFail[f] = p->sPass[f] = SCE_GXM_STENCIL_OP_KEEP;
			p->sCompare[f] = p->sWrite[f] = 0xFF;
		}
	}

	/* --- dynamic + static viewport state ---------------------------------- */
	if (ci->pDynamicState) {
		for (uint32_t i = 0; i < ci->pDynamicState->dynamicStateCount; i++) {
			switch (ci->pDynamicState->pDynamicStates[i]) {
			case VK_DYNAMIC_STATE_VIEWPORT:             p->dynamicMask |= VKV_DYN_VIEWPORT; break;
			case VK_DYNAMIC_STATE_SCISSOR:              p->dynamicMask |= VKV_DYN_SCISSOR; break;
			case VK_DYNAMIC_STATE_DEPTH_BIAS:           p->dynamicMask |= VKV_DYN_DEPTH_BIAS; break;
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: p->dynamicMask |= VKV_DYN_STENCIL_CMP; break;
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:   p->dynamicMask |= VKV_DYN_STENCIL_WR; break;
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE:    p->dynamicMask |= VKV_DYN_STENCIL_REF; break;
			case VK_DYNAMIC_STATE_LINE_WIDTH:           p->dynamicMask |= VKV_DYN_LINE_WIDTH; break;
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
				break;
			default:
				VKV_UNSUPPORTED("extended dynamic state");
				break;
			}
		}
	}
	const VkPipelineViewportStateCreateInfo *vs = ci->pViewportState;
	if (vs && vs->pViewports && vs->viewportCount && !(p->dynamicMask & VKV_DYN_VIEWPORT)) {
		p->viewport = vs->pViewports[0];
		p->hasViewport = true;
	}
	if (vs && vs->pScissors && vs->scissorCount && !(p->dynamicMask & VKV_DYN_SCISSOR)) {
		p->scissor = vs->pScissors[0];
		p->hasScissor = true;
	}
	*out = p;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkGraphicsPipelineCreateInfo *cis, const VkAllocationCallbacks *alloc, VkPipeline *pPipelines) {
	VKV_TRACE("vkCreateGraphicsPipelines");
	(void)cache; (void)alloc;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < count; i++) {
		VkvPipeline *p = NULL;
		VkResult r = vkv_create_pipeline(device, &cis[i], &p);
		pPipelines[i] = r == VK_SUCCESS ? VKV_H(VkPipeline, p) : VK_NULL_HANDLE;
		if (r != VK_SUCCESS) result = r;
	}
	return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
	const VkComputePipelineCreateInfo *cis, const VkAllocationCallbacks *alloc, VkPipeline *pPipelines) {
	(void)device; (void)cache; (void)cis; (void)alloc;
	VKV_ERROR("compute pipelines are not available on GXM");
	for (uint32_t i = 0; i < count; i++) pPipelines[i] = VK_NULL_HANDLE;
	return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *alloc) {
	VKV_TRACE("vkDestroyPipeline");
	(void)alloc;
	VkvPipeline *p = VKV_ND(VkvPipeline, pipeline);
	if (!p) return;
	/* patched programs may still be referenced by in-flight scenes */
	vkv_device_wait_gpu(device);
	vkv_destroy_pipeline(device, p);
}
