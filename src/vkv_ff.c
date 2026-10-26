/*
 * VkVita: fixed-function compatibility layer (OpenGL 1.x style
 * immediate mode on top of internal GXM pipelines).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

#include <math.h>

#define FF_MAX_STACK    32
#define FF_MAX_PIPES    64
#define FF_MAX_VERTICES 65535

typedef struct FFVertex {
	float   pos[3];
	float   uv[2];
	uint8_t rgba[4];
} FFVertex; /* 24 bytes, stream 0 */

typedef struct FFPipeKey {
	uint8_t blend, src, dst, depthTest, depthWrite, cull, front, textured, primClass;
} FFPipeKey;

typedef struct FFPipe {
	FFPipeKey                     key;
	VkvPipeline                  *pipe;
	const SceGxmProgramParameter *mvp, *alphaRef;
} FFPipe;

struct VkvFFContext_T {
	VkvObject          obj;
	struct VkDevice_T *dev;
	VkvRenderPass     *rp;
	VkvShaderModule   *vs, *fsColor, *fsTex;
	FFPipe             pipes[FF_MAX_PIPES];
	uint32_t           pipeCount;

	/* streaming vertex memory, one slice per frame */
	VkvGpuBlock        ring;
	uint32_t           sliceSize, slice, sliceOffset, frames;

	/* state */
	uint32_t           caps;
	VkBlendFactor      src, dst;
	float              alphaRef;
	VkCullModeFlags    cullMode;
	VkFrontFace        front;
	bool               hasTexture;
	SceGxmTexture      tex;

	/* matrices */
	VkvFFMatrixMode    mode;
	float              stack[3][FF_MAX_STACK][16];
	uint32_t           depth[3];

	/* immediate mode */
	VkCommandBuffer    cmd;
	VkvFFPrimitive     prim;
	bool               inBegin;
	FFVertex           cur;
	FFVertex          *verts;
	uint32_t           vcount, vcap;
};

/* ------------------------------------------------------------------------ */
/* Built-in Cg shaders                                                      */
/* ------------------------------------------------------------------------ */
static const char k_ffVertexCg[] =
	"void main(float3 aPosition, float2 aTexcoord, float4 aColor,\n"
	"          uniform float4x4 uMvp,\n"
	"          float4 out vPosition : POSITION,\n"
	"          float2 out vTexcoord : TEXCOORD0,\n"
	"          float4 out vColor : COLOR)\n"
	"{\n"
	"    vPosition = mul(float4(aPosition, 1.0f), uMvp);\n"
	"    vTexcoord = aTexcoord;\n"
	"    vColor = aColor;\n"
	"}\n";

static const char k_ffColorCg[] =
	"float4 main(float4 vColor : COLOR, uniform float uAlphaRef) : COLOR\n"
	"{\n"
	"    if (vColor.a < uAlphaRef) discard;\n"
	"    return vColor;\n"
	"}\n";

static const char k_ffTexCg[] =
	"float4 main(float2 vTexcoord : TEXCOORD0, float4 vColor : COLOR,\n"
	"            uniform sampler2D uTex : TEXUNIT0, uniform float uAlphaRef) : COLOR\n"
	"{\n"
	"    float4 c = tex2D(uTex, vTexcoord) * vColor;\n"
	"    if (c.a < uAlphaRef) discard;\n"
	"    return c;\n"
	"}\n";

static const VkvAttributeBindingVITA k_ffAttrs[] = { { 0, "aPosition" }, { 1, "aTexcoord" }, { 2, "aColor" } };

static VkResult ff_module(struct VkDevice_T *dev, const void *gxp, size_t gxpSize, const char *cg, const char *name,
                          bool vertex, VkvShaderModule **out) {
	VkvShaderModuleInfoVITA ext = { VKV_STRUCTURE_TYPE_SHADER_MODULE_INFO_VITA, NULL,
	                                gxp ? VKV_SHADER_SOURCE_GXP : VKV_SHADER_SOURCE_CG, name,
	                                vertex ? 3u : 0u, vertex ? k_ffAttrs : NULL, 0, NULL };
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, &ext, 0,
	                                gxp ? gxpSize : strlen(cg), gxp ? (const uint32_t *)gxp : (const uint32_t *)cg };
	VkShaderModule m;
	VkResult r = vkCreateShaderModule(dev, &ci, NULL, &m);
	*out = r == VK_SUCCESS ? VKV_ND(VkvShaderModule, m) : NULL;
	return r;
}

