/*
 * VkVita: shader modules (GXP binaries or Cg source via vitaShaRK),
 * reflection, attribute/uniform mapping, shader logging and caching.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

#include <stdio.h>
#include <malloc.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#if VKV_WITH_SHARK
#include <vitashark.h>
#endif
#if VKV_WITH_SPIRV
#include "vkv_spirv.h"
#endif

#define VKV_DEFAULT_UNIFORM_CONTAINER 14
#ifndef VKV_SPIRV_MAGIC
#define VKV_SPIRV_MAGIC 0x07230203u
#endif
#define VKV_SPIRV_HASH_SEED 0x53505601u

static uint32_t vkv_fnv1a(const void *data, uint32_t size, uint32_t seed) {
	const uint8_t *p = data;
	uint32_t h = 2166136261u ^ seed;
	for (uint32_t i = 0; i < size; i++) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static char *vkv_strdup(const char *s) {
	if (!s) return NULL;
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d) memcpy(d, s, n);
	return d;
}

static const char *k_stageNames[] = { "vertex", "fragment" };

/* ------------------------------------------------------------------------ */
/* vitaShaRK                                                                */
/* ------------------------------------------------------------------------ */
#if VKV_WITH_SHARK
static const char *s_compilingName = "";

static void vkv_shark_log(const char *msg, shark_log_level level, int line) {
	VkvLogLevel l = level == SHARK_LOG_ERROR ? VKV_LOG_ERROR : level == SHARK_LOG_WARNING ? VKV_LOG_WARN : VKV_LOG_INFO;
	if (l == VKV_LOG_INFO && !vkv_dbg(VKV_DEBUG_LOG_SHADERS)) return;
	vkv_log(l, "cg[%s:%d]: %s", s_compilingName, line, msg);
}
#endif

bool vkv_shark_init(struct VkDevice_T *dev) {
#if VKV_WITH_SHARK
	if (dev->sharkTried) return dev->sharkReady;
	dev->sharkTried = true;
	int r = shark_init(g_vkv.cfg.sharkModulePath);
	if (r < 0) {
		VKV_WARN("runtime shader compiler unavailable (shark_init 0x%08X). Install libshacccg.suprx to %s, or "
		         "use precompiled GXP shaders.", (unsigned)r,
		         g_vkv.cfg.sharkModulePath ? g_vkv.cfg.sharkModulePath : "ur0:data/libshacccg.suprx");
		return false;
	}
	shark_install_log_cb(vkv_shark_log);
	shark_set_warnings_level(vkv_dbg(VKV_DEBUG_LOG_SHADERS) ? SHARK_WARN_MEDIUM : SHARK_WARN_SILENT);
	dev->sharkReady = true;
	VKV_INFO("runtime Cg compiler ready");
	return true;
#else
	(void)dev;
	return false;
#endif
}

void vkv_shark_term(struct VkDevice_T *dev) {
#if VKV_WITH_SHARK
	if (dev->sharkReady) shark_end();
#endif
	dev->sharkReady = false;
	dev->sharkTried = false;
}

static void vkv_shader_path(char *out, size_t cap, uint32_t hash, VkvStage stage) {
	snprintf(out, cap, "%s/%08x_%c.gxp", g_vkv.cfg.shaderDir, (unsigned)hash, stage == VKV_STAGE_VERTEX ? 'v' : 'f');
}

static SceGxmProgram *vkv_load_cached(uint32_t hash, VkvStage stage, uint32_t *outSize) {
	char path[300];
	vkv_shader_path(path, sizeof(path), hash, stage);
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return NULL;
	int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	SceGxmProgram *p = size > 0 ? memalign(16, size) : NULL;
	if (p && sceIoRead(fd, p, size) != size) { free(p); p = NULL; }
	sceIoClose(fd);
	if (p && sceGxmProgramCheck(p) < 0) { free(p); p = NULL; }
	if (p) {
		*outSize = size;
		VKV_INFO("shader cache hit: %s", path);
	}
	return p;
}

