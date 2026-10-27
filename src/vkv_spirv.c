/*
 * VkVita: SPIR-V to Cg translation on top of SPIRV-Cross (HLSL backend,
 * shader model 3.0), rewritten into the Cg dialect GXM's compiler accepts.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_spirv.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spirv_cross_c.h>

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */
typedef struct Sb {
	char  *p;
	size_t n, cap;
	bool   oom;
} Sb;

static void sb_add(Sb *b, const char *s, size_t n) {
	if (b->oom) return;
	if (b->n + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 4096;
		while (cap < b->n + n + 1) cap *= 2;
		char *p = realloc(b->p, cap);
		if (!p) { b->oom = true; return; }
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = 0;
}

static void sb_puts(Sb *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_printf(Sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(Sb *b, const char *fmt, ...) {
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if ((size_t)n < sizeof(tmp)) { sb_add(b, tmp, (size_t)n); return; }
	char *big = malloc((size_t)n + 1);
	if (!big) { b->oom = true; return; }
	va_start(ap, fmt);
	vsnprintf(big, (size_t)n + 1, fmt, ap);
	va_end(ap);
	sb_add(b, big, (size_t)n);
	free(big);
}

static char *dupn(const char *s, size_t n) {
	char *d = malloc(n + 1);
	if (!d) return NULL;
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

static const char *skip_ws(const char *s) {
	while (*s == ' ' || *s == '\t') s++;
	return s;
}

static bool starts_with(const char *s, const char *prefix) { return !strncmp(s, prefix, strlen(prefix)); }

static bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

typedef struct Ctx {
	VkvSpirvLogFn log;
	void         *user;
	bool          failed;
} Ctx;

static void logf_(Ctx *c, VkvSpirvLogLevel lvl, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void logf_(Ctx *c, VkvSpirvLogLevel lvl, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (lvl == VKV_SPIRV_LOG_ERROR) c->failed = true;
	if (c->log) c->log(lvl, msg, c->user);
}

static void spvc_error_cb(void *user, const char *error) {
	logf_((Ctx *)user, VKV_SPIRV_LOG_ERROR, "SPIRV-Cross: %s", error);
}

/* ------------------------------------------------------------------------ */
/* Reflection                                                               */
/* ------------------------------------------------------------------------ */
typedef struct Block {
	char         name[128];
	int32_t      set;       /* -1 = push constants */
	uint32_t     binding;
	spvc_type_id type;      /* the block struct */
	uint32_t     cursor;    /* next member while rewriting the cbuffer */
} Block;

typedef struct Sampler {
	char            name[128];
	VkvSpirvTexture t;
	bool            fixedUnit;  /* combined image sampler: unit = binding if free */
	char            array[128]; /* element of this sampler array, "" when not an array */
	uint32_t        arraySize;
} Sampler;

#define MAX_BLOCKS   16
#define MAX_SAMPLERS 16
#define MAX_IO       32

typedef struct Io {
	char type[32];
	char name[128];
	char sem[32];
	bool isInt;        /* integer varying, carried as float */
	char floatType[32];
	bool frontFacing;
} Io;

typedef struct Tr {
	Ctx      *c;
	VkvSpirvStage stage;
	Block     blocks[MAX_BLOCKS];
	uint32_t  blockCount;
	Sampler   samplers[MAX_SAMPLERS];
	uint32_t  samplerCount;
	Io        in[MAX_IO], out[MAX_IO];
	uint32_t  inCount, outCount;
	VkvSpirvUniform *unis;
	uint32_t  uniCount, uniCap;
	Sb        intInit;     /* lines run at the start of main: integer and struct uniforms */
	spvc_compiler comp;
	bool      pointSize;   /* the vertex shader writes gl_PointSize */
} Tr;

static const char *k_unsupportedRes[] = {
	"storage buffers", "storage images", "subpass inputs", "acceleration structures",
};

static void block_name(spvc_compiler comp, spvc_type_id base, char *out, size_t cap) {
	const char *n = spvc_compiler_get_name(comp, base);
	if (n && *n) snprintf(out, cap, "%s", n);
	else snprintf(out, cap, "_%u", (unsigned)base);
}

/* Array size of a resource variable, 1 when it is not an array, 0 when the
 * array has no fixed size or more than one dimension. */
static unsigned resource_array_size(spvc_compiler comp, spvc_resources res, spvc_resource_type kind, spvc_variable_id id) {
	const spvc_reflected_resource *list;
	size_t n;
	if (spvc_resources_get_resource_list_for_type(res, kind, &list, &n) != SPVC_SUCCESS) return 1;
	for (size_t i = 0; i < n; i++) {
		if (list[i].id != id) continue;
		spvc_type type = spvc_compiler_get_type_handle(comp, list[i].type_id);
		unsigned dims = spvc_type_get_num_array_dimensions(type);
		if (!dims) return 1;
		if (dims > 1 || !spvc_type_array_dimension_is_literal(type, 0)) return 0;
		return spvc_type_get_array_dimension(type, 0);
	}
	return 1;
}

static Sampler *new_sampler(Tr *t) {
	if (t->samplerCount >= MAX_SAMPLERS) {
		logf_(t->c, VKV_SPIRV_LOG_ERROR, "more than %d textures in one stage", MAX_SAMPLERS);
		return NULL;
	}
	Sampler *s = &t->samplers[t->samplerCount++];
	memset(s, 0, sizeof(*s));
	return s;
}

static bool reflect(Tr *t, spvc_compiler comp, spvc_resources res) {
	const spvc_reflected_resource *list;
	size_t n;
	static const spvc_resource_type bad[] = {
		SPVC_RESOURCE_TYPE_STORAGE_BUFFER, SPVC_RESOURCE_TYPE_STORAGE_IMAGE, SPVC_RESOURCE_TYPE_SUBPASS_INPUT,
		SPVC_RESOURCE_TYPE_ACCELERATION_STRUCTURE,
	};
	for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		if (spvc_resources_get_resource_list_for_type(res, bad[k], &list, &n) != SPVC_SUCCESS || !n) continue;
		logf_(t->c, VKV_SPIRV_LOG_ERROR, "%s are not supported on GXM (first one: '%s')", k_unsupportedRes[k],
		      list[0].name);
	}

	/* separate images and samplers become combined samplers named vkv_tex<n> */
	spvc_variable_id dummy = 0;
	spvc_compiler_build_dummy_sampler_for_combined_images(comp, &dummy);
	const spvc_combined_image_sampler *combos = NULL;
	size_t comboCount = 0;
	if (spvc_compiler_build_combined_image_samplers(comp) == SPVC_SUCCESS)
		spvc_compiler_get_combined_image_samplers(comp, &combos, &comboCount);
	for (size_t i = 0; i < comboCount; i++) {
		char name[32];
		snprintf(name, sizeof(name), "vkv_tex%u", (unsigned)i);
		spvc_compiler_set_name(comp, combos[i].combined_id, name);
		unsigned images = resource_array_size(comp, res, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, combos[i].image_id);
		unsigned samplers = dummy && combos[i].sampler_id == dummy
		                    ? 1 : resource_array_size(comp, res, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS, combos[i].sampler_id);
		if (!images || !samplers) {
			logf_(t->c, VKV_SPIRV_LOG_ERROR, "texture arrays need one fixed size ('%s')", name);
			continue;
		}
		unsigned count = images > samplers ? images : samplers;
		for (unsigned k = 0; k < count; k++) {
			Sampler *s = new_sampler(t);
			if (!s) break;
			if (count > 1) {
				snprintf(s->name, sizeof(s->name), "%s_%u", name, k);
				snprintf(s->array, sizeof(s->array), "%s", name);
				s->arraySize = count;
			} else {
				snprintf(s->name, sizeof(s->name), "%s", name);
			}
			s->t.imageSet = (int32_t)spvc_compiler_get_decoration(comp, combos[i].image_id, SpvDecorationDescriptorSet);
			s->t.imageBinding = spvc_compiler_get_decoration(comp, combos[i].image_id, SpvDecorationBinding);
			s->t.imageElement = images > 1 ? k : 0;
			if (dummy && combos[i].sampler_id == dummy) {
				s->t.samplerSet = -2;
			} else {
				s->t.samplerSet = (int32_t)spvc_compiler_get_decoration(comp, combos[i].sampler_id,
				                                                        SpvDecorationDescriptorSet);
				s->t.samplerBinding = spvc_compiler_get_decoration(comp, combos[i].sampler_id, SpvDecorationBinding);
				s->t.samplerElement = samplers > 1 ? k : 0;
			}
		}
	}

	if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &n) == SPVC_SUCCESS) {
		for (size_t i = 0; i < n; i++) {
			unsigned count = resource_array_size(comp, res, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, list[i].id);
			if (!count) {
				logf_(t->c, VKV_SPIRV_LOG_ERROR, "sampler arrays need one fixed size ('%s')", list[i].name);
				continue;
			}
			for (unsigned k = 0; k < count; k++) {
				Sampler *s = new_sampler(t);
				if (!s) break;
				if (count > 1) {
					snprintf(s->name, sizeof(s->name), "%.100s_%u", list[i].name, k);
					snprintf(s->array, sizeof(s->array), "%s", list[i].name);
					s->arraySize = count;
				} else {
					snprintf(s->name, sizeof(s->name), "%s", list[i].name);
				}
				s->t.imageSet = (int32_t)spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationDescriptorSet);
				s->t.imageBinding = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding);
				s->t.imageElement = k;
				s->t.samplerSet = -1;
				/* the first element keeps its binding as texture unit where it can */
				s->fixedUnit = k == 0;
			}
		}
	}

	/* texture units: combined samplers keep their binding where possible */
	uint32_t used = 0;
	for (uint32_t i = 0; i < t->samplerCount; i++) {
		Sampler *s = &t->samplers[i];
		if (s->fixedUnit && s->t.imageBinding < 16 && !(used & (1u << s->t.imageBinding))) {
			s->t.unit = s->t.imageBinding;
			used |= 1u << s->t.unit;
		} else {
			s->t.unit = 0xFFFFFFFFu;
		}
	}
	for (uint32_t i = 0; i < t->samplerCount; i++) {
		Sampler *s = &t->samplers[i];
		if (s->t.unit != 0xFFFFFFFFu) continue;
		uint32_t u = 0;
		while (u < 16 && (used & (1u << u))) u++;
		if (u == 16) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "more than 16 textures in one stage"); break; }
		s->t.unit = u;
		used |= 1u << u;
	}

	static const spvc_resource_type blocks[] = { SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, SPVC_RESOURCE_TYPE_PUSH_CONSTANT };
	for (int k = 0; k < 2; k++) {
		if (spvc_resources_get_resource_list_for_type(res, blocks[k], &list, &n) != SPVC_SUCCESS) continue;
		for (size_t i = 0; i < n; i++) {
			if (t->blockCount >= MAX_BLOCKS) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "too many uniform blocks"); break; }
			Block *b = &t->blocks[t->blockCount++];
			block_name(comp, list[i].base_type_id, b->name, sizeof(b->name));
			b->type = list[i].base_type_id;
			b->cursor = 0;
			if (k == 1) {
				b->set = -1;
				b->binding = 0;
			} else {
				b->set = (int32_t)spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationDescriptorSet);
				b->binding = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding);
			}
		}
	}

	if (t->stage == VKV_SPIRV_VERTEX &&
	    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &n) == SPVC_SUCCESS) {
		for (size_t i = 0; i < n; i++) {
			char sem[16];
			spvc_hlsl_vertex_attribute_remap remap;
			remap.location = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationLocation);
			const char *nm = spvc_compiler_get_name(comp, list[i].id);
			if (nm && !strcmp(nm, "vkv_vertex_index")) snprintf(sem, sizeof(sem), "INDEX");
			else if (nm && !strcmp(nm, "vkv_instance_index")) snprintf(sem, sizeof(sem), "INSTANCE");
			else snprintf(sem, sizeof(sem), "ATTR%u", remap.location);
			remap.semantic = sem;
			if (spvc_compiler_hlsl_add_vertex_attribute_remap(comp, &remap, 1) != SPVC_SUCCESS)
				logf_(t->c, VKV_SPIRV_LOG_ERROR, "could not remap vertex input '%s'", list[i].name);
		}
	}
	return !t->c->failed;
}