/* ------------------------------------------------------------------------ */
/* Matrices (column-major)                                                  */
/* ------------------------------------------------------------------------ */
static void m4_identity(float m[16]) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(float out[16], const float a[16], const float b[16]) {
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++)
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
			                 a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
	memcpy(out, r, sizeof(r));
}

static float *ff_top(VkvFFContext ff) { return ff->stack[ff->mode][ff->depth[ff->mode]]; }

static void ff_mult(VkvFFContext ff, const float m[16]) {
	float *t = ff_top(ff);
	m4_mul(t, t, m);
}

/* ------------------------------------------------------------------------ */
/* Context                                                                  */
/* ------------------------------------------------------------------------ */
VkResult vkvffCreateContext(VkDevice device, const VkvFFInitInfo *info, VkvFFContext *out) {
	*out = NULL;
	VkvRenderPass *rp = VKV_ND(VkvRenderPass, info->renderPass);
	if (!rp) {
		VKV_ERROR("vkvffCreateContext: a render pass is required");
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	bool needCompiler = !info->vertexGxp || !info->fragmentColorGxp || !info->fragmentTexGxp;
	if (needCompiler && !vkv_shark_init(device)) {
		VKV_ERROR("fixed-function layer needs libshacccg.suprx (runtime Cg compiler) or precompiled GXP shaders "
		          "in VkvFFInitInfo");
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	VkvFFContext ff = calloc(1, sizeof(*ff));
	if (!ff) return VK_ERROR_OUT_OF_HOST_MEMORY;
	ff->obj.magic = VKV_MAGIC_FF;
	ff->dev = device;
	ff->rp = rp;

	VkResult r = ff_module(device, info->vertexGxp, info->vertexGxpSize, k_ffVertexCg, "ff_vertex", true, &ff->vs);
	if (r == VK_SUCCESS)
		r = ff_module(device, info->fragmentColorGxp, info->fragmentColorGxpSize, k_ffColorCg, "ff_color", false,
		              &ff->fsColor);
	if (r == VK_SUCCESS)
		r = ff_module(device, info->fragmentTexGxp, info->fragmentTexGxpSize, k_ffTexCg, "ff_texture", false, &ff->fsTex);
	if (r != VK_SUCCESS) { vkvffDestroyContext(ff); return r; }

	ff->frames = g_vkv.cfg.ffFrames;
	ff->sliceSize = VKV_ALIGN(g_vkv.cfg.ffVertexBufferSize, 4096);
	if (!vkv_gpu_alloc(VKV_HEAP_RAM, ff->sliceSize * ff->frames, SCE_GXM_MEMORY_ATTRIB_READ, &ff->ring)) {
		vkvffDestroyContext(ff);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	ff->src = VK_BLEND_FACTOR_ONE;
	ff->dst = VK_BLEND_FACTOR_ZERO;
	ff->caps = VKVFF_DEPTH_WRITE;
	ff->alphaRef = 0.0f;
	ff->cullMode = VK_CULL_MODE_BACK_BIT;
	ff->front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	for (int m = 0; m < 3; m++) m4_identity(ff->stack[m][0]);
	ff->cur.rgba[0] = ff->cur.rgba[1] = ff->cur.rgba[2] = ff->cur.rgba[3] = 255;
	VKV_INFO("fixed-function context ready (%u x %u KB streaming slices)", (unsigned)ff->frames,
	         (unsigned)(ff->sliceSize / 1024));
	*out = ff;
	return VK_SUCCESS;
}

void vkvffDestroyContext(VkvFFContext ff) {
	if (!ff) return;
	vkv_device_wait_gpu(ff->dev);
	for (uint32_t i = 0; i < ff->pipeCount; i++) vkv_destroy_pipeline(ff->dev, ff->pipes[i].pipe);
	if (ff->vs) vkDestroyShaderModule(ff->dev, VKV_H(VkShaderModule, ff->vs), NULL);
	if (ff->fsColor) vkDestroyShaderModule(ff->dev, VKV_H(VkShaderModule, ff->fsColor), NULL);
	if (ff->fsTex) vkDestroyShaderModule(ff->dev, VKV_H(VkShaderModule, ff->fsTex), NULL);
	vkv_gpu_free(&ff->ring);
	free(ff->verts);
	ff->obj.magic = VKV_MAGIC_DEAD;
	free(ff);
}

void vkvffBeginFrame(VkvFFContext ff) {
	ff->slice = (ff->slice + 1) % ff->frames;
	ff->sliceOffset = 0;
}

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */
void vkvffEnable(VkvFFContext ff, uint32_t caps) { ff->caps |= caps; }
void vkvffDisable(VkvFFContext ff, uint32_t caps) { ff->caps &= ~caps; }
void vkvffBlendFunc(VkvFFContext ff, VkBlendFactor src, VkBlendFactor dst) { ff->src = src; ff->dst = dst; }
void vkvffAlphaFunc(VkvFFContext ff, float ref) { ff->alphaRef = ref; }
void vkvffCullFace(VkvFFContext ff, VkCullModeFlags mode, VkFrontFace front) { ff->cullMode = mode; ff->front = front; }

void vkvffBindTexture(VkvFFContext ff, VkImageView view, VkSampler sampler) {
	VkvImageView *v = VKV_ND(VkvImageView, view);
	ff->hasTexture = false;
	if (!v) return;
	if (!v->texValid && !vkv_view_build_texture(v)) {
		VKV_WARN("vkvffBindTexture: view cannot be sampled");
		return;
	}
	ff->tex = v->tex;
	vkv_texture_apply_sampler(&ff->tex, VKV_ND(VkvSampler, sampler), v->levelCount);
	ff->hasTexture = true;
}

void vkvffMatrixMode(VkvFFContext ff, VkvFFMatrixMode mode) { if (mode <= VKVFF_TEXTURE) ff->mode = mode; }
void vkvffLoadIdentity(VkvFFContext ff) { m4_identity(ff_top(ff)); }
void vkvffLoadMatrixf(VkvFFContext ff, const float m[16]) { memcpy(ff_top(ff), m, 16 * sizeof(float)); }
void vkvffMultMatrixf(VkvFFContext ff, const float m[16]) { ff_mult(ff, m); }

void vkvffPushMatrix(VkvFFContext ff) {
	uint32_t *d = &ff->depth[ff->mode];
	if (*d + 1 >= FF_MAX_STACK) { if (vkv_validate()) vkv_fail(__func__, "matrix stack overflow"); return; }
	memcpy(ff->stack[ff->mode][*d + 1], ff->stack[ff->mode][*d], 16 * sizeof(float));
	(*d)++;
}

void vkvffPopMatrix(VkvFFContext ff) {
	uint32_t *d = &ff->depth[ff->mode];
	if (!*d) { if (vkv_validate()) vkv_fail(__func__, "matrix stack underflow"); return; }
	(*d)--;
}

void vkvffTranslatef(VkvFFContext ff, float x, float y, float z) {
	float m[16];
	m4_identity(m);
	m[12] = x; m[13] = y; m[14] = z;
	ff_mult(ff, m);
}

void vkvffScalef(VkvFFContext ff, float x, float y, float z) {
	float m[16];
	m4_identity(m);
	m[0] = x; m[5] = y; m[10] = z;
	ff_mult(ff, m);
}

void vkvffRotatef(VkvFFContext ff, float angle, float x, float y, float z) {
	float len = sqrtf(x * x + y * y + z * z);
	if (len <= 0.0f) return;
	x /= len; y /= len; z /= len;
	float a = angle * 3.14159265358979f / 180.0f, c = cosf(a), s = sinf(a), t = 1.0f - c;
	float m[16] = {
		t * x * x + c,     t * x * y + s * z, t * x * z - s * y, 0,
		t * x * y - s * z, t * y * y + c,     t * y * z + s * x, 0,
		t * x * z + s * y, t * y * z - s * x, t * z * z + c,     0,
		0, 0, 0, 1
	};
	ff_mult(ff, m);
}

void vkvffOrtho(VkvFFContext ff, float l, float r, float b, float t, float n, float f) {
	/* OpenGL convention (y up, z in [-1,1]) remapped to Vulkan clip space
	 * (y down, z in [0,1]) so it works without VKV_COMPAT_GL_CLIP_SPACE. */
	float m[16];
	m4_identity(m);
	m[0] = 2.0f / (r - l);
	m[5] = -2.0f / (t - b);
	m[10] = -1.0f / (f - n);
	m[12] = -(r + l) / (r - l);
	m[13] = (t + b) / (t - b);
	m[14] = -n / (f - n);
	if (vkv_compat(VKV_COMPAT_GL_CLIP_SPACE)) {
		m[5] = 2.0f / (t - b); m[13] = -(t + b) / (t - b);
		m[10] = -2.0f / (f - n); m[14] = -(f + n) / (f - n);
	}
	ff_mult(ff, m);
}

void vkvffFrustum(VkvFFContext ff, float l, float r, float b, float t, float n, float f) {
	float m[16];
	memset(m, 0, sizeof(m));
	m[0] = 2.0f * n / (r - l);
	m[5] = -2.0f * n / (t - b);
	m[8] = (r + l) / (r - l);
	m[9] = -(t + b) / (t - b);
	m[10] = -f / (f - n);
	m[11] = -1.0f;
	m[14] = -f * n / (f - n);
	if (vkv_compat(VKV_COMPAT_GL_CLIP_SPACE)) {
		m[5] = 2.0f * n / (t - b); m[9] = (t + b) / (t - b);
		m[10] = -(f + n) / (f - n); m[14] = -2.0f * f * n / (f - n);
	}
	ff_mult(ff, m);
}

void vkvffPerspective(VkvFFContext ff, float fovy, float aspect, float n, float f) {
	float t = n * tanf(fovy * 3.14159265358979f / 360.0f);
	vkvffFrustum(ff, -t * aspect, t * aspect, -t, t, n, f);
}

/* ------------------------------------------------------------------------ */
/* Pipelines                                                                */
/* ------------------------------------------------------------------------ */
static FFPipe *ff_get_pipe(VkvFFContext ff, uint8_t primClass) {
	FFPipeKey k;
	memset(&k, 0, sizeof(k));
	k.blend = (ff->caps & VKVFF_BLEND) != 0;
	k.src = k.blend ? (uint8_t)ff->src : 0;
	k.dst = k.blend ? (uint8_t)ff->dst : 0;
	k.depthTest = (ff->caps & VKVFF_DEPTH_TEST) != 0;
	k.depthWrite = k.depthTest && (ff->caps & VKVFF_DEPTH_WRITE);
	k.cull = (ff->caps & VKVFF_CULL_FACE) ? (uint8_t)ff->cullMode : 0;
	k.front = (uint8_t)ff->front;
	k.textured = (ff->caps & VKVFF_TEXTURE_2D) && ff->hasTexture;
	k.primClass = primClass;
	for (uint32_t i = 0; i < ff->pipeCount; i++)
		if (!memcmp(&ff->pipes[i].key, &k, sizeof(k))) return &ff->pipes[i];
	if (ff->pipeCount == FF_MAX_PIPES) {
		VKV_WARN("fixed-function: too many state combinations, reusing the first pipeline");
		return &ff->pipes[0];
	}

	static const VkPrimitiveTopology topo[3] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkShaderModule vs = VKV_H(VkShaderModule, ff->vs);
	VkShaderModule fs = VKV_H(VkShaderModule, k.textured ? ff->fsTex : ff->fsColor);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
	};
	VkVertexInputBindingDescription bind = { 0, sizeof(FFVertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attrs[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FFVertex, pos) },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(FFVertex, uv) },
		{ 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(FFVertex, rgba) },
	};
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
	                                            1, &bind, 3, attrs };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
	                                              topo[primClass], VK_FALSE };
	VkPipelineRasterizationStateCreateInfo rs;
	memset(&rs, 0, sizeof(rs));
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = k.cull;
	rs.frontFace = (VkFrontFace)k.front;
	rs.lineWidth = 1.0f;
	VkPipelineDepthStencilStateCreateInfo ds;
	memset(&ds, 0, sizeof(ds));
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = k.depthTest;
	ds.depthWriteEnable = k.depthWrite;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendAttachmentState ba;
	memset(&ba, 0, sizeof(ba));
	ba.blendEnable = k.blend;
	ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = (VkBlendFactor)k.src;
	ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = (VkBlendFactor)k.dst;
	ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
	ba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
	                                           VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba, { 0, 0, 0, 0 } };
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
	VkGraphicsPipelineCreateInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	ci.stageCount = 2;
	ci.pStages = stages;
	ci.pVertexInputState = &vi;
	ci.pInputAssemblyState = &ia;
	ci.pRasterizationState = &rs;
	ci.pDepthStencilState = &ds;
	ci.pColorBlendState = &cb;
	ci.pDynamicState = &dsi;
	ci.renderPass = VKV_H(VkRenderPass, ff->rp);

	FFPipe *fp = &ff->pipes[ff->pipeCount];
	if (vkv_create_pipeline(ff->dev, &ci, &fp->pipe) != VK_SUCCESS) return NULL;
	fp->key = k;
	fp->mvp = sceGxmProgramFindParameterByName(fp->pipe->vprog, "uMvp");
	fp->alphaRef = sceGxmProgramFindParameterByName(fp->pipe->fprog, "uAlphaRef");
	ff->pipeCount++;
	return fp;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                  */