#if VKV_WITH_SHARK
static void vkv_dump_shader(uint32_t hash, VkvStage stage, const void *data, uint32_t size, const char *name) {
	char path[300];
	sceIoMkdir("ux0:data/vkvita", 0777);
	sceIoMkdir(g_vkv.cfg.shaderDir, 0777);
	vkv_shader_path(path, sizeof(path), hash, stage);
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) { VKV_WARN("could not write %s", path); return; }
	sceIoWrite(fd, data, size);
	sceIoClose(fd);
	vkv_log(VKV_LOG_INFO, "dumped %s shader '%s' to %s (%u bytes)", k_stageNames[stage], name, path, (unsigned)size);
}
#endif

SceGxmProgram *vkv_shark_compile(struct VkDevice_T *dev, const char *src, VkvStage stage, const char *name,
                                 uint32_t hash, uint32_t *outSize) {
	SceGxmProgram *copy = NULL;
	if (vkv_compat(VKV_COMPAT_SHADER_CACHE)) {
		copy = vkv_load_cached(hash, stage, outSize);
		if (copy) return copy;
	}
#if VKV_WITH_SHARK
	if (!vkv_shark_init(dev)) return NULL;
	uint64_t t0 = vkv_time_us();
	uint32_t size = (uint32_t)strlen(src);
	s_compilingName = name;
	SceGxmProgram *p = shark_compile_shader_extended(src, &size,
		stage == VKV_STAGE_VERTEX ? SHARK_VERTEX_SHADER : SHARK_FRAGMENT_SHADER, SHARK_OPT_DEFAULT, 0, 0, 0);
	if (!p) {
		VKV_ERROR("Cg compile of %s shader '%s' failed (see messages above)", k_stageNames[stage], name);
		shark_clear_output();
		return NULL;
	}
	copy = memalign(16, size);
	if (copy) memcpy(copy, p, size);
	shark_clear_output();
	if (vkv_dbg(VKV_DEBUG_LOG_SHADERS))
		vkv_log(VKV_LOG_INFO, "compiled %s shader '%s' in %.2f ms (%u bytes)", k_stageNames[stage], name,
		        (vkv_time_us() - t0) / 1000.0f, (unsigned)size);
	if (copy && (vkv_dbg(VKV_DEBUG_DUMP_SHADERS) || vkv_compat(VKV_COMPAT_SHADER_CACHE)))
		vkv_dump_shader(hash, stage, copy, size, name);
	*outSize = size;
	return copy;
#else
	(void)dev; (void)src; (void)name;
	VKV_ERROR("shader '%s' is Cg source but VkVita was built without vitaShaRK (VKV_WITH_SHARK=OFF). "
	          "Compile it to GXP or rebuild with vitaShaRK.", name);
	return NULL;
#endif
}

/* ------------------------------------------------------------------------ */
/* Reflection                                                               */
/* ------------------------------------------------------------------------ */
static const char *vkv_cat_name(SceGxmParameterCategory c) {
	switch (c) {
	case SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE:      return "attribute";
	case SCE_GXM_PARAMETER_CATEGORY_UNIFORM:        return "uniform";
	case SCE_GXM_PARAMETER_CATEGORY_SAMPLER:        return "sampler";
	case SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER: return "uniform_buffer";
	default:                                        return "?";
	}
}

void vkv_shader_log_program(const char *name, const SceGxmProgram *p) {
	unsigned n = sceGxmProgramGetParameterCount(p);
	vkv_log(VKV_LOG_INFO, "shader '%s': %s program, %u params, default uniform buffer %u bytes", name,
	        sceGxmProgramGetType(p) == SCE_GXM_VERTEX_PROGRAM ? "vertex" : "fragment", n,
	        sceGxmProgramGetDefaultUniformBufferSize(p));
	for (unsigned i = 0; i < n; i++) {
		const SceGxmProgramParameter *pp = sceGxmProgramGetParameter(p, i);
		vkv_log(VKV_LOG_INFO, "  [%u] %-20s %-14s type=%d comps=%u array=%u semantic=%d.%u res=%u container=%u", i,
		        sceGxmProgramParameterGetName(pp), vkv_cat_name(sceGxmProgramParameterGetCategory(pp)),
		        (int)sceGxmProgramParameterGetType(pp), sceGxmProgramParameterGetComponentCount(pp),
		        sceGxmProgramParameterGetArraySize(pp), (int)sceGxmProgramParameterGetSemantic(pp),
		        sceGxmProgramParameterGetSemanticIndex(pp), sceGxmProgramParameterGetResourceIndex(pp),
		        sceGxmProgramParameterGetContainerIndex(pp));
	}
}