/* ------------------------------------------------------------------------ */
/* Rewriting                                                                */
/* ------------------------------------------------------------------------ */
static Block *find_block(Tr *t, const char *name) {
	for (uint32_t i = 0; i < t->blockCount; i++)
		if (!strcmp(t->blocks[i].name, name)) return &t->blocks[i];
	return NULL;
}

static const Sampler *find_sampler(const Tr *t, const char *name) {
	for (uint32_t i = 0; i < t->samplerCount; i++)
		if (!strcmp(t->samplers[i].name, name)) return &t->samplers[i];
	return NULL;
}

static bool add_uniform(Tr *t, const char *name, uint32_t offset, const Block *b, VkvSpirvScalar scalar) {
	if (t->uniCount == t->uniCap) {
		uint32_t cap = t->uniCap ? t->uniCap * 2 : 16;
		VkvSpirvUniform *u = realloc(t->unis, cap * sizeof(*u));
		if (!u) return false;
		t->unis = u;
		t->uniCap = cap;
	}
	VkvSpirvUniform *u = &t->unis[t->uniCount++];
	u->name = dupn(name, strlen(name));
	u->offset = offset;
	u->set = b->set;
	u->binding = b->binding;
	u->scalar = scalar;
	return u->name != NULL;
}

/* "float4", "int3", "uint", "bool2" -> scalar kind and vector width suffix */
static VkvSpirvScalar scalar_kind(const char *type, const char **suffix) {
	if (starts_with(type, "float")) { *suffix = type + 5; return VKV_SPIRV_FLOAT; }
	if (starts_with(type, "half")) { *suffix = type + 4; return VKV_SPIRV_FLOAT; }
	if (starts_with(type, "uint")) { *suffix = type + 4; return VKV_SPIRV_UINT; }
	if (starts_with(type, "int")) { *suffix = type + 3; return VKV_SPIRV_INT; }
	if (starts_with(type, "bool")) { *suffix = type + 4; return VKV_SPIRV_UINT; }
	*suffix = NULL;
	return VKV_SPIRV_FLOAT;
}