/* ------------------------------------------------------------------------ */
static void ff_emit(VkvFFContext ff, VkCommandBuffer cmd, VkvFFPrimitive prim, const FFVertex *v, uint32_t n) {
	if (!n) return;
	if (vkv_validate() && !cmd->inRenderPass) {
		vkv_fail("vkvffEnd", "fixed-function draw outside a render pass");
		return;
	}
	uint8_t primClass = prim == VKVFF_POINTS ? 0 : prim == VKVFF_LINES ? 1 : 2;
	SceGxmPrimitiveType gp = SCE_GXM_PRIMITIVE_TRIANGLES;
	switch (prim) {
	case VKVFF_POINTS:         gp = SCE_GXM_PRIMITIVE_POINTS; break;
	case VKVFF_LINES:          gp = SCE_GXM_PRIMITIVE_LINES; n -= n % 2; break;
	case VKVFF_TRIANGLE_STRIP: gp = SCE_GXM_PRIMITIVE_TRIANGLE_STRIP; break;
	case VKVFF_TRIANGLE_FAN:   gp = SCE_GXM_PRIMITIVE_TRIANGLE_FAN; break;
	default:                   gp = SCE_GXM_PRIMITIVE_TRIANGLES; n -= n % 3; break;
	}
	if (!n) return;
	if (n > FF_MAX_VERTICES) {
		VKV_WARN("fixed-function: draw of %u vertices truncated to %u", (unsigned)n, FF_MAX_VERTICES);
		n = FF_MAX_VERTICES;
	}
	uint32_t bytes = n * sizeof(FFVertex);
	if (ff->sliceOffset + bytes > ff->sliceSize) {
		VKV_WARN("fixed-function: streaming buffer full (%u KB per frame); raise ffVertexBufferSize",
		         (unsigned)(ff->sliceSize / 1024));
		return;
	}
	FFPipe *fp = ff_get_pipe(ff, primClass);
	if (!fp) return;
	uint8_t *dst = (uint8_t *)ff->ring.base + ff->slice * ff->sliceSize + ff->sliceOffset;
	memcpy(dst, v, bytes);
	ff->sliceOffset += VKV_ALIGN(bytes, 16);

	VkvCmdFFDraw *c = vkv_cmd_alloc(cmd, VKV_CMD_FF_DRAW, sizeof(VkvCmdFFDraw));
	if (!c) return;
	c->pipe = fp->pipe;
	c->vertices = dst;
	c->count = n;
	c->prim = gp;
	c->mvpParam = fp->mvp;
	c->alphaRefParam = fp->alphaRef;
	m4_mul(c->mvp, ff->stack[VKVFF_PROJECTION][ff->depth[VKVFF_PROJECTION]],
	       ff->stack[VKVFF_MODELVIEW][ff->depth[VKVFF_MODELVIEW]]);
	c->alphaRef = (ff->caps & VKVFF_ALPHA_TEST) ? ff->alphaRef : -1.0f;
	c->textured = fp->key.textured;
	if (c->textured) c->tex = ff->tex;
}

static bool ff_texmat_identity(VkvFFContext ff, const float **m) {
	*m = ff->stack[VKVFF_TEXTURE][ff->depth[VKVFF_TEXTURE]];
	static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	return !memcmp(*m, id, sizeof(id));
}

static void ff_apply_texmat(VkvFFContext ff, FFVertex *v, uint32_t n) {
	const float *m;
	if (ff_texmat_identity(ff, &m)) return;
	for (uint32_t i = 0; i < n; i++) {
		float u = v[i].uv[0], t = v[i].uv[1];
		v[i].uv[0] = m[0] * u + m[4] * t + m[12];
		v[i].uv[1] = m[1] * u + m[5] * t + m[13];
	}
}

static bool ff_reserve(VkvFFContext ff, uint32_t n) {
	if (n <= ff->vcap) return true;
	uint32_t cap = ff->vcap ? ff->vcap * 2 : 256;
	while (cap < n) cap *= 2;
	FFVertex *nv = realloc(ff->verts, cap * sizeof(FFVertex));
	if (!nv) return false;
	ff->verts = nv;
	ff->vcap = cap;
	return true;
}

void vkvffBegin(VkvFFContext ff, VkCommandBuffer cmd, VkvFFPrimitive prim) {
	if (vkv_validate() && ff->inBegin) vkv_fail(__func__, "vkvffBegin called twice without vkvffEnd");
	ff->cmd = cmd;
	ff->prim = prim;
	ff->inBegin = true;
	ff->vcount = 0;
}

void vkvffColor4f(VkvFFContext ff, float r, float g, float b, float a) {
	ff->cur.rgba[0] = (uint8_t)(VKV_MIN(VKV_MAX(r, 0.0f), 1.0f) * 255.0f + 0.5f);
	ff->cur.rgba[1] = (uint8_t)(VKV_MIN(VKV_MAX(g, 0.0f), 1.0f) * 255.0f + 0.5f);
	ff->cur.rgba[2] = (uint8_t)(VKV_MIN(VKV_MAX(b, 0.0f), 1.0f) * 255.0f + 0.5f);
	ff->cur.rgba[3] = (uint8_t)(VKV_MIN(VKV_MAX(a, 0.0f), 1.0f) * 255.0f + 0.5f);
}

void vkvffColor4ub(VkvFFContext ff, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	ff->cur.rgba[0] = r; ff->cur.rgba[1] = g; ff->cur.rgba[2] = b; ff->cur.rgba[3] = a;
}

void vkvffTexCoord2f(VkvFFContext ff, float u, float v) { ff->cur.uv[0] = u; ff->cur.uv[1] = v; }

void vkvffVertex3f(VkvFFContext ff, float x, float y, float z) {
	if (!ff->inBegin) { if (vkv_validate()) vkv_fail(__func__, "vertex outside vkvffBegin/End"); return; }
	if (!ff_reserve(ff, ff->vcount + 1)) return;
	FFVertex *v = &ff->verts[ff->vcount++];
	*v = ff->cur;
	v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
}