bool vkv_shader_is_system_input(const SceGxmProgramParameter *pp) {
	const char *n = pp ? sceGxmProgramParameterGetName(pp) : NULL;
	return n && (!strcmp(n, "stage_input_vkv_vertex_index") || !strcmp(n, "stage_input_vkv_instance_index"));
}

const SceGxmProgramParameter *vkv_shader_find_attribute(const VkvShaderModule *m, const SceGxmProgram *p,
                                                        uint32_t location) {
	/* 1. explicit name from VkvShaderModuleInfoVITA */
	for (uint32_t i = 0; i < m->attrCount; i++)
		if (m->attrs[i].location == location && m->attrs[i].name) {
			const SceGxmProgramParameter *pp = sceGxmProgramFindParameterByName(p, m->attrs[i].name);
			if (pp && sceGxmProgramParameterGetCategory(pp) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) return pp;
			VKV_WARN("shader '%s': attribute '%s' (location %u) not found", m->name, m->attrs[i].name,
			         (unsigned)location);
			return NULL;
		}
	/* 2. Cg semantic ATTRn */
	const SceGxmProgramParameter *pp = sceGxmProgramFindParameterBySemantic(p, SCE_GXM_PARAMETER_SEMANTIC_ATTR, location);
	if (pp && sceGxmProgramParameterGetCategory(pp) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) return pp;
	/* 3. declaration order */
	unsigned n = sceGxmProgramGetParameterCount(p), k = 0;
	for (unsigned i = 0; i < n; i++) {
		pp = sceGxmProgramGetParameter(p, i);
		if (sceGxmProgramParameterGetCategory(pp) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) continue;
		if (vkv_shader_is_system_input(pp)) continue;
		if (k++ == location) return pp;
	}
	return NULL;
}

uint32_t vkv_build_uniform_map(const VkvShaderModule *m, const SceGxmProgram *p, uint32_t base, VkvUniformMap *out,
                               uint32_t max) {
	unsigned n = sceGxmProgramGetParameterCount(p);
	uint32_t count = 0, cursor = base;
	for (unsigned i = 0; i < n; i++) {
		const SceGxmProgramParameter *pp = sceGxmProgramGetParameter(p, i);
		if (sceGxmProgramParameterGetCategory(pp) != SCE_GXM_PARAMETER_CATEGORY_UNIFORM) continue;
		if (sceGxmProgramParameterGetContainerIndex(pp) != VKV_DEFAULT_UNIFORM_CONTAINER) continue;
		const char *name = sceGxmProgramParameterGetName(pp);
		uint32_t comps = sceGxmProgramParameterGetComponentCount(pp);
		uint32_t arr = sceGxmProgramParameterGetArraySize(pp);
		if (!arr) arr = 1;
		if (!comps) comps = 1;
		uint32_t stride, size, align;
		if (arr > 1) {
			stride = 16 * ((comps + 3) / 4);
			size = stride * arr;
			align = 16;
		} else {
			stride = comps * 4;
			size = stride;
			align = comps == 1 ? 4 : comps == 2 ? 8 : 16;
		}
		int32_t explicitOff = -1, src = -1;
		for (uint32_t u = 0; u < m->uniCount; u++)
			if (m->unis[u].name && !strcmp(m->unis[u].name, name)) {
				explicitOff = (int32_t)m->unis[u].offset;
				src = (int32_t)u;
			}
		uint32_t off = explicitOff >= 0 ? (uint32_t)explicitOff : VKV_ALIGN(cursor, align);
		if (explicitOff < 0) cursor = off + size;
		int8_t set = src >= 0 && m->uniSet ? m->uniSet[src] : -1;

		if (sceGxmProgramParameterGetType(pp) != SCE_GXM_PARAMETER_TYPE_F32) {
			VKV_WARN("shader '%s': uniform '%s' is not float; push constants only feed float uniforms", m->name, name);
			continue;
		}
		if (count >= max) { VKV_WARN("shader '%s': too many default uniforms", m->name); break; }
		out[count].param = pp;
		out[count].offset = off;
		out[count].comps = comps;
		out[count].elems = arr;
		out[count].elemStride = stride;
		out[count].set = set;
		out[count].binding = src >= 0 && m->uniBinding ? m->uniBinding[src] : 0;
		out[count].scalar = src >= 0 && m->uniScalar ? m->uniScalar[src] : 0;
		if (vkv_dbg(VKV_DEBUG_LOG_SHADERS)) {
			if (set < 0)
				vkv_log(VKV_LOG_INFO, "shader '%s': uniform %-16s <- push constant bytes [%u..%u)", m->name, name,
				        (unsigned)off, (unsigned)(off + size));
			else
				vkv_log(VKV_LOG_INFO, "shader '%s': uniform %-16s <- set %d binding %u bytes [%u..%u)", m->name, name,
				        (int)set, (unsigned)out[count].binding, (unsigned)off, (unsigned)(off + size));
		}
		count++;
	}
	return count;
}

