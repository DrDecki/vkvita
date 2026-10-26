/*
 * VkVita fixed-function compatibility layer.
 *
 * A small OpenGL 1.x style immediate-mode API (matrix stacks, Begin/End,
 * client arrays, blending, alpha test, one texture) that records into a
 * regular VkCommandBuffer inside a render pass. It exists so older
 * fixed-function code can be brought up quickly, then moved to real
 * pipelines piece by piece.
 *
 * Its built-in shaders are Cg compiled at runtime with vitaShaRK, or GXP
 * blobs you provide in VkvFFInitInfo (see docs/FIXED_FUNCTION.md).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
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
	VKVFF_QUADS            /* converted to triangles */
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
	VKVFF_DEPTH_WRITE= 1u << 5   /* on by default, like glDepthMask(GL_TRUE) */
} VkvFFCap;

typedef struct VkvFFInitInfo {
	VkRenderPass renderPass;     /* render pass the FF draws are recorded in */
	uint32_t     subpass;        /* must be 0 */
	/* Optional precompiled shaders (GXP). If NULL, Cg is compiled at runtime. */
	const void  *vertexGxp;        size_t vertexGxpSize;
	const void  *fragmentColorGxp; size_t fragmentColorGxpSize;
	const void  *fragmentTexGxp;   size_t fragmentTexGxpSize;
} VkvFFInitInfo;

typedef struct VkvFFContext_T *VkvFFContext;

VkResult vkvffCreateContext(VkDevice device, const VkvFFInitInfo *info, VkvFFContext *ctx);
void     vkvffDestroyContext(VkvFFContext ctx);

/* Call once per frame before recording; rotates the streaming buffer.
 * Waits until the buffer slot from ffFrames frames ago is free. */
void     vkvffBeginFrame(VkvFFContext ctx);

/* State */
void vkvffEnable(VkvFFContext ctx, uint32_t caps);
void vkvffDisable(VkvFFContext ctx, uint32_t caps);
void vkvffBlendFunc(VkvFFContext ctx, VkBlendFactor src, VkBlendFactor dst);
void vkvffAlphaFunc(VkvFFContext ctx, float ref); /* passes when alpha >= ref */
void vkvffCullFace(VkvFFContext ctx, VkCullModeFlags mode, VkFrontFace front);
void vkvffBindTexture(VkvFFContext ctx, VkImageView view, VkSampler sampler);

/* Matrices (column-major, OpenGL conventions) */
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

/* Immediate mode */
void vkvffBegin(VkvFFContext ctx, VkCommandBuffer cmd, VkvFFPrimitive prim);
void vkvffColor4f(VkvFFContext ctx, float r, float g, float b, float a);
void vkvffColor4ub(VkvFFContext ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void vkvffTexCoord2f(VkvFFContext ctx, float u, float v);
void vkvffVertex2f(VkvFFContext ctx, float x, float y);
void vkvffVertex3f(VkvFFContext ctx, float x, float y, float z);
void vkvffEnd(VkvFFContext ctx);

/* Client arrays (like glDrawArrays). Any of uv/rgba may be NULL. */
void vkvffDrawArrays(VkvFFContext ctx, VkCommandBuffer cmd, VkvFFPrimitive prim, uint32_t count,
                     const float *xyz, const float *uv, const uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* VKVITA_FF_H */
