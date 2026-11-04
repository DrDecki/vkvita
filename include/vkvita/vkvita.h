// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VKVITA_H
#define VKVITA_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKVITA_VERSION_MAJOR 0
#define VKVITA_VERSION_MINOR 1
#define VKVITA_VERSION_PATCH 0

typedef enum VkvDebugFlagBits {
	VKV_DEBUG_LOG_INFO        = 1u << 0,
	VKV_DEBUG_LOG_SHADERS     = 1u << 1,
	VKV_DEBUG_VALIDATION      = 1u << 2,
	VKV_DEBUG_PROFILE         = 1u << 3,
	VKV_DEBUG_LOG_CALLS       = 1u << 4,
	VKV_DEBUG_GPU_SYNC        = 1u << 5,
	VKV_DEBUG_DUMP_SHADERS    = 1u << 6,
	VKV_DEBUG_LOG_UNSUPPORTED = 1u << 7,
	VKV_DEBUG_BREAK_ON_ERROR  = 1u << 8
} VkvDebugFlagBits;
typedef uint32_t VkvDebugFlags;

typedef enum VkvCompatFlagBits {
	VKV_COMPAT_GL_CLIP_SPACE  = 1u << 0,
	VKV_COMPAT_INVERT_CULL    = 1u << 1,
	VKV_COMPAT_DISABLE_CULL   = 1u << 2,
	VKV_COMPAT_CPU_CLEAR      = 1u << 3,
	VKV_COMPAT_SHADER_CACHE   = 1u << 4,
	VKV_COMPAT_FORCE_VSYNC    = 1u << 5,
	VKV_COMPAT_NO_VSYNC       = 1u << 6,
	VKV_COMPAT_IGNORE_SCISSOR = 1u << 7,
	VKV_COMPAT_FAST_TRANSFERS = 1u << 8,
	VKV_COMPAT_CPU_COPY       = 1u << 9
} VkvCompatFlagBits;
typedef uint32_t VkvCompatFlags;

typedef struct VkvConfig {
	VkvDebugFlags  debugFlags;
	VkvCompatFlags compatFlags;
	const char    *logPath;
	const char    *shaderDir;
	const char    *sharkModulePath;
	uint32_t       profileInterval;
	uint32_t       parameterBufferSize;
	uint32_t       displayQueueDepth;
	uint32_t       poolBlockSize;
	uint32_t       poolMaxAllocSize;
	uint32_t       scenesPerFrame;
	uint32_t       ffVertexBufferSize;
	uint32_t       ffFrames;
	uint32_t       storageArraySize;
	const char    *shaderPackDir;
	uint32_t       splash;
	uint32_t       spirvBackend;
} VkvConfig;

void     vkvGetDefaultConfig(VkvConfig *config);
void     vkvSetConfig(const VkvConfig *config);
void     vkvGetConfig(VkvConfig *config);
void     vkvSetDebugFlags(VkvDebugFlags flags);
void     vkvSetCompatFlags(VkvCompatFlags flags);
int      vkvLoadConfigFile(const char *path);

void     vkvShowSplash(void);

typedef enum VkvLogLevel {
	VKV_LOG_DEBUG = 0,
	VKV_LOG_INFO  = 1,
	VKV_LOG_WARN  = 2,
	VKV_LOG_ERROR = 3
} VkvLogLevel;
typedef void (*PFN_vkvLogCallback)(VkvLogLevel level, const char *message, void *user);
void vkvSetLogCallback(PFN_vkvLogCallback callback, void *user);
void vkvLog(VkvLogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

typedef struct VkvFrameStats {
	uint32_t frame;
	uint32_t drawCalls;
	uint32_t indices;
	uint32_t scenes;
	uint32_t pipelineBinds;
	uint32_t descriptorBinds;
	uint32_t clears;
	uint32_t copies;
	uint32_t ffDraws;
	float    submitCpuMs;
	float    gpuWaitMs;
	float    frameMs;
	float    fps;
	uint32_t cdramBytes;
	uint32_t ramBytes;
	uint32_t validationErrors;
} VkvFrameStats;

void vkvGetFrameStats(VkvFrameStats *stats);

VkResult vkvCreateDisplaySurface(VkInstance instance, VkSurfaceKHR *surface);

#define VKV_STRUCTURE_TYPE_SHADER_MODULE_INFO_VITA ((VkStructureType)1450100000)

typedef enum VkvShaderSource {
	VKV_SHADER_SOURCE_AUTO = 0,
	VKV_SHADER_SOURCE_GXP  = 1,
	VKV_SHADER_SOURCE_CG   = 2,
	VKV_SHADER_SOURCE_SPIRV = 3
} VkvShaderSource;

typedef struct VkvAttributeBindingVITA {
	uint32_t    location;
	const char *name;
} VkvAttributeBindingVITA;

typedef struct VkvUniformBindingVITA {
	const char *name;
	uint32_t    offset;
} VkvUniformBindingVITA;

typedef struct VkvShaderModuleInfoVITA {
	VkStructureType                sType;
	const void                    *pNext;
	VkvShaderSource                source;
	const char                    *debugName;
	uint32_t                       attributeBindingCount;
	const VkvAttributeBindingVITA *pAttributeBindings;
	uint32_t                       uniformBindingCount;
	const VkvUniformBindingVITA   *pUniformBindings;
} VkvShaderModuleInfoVITA;

VkResult vkvCreateShaderModuleFromCg(VkDevice device, const char *source, const char *debugName,
                                     VkShaderModule *module);
VkResult vkvCreateShaderModuleFromFile(VkDevice device, const char *path, VkShaderModule *module);

int vkvShaderCompilerAvailable(VkDevice device);

#ifdef __cplusplus
}
#endif

#endif