/* ------------------------------------------------------------------------ */
/* SPIR-V                                                                   */
/* ------------------------------------------------------------------------ */
static bool vkv_shader_add_uniform(VkvShaderModule *m, const char *name, uint32_t offset, int8_t set, uint8_t binding,
                                   uint8_t scalar) {
	for (uint32_t u = 0; u < m->uniCount; u++)
		if (m->unis[u].name && !strcmp(m->unis[u].name, name)) return true;
	uint32_t n = m->uniCount + 1;
	VkvUniformBindingVITA *unis = realloc(m->unis, n * sizeof(*unis));
	if (unis) m->unis = unis;
	int8_t *sets = realloc(m->uniSet, n);
	if (sets) m->uniSet = sets;
	uint8_t *bindings = realloc(m->uniBinding, n);
	if (bindings) m->uniBinding = bindings;
	uint8_t *scalars = realloc(m->uniScalar, n);
	if (scalars) m->uniScalar = scalars;
	if (!unis || !sets || !bindings || !scalars) return false;
	m->unis[m->uniCount].name = vkv_strdup(name);
	m->unis[m->uniCount].offset = offset;
	m->uniSet[m->uniCount] = set;
	m->uniBinding[m->uniCount] = binding;
	m->uniScalar[m->uniCount] = scalar;
	m->uniCount = n;
	return m->unis[n - 1].name != NULL;
}

#if VKV_WITH_SPIRV
static void vkv_spirv_log(VkvSpirvLogLevel level, const char *msg, void *user) {
	const VkvShaderModule *m = user;
	VkvLogLevel l = level == VKV_SPIRV_LOG_ERROR ? VKV_LOG_ERROR : level == VKV_SPIRV_LOG_WARN ? VKV_LOG_WARN : VKV_LOG_INFO;
	if (l == VKV_LOG_INFO && !vkv_dbg(VKV_DEBUG_LOG_SHADERS)) return;
	vkv_log(l, "spirv[%s]: %s", m->name, msg);
}

static void vkv_log_source(const char *name, const char *src) {
	unsigned line = 1;
	for (const char *p = src; *p; line++) {
		const char *nl = strchr(p, '\n');
		int len = nl ? (int)(nl - p) : (int)strlen(p);
		vkv_log(VKV_LOG_INFO, "cg-src[%s:%u] %.*s", name, line, len, p);
		if (!nl) break;
		p = nl + 1;
	}
}

