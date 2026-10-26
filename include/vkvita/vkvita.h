/*
 * VkVita - Vulkan-style API on top of the PS Vita GXM graphics stack.
 *
 * Include <vulkan/vulkan.h> as usual and link libvkvita.a. The standard
 * vk* entry points listed in docs/COVERAGE.md are implemented directly by
 * this library (no loader, no ICD). This header adds the Vita-specific
 * pieces: configuration, debug/profiling controls, the display surface and
 * shader module helpers.
 *
 * SPDX-License-Identifier: MIT
 */
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

/* ------------------------------------------------------------------------ */
/* Debug and profiling switches. All of them can be changed at runtime.      */
/* ------------------------------------------------------------------------ */
typedef enum VkvDebugFlagBits {
	VKV_DEBUG_LOG_INFO        = 1u << 0, /* general init/teardown info */
	VKV_DEBUG_LOG_SHADERS     = 1u << 1, /* shader reflection + Cg compiler output */
	VKV_DEBUG_VALIDATION      = 1u << 2, /* extra error checks on every call/draw */
	VKV_DEBUG_PROFILE         = 1u << 3, /* per-frame stats, report every N frames */
	VKV_DEBUG_LOG_CALLS       = 1u << 4, /* trace API calls (slow, for crash hunting) */
	VKV_DEBUG_GPU_SYNC        = 1u << 5, /* wait for the GPU after every submit */
	VKV_DEBUG_DUMP_SHADERS    = 1u << 6, /* write runtime-compiled GXP to shaderDir */
	VKV_DEBUG_LOG_UNSUPPORTED = 1u << 7, /* report each unsupported feature once */
	VKV_DEBUG_BREAK_ON_ERROR  = 1u << 8  /* validation errors make the call fail hard */
} VkvDebugFlagBits;
typedef uint32_t VkvDebugFlags;

/* ------------------------------------------------------------------------ */
/* Compatibility switches for older code and porting quirks.                */
/* ------------------------------------------------------------------------ */
typedef enum VkvCompatFlagBits {
	VKV_COMPAT_GL_CLIP_SPACE  = 1u << 0, /* OpenGL clip space: Y up, Z in [-1,1] */
	VKV_COMPAT_INVERT_CULL    = 1u << 1, /* swap CW/CCW culling */
	VKV_COMPAT_DISABLE_CULL   = 1u << 2, /* never cull (find missing geometry) */
	VKV_COMPAT_CPU_CLEAR      = 1u << 3, /* clear with the CPU, not the transfer unit */
	VKV_COMPAT_SHADER_CACHE   = 1u << 4, /* look for <shaderDir>/<hash>.gxp before compiling Cg */
	VKV_COMPAT_FORCE_VSYNC    = 1u << 5, /* every present waits for vblank */
	VKV_COMPAT_NO_VSYNC       = 1u << 6, /* no present waits for vblank */
	VKV_COMPAT_IGNORE_SCISSOR = 1u << 7, /* skip scissor (region clip) */
	VKV_COMPAT_FAST_TRANSFERS = 1u << 8  /* don't wait for the GPU before CPU-side copies (faster, may race) */
} VkvCompatFlagBits;
typedef uint32_t VkvCompatFlags;

typedef struct VkvConfig {
	VkvDebugFlags  debugFlags;
	VkvCompatFlags compatFlags;
	const char    *logPath;            /* log file, NULL = debug output only (sceClibPrintf) */
	const char    *shaderDir;          /* dump/cache dir, default "ux0:data/vkvita/shaders" */
	const char    *sharkModulePath;    /* NULL = vitaShaRK default (ur0:data/libshacccg.suprx) */
	uint32_t       profileInterval;    /* frames per profile report, default 120 */
	uint32_t       parameterBufferSize;/* GXM parameter buffer, default 16 MiB */
	uint32_t       displayQueueDepth;  /* max pending flips, default 2 */
	uint32_t       poolBlockSize;      /* sub-allocator block size, default 4 MiB */
	uint32_t       poolMaxAllocSize;   /* allocations up to this size are pooled, default 512 KiB */
	uint32_t       scenesPerFrame;     /* hint for render targets, default 4 */
	uint32_t       ffVertexBufferSize; /* fixed-function streaming buffer per frame, default 1 MiB */
	uint32_t       ffFrames;           /* fixed-function frames in flight, default 3 */
} VkvConfig;

void     vkvGetDefaultConfig(VkvConfig *config);
/* Call before vkCreateInstance. Strings are copied. */
void     vkvSetConfig(const VkvConfig *config);
void     vkvGetConfig(VkvConfig *config);
/* Runtime toggles (safe between frames). */
void     vkvSetDebugFlags(VkvDebugFlags flags);
void     vkvSetCompatFlags(VkvCompatFlags flags);
/*
 * Load "key = value" settings from a text file, merged over the current
 * config. Returns 0 on success, <0 if the file could not be read.
 * vkCreateInstance automatically tries "ux0:data/vkvita/config.txt".
 * See docs/DEBUGGING.md for the keys.
 */
