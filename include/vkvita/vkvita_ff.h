// SPDX-License-Identifier: MIT
#ifndef VKVITA_FF_H
#define VKVITA_FF_H

#include "vkvita.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VkvFFPrimitive {
	VKVFF_POINTS = 0,
	VKVFF_LINES,
	VKVFF_TRIANGLES,
	VKVFF_TRIANGLE_STRIP,
	VKVFF_TRIANGLE_FAN,
	VKVFF_QUADS
} VkvFFPrimitive;

typedef enum VkvFFMatrixMode {
	VKVFF_MODELVIEW = 0,
	VKVFF_PROJECTION,
	VKVFF_TEXTURE
} VkvFFMatrixMode;

typedef enum VkvFFCap {
	VKVFF_TEXTURE_2D = 1u << 0,
	VKVFF_BLEND      = 1u << 1,
	VKVFF_DEPTH_TEST = 1u << 2,
	VKVFF_CULL_FACE  = 1u << 3,
	VKVFF_ALPHA_TEST = 1u << 4,
	VKVFF_DEPTH_WRITE= 1u << 5
} VkvFFCap;

typedef struct VkvFFInitInfo {
	VkRenderPass renderPass;
	uint32_t     subpass;
	const void  *vertexGxp;        size_t vertexGxpSize;
	const void  *fragmentColorGxp; size_t fragmentColorGxpSize;
	const void  *fragmentTexGxp;   size_t fragmentTexGxpSize;
} VkvFFInitInfo;

typedef struct VkvFFContext_T *VkvFFContext;

VkResult vkvffCreateContext(VkDevice device, const VkvFFInitInfo *info, VkvFFContext *ctx);
void     vkvffDestroyContext(VkvFFContext ctx);

void     vkvffBeginFrame(VkvFFContext ctx);

void vkvffEnable(VkvFFContext ctx, uint32_t caps);
void vkvffDisable(VkvFFContext ctx, uint32_t caps);
void vkvffBlendFunc(VkvFFContext ctx, VkBlendFactor src, VkBlendFactor dst);
void vkvffAlphaFunc(VkvFFContext ctx, float ref);
void vkvffCullFace(VkvFFContext ctx, VkCullModeFlags mode, VkFrontFace front);
void vkvffBindTexture(VkvFFContext ctx, VkImageView view, VkSampler sampler);

void vkvffMatrixMode(VkvFFContext ctx, VkvFFMatrixMode mode);
void vkvffLoadIdentity(VkvFFContext ctx);
void vkvffLoadMatrixf(VkvFFContext ctx, const float m[16]);
void vkvffMultMatrixf(VkvFFContext ctx, const float m[16]);
void vkvffPushMatrix(VkvFFContext ctx);
void vkvffPopMatrix(VkvFFContext ctx);
void vkvffTranslatef(VkvFFContext ctx, float x, float y, float z);
void vkvffScalef(VkvFFContext ctx, float x, float y, float z);
void vkvffRotatef(VkvFFContext ctx, float angleDeg, float x, float y, float z);
void vkvffOrtho(VkvFFContext ctx, float l, float r, float b, float t, float n, float f);
void vkvffFrustum(VkvFFContext ctx, float l, float r, float b, float t, float n, float f);
void vkvffPerspective(VkvFFContext ctx, float fovyDeg, float aspect, float n, float f);

void vkvffBegin(VkvFFContext ctx, VkCommandBuffer cmd, VkvFFPrimitive prim);
void vkvffColor4f(VkvFFContext ctx, float r, float g, float b, float a);
void vkvffColor4ub(VkvFFContext ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void vkvffTexCoord2f(VkvFFContext ctx, float u, float v);
void vkvffVertex2f(VkvFFContext ctx, float x, float y);
void vkvffVertex3f(VkvFFContext ctx, float x, float y, float z);
void vkvffEnd(VkvFFContext ctx);

void vkvffDrawArrays(VkvFFContext ctx, VkCommandBuffer cmd, VkvFFPrimitive prim, uint32_t count,
                     const float *xyz, const float *uv, const uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif
