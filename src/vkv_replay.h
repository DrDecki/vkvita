// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VKV_REPLAY_H
#define VKV_REPLAY_H

#include "vkv_internal.h"

#include <math.h>

#define VKV_UNI_SHADOW 2048

typedef struct VkvReplay {
	struct VkDevice_T        *dev;
	SceGxmContext            *ctx;

	bool                      inScene;
	VkvRenderPass            *rp;
	VkvFramebuffer           *fb;
	const struct VkvCmdBeginRP *begin;
	uint32_t                  subpass;
	float                     fbInv[4] __attribute__((aligned(16)));
	float                     scissorPx[4] __attribute__((aligned(16)));
	bool                      exactScissor;
	bool                      fpScissorOn;
	VkvImage                 *colorImg, *depthImg;
	VkvImageView             *colorView, *depthView;
	SceGxmColorSurface        color;
	SceGxmDepthStencilSurface depth;
	bool                      splits;
	uint32_t                  sceneDraws;
	bool                      sceneClearsDepth;
	float                     sceneDepth;
	uint32_t                  sceneStencil;
	uint32_t                  scenesLeft;
	VkvFence                 *fence;
	bool                      fenceAttached;
	bool                      transferTail;
	bool                      queryActive;

	VkvPipeline              *pipe;
	bool                      pipeDirty;
	bool                      resDirty;
	bool                      dynDirty;
	int                       vpVariant;
	VkvBuffer                *vb[VKV_MAX_BINDINGS];
	uint32_t                  vbOff[VKV_MAX_BINDINGS];
	VkvBuffer                *ib;
	uint32_t                  ibOff;
	VkIndexType               ibType;
	VkvSet                   *sets[VKV_MAX_SETS];
	uint32_t                  dyn[VKV_MAX_SETS][VKV_MAX_DYNAMIC];
	uint8_t                   push[VKV_MAX_PUSH] __attribute__((aligned(16)));
	uint32_t                  pushHigh;

	VkViewport                viewport;
	bool                      hasViewport, viewportDirty;
	VkRect2D                  scissor;
	bool                      hasScissor, scissorDirty;
	uint32_t                  sCmp[2], sWr[2], sRef[2];
	uint32_t                  dynSet;
	float                     biasConst, biasSlope;
	float                     lineWidth;

	uint8_t                   uniShadow[2][VKV_UNI_SHADOW] __attribute__((aligned(16)));
	uint8_t                   uniStage[VKV_UNI_SHADOW] __attribute__((aligned(16)));
	uint32_t                  uniShadowSize[2];
} VkvReplay;

void vkv_replay(VkvReplay *R, struct VkCommandBuffer_T *cb);
void vkv_begin_scene(VkvReplay *R, bool loadDepth, bool clearDepth, float depth, uint32_t stencil);
void vkv_end_scene(VkvReplay *R, bool final);

void vkv_clear_color_rect(VkvReplay *R, VkvImage *img, uint32_t level, uint32_t layer, const VkClearColorValue *c,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void vkv_exec_clear_attachments(VkvReplay *R, const VkvCmdClearAttach *c);
void vkv_exec_clear_color_image(VkvReplay *R, const VkvCmdClearColorImage *c);

void vkv_exec_draw(VkvReplay *R, const VkvCmdDraw *c, bool indexed);
void vkv_exec_draw_indirect(VkvReplay *R, const VkvCmdDrawIndirect *c);
void vkv_exec_ff_draw(VkvReplay *R, const VkvCmdFFDraw *c);

void vkv_transfer_sync(VkvReplay *R);
void vkv_exec_copy_buf_img(VkvReplay *R, const VkvCmdCopyBufImg *c, bool toImage);
void vkv_exec_copy_image(VkvReplay *R, const VkvCmdCopyImage *c);
void vkv_exec_blit_image(VkvReplay *R, const VkvCmdBlitImage *c);

#endif