/* Struct members of uniform blocks are flattened: every scalar, vector and
 * matrix inside becomes its own uniform, and main starts by copying them into
 * a static copy of the struct, so the rest of the shader is unchanged. */
#define MAX_FLAT_LEAVES 256

static bool flatten_leaf(Tr *t, Sb *o, spvc_type type, uint32_t offset, const char *path, const char *mangled,
                         const Block *b, uint32_t *leaves) {
	spvc_basetype bt = spvc_type_get_basetype(type);
	unsigned vec = spvc_type_get_vector_size(type), cols = spvc_type_get_columns(type);
	const char *base = bt == SPVC_BASETYPE_FP32 || bt == SPVC_BASETYPE_FP16 ? "float"
	                 : bt == SPVC_BASETYPE_INT32 ? "int" : bt == SPVC_BASETYPE_UINT32 ? "uint"
	                 : bt == SPVC_BASETYPE_BOOLEAN ? "bool" : NULL;
	if (!base) {
		logf_(t->c, VKV_SPIRV_LOG_WARN, "'%s' in block '%s' has a type that cannot be fed", path, b->name);
		return true;
	}
	if (cols > 1 && cols != vec) {
		logf_(t->c, VKV_SPIRV_LOG_WARN, "non-square matrix '%s' in block '%s' is declared but not fed", path, b->name);
		return true;
	}
	if (cols > 1 && strcmp(base, "float")) {
		logf_(t->c, VKV_SPIRV_LOG_WARN, "integer matrix '%s' in block '%s' is declared but not fed", path, b->name);
		return true;
	}
	if (++*leaves > MAX_FLAT_LEAVES) {
		logf_(t->c, VKV_SPIRV_LOG_ERROR, "block '%s' has more than %d values inside structs", b->name, MAX_FLAT_LEAVES);
		return false;
	}
	char suffix[24] = "";
	if (cols > 1) snprintf(suffix, sizeof(suffix), "%ux%u", cols, vec);
	else if (vec > 1) snprintf(suffix, sizeof(suffix), "%u", vec);
	sb_printf(o, "uniform float%s %s;\n", suffix, mangled);
	if (!strcmp(base, "float")) sb_printf(&t->intInit, "    %s = %s;\n", path, mangled);
	else if (!strcmp(base, "bool")) sb_printf(&t->intInit, "    %s = %s != 0.0f;\n", path, mangled);
	else sb_printf(&t->intInit, "    %s = %s%s(%s);\n", path, base, suffix, mangled);
	VkvSpirvScalar kind = !strcmp(base, "int") ? VKV_SPIRV_INT : !strcmp(base, "float") ? VKV_SPIRV_FLOAT : VKV_SPIRV_UINT;
	return add_uniform(t, mangled, offset, b, kind);
}

static bool flatten(Tr *t, Sb *o, spvc_type_id id, uint32_t offset, uint32_t arrayStride, const char *path,
                    const char *mangled, const Block *b, uint32_t *leaves, int depth) {
	spvc_type type = spvc_compiler_get_type_handle(t->comp, id);
	if (depth > 8) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "structs nested too deep in block '%s'", b->name); return false; }
	unsigned dims = spvc_type_get_num_array_dimensions(type);
	if (dims > 1) {
		logf_(t->c, VKV_SPIRV_LOG_WARN, "multidimensional array '%s' in block '%s' is declared but not fed", path,
		      b->name);
		return true;
	}
	if (dims == 1) {
		if (!spvc_type_array_dimension_is_literal(type, 0)) {
			logf_(t->c, VKV_SPIRV_LOG_WARN, "array '%s' in block '%s' has no fixed size, not fed", path, b->name);
			return true;
		}
		unsigned n = spvc_type_get_array_dimension(type, 0);
		spvc_type_id elem = spvc_type_get_base_type_id(type);
		/* an array of arrays is rejected above, so the element is the base type */
		for (unsigned k = 0; k < n; k++) {
			char p2[256], m2[256];
			snprintf(p2, sizeof(p2), "%s[%u]", path, k);
			snprintf(m2, sizeof(m2), "%s_%u", mangled, k);
			if (!flatten(t, o, elem, offset + k * arrayStride, 0, p2, m2, b, leaves, depth + 1)) return false;
		}
		return true;
	}
	if (spvc_type_get_basetype(type) != SPVC_BASETYPE_STRUCT)
		return flatten_leaf(t, o, type, offset, path, mangled, b, leaves);
	unsigned members = spvc_type_get_num_member_types(type);
	for (unsigned m = 0; m < members; m++) {
		unsigned moff = 0, mstride = 0;
		spvc_compiler_type_struct_member_offset(t->comp, type, m, &moff);
		spvc_type_id mid = spvc_type_get_member_type(type, m);
		if (spvc_type_get_num_array_dimensions(spvc_compiler_get_type_handle(t->comp, mid)))
			spvc_compiler_type_struct_member_array_stride(t->comp, type, m, &mstride);
		const char *mn = spvc_compiler_get_member_name(t->comp, id, m);
		char mbuf[32];
		if (!mn || !*mn) { snprintf(mbuf, sizeof(mbuf), "_m%u", m); mn = mbuf; }
		char p2[256], m2[256];
		snprintf(p2, sizeof(p2), "%s.%s", path, mn);
		snprintf(m2, sizeof(m2), "%s_%s", mangled, mn);
		if (!flatten(t, o, mid, offset + moff, mstride, p2, m2, b, leaves, depth + 1)) return false;
	}
	return true;
}

