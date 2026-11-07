/*
 * sdl3_gpu: SDL3's GPU API running on VkVita through SDL's Vulkan driver.
 *
 * Draws a textured quad that slides left and right on a dark blue background.
 * The texture is a white and grey checkerboard, tinted green by a fragment
 * uniform, so a green checkerboard means vertex buffers, index buffers,
 * texture uploads, samplers and both kinds of uniform pushes all work.
 *
 * Runs until START or a quit event, or for the number of frames given as the
 * first argument (used by the host test).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <vkvita/vkvita.h>
#elif defined(VKV_HOST_TEST)
int sceKernelExitProcess(int res); /* from the host test's mock, prints its report */
#endif

#include "quad_frag_spv.h"
#include "quad_vert_spv.h"

#define CHECK(x)                                                         \
	do {                                                                 \
		if (!(x)) {                                                      \
			SDL_Log("sdl3_gpu: %s failed: %s", #x, SDL_GetError());       \
			return 1;                                                    \
		}                                                                \
	} while (0)

typedef struct {
	float x, y, u, v;
} Vertex;

static SDL_GPUShader *make_shader(SDL_GPUDevice *dev, const uint32_t *code, size_t size, SDL_GPUShaderStage stage,
                                  uint32_t samplers, uint32_t uniforms) {
	SDL_GPUShaderCreateInfo ci = { 0 };
	ci.code = (const Uint8 *)code;
	ci.code_size = size;
	ci.entrypoint = "main";
	ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
	ci.stage = stage;
	ci.num_samplers = samplers;
	ci.num_uniform_buffers = uniforms;
	return SDL_CreateGPUShader(dev, &ci);
}

int main(int argc, char **argv) {
	int maxFrames = argc > 1 ? atoi(argv[1]) : 0;
#if defined(__vita__)
	/* the only VkVita call: the boot screen logo in the LiveArea color, log file for VkVita */
	VkvConfig cfg;
	vkvGetDefaultConfig(&cfg);
	cfg.splashColor = 0x32AAE6;
	cfg.logPath = "ux0:data/vkvita/sdl3_gpu.log";
	vkvSetConfig(&cfg);
#endif

	CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
	SDL_Window *win = SDL_CreateWindow("sdl3_gpu", 960, 544, SDL_WINDOW_VULKAN);
	CHECK(win);
	SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
	CHECK(dev);
	SDL_Log("sdl3_gpu: GPU driver %s", SDL_GetGPUDeviceDriver(dev));
	CHECK(SDL_ClaimWindowForGPUDevice(dev, win));

	SDL_GPUShader *vs = make_shader(dev, k_quad_vert_spv, sizeof(k_quad_vert_spv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
	SDL_GPUShader *fs = make_shader(dev, k_quad_frag_spv, sizeof(k_quad_frag_spv), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
	CHECK(vs && fs);

	SDL_GPUVertexBufferDescription vbd = { 0 };
	vbd.slot = 0;
	vbd.pitch = sizeof(Vertex);
	vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	SDL_GPUVertexAttribute attrs[2] = {
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 2 * sizeof(float) },
	};
	SDL_GPUColorTargetDescription ctd = { 0 };
	ctd.format = SDL_GetGPUSwapchainTextureFormat(dev, win);
	SDL_GPUGraphicsPipelineCreateInfo pci = { 0 };
	pci.vertex_shader = vs;
	pci.fragment_shader = fs;
	pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
	pci.vertex_input_state.num_vertex_buffers = 1;
	pci.vertex_input_state.vertex_attributes = attrs;
	pci.vertex_input_state.num_vertex_attributes = 2;
	pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	pci.target_info.color_target_descriptions = &ctd;
	pci.target_info.num_color_targets = 1;
	SDL_GPUGraphicsPipeline *pipe = SDL_CreateGPUGraphicsPipeline(dev, &pci);
	CHECK(pipe);
	SDL_ReleaseGPUShader(dev, vs);
	SDL_ReleaseGPUShader(dev, fs);

	/* geometry and texture go up through one transfer buffer */
	static const Vertex verts[4] = { { -1, -1, 0, 1 }, { 1, -1, 1, 1 }, { 1, 1, 1, 0 }, { -1, 1, 0, 0 } };
	static const Uint16 idx[6] = { 0, 1, 2, 0, 2, 3 };
	enum { TEX = 8 };
	Uint32 texels[TEX * TEX];
	for (int y = 0; y < TEX; y++)
		for (int x = 0; x < TEX; x++) texels[y * TEX + x] = ((x ^ y) & 1) ? 0xffffffffu : 0xff999999u;

	SDL_GPUBufferCreateInfo bci = { SDL_GPU_BUFFERUSAGE_VERTEX, sizeof(verts), 0 };
	SDL_GPUBuffer *vbuf = SDL_CreateGPUBuffer(dev, &bci);
	bci.usage = SDL_GPU_BUFFERUSAGE_INDEX;
	bci.size = sizeof(idx);
	SDL_GPUBuffer *ibuf = SDL_CreateGPUBuffer(dev, &bci);
	SDL_GPUTextureCreateInfo tci = { 0 };
	tci.type = SDL_GPU_TEXTURETYPE_2D;
	tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	tci.width = tci.height = TEX;
	tci.layer_count_or_depth = 1;
	tci.num_levels = 1;
	SDL_GPUTexture *tex = SDL_CreateGPUTexture(dev, &tci);
	SDL_GPUSamplerCreateInfo sci = { 0 };
	sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_NEAREST;
	sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	SDL_GPUSampler *smp = SDL_CreateGPUSampler(dev, &sci);
	CHECK(vbuf && ibuf && tex && smp);

	SDL_GPUTransferBufferCreateInfo tbci = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
		                                     sizeof(verts) + sizeof(idx) + sizeof(texels), 0 };
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
	CHECK(tb);
	Uint8 *map = SDL_MapGPUTransferBuffer(dev, tb, false);
	CHECK(map);
	SDL_memcpy(map, verts, sizeof(verts));
	SDL_memcpy(map + sizeof(verts), idx, sizeof(idx));
	SDL_memcpy(map + sizeof(verts) + sizeof(idx), texels, sizeof(texels));
	SDL_UnmapGPUTransferBuffer(dev, tb);

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
	CHECK(cmd);
	SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTransferBufferLocation src = { tb, 0 };
	SDL_GPUBufferRegion dst = { vbuf, 0, sizeof(verts) };
	SDL_UploadToGPUBuffer(copy, &src, &dst, false);
	src.offset = sizeof(verts);
	dst = (SDL_GPUBufferRegion){ ibuf, 0, sizeof(idx) };
	SDL_UploadToGPUBuffer(copy, &src, &dst, false);
	SDL_GPUTextureTransferInfo tti = { tb, sizeof(verts) + sizeof(idx), TEX, TEX };
	SDL_GPUTextureRegion tr = { 0 };
	tr.texture = tex;
	tr.w = tr.h = TEX;
	tr.d = 1;
	SDL_UploadToGPUTexture(copy, &tti, &tr, false);
	SDL_EndGPUCopyPass(copy);
	CHECK(SDL_SubmitGPUCommandBuffer(cmd));
	SDL_ReleaseGPUTransferBuffer(dev, tb);

	bool quit = false;
	for (int frame = 0; !quit && (!maxFrames || frame < maxFrames); frame++) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) quit = true;
			if (ev.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(ev.gdevice.which);
			if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) quit = true;
		}
		cmd = SDL_AcquireGPUCommandBuffer(dev);
		CHECK(cmd);
		SDL_GPUTexture *swap = NULL;
		CHECK(SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, NULL, NULL));
		if (swap) {
			SDL_GPUColorTargetInfo ct = { 0 };
			ct.texture = swap;
			ct.clear_color = (SDL_FColor){ 0.05f, 0.08f, 0.25f, 1.0f };
			ct.load_op = SDL_GPU_LOADOP_CLEAR;
			ct.store_op = SDL_GPU_STOREOP_STORE;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
			SDL_BindGPUGraphicsPipeline(pass, pipe);
			SDL_GPUBufferBinding vb = { vbuf, 0 }, ib = { ibuf, 0 };
			SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
			SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
			SDL_GPUTextureSamplerBinding tsb = { tex, smp };
			SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
			float t = (float)(frame % 240) / 240.0f;
			float place[4] = { (t < 0.5f ? t : 1.0f - t) * 1.2f - 0.3f, 0.0f, 0.3f, 0.5f };
			float tint[4] = { 0.2f, 1.0f, 0.4f, 1.0f };
			SDL_PushGPUVertexUniformData(cmd, 0, place, sizeof(place));
			SDL_PushGPUFragmentUniformData(cmd, 0, tint, sizeof(tint));
			SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
			SDL_EndGPURenderPass(pass);
		}
		CHECK(SDL_SubmitGPUCommandBuffer(cmd));
	}

	SDL_WaitForGPUIdle(dev);
	SDL_ReleaseGPUSampler(dev, smp);
	SDL_ReleaseGPUTexture(dev, tex);
	SDL_ReleaseGPUBuffer(dev, ibuf);
	SDL_ReleaseGPUBuffer(dev, vbuf);
	SDL_ReleaseGPUGraphicsPipeline(dev, pipe);
	SDL_ReleaseWindowFromGPUDevice(dev, win);
	SDL_DestroyGPUDevice(dev);
	SDL_DestroyWindow(win);
	SDL_Quit();
	SDL_Log("sdl3_gpu: done");
#if defined(__vita__) || defined(VKV_HOST_TEST)
	sceKernelExitProcess(0);
#endif
	return 0;
}
