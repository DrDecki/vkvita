/*
 * VkVita: SPIR-V to Cg translation.
 *
 * Every SPIR-V value becomes a temporary declared at the top of its
 * function, structured control flow becomes if/else and for(;;) loops, and
 * GLSL matrices are kept transposed (a Cg row holds a GLSL column), which is
 * how GXM loads column-major uniform data.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_spirv.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spirv.h"
#include "GLSL.std.450.h"

/* Return codes of vkv_spirv_translate_native: 0 ok, -1 the shader cannot run
 * on GXM, -2 the shader uses something this translator does not handle. */
#define TR_OK          0
#define TR_FAIL       -1
#define TR_UNSUPPORTED -2

/* --- String builder --- */
typedef struct Sb {
	char  *p;
	size_t n, cap;
	bool   oom;
} Sb;

static void sb_add(Sb *b, const char *s, size_t n) {
	if (b->oom) return;
	if (b->n + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
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
	char tmp[512];
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

static char *sb_take(Sb *b) {
	char *p = b->p;
	if (!p && !b->oom) p = calloc(1, 1);
	b->p = NULL;
	b->n = b->cap = 0;
	return p;
}

static char *xstrdup(const char *s) {
	size_t n = strlen(s);
	char *d = malloc(n + 1);
	if (d) memcpy(d, s, n + 1);
	return d;
}

static char *xprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *xprintf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) return NULL;
	char *s = malloc((size_t)n + 1);
	if (!s) return NULL;
	va_start(ap, fmt);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return s;
}

/* --- Module data --- */
typedef struct Member {
	char    *name;
	int32_t  offset;
	int32_t  matrixStride;
	int32_t  builtin;
	int32_t  location;
	bool     rowMajor;
} Member;

typedef struct Id {
	const uint32_t *def;     /* defining instruction */
	uint32_t        type;    /* result type of a value */
	char           *name;    /* OpName */
	Member         *mem;     /* struct members */
	uint32_t        memCount;
	int32_t         location, binding, set, builtin, arrayStride;
	bool            flat, noPerspective, block, bufferBlock, hasSet;
	bool            used;    /* referenced by reachable code */
	/* emission */
	char           *expr;    /* how the value or variable is written in Cg */
	int32_t         samplerRef; /* sampled image value: index into Tr.samplers, -1 none */
	uint32_t        samplerElem; /* id of the element index, 0 = element 0 */
	bool            isConstExpr;
	bool            inl;         /* written into its only use instead of a temporary */
	uint16_t        uses;
} Id;

typedef struct Block {
	uint32_t        label;
	const uint32_t *first;   /* first instruction after OpLabel */
	const uint32_t *term;    /* terminator */
	uint32_t        mergeOp; /* SpvOpSelectionMerge, SpvOpLoopMerge or 0 */
	uint32_t        merge, cont;
} Block;

typedef struct Func {
	uint32_t        id;
	const uint32_t *begin, *end;
	Block          *blocks;
	uint32_t        blockCount;
	bool            reachable, emitted;
} Func;

/* A GXM texture unit fed by one element of a combined sampler or of an
 * image/sampler pair. */
typedef struct Smp {
	char            name[96];
	VkvSpirvTexture t;
	uint32_t        imageVar, samplerVar; /* samplerVar 0 = combined */
	uint32_t        element;
	uint32_t        count;     /* elements of the array this one belongs to */
	uint32_t        first;     /* index of element 0 in Tr.samplers */
	bool            fixedUnit;
	const char     *cgType;
} Smp;

typedef struct Io {
	uint32_t var;
	uint32_t member;     /* member of a builtin block, ~0u for a plain variable */
	char     name[160];  /* static copy in Cg */
	char     type[32];
	char     sem[24];
	bool     isInt, frontFacing, isOutput;
	char     floatType[16];
} Io;

#define MAX_SAMPLERS 16
#define NO_SAMPLER   0xFFFFFFFFu /* samplerVar of an image used without a sampler (texelFetch, size queries) */
#define MAX_IO       32
#define MAX_LOOPS    32

typedef struct Loop {
	uint32_t header, merge, cont;
} Loop;

typedef struct Tr {
	VkvSpirvLogFn  log;
	void          *user;
	VkvSpirvStage  stage;
	int            status;
	const uint32_t *words;
	size_t          count;
	uint32_t        bound;
	Id             *ids;
	Func           *funcs;
	uint32_t        funcCount;
	uint32_t        entry;
	uint32_t        glsl450;
	Smp             samplers[MAX_SAMPLERS];
	uint32_t        samplerCount;
	Io              io[MAX_IO];
	uint32_t        ioCount;
	VkvSpirvUniform *unis;
	uint32_t        uniCount, uniCap;
	Sb              globals;   /* uniforms, statics, structs */
	Sb              init;      /* start of main: conversions of uniforms */
	Sb              funcsOut;  /* functions in call order */
	VkvSpirvStride  strides[64];
	uint32_t        strideCount;
	bool            pointSize;
	bool            texInfo;      /* uses vkv_texinfo (texture sizes, mip levels, compare ops) */
	bool            needCompare;  /* uses vkv_compare (depth compare in the shader) */
	/* function being emitted */
	Func           *fn;
	Loop            loops[MAX_LOOPS];
	uint32_t        loopDepth;
	int             indent;
	Sb             *out;
} Tr;

static uint32_t g_nativeArraySize = 64;
static int      g_nativeScissor;

