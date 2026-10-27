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
	char     name[128];
	int32_t  set;       /* -1 = push constants */
	uint32_t binding;
} Block;

typedef struct Sampler {
	char            name[128];
	VkvSpirvTexture t;
	bool            fixedUnit;  /* combined image sampler: unit = binding if free */
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
	Sb        intInit;     /* "name = int(name_vkv);" lines for integer uniforms */
} Tr;

static const char *k_unsupportedRes[] = {
	"storage buffers", "storage images", "subpass inputs", "acceleration structures",
};

static void block_name(spvc_compiler comp, spvc_type_id base, char *out, size_t cap) {
	const char *n = spvc_compiler_get_name(comp, base);
	if (n && *n) snprintf(out, cap, "%s", n);
	else snprintf(out, cap, "_%u", (unsigned)base);
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
		if (t->samplerCount >= MAX_SAMPLERS) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "too many samplers"); break; }
		Sampler *s = &t->samplers[t->samplerCount++];
		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "vkv_tex%u", (unsigned)i);
		spvc_compiler_set_name(comp, combos[i].combined_id, s->name);
		s->t.imageSet = (int32_t)spvc_compiler_get_decoration(comp, combos[i].image_id, SpvDecorationDescriptorSet);
		s->t.imageBinding = spvc_compiler_get_decoration(comp, combos[i].image_id, SpvDecorationBinding);
		if (dummy && combos[i].sampler_id == dummy) {
			s->t.samplerSet = -2;
		} else {
			s->t.samplerSet = (int32_t)spvc_compiler_get_decoration(comp, combos[i].sampler_id, SpvDecorationDescriptorSet);
			s->t.samplerBinding = spvc_compiler_get_decoration(comp, combos[i].sampler_id, SpvDecorationBinding);
		}
	}

	if (spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &n) == SPVC_SUCCESS) {
		for (size_t i = 0; i < n; i++) {
			spvc_type type = spvc_compiler_get_type_handle(comp, list[i].type_id);
			if (spvc_type_get_num_array_dimensions(type)) {
				logf_(t->c, VKV_SPIRV_LOG_ERROR, "sampler arrays are not supported ('%s')", list[i].name);
				continue;
			}
			if (t->samplerCount >= MAX_SAMPLERS) { logf_(t->c, VKV_SPIRV_LOG_ERROR, "too many samplers"); break; }
			Sampler *s = &t->samplers[t->samplerCount++];
			memset(s, 0, sizeof(*s));
			snprintf(s->name, sizeof(s->name), "%s", list[i].name);
			s->t.imageSet = (int32_t)spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationDescriptorSet);
			s->t.imageBinding = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding);
			s->t.samplerSet = -1;
			s->fixedUnit = true;
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
			snprintf(sem, sizeof(sem), "ATTR%u", remap.location);
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
static const Block *find_block(const Tr *t, const char *name) {
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

/* One cbuffer member: "    row_major float4x4 pc_mvp : packoffset(c0);" */
static void emit_cbuffer_member(Tr *t, Sb *o, const char *line, const Block *b) {
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
		logf_(t->c, VKV_SPIRV_LOG_WARN, "struct member '%s' of type %s in block '%s' is declared but not fed", name, type,
		      b->name);
		sb_printf(o, "uniform %s %s%s;\n", type, name, arr);
		return;
	}
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

	if (!strcmp(io->sem, "VPOS")) snprintf(io->sem, sizeof(io->sem), "WPOS");
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
	if (!io->frontFacing && suffix && kind != VKV_SPIRV_FLOAT && !attribute) {
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

	char *cur = replace_all(line, "stage_input.", "stage_input_", false);
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
	}

	Sb o = { 0 };
	sb_printf(&o, "/* generated by VkVita from SPIR-V (%s) */\n", t->stage == VKV_SPIRV_VERTEX ? "vertex" : "fragment");
	for (size_t i = 0; i < count && !t->c->failed; i++) {
		const char *line = lines[i];

		if (starts_with(line, "cbuffer ")) {
			char name[128];
			sscanf(line + 8, "%127[^ \t:]", name);
			const Block *b = find_block(t, name);
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
		fix_literal_swizzles(&o, line);
		sb_puts(&o, "\n");
	}
	free(lines);
	free(text);
	if (o.oom || t->c->failed) { free(o.p); return NULL; }
	return o.p;
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

	uint32_t *lowered = NULL;
	size_t loweredCount = 0;
	if (stage == VKV_SPIRV_FRAGMENT) {
		int lr = vkv_spirv_lower_subpass(words, wordCount, &lowered, &loweredCount);
		if (lr < 0) {
			logf_(&c, VKV_SPIRV_LOG_ERROR, "malformed SPIR-V module");
			free(t);
			return -1;
		}
		if (lr > 0) {
			logf_(&c, VKV_SPIRV_LOG_INFO, "subpass inputs are read as textures at the current pixel");
			words = lowered;
			wordCount = loweredCount;
		}
	}

	if (spvc_context_create(&ctx) != SPVC_SUCCESS) { free(lowered); free(t); return -1; }
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

	if (spvc_compiler_create_compiler_options(comp, &opts) != SPVC_SUCCESS) goto done;
	spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, 30);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT, SPVC_TRUE);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y, SPVC_FALSE);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, SPVC_FALSE);
	if (spvc_compiler_install_compiler_options(comp, opts) != SPVC_SUCCESS) goto done;

	if (spvc_compiler_create_shader_resources(comp, &res) != SPVC_SUCCESS) goto done;
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
