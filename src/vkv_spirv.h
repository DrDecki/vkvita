/*
 * VkVita: SPIR-V to Cg translation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef VKV_SPIRV_H
#define VKV_SPIRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKV_SPIRV_MAGIC 0x07230203u

/* Descriptor set of the uniform block VkVita adds for lowered subpass inputs.
 * It holds one float2, 1 / framebuffer size, and is filled while drawing. */
#define VKV_SPIRV_INTERNAL_SET 31

typedef enum VkvSpirvStage { VKV_SPIRV_VERTEX = 0, VKV_SPIRV_FRAGMENT = 1 } VkvSpirvStage;

typedef enum VkvSpirvScalar { VKV_SPIRV_FLOAT = 0, VKV_SPIRV_INT = 1, VKV_SPIRV_UINT = 2 } VkvSpirvScalar;

typedef struct VkvSpirvUniform {
	char          *name;
	uint32_t       offset;
	int32_t        set;
	uint32_t       binding;
	VkvSpirvScalar scalar;
} VkvSpirvUniform;

/* A GXM texture unit and the descriptors that feed it. samplerSet is -1 when
 * the image is a combined image sampler, -2 when no sampler is used. */
typedef struct VkvSpirvTexture {
	uint32_t unit;
	int32_t  imageSet;
	uint32_t imageBinding;
	int32_t  samplerSet;
	uint32_t samplerBinding;
} VkvSpirvTexture;

typedef struct VkvSpirvResult {
	char            *cg;
	VkvSpirvUniform *unis;
	uint32_t         uniCount;
	VkvSpirvTexture *texs;
	uint32_t         texCount;
} VkvSpirvResult;

typedef enum VkvSpirvLogLevel { VKV_SPIRV_LOG_INFO = 0, VKV_SPIRV_LOG_WARN = 1, VKV_SPIRV_LOG_ERROR = 2 } VkvSpirvLogLevel;

typedef void (*VkvSpirvLogFn)(VkvSpirvLogLevel level, const char *msg, void *user);

int  vkv_spirv_translate(const uint32_t *words, size_t wordCount, VkvSpirvStage stage, VkvSpirvLogFn log, void *user,
                         VkvSpirvResult *out);
void vkv_spirv_free(VkvSpirvResult *r);

/* Rewrites subpass inputs into textures sampled at the current pixel.
 * Returns 1 and a malloc'ed module when something changed, 0 when the module
 * has no subpass inputs and -1 on malformed input. */
int vkv_spirv_lower_subpass(const uint32_t *words, size_t wordCount, uint32_t **outWords, size_t *outCount);

#ifdef __cplusplus
}
#endif

#endif
