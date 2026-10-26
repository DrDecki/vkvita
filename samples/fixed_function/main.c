/*
 * VkVita sample: fixed_function
 *
 * Old OpenGL 1.x style code (matrix stacks, Begin/End, quads, lines,
 * blending, alpha test, client arrays) running through the VkVita
 * fixed-function layer inside a normal Vulkan render pass.
 *
 * Controls: CROSS alpha test on/off, CIRCLE blending on/off, START quit.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <vkvita/vkvita_ff.h>
#include "vkv_sample.h"

#define TEX 64

static void cube(VkvFFContext ff) {
	static const float v[8][3] = { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
	                               { -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 } };
	static const int q[6][4] = { { 4, 5, 6, 7 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 },
	                             { 0, 4, 7, 3 }, { 7, 6, 2, 3 }, { 0, 1, 5, 4 } };
	static const float c[6][3] = { { 1, .3f, .3f }, { .3f, 1, .3f }, { .3f, .3f, 1 },
	                               { 1, 1, .3f },   { .3f, 1, 1 },   { 1, .3f, 1 } };
	(void)ff;
	for (int f = 0; f < 6; f++) {
		vkvffColor4f(ff, c[f][0], c[f][1], c[f][2], 1.0f);
		for (int i = 0; i < 4; i++) vkvffVertex3f(ff, v[q[f][i]][0], v[q[f][i]][1], v[q[f][i]][2]);
	}
}

int main(void) {
	sample_configure("fixed_function");
	Sample s;
	sample_init(&s, "fixed_function", true);

	/* A small RGBA texture with a soft alpha edge (for alpha test/blend). */
	VkBuffer staging;
	VkDeviceMemory stagingMem;
	uint8_t *px;
	sample_create_buffer(&s, TEX * TEX * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                     &staging, &stagingMem, (void **)&px);
	for (int y = 0; y < TEX; y++)
		for (int x = 0; x < TEX; x++) {
			float dx = (x - TEX / 2 + 0.5f) / (TEX / 2), dy = (y - TEX / 2 + 0.5f) / (TEX / 2);
			float d = sqrtf(dx * dx + dy * dy);
			uint8_t *p = px + (y * TEX + x) * 4;
			p[0] = (uint8_t)(255 * (0.5f + 0.5f * dx));
			p[1] = 200;
			p[2] = (uint8_t)(255 * (0.5f + 0.5f * dy));
			p[3] = d < 1.0f ? (uint8_t)(255 * (1.0f - d)) : 0;
		}
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM;
	ii.extent = (VkExtent3D){ TEX, TEX, 1 };
	ii.mipLevels = ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImage img;
	VK_CHECK(vkCreateImage(s.device, &ii, NULL, &img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s.device, img, &req);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = sample_memory_type(&s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory imgMem;
	VK_CHECK(vkAllocateMemory(s.device, &ai, NULL, &imgMem));
	VK_CHECK(vkBindImageMemory(s.device, img, imgMem, 0));
	VkCommandBuffer up = sample_begin_once(&s);
	VkBufferImageCopy bic = { 0 };
	bic.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	bic.imageExtent = ii.extent;
	vkCmdCopyBufferToImage(up, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
	sample_end_once(&s, up);
	vkDestroyBuffer(s.device, staging, NULL);
	vkFreeMemory(s.device, stagingMem, NULL);
	VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vci.image = img;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ii.format;
	vci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageView view;
	VK_CHECK(vkCreateImageView(s.device, &vci, NULL, &view));
	VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler sampler;
	VK_CHECK(vkCreateSampler(s.device, &sci, NULL, &sampler));

	/* The FF context records into command buffers of this render pass. */
	VkvFFInitInfo ffi = { 0 };
	ffi.renderPass = s.renderPass;
	VkvFFContext ff;
	VK_CHECK(vkvffCreateContext(s.device, &ffi, &ff));

	bool alphaTest = true, blend = true;
	float t = 0.0f;
	while (sample_poll_input(&s)) {
		if (s.pressed & SCE_CTRL_CROSS) alphaTest = !alphaTest;
		if (s.pressed & SCE_CTRL_CIRCLE) blend = !blend;
		t += 1.0f / 60.0f;

		VkCommandBuffer cb = sample_begin_frame(&s);
		vkvffBeginFrame(ff);

		/* 3D: spinning cube made of GL_QUADS, depth tested */
		vkvffMatrixMode(ff, VKVFF_PROJECTION);
		vkvffLoadIdentity(ff);
		vkvffPerspective(ff, 60.0f, (float)s.extent.width / (float)s.extent.height, 0.1f, 100.0f);
		vkvffMatrixMode(ff, VKVFF_MODELVIEW);
		vkvffLoadIdentity(ff);
		vkvffTranslatef(ff, -1.2f, 0.0f, -5.0f);
		vkvffRotatef(ff, t * 50.0f, 1.0f, 1.0f, 0.0f);
		vkvffEnable(ff, VKVFF_DEPTH_TEST | VKVFF_CULL_FACE);
		vkvffDisable(ff, VKVFF_BLEND | VKVFF_TEXTURE_2D | VKVFF_ALPHA_TEST);
		vkvffBegin(ff, cb, VKVFF_QUADS);
		cube(ff);
		vkvffEnd(ff);

		/* textured, blended, alpha-tested sprite */
		vkvffLoadIdentity(ff);
		vkvffTranslatef(ff, 1.4f, 0.0f, -4.0f);
		vkvffRotatef(ff, t * 30.0f, 0.0f, 0.0f, 1.0f);
		vkvffDisable(ff, VKVFF_CULL_FACE);
		vkvffEnable(ff, VKVFF_TEXTURE_2D);
		vkvffBindTexture(ff, view, sampler);
		if (blend) { vkvffEnable(ff, VKVFF_BLEND); vkvffBlendFunc(ff, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA); }
		if (alphaTest) { vkvffEnable(ff, VKVFF_ALPHA_TEST); vkvffAlphaFunc(ff, 0.25f); }
		vkvffBegin(ff, cb, VKVFF_TRIANGLE_STRIP);
		vkvffColor4f(ff, 1, 1, 1, 1);
		vkvffTexCoord2f(ff, 0, 0); vkvffVertex2f(ff, -1, -1);
		vkvffTexCoord2f(ff, 1, 0); vkvffVertex2f(ff, 1, -1);
		vkvffTexCoord2f(ff, 0, 1); vkvffVertex2f(ff, -1, 1);
		vkvffTexCoord2f(ff, 1, 1); vkvffVertex2f(ff, 1, 1);
		vkvffEnd(ff);

		/* 2D HUD: glOrtho in pixels, lines + client arrays */
		vkvffDisable(ff, VKVFF_DEPTH_TEST | VKVFF_TEXTURE_2D | VKVFF_ALPHA_TEST);
		vkvffEnable(ff, VKVFF_BLEND);
		vkvffBlendFunc(ff, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
		vkvffMatrixMode(ff, VKVFF_PROJECTION);
		vkvffLoadIdentity(ff);
		vkvffOrtho(ff, 0, 960, 544, 0, -1, 1); /* top-left origin, like most 2D code */
		vkvffMatrixMode(ff, VKVFF_MODELVIEW);
		vkvffLoadIdentity(ff);
		vkvffBegin(ff, cb, VKVFF_LINES);
		vkvffColor4ub(ff, 255, 255, 255, 60);
		for (int x = 0; x <= 960; x += 64) { vkvffVertex2f(ff, (float)x, 500); vkvffVertex2f(ff, (float)x, 540); }
		vkvffEnd(ff);

		VkvFrameStats st;
		vkvGetFrameStats(&st);
		float w = st.frameMs > 0 ? st.frameMs * 20.0f : 0.0f; /* 16.6 ms ~ 333 px */
		const float bar[] = { 16, 16, 0, 16 + w, 16, 0, 16, 40, 0, 16 + w, 40, 0 };
		const uint8_t col[] = { 80, 220, 120, 200, 80, 220, 120, 200, 40, 120, 60, 200, 40, 120, 60, 200 };
		vkvffDrawArrays(ff, cb, VKVFF_TRIANGLE_STRIP, 4, bar, NULL, col);

		sample_end_frame(&s);
	}

	vkDeviceWaitIdle(s.device);
	vkvffDestroyContext(ff);
	vkDestroySampler(s.device, sampler, NULL);
	vkDestroyImageView(s.device, view, NULL);
	vkDestroyImage(s.device, img, NULL);
	vkFreeMemory(s.device, imgMem, NULL);
	sample_shutdown(&s);
	sceKernelExitProcess(0);
	return 0;
}