/* Finds the block member a cbuffer line declares: members come in order and
 * are called <instance>_<member>. */
static int block_member(Tr *t, Block *b, const char *name) {
	spvc_type type = spvc_compiler_get_type_handle(t->comp, b->type);
	unsigned members = spvc_type_get_num_member_types(type);
	for (unsigned pass = 0; pass < 2; pass++)
		for (unsigned k = pass ? 0 : b->cursor; k < members; k++) {
			const char *mn = spvc_compiler_get_member_name(t->comp, b->type, k);
			size_t nl = strlen(name), ml = mn ? strlen(mn) : 0;
			if (ml && nl > ml && name[nl - ml - 1] == '_' && !strcmp(name + nl - ml, mn)) {
				b->cursor = k + 1;
				return (int)k;
			}
		}
	return -1;
}

static void emit_struct_member(Tr *t, Sb *o, const char *type, const char *name, const char *arr, Block *b) {
	int k = block_member(t, b, name);
	if (k < 0) {
		logf_(t->c, VKV_SPIRV_LOG_WARN, "struct member '%s' in block '%s' was not found, not fed", name, b->name);
		sb_printf(o, "uniform %s %s%s;\n", type, name, arr);
		return;
	}
	spvc_type bt = spvc_compiler_get_type_handle(t->comp, b->type);
	unsigned off = 0, stride = 0;
	spvc_compiler_type_struct_member_offset(t->comp, bt, (unsigned)k, &off);
	spvc_type_id mid = spvc_type_get_member_type(bt, (unsigned)k);
	if (spvc_type_get_num_array_dimensions(spvc_compiler_get_type_handle(t->comp, mid)))
		spvc_compiler_type_struct_member_array_stride(t->comp, bt, (unsigned)k, &stride);
	sb_printf(o, "static %s %s%s;\n", type, name, arr);
	uint32_t leaves = 0;
	flatten(t, o, mid, off, stride, name, name, b, &leaves, 0);
}

/* One cbuffer member: "    row_major float4x4 pc_mvp : packoffset(c0);" */
static void emit_cbuffer_member(Tr *t, Sb *o, const char *line, Block *b) {
	const char *s = skip_ws(line);
	if (starts_with(s, "row_major ")) s += 10;
	if (starts_with(s, "column_major ")) {
		logf_(t->c, VKV_SPIRV_LOG_WARN, "row-major matrix in block '%s' is uploaded transposed: %s", b->name, s);
		s += 13;
	}
	const char *po = strstr(s, " : packoffset(c");
	if (!po) {
		logf_(t->c, VKV_SPIRV_LOG_ERROR, "unexpected uniform block member: %s", s);
		return;
	}
	unsigned reg = 0;
	char comp = 0;
	const char *r = po + strlen(" : packoffset(c");
	reg = (unsigned)strtoul(r, (char **)&r, 10);
	if (*r == '.') comp = r[1];
	uint32_t offset = reg * 16 + (comp == 'y' ? 4 : comp == 'z' ? 8 : comp == 'w' ? 12 : 0);

	char decl[256];
	snprintf(decl, sizeof(decl), "%.*s", (int)(po - s), s);
	char *sp = strchr(decl, ' ');
	if (!sp) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "unexpected uniform block member: %s", s); return; }
	*sp = 0;
	const char *type = decl, *rest = sp + 1;
	char name[160];
	snprintf(name, sizeof(name), "%s", rest);
	char *br = strchr(name, '[');
	char arr[32] = "";
	if (br) { snprintf(arr, sizeof(arr), "%s", br); *br = 0; }

	const char *suffix;
	VkvSpirvScalar kind = scalar_kind(type, &suffix);
	if (!suffix) {
		emit_struct_member(t, o, type, name, arr, b);
		return;
	}
	block_member(t, b, name);
	if (kind == VKV_SPIRV_FLOAT) {
		sb_printf(o, "uniform %s %s%s;\n", type, name, arr);
		add_uniform(t, name, offset, b, kind);
		return;
	}
	if (arr[0] || strchr(suffix, 'x')) {
		logf_(t->c, VKV_SPIRV_LOG_WARN, "integer array/matrix '%s' in block '%s' is declared but not fed", name, b->name);
		sb_printf(o, "uniform float%s %s_vkv%s;\nstatic %s %s%s;\n", suffix, name, arr, type, name, arr);
		return;
	}
	/* integer uniform: fed as float, converted back at the start of main */
	char fname[200];
	snprintf(fname, sizeof(fname), "%s_vkv", name);
	sb_printf(o, "uniform float%s %s;\nstatic %s %s;\n", suffix, fname, type, name);
	sb_printf(&t->intInit, "    %s = %s(%s);\n", name, type, fname);
	add_uniform(t, fname, offset, b, kind);
}

static bool parse_io(Tr *t, const char *line, Io *io) {
	const char *s = skip_ws(line);
	for (;;) {
		if (starts_with(s, "nointerpolation ")) { s += 16; continue; }
		if (starts_with(s, "centroid ")) { s += 9; continue; }
		if (starts_with(s, "noperspective ")) {
			logf_(t->c, VKV_SPIRV_LOG_WARN, "noperspective interpolation is not available, using perspective");
			s += 14;
			continue;
		}
		break;
	}
	char type[32], name[128], sem[32];
	if (sscanf(s, "%31s %127s : %31[^;];", type, name, sem) != 3) {
		logf_(t->c, VKV_SPIRV_LOG_ERROR, "unexpected stage interface member: %s", s);
		return false;
	}
	if (strchr(name, '[')) {
		logf_(t->c, VKV_SPIRV_LOG_ERROR, "arrays in the stage interface are not supported ('%s')", name);
		return false;
	}
	memset(io, 0, sizeof(*io));
	snprintf(io->type, sizeof(io->type), "%s", type);
	snprintf(io->name, sizeof(io->name), "%s", name);
	snprintf(io->sem, sizeof(io->sem), "%s", sem);

	bool gxmIndex = !strcmp(io->sem, "INDEX") || !strcmp(io->sem, "INSTANCE");
	if (!strcmp(io->name, "vkv_point_coord")) snprintf(io->sem, sizeof(io->sem), "SPRITECOORD");
	else if (gxmIndex) snprintf(io->type, sizeof(io->type), "int"); /* the GXM compiler declares these as int */
	else if (!strcmp(io->sem, "VPOS")) snprintf(io->sem, sizeof(io->sem), "WPOS");
	else if (!strcmp(io->sem, "SV_IsFrontFace")) {
		snprintf(io->sem, sizeof(io->sem), "FACE");
		snprintf(io->type, sizeof(io->type), "float");
		io->frontFacing = true;
	} else if (!strcmp(io->sem, "COLOR0") && t->stage == VKV_SPIRV_FRAGMENT) {
		snprintf(io->sem, sizeof(io->sem), "COLOR");
	} else if (starts_with(io->sem, "SV_")) {
		logf_(t->c, VKV_SPIRV_LOG_ERROR, "built-in '%s' (%s) is not available on GXM", io->name, io->sem);
		return false;
	}

	const char *suffix;
	VkvSpirvScalar kind = scalar_kind(io->type, &suffix);
	bool attribute = t->stage == VKV_SPIRV_VERTEX && starts_with(io->sem, "ATTR");
	if (!io->frontFacing && !gxmIndex && suffix && kind != VKV_SPIRV_FLOAT && !attribute) {
		io->isInt = true;
		snprintf(io->floatType, sizeof(io->floatType), "float%s", suffix);
	}
	return true;
}

