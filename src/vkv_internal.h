/*
 * VkVita internal definitions.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef VKV_INTERNAL_H
#define VKV_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#if defined(__ARM_PCS) && !defined(__ARM_PCS_VFP)
void vkv_vfp_sceGxmSetViewport(SceGxmContext *context, float xOffset, float xScale, float yOffset, float yScale,
                               float zOffset, float zScale) __asm__("sceGxmSetViewport") __attribute__((pcs("aapcs-vfp")));
void vkv_vfp_sceGxmDepthStencilSurfaceSetBackgroundDepth(SceGxmDepthStencilSurface *surface, float backgroundDepth)
	__asm__("sceGxmDepthStencilSurfaceSetBackgroundDepth") __attribute__((pcs("aapcs-vfp")));
#define sceGxmSetViewport vkv_vfp_sceGxmSetViewport
#define sceGxmDepthStencilSurfaceSetBackgroundDepth vkv_vfp_sceGxmDepthStencilSurfaceSetBackgroundDepth
#endif

#include "vkvita/vkvita.h"
#include "vkvita/vkvita_ff.h"

/* ------------------------------------------------------------------------ */
/* Build options                                                            */
/* ------------------------------------------------------------------------ */
#ifndef VKV_ENABLE_DEBUG
#define VKV_ENABLE_DEBUG 1
#endif

#define VKV_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define VKV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define VKV_MAX(a, b) ((a) > (b) ? (a) : (b))
#define VKV_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define VKV_DISPLAY_WIDTH   960
#define VKV_DISPLAY_HEIGHT  544
#define VKV_DISPLAY_STRIDE  960
#define VKV_MAX_SWAP_IMAGES 4
#define VKV_MAX_SETS        4
#define VKV_MAX_BINDINGS    16   /* vertex buffer bindings */
#define VKV_MAX_PUSH        128  /* push constant bytes */
#define VKV_MAX_DYNAMIC     16   /* dynamic descriptors per set */
#define VKV_NOTIF_SLOTS     512
#define VKV_CDRAM_ALIGN     (256 * 1024)
#define VKV_RAM_ALIGN       4096

/* ------------------------------------------------------------------------ */
/* Handles                                                                  */
/* ------------------------------------------------------------------------ */
#define VKV_ND(T, h)    ((T *)(uintptr_t)(h))        /* non-dispatchable -> struct */
#define VKV_H(T, p)     ((T)(uintptr_t)(p))          /* struct -> handle */

enum {
	VKV_MAGIC_INSTANCE = 0x564B4931, VKV_MAGIC_PHYSDEV, VKV_MAGIC_DEVICE, VKV_MAGIC_QUEUE,
	VKV_MAGIC_MEMORY, VKV_MAGIC_BUFFER, VKV_MAGIC_IMAGE, VKV_MAGIC_VIEW, VKV_MAGIC_SAMPLER,
	VKV_MAGIC_SHADER, VKV_MAGIC_PLAYOUT, VKV_MAGIC_DSLAYOUT, VKV_MAGIC_DPOOL, VKV_MAGIC_DSET,
	VKV_MAGIC_RPASS, VKV_MAGIC_FBUF, VKV_MAGIC_PIPELINE, VKV_MAGIC_CPOOL, VKV_MAGIC_CMDBUF,
	VKV_MAGIC_FENCE, VKV_MAGIC_SEMAPHORE, VKV_MAGIC_SURFACE, VKV_MAGIC_SWAPCHAIN, VKV_MAGIC_PCACHE,
	VKV_MAGIC_FF, VKV_MAGIC_DEAD = 0xDEADBEEF
};

typedef struct VkvObject {
	uint32_t magic;
} VkvObject;

/* ------------------------------------------------------------------------ */
/* Global state, logging, profiling                                          */
/* ------------------------------------------------------------------------ */
typedef struct VkvStatsCounters {
	uint32_t drawCalls, indices, scenes, pipelineBinds, descriptorBinds, clears, copies, ffDraws;
	uint64_t submitUs, gpuWaitUs;
} VkvStatsCounters;