int      vkvLoadConfigFile(const char *path);

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */
typedef enum VkvLogLevel {
	VKV_LOG_DEBUG = 0,
	VKV_LOG_INFO  = 1,
	VKV_LOG_WARN  = 2,
	VKV_LOG_ERROR = 3
} VkvLogLevel;
typedef void (*PFN_vkvLogCallback)(VkvLogLevel level, const char *message, void *user);
void vkvSetLogCallback(PFN_vkvLogCallback callback, void *user);
void vkvLog(VkvLogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ------------------------------------------------------------------------ */
/* Profiling                                                                */
/* ------------------------------------------------------------------------ */
typedef struct VkvFrameStats {
	uint32_t frame;            /* presented frames so far */
	uint32_t drawCalls;
	uint32_t indices;          /* indices/vertices submitted */
	uint32_t scenes;           /* GXM scenes (render passes) */
	uint32_t pipelineBinds;
	uint32_t descriptorBinds;
	uint32_t clears;
	uint32_t copies;           /* buffer/image copies executed */
	uint32_t ffDraws;          /* fixed-function draws */
	float    submitCpuMs;      /* CPU time spent in vkQueueSubmit */
	float    gpuWaitMs;        /* time blocked on fences / GPU sync */
	float    frameMs;          /* present-to-present */
	float    fps;              /* averaged over the profile interval */
	uint32_t cdramBytes;       /* video memory in use */
	uint32_t ramBytes;         /* uncached main memory in use */
	uint32_t validationErrors; /* total since start */
} VkvFrameStats;

/* Stats of the last completed frame (updated by vkQueuePresentKHR). */
void vkvGetFrameStats(VkvFrameStats *stats);

/* ------------------------------------------------------------------------ */
/* Display surface                                                          */
/* ------------------------------------------------------------------------ */
/*
 * The Vita has one display. Either call this, or use the standard
 * VK_EXT_headless_surface (vkCreateHeadlessSurfaceEXT), which maps to the
 * same display surface so unmodified code works.
 */
VkResult vkvCreateDisplaySurface(VkInstance instance, VkSurfaceKHR *surface);

/* ------------------------------------------------------------------------ */
/* Shaders                                                                  */
/* ------------------------------------------------------------------------ */
/*
 * GXM cannot run SPIR-V. VkShaderModuleCreateInfo::pCode may hold:
 *   - a GXP binary (compiled offline with psp2cgc), detected by its "GXP" magic
 *   - Cg source text, compiled at pipeline creation with vitaShaRK
 *     (needs libshacccg.suprx on the device and a VKV_WITH_SHARK build)
 * SPIR-V input is rejected with a clear error. For Cg source, codeSize is
 * the string length (the multiple-of-4 rule is not enforced).
 *
 * Chain VkvShaderModuleInfoVITA into pNext to control how Vulkan locations
 * and push constants map onto GXM parameter names.
 */
#define VKV_STRUCTURE_TYPE_SHADER_MODULE_INFO_VITA ((VkStructureType)1450100000)

typedef enum VkvShaderSource {
	VKV_SHADER_SOURCE_AUTO = 0, /* detect GXP / SPIR-V magic, otherwise Cg */
	VKV_SHADER_SOURCE_GXP  = 1,
	VKV_SHADER_SOURCE_CG   = 2
} VkvShaderSource;

/* Vertex input location -> attribute parameter name in the vertex shader. */
typedef struct VkvAttributeBindingVITA {
	uint32_t    location;
	const char *name;
} VkvAttributeBindingVITA;

/* Default-uniform parameter name -> byte offset in the push constant block. */
typedef struct VkvUniformBindingVITA {
	const char *name;
	uint32_t    offset;
} VkvUniformBindingVITA;

typedef struct VkvShaderModuleInfoVITA {
	VkStructureType                sType; /* VKV_STRUCTURE_TYPE_SHADER_MODULE_INFO_VITA */
	const void                    *pNext;
	VkvShaderSource                source;
	const char                    *debugName;
	uint32_t                       attributeBindingCount;
	const VkvAttributeBindingVITA *pAttributeBindings;
	uint32_t                       uniformBindingCount;
	const VkvUniformBindingVITA   *pUniformBindings;
} VkvShaderModuleInfoVITA;

/* Convenience: create a module from Cg source or from a file (.gxp or Cg). */
VkResult vkvCreateShaderModuleFromCg(VkDevice device, const char *source, const char *debugName,
                                     VkShaderModule *module);
VkResult vkvCreateShaderModuleFromFile(VkDevice device, const char *path, VkShaderModule *module);

/* 1 if runtime Cg compilation is available (built with vitaShaRK and the
 * compiler module was found on the device). */
int vkvShaderCompilerAvailable(VkDevice device);

#ifdef __cplusplus
}
#endif

#endif /* VKVITA_H */