/* Replaces whole-word occurrences of `from` in `line` with `to`. */
static void replace_word(Sb *o, const char *line, const char *from, const char *to) {
	size_t fl = strlen(from);
	const char *s = line;
	for (;;) {
		const char *hit = strstr(s, from);
		while (hit && ((hit > line && is_ident(hit[-1])) || is_ident(hit[fl]))) hit = strstr(hit + 1, from);
		if (!hit) { sb_puts(o, s); return; }
		sb_add(o, s, (size_t)(hit - s));
		sb_puts(o, to);
		s = hit + fl;
	}
}

/* 0.5f.xxx -> float3(0.5f): the GXM compiler does not swizzle literals. */
static void fix_literal_swizzles(Sb *o, const char *line) {
	const char *s = line;
	for (;;) {
		const char *hit = strstr(s, "f.x");
		const char *lit = NULL;
		size_t xs = 0;
		while (hit) {
			xs = 0;
			while (hit[2 + xs] == 'x') xs++;
			bool ok = xs >= 1 && xs <= 4 && !is_ident(hit[2 + xs]) && hit > line && isdigit((unsigned char)hit[-1]);
			if (ok) {
				const char *b = hit - 1;
				while (b > line && (isdigit((unsigned char)b[-1]) || b[-1] == '.' ||
				                    ((b[-1] == '-' || b[-1] == '+') && b - 1 > line && (b[-2] == 'e' || b[-2] == 'E')) ||
				                    ((b[-1] == 'e' || b[-1] == 'E') && isdigit((unsigned char)b[0]))))
					b--;
				if (!(b > line && is_ident(b[-1]))) { lit = b; break; }
			}
			hit = strstr(hit + 1, "f.x");
		}
		if (!lit) { sb_puts(o, s); return; }
		sb_add(o, s, (size_t)(lit - s));
		if (xs == 1) sb_add(o, lit, (size_t)(hit + 1 - lit));
		else sb_printf(o, "float%u(%.*s)", (unsigned)xs, (int)(hit + 1 - lit), lit);
		s = hit + 2 + xs;
	}
}

/* (-1.0f).xx -> float2(-1.0f): same as above for a literal in parentheses. */
static char *fix_paren_literal_swizzles(const char *line) {
	Sb o = { 0 };
	const char *s = line;
	for (;;) {
		const char *hit = strstr(s, ").x");
		const char *open = NULL;
		size_t xs = 0;
		while (hit) {
			xs = 0;
			while (hit[2 + xs] == 'x') xs++;
			if (xs >= 1 && xs <= 4 && !is_ident(hit[2 + xs])) {
				const char *b = hit;
				while (b > s && b[-1] != '(' && b[-1] != ')') b--;
				if (b > s && b[-1] == '(' && !(b - 1 > line && is_ident(b[-2]))) {
					/* the parentheses must hold nothing but a number */
					bool num = b < hit, digit = false;
					for (const char *q = b; q < hit && num; q++) {
						if (isdigit((unsigned char)*q)) digit = true;
						else if (!strchr("+-.eEf ", *q)) num = false;
					}
					if (num && digit) { open = b - 1; break; }
				}
			}
			hit = strstr(hit + 1, ").x");
		}
		if (!open) { sb_puts(&o, s); return o.p; }
		sb_add(&o, s, (size_t)(open - s));
		if (xs == 1) sb_add(&o, open, (size_t)(hit + 1 - open));
		else sb_printf(&o, "float%u%.*s", (unsigned)xs, (int)(hit + 1 - open), open);
		s = hit + 2 + xs;
	}
}

/* Matching closing bracket for the one at s[0]; NULL when unbalanced. */
static const char *match_close(const char *s, char open, char close) {
	int depth = 0;
	for (const char *p = s; *p; p++) {
		if (*p == open) depth++;
		else if (*p == close && --depth == 0) return p;
	}
	return NULL;
}

static bool is_int_literal(const char *s, size_t n) {
	while (n && (*s == ' ')) { s++; n--; }
	while (n && (s[n - 1] == ' ' || s[n - 1] == 'u' || s[n - 1] == 'U')) n--;
	if (!n) return false;
	for (size_t i = 0; i < n; i++)
		if (!isdigit((unsigned char)s[i])) return false;
	return true;
}

/* tex[2] -> tex_2. A variable index picks the element in a chain of ?:
 * around the call that samples it: tex2D(tex[i], uv) ->
 * ((i) == 0 ? tex2D(tex_0, uv) : (i) == 1 ? tex2D(tex_1, uv) : tex2D(tex_2, uv)). */