static void tr_log(Tr *t, VkvSpirvLogLevel lvl, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void tr_log(Tr *t, VkvSpirvLogLevel lvl, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (t->log) t->log(lvl, msg, t->user);
}

static void fail(Tr *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Tr *t, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (t->status == TR_OK) t->status = TR_FAIL;
	if (t->log) t->log(VKV_SPIRV_LOG_ERROR, msg, t->user);
}

/* Something the translator does not handle (yet). */
static void unsupported(Tr *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void unsupported(Tr *t, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (t->status == TR_OK) t->status = TR_UNSUPPORTED;
	if (t->log) t->log(VKV_SPIRV_LOG_ERROR, msg, t->user);
}

static inline uint32_t op_of(const uint32_t *w) { return w[0] & 0xFFFFu; }
static inline uint32_t wc_of(const uint32_t *w) { return w[0] >> 16; }

static Id *id_at(Tr *t, uint32_t id) { return id < t->bound ? &t->ids[id] : NULL; }
static const uint32_t *def_of(Tr *t, uint32_t id) { return id < t->bound ? t->ids[id].def : NULL; }
static uint32_t def_op(Tr *t, uint32_t id) { const uint32_t *d = def_of(t, id); return d ? op_of(d) : 0; }

/* --- Types --- */
static uint32_t strip_ptr(Tr *t, uint32_t type) {
	const uint32_t *d = def_of(t, type);
	return d && op_of(d) == SpvOpTypePointer ? d[3] : type;
}


static uint32_t scalar_of(Tr *t, uint32_t type) {
	const uint32_t *d = def_of(t, type);
	while (d && (op_of(d) == SpvOpTypeVector || op_of(d) == SpvOpTypeMatrix)) d = def_of(t, d[2]);
	return d ? d[1] : 0;
}

static uint32_t vec_size(Tr *t, uint32_t type) {
	const uint32_t *d = def_of(t, type);
	return d && op_of(d) == SpvOpTypeVector ? d[3] : 1;
}

static bool is_float_type(Tr *t, uint32_t type) { return def_op(t, scalar_of(t, type)) == SpvOpTypeFloat; }
static bool is_bool_type(Tr *t, uint32_t type) { return def_op(t, scalar_of(t, type)) == SpvOpTypeBool; }

static bool const_u32(Tr *t, uint32_t id, uint32_t *v) {
	const uint32_t *d = def_of(t, id);
	if (!d || (op_of(d) != SpvOpConstant && op_of(d) != SpvOpSpecConstant)) return false;
	*v = d[3];
	return true;
}

static uint32_t array_len(Tr *t, uint32_t arrType) {
	const uint32_t *d = def_of(t, arrType);
	uint32_t n = 0;
	if (!d || op_of(d) != SpvOpTypeArray || !const_u32(t, d[3], &n)) return 0;
	return n;
}

static const char *base_name(Tr *t, uint32_t scalar) {
	const uint32_t *d = def_of(t, scalar);
	if (!d) return "float";
	switch (op_of(d)) {
	case SpvOpTypeBool: return "bool";
	case SpvOpTypeInt: return "int"; /* the GXM compiler has no unsigned types */
	default: return "float";
	}
}

/* Cg spelling of a value type without array dimensions. */
static void type_str(Tr *t, uint32_t type, char *out, size_t cap) {
	const uint32_t *d = def_of(t, type);
	if (!d) { snprintf(out, cap, "float"); return; }
	switch (op_of(d)) {
	case SpvOpTypeVoid: snprintf(out, cap, "void"); return;
	case SpvOpTypeBool: case SpvOpTypeInt: case SpvOpTypeFloat:
		snprintf(out, cap, "%s", base_name(t, type));
		return;
	case SpvOpTypeVector: snprintf(out, cap, "%s%u", base_name(t, d[2]), d[3]); return;
	case SpvOpTypeMatrix: {
		/* d[3] GLSL columns of vec<rows>; the transposed Cg matrix has a row per column */
		uint32_t rows = vec_size(t, d[2]);
		snprintf(out, cap, "%s%ux%u", base_name(t, scalar_of(t, type)), d[3], rows);
		return;
	}
	case SpvOpTypeStruct: snprintf(out, cap, "S%u", type); return;
	case SpvOpTypeArray: case SpvOpTypeRuntimeArray: type_str(t, d[2], out, cap); return;
	case SpvOpTypePointer: type_str(t, d[3], out, cap); return;
	default: snprintf(out, cap, "float"); return;
	}
}

/* Array dimensions of a type as a declarator suffix, "[4]" or "". */
static void array_suffix(Tr *t, uint32_t type, char *out, size_t cap) {
	out[0] = 0;
	size_t n = 0;
	const uint32_t *d = def_of(t, strip_ptr(t, type));
	while (d && op_of(d) == SpvOpTypeArray && n + 16 < cap) {
		n += (size_t)snprintf(out + n, cap - n, "[%u]", array_len(t, d[1]));
		d = def_of(t, d[2]);
	}
	if (d && op_of(d) == SpvOpTypeRuntimeArray) snprintf(out + n, cap - n, "[%u]", g_nativeArraySize);
}

/* --- Names --- */
static const char *const k_cgReserved[] = {
	"row", "column", "fixed", "half", "packed", "emit", "get", "decl", "dword", "interface", "asm", "asm_fragment",
	"pixelfragment", "vertexfragment", "pixelshader", "vertexshader", "sampler_state", "this", "template", "typename",
	"typeid", "friend", "virtual", "mutable", "explicit", "operator", "namespace", "using", "private", "protected",
	"public", "class", "new", "delete", "throw", "try", "catch", "goto", "union", "enum", "long", "short", "signed",
	"char", "double", "auto", "extern", "register", "volatile", "inline", "sizeof", "static_cast", "const_cast",
	"dynamic_cast", "reinterpret_cast", "compile", "technique", "pass", "string", "texture", "matrix", "vector",
	"shared", "snorm", "unorm", "precise", "centroid", "nointerpolation", "noperspective", "linear", "sample",
	"discard", "in", "out", "inout", "uniform", "varying", "static", "const", "struct", "return", "if", "else", "for",
	"while", "do", "break", "continue", "switch", "case", "default", "true", "false", "void", "bool", "int", "float",
	"main", "sampler", "sampler1D", "sampler2D", "sampler3D", "samplerCUBE", "typedef", "unsigned",
};

static bool is_reserved(const char *n) {
	for (size_t i = 0; i < sizeof(k_cgReserved) / sizeof(k_cgReserved[0]); i++)
		if (!strcmp(n, k_cgReserved[i])) return true;
	return !strncmp(n, "gl_", 3) || !strncmp(n, "vkv_", 4) || !strncmp(n, "_", 1) || !strncmp(n, "stage_", 6);
}

/* A readable, unique Cg identifier for an id: its OpName plus the id. */
static char *ident(Tr *t, uint32_t id, const char *prefix) {
	const Id *e = id_at(t, id);
	char clean[64];
	size_t n = 0;
	if (e && e->name)
		for (const char *s = e->name; *s && n + 1 < sizeof(clean); s++)
			if (isalnum((unsigned char)*s) || *s == '_') clean[n++] = *s;
	clean[n] = 0;
	if (n && !isdigit((unsigned char)clean[0])) return xprintf("%s%s_%u", prefix, clean, id);
	return xprintf("%s_%u", prefix, id);
}

static void clean_name(const char *in, char *out, size_t cap) {
	size_t n = 0;
	for (const char *s = in; s && *s && n + 1 < cap; s++)
		if (isalnum((unsigned char)*s) || *s == '_') out[n++] = *s;
	out[n] = 0;
	if (n && isdigit((unsigned char)out[0])) { memmove(out + 1, out, n + 1 < cap ? n + 1 : cap - 1); out[0] = 'n'; }
	if (n && is_reserved(out) && n + 4 < cap) strcat(out, "_vkv");
}

/* --- Parsing --- */
static bool parse(Tr *t) {
	const uint32_t *w = t->words + 5, *end = t->words + t->count;
	Func *cur = NULL;
	uint32_t blockCap = 0;
	while (w < end) {
		uint32_t op = op_of(w), wc = wc_of(w);
		if (!wc || w + wc > end) { fail(t, "malformed SPIR-V instruction"); return false; }
		switch (op) {
		case SpvOpName:
			if (w[1] < t->bound && !t->ids[w[1]].name) t->ids[w[1]].name = xstrdup((const char *)(w + 2));
			break;
		case SpvOpMemberName: {
			Id *e = id_at(t, w[1]);
			if (!e) break;
			if (w[2] >= e->memCount) {
				Member *m = realloc(e->mem, (w[2] + 1) * sizeof(Member));
				if (!m) { fail(t, "out of memory"); return false; }
				memset(m + e->memCount, 0, (w[2] + 1 - e->memCount) * sizeof(Member));
				for (uint32_t k = e->memCount; k <= w[2]; k++) { m[k].offset = -1; m[k].builtin = -1; m[k].location = -1; }
				e->mem = m;
				e->memCount = w[2] + 1;
			}
			free(e->mem[w[2]].name);
			e->mem[w[2]].name = xstrdup((const char *)(w + 3));
			break;
		}
		case SpvOpExtInstImport:
			if (!strcmp((const char *)(w + 2), "GLSL.std.450")) t->glsl450 = w[1];
			break;
		case SpvOpDecorate: {
			Id *e = id_at(t, w[1]);
			if (!e || wc < 3) break;
			uint32_t v = wc > 3 ? w[3] : 0;
			switch (w[2]) {
			case SpvDecorationLocation: e->location = (int32_t)v; break;
			case SpvDecorationBinding: e->binding = (int32_t)v; break;
			case SpvDecorationDescriptorSet: e->set = (int32_t)v; e->hasSet = true; break;
			case SpvDecorationBuiltIn: e->builtin = (int32_t)v; break;
			case SpvDecorationArrayStride: e->arrayStride = (int32_t)v; break;
			case SpvDecorationFlat: e->flat = true; break;
			case SpvDecorationNoPerspective: e->noPerspective = true; break;
			case SpvDecorationBlock: e->block = true; break;
			case SpvDecorationBufferBlock: e->bufferBlock = true; break;
			default: break;
			}
			break;
		}
		case SpvOpMemberDecorate: {
			Id *e = id_at(t, w[1]);
			if (!e || wc < 4) break;
			if (w[2] >= e->memCount) {
				Member *m = realloc(e->mem, (w[2] + 1) * sizeof(Member));
				if (!m) { fail(t, "out of memory"); return false; }
				memset(m + e->memCount, 0, (w[2] + 1 - e->memCount) * sizeof(Member));
				for (uint32_t k = e->memCount; k <= w[2]; k++) { m[k].offset = -1; m[k].builtin = -1; m[k].location = -1; }
				e->mem = m;
				e->memCount = w[2] + 1;
			}
			Member *m = &e->mem[w[2]];
			uint32_t v = wc > 4 ? w[4] : 0;
			switch (w[3]) {
			case SpvDecorationOffset: m->offset = (int32_t)v; break;
			case SpvDecorationMatrixStride: m->matrixStride = (int32_t)v; break;
			case SpvDecorationRowMajor: m->rowMajor = true; break;
			case SpvDecorationBuiltIn: m->builtin = (int32_t)v; break;
			case SpvDecorationLocation: m->location = (int32_t)v; break;
			default: break;
			}
			break;
		}
		case SpvOpFunction: {
			Func *f = realloc(t->funcs, (t->funcCount + 1) * sizeof(Func));
			if (!f) { fail(t, "out of memory"); return false; }
			t->funcs = f;
			cur = &t->funcs[t->funcCount++];
			memset(cur, 0, sizeof(*cur));
			cur->id = w[2];
			cur->begin = w;
			blockCap = 0;
			if (w[2] < t->bound) { t->ids[w[2]].def = w; t->ids[w[2]].type = w[1]; }
			break;
		}
		case SpvOpFunctionEnd:
			if (cur) cur->end = w;
			cur = NULL;
			break;
		case SpvOpLabel:
			if (cur) {
				if (cur->blockCount == blockCap) {
					blockCap = blockCap ? blockCap * 2 : 16;
					Block *b = realloc(cur->blocks, blockCap * sizeof(Block));
					if (!b) { fail(t, "out of memory"); return false; }
					cur->blocks = b;
				}
				Block *b = &cur->blocks[cur->blockCount++];
				memset(b, 0, sizeof(*b));
				b->label = w[1];
				b->first = w + wc;
			}
			if (w[1] < t->bound) t->ids[w[1]].def = w;
			break;
		case SpvOpSelectionMerge:
			if (cur && cur->blockCount) { Block *b = &cur->blocks[cur->blockCount - 1]; b->mergeOp = op; b->merge = w[1]; }
			break;
		case SpvOpLoopMerge:
			if (cur && cur->blockCount) {
				Block *b = &cur->blocks[cur->blockCount - 1];
				b->mergeOp = op;
				b->merge = w[1];
				b->cont = w[2];
			}
			break;
		case SpvOpBranch: case SpvOpBranchConditional: case SpvOpSwitch: case SpvOpReturn: case SpvOpReturnValue:
		case SpvOpKill: case SpvOpUnreachable: case SpvOpTerminateInvocation:
			if (cur && cur->blockCount) cur->blocks[cur->blockCount - 1].term = w;
			break;
		case SpvOpEntryPoint: {
			SpvExecutionModel want = t->stage == VKV_SPIRV_VERTEX ? SpvExecutionModelVertex : SpvExecutionModelFragment;
			if (w[1] == (uint32_t)want && !t->entry) t->entry = w[2];
			break;
		}
		default:
			break;
		}
		/* result ids of everything else */
		if (op != SpvOpFunction && op != SpvOpLabel && op != SpvOpName && op != SpvOpMemberName) {
			uint32_t resType = 0, res = 0;
			switch (op) {
			case SpvOpTypeVoid: case SpvOpTypeBool: case SpvOpTypeInt: case SpvOpTypeFloat: case SpvOpTypeVector:
			case SpvOpTypeMatrix: case SpvOpTypeImage: case SpvOpTypeSampler: case SpvOpTypeSampledImage:
			case SpvOpTypeArray: case SpvOpTypeRuntimeArray: case SpvOpTypeStruct: case SpvOpTypePointer:
			case SpvOpTypeFunction: case SpvOpExtInstImport: case SpvOpString:
				res = w[1];
				break;
			case SpvOpDecorate: case SpvOpMemberDecorate: case SpvOpEntryPoint: case SpvOpExecutionMode:
			case SpvOpCapability: case SpvOpMemoryModel: case SpvOpSource: case SpvOpSourceExtension:
			case SpvOpExtension: case SpvOpLine: case SpvOpNoLine: case SpvOpStore: case SpvOpSelectionMerge:
			case SpvOpLoopMerge: case SpvOpBranch: case SpvOpBranchConditional: case SpvOpSwitch: case SpvOpReturn:
			case SpvOpReturnValue: case SpvOpKill: case SpvOpUnreachable: case SpvOpFunctionEnd: case SpvOpNop:
			case SpvOpCopyMemory: case SpvOpModuleProcessed: case SpvOpDecorateString: case SpvOpMemberDecorateString:
			case SpvOpSourceContinued: case SpvOpTerminateInvocation: case SpvOpDecorationGroup:
			case SpvOpGroupDecorate: case SpvOpGroupMemberDecorate: case SpvOpExecutionModeId:
				break;
			default:
				if (wc >= 3) { resType = w[1]; res = w[2]; }
				break;
			}
			if (res && res < t->bound) {
				t->ids[res].def = w;
				t->ids[res].type = resType;
			}
		}
		w += wc;
	}
	if (!t->entry) {
		fail(t, "module has no %s entry point", t->stage == VKV_SPIRV_VERTEX ? "vertex" : "fragment");
		return false;
	}
	return true;
}

/* --- Constants --- */
static void float_str(float f, char *out, size_t cap) {
	if (isnan(f)) { snprintf(out, cap, "(0.0f / 0.0f)"); return; }
	if (isinf(f)) { snprintf(out, cap, f > 0 ? "(1.0f / 0.0f)" : "(-1.0f / 0.0f)"); return; }
	char tmp[48];
	snprintf(tmp, sizeof(tmp), "%.9g", (double)f);
	if (!strpbrk(tmp, ".eEn")) strcat(tmp, ".0");
	if (f < 0 || (f == 0 && signbit(f))) snprintf(out, cap, "(%sf)", tmp);
	else snprintf(out, cap, "%sf", tmp);
}

static char *const_expr(Tr *t, uint32_t id);

/* A constant as a Cg expression. Composite constants become static const
 * globals, so they can be swizzled like any variable. */
static char *const_expr(Tr *t, uint32_t id) {
	Id *e = id_at(t, id);
	if (!e || !e->def) return xstrdup("0");
	if (e->expr) return xstrdup(e->expr);
	const uint32_t *d = e->def;
	uint32_t op = op_of(d), type = d[1];
	char ts[48];
	type_str(t, type, ts, sizeof(ts));
	char *r = NULL;
	switch (op) {
	case SpvOpConstantTrue: case SpvOpSpecConstantTrue: r = xstrdup("true"); break;
	case SpvOpConstantFalse: case SpvOpSpecConstantFalse: r = xstrdup("false"); break;
	case SpvOpConstant: case SpvOpSpecConstant: {
		const uint32_t *td = def_of(t, type);
		if (td && op_of(td) == SpvOpTypeFloat) {
			if (td[2] != 32) { unsupported(t, "%u-bit float constants are not supported", td[2]); return xstrdup("0.0f"); }
			float f;
			memcpy(&f, &d[3], 4);
			char s[64];
			float_str(f, s, sizeof(s));
			r = xstrdup(s);
		} else if (td && op_of(td) == SpvOpTypeInt) {
			int32_t v = (int32_t)d[3];
			r = v < 0 ? xprintf("(%d)", v) : xprintf("%d", v);
		} else {
			r = xstrdup("0");
		}
		break;
	}
	case SpvOpConstantNull: {
		const uint32_t *td = def_of(t, type);
		if (td && (op_of(td) == SpvOpTypeStruct || op_of(td) == SpvOpTypeArray)) {
			unsupported(t, "null constants of structs or arrays are not supported");
			return xstrdup("0");
		}
		r = is_bool_type(t, type) ? xprintf("%s(false)", ts) : is_float_type(t, type) ? xprintf("%s(0.0f)", ts)
		                                                                             : xprintf("%s(0)", ts);
		if (def_op(t, type) == SpvOpTypeBool) { free(r); r = xstrdup("false"); }
		else if (def_op(t, type) == SpvOpTypeFloat) { free(r); r = xstrdup("0.0f"); }
		else if (def_op(t, type) == SpvOpTypeInt) { free(r); r = xstrdup("0"); }
		break;
	}
	case SpvOpConstantComposite: case SpvOpSpecConstantComposite: {
		Sb b = { 0 };
		const uint32_t *td = def_of(t, type);
		char name[32];
		snprintf(name, sizeof(name), "_c%u", id);
		bool arr = td && op_of(td) == SpvOpTypeArray;
		if (td && op_of(td) == SpvOpTypeStruct) {
			unsupported(t, "constant structs are not supported");
			return xstrdup("0");
		}
		char suffix[32];
		array_suffix(t, type, suffix, sizeof(suffix));
		if (arr) {
			if (def_op(t, td[2]) == SpvOpTypeArray || def_op(t, td[2]) == SpvOpTypeStruct) {
				unsupported(t, "constant arrays of arrays or structs are not supported");
				return xstrdup("0");
			}
			sb_printf(&b, "static const %s %s%s = { ", ts, name, suffix);
		} else {
			sb_printf(&b, "static const %s %s = %s(", ts, name, ts);
		}
		for (uint32_t k = 3; k < wc_of(d); k++) {
			char *c = const_expr(t, d[k]);
			sb_printf(&b, "%s%s", k > 3 ? ", " : "", c ? c : "0");
			free(c);
		}
		sb_puts(&b, arr ? " };\n" : ");\n");
		if (b.p) sb_puts(&t->globals, b.p);
		free(b.p);
		r = xstrdup(name);
		break;
	}
	case SpvOpUndef: {
		r = is_bool_type(t, type) ? xprintf("%s(false)", ts) : is_float_type(t, type) ? xprintf("%s(0.0f)", ts)
		                                                                             : xprintf("%s(0)", ts);
		break;
	}
	default:
		unsupported(t, "constant opcode %u is not supported", op);
		return xstrdup("0");
	}
	e->expr = r ? xstrdup(r) : NULL;
	e->isConstExpr = true;
	return r;
}

static bool is_constant_op(uint32_t op) {
	return op == SpvOpConstant || op == SpvOpConstantTrue || op == SpvOpConstantFalse || op == SpvOpConstantNull ||
	       op == SpvOpConstantComposite || op == SpvOpSpecConstant || op == SpvOpSpecConstantTrue ||
	       op == SpvOpSpecConstantFalse || op == SpvOpSpecConstantComposite || op == SpvOpUndef;
}

/* The Cg text of a value id. Returned strings stay owned by the id table. */
static const char *val(Tr *t, uint32_t id) {
	Id *e = id_at(t, id);
	if (!e) return "0";
	if (e->expr) return e->expr;
	if (e->def && is_constant_op(op_of(e->def))) {
		free(const_expr(t, id));
		if (e->expr) return e->expr;
	}
	e->expr = xprintf("_%u", id);
	return e->expr ? e->expr : "0";
}

/* --- Uniforms --- */
static void add_uniform(Tr *t, const char *name, uint32_t offset, uint32_t stride, int32_t set, uint32_t binding,
                        VkvSpirvScalar scalar) {
	if (t->uniCount == t->uniCap) {
		uint32_t cap = t->uniCap ? t->uniCap * 2 : 16;
		VkvSpirvUniform *u = realloc(t->unis, cap * sizeof(*u));
		if (!u) { fail(t, "out of memory"); return; }
		t->unis = u;
		t->uniCap = cap;
	}
	VkvSpirvUniform *u = &t->unis[t->uniCount++];
	u->name = xstrdup(name);
	u->offset = offset;
	u->stride = stride;
	u->set = set;
	u->binding = binding;
	u->scalar = scalar;
}

static VkvSpirvScalar scalar_kind(Tr *t, uint32_t type) {
	const uint32_t *d = def_of(t, scalar_of(t, type));
	if (!d) return VKV_SPIRV_FLOAT;
	if (op_of(d) == SpvOpTypeFloat) return VKV_SPIRV_FLOAT;
	if (op_of(d) == SpvOpTypeInt) return d[3] ? VKV_SPIRV_INT : VKV_SPIRV_UINT;
	return VKV_SPIRV_UINT; /* bool: 0 or 1 */
}

typedef struct BlockCtx {
	int32_t  set;
	uint32_t binding;
	uint32_t structId;
	const char *blockName;
} BlockCtx;

/* Members with a std430 layout that vkv_spirv_lower_ssbo widened keep their real offset and stride. */
static void real_layout(Tr *t, uint32_t structId, uint32_t member, uint32_t *offset, uint32_t *stride) {
	for (uint32_t q = 0; q < t->strideCount; q++)
		if (t->strides[q].structId == structId && t->strides[q].member == member) {
			*offset = t->strides[q].offset;
			if (t->strides[q].stride) *stride = t->strides[q].stride;
		}
}

/* A leaf (scalar, vector or matrix, maybe an array of them) of a uniform
 * block. Float data is declared as a uniform the shader reads directly.
 * Integers, booleans and row-major matrices are fed as float uniforms and
 * converted into a static copy at the start of main. */
static void uniform_leaf(Tr *t, const BlockCtx *bc, uint32_t type, uint32_t arrayLen, uint32_t offset, uint32_t stride,
                         bool rowMajor, const char *name, const char *path) {
	const uint32_t *d = def_of(t, type);
	uint32_t op = d ? op_of(d) : 0;
	if (op != SpvOpTypeFloat && op != SpvOpTypeInt && op != SpvOpTypeBool && op != SpvOpTypeVector && op != SpvOpTypeMatrix) {
		unsupported(t, "uniform '%s' has a type that cannot be fed", path);
		return;
	}
	char ts[32];
	type_str(t, type, ts, sizeof(ts));
	uint32_t vec = op == SpvOpTypeVector ? d[3] : 1;
	char fts[32];
	if (op == SpvOpTypeMatrix) {
		uint32_t cols = d[3], rows = vec_size(t, d[2]);
		if (!is_float_type(t, type)) { unsupported(t, "integer matrix '%s' in a uniform block", path); return; }
		if (rowMajor) snprintf(fts, sizeof(fts), "float%ux%u", rows, cols);
		else snprintf(fts, sizeof(fts), "float%ux%u", cols, rows);
	} else if (vec > 1) {
		snprintf(fts, sizeof(fts), "float%u", vec);
	} else {
		snprintf(fts, sizeof(fts), "float");
	}
	char arr[16] = "";
	if (arrayLen) snprintf(arr, sizeof(arr), "[%u]", arrayLen);
	VkvSpirvScalar kind = scalar_kind(t, type);
	bool direct = kind == VKV_SPIRV_FLOAT && !(op == SpvOpTypeMatrix && rowMajor);
	if (direct && !strcmp(path, name)) {
		sb_printf(&t->globals, "uniform %s %s%s;\n", ts, name, arr);
		add_uniform(t, name, offset, arrayLen ? stride : 0, bc->set, bc->binding, kind);
		return;
	}
	char fed[128];
	snprintf(fed, sizeof(fed), "%s_u", name);
	sb_printf(&t->globals, "uniform %s %s%s;\n", fts, fed, arr);
	add_uniform(t, fed, offset, arrayLen ? stride : 0, bc->set, bc->binding, kind);
	uint32_t n = arrayLen ? arrayLen : 1;
	for (uint32_t k = 0; k < n; k++) {
		char dst[200], src[160];
		if (arrayLen) {
			snprintf(dst, sizeof(dst), "%s[%u]", path, k);
			snprintf(src, sizeof(src), "%s[%u]", fed, k);
		} else {
			snprintf(dst, sizeof(dst), "%s", path);
			snprintf(src, sizeof(src), "%s", fed);
		}
		if (op == SpvOpTypeMatrix && rowMajor) sb_printf(&t->init, "    %s = transpose(%s);\n", dst, src);
		else if (kind == VKV_SPIRV_FLOAT) sb_printf(&t->init, "    %s = %s;\n", dst, src);
		else if (op == SpvOpTypeBool || (op == SpvOpTypeVector && is_bool_type(t, type)))
			sb_printf(&t->init, "    %s = %s != 0.0f;\n", dst, src);
		else sb_printf(&t->init, "    %s = %s(%s);\n", dst, ts, src);
	}
}

/* Flattens a struct member of a uniform block into leaf uniforms that fill a
 * static copy named `path`. Arrays of structs become one uniform array per leaf. */
static void uniform_struct(Tr *t, const BlockCtx *bc, uint32_t type, uint32_t offset, const char *mangled,
                           const char *path, int depth) {
	if (depth > 8) { unsupported(t, "uniform structs nested too deep"); return; }
	const uint32_t *d = def_of(t, type);
	if (!d) return;
	if (op_of(d) == SpvOpTypeArray) {
		uint32_t n = array_len(t, type);
		uint32_t stride = (uint32_t)id_at(t, type)->arrayStride;
		uint32_t elem = d[2];
		for (uint32_t k = 0; k < n; k++) {
			char m2[160], p2[200];
			snprintf(m2, sizeof(m2), "%s_%u", mangled, k);
			snprintf(p2, sizeof(p2), "%s[%u]", path, k);
			if (def_op(t, elem) == SpvOpTypeStruct || def_op(t, elem) == SpvOpTypeArray)
				uniform_struct(t, bc, elem, offset + k * stride, m2, p2, depth + 1);
			else
				uniform_leaf(t, bc, elem, 0, offset + k * stride, 0, false, m2, p2);
		}
		return;
	}
	if (op_of(d) != SpvOpTypeStruct) { uniform_leaf(t, bc, type, 0, offset, 0, false, mangled, path); return; }
	Id *s = id_at(t, type);
	uint32_t members = wc_of(d) - 2;
	for (uint32_t m = 0; m < members; m++) {
		uint32_t mt = d[2 + m];
		uint32_t moff = m < s->memCount && s->mem[m].offset >= 0 ? (uint32_t)s->mem[m].offset : 0;
		bool rowMajor = m < s->memCount && s->mem[m].rowMajor;
		char mn[64];
		if (m < s->memCount && s->mem[m].name && s->mem[m].name[0]) clean_name(s->mem[m].name, mn, sizeof(mn));
		else snprintf(mn, sizeof(mn), "m%u", m);
		char m2[160], p2[200];
		snprintf(m2, sizeof(m2), "%s_%s", mangled, mn);
		snprintf(p2, sizeof(p2), "%s.%s", path, mn);
		const uint32_t *md = def_of(t, mt);
		if (md && (op_of(md) == SpvOpTypeStruct ||
		           (op_of(md) == SpvOpTypeArray && (def_op(t, md[2]) == SpvOpTypeStruct || def_op(t, md[2]) == SpvOpTypeArray)))) {
			uniform_struct(t, bc, mt, offset + moff, m2, p2, depth + 1);
		} else if (md && op_of(md) == SpvOpTypeArray) {
			uint32_t n = array_len(t, mt);
			uniform_leaf(t, bc, md[2], n, offset + moff, (uint32_t)id_at(t, mt)->arrayStride, rowMajor, m2, p2);
		} else {
			uniform_leaf(t, bc, mt, 0, offset + moff, 0, rowMajor, m2, p2);
		}
	}
}

/* Declares a uniform buffer or push constant block. Each top-level member is
 * written so that an access chain on the block can name it as <block>_<member>. */
static void declare_block(Tr *t, uint32_t var) {
	Id *v = id_at(t, var);
	uint32_t st = strip_ptr(t, v->type);
	const uint32_t *sd = def_of(t, st);
	if (!sd || op_of(sd) != SpvOpTypeStruct) {
		unsupported(t, "arrays of uniform blocks are not supported");
		return;
	}
	uint32_t storage = def_of(t, v->type)[2];
	BlockCtx bc;
	bc.set = storage == SpvStorageClassPushConstant ? -1 : v->set;
	bc.binding = storage == SpvStorageClassPushConstant ? 0 : (uint32_t)v->binding;
	bc.structId = st;
	char bn[64];
	if (v->name && v->name[0]) clean_name(v->name, bn, sizeof(bn));
	else if (id_at(t, st)->name && id_at(t, st)->name[0]) clean_name(id_at(t, st)->name, bn, sizeof(bn));
	else snprintf(bn, sizeof(bn), "ub%u", var);
	bc.blockName = bn;
	Id *s = id_at(t, st);
	uint32_t members = wc_of(sd) - 2;
	for (uint32_t m = 0; m < members; m++) {
		uint32_t mt = sd[2 + m];
		uint32_t moff = m < s->memCount && s->mem[m].offset >= 0 ? (uint32_t)s->mem[m].offset : 0;
		uint32_t stride = def_op(t, mt) == SpvOpTypeArray || def_op(t, mt) == SpvOpTypeRuntimeArray
		                  ? (uint32_t)id_at(t, mt)->arrayStride : 0;
		real_layout(t, st, m, &moff, &stride);
		bool rowMajor = m < s->memCount && s->mem[m].rowMajor;
		char mn[64];
		if (m < s->memCount && s->mem[m].name && s->mem[m].name[0]) clean_name(s->mem[m].name, mn, sizeof(mn));
		else snprintf(mn, sizeof(mn), "m%u", m);
		char name[160];
		snprintf(name, sizeof(name), "%s_%s", bn, mn);
		const uint32_t *md = def_of(t, mt);
		uint32_t elem = md && (op_of(md) == SpvOpTypeArray || op_of(md) == SpvOpTypeRuntimeArray) ? md[2] : 0;
		bool structy = md && (op_of(md) == SpvOpTypeStruct || (elem && (def_op(t, elem) == SpvOpTypeStruct ||
		                                                                 def_op(t, elem) == SpvOpTypeArray)));
		if (structy) {
			char ts[32], suffix[32];
			type_str(t, mt, ts, sizeof(ts));
			array_suffix(t, mt, suffix, sizeof(suffix));
			sb_printf(&t->globals, "static %s %s%s;\n", ts, name, suffix);
			if (md && op_of(md) == SpvOpTypeRuntimeArray) {
				uint32_t n = g_nativeArraySize;
				for (uint32_t k = 0; k < n; k++) {
					char m2[200], p2[200];
					snprintf(m2, sizeof(m2), "%s_%u", name, k);
					snprintf(p2, sizeof(p2), "%s[%u]", name, k);
					uniform_struct(t, &bc, elem, moff + k * stride, m2, p2, 1);
				}
			} else {
				uniform_struct(t, &bc, mt, moff, name, name, 0);
			}
			continue;
		}
		uint32_t n = md && op_of(md) == SpvOpTypeArray ? array_len(t, mt)
		             : md && op_of(md) == SpvOpTypeRuntimeArray ? g_nativeArraySize : 0;
		uint32_t leafType = n ? elem : mt;
		VkvSpirvScalar kind = scalar_kind(t, leafType);
		bool direct = kind == VKV_SPIRV_FLOAT && !(def_op(t, leafType) == SpvOpTypeMatrix && rowMajor);
		if (!direct) {
			char ts[32], suffix[32];
			type_str(t, leafType, ts, sizeof(ts));
			snprintf(suffix, sizeof(suffix), n ? "[%u]" : "", n);
			sb_printf(&t->globals, "static %s %s%s;\n", ts, name, suffix);
		}
		uniform_leaf(t, &bc, leafType, n, moff, stride, rowMajor, name, name);
	}
	free(v->expr);
	v->expr = xstrdup(bn);
}

/* --- Samplers --- */
static const char *sampler_type(Tr *t, uint32_t imageType) {
	const uint32_t *d = def_of(t, imageType);
	if (!d || op_of(d) != SpvOpTypeImage) return NULL;
	if (d[5]) { unsupported(t, "arrayed images are not supported"); return NULL; }
	if (d[6]) { unsupported(t, "multisampled images cannot be sampled on GXM"); return NULL; }
	switch (d[3]) {
	case SpvDim1D: return "sampler1D";
	case SpvDim2D: return "sampler2D";
	case SpvDimCube: return "samplerCUBE";
	case SpvDim3D: return "sampler3D";
	default: unsupported(t, "image dimension %u is not supported", d[3]); return NULL;
	}
}

/* Element count of a variable's array type, 1 for no array, 0 for an unsized one. */
static uint32_t var_array_len(Tr *t, uint32_t var) {
	uint32_t ty = strip_ptr(t, id_at(t, var)->type);
	const uint32_t *d = def_of(t, ty);
	if (d && op_of(d) == SpvOpTypeArray) return array_len(t, ty);
	if (d && op_of(d) == SpvOpTypeRuntimeArray) return 0;
	return 1;
}

static uint32_t var_elem_type(Tr *t, uint32_t var) {
	uint32_t ty = strip_ptr(t, id_at(t, var)->type);
	const uint32_t *d = def_of(t, ty);
	while (d && (op_of(d) == SpvOpTypeArray || op_of(d) == SpvOpTypeRuntimeArray)) { ty = d[2]; d = def_of(t, ty); }
	return ty;
}

/* The variable a loaded image, sampler or sampled image comes from, and the
 * array element (id of the index, 0 for element 0). */
static uint32_t trace_resource(Tr *t, uint32_t id, uint32_t *elem) {
	*elem = 0;
	for (int guard = 0; guard < 8; guard++) {
		const uint32_t *d = def_of(t, id);
		if (!d) return 0;
		switch (op_of(d)) {
		case SpvOpLoad: id = d[3]; break;
		case SpvOpCopyObject: id = d[3]; break;
		case SpvOpAccessChain: case SpvOpInBoundsAccessChain:
			if (wc_of(d) != 5) { unsupported(t, "multidimensional texture arrays are not supported"); return 0; }
			*elem = d[4];
			id = d[3];
			break;
		case SpvOpVariable: return id;
		default: return 0;
		}
	}
	return 0;
}

static int32_t find_combo(Tr *t, uint32_t imageVar, uint32_t samplerVar) {
	for (uint32_t i = 0; i < t->samplerCount; i++)
		if (t->samplers[i].imageVar == imageVar && t->samplers[i].samplerVar == samplerVar && t->samplers[i].element == 0)
			return (int32_t)i;
	return -1;
}

static int32_t add_combo(Tr *t, uint32_t imageVar, uint32_t samplerVar, uint32_t comboIndex) {
	int32_t have = find_combo(t, imageVar, samplerVar);
	if (have >= 0) return have;
	uint32_t images = var_array_len(t, imageVar);
	uint32_t samplers = samplerVar && samplerVar != NO_SAMPLER ? var_array_len(t, samplerVar) : 1;
	if (!images || !samplers) { fail(t, "texture arrays need one fixed size"); return -1; }
	uint32_t count = images > samplers ? images : samplers;
	const char *ct = sampler_type(t, var_elem_type(t, imageVar));
	if (!ct) return -1;
	uint32_t first = t->samplerCount;
	Id *iv = id_at(t, imageVar);
	for (uint32_t k = 0; k < count; k++) {
		if (t->samplerCount >= MAX_SAMPLERS) { fail(t, "more than %d textures in one stage", MAX_SAMPLERS); return -1; }
		Smp *s = &t->samplers[t->samplerCount++];
		memset(s, 0, sizeof(*s));
		s->imageVar = imageVar;
		s->samplerVar = samplerVar;
		s->element = k;
		s->count = count;
		s->first = first;
		s->cgType = ct;
		if (samplerVar) {
			if (count > 1) snprintf(s->name, sizeof(s->name), "vkv_tex%u_%u", comboIndex, k);
			else snprintf(s->name, sizeof(s->name), "vkv_tex%u", comboIndex);
			s->t.imageSet = iv->set;
			s->t.imageBinding = (uint32_t)iv->binding;
			s->t.imageElement = images > 1 ? k : 0;
			if (samplerVar == NO_SAMPLER) {
				s->t.samplerSet = -2;
			} else {
				Id *sv = id_at(t, samplerVar);
				s->t.samplerSet = sv->set;
				s->t.samplerBinding = (uint32_t)sv->binding;
				s->t.samplerElement = samplers > 1 ? k : 0;
			}
		} else {
			char base[64];
			if (iv->name && iv->name[0]) clean_name(iv->name, base, sizeof(base));
			else snprintf(base, sizeof(base), "smp%u", imageVar);
			if (count > 1) snprintf(s->name, sizeof(s->name), "%s_%u", base, k);
			else snprintf(s->name, sizeof(s->name), "%s", base);
			s->t.imageSet = iv->set;
			s->t.imageBinding = (uint32_t)iv->binding;
			s->t.imageElement = k;
			s->t.samplerSet = -1;
			s->fixedUnit = k == 0;
		}
	}
	return (int32_t)first;
}

/* Finds every texture the entry point can sample: image/sampler pairs from
 * OpSampledImage first, then combined samplers. */
static void collect_samplers(Tr *t) {
	uint32_t combos = 0;
	for (const uint32_t *w = t->words + 5; w < t->words + t->count; w += wc_of(w)) {
		if (op_of(w) != SpvOpSampledImage) continue;
		uint32_t ie, se;
		uint32_t iv = trace_resource(t, w[3], &ie), sv = trace_resource(t, w[4], &se);
		if (!iv || !sv) { unsupported(t, "could not find the image or sampler of a texture access"); continue; }
		if (find_combo(t, iv, sv) < 0) add_combo(t, iv, sv, combos++);
	}
	/* images read without a sampler: texelFetch and size queries on a texture2D */
	for (const uint32_t *w = t->words + 5; w < t->words + t->count; w += wc_of(w)) {
		uint32_t op = op_of(w);
		if (op != SpvOpImageFetch && op != SpvOpImageQuerySizeLod && op != SpvOpImageQuerySize &&
		    op != SpvOpImageQueryLevels)
			continue;
		if (def_op(t, w[3]) == SpvOpImage) continue; /* taken from a sampled image */
		uint32_t e;
		uint32_t iv = trace_resource(t, w[3], &e);
		if (!iv) { unsupported(t, "could not find the image of a texel fetch or size query"); continue; }
		if (def_op(t, var_elem_type(t, iv)) == SpvOpTypeSampledImage) continue;
		if (find_combo(t, iv, NO_SAMPLER) < 0) add_combo(t, iv, NO_SAMPLER, combos++);
	}
	for (const uint32_t *w = t->words + 5; w < t->words + t->count; w += wc_of(w)) {
		if (op_of(w) != SpvOpVariable || w[3] != SpvStorageClassUniformConstant) continue;
		if (def_op(t, var_elem_type(t, w[2])) != SpvOpTypeSampledImage) continue;
		if (find_combo(t, w[2], 0) >= 0) continue;
		uint32_t imgType = def_of(t, var_elem_type(t, w[2]))[2];
		Id *e = id_at(t, w[2]);
		(void)imgType;
		/* combined samplers carry their image type through the sampled image type */
		uint32_t images = var_array_len(t, w[2]);
		if (!images) { fail(t, "sampler arrays need one fixed size"); continue; }
		const char *ct = sampler_type(t, imgType);
		if (!ct) continue;
		uint32_t first = t->samplerCount;
		for (uint32_t k = 0; k < images; k++) {
			if (t->samplerCount >= MAX_SAMPLERS) { fail(t, "more than %d textures in one stage", MAX_SAMPLERS); return; }
			Smp *s = &t->samplers[t->samplerCount++];
			memset(s, 0, sizeof(*s));
			s->imageVar = w[2];
			s->element = k;
			s->count = images;
			s->first = first;
			s->cgType = ct;
			char base[64];
			if (e->name && e->name[0]) clean_name(e->name, base, sizeof(base));
			else snprintf(base, sizeof(base), "smp%u", w[2]);
			if (images > 1) snprintf(s->name, sizeof(s->name), "%s_%u", base, k);
			else snprintf(s->name, sizeof(s->name), "%s", base);
			s->t.imageSet = e->set;
			s->t.imageBinding = (uint32_t)e->binding;
			s->t.imageElement = k;
			s->t.samplerSet = -1;
			s->fixedUnit = k == 0;
		}
	}
	/* texture units: combined samplers keep their binding where possible */
	uint32_t used = 0;
	for (uint32_t i = 0; i < t->samplerCount; i++) {
		Smp *s = &t->samplers[i];
		if (s->fixedUnit && s->t.imageBinding < 16 && !(used & (1u << s->t.imageBinding))) {
			s->t.unit = s->t.imageBinding;
			used |= 1u << s->t.unit;
		} else {
			s->t.unit = 0xFFFFFFFFu;
		}
	}
	for (uint32_t i = 0; i < t->samplerCount; i++) {
		Smp *s = &t->samplers[i];
		if (s->t.unit != 0xFFFFFFFFu) continue;
		uint32_t u = 0;
		while (u < 16 && (used & (1u << u))) u++;
		if (u == 16) { fail(t, "more than 16 textures in one stage"); return; }
		s->t.unit = u;
		used |= 1u << u;
	}
	for (uint32_t i = 0; i < t->samplerCount; i++)
		sb_printf(&t->globals, "uniform %s %s : TEXUNIT%u;\n", t->samplers[i].cgType, t->samplers[i].name,
		          (unsigned)t->samplers[i].t.unit);
}

/* --- Stage interface --- */
static Io *new_io(Tr *t) {
	if (t->ioCount >= MAX_IO) { fail(t, "too many stage inputs or outputs"); return NULL; }
	Io *io = &t->io[t->ioCount++];
	memset(io, 0, sizeof(*io));
	io->member = ~0u;
	return io;
}

static bool io_semantic(Tr *t, Io *io, int32_t builtin, int32_t location, bool output, const char *what) {
	if (builtin >= 0) {
		switch (builtin) {
		case SpvBuiltInPosition:
			if (t->stage == VKV_SPIRV_VERTEX && output) { snprintf(io->sem, sizeof(io->sem), "POSITION"); return true; }
			break;
		case SpvBuiltInPointSize:
			if (t->stage == VKV_SPIRV_VERTEX && output) {
				snprintf(io->sem, sizeof(io->sem), "PSIZE");
				t->pointSize = true;
				return true;
			}
			break;
		case SpvBuiltInFragCoord:
			if (t->stage == VKV_SPIRV_FRAGMENT && !output) { snprintf(io->sem, sizeof(io->sem), "WPOS"); return true; }
			break;
		case SpvBuiltInFrontFacing:
			if (t->stage == VKV_SPIRV_FRAGMENT && !output) {
				snprintf(io->sem, sizeof(io->sem), "FACE");
				snprintf(io->type, sizeof(io->type), "float");
				io->frontFacing = true;
				return true;
			}
			break;
		default:
			break;
		}
		fail(t, "built-in %d ('%s') is not available on GXM", builtin, what);
		return false;
	}
	if (!strcmp(io->name, "vkv_vertex_index")) { snprintf(io->sem, sizeof(io->sem), "INDEX"); snprintf(io->type, sizeof(io->type), "int"); return true; }
	if (!strcmp(io->name, "vkv_instance_index")) { snprintf(io->sem, sizeof(io->sem), "INSTANCE"); snprintf(io->type, sizeof(io->type), "int"); return true; }
	if (!strcmp(io->name, "vkv_point_coord")) { snprintf(io->sem, sizeof(io->sem), "SPRITECOORD"); return true; }
	if (location < 0) { fail(t, "stage variable '%s' has no location", what); return false; }
	if (t->stage == VKV_SPIRV_VERTEX && !output) {
		snprintf(io->sem, sizeof(io->sem), "ATTR%d", location);
		return true;
	}
	if (t->stage == VKV_SPIRV_FRAGMENT && output) {
		if (location != 0) { fail(t, "GXM has one color output, '%s' uses location %d", what, location); return false; }
		snprintf(io->sem, sizeof(io->sem), "COLOR");
		return true;
	}
	if (location >= 10) {
		fail(t, "varying '%s' uses location %d, GXM has ten varying slots (locations 0 to 9)", what, location);
		return false;
	}
	snprintf(io->sem, sizeof(io->sem), "TEXCOORD%d", location);
	return true;
}

static void finish_io_type(Tr *t, Io *io, uint32_t type) {
	if (!io->type[0]) type_str(t, type, io->type, sizeof(io->type));
	bool varying = !(t->stage == VKV_SPIRV_VERTEX && !io->isOutput && !strncmp(io->sem, "ATTR", 4)) &&
	               strcmp(io->sem, "INDEX") && strcmp(io->sem, "INSTANCE") && !io->frontFacing;
	if (varying && !is_float_type(t, type) && def_op(t, scalar_of(t, type)) != 0) {
		io->isInt = true;
		uint32_t v = vec_size(t, type);
		if (v > 1) snprintf(io->floatType, sizeof(io->floatType), "float%u", v);
		else snprintf(io->floatType, sizeof(io->floatType), "float");
	}
}

static bool member_written(Tr *t, uint32_t var, uint32_t member);

static void declare_io(Tr *t, uint32_t var, bool output) {
	Id *v = id_at(t, var);
	uint32_t ty = strip_ptr(t, v->type);
	const uint32_t *td = def_of(t, ty);
	if (td && op_of(td) == SpvOpTypeStruct) {
		/* gl_PerVertex: one static per written built-in member */
		Id *s = id_at(t, ty);
		bool builtinBlock = s->memCount && s->mem[0].builtin >= 0;
		if (!builtinBlock) {
			/* a user interface block: every member is its own varying */
			int32_t loc = v->location;
			char bn[64];
			if (v->name && v->name[0]) clean_name(v->name, bn, sizeof(bn));
			else snprintf(bn, sizeof(bn), "blk");
			for (uint32_t m = 0; m < wc_of(td) - 2; m++) {
				uint32_t mt = td[2 + m];
				uint32_t mop = def_op(t, mt);
				if (mop == SpvOpTypeArray || mop == SpvOpTypeMatrix || mop == SpvOpTypeStruct) {
					fail(t, "arrays, matrices and structs inside interface blocks are not supported");
					return;
				}
				Io *io = new_io(t);
				if (!io) return;
				io->var = var;
				io->member = m;
				io->isOutput = output;
				char mn[64];
				if (m < s->memCount && s->mem[m].name && s->mem[m].name[0]) clean_name(s->mem[m].name, mn, sizeof(mn));
				else snprintf(mn, sizeof(mn), "m%u", m);
				snprintf(io->name, sizeof(io->name), "%s_%s_%u", bn, mn, var);
				int32_t ml = m < s->memCount && s->mem[m].location >= 0 ? s->mem[m].location : loc < 0 ? -1 : loc + (int32_t)m;
				if (!io_semantic(t, io, -1, ml, output, io->name)) return;
				finish_io_type(t, io, mt);
				char st[32];
				type_str(t, mt, st, sizeof(st));
				sb_printf(&t->globals, "static %s %s;\n", st, io->name);
			}
			return;
		}
		for (uint32_t m = 0; m < wc_of(td) - 2; m++) {
			if (!output || !member_written(t, var, m)) continue;
			if (s->mem[m].builtin == SpvBuiltInClipDistance || s->mem[m].builtin == SpvBuiltInCullDistance) {
				unsupported(t, "gl_ClipDistance and gl_CullDistance are not available on GXM");
				continue;
			}
			Io *io = new_io(t);
			if (!io) return;
			io->var = var;
			io->member = m;
			io->isOutput = output;
			snprintf(io->name, sizeof(io->name), "%s", s->mem[m].builtin == SpvBuiltInPosition ? "gl_Position"
			         : s->mem[m].builtin == SpvBuiltInPointSize ? "gl_PointSize" : "gl_member");
			if (!io_semantic(t, io, s->mem[m].builtin, -1, output, io->name)) return;
			finish_io_type(t, io, td[2 + m]);
			sb_printf(&t->globals, "static %s %s;\n", io->type, io->name);
		}
		return;
	}
	if (td && op_of(td) == SpvOpTypeArray) {
		fail(t, "arrays in the stage interface are not supported ('%s')", v->name ? v->name : "?");
		return;
	}
	Io *io = new_io(t);
	if (!io) return;
	io->var = var;
	io->isOutput = output;
	char nm[64];
	if (v->name && v->name[0]) clean_name(v->name, nm, sizeof(nm));
	else snprintf(nm, sizeof(nm), "io");
	if (v->name && (!strcmp(v->name, "vkv_vertex_index") || !strcmp(v->name, "vkv_instance_index") ||
	                !strcmp(v->name, "vkv_point_coord")))
		snprintf(io->name, sizeof(io->name), "%s", v->name);
	else
		snprintf(io->name, sizeof(io->name), "%s_%u", nm, var);
	if (!io_semantic(t, io, v->builtin, v->location, output, v->name ? v->name : io->name)) return;
	if (v->noPerspective) tr_log(t, VKV_SPIRV_LOG_WARN, "noperspective interpolation is not available, using perspective");
	finish_io_type(t, io, ty);
	char static_type[32];
	type_str(t, ty, static_type, sizeof(static_type));
	sb_printf(&t->globals, "static %s %s;\n", io->frontFacing ? "bool" : static_type, io->name);
	free(v->expr);
	v->expr = xstrdup(io->name);
}

/* --- Function bodies --- */
static void ind(Tr *t) { for (int i = 0; i < t->indent; i++) sb_puts(t->out, "    "); }

static Block *find_block(Func *f, uint32_t label) {
	for (uint32_t i = 0; i < f->blockCount; i++)
		if (f->blocks[i].label == label) return &f->blocks[i];
	return NULL;
}

/* Builds the Cg l-value for an access chain or variable. */
static char *ptr_expr(Tr *t, uint32_t ptr);

static char *chain_expr(Tr *t, const char *base, uint32_t baseType, const uint32_t *idx, uint32_t n) {
	Sb b = { 0 };
	sb_puts(&b, base);
	uint32_t ty = baseType;
	for (uint32_t k = 0; k < n; k++) {
		const uint32_t *d = def_of(t, ty);
		if (!d) break;
		uint32_t cv;
		switch (op_of(d)) {
		case SpvOpTypeStruct: {
			if (!const_u32(t, idx[k], &cv)) { fail(t, "dynamic struct index"); return sb_take(&b); }
			Id *s = id_at(t, ty);
			char mn[64];
			if (cv < s->memCount && s->mem[cv].name && s->mem[cv].name[0]) clean_name(s->mem[cv].name, mn, sizeof(mn));
			else snprintf(mn, sizeof(mn), "m%u", cv);
			sb_printf(&b, ".%s", mn);
			ty = d[2 + cv];
			break;
		}
		case SpvOpTypeVector:
			if (const_u32(t, idx[k], &cv) && cv < 4) sb_printf(&b, ".%c", "xyzw"[cv]);
			else sb_printf(&b, "[%s]", val(t, idx[k]));
			ty = d[2];
			break;
		case SpvOpTypeMatrix: case SpvOpTypeArray: case SpvOpTypeRuntimeArray:
			sb_printf(&b, "[%s]", val(t, idx[k]));
			ty = d[2];
			break;
		default:
			fail(t, "bad access chain");
			return sb_take(&b);
		}
	}
	return sb_take(&b);
}

static bool is_block_var(Tr *t, uint32_t var) {
	const uint32_t *d = def_of(t, var);
	if (!d || op_of(d) != SpvOpVariable) return false;
	return d[3] == SpvStorageClassUniform || d[3] == SpvStorageClassPushConstant || d[3] == SpvStorageClassStorageBuffer;
}

static char *ptr_expr(Tr *t, uint32_t ptr) {
	const uint32_t *d = def_of(t, ptr);
	if (!d) return xstrdup("0");
	uint32_t op = op_of(d);
	if (op == SpvOpVariable || op == SpvOpFunctionParameter) {
		Id *e = id_at(t, ptr);
		if (!e->expr) e->expr = ident(t, ptr, op == SpvOpVariable && d[3] == SpvStorageClassFunction ? "v" : "p");
		return xstrdup(e->expr);
	}
	if (op == SpvOpAccessChain || op == SpvOpInBoundsAccessChain) {
		uint32_t baseId = d[3];
		const uint32_t *idx = d + 4;
		uint32_t n = wc_of(d) - 4;
		uint32_t baseType = strip_ptr(t, id_at(t, baseId)->type);
		/* uniform blocks: the first index names a member, written as <block>_<member> */
		if (is_block_var(t, baseId) && n >= 1) {
			uint32_t m;
			if (!const_u32(t, idx[0], &m)) { fail(t, "dynamic uniform block member"); return xstrdup("0"); }
			Id *bv = id_at(t, baseId);
			Id *s = id_at(t, baseType);
			char mn[64];
			if (m < s->memCount && s->mem[m].name && s->mem[m].name[0]) clean_name(s->mem[m].name, mn, sizeof(mn));
			else snprintf(mn, sizeof(mn), "m%u", m);
			char base[160];
			snprintf(base, sizeof(base), "%s_%s", bv->expr ? bv->expr : "ub", mn);
			return chain_expr(t, base, def_of(t, baseType)[2 + m], idx + 1, n - 1);
		}
		/* gl_PerVertex and other interface blocks: each member is a static of its own */
		const uint32_t *bd = def_of(t, baseId);
		if (bd && op_of(bd) == SpvOpVariable && (bd[3] == SpvStorageClassInput || bd[3] == SpvStorageClassOutput) &&
		    def_op(t, baseType) == SpvOpTypeStruct) {
			uint32_t m;
			if (!const_u32(t, idx[0], &m)) { fail(t, "dynamic gl_PerVertex index"); return xstrdup("0"); }
			for (uint32_t i = 0; i < t->ioCount; i++)
				if (t->io[i].var == baseId && t->io[i].member == m)
					return chain_expr(t, t->io[i].name, def_of(t, baseType)[2 + m], idx + 1, n - 1);
			fail(t, "unknown gl_PerVertex member %u", m);
			return xstrdup("0");
		}
		char *base = ptr_expr(t, baseId);
		char *r = chain_expr(t, base, baseType, idx, n);
		free(base);
		return r;
	}
	if (op == SpvOpCopyObject) return ptr_expr(t, d[3]);
	unsupported(t, "pointer from opcode %u is not supported", op);
	return xstrdup("0");
}

static bool member_written(Tr *t, uint32_t var, uint32_t member) {
	for (const uint32_t *w = t->words + 5; w < t->words + t->count; w += wc_of(w)) {
		uint32_t op = op_of(w);
		if ((op == SpvOpAccessChain || op == SpvOpInBoundsAccessChain) && w[3] == var && wc_of(w) > 4) {
			uint32_t m;
			if (const_u32(t, w[4], &m) && m == member) return true;
		}
		if (op == SpvOpStore && w[1] == var) return true;
	}
	return false;
}

/* Value of type `type` taken from a vector or matrix component path. */
static const char k_swz[] = "xyzw";

static void emit_assign(Tr *t, uint32_t res, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void emit_assign(Tr *t, uint32_t res, const char *fmt, ...) {
	char *rhs;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	rhs = malloc((size_t)n + 1);
	if (!rhs) { t->out->oom = true; return; }
	va_start(ap, fmt);
	vsnprintf(rhs, (size_t)n + 1, fmt, ap);
	va_end(ap);
	Id *e = id_at(t, res);
	if (e && e->inl) {
		free(e->expr);
		e->expr = xprintf("(%s)", rhs);
		free(rhs);
		return;
	}
	ind(t);
	sb_printf(t->out, "%s = %s;\n", val(t, res), rhs);
	free(rhs);
}

static const char *binop_sym(uint32_t op) {
	switch (op) {
	case SpvOpFAdd: case SpvOpIAdd: return "+";
	case SpvOpFSub: case SpvOpISub: return "-";
	case SpvOpFMul: case SpvOpIMul: case SpvOpVectorTimesScalar: case SpvOpMatrixTimesScalar: return "*";
	case SpvOpFDiv: case SpvOpSDiv: case SpvOpUDiv: return "/";
	case SpvOpSMod: case SpvOpUMod: case SpvOpSRem: return "%";
	case SpvOpFOrdEqual: case SpvOpFUnordEqual: case SpvOpIEqual: case SpvOpLogicalEqual: return "==";
	case SpvOpFOrdNotEqual: case SpvOpFUnordNotEqual: case SpvOpINotEqual: case SpvOpLogicalNotEqual: return "!=";
	case SpvOpFOrdLessThan: case SpvOpFUnordLessThan: case SpvOpSLessThan: case SpvOpULessThan: return "<";
	case SpvOpFOrdGreaterThan: case SpvOpFUnordGreaterThan: case SpvOpSGreaterThan: case SpvOpUGreaterThan: return ">";
	case SpvOpFOrdLessThanEqual: case SpvOpFUnordLessThanEqual: case SpvOpSLessThanEqual: case SpvOpULessThanEqual: return "<=";
	case SpvOpFOrdGreaterThanEqual: case SpvOpFUnordGreaterThanEqual: case SpvOpSGreaterThanEqual:
	case SpvOpUGreaterThanEqual: return ">=";
	case SpvOpLogicalAnd: return "&&";
	case SpvOpLogicalOr: return "||";
	case SpvOpBitwiseAnd: return "&";
	case SpvOpBitwiseOr: return "|";
	case SpvOpBitwiseXor: return "^";
	case SpvOpShiftLeftLogical: return "<<";
	case SpvOpShiftRightLogical: case SpvOpShiftRightArithmetic: return ">>";
	default: return NULL;
	}
}

/* The texture info uniform, two entries per texture unit: (width, height, mip
 * levels, compare op + 1) and (LOD bias, weights for less, equal and greater
 * of the depth compare). Declared on first use. */
static void use_texinfo(Tr *t) { t->texInfo = true; }

/* Writes `tmpl` once per element of the sampler array behind `sampledImage`,
 * with $S replaced by the sampler name, $U by the index of its size entry in
 * vkv_texinfo and $V by the index of its LOD bias entry. A dynamic
 * index picks the element in a chain of ?:. */
static char *per_sampler(Tr *t, uint32_t sampledImage, const char *tmpl) {
	Id *si = id_at(t, sampledImage);
	if (!si || si->samplerRef < 0) { unsupported(t, "texture access through an unknown sampler"); return NULL; }
	const Smp *s0 = &t->samplers[si->samplerRef];
	uint32_t cv = 0, first = 0, last = s0->count;
	bool dynamic = s0->count > 1 && si->samplerElem && !const_u32(t, si->samplerElem, &cv);
	if (!dynamic) { if (cv >= s0->count) cv = 0; first = cv; last = cv + 1; }
	Sb b = { 0 };
	if (dynamic) sb_puts(&b, "(");
	for (uint32_t k = first; k < last; k++) {
		const Smp *sk = &t->samplers[s0->first + k];
		if (dynamic && k + 1 < last) sb_printf(&b, "(%s) == %u ? ", val(t, si->samplerElem), (unsigned)k);
		for (const char *p = tmpl; *p; p++) {
			if (p[0] == '$' && p[1] == 'S') { sb_puts(&b, sk->name); p++; }
			else if (p[0] == '$' && p[1] == 'U') { sb_printf(&b, "%u", (unsigned)sk->t.unit * 2); p++; }
			else if (p[0] == '$' && p[1] == 'V') { sb_printf(&b, "%u", (unsigned)sk->t.unit * 2 + 1); p++; }
			else sb_add(&b, p, 1);
		}
		if (dynamic && k + 1 < last) sb_puts(&b, " : ");
	}
	if (dynamic) sb_puts(&b, ")");
	return sb_take(&b);
}

static const Smp *sampler_of(Tr *t, uint32_t sampledImage) {
	Id *si = id_at(t, sampledImage);
	return si && si->samplerRef >= 0 ? &t->samplers[si->samplerRef] : NULL;
}

static const char *tex_fn(const Smp *s, const char *kind) {
	static char buf[32];
	const char *dim = !strcmp(s->cgType, "sampler1D") ? "1D" : !strcmp(s->cgType, "samplerCUBE") ? "CUBE"
	                  : !strcmp(s->cgType, "sampler3D") ? "3D" : "2D";
	snprintf(buf, sizeof(buf), "tex%s%s", dim, kind);
	return buf;
}

static uint32_t coord_need(const Smp *s) {
	return !strcmp(s->cgType, "sampler1D") ? 1 : !strcmp(s->cgType, "samplerCUBE") || !strcmp(s->cgType, "sampler3D") ? 3 : 2;
}

/* The image operands that follow a texture instruction. */
typedef struct ImgOps { uint32_t bias, lod, gx, gy, offset; bool bad; } ImgOps;

static ImgOps image_operands(Tr *t, const uint32_t *w, uint32_t at) {
	ImgOps o = { 0 };
	uint32_t wc = wc_of(w), mask = at < wc ? w[at++] : 0;
	if (mask & SpvImageOperandsBiasMask) o.bias = w[at++];
	if (mask & SpvImageOperandsLodMask) o.lod = w[at++];
	if (mask & SpvImageOperandsGradMask) { o.gx = w[at++]; o.gy = w[at++]; }
	if (mask & SpvImageOperandsConstOffsetMask) o.offset = w[at++];
	if (mask & SpvImageOperandsOffsetMask) o.offset = w[at++];
	if (mask & ~(SpvImageOperandsBiasMask | SpvImageOperandsLodMask | SpvImageOperandsGradMask |
	             SpvImageOperandsConstOffsetMask | SpvImageOperandsOffsetMask)) {
		unsupported(t, "image operands 0x%x are not supported", mask);
		o.bad = true;
	}
	return o;
}

static void emit_image_sample(Tr *t, const uint32_t *w) {
	uint32_t op = op_of(w);
	uint32_t res = w[2], si = w[3], coord = w[4];
	const Smp *s = sampler_of(t, si);
	if (!s) { unsupported(t, "texture access through an unknown sampler"); return; }
	bool proj = op == SpvOpImageSampleProjImplicitLod || op == SpvOpImageSampleProjExplicitLod ||
	            op == SpvOpImageSampleProjDrefImplicitLod || op == SpvOpImageSampleProjDrefExplicitLod;
	bool dref = op == SpvOpImageSampleDrefImplicitLod || op == SpvOpImageSampleDrefExplicitLod ||
	            op == SpvOpImageSampleProjDrefImplicitLod || op == SpvOpImageSampleProjDrefExplicitLod;
	if (!dref && !is_float_type(t, w[1])) { fail(t, "integer textures are not supported on GXM"); return; }
	ImgOps o = image_operands(t, w, dref ? 6 : 5);
	if (o.bad) return;
	uint32_t need = coord_need(s);
	uint32_t cn = vec_size(t, id_at(t, coord)->type);
	const char *c = val(t, coord);
	char cc[600];
	/* projective coordinates are divided here, the depth reference stays separate */
	if (proj) snprintf(cc, sizeof(cc), "(%s.%.*s / %s.%c)", c, (int)need, k_swz, c, k_swz[cn - 1]);
	else if (cn > need) snprintf(cc, sizeof(cc), "%s.%.*s", c, (int)need, k_swz);
	else snprintf(cc, sizeof(cc), "%s", c);
	if (o.offset) {
		if (need != 2) { unsupported(t, "texture offsets are only supported on 2D textures"); return; }
		use_texinfo(t);
		char *shifted = xprintf("(%s + float2(%s) / vkv_texinfo[$U].xy)", cc, val(t, o.offset));
		if (!shifted) { fail(t, "out of memory"); return; }
		snprintf(cc, sizeof(cc), "%s", shifted);
		free(shifted);
	}
	char call[1000];
	const char *pad = need == 1 ? ", 0.0f, 0.0f" : need == 2 ? ", 0.0f" : "";
	if (o.lod) snprintf(call, sizeof(call), "%s($S, float4(%s%s, %s))", tex_fn(s, "lod"), cc, pad, val(t, o.lod));
	else if (o.bias) snprintf(call, sizeof(call), "%s($S, float4(%s%s, %s))", tex_fn(s, "bias"), cc, pad, val(t, o.bias));
	else if (o.gx) snprintf(call, sizeof(call), "%s($S, %s, %s, %s)", tex_fn(s, "grad"), cc, val(t, o.gx), val(t, o.gy));
	else snprintf(call, sizeof(call), "%s($S, %s)", tex_fn(s, ""), cc);
	char tmpl[1300];
	if (dref) {
		/* GXM has no compare samplers: read the depth and compare with the sampler's op */
		use_texinfo(t);
		t->needCompare = true;
		char r[160];
		if (proj) snprintf(r, sizeof(r), "(%s / %s.%c)", val(t, w[5]), c, k_swz[cn - 1]);
		else snprintf(r, sizeof(r), "%s", val(t, w[5]));
		snprintf(tmpl, sizeof(tmpl), "vkv_compare(%s, %s.x, vkv_texinfo[$V].yzw)", r, call);
	} else {
		snprintf(tmpl, sizeof(tmpl), "%s", call);
	}
	char *e = per_sampler(t, si, tmpl);
	if (e) emit_assign(t, res, "%s", e);
	free(e);
}

/* texelFetch: GXM can only sample, so the texel center is sampled at that
 * level, minus the LOD bias of the sampler the texture is bound with. */
static void emit_image_fetch(Tr *t, const uint32_t *w) {
	uint32_t res = w[2], img = w[3], coord = w[4];
	const Smp *s = sampler_of(t, img);
	if (!s) { unsupported(t, "texel fetch from an unknown texture"); return; }
	if (coord_need(s) != 2) { unsupported(t, "texel fetch is only supported on 2D textures"); return; }
	if (!is_float_type(t, w[1])) { fail(t, "integer textures are not supported on GXM"); return; }
	ImgOps o = image_operands(t, w, 5);
	if (o.bad) return;
	use_texinfo(t);
	char lod[96], pos[256];
	snprintf(lod, sizeof(lod), "%s", o.lod ? val(t, o.lod) : "0");
	if (o.offset) snprintf(pos, sizeof(pos), "float2(%s + %s)", val(t, coord), val(t, o.offset));
	else snprintf(pos, sizeof(pos), "float2(%s)", val(t, coord));
	char tmpl[700];
	snprintf(tmpl, sizeof(tmpl),
	         "tex2Dlod($S, float4((%s + 0.5f) / max(floor(vkv_texinfo[$U].xy / exp2(float(%s))), 1.0f), 0.0f, "
	         "float(%s) - vkv_texinfo[$V].x))",
	         pos, lod, lod);
	char *e = per_sampler(t, img, tmpl);
	if (e) emit_assign(t, res, "%s", e);
	free(e);
}

static void emit_image_query(Tr *t, const uint32_t *w) {
	uint32_t op = op_of(w), res = w[2], img = w[3];
	if (!sampler_of(t, img)) { unsupported(t, "size query of an unknown texture"); return; }
	use_texinfo(t);
	char tmpl[400];
	if (op == SpvOpImageQueryLevels) {
		snprintf(tmpl, sizeof(tmpl), "int(vkv_texinfo[$U].z)");
	} else {
		const char *lod = op == SpvOpImageQuerySizeLod ? val(t, w[4]) : "0";
		uint32_t n = vec_size(t, w[1]);
		if (n == 1)
			snprintf(tmpl, sizeof(tmpl), "int(max(floor(vkv_texinfo[$U].x / exp2(float(%s))), 1.0f))", lod);
		else if (n == 2)
			snprintf(tmpl, sizeof(tmpl), "int2(max(floor(vkv_texinfo[$U].xy / exp2(float(%s))), 1.0f))", lod);
		else {
			unsupported(t, "size queries of array or 3D textures are not supported");
			return;
		}
	}
	char *e = per_sampler(t, img, tmpl);
	if (e) emit_assign(t, res, "%s", e);
	free(e);
}

static void emit_ext(Tr *t, const uint32_t *w) {
	uint32_t res = w[2], inst = w[4], n = wc_of(w) - 5;
	const uint32_t *a = w + 5;
	if (w[3] != t->glsl450) { unsupported(t, "extended instruction set is not supported"); return; }
	const char *fn = NULL;
	switch (inst) {
	case GLSLstd450Round: case GLSLstd450RoundEven: fn = "round"; break;
	case GLSLstd450Trunc: fn = "trunc"; break;
	case GLSLstd450FAbs: case GLSLstd450SAbs: fn = "abs"; break;
	case GLSLstd450FSign: case GLSLstd450SSign: fn = "sign"; break;
	case GLSLstd450Floor: fn = "floor"; break;
	case GLSLstd450Ceil: fn = "ceil"; break;
	case GLSLstd450Fract: fn = "frac"; break;
	case GLSLstd450Radians: fn = "radians"; break;
	case GLSLstd450Degrees: fn = "degrees"; break;
	case GLSLstd450Sin: fn = "sin"; break;
	case GLSLstd450Cos: fn = "cos"; break;
	case GLSLstd450Tan: fn = "tan"; break;
	case GLSLstd450Asin: fn = "asin"; break;
	case GLSLstd450Acos: fn = "acos"; break;
	case GLSLstd450Atan: fn = "atan"; break;
	case GLSLstd450Sinh: fn = "sinh"; break;
	case GLSLstd450Cosh: fn = "cosh"; break;
	case GLSLstd450Tanh: fn = "tanh"; break;
	case GLSLstd450Atan2: fn = "atan2"; break;
	case GLSLstd450Pow: fn = "pow"; break;
	case GLSLstd450Exp: fn = "exp"; break;
	case GLSLstd450Log: fn = "log"; break;
	case GLSLstd450Exp2: fn = "exp2"; break;
	case GLSLstd450Log2: fn = "log2"; break;
	case GLSLstd450Sqrt: fn = "sqrt"; break;
	case GLSLstd450InverseSqrt: fn = "rsqrt"; break;
	case GLSLstd450Determinant: fn = "determinant"; break;
	case GLSLstd450FMin: case GLSLstd450UMin: case GLSLstd450SMin: case GLSLstd450NMin: fn = "min"; break;
	case GLSLstd450FMax: case GLSLstd450UMax: case GLSLstd450SMax: case GLSLstd450NMax: fn = "max"; break;
	case GLSLstd450FClamp: case GLSLstd450UClamp: case GLSLstd450SClamp: case GLSLstd450NClamp: fn = "clamp"; break;
	case GLSLstd450FMix: fn = "lerp"; break;
	case GLSLstd450Step: fn = "step"; break;
	case GLSLstd450SmoothStep: fn = "smoothstep"; break;
	case GLSLstd450Length: fn = "length"; break;
	case GLSLstd450Distance: fn = "distance"; break;
	case GLSLstd450Cross: fn = "cross"; break;
	case GLSLstd450Normalize: fn = "normalize"; break;
	case GLSLstd450FaceForward: fn = "faceforward"; break;
	case GLSLstd450Reflect: fn = "reflect"; break;
	case GLSLstd450Refract: fn = "refract"; break;
	case GLSLstd450Ldexp: fn = "ldexp"; break;
	case GLSLstd450Fma:
		emit_assign(t, res, "(%s * %s + %s)", val(t, a[0]), val(t, a[1]), val(t, a[2]));
		return;
	case GLSLstd450Asinh: emit_assign(t, res, "log(%s + sqrt(%s * %s + 1.0f))", val(t, a[0]), val(t, a[0]), val(t, a[0])); return;
	case GLSLstd450Acosh: emit_assign(t, res, "log(%s + sqrt(%s * %s - 1.0f))", val(t, a[0]), val(t, a[0]), val(t, a[0])); return;
	case GLSLstd450Atanh: emit_assign(t, res, "(0.5f * log((1.0f + %s) / (1.0f - %s)))", val(t, a[0]), val(t, a[0])); return;
	default:
		unsupported(t, "GLSL.std.450 instruction %u is not supported", inst);
		return;
	}
	/* mix with a boolean selector is a select */
	if (inst == GLSLstd450FMix && is_bool_type(t, id_at(t, a[2])->type)) {
		emit_assign(t, res, "(%s ? %s : %s)", val(t, a[2]), val(t, a[1]), val(t, a[0]));
		return;
	}
	Sb b = { 0 };
	for (uint32_t k = 0; k < n; k++) sb_printf(&b, "%s%s", k ? ", " : "", val(t, a[k]));
	/* scalar arguments next to vectors are widened so overloads match */
	if ((inst == GLSLstd450FMix || inst == GLSLstd450FClamp || inst == GLSLstd450FMin || inst == GLSLstd450FMax ||
	     inst == GLSLstd450Step || inst == GLSLstd450SmoothStep || inst == GLSLstd450SClamp || inst == GLSLstd450SMin ||
	     inst == GLSLstd450SMax) && vec_size(t, w[1]) > 1) {
		free(b.p);
		b = (Sb){ 0 };
		char rt[32];
		type_str(t, w[1], rt, sizeof(rt));
		for (uint32_t k = 0; k < n; k++) {
			bool scalar = vec_size(t, id_at(t, a[k])->type) == 1 && def_op(t, id_at(t, a[k])->type) != SpvOpTypeVector;
			if (scalar) sb_printf(&b, "%s%s(%s)", k ? ", " : "", rt, val(t, a[k]));
			else sb_printf(&b, "%s%s", k ? ", " : "", val(t, a[k]));
		}
	}
	emit_assign(t, res, "%s(%s)", fn, b.p ? b.p : "");
	free(b.p);
}

static bool has_phi_from(Tr *t, uint32_t from, uint32_t to) {
	Block *b = find_block(t->fn, to);
	if (!b) return false;
	for (const uint32_t *w = b->first; w < b->term && op_of(w) == SpvOpPhi; w += wc_of(w))
		for (uint32_t k = 3; k + 1 < wc_of(w); k += 2)
			if (w[k + 1] == from) return true;
	return false;
}

static void emit_phis_for(Tr *t, uint32_t from, uint32_t to) {
	Block *b = find_block(t->fn, to);
	if (!b) return;
	/* two steps, in case one phi reads another of the same block */
	int count = 0;
	for (const uint32_t *w = b->first; w < b->term && op_of(w) == SpvOpPhi; w += wc_of(w)) {
		for (uint32_t k = 3; k + 1 < wc_of(w); k += 2)
			if (w[k + 1] == from) {
				char ts[32];
				type_str(t, w[1], ts, sizeof(ts));
				ind(t);
				sb_printf(t->out, "%s _p%u_%d = %s;\n", ts, w[2], count, val(t, w[k]));
			}
		count++;
	}
	count = 0;
	for (const uint32_t *w = b->first; w < b->term && op_of(w) == SpvOpPhi; w += wc_of(w)) {
		for (uint32_t k = 3; k + 1 < wc_of(w); k += 2)
			if (w[k + 1] == from) { ind(t); sb_printf(t->out, "%s = _p%u_%d;\n", val(t, w[2]), w[2], count); }
		count++;
	}
}

static void emit_range(Tr *t, uint32_t label, uint32_t stop, uint32_t from, int depth);

static Loop *cur_loop(Tr *t) { return t->loopDepth ? &t->loops[t->loopDepth - 1] : NULL; }

static void emit_continue_code(Tr *t, uint32_t from);

/* A jump from block `from` to `to` while emitting up to `stop`. */
static void emit_goto(Tr *t, uint32_t from, uint32_t to, uint32_t stop, int depth) {
	emit_phis_for(t, from, to);
	Loop *l = cur_loop(t);
	if (to == stop) return;
	if (l && to == l->merge) { ind(t); sb_puts(t->out, "break;\n"); return; }
	if (l && to == l->cont) { emit_continue_code(t, from); return; }
	if (l && to == l->header) { ind(t); sb_puts(t->out, "continue;\n"); return; }
	emit_range(t, to, stop, from, depth + 1);
}

/* The continue block runs before every next iteration, so it is written
 * out wherever the loop continues. */
static void emit_continue_code(Tr *t, uint32_t from) {
	Loop *l = cur_loop(t);
	(void)from;
	if (l->cont == l->header) { ind(t); sb_puts(t->out, "continue;\n"); return; }
	emit_range(t, l->cont, l->header, from, 1);
	ind(t);
	sb_puts(t->out, "continue;\n");
}

static void emit_inst(Tr *t, const uint32_t *w);

static void emit_block_body(Tr *t, Block *b) {
	for (const uint32_t *w = b->first; w < b->term; w += wc_of(w)) {
		uint32_t op = op_of(w);
		if (op == SpvOpPhi || op == SpvOpSelectionMerge || op == SpvOpLoopMerge || op == SpvOpLine || op == SpvOpNoLine)
			continue;
		emit_inst(t, w);
		if (t->status != TR_OK) return;
	}
}

/* Writes an else arm. When the arm is nothing but another if/else chain it
 * becomes "else if", which keeps chains of tests flat for the GXM compiler. */
static void emit_else(Tr *t, uint32_t from, uint32_t target, uint32_t merge, int depth) {
	Sb arm = { 0 };
	Sb *saved = t->out;
	t->out = &arm;
	t->indent++;
	emit_goto(t, from, target, merge, depth);
	t->indent--;
	t->out = saved;
	if (arm.oom) { t->out->oom = true; free(arm.p); return; }
	/* lines at the arm's own indentation may only belong to one if/else chain */
	size_t base = (size_t)(t->indent + 1) * 4;
	bool chain = arm.p && arm.n > base && !strncmp(arm.p + base, "if (", 4);
	for (const char *l = arm.p; chain && l && *l;) {
		const char *nl = strchr(l, '\n');
		size_t len = nl ? (size_t)(nl - l) : strlen(l);
		size_t sp = 0;
		while (sp < len && l[sp] == ' ') sp++;
		if (sp == base && strncmp(l + sp, "if (", 4) && strncmp(l + sp, "{", 1) && strncmp(l + sp, "}", 1) &&
		    strncmp(l + sp, "else", 4))
			chain = false;
		if (sp < base && len) chain = false;
		l = nl ? nl + 1 : NULL;
	}
	/* only the first "if" may start the chain; a second if at this level is a new statement */
	if (chain) {
		int ifs = 0;
		for (const char *l = arm.p; l && *l;) {
			const char *nl = strchr(l, '\n');
			size_t sp = 0;
			while (l[sp] == ' ') sp++;
			if (sp == base && !strncmp(l + sp, "if (", 4)) ifs++;
			l = nl ? nl + 1 : NULL;
		}
		chain = ifs == 1;
	}
	if (chain) {
		ind(t);
		sb_puts(t->out, "else ");
		/* the arm one level less indented, its first line right after "else " */
		bool first = true;
		for (const char *l = arm.p; l && *l;) {
			const char *nl = strchr(l, '\n');
			size_t len = nl ? (size_t)(nl - l) : strlen(l);
			size_t skip = first ? base : (len >= 4 ? 4 : len);
			sb_add(t->out, l + skip, len - skip);
			sb_puts(t->out, "\n");
			first = false;
			l = nl ? nl + 1 : NULL;
		}
	} else {
		ind(t); sb_puts(t->out, "else\n");
		ind(t); sb_puts(t->out, "{\n");
		if (arm.p) sb_puts(t->out, arm.p);
		ind(t); sb_puts(t->out, "}\n");
	}
	free(arm.p);
}

/* Carries on at `target` after an if or a loop has been written. */
static void emit_after(Tr *t, uint32_t target, uint32_t stop, uint32_t from, int depth) {
	if (target == stop) return;
	Loop *l = cur_loop(t);
	if (l && target == l->merge) { ind(t); sb_puts(t->out, "break;\n"); }
	else if (l && target == l->cont) emit_continue_code(t, from);
	else if (l && target == l->header) { ind(t); sb_puts(t->out, "continue;\n"); }
	else emit_range(t, target, stop, from, depth + 1);
}

static void emit_range(Tr *t, uint32_t label, uint32_t stop, uint32_t from, int depth) {
	(void)from;
	if (depth > 200) { unsupported(t, "control flow nested too deep"); return; }
	Block *b = find_block(t->fn, label);
	if (!b || !b->term) { fail(t, "branch to a missing block"); return; }
	if (t->status != TR_OK) return;
	bool loop = b->mergeOp == SpvOpLoopMerge;
	if (loop) {
		if (t->loopDepth >= MAX_LOOPS) { unsupported(t, "loops nested too deep"); return; }
		t->loops[t->loopDepth++] = (Loop){ b->label, b->merge, b->cont };
		ind(t);
		sb_puts(t->out, "for (;;)\n");
		ind(t);
		sb_puts(t->out, "{\n");
		t->indent++;
	}
	emit_block_body(t, b);
	const uint32_t *term = b->term;
	switch (op_of(term)) {
	case SpvOpBranch:
		emit_goto(t, b->label, term[1], loop ? 0 : stop, depth);
		break;
	case SpvOpBranchConditional: {
		uint32_t tl = term[2], fl = term[3];
		uint32_t merge = b->mergeOp == SpvOpSelectionMerge ? b->merge : (loop ? 0 : stop);
		/* an arm that goes straight to the merge only needs writing when it feeds a phi there */
		bool tEmpty = tl == merge && !has_phi_from(t, b->label, tl);
		bool fEmpty = fl == merge && !has_phi_from(t, b->label, fl);
		if (tEmpty && fEmpty) {
			/* nothing to do in either arm */
		} else if (tEmpty) {
			ind(t); sb_printf(t->out, "if (!(%s))\n", val(t, term[1]));
			ind(t); sb_puts(t->out, "{\n");
			t->indent++; emit_goto(t, b->label, fl, merge, depth); t->indent--;
			ind(t); sb_puts(t->out, "}\n");
		} else {
			ind(t); sb_printf(t->out, "if (%s)\n", val(t, term[1]));
			ind(t); sb_puts(t->out, "{\n");
			t->indent++; emit_goto(t, b->label, tl, merge, depth); t->indent--;
			ind(t); sb_puts(t->out, "}\n");
			if (!fEmpty) emit_else(t, b->label, fl, merge, depth);
		}
		if (b->mergeOp == SpvOpSelectionMerge) emit_after(t, merge, stop, b->label, depth);
		break;
	}
	case SpvOpReturn: ind(t); sb_puts(t->out, "return;\n"); break;
	case SpvOpReturnValue: ind(t); sb_printf(t->out, "return %s;\n", val(t, term[1])); break;
	case SpvOpKill: case SpvOpTerminateInvocation: ind(t); sb_puts(t->out, "discard;\n"); break;
	case SpvOpUnreachable: break;
	case SpvOpSwitch: {
		/* an if/else chain: Cg on GXM has no switch statement */
		uint32_t merge = b->mergeOp == SpvOpSelectionMerge ? b->merge : stop;
		uint32_t def = term[2], n = (wc_of(term) - 3) / 2;
		if (scalar_of(t, id_at(t, term[1])->type) && def_op(t, id_at(t, term[1])->type) == SpvOpTypeInt &&
		    def_of(t, id_at(t, term[1])->type)[2] != 32) {
			unsupported(t, "switch on a non 32-bit value");
			break;
		}
		bool firstCase = true;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t target = term[4 + 2 * i];
			bool seen = target == def;
			for (uint32_t j = 0; j < i && !seen; j++) seen = term[4 + 2 * j] == target;
			if (seen) continue;
			if (target == merge && !has_phi_from(t, b->label, target)) continue;
			Sb cond = { 0 };
			for (uint32_t j = i; j < n; j++)
				if (term[4 + 2 * j] == target)
					sb_printf(&cond, "%s%s == %d", cond.n ? " || " : "", val(t, term[1]), (int32_t)term[3 + 2 * j]);
			ind(t); sb_printf(t->out, "%sif (%s)\n", firstCase ? "" : "else ", cond.p ? cond.p : "false");
			free(cond.p);
			ind(t); sb_puts(t->out, "{\n");
			t->indent++; emit_goto(t, b->label, target, merge, depth); t->indent--;
			ind(t); sb_puts(t->out, "}\n");
			firstCase = false;
		}
		if (def != merge || has_phi_from(t, b->label, def)) {
			if (!firstCase) { ind(t); sb_puts(t->out, "else\n"); }
			ind(t); sb_puts(t->out, "{\n");
			t->indent++; emit_goto(t, b->label, def, merge, depth); t->indent--;
			ind(t); sb_puts(t->out, "}\n");
		}
		if (b->mergeOp == SpvOpSelectionMerge) emit_after(t, merge, stop, b->label, depth);
		break;
	}
	default: fail(t, "unknown block terminator"); break;
	}
	if (loop) {
		t->indent--;
		ind(t);
		sb_puts(t->out, "}\n");
		t->loopDepth--;
		emit_after(t, b->merge, stop, b->label, depth);
	}
}

static void emit_inst(Tr *t, const uint32_t *w) {
	uint32_t op = op_of(w), wc = wc_of(w);
	uint32_t rt = wc > 1 ? w[1] : 0, res = wc > 2 ? w[2] : 0;
	const char *sym = binop_sym(op);
	if (sym) {
		bool isDiv = op == SpvOpSMod || op == SpvOpUMod || op == SpvOpSRem || op == SpvOpSDiv || op == SpvOpUDiv;
		if (isDiv && (op == SpvOpSMod || op == SpvOpUMod || op == SpvOpSRem))
			tr_log(t, VKV_SPIRV_LOG_WARN, "integer remainder left in the shader, the GXM compiler may reject it");
		emit_assign(t, res, "(%s %s %s)", val(t, w[3]), sym, val(t, w[4]));
		return;
	}
	switch (op) {
	case SpvOpNop: case SpvOpLine: case SpvOpNoLine: case SpvOpSelectionMerge: case SpvOpLoopMerge:
		return;
	case SpvOpVariable: {
		/* function variables are declared at the top */
		if (wc > 4) {
			char *p = ptr_expr(t, res);
			ind(t);
			sb_printf(t->out, "%s = %s;\n", p, val(t, w[4]));
			free(p);
		}
		return;
	}
	case SpvOpLoad: {
		Id *pe = id_at(t, w[3]);
		uint32_t pointee = strip_ptr(t, pe->type);
		uint32_t pop = def_op(t, pointee);
		if (pop == SpvOpTypeSampledImage || pop == SpvOpTypeImage || pop == SpvOpTypeSampler) {
			/* resolved by OpSampledImage or used as a combined sampler */
			uint32_t elem;
			uint32_t var = trace_resource(t, res, &elem);
			Id *e = id_at(t, res);
			e->samplerRef = pop == SpvOpTypeSampledImage ? find_combo(t, var, 0)
			                : pop == SpvOpTypeImage ? find_combo(t, var, NO_SAMPLER) : -1;
			e->samplerElem = elem;
			return;
		}
		char *p = ptr_expr(t, w[3]);
		emit_assign(t, res, "%s", p);
		free(p);
		return;
	}
	case SpvOpStore: {
		char *p = ptr_expr(t, w[1]);
		ind(t);
		sb_printf(t->out, "%s = %s;\n", p, val(t, w[2]));
		free(p);
		return;
	}
	case SpvOpCopyMemory: {
		char *a = ptr_expr(t, w[1]), *b = ptr_expr(t, w[2]);
		ind(t);
		sb_printf(t->out, "%s = %s;\n", a, b);
		free(a);
		free(b);
		return;
	}
	case SpvOpAccessChain: case SpvOpInBoundsAccessChain: {
		uint32_t pointee = strip_ptr(t, rt);
		uint32_t pop = def_op(t, pointee);
		if (pop == SpvOpTypeSampledImage || pop == SpvOpTypeImage || pop == SpvOpTypeSampler) return;
		/* pointers are not values in Cg: the chain is rebuilt where it is used */
		return;
	}
	case SpvOpSampledImage: {
		uint32_t ie, se;
		uint32_t iv = trace_resource(t, w[3], &ie), sv = trace_resource(t, w[4], &se);
		Id *e = id_at(t, res);
		e->samplerRef = find_combo(t, iv, sv);
		e->samplerElem = ie ? ie : se;
		return;
	}
	case SpvOpImageSampleImplicitLod: case SpvOpImageSampleExplicitLod: case SpvOpImageSampleProjImplicitLod:
	case SpvOpImageSampleProjExplicitLod: case SpvOpImageSampleDrefImplicitLod: case SpvOpImageSampleDrefExplicitLod:
	case SpvOpImageSampleProjDrefImplicitLod: case SpvOpImageSampleProjDrefExplicitLod:
		emit_image_sample(t, w);
		return;
	case SpvOpImage: {
		/* the image of a sampled image: it keeps pointing at the same texture unit */
		Id *e = id_at(t, res), *from = id_at(t, w[3]);
		e->samplerRef = from->samplerRef;
		e->samplerElem = from->samplerElem;
		return;
	}
	case SpvOpImageFetch: emit_image_fetch(t, w); return;
	case SpvOpImageQuerySizeLod: case SpvOpImageQuerySize: case SpvOpImageQueryLevels: emit_image_query(t, w); return;
	case SpvOpOuterProduct: {
		/* column j is c * r[j]; Cg rows are GLSL columns */
		char ts[32];
		type_str(t, rt, ts, sizeof(ts));
		uint32_t n = vec_size(t, id_at(t, w[4])->type);
		Sb b = { 0 };
		for (uint32_t k = 0; k < n; k++) sb_printf(&b, "%s%s * %s.%c", k ? ", " : "", val(t, w[3]), val(t, w[4]), k_swz[k]);
		emit_assign(t, res, "%s(%s)", ts, b.p ? b.p : "");
		free(b.p);
		return;
	}
	case SpvOpArrayLength: emit_assign(t, res, "%u", (unsigned)g_nativeArraySize); return;
	case SpvOpCopyObject:
		if (def_op(t, rt) == SpvOpTypePointer) return;
		emit_assign(t, res, "%s", val(t, w[3]));
		return;
	case SpvOpFNegate: case SpvOpSNegate: emit_assign(t, res, "(-%s)", val(t, w[3])); return;
	case SpvOpNot: emit_assign(t, res, "(~%s)", val(t, w[3])); return;
	case SpvOpLogicalNot: emit_assign(t, res, "(!%s)", val(t, w[3])); return;
	case SpvOpFMod:
		emit_assign(t, res, "(%s - %s * floor(%s / %s))", val(t, w[3]), val(t, w[4]), val(t, w[3]), val(t, w[4]));
		return;
	case SpvOpFRem: emit_assign(t, res, "fmod(%s, %s)", val(t, w[3]), val(t, w[4])); return;
	case SpvOpDot: emit_assign(t, res, "dot(%s, %s)", val(t, w[3]), val(t, w[4])); return;
	case SpvOpMatrixTimesVector: emit_assign(t, res, "mul(%s, %s)", val(t, w[4]), val(t, w[3])); return;
	case SpvOpVectorTimesMatrix: emit_assign(t, res, "mul(%s, %s)", val(t, w[4]), val(t, w[3])); return;
	case SpvOpMatrixTimesMatrix: emit_assign(t, res, "mul(%s, %s)", val(t, w[4]), val(t, w[3])); return;
	case SpvOpTranspose: emit_assign(t, res, "transpose(%s)", val(t, w[3])); return;
	case SpvOpDPdx: case SpvOpDPdxFine: case SpvOpDPdxCoarse: emit_assign(t, res, "ddx(%s)", val(t, w[3])); return;
	case SpvOpDPdy: case SpvOpDPdyFine: case SpvOpDPdyCoarse: emit_assign(t, res, "ddy(%s)", val(t, w[3])); return;
	case SpvOpFwidth: case SpvOpFwidthFine: case SpvOpFwidthCoarse:
		emit_assign(t, res, "(abs(ddx(%s)) + abs(ddy(%s)))", val(t, w[3]), val(t, w[3]));
		return;
	case SpvOpAny: emit_assign(t, res, "any(%s)", val(t, w[3])); return;
	case SpvOpAll: emit_assign(t, res, "all(%s)", val(t, w[3])); return;
	case SpvOpIsNan: emit_assign(t, res, "isnan(%s)", val(t, w[3])); return;
	case SpvOpIsInf: emit_assign(t, res, "isinf(%s)", val(t, w[3])); return;
	case SpvOpSelect: emit_assign(t, res, "(%s ? %s : %s)", val(t, w[3]), val(t, w[4]), val(t, w[5])); return;
	case SpvOpConvertFToS: case SpvOpConvertFToU: case SpvOpConvertSToF: case SpvOpConvertUToF: case SpvOpUConvert:
	case SpvOpSConvert: case SpvOpFConvert: {
		char ts[32];
		type_str(t, rt, ts, sizeof(ts));
		emit_assign(t, res, "%s(%s)", ts, val(t, w[3]));
		return;
	}
	case SpvOpBitcast: {
		uint32_t from = id_at(t, w[3])->type;
		if (is_float_type(t, from) != is_float_type(t, rt)) {
			unsupported(t, "bitcasts between float and integer are not supported on GXM");
			return;
		}
		emit_assign(t, res, "%s", val(t, w[3]));
		return;
	}
	case SpvOpCompositeConstruct: {
		const uint32_t *td = def_of(t, rt);
		if (td && (op_of(td) == SpvOpTypeStruct || op_of(td) == SpvOpTypeArray)) {
			for (uint32_t k = 3; k < wc; k++) {
				ind(t);
				if (op_of(td) == SpvOpTypeArray) sb_printf(t->out, "%s[%u] = %s;\n", val(t, res), k - 3, val(t, w[k]));
				else {
					Id *s = id_at(t, rt);
					char mn[64];
					uint32_t m = k - 3;
					if (m < s->memCount && s->mem[m].name && s->mem[m].name[0]) clean_name(s->mem[m].name, mn, sizeof(mn));
					else snprintf(mn, sizeof(mn), "m%u", m);
					sb_printf(t->out, "%s.%s = %s;\n", val(t, res), mn, val(t, w[k]));
				}
			}
			return;
		}
		char ts[32];
		type_str(t, rt, ts, sizeof(ts));
		Sb b = { 0 };
		for (uint32_t k = 3; k < wc; k++) sb_printf(&b, "%s%s", k > 3 ? ", " : "", val(t, w[k]));
		emit_assign(t, res, "%s(%s)", ts, b.p ? b.p : "");
		free(b.p);
		return;
	}
	case SpvOpCompositeExtract: {
		char *e = chain_expr(t, val(t, w[3]), id_at(t, w[3])->type, NULL, 0);
		/* literal indices: build the path directly */
		Sb b = { 0 };
		sb_puts(&b, e);
		free(e);
		uint32_t ty = id_at(t, w[3])->type;
		for (uint32_t k = 4; k < wc; k++) {
			const uint32_t *d = def_of(t, ty);
			if (!d) break;
			if (op_of(d) == SpvOpTypeStruct) {
				Id *s = id_at(t, ty);
				char mn[64];
				if (w[k] < s->memCount && s->mem[w[k]].name && s->mem[w[k]].name[0]) clean_name(s->mem[w[k]].name, mn, sizeof(mn));
				else snprintf(mn, sizeof(mn), "m%u", w[k]);
				sb_printf(&b, ".%s", mn);
				ty = d[2 + w[k]];
			} else if (op_of(d) == SpvOpTypeVector) {
				sb_printf(&b, ".%c", k_swz[w[k] & 3]);
				ty = d[2];
			} else {
				sb_printf(&b, "[%u]", w[k]);
				ty = d[2];
			}
		}
		emit_assign(t, res, "%s", b.p ? b.p : "0");
		free(b.p);
		return;
	}
	case SpvOpCompositeInsert: {
		emit_assign(t, res, "%s", val(t, w[4]));
		Sb b = { 0 };
		sb_puts(&b, val(t, res));
		uint32_t ty = rt;
		for (uint32_t k = 5; k < wc; k++) {
			const uint32_t *d = def_of(t, ty);
			if (!d) break;
			if (op_of(d) == SpvOpTypeStruct) {
				Id *s = id_at(t, ty);
				char mn[64];
				if (w[k] < s->memCount && s->mem[w[k]].name && s->mem[w[k]].name[0]) clean_name(s->mem[w[k]].name, mn, sizeof(mn));
				else snprintf(mn, sizeof(mn), "m%u", w[k]);
				sb_printf(&b, ".%s", mn);
				ty = d[2 + w[k]];
			} else if (op_of(d) == SpvOpTypeVector) {
				sb_printf(&b, ".%c", k_swz[w[k] & 3]);
				ty = d[2];
			} else {
				sb_printf(&b, "[%u]", w[k]);
				ty = d[2];
			}
		}
		ind(t);
		sb_printf(t->out, "%s = %s;\n", b.p ? b.p : "0", val(t, w[3]));
		free(b.p);
		return;
	}
	case SpvOpVectorShuffle: {
		uint32_t n1 = vec_size(t, id_at(t, w[3])->type);
		bool all1 = true, all2 = true;
		for (uint32_t k = 5; k < wc; k++) {
			if (w[k] == 0xFFFFFFFFu) continue;
			if (w[k] >= n1) all1 = false; else all2 = false;
		}
		uint32_t rn = wc - 5;
		if (all1 || all2) {
			char swz[5] = { 0 };
			for (uint32_t k = 0; k < rn && k < 4; k++)
				swz[k] = w[5 + k] == 0xFFFFFFFFu ? 'x' : k_swz[(all1 ? w[5 + k] : w[5 + k] - n1) & 3];
			emit_assign(t, res, "%s.%s", val(t, all1 ? w[3] : w[4]), swz);
			return;
		}
		char ts[32];
		type_str(t, rt, ts, sizeof(ts));
		Sb b = { 0 };
		for (uint32_t k = 5; k < wc; k++) {
			uint32_t c = w[k] == 0xFFFFFFFFu ? 0 : w[k];
			if (c < n1) sb_printf(&b, "%s%s.%c", k > 5 ? ", " : "", val(t, w[3]), k_swz[c & 3]);
			else sb_printf(&b, "%s%s.%c", k > 5 ? ", " : "", val(t, w[4]), k_swz[(c - n1) & 3]);
		}
		emit_assign(t, res, "%s(%s)", ts, b.p ? b.p : "");
		free(b.p);
		return;
	}
	case SpvOpVectorExtractDynamic: emit_assign(t, res, "%s[%s]", val(t, w[3]), val(t, w[4])); return;
	case SpvOpVectorInsertDynamic:
		emit_assign(t, res, "%s", val(t, w[3]));
		ind(t);
		sb_printf(t->out, "%s[%s] = %s;\n", val(t, res), val(t, w[5]), val(t, w[4]));
		return;
	case SpvOpFunctionCall: {
		Sb b = { 0 };
		for (uint32_t k = 4; k < wc; k++) {
			uint32_t a = w[k];
			if (def_op(t, id_at(t, a)->type) == SpvOpTypePointer) {
				char *p = ptr_expr(t, a);
				sb_printf(&b, "%s%s", k > 4 ? ", " : "", p);
				free(p);
			} else {
				sb_printf(&b, "%s%s", k > 4 ? ", " : "", val(t, a));
			}
		}
		char *fn = ident(t, w[3], "f");
		if (def_op(t, rt) == SpvOpTypeVoid) { ind(t); sb_printf(t->out, "%s(%s);\n", fn, b.p ? b.p : ""); }
		else emit_assign(t, res, "%s(%s)", fn, b.p ? b.p : "");
		free(fn);
		free(b.p);
		return;
	}
	case SpvOpExtInst: emit_ext(t, w); return;
	case SpvOpImageRead: case SpvOpImageWrite: case SpvOpImageQueryLod: case SpvOpImageQuerySamples:
	case SpvOpImageGather: case SpvOpImageDrefGather:
		fail(t, "image instruction %u is not available on GXM", op);
		return;
	default:
		unsupported(t, "SPIR-V instruction %u is not supported yet", op);
		return;
	}
}

/* Values that are used once, in the block that computes them, and have no
 * side effects are written straight into that use. This keeps the Cg short,
 * which the GXM compiler handles a lot faster. */
static bool inline_op(Tr *t, const uint32_t *w) {
	uint32_t op = op_of(w);
	if (binop_sym(op)) return true;
	switch (op) {
	case SpvOpFNegate: case SpvOpSNegate: case SpvOpNot: case SpvOpLogicalNot: case SpvOpFMod: case SpvOpFRem:
	case SpvOpDot: case SpvOpMatrixTimesVector: case SpvOpVectorTimesMatrix: case SpvOpMatrixTimesMatrix:
	case SpvOpTranspose: case SpvOpSelect: case SpvOpConvertFToS: case SpvOpConvertFToU: case SpvOpConvertSToF:
	case SpvOpConvertUToF: case SpvOpUConvert: case SpvOpSConvert: case SpvOpFConvert: case SpvOpBitcast:
	case SpvOpCompositeExtract: case SpvOpVectorShuffle: case SpvOpVectorExtractDynamic: case SpvOpAny: case SpvOpAll:
	case SpvOpIsNan: case SpvOpIsInf: case SpvOpDPdx: case SpvOpDPdy: case SpvOpDPdxFine: case SpvOpDPdyFine:
	case SpvOpDPdxCoarse: case SpvOpDPdyCoarse: case SpvOpFwidth: case SpvOpFwidthFine: case SpvOpFwidthCoarse:
	case SpvOpExtInst: case SpvOpImageSampleImplicitLod: case SpvOpImageSampleExplicitLod:
	case SpvOpImageSampleProjImplicitLod: case SpvOpImageSampleProjExplicitLod: case SpvOpImageSampleDrefImplicitLod:
	case SpvOpImageSampleDrefExplicitLod: case SpvOpImageSampleProjDrefImplicitLod:
	case SpvOpImageSampleProjDrefExplicitLod: case SpvOpImageFetch: case SpvOpImageQuerySizeLod:
	case SpvOpImageQuerySize: case SpvOpImageQueryLevels: case SpvOpCompositeConstruct: case SpvOpCopyObject:
		return true;
	case SpvOpLoad: {
		/* only from storage the shader never writes: inputs and uniforms */
		const uint32_t *v = def_of(t, w[3]);
		while (v && (op_of(v) == SpvOpAccessChain || op_of(v) == SpvOpInBoundsAccessChain)) v = def_of(t, v[3]);
		if (!v || op_of(v) != SpvOpVariable) return false;
		return v[3] == SpvStorageClassInput || v[3] == SpvStorageClassUniform || v[3] == SpvStorageClassPushConstant;
	}
	default:
		return false;
	}
}

/* The consumer swizzles or indexes the value, which the GXM compiler does not
 * allow on literals: keep constructors in a temporary there. */
static bool swizzles_operand(uint32_t op) {
	return op == SpvOpCompositeExtract || op == SpvOpVectorShuffle || op == SpvOpVectorExtractDynamic ||
	       op == SpvOpCompositeInsert || op == SpvOpVectorInsertDynamic;
}

static void mark_inlines(Tr *t, Func *f) {
	for (uint32_t bi = 0; bi < f->blockCount; bi++) {
		Block *b = &f->blocks[bi];
		if (!b->term) continue;
		for (const uint32_t *w = b->first; w <= b->term; w += wc_of(w))
			for (uint32_t k = 1; k < wc_of(w); k++)
				if (w[k] < t->bound && t->ids[w[k]].uses < 0xFFFF) t->ids[w[k]].uses++;
	}
	for (uint32_t bi = 0; bi < f->blockCount; bi++) {
		Block *b = &f->blocks[bi];
		if (!b->term) continue;
		for (const uint32_t *w = b->first; w < b->term; w += wc_of(w)) {
			if (wc_of(w) < 3 || !inline_op(t, w)) continue;
			uint32_t res = w[2], rt = w[1];
			Id *e = id_at(t, res);
			uint32_t top = def_op(t, rt);
			/* the definition itself counts once */
			if (!e || e->uses != 2 || top == SpvOpTypeStruct || top == SpvOpTypeArray || top == SpvOpTypeVoid ||
			    top == SpvOpTypePointer || top == SpvOpTypeSampledImage || top == SpvOpTypeImage)
				continue;
			/* the one use has to follow in the same block, and not be a phi or a merge */
			const uint32_t *use = NULL;
			for (const uint32_t *u = w + wc_of(w); u <= b->term && !use; u += wc_of(u))
				for (uint32_t k = 1; k < wc_of(u); k++)
					if (u[k] == res) { use = u; break; }
			if (!use || op_of(use) == SpvOpPhi || op_of(use) == SpvOpLoopMerge || op_of(use) == SpvOpSelectionMerge)
				continue;
			if (op_of(w) == SpvOpCompositeConstruct && swizzles_operand(op_of(use))) continue;
			e->inl = true;
		}
	}
}

/* Declares the temporaries of a function: every value it computes, its
 * phis and its function variables. */
static void declare_locals(Tr *t, Func *f) {
	for (uint32_t bi = 0; bi < f->blockCount; bi++) {
		Block *b = &f->blocks[bi];
		if (!b->term) continue;
		for (const uint32_t *w = b->first; w < b->term; w += wc_of(w)) {
			uint32_t op = op_of(w), wc = wc_of(w);
			if (op == SpvOpVariable) {
				uint32_t ty = strip_ptr(t, w[1]);
				char ts[32], suffix[32];
				type_str(t, ty, ts, sizeof(ts));
				array_suffix(t, ty, suffix, sizeof(suffix));
				char *p = ptr_expr(t, w[2]);
				ind(t);
				sb_printf(t->out, "%s %s%s;\n", ts, p, suffix);
				free(p);
				continue;
			}
			if (wc < 3 || op == SpvOpStore || op == SpvOpCopyMemory || op == SpvOpSelectionMerge ||
			    op == SpvOpLoopMerge || op == SpvOpLine || op == SpvOpNoLine || op == SpvOpNop)
				continue;
			uint32_t ty = w[1];
			uint32_t top = def_op(t, ty);
			if (!top || top == SpvOpTypeVoid || top == SpvOpTypePointer || top == SpvOpTypeSampledImage ||
			    top == SpvOpTypeImage || top == SpvOpTypeSampler || t->ids[w[2]].inl)
				continue;
			char ts[32], suffix[32];
			type_str(t, ty, ts, sizeof(ts));
			array_suffix(t, ty, suffix, sizeof(suffix));
			ind(t);
			sb_printf(t->out, "%s %s%s;\n", ts, val(t, w[2]), suffix);
		}
	}
}

static void emit_function(Tr *t, Func *f) {
	if (f->emitted) return;
	f->emitted = true;
	/* callees first */
	for (const uint32_t *w = f->begin; w < f->end; w += wc_of(w))
		if (op_of(w) == SpvOpFunctionCall)
			for (uint32_t i = 0; i < t->funcCount; i++)
				if (t->funcs[i].id == w[3]) emit_function(t, &t->funcs[i]);
	if (t->status != TR_OK) return;

	Sb out = { 0 };
	Sb *saved = t->out;
	t->out = &out;
	t->fn = f;
	char rts[32];
	type_str(t, f->begin[1], rts, sizeof(rts));
	char *name = f->id == t->entry ? xstrdup("vkv_entry") : ident(t, f->id, "f");
	sb_printf(&out, "%s %s(", rts, name);
	free(name);
	bool first = true;
	for (const uint32_t *w = f->begin + wc_of(f->begin); w < f->end && op_of(w) == SpvOpFunctionParameter; w += wc_of(w)) {
		uint32_t ty = w[1];
		char ts[32], suffix[32];
		bool ptr = def_op(t, ty) == SpvOpTypePointer;
		type_str(t, strip_ptr(t, ty), ts, sizeof(ts));
		array_suffix(t, ty, suffix, sizeof(suffix));
		if (def_op(t, strip_ptr(t, ty)) == SpvOpTypeSampledImage || def_op(t, strip_ptr(t, ty)) == SpvOpTypeImage) {
			unsupported(t, "textures as function parameters are not supported");
			break;
		}
		char *p = ptr ? ptr_expr(t, w[2]) : xstrdup(val(t, w[2]));
		sb_printf(&out, "%s%s%s %s%s", first ? "" : ", ", ptr ? "inout " : "", ts, p, suffix);
		free(p);
		first = false;
	}
	sb_puts(&out, ")\n{\n");
	t->indent = 1;
	t->loopDepth = 0;
	mark_inlines(t, f);
	declare_locals(t, f);
	if (f->blockCount) emit_range(t, f->blocks[0].label, 0, 0, 0);
	sb_puts(&out, "}\n\n");
	t->out = saved;
	if (out.oom) t->status = TR_FAIL;
	if (out.p) sb_puts(&t->funcsOut, out.p);
	free(out.p);
}

/* --- Main --- */
static void emit_main(Tr *t, Sb *o) {
	bool scissor = g_nativeScissor && t->stage == VKV_SPIRV_FRAGMENT;
	const char *wpos = NULL;
	for (uint32_t i = 0; i < t->ioCount; i++)
		if (!strcmp(t->io[i].sem, "WPOS")) wpos = t->io[i].name;
	if (scissor) {
		sb_puts(o, "uniform float4 vkv_scissor;\n");
		add_uniform(t, "vkv_scissor", 0, 0, VKV_SPIRV_INTERNAL_SET, 1, VKV_SPIRV_FLOAT);
	}
	sb_puts(o, "void main(");
	bool first = true;
	if (scissor && !wpos) { sb_puts(o, "float4 vkv_wpos : WPOS"); first = false; }
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < t->ioCount; i++) {
			const Io *io = &t->io[i];
			if (io->isOutput != (pass == 1)) continue;
			sb_printf(o, "%s%s%s stage_%s_%s : %s", first ? "" : ",\n          ", io->isOutput ? "out " : "",
			          io->isInt ? io->floatType : io->type, io->isOutput ? "output" : "input", io->name, io->sem);
			first = false;
		}
	sb_puts(o, ")\n{\n");
	if (scissor) {
		char w[128];
		if (wpos) snprintf(w, sizeof(w), "stage_input_%s", wpos);
		else snprintf(w, sizeof(w), "vkv_wpos");
		sb_printf(o, "    float2 vkv_px = floor(%s.xy - 0.25f);\n"
		             "    if (vkv_px.x < vkv_scissor.x || vkv_px.y < vkv_scissor.y || vkv_px.x >= vkv_scissor.z ||\n"
		             "        vkv_px.y >= vkv_scissor.w)\n"
		             "        discard;\n", w);
	}
	if (t->init.p) sb_puts(o, t->init.p);
	for (uint32_t i = 0; i < t->ioCount; i++) {
		const Io *io = &t->io[i];
		if (io->isOutput) continue;
		if (io->frontFacing) sb_printf(o, "    %s = stage_input_%s > 0.0f;\n", io->name, io->name);
		else if (io->isInt) sb_printf(o, "    %s = %s(floor(stage_input_%s + 0.5f));\n", io->name, io->type, io->name);
		else sb_printf(o, "    %s = stage_input_%s;\n", io->name, io->name);
	}
	sb_puts(o, "    vkv_entry();\n");
	for (uint32_t i = 0; i < t->ioCount; i++) {
		const Io *io = &t->io[i];
		if (!io->isOutput) continue;
		if (io->isInt) sb_printf(o, "    stage_output_%s = %s(%s);\n", io->name, io->floatType, io->name);
		else sb_printf(o, "    stage_output_%s = %s;\n", io->name, io->name);
	}
	sb_puts(o, "}\n");
}

/* Struct types used by private, function or uniform-copy values, declared before use. */
static void declare_structs(Tr *t, Sb *o) {
	bool *done = calloc(t->bound, 1);
	if (!done) { t->status = TR_FAIL; return; }
	for (int pass = 0; pass < 16; pass++) {
		bool progress = false;
		for (const uint32_t *w = t->words + 5; w < t->words + t->count; w += wc_of(w)) {
			if (op_of(w) != SpvOpTypeStruct || done[w[1]]) continue;
			Id *s = id_at(t, w[1]);
			if ((s->memCount && s->mem[0].builtin >= 0) || s->block || s->bufferBlock) { done[w[1]] = true; continue; }
			bool ready = true;
			for (uint32_t k = 2; k < wc_of(w); k++) {
				uint32_t mt = w[k];
				while (def_op(t, mt) == SpvOpTypeArray || def_op(t, mt) == SpvOpTypeRuntimeArray) mt = def_of(t, mt)[2];
				if (def_op(t, mt) == SpvOpTypeStruct && !done[mt]) ready = false;
			}
			if (!ready) continue;
			sb_printf(o, "struct S%u\n{\n", w[1]);
			for (uint32_t k = 2; k < wc_of(w); k++) {
				char ts[32], suffix[32], mn[64];
				type_str(t, w[k], ts, sizeof(ts));
				array_suffix(t, w[k], suffix, sizeof(suffix));
				uint32_t m = k - 2;
				if (m < s->memCount && s->mem[m].name && s->mem[m].name[0]) clean_name(s->mem[m].name, mn, sizeof(mn));
				else snprintf(mn, sizeof(mn), "m%u", m);
				sb_printf(o, "    %s %s%s;\n", ts, mn, suffix);
			}
			sb_puts(o, "};\n\n");
			done[w[1]] = true;
			progress = true;
		}
		if (!progress) break;
	}
	free(done);
}

static void declare_globals(Tr *t) {
	for (const uint32_t *w = t->words + 5; w < t->words + t->count; w += wc_of(w)) {
		if (op_of(w) != SpvOpVariable) continue;
		uint32_t var = w[2], storage = w[3];
		uint32_t pointee = strip_ptr(t, w[1]);
		switch (storage) {
		case SpvStorageClassInput: declare_io(t, var, false); break;
		case SpvStorageClassOutput: declare_io(t, var, true); break;
		case SpvStorageClassUniform: case SpvStorageClassPushConstant:
			if (id_at(t, pointee)->bufferBlock) {
				fail(t, "storage buffers that the shader writes are not supported on GXM ('%s')",
				     id_at(t, var)->name ? id_at(t, var)->name : "?");
				break;
			}
			declare_block(t, var);
			break;
		case SpvStorageClassStorageBuffer:
			fail(t, "storage buffers that the shader writes are not supported on GXM ('%s'); read-only ones work",
			     id_at(t, var)->name ? id_at(t, var)->name : "?");
			break;
		case SpvStorageClassUniformConstant: {
			uint32_t et = var_elem_type(t, var);
			uint32_t eop = def_op(t, et);
			if (eop == SpvOpTypeSampledImage || eop == SpvOpTypeImage || eop == SpvOpTypeSampler) break;
			unsupported(t, "uniform constant of an unsupported type");
			break;
		}
		case SpvStorageClassPrivate: {
			char ts[32], suffix[32];
			type_str(t, pointee, ts, sizeof(ts));
			array_suffix(t, pointee, suffix, sizeof(suffix));
			char *p = ptr_expr(t, var);
			sb_printf(&t->globals, "static %s %s%s;\n", ts, p, suffix);
			if (wc_of(w) > 4) sb_printf(&t->init, "    %s = %s;\n", p, val(t, w[4]));
			free(p);
			break;
		}
		case SpvStorageClassFunction: break;
		case SpvStorageClassImage: case SpvStorageClassWorkgroup: case SpvStorageClassCrossWorkgroup:
		default:
			fail(t, "variables of storage class %u are not supported on GXM", storage);
			break;
		}
		if (t->status == TR_FAIL) return;
	}
}

static void free_tr(Tr *t) {
	if (t->ids)
		for (uint32_t i = 0; i < t->bound; i++) {
			free(t->ids[i].name);
			free(t->ids[i].expr);
			for (uint32_t k = 0; k < t->ids[i].memCount; k++) free(t->ids[i].mem[k].name);
			free(t->ids[i].mem);
		}
	free(t->ids);
	for (uint32_t i = 0; i < t->funcCount; i++) free(t->funcs[i].blocks);
	free(t->funcs);
	for (uint32_t i = 0; i < t->uniCount; i++) free(t->unis[i].name);
	free(t->unis);
	free(t->globals.p);
	free(t->init.p);
	free(t->funcsOut.p);
}

void vkv_spirv_native_set_options(uint32_t storageArraySize, int scissorVariant) {
	g_nativeArraySize = storageArraySize ? storageArraySize : 64;
	g_nativeScissor = scissorVariant;
}

int vkv_spirv_translate_native(const uint32_t *words, size_t wordCount, VkvSpirvStage stage, VkvSpirvLogFn log,
                               void *user, const VkvSpirvStride *strides, uint32_t strideCount, VkvSpirvResult *out) {
	memset(out, 0, sizeof(*out));
	Tr t;
	memset(&t, 0, sizeof(t));
	t.log = log;
	t.user = user;
	t.stage = stage;
	t.words = words;
	t.count = wordCount;
	if (!words || wordCount < 5 || words[0] != VKV_SPIRV_MAGIC) { fail(&t, "not a SPIR-V module"); return TR_FAIL; }
	t.bound = words[3];
	if (t.bound > (1u << 22)) { fail(&t, "SPIR-V id bound too large"); return TR_FAIL; }
	t.ids = calloc(t.bound, sizeof(Id));
	if (!t.ids) return TR_FAIL;
	for (uint32_t i = 0; i < t.bound; i++) {
		t.ids[i].location = -1;
		t.ids[i].binding = 0;
		t.ids[i].builtin = -1;
		t.ids[i].samplerRef = -1;
	}
	if (strideCount > 64) strideCount = 64;
	if (strides) memcpy(t.strides, strides, strideCount * sizeof(*strides));
	t.strideCount = strideCount;

	int status = TR_FAIL;
	if (parse(&t)) {
		collect_samplers(&t);
		if (t.status == TR_OK) declare_globals(&t);
		for (uint32_t i = 0; t.status == TR_OK && i < t.funcCount; i++)
			if (t.funcs[i].id == t.entry) emit_function(&t, &t.funcs[i]);
		if (t.status == TR_OK) {
			Sb o = { 0 };
			sb_printf(&o, "/* generated by VkVita from SPIR-V (%s) */\n", stage == VKV_SPIRV_VERTEX ? "vertex" : "fragment");
			declare_structs(&t, &o);
			if (t.globals.p) sb_puts(&o, t.globals.p);
			if (t.texInfo) {
				sb_puts(&o, "uniform float4 vkv_texinfo[32];\n");
				add_uniform(&t, "vkv_texinfo", 0, 16, VKV_SPIRV_INTERNAL_SET, stage == VKV_SPIRV_VERTEX ? 2 : 3,
				            VKV_SPIRV_FLOAT);
			}
			if (t.needCompare)
				sb_puts(&o, "\n/* depth compare: VkVita feeds the sampler's compare op as weights for less, equal and greater */\n"
				            "float vkv_compare(float r, float d, float3 op)\n{\n"
				            "    float s = sign(d - r);\n"
				            "    return op.y + (op.x - op.y) * saturate(s) + (op.z - op.y) * saturate(-s);\n}\n");
			sb_puts(&o, "\n");
			if (t.funcsOut.p) sb_puts(&o, t.funcsOut.p);
			emit_main(&t, &o);
			if (o.oom || t.status != TR_OK) {
				free(o.p);
				if (t.status == TR_OK) t.status = TR_FAIL;
			} else {
				out->cg = o.p;
				out->unis = t.unis;
				out->uniCount = t.uniCount;
				t.unis = NULL;
				t.uniCount = 0;
				if (t.samplerCount) {
					out->texs = malloc(t.samplerCount * sizeof(*out->texs));
					if (out->texs) {
						for (uint32_t i = 0; i < t.samplerCount; i++) out->texs[i] = t.samplers[i].t;
						out->texCount = t.samplerCount;
					}
				}
			}
		}
		status = t.status;
		if (status == TR_OK && !out->cg) status = TR_FAIL;
	}
	free_tr(&t);
	return status;
}