void vkvffVertex2f(VkvFFContext ff, float x, float y) { vkvffVertex3f(ff, x, y, 0.0f); }

/* QUADS -> TRIANGLES (0,1,2  0,2,3), in place into a new buffer */
static FFVertex *ff_quads_to_tris(const FFVertex *q, uint32_t n, uint32_t *outN) {
	uint32_t quads = n / 4;
	FFVertex *t = malloc(quads * 6 * sizeof(FFVertex));
	if (!t) { *outN = 0; return NULL; }
	for (uint32_t i = 0; i < quads; i++) {
		const FFVertex *s = q + i * 4;
		FFVertex *d = t + i * 6;
		d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
		d[3] = s[0]; d[4] = s[2]; d[5] = s[3];
	}
	*outN = quads * 6;
	return t;
}

static void ff_submit(VkvFFContext ff, VkCommandBuffer cmd, VkvFFPrimitive prim, FFVertex *v, uint32_t n) {
	ff_apply_texmat(ff, v, n);
	if (prim == VKVFF_QUADS) {
		uint32_t tn;
		FFVertex *t = ff_quads_to_tris(v, n, &tn);
		ff_emit(ff, cmd, VKVFF_TRIANGLES, t, tn);
		free(t);
	} else {
		ff_emit(ff, cmd, prim, v, n);
	}
}

void vkvffEnd(VkvFFContext ff) {
	if (!ff->inBegin) { if (vkv_validate()) vkv_fail(__func__, "vkvffEnd without vkvffBegin"); return; }
	ff->inBegin = false;
	ff_submit(ff, ff->cmd, ff->prim, ff->verts, ff->vcount);
	ff->vcount = 0;
}

void vkvffDrawArrays(VkvFFContext ff, VkCommandBuffer cmd, VkvFFPrimitive prim, uint32_t count, const float *xyz,
                     const float *uv, const uint8_t *rgba) {
	if (!count || !xyz || !ff_reserve(ff, count)) return;
	for (uint32_t i = 0; i < count; i++) {
		FFVertex *v = &ff->verts[i];
		v->pos[0] = xyz[i * 3]; v->pos[1] = xyz[i * 3 + 1]; v->pos[2] = xyz[i * 3 + 2];
		if (uv) { v->uv[0] = uv[i * 2]; v->uv[1] = uv[i * 2 + 1]; }
		else { v->uv[0] = ff->cur.uv[0]; v->uv[1] = ff->cur.uv[1]; }
		if (rgba) memcpy(v->rgba, rgba + i * 4, 4);
		else memcpy(v->rgba, ff->cur.rgba, 4);
	}
	ff_submit(ff, cmd, prim, ff->verts, count);
}