static char *rewrite_sampler_arrays(Tr *t, const char *line) {
	char *cur = dupn(line, strlen(line));
	for (uint32_t si = 0; si < t->samplerCount && cur; si++) {
		const Sampler *smp = &t->samplers[si];
		if (!smp->array[0] || strcmp(smp->name + strlen(smp->array), "_0")) continue; /* once per array */
		const char *base = smp->array;
		size_t bl = strlen(base);
		for (int guard = 0; guard < 64; guard++) {
			char *hit = strstr(cur, base);
			while (hit && ((hit > cur && is_ident(hit[-1])) || hit[bl] != '[')) hit = strstr(hit + 1, base);
			if (!hit) break;
			const char *idxOpen = hit + bl, *idxClose = match_close(idxOpen, '[', ']');
			if (!idxClose) break;
			size_t il = (size_t)(idxClose - idxOpen - 1);
			Sb n = { 0 };
			if (is_int_literal(idxOpen + 1, il)) {
				unsigned k = (unsigned)strtoul(skip_ws(idxOpen + 1), NULL, 10);
				sb_add(&n, cur, (size_t)(hit - cur));
				sb_printf(&n, "%s_%u", base, k);
				sb_puts(&n, idxClose + 1);
			} else {
				/* the call around it: identifier, '(' and then the array */
				const char *open = hit;
				while (open > cur && open[-1] == ' ') open--;
				if (open == cur || open[-1] != '(') {
					logf_(t->c, VKV_SPIRV_LOG_ERROR, "sampler array '%s' is indexed outside a texture call", base);
					free(cur);
					return NULL;
				}
				open--;
				const char *fn = open;
				while (fn > cur && is_ident(fn[-1])) fn--;
				const char *close = match_close(open, '(', ')');
				if (fn == open || !close) {
					logf_(t->c, VKV_SPIRV_LOG_ERROR, "could not rewrite the use of sampler array '%s'", base);
					free(cur);
					return NULL;
				}
				sb_add(&n, cur, (size_t)(fn - cur));
				sb_puts(&n, "(");
				for (uint32_t k = 0; k < smp->arraySize; k++) {
					if (k + 1 < smp->arraySize) sb_printf(&n, "(%.*s) == %u ? ", (int)il, idxOpen + 1, (unsigned)k);
					sb_printf(&n, "%.*s(%s_%u%.*s)", (int)(open - fn), fn, base, (unsigned)k, (int)(close - idxClose - 1),
					          idxClose + 1);
					if (k + 1 < smp->arraySize) sb_puts(&n, " : ");
				}
				sb_puts(&n, ")");
				sb_puts(&n, close + 1);
			}
			if (n.oom) { free(n.p); free(cur); return NULL; }
			free(cur);
			cur = n.p;
		}
	}
	return cur;
}

static void emit_signature(Tr *t, Sb *o) {
	sb_puts(o, "void main(");
	bool first = true;
	for (uint32_t i = 0; i < t->inCount; i++) {
		const Io *io = &t->in[i];
		sb_printf(o, "%s%s stage_input_%s : %s", first ? "" : ",\n          ", io->isInt ? io->floatType : io->type,
		          io->name, io->sem);
		first = false;
	}
	for (uint32_t i = 0; i < t->outCount; i++) {
		const Io *io = &t->out[i];
		sb_printf(o, "%sout %s stage_output_%s : %s", first ? "" : ",\n          ", io->isInt ? io->floatType : io->type,
		          io->name, io->sem);
		first = false;
	}
	sb_puts(o, ")\n");
}

static char *replace_all(const char *s, const char *from, const char *to, bool wholeWord) {
	Sb n = { 0 };
	if (wholeWord) {
		replace_word(&n, s, from, to);
	} else {
		size_t fl = strlen(from);
		const char *hit;
		while ((hit = strstr(s, from))) {
			sb_add(&n, s, (size_t)(hit - s));
			sb_puts(&n, to);
			s = hit + fl;
		}
		sb_puts(&n, s);
	}
	return n.p ? n.p : dupn("", 0);
}

static void emit_main_statement(Tr *t, Sb *o, const char *line) {
	const char *s = skip_ws(line);
	if (starts_with(s, "SPIRV_Cross_Output stage_output;") || starts_with(s, "return stage_output;")) return;

	char *arrays = rewrite_sampler_arrays(t, line);
	if (!arrays) { if (!t->c->failed) o->oom = true; return; }
	char *cur = replace_all(arrays, "stage_input.", "stage_input_", false);
	free(arrays);
	char *next = replace_all(cur, "stage_output.", "stage_output_", false);
	free(cur);
	cur = next;

	for (uint32_t i = 0; i < t->inCount && cur; i++) {
		const Io *io = &t->in[i];
		if (!io->isInt && !io->frontFacing) continue;
		char from[160], to[256];
		snprintf(from, sizeof(from), "stage_input_%s", io->name);
		if (io->frontFacing) snprintf(to, sizeof(to), "(%s > 0.0f)", from);
		else snprintf(to, sizeof(to), "%s(floor(%s + 0.5f))", io->type, from);
		next = replace_all(cur, from, to, true);
		free(cur);
		cur = next;
	}
	for (uint32_t i = 0; i < t->outCount && cur; i++) {
		const Io *io = &t->out[i];
		if (!io->isInt) continue;
		char lhs[200];
		snprintf(lhs, sizeof(lhs), "stage_output_%s = ", io->name);
		char *eq = strstr(cur, lhs);
		char *semi = eq ? strrchr(cur, ';') : NULL;
		if (!eq || !semi) continue;
		Sb n = { 0 };
		char *rhs = eq + strlen(lhs);
		sb_add(&n, cur, (size_t)(rhs - cur));
		sb_printf(&n, "%s(%.*s)", io->floatType, (int)(semi - rhs), rhs);
		sb_puts(&n, semi);
		free(cur);
		cur = n.p;
	}
	if (!cur) { o->oom = true; return; }
	sb_puts(o, cur);
	sb_puts(o, "\n");
	if (t->pointSize && strstr(cur, "vert_main();")) sb_puts(o, "    stage_output_gl_PointSize = gl_PointSize;\n");
	free(cur);
}

static bool collect_struct(Tr *t, char **lines, size_t count, size_t *i, Io *dst, uint32_t *dstCount) {
	size_t k = *i + 1;
	if (k < count && !strcmp(skip_ws(lines[k]), "{")) k++;
	for (; k < count; k++) {
		const char *s = skip_ws(lines[k]);
		if (starts_with(s, "};")) break;
		if (!*s) continue;
		if (*dstCount >= MAX_IO) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "too many stage inputs/outputs"); return false; }
		if (!parse_io(t, lines[k], &dst[*dstCount])) return false;
		(*dstCount)++;
	}
	*i = k;
	return true;
}

/* The GXM compiler has no unsigned types: uint becomes int and 1u becomes 1. */
static char *drop_unsigned(char *src) {
	static const char *const from[] = { "uint", "uint2", "uint3", "uint4" };
	static const char *const to[] = { "int", "int2", "int3", "int4" };
	char *cur = src;
	for (int k = 0; k < 4 && cur; k++) {
		char *n = replace_all(cur, from[k], to[k], true);
		free(cur);
		cur = n;
	}
	if (!cur) return NULL;
	/* unsigned literal suffixes: digits followed by u or U, not part of a name */
	char *w = cur;
	for (char *r = cur; *r; r++) {
		if ((*r == 'u' || *r == 'U') && r > cur && isdigit((unsigned char)r[-1]) && !is_ident(r[1])) {
			const char *b = r - 1;
			while (b > cur && (isalnum((unsigned char)b[-1]) || b[-1] == '.')) b--;
			if (isdigit((unsigned char)*b)) continue; /* a number like 10u: skip the suffix */
		}
		*w++ = *r;
	}
	*w = 0;
	return cur;
}