static SceGxmProgram *vkv_spirv_compile(struct VkDevice_T *dev, VkvShaderModule *m, VkvStage stage, uint32_t *size) {
	VkvSpirvResult r;
	uint64_t t0 = vkv_time_us();
	if (vkv_spirv_translate((const uint32_t *)m->code, m->codeSize / 4,
	                        stage == VKV_STAGE_VERTEX ? VKV_SPIRV_VERTEX : VKV_SPIRV_FRAGMENT, vkv_spirv_log, m, &r) != 0) {
		VKV_ERROR("shader '%s': SPIR-V to Cg translation of the %s stage failed (see messages above)", m->name,
		          k_stageNames[stage]);
		return NULL;
	}
	if (vkv_dbg(VKV_DEBUG_LOG_SHADERS)) {
		vkv_log(VKV_LOG_INFO, "translated %s shader '%s' from SPIR-V in %.2f ms", k_stageNames[stage], m->name,
		        (vkv_time_us() - t0) / 1000.0f);
		vkv_log_source(m->name, r.cg);
	}
	bool ok = true;
	for (uint32_t i = 0; i < r.uniCount && ok; i++)
		ok = vkv_shader_add_uniform(m, r.unis[i].name, r.unis[i].offset, (int8_t)r.unis[i].set,
		                            (uint8_t)r.unis[i].binding, (uint8_t)r.unis[i].scalar);
	if (ok && r.texCount) {
		m->texb[stage] = malloc(r.texCount * sizeof(VkvTexBinding));
		ok = m->texb[stage] != NULL;
		for (uint32_t i = 0; ok && i < r.texCount; i++) {
			m->texb[stage][i].unit = r.texs[i].unit;
			m->texb[stage][i].imageSet = r.texs[i].imageSet;
			m->texb[stage][i].imageBinding = r.texs[i].imageBinding;
			m->texb[stage][i].samplerSet = r.texs[i].samplerSet;
			m->texb[stage][i].samplerBinding = r.texs[i].samplerBinding;
		}
		if (ok) m->texCount[stage] = r.texCount;
	}
	m->texExplicit[stage] = true;
	SceGxmProgram *p = ok ? vkv_shark_compile(dev, r.cg, stage, m->name, m->hash, size) : NULL;
	vkv_spirv_free(&r);
	return p;
}
#endif

