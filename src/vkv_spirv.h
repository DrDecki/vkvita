// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VKV_SPIRV_H
#define VKV_SPIRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKV_SPIRV_MAGIC 0x07230203u

#define VKV_SPIRV_INTERNAL_SET 31

typedef enum VkvSpirvStage { VKV_SPIRV_VERTEX = 0, VKV_SPIRV_FRAGMENT = 1 } VkvSpirvStage;

typedef enum VkvSpirvScalar { VKV_SPIRV_FLOAT = 0, VKV_SPIRV_INT = 1, VKV_SPIRV_UINT = 2 } VkvSpirvScalar;

typedef struct VkvSpirvUniform {
	char          *name;
	uint32_t       offset;
	uint32_t       stride;
	int32_t        set;
	uint32_t       binding;
	VkvSpirvScalar scalar;
} VkvSpirvUniform;

typedef struct VkvSpirvTexture {
	uint32_t unit;
	int32_t  imageSet;
	uint32_t imageBinding;
	uint32_t imageElement;
	int32_t  samplerSet;
	uint32_t samplerBinding;
	uint32_t samplerElement;
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
void vkv_spirv_set_storage_array_size(uint32_t elements);
void vkv_spirv_set_scissor_variant(int enabled);
void vkv_spirv_free(VkvSpirvResult *r);

int vkv_spirv_lower_subpass(const uint32_t *words, size_t wordCount, uint32_t **outWords, size_t *outCount);

int vkv_spirv_lower_builtins(const uint32_t *words, size_t wordCount, uint32_t **outWords, size_t *outCount);

typedef struct VkvSpirvStride {
	uint32_t structId, member, stride, offset;
} VkvSpirvStride;

int vkv_spirv_lower_ssbo(const uint32_t *words, size_t wordCount, uint32_t arraySize, uint32_t **outWords,
                         size_t *outCount, VkvSpirvStride *strides, uint32_t *strideCount, uint32_t maxStrides);

int vkv_spirv_lower_intdiv(const uint32_t *words, size_t wordCount, uint32_t **outWords, size_t *outCount);

void vkv_spirv_native_set_options(uint32_t storageArraySize, int scissorVariant);
int  vkv_spirv_translate_native(const uint32_t *words, size_t wordCount, VkvSpirvStage stage, VkvSpirvLogFn log,
                                void *user, const VkvSpirvStride *strides, uint32_t strideCount, VkvSpirvResult *out);

#ifdef __cplusplus
}
#endif

#endif