static char *rewrite(Tr *t, const char *hlsl) {
	/* split into lines */
	size_t count = 0, cap = 256;
	char **lines = malloc(cap * sizeof(char *));
	char *text = dupn(hlsl, strlen(hlsl));
	if (!lines || !text) { free(lines); free(text); return NULL; }
	for (char *p = text; *p;) {
		char *nl = strchr(p, '\n');
		if (count == cap) {
			cap *= 2;
			char **nlines = realloc(lines, cap * sizeof(char *));
			if (!nlines) { free(lines); free(text); return NULL; }
			lines = nlines;
		}
		lines[count++] = p;
		if (!nl) break;
		*nl = 0;
		p = nl + 1;
	}

	/* first pass: stage interface structs */
	for (size_t i = 0; i < count; i++) {
		if (!strcmp(lines[i], "struct SPIRV_Cross_Input")) collect_struct(t, lines, count, &i, t->in, &t->inCount);
		else if (!strcmp(lines[i], "struct SPIRV_Cross_Output")) collect_struct(t, lines, count, &i, t->out, &t->outCount);
		else if (!strcmp(lines[i], "static float gl_PointSize;")) t->pointSize = t->stage == VKV_SPIRV_VERTEX;
	}
	/* shader model 3 compatibility drops gl_PointSize, GXM has it as PSIZE */
	if (t->pointSize && t->outCount < MAX_IO) {
		Io *io = &t->out[t->outCount++];
		memset(io, 0, sizeof(*io));
		snprintf(io->type, sizeof(io->type), "float");
		snprintf(io->name, sizeof(io->name), "gl_PointSize");
		snprintf(io->sem, sizeof(io->sem), "PSIZE");
	}

	Sb o = { 0 };
	sb_printf(&o, "/* generated by VkVita from SPIR-V (%s) */\n", t->stage == VKV_SPIRV_VERTEX ? "vertex" : "fragment");
	for (size_t i = 0; i < count && !t->c->failed; i++) {
		const char *line = lines[i];

		if (starts_with(line, "cbuffer ")) {
			char name[128];
			sscanf(line + 8, "%127[^ \t:]", name);
			Block *b = find_block(t, name);
			if (!b) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "unknown uniform block '%s'", name); break; }
			for (i++; i < count; i++) {
				const char *m = skip_ws(lines[i]);
				if (!strcmp(m, "{") || !*m) continue;
				if (starts_with(m, "};")) break;
				emit_cbuffer_member(t, &o, lines[i], b);
			}
			continue;
		}
		if (strstr(line, "gl_HalfPixel")) continue;
		if (starts_with(line, "uniform sampler")) {
			char type[64], name[160];
			if (sscanf(line, "uniform %63s %159[^;];", type, name) != 2) {
				logf_(t->c, VKV_SPIRV_LOG_ERROR, "unexpected sampler declaration: %s", line);
				break;
			}
			char *br = strchr(name, '[');
			if (br) {
				/* a sampler array becomes one sampler per element */
				*br = 0;
				bool any = false;
				for (uint32_t k = 0; k < t->samplerCount; k++)
					if (!strcmp(t->samplers[k].array, name)) {
						sb_printf(&o, "uniform %s %s : TEXUNIT%u;\n", type, t->samplers[k].name,
						          (unsigned)t->samplers[k].t.unit);
						any = true;
					}
				if (!any) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "unknown sampler array '%s'", name); break; }
				continue;
			}
			const Sampler *smp = find_sampler(t, name);
			if (!smp) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "unknown sampler '%s'", name); break; }
			sb_printf(&o, "uniform %s %s : TEXUNIT%u;\n", type, name, (unsigned)smp->t.unit);
			continue;
		}
		if (!strcmp(line, "struct SPIRV_Cross_Input") || !strcmp(line, "struct SPIRV_Cross_Output")) {
			for (i++; i < count && !starts_with(skip_ws(lines[i]), "};"); i++) {}
			if (i + 1 < count && !*lines[i + 1]) i++;
			continue;
		}
		if (starts_with(line, "SPIRV_Cross_Output main(") || starts_with(line, "void main(")) {
			emit_signature(t, &o);
			i++;
			if (i < count) sb_printf(&o, "%s\n", lines[i]);
			if (t->intInit.p) sb_puts(&o, t->intInit.p);
			for (i++; i < count && strcmp(lines[i], "}"); i++) emit_main_statement(t, &o, lines[i]);
			sb_puts(&o, "}\n");
			continue;
		}
		char *arrays = rewrite_sampler_arrays(t, line);
		if (!arrays) { o.oom = !t->c->failed; break; }
		char *fixed = fix_paren_literal_swizzles(arrays);
		free(arrays);
		/* the GXM compiler has no matrix layout qualifiers (struct members) */
		if (fixed && strstr(fixed, "row_major ")) {
			char *nf = replace_all(fixed, "row_major ", "", false);
			free(fixed);
			fixed = nf;
		}
		if (!fixed) { o.oom = true; break; }
		fix_literal_swizzles(&o, fixed);
		free(fixed);
		sb_puts(&o, "\n");
	}
	free(lines);
	free(text);
	if (o.oom || t->c->failed) { free(o.p); return NULL; }
	return drop_unsigned(o.p);
}

/* ------------------------------------------------------------------------ */
/* Names                                                                    */
/* ------------------------------------------------------------------------ */
/* Words the GXM Cg compiler reserves that SPIRV-Cross does not rename for
 * HLSL, so a GLSL variable called "row" would not compile. */
static const char *const k_cgKeywords[] = {
	"row", "column", "fixed", "half", "packed", "emit", "get", "decl", "dword", "interface", "asm", "asm_fragment",
	"pixelfragment", "vertexfragment", "pixelshader", "vertexshader", "sampler_state", "this", "template", "typename",
	"typeid", "friend", "virtual", "mutable", "explicit", "operator", "namespace", "using", "private", "protected",
	"public", "class", "new", "delete", "throw", "try", "catch", "goto", "union", "enum", "long", "short", "signed",
	"char", "double", "auto", "extern", "register", "volatile", "inline", "sizeof", "static_cast", "const_cast",
	"dynamic_cast", "reinterpret_cast", "compile", "technique", "pass", "string", "texture", "matrix", "vector",
	"shared", "snorm", "unorm", "precise", "centroid", "nointerpolation", "noperspective", "linear", "sample",
	"discard", "in", "out", "inout", "uniform", "varying",
};