/* ------------------------------------------------------------------------ */
/* Module objects                                                           */
/* ------------------------------------------------------------------------ */
VkResult vkv_shader_get_program(struct VkDevice_T *dev, VkvShaderModule *m, VkvStage stage,
                                const SceGxmProgram **prog, SceGxmShaderPatcherId *pid) {
	if (!m->prog[stage]) {
		if (m->source == VKV_SHADER_SOURCE_GXP) {
			SceGxmProgram *p = (SceGxmProgram *)m->code;
			if (sceGxmProgramCheck(p) < 0) {
				VKV_ERROR("shader '%s': invalid GXP binary", m->name);
				return VK_ERROR_INVALID_SHADER_NV;
			}
			SceGxmProgramType want = stage == VKV_STAGE_VERTEX ? SCE_GXM_VERTEX_PROGRAM : SCE_GXM_FRAGMENT_PROGRAM;
			if (sceGxmProgramGetType(p) != want) {
				VKV_ERROR("shader '%s': GXP is a %s program but is used as %s", m->name,
				          sceGxmProgramGetType(p) == SCE_GXM_VERTEX_PROGRAM ? "vertex" : "fragment", k_stageNames[stage]);
				return VK_ERROR_INVALID_SHADER_NV;
			}
			m->prog[stage] = p;
			m->progOwned[stage] = false;
		} else if (m->source == VKV_SHADER_SOURCE_SPIRV) {
#if VKV_WITH_SPIRV
			uint32_t size = 0;
			SceGxmProgram *p = vkv_spirv_compile(dev, m, stage, &size);
			if (!p) return VK_ERROR_INVALID_SHADER_NV;
			m->prog[stage] = p;
			m->progOwned[stage] = true;
#else
			return VK_ERROR_INVALID_SHADER_NV;
#endif
		} else {
			uint32_t size = 0;
			SceGxmProgram *p = vkv_shark_compile(dev, m->code, stage, m->name, m->hash, &size);
			if (!p) return VK_ERROR_INVALID_SHADER_NV;
			m->prog[stage] = p;
			m->progOwned[stage] = true;
		}
		if (vkv_dbg(VKV_DEBUG_LOG_SHADERS)) vkv_shader_log_program(m->name, m->prog[stage]);
	}
	if (!m->registered[stage]) {
		int r = sceGxmShaderPatcherRegisterProgram(dev->patcher, m->prog[stage], &m->pid[stage]);
		if (r < 0) {
			VKV_ERROR("shader '%s': sceGxmShaderPatcherRegisterProgram failed 0x%08X", m->name, (unsigned)r);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		m->registered[stage] = true;
	}
	*prog = m->prog[stage];
	*pid = m->pid[stage];
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                    const VkAllocationCallbacks *alloc, VkShaderModule *pModule) {
	VKV_TRACE("vkCreateShaderModule");
	(void)alloc;
	const VkvShaderModuleInfoVITA *ext = NULL;
	for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
		if (s->sType == VKV_STRUCTURE_TYPE_SHADER_MODULE_INFO_VITA) ext = (const VkvShaderModuleInfoVITA *)s;

	const uint8_t *code = (const uint8_t *)ci->pCode;
	uint32_t size = (uint32_t)ci->codeSize;
	if (VKV_CHECK(code && size, "empty shader code")) return VK_ERROR_INVALID_SHADER_NV;

	VkvShaderSource src = ext ? ext->source : VKV_SHADER_SOURCE_AUTO;
	if (src == VKV_SHADER_SOURCE_AUTO) {
		if (size >= 4 && code[0] == 'G' && code[1] == 'X' && code[2] == 'P' && code[3] == 0) src = VKV_SHADER_SOURCE_GXP;
		else if (size >= 4 && *(const uint32_t *)code == VKV_SPIRV_MAGIC) src = VKV_SHADER_SOURCE_SPIRV;
		else src = VKV_SHADER_SOURCE_CG;
	}
	if (src == VKV_SHADER_SOURCE_SPIRV) {
#if VKV_WITH_SPIRV
		if (VKV_CHECK(size % 4 == 0 && size >= 20, "SPIR-V code size %u is not a multiple of 4", (unsigned)size))
			return VK_ERROR_INVALID_SHADER_NV;
#else
		VKV_ERROR("vkCreateShaderModule: this VkVita was built without SPIR-V support (VKV_WITH_SPIRV=OFF). "
		          "Provide Cg source or a GXP binary (see docs/SHADERS.md).");
		return VK_ERROR_INVALID_SHADER_NV;
#endif
	}

	VkvShaderModule *m = calloc(1, sizeof(*m));
	if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
	m->obj.magic = VKV_MAGIC_SHADER;
	m->device = device;
	m->source = src;
	m->refs = 1; /* the module's own reference, dropped by vkDestroyShaderModule */
	/* GXP blobs need 16-byte alignment; Cg needs a NUL terminator. */
	m->code = memalign(16, size + 1);
	if (!m->code) { free(m); return VK_ERROR_OUT_OF_HOST_MEMORY; }
	memcpy(m->code, code, size);
	m->code[size] = 0;
	m->codeSize = size;
	m->hash = vkv_fnv1a(code, size, src == VKV_SHADER_SOURCE_SPIRV ? VKV_SPIRV_HASH_SEED : 0);

	if (ext && ext->debugName) snprintf(m->name, sizeof(m->name), "%s", ext->debugName);
	else
		snprintf(m->name, sizeof(m->name), "%s_%08x",
		         src == VKV_SHADER_SOURCE_GXP ? "gxp" : src == VKV_SHADER_SOURCE_SPIRV ? "spv" : "cg", (unsigned)m->hash);

	if (ext && ext->attributeBindingCount) {
		m->attrs = calloc(ext->attributeBindingCount, sizeof(*m->attrs));
		m->attrCount = ext->attributeBindingCount;
		for (uint32_t i = 0; i < m->attrCount; i++) {
			m->attrs[i].location = ext->pAttributeBindings[i].location;
			m->attrs[i].name = vkv_strdup(ext->pAttributeBindings[i].name);
		}
	}
	if (ext)
		for (uint32_t i = 0; i < ext->uniformBindingCount; i++)
			vkv_shader_add_uniform(m, ext->pUniformBindings[i].name, ext->pUniformBindings[i].offset, -1, 0, 0);

	if (src == VKV_SHADER_SOURCE_GXP && sceGxmProgramCheck((const SceGxmProgram *)m->code) < 0) {
		VKV_ERROR("vkCreateShaderModule: '%s' has a GXP header but fails sceGxmProgramCheck", m->name);
		free(m->code);
		free(m);
		return VK_ERROR_INVALID_SHADER_NV;
	}
	if (vkv_dbg(VKV_DEBUG_LOG_SHADERS))
		vkv_log(VKV_LOG_INFO, "shader module '%s': %s, %u bytes", m->name,
		        src == VKV_SHADER_SOURCE_GXP ? "GXP binary" : src == VKV_SHADER_SOURCE_SPIRV ? "SPIR-V" : "Cg source",
		        (unsigned)size);
	*pModule = VKV_H(VkShaderModule, m);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device, VkShaderModule module,
                                                 const VkAllocationCallbacks *alloc) {
	(void)alloc;
	VkvShaderModule *m = VKV_ND(VkvShaderModule, module);
	if (!m) return;
	(void)device;
	/* Pipelines keep a reference: the patcher needs the registered program
	 * for as long as patched programs made from it exist. Vulkan allows the
	 * module to be destroyed right after pipeline creation. */
	m->destroyed = true;
	vkv_shader_release(m);
}

void vkv_shader_retain(VkvShaderModule *m) { m->refs++; }

void vkv_shader_release(VkvShaderModule *m) {
	if (m->refs) m->refs--;
	if (m->refs || !m->destroyed) return;
	for (int s = 0; s < VKV_STAGE_COUNT; s++) {
		if (m->registered[s] && sceGxmShaderPatcherUnregisterProgram(m->device->patcher, m->pid[s]) < 0)
			VKV_WARN("shader '%s': unregister failed", m->name);
		if (m->progOwned[s]) free(m->prog[s]);
	}
	for (uint32_t i = 0; i < m->attrCount; i++) free((void *)m->attrs[i].name);
	for (uint32_t i = 0; i < m->uniCount; i++) free((void *)m->unis[i].name);
	free(m->attrs);
	free(m->unis);
	free(m->uniSet);
	free(m->uniBinding);
	free(m->uniScalar);
	for (int s = 0; s < VKV_STAGE_COUNT; s++) free(m->texb[s]);
	free(m->code);
	m->obj.magic = VKV_MAGIC_DEAD;
	free(m);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */
VkResult vkvCreateShaderModuleFromCg(VkDevice device, const char *source, const char *debugName,
                                     VkShaderModule *module) {
	VkvShaderModuleInfoVITA ext = { VKV_STRUCTURE_TYPE_SHADER_MODULE_INFO_VITA, NULL, VKV_SHADER_SOURCE_CG,
	                                debugName, 0, NULL, 0, NULL };
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, &ext, 0, strlen(source),
	                                (const uint32_t *)source };
	return vkCreateShaderModule(device, &ci, NULL, module);
}

VkResult vkvCreateShaderModuleFromFile(VkDevice device, const char *path, VkShaderModule *module) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		VKV_ERROR("cannot open shader file %s (0x%08X)", path, (unsigned)fd);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	char *buf = size > 0 ? memalign(16, size + 1) : NULL;
	if (!buf || sceIoRead(fd, buf, size) != size) {
		sceIoClose(fd);
		free(buf);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	sceIoClose(fd);
	buf[size] = 0;
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	VkvShaderModuleInfoVITA ext = { VKV_STRUCTURE_TYPE_SHADER_MODULE_INFO_VITA, NULL, VKV_SHADER_SOURCE_AUTO, base,
	                                0, NULL, 0, NULL };
	VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, &ext, 0, (size_t)size,
	                                (const uint32_t *)buf };
	VkResult r = vkCreateShaderModule(device, &ci, NULL, module);
	free(buf);
	return r;
}

int vkvShaderCompilerAvailable(VkDevice device) {
	return vkv_shark_init(device) ? 1 : 0;
}