typedef struct VkvGlobals {
	VkvConfig          cfg;
	char               logPath[256];
	char               shaderDir[256];
	char               sharkPath[256];
	PFN_vkvLogCallback logCb;
	void              *logUser;
	int                logFd;
	bool               logFdTried;
	VkvStatsCounters   cur;
	VkvFrameStats      last;
	uint64_t           lastPresentUs;
	uint64_t           intervalStartUs;
	uint32_t           intervalFrames;
	uint32_t           validationErrors;
	uint32_t           cdramBytes, ramBytes;
	bool               validationLayer; /* VK_LAYER_KHRONOS_validation requested */
} VkvGlobals;

extern VkvGlobals g_vkv;

void vkv_log(VkvLogLevel lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void vkv_config_init_once(void);
void vkv_load_default_config_file(void);
uint64_t vkv_time_us(void);

static inline bool vkv_dbg(uint32_t flag) {
#if VKV_ENABLE_DEBUG
	return (g_vkv.cfg.debugFlags & flag) != 0;
#else
	(void)flag;
	return false;
#endif
}
static inline bool vkv_compat(uint32_t flag) { return (g_vkv.cfg.compatFlags & flag) != 0; }
static inline bool vkv_validate(void) {
#if VKV_ENABLE_DEBUG
	return (g_vkv.cfg.debugFlags & VKV_DEBUG_VALIDATION) != 0 || g_vkv.validationLayer;
#else
	return false;
#endif
}

#define VKV_INFO(...)  do { if (vkv_dbg(VKV_DEBUG_LOG_INFO)) vkv_log(VKV_LOG_INFO, __VA_ARGS__); } while (0)
#define VKV_WARN(...)  vkv_log(VKV_LOG_WARN, __VA_ARGS__)
#define VKV_ERROR(...) vkv_log(VKV_LOG_ERROR, __VA_ARGS__)
#define VKV_TRACE(name) do { if (vkv_dbg(VKV_DEBUG_LOG_CALLS)) vkv_log(VKV_LOG_DEBUG, "call %s", name); } while (0)

/* Validation failure: logs, counts, and returns true so callers can bail. */
bool vkv_fail(const char *func, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define VKV_CHECK(cond, ...) (vkv_validate() && !(cond) && vkv_fail(__func__, __VA_ARGS__))
/* Report a feature that is not supported, once per call site. */
void vkv_unsupported_once(const char *key, const char *what);
#define VKV_UNSUPPORTED(what) do { static const char k_ = 0; vkv_unsupported_once(&k_, what); } while (0)

/* Object validation: checks the magic when validation is on. */
#define VKV_OBJ_OK(p, m) ((p) != NULL && (!vkv_validate() || ((const VkvObject *)(p))->magic == (uint32_t)(m)))

/* ------------------------------------------------------------------------ */
/* GPU memory                                                               */
/* ------------------------------------------------------------------------ */
typedef enum VkvHeapKind { VKV_HEAP_CDRAM = 0, VKV_HEAP_RAM = 1, VKV_HEAP_COUNT } VkvHeapKind;

typedef struct VkvGpuBlock {
	void       *base;
	SceUID      uid;
	uint32_t    size;
	VkvHeapKind kind;
} VkvGpuBlock;

bool  vkv_gpu_alloc(VkvHeapKind kind, uint32_t size, SceGxmMemoryAttribFlags attr, VkvGpuBlock *out);
void  vkv_gpu_free(VkvGpuBlock *blk);
void *vkv_alloc_vertex_usse(uint32_t size, SceUID *uid, unsigned int *offset);
void *vkv_alloc_fragment_usse(uint32_t size, SceUID *uid, unsigned int *offset);
void  vkv_free_vertex_usse(SceUID uid);
void  vkv_free_fragment_usse(SceUID uid);

typedef struct VkvFreeRange {
	struct VkvFreeRange *next;
	uint32_t             offset, size;
} VkvFreeRange;

typedef struct VkvPoolBlock {
	struct VkvPoolBlock *next;
	VkvGpuBlock          blk;
	VkvFreeRange        *free;
	uint32_t             used;
} VkvPoolBlock;

typedef struct VkvPool {
	VkvPoolBlock *blocks[VKV_HEAP_COUNT];
} VkvPool;

/* ------------------------------------------------------------------------ */
/* Formats                                                                  */
/* ------------------------------------------------------------------------ */
typedef struct VkvFormatInfo {
	VkFormat            vk;
	uint8_t             bpp;          /* bytes per texel, 0 = unsupported */
	uint8_t             isDepth;
	uint8_t             renderable;   /* usable as a color attachment */
	uint8_t             attribComps;  /* vertex attribute components, 0 = not a vertex format */
	SceGxmTextureFormat tex;          /* 0 = not sampleable */
	SceGxmColorFormat   color;
	SceGxmAttributeFormat attrib;
	SceGxmOutputRegisterFormat outReg;
	SceGxmTransferFormat xfer;
} VkvFormatInfo;

const VkvFormatInfo *vkv_format(VkFormat f);

/* ------------------------------------------------------------------------ */
/* Instance / physical device / device                                      */
/* ------------------------------------------------------------------------ */
struct VkPhysicalDevice_T {
	VkvObject  obj;
	struct VkInstance_T *instance;
};

struct VkInstance_T {
	VkvObject                 obj;
	struct VkPhysicalDevice_T phys;
	uint32_t                  apiVersion;
};

typedef struct VkSurfaceKHR_T {
	VkvObject obj;
} VkvSurface;

struct VkQueue_T {
	VkvObject             obj;
	struct VkDevice_T    *device;
};

typedef struct VkvDisplayData {
	void    *address;
	uint32_t width, height, pitch;
	uint32_t vsync;
} VkvDisplayData;

struct VkDevice_T {
	VkvObject             obj;
	struct VkInstance_T  *instance;
	struct VkQueue_T      queue;

	SceGxmContext        *ctx;
	void                 *ctxHostMem;
	VkvGpuBlock           vdmRing, vertexRing, fragmentRing;
	SceUID                fragUsseRingUid;
	SceGxmShaderPatcher  *patcher;
	VkvGpuBlock           patcherBuf;
	SceUID                patcherVUsseUid, patcherFUsseUid;

	VkvPool               pool;

	/* notifications for fences */
	volatile unsigned int *notifRegion;
	uint8_t               notifUsed[VKV_NOTIF_SLOTS];
	uint32_t              notifCounter;

	/* 0..N index buffers for non-indexed draws */
	VkvGpuBlock           linearIdx16;
	uint32_t              linearIdx16Count;
	VkvGpuBlock           linearIdx32;
	uint32_t              linearIdx32Count;

	bool                  sharkReady;
	bool                  sharkTried;
	bool                  usedTransfer;   /* transfer unit used since last finish */
	bool                  gpuBusy;        /* scenes submitted since last finish */
	bool                  transferPending;/* next scene must wait for a transfer fill */
	struct VkSwapchainKHR_T *swapchain;  /* the one display swapchain */
};

/* ------------------------------------------------------------------------ */
/* Resources                                                                */
/* ------------------------------------------------------------------------ */
typedef struct VkDeviceMemory_T {
	VkvObject     obj;
	uint8_t      *ptr;
	uint32_t      size;
	uint32_t      typeIndex;
	VkvPoolBlock *block;   /* NULL = dedicated */
	uint32_t      offset;  /* in block */
	VkvGpuBlock   dedicated;
} VkvMemory;

typedef struct VkBuffer_T {
	VkvObject          obj;
	uint32_t           size;
	VkBufferUsageFlags usage;
	VkvMemory         *mem;
	uint8_t           *ptr;
} VkvBuffer;

typedef struct VkImage_T {
	VkvObject             obj;
	VkFormat              format;
	const VkvFormatInfo  *fi;
	uint32_t              width, height;
	uint32_t              mipLevels, arrayLayers;
	VkImageUsageFlags     usage;
	VkImageTiling         tiling;
	uint32_t              rowPitch;    /* bytes */
	uint32_t              alignedW, alignedH; /* depth: tile aligned */
	uint32_t              size, alignment;
	VkvMemory            *mem;
	uint8_t              *ptr;
	SceGxmSyncObject     *sync;        /* for color attachments */
	bool                  swapchain;
	bool                  written;     /* has been rendered to (for loadOp LOAD on depth) */
} VkvImage;

typedef struct VkImageView_T {
	VkvObject      obj;
	VkvImage      *image;
	VkFormat       format;
	SceGxmTexture  tex;
	bool           texValid;
} VkvImageView;

typedef struct VkSampler_T {
	VkvObject              obj;
	SceGxmTextureFilter    minFilter, magFilter;
	SceGxmTextureAddrMode  addrU, addrV;
} VkvSampler;

void vkv_texture_apply_sampler(SceGxmTexture *tex, const VkvSampler *s);

/* ------------------------------------------------------------------------ */
/* Shaders                                                                  */
/* ------------------------------------------------------------------------ */
typedef enum VkvStage { VKV_STAGE_VERTEX = 0, VKV_STAGE_FRAGMENT = 1, VKV_STAGE_COUNT } VkvStage;

typedef struct VkvUniformMap {
	const SceGxmProgramParameter *param;
	uint32_t offset;      /* byte offset in the push constant block */
	uint32_t comps;       /* components per element */
	uint32_t elems;       /* array size */
	uint32_t elemStride;  /* bytes between elements */
} VkvUniformMap;

typedef struct VkShaderModule_T {
	VkvObject                 obj;
	VkvShaderSource           source;
	char                     *code;   /* GXP bytes or NUL-terminated Cg */
	uint32_t                  codeSize;
	uint32_t                  hash;
	char                      name[48];
	uint32_t                  attrCount;
	VkvAttributeBindingVITA  *attrs;  /* names are owned copies */
	uint32_t                  uniCount;
	VkvUniformBindingVITA    *unis;
	/* program per stage, compiled/registered lazily */
	SceGxmProgram            *prog[VKV_STAGE_COUNT];
	bool                      progOwned[VKV_STAGE_COUNT];
	SceGxmShaderPatcherId     pid[VKV_STAGE_COUNT];
	bool                      registered[VKV_STAGE_COUNT];
	struct VkDevice_T        *device;
	uint32_t                  refs;      /* pipelines using this module */
	bool                      destroyed; /* vkDestroyShaderModule was called */
} VkvShaderModule;

void vkv_shader_retain(VkvShaderModule *m);
void vkv_shader_release(VkvShaderModule *m);

VkResult vkv_shader_get_program(struct VkDevice_T *dev, VkvShaderModule *m, VkvStage stage,
                                const SceGxmProgram **prog, SceGxmShaderPatcherId *pid);
void     vkv_shader_log_program(const char *name, const SceGxmProgram *p);
const SceGxmProgramParameter *vkv_shader_find_attribute(const VkvShaderModule *m, const SceGxmProgram *p,
                                                        uint32_t location);
uint32_t vkv_build_uniform_map(const VkvShaderModule *m, const SceGxmProgram *p, uint32_t baseOffset,
                               VkvUniformMap *out, uint32_t max);
bool     vkv_shark_init(struct VkDevice_T *dev);
void     vkv_shark_term(struct VkDevice_T *dev);
SceGxmProgram *vkv_shark_compile(struct VkDevice_T *dev, const char *src, VkvStage stage, const char *name,
                                 uint32_t hash, uint32_t *outSize);

/* ------------------------------------------------------------------------ */
/* Descriptors and pipelines                                                */
/* ------------------------------------------------------------------------ */
typedef struct VkvDslBinding {
	uint32_t          binding;
	VkDescriptorType  type;
	uint32_t          count;
	uint32_t          first;     /* index into the set's descriptor array */
	uint32_t          dynIndex;  /* for dynamic buffers: index into dynamic offsets */
} VkvDslBinding;

typedef struct VkDescriptorSetLayout_T {
	VkvObject      obj;
	uint32_t       bindingCount;
	VkvDslBinding *bindings;
	uint32_t       descriptorCount;
	uint32_t       dynamicCount;
} VkvSetLayout;

typedef struct VkvDescriptor {
	VkDescriptorType type;
	uint32_t         binding;
	VkvBuffer       *buffer;
	uint32_t         offset, range;
	VkvImageView    *view;
	VkvSampler      *sampler;
	SceGxmTexture    tex;      /* view texture + sampler state, ready to bind */
	bool             valid;
} VkvDescriptor;

typedef struct VkDescriptorSet_T {
	VkvObject                 obj;
	VkvSetLayout             *layout;
	struct VkDescriptorPool_T *pool;
	struct VkDescriptorSet_T *next;   /* pool list */
	struct VkDescriptorSet_T *prev;
	uint32_t                  count;
	VkvDescriptor             desc[];
} VkvSet;

typedef struct VkDescriptorPool_T {
	VkvObject obj;
	VkvSet   *sets;
} VkvDescPool;

typedef struct VkPipelineLayout_T {
	VkvObject          obj;
	uint32_t           setCount;
	VkvSetLayout      *sets[VKV_MAX_SETS];
	uint32_t           rangeCount;
	VkPushConstantRange ranges[8];
	uint32_t           pushSize;
} VkvPipelineLayout;

typedef struct VkvAttachmentDesc {
	VkFormat            format;
	VkAttachmentLoadOp  loadOp, stencilLoadOp;
	VkAttachmentStoreOp storeOp, stencilStoreOp;
} VkvAttachmentDesc;

typedef struct VkRenderPass_T {
	VkvObject         obj;
	uint32_t          attachmentCount;
	VkvAttachmentDesc att[8];
	int32_t           colorIndex;   /* attachment index or -1 */
	int32_t           depthIndex;   /* attachment index or -1 */
} VkvRenderPass;

typedef struct VkFramebuffer_T {
	VkvObject           obj;
	VkvRenderPass      *rp;
	uint32_t            width, height;
	VkvImageView       *views[8];
	uint32_t            viewCount;
	SceGxmRenderTarget *rt;
} VkvFramebuffer;

#define VKV_MAX_UNIFORM_MAP 32

typedef struct VkPipeline_T {
	VkvObject                 obj;
	struct VkDevice_T        *device;
	VkvPipelineLayout        *layout;
	VkvRenderPass            *rp;
	const SceGxmProgram      *vprog, *fprog;
	SceGxmShaderPatcherId     vpid, fpid;
	VkvShaderModule          *vmod, *fmod;
	char                      vname[48], fname[48];

	/* vertex input */
	SceGxmVertexAttribute     attrs[SCE_GXM_MAX_VERTEX_ATTRIBUTES];
	uint32_t                  attrCount;
	SceGxmVertexStream        streams[SCE_GXM_MAX_VERTEX_STREAMS];
	uint32_t                  streamCount;
	uint32_t                  streamBinding[SCE_GXM_MAX_VERTEX_STREAMS]; /* stream -> vk binding */
	uint8_t                   streamInstanced[SCE_GXM_MAX_VERTEX_STREAMS];
	uint16_t                  streamStride[SCE_GXM_MAX_VERTEX_STREAMS];   /* real stride */
	SceGxmVertexProgram      *vp[2];    /* [0]=16-bit index source, [1]=32-bit */
	SceGxmFragmentProgram    *fp;

	/* push constant mapping */
	VkvUniformMap             vuni[VKV_MAX_UNIFORM_MAP];
	uint32_t                  vuniCount;
	VkvUniformMap             funi[VKV_MAX_UNIFORM_MAP];
	uint32_t                  funiCount;
	uint32_t                  vDefaultSize, fDefaultSize;

	/* reflection for validation */
	uint32_t                  fSamplerMask, vSamplerMask, bufferMask;

	/* fixed state */
	SceGxmPrimitiveType       prim;
	bool                      primSupported;
	SceGxmCullMode            cull;
	VkCullModeFlags           vkCull;
	VkFrontFace               vkFront;
	SceGxmPolygonMode         polyMode;
	SceGxmDepthFunc           depthFunc;
	SceGxmDepthWriteMode      depthWrite;
	bool                      stencilEnable;
	SceGxmStencilFunc         sFunc[2];
	SceGxmStencilOp           sFail[2], sDepthFail[2], sPass[2];
	uint8_t                   sCompare[2], sWrite[2];
	uint32_t                  sRef[2];
	bool                      depthBiasEnable;
	int                       depthBiasFactor, depthBiasUnits;
	uint32_t                  dynamicMask;  /* VKV_DYN_* */
	VkViewport                viewport;
	VkRect2D                  scissor;
	bool                      hasViewport, hasScissor;
} VkvPipeline;

enum {
	VKV_DYN_VIEWPORT   = 1u << 0,
	VKV_DYN_SCISSOR    = 1u << 1,
	VKV_DYN_DEPTH_BIAS = 1u << 2,
	VKV_DYN_STENCIL_CMP= 1u << 3,
	VKV_DYN_STENCIL_WR = 1u << 4,
	VKV_DYN_STENCIL_REF= 1u << 5
};

SceGxmVertexProgram *vkv_pipeline_vertex_program(VkvPipeline *p, bool index32);

typedef struct VkPipelineCache_T {
	VkvObject obj;
} VkvPipelineCache;

/* ------------------------------------------------------------------------ */
/* Command buffers                                                          */
/* ------------------------------------------------------------------------ */
typedef enum VkvCmdType {
	VKV_CMD_BEGIN_RP = 1,
	VKV_CMD_END_RP,
	VKV_CMD_BIND_PIPELINE,
	VKV_CMD_BIND_VB,
	VKV_CMD_BIND_IB,
	VKV_CMD_BIND_SETS,
	VKV_CMD_PUSH,
	VKV_CMD_VIEWPORT,
	VKV_CMD_SCISSOR,
	VKV_CMD_DEPTH_BIAS,
	VKV_CMD_STENCIL,
	VKV_CMD_DRAW,
	VKV_CMD_DRAW_INDEXED,
	VKV_CMD_COPY_BUFFER,
	VKV_CMD_COPY_BUF_IMG,
	VKV_CMD_COPY_IMG_BUF,
	VKV_CMD_UPDATE_BUFFER,
	VKV_CMD_FILL_BUFFER,
	VKV_CMD_CLEAR_ATTACH,
	VKV_CMD_CLEAR_COLOR_IMAGE,
	VKV_CMD_BARRIER,
	VKV_CMD_FF_DRAW
} VkvCmdType;

typedef struct VkvCmd {
	uint16_t type;
	uint16_t pad;
	uint32_t size;  /* including header */
} VkvCmd;

typedef struct VkvCmdBeginRP {
	VkvCmd          h;
	VkvRenderPass  *rp;
	VkvFramebuffer *fb;
	VkRect2D        area;
	uint32_t        clearCount;
	VkClearValue    clears[8];
} VkvCmdBeginRP;

typedef struct VkvCmdBindPipeline { VkvCmd h; VkvPipeline *p; } VkvCmdBindPipeline;
typedef struct VkvCmdBindVB { VkvCmd h; uint32_t first, count; VkvBuffer *buf[VKV_MAX_BINDINGS]; uint32_t off[VKV_MAX_BINDINGS]; } VkvCmdBindVB;
typedef struct VkvCmdBindIB { VkvCmd h; VkvBuffer *buf; uint32_t off; VkIndexType type; } VkvCmdBindIB;
typedef struct VkvCmdBindSets {
	VkvCmd h; uint32_t first, count; VkvSet *sets[VKV_MAX_SETS];
	uint32_t dynCount; uint32_t dyn[VKV_MAX_SETS * VKV_MAX_DYNAMIC];
} VkvCmdBindSets;
typedef struct VkvCmdPush { VkvCmd h; uint32_t offset, size; uint8_t data[]; } VkvCmdPush;
typedef struct VkvCmdViewport { VkvCmd h; VkViewport vp; } VkvCmdViewport;
typedef struct VkvCmdScissor { VkvCmd h; VkRect2D rect; } VkvCmdScissor;
typedef struct VkvCmdDepthBias { VkvCmd h; float constant, slope; } VkvCmdDepthBias;
typedef struct VkvCmdStencil { VkvCmd h; uint32_t what; VkStencilFaceFlags faces; uint32_t value; } VkvCmdStencil;
typedef struct VkvCmdDraw { VkvCmd h; uint32_t count, instances, first, firstInstance; int32_t vertexOffset; } VkvCmdDraw;
typedef struct VkvCmdCopyBuffer { VkvCmd h; VkvBuffer *src, *dst; uint32_t n; VkBufferCopy r[]; } VkvCmdCopyBuffer;
typedef struct VkvCmdCopyBufImg { VkvCmd h; VkvBuffer *buf; VkvImage *img; uint32_t n; VkBufferImageCopy r[]; } VkvCmdCopyBufImg;
typedef struct VkvCmdUpdateBuffer { VkvCmd h; VkvBuffer *dst; uint32_t off, size; uint8_t data[]; } VkvCmdUpdateBuffer;
typedef struct VkvCmdFillBuffer { VkvCmd h; VkvBuffer *dst; uint32_t off, size, value; } VkvCmdFillBuffer;
typedef struct VkvCmdClearAttach {
	VkvCmd h; uint32_t na, nr; VkClearAttachment a[8]; VkClearRect r[];
} VkvCmdClearAttach;
typedef struct VkvCmdClearColorImage { VkvCmd h; VkvImage *img; VkClearColorValue c; } VkvCmdClearColorImage;

/* Fixed-function draw (records everything needed, no external state) */
typedef struct VkvCmdFFDraw {
	VkvCmd               h;
	VkvPipeline         *pipe;
	const void          *vertices;
	uint32_t             count;
	SceGxmPrimitiveType  prim;
	const SceGxmProgramParameter *mvpParam, *alphaRefParam;
	float                mvp[16];
	float                alphaRef;
	bool                 textured;
	SceGxmTexture        tex;
} VkvCmdFFDraw;

typedef enum VkvCbState { VKV_CB_INITIAL = 0, VKV_CB_RECORDING, VKV_CB_EXECUTABLE } VkvCbState;

struct VkCommandBuffer_T {
	VkvObject                 obj;
	struct VkDevice_T        *device;
	struct VkCommandPool_T   *pool;
	struct VkCommandBuffer_T *next, *prev;
	uint8_t                  *data;
	uint32_t                  size, cap;
	VkvCbState                state;
	VkCommandBufferUsageFlags usage;
	/* recording-time validation state */
	bool                      inRenderPass;
	VkvPipeline              *pipeline;
	VkvRenderPass            *rp;
	uint32_t                  sceneCount;
	bool                      oom;
};

typedef struct VkCommandPool_T {
	VkvObject                 obj;
	struct VkCommandBuffer_T *buffers;
} VkvCmdPool;

void *vkv_cmd_alloc(struct VkCommandBuffer_T *cb, VkvCmdType type, uint32_t size);

/* ------------------------------------------------------------------------ */
/* Sync and presentation                                                    */
/* ------------------------------------------------------------------------ */
typedef struct VkFence_T {
	VkvObject               obj;
	struct VkDevice_T      *device;
	int32_t                 slot;
	SceGxmNotification      notif;
	bool                    signaled;  /* CPU-side signal */
	bool                    pending;   /* waiting on GPU notification */
} VkvFence;

typedef struct VkSemaphore_T {
	VkvObject obj;
} VkvSemaphore;

typedef struct VkSwapchainKHR_T {
	VkvObject          obj;
	struct VkDevice_T *device;
	uint32_t           imageCount;
	VkvImage          *images[VKV_MAX_SWAP_IMAGES];
	VkvGpuBlock        mem[VKV_MAX_SWAP_IMAGES];
	VkvMemory          memObj[VKV_MAX_SWAP_IMAGES];
	uint32_t           acquireIndex;
	uint32_t           frontIndex;
	VkPresentModeKHR   presentMode;
	VkFormat           format;
	uint32_t           width, height;
} VkvSwapchain;

/* Executes recorded command buffers on the GXM context. */
VkResult vkv_queue_execute(struct VkDevice_T *dev, uint32_t cbCount, const VkCommandBuffer *cbs,
                           VkvFence *fence);
void     vkv_device_wait_gpu(struct VkDevice_T *dev);
int32_t  vkv_notif_alloc(struct VkDevice_T *dev);
void     vkv_notif_free(struct VkDevice_T *dev, int32_t slot);

/* Memory helpers used by swapchain and FF layer */
VkResult vkv_memory_alloc(struct VkDevice_T *dev, uint32_t typeIndex, uint32_t size, uint32_t align,
                          VkvMemory *out);
void     vkv_memory_free(struct VkDevice_T *dev, VkvMemory *mem);
void     vkv_image_setup_layout(VkvImage *img);
VkResult vkv_image_ensure_sync(VkvImage *img);
bool     vkv_view_build_texture(VkvImageView *v);

/* Pipeline creation used by the FF layer */
VkResult vkv_create_pipeline(struct VkDevice_T *dev, const VkGraphicsPipelineCreateInfo *ci, VkvPipeline **out);
void     vkv_destroy_pipeline(struct VkDevice_T *dev, VkvPipeline *p);

/* Memory type indices exposed to the application */
enum { VKV_MEMTYPE_CDRAM = 0, VKV_MEMTYPE_RAM = 1, VKV_MEMTYPE_COUNT = 2 };

/* Proc address table */
PFN_vkVoidFunction vkv_lookup_proc(const char *name);

/* Stats */
void vkv_stats_frame_end(void);

#endif /* VKV_INTERNAL_H */