static bool is_cg_keyword(const char *n) {
	for (size_t i = 0; i < sizeof(k_cgKeywords) / sizeof(k_cgKeywords[0]); i++)
		if (!strcmp(n, k_cgKeywords[i])) return true;
	return false;
}

static void rename_keywords(Tr *t, spvc_compiler comp, uint32_t bound) {
	for (uint32_t id = 1; id < bound; id++) {
		const char *n = spvc_compiler_get_name(comp, id);
		if (!n || !*n || !is_cg_keyword(n)) continue;
		char renamed[64];
		snprintf(renamed, sizeof(renamed), "%s_vkv", n);
		logf_(t->c, VKV_SPIRV_LOG_INFO, "'%s' is a Cg keyword, renamed to '%s'", n, renamed);
		spvc_compiler_set_name(comp, id, renamed);
	}
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */
int vkv_spirv_translate(const uint32_t *words, size_t wordCount, VkvSpirvStage stage, VkvSpirvLogFn log, void *user,
                        VkvSpirvResult *out) {
	memset(out, 0, sizeof(*out));
	Ctx c = { log, user, false };
	if (!words || wordCount < 5 || words[0] != VKV_SPIRV_MAGIC) {
		logf_(&c, VKV_SPIRV_LOG_ERROR, "not a SPIR-V module");
		return -1;
	}
	Tr *t = calloc(1, sizeof(*t));
	if (!t) return -1;
	t->c = &c;
	t->stage = stage;

	spvc_context ctx = NULL;
	spvc_parsed_ir ir = NULL;
	spvc_compiler comp = NULL;
	spvc_compiler_options opts = NULL;
	spvc_resources res = NULL;
	const char *hlsl = NULL;
	char *cg = NULL;
	SpvExecutionModel want = stage == VKV_SPIRV_VERTEX ? SpvExecutionModelVertex : SpvExecutionModelFragment;

	uint32_t *builtins = NULL, *lowered = NULL, *intdiv = NULL;
	size_t builtinsCount = 0, loweredCount = 0, intdivCount = 0;
	int br = vkv_spirv_lower_builtins(words, wordCount, &builtins, &builtinsCount);
	if (br > 0) {
		words = builtins;
		wordCount = builtinsCount;
	}
	int dr = br < 0 ? 0 : vkv_spirv_lower_intdiv(words, wordCount, &intdiv, &intdivCount);
	if (dr > 0) {
		logf_(&c, VKV_SPIRV_LOG_INFO, "integer division is done in float");
		words = intdiv;
		wordCount = intdivCount;
	}
	int lr = br < 0 || dr < 0 || stage != VKV_SPIRV_FRAGMENT ? 0
	         : vkv_spirv_lower_subpass(words, wordCount, &lowered, &loweredCount);
	if (br < 0 || dr < 0 || lr < 0) {
		logf_(&c, VKV_SPIRV_LOG_ERROR, "malformed SPIR-V module");
		free(builtins);
		free(intdiv);
		free(t);
		return -1;
	}
	if (lr > 0) {
		logf_(&c, VKV_SPIRV_LOG_INFO, "subpass inputs are read as textures at the current pixel");
		words = lowered;
		wordCount = loweredCount;
	}

	if (spvc_context_create(&ctx) != SPVC_SUCCESS) { free(lowered); free(intdiv); free(builtins); free(t); return -1; }
	spvc_context_set_error_callback(ctx, spvc_error_cb, &c);
	if (spvc_context_parse_spirv(ctx, (const SpvId *)words, wordCount, &ir) != SPVC_SUCCESS) goto done;
	if (spvc_context_create_compiler(ctx, SPVC_BACKEND_HLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS)
		goto done;

	const spvc_entry_point *eps;
	size_t epCount;
	const char *epName = NULL;
	spvc_compiler_get_entry_points(comp, &eps, &epCount);
	for (size_t i = 0; i < epCount; i++)
		if (eps[i].execution_model == want) { epName = eps[i].name; break; }
	if (!epName) {
		logf_(&c, VKV_SPIRV_LOG_ERROR, "module has no %s entry point", stage == VKV_SPIRV_VERTEX ? "vertex" : "fragment");
		goto done;
	}
	if (spvc_compiler_set_entry_point(comp, epName, want) != SPVC_SUCCESS) goto done;
	rename_keywords(t, comp, (uint32_t)words[3]);

	if (spvc_compiler_create_compiler_options(comp, &opts) != SPVC_SUCCESS) goto done;
	spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, 30);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT, SPVC_TRUE);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y, SPVC_FALSE);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, SPVC_FALSE);
	if (spvc_compiler_install_compiler_options(comp, opts) != SPVC_SUCCESS) goto done;

	if (spvc_compiler_create_shader_resources(comp, &res) != SPVC_SUCCESS) goto done;
	t->comp = comp;
	if (!reflect(t, comp, res)) goto done;

	if (spvc_compiler_compile(comp, &hlsl) != SPVC_SUCCESS || !hlsl) goto done;
	cg = rewrite(t, hlsl);

done:
	if (cg && !c.failed) {
		out->cg = cg;
		out->unis = t->unis;
		out->uniCount = t->uniCount;
		t->unis = NULL;
		t->uniCount = 0;
		if (t->samplerCount) {
			out->texs = malloc(t->samplerCount * sizeof(*out->texs));
			if (out->texs) {
				for (uint32_t i = 0; i < t->samplerCount; i++) out->texs[i] = t->samplers[i].t;
				out->texCount = t->samplerCount;
			}
		}
	} else {
		free(cg);
		cg = NULL;
	}
	for (uint32_t i = 0; i < t->uniCount; i++) free(t->unis[i].name);
	free(t->unis);
	free(t->intInit.p);
	free(t);
	if (ctx) spvc_context_destroy(ctx);
	free(lowered);
	free(intdiv);
	free(builtins);
	return out->cg ? 0 : -1;
}

void vkv_spirv_free(VkvSpirvResult *r) {
	if (!r) return;
	for (uint32_t i = 0; i < r->uniCount; i++) free(r->unis[i].name);
	free(r->unis);
	free(r->texs);
	free(r->cg);
	memset(r, 0, sizeof(*r));
}
