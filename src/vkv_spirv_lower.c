/*
 * VkVita: lowers subpass inputs in SPIR-V before it is translated to Cg.
 *
 * GXM has no framebuffer fetch and shader model 3 has no texelFetch, so an
 * input attachment becomes an ordinary 2D texture that is sampled at the
 * center of the current pixel:
 *
 *   subpassLoad(x)  ->  texture(x, (floor(gl_FragCoord.xy - 0.25) + 0.5) * vkv_fb_inv)
 *
 * The 0.25 makes this pick the right pixel whether the fragment position
 * arrives as the pixel corner plus 0.5 or as the pixel center plus 0.5.
 *
 * vkv_fb_inv (1 / framebuffer size) lives in a small uniform block at
 * descriptor set VKV_SPIRV_INTERNAL_SET that VkVita fills while drawing.
 * The descriptor set and binding of the attachment stay the same, so the
 * application's VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT descriptor feeds it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vkv_spirv.h"

enum {
	OP_NAME = 5, OP_MEMBER_NAME = 6, OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_MEMORY_MODEL = 14,
	OP_ENTRY_POINT = 15, OP_CAPABILITY = 17, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
	OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43,
	OP_CONSTANT_COMPOSITE = 44, OP_FUNCTION = 54, OP_VARIABLE = 59, OP_LOAD = 61, OP_ACCESS_CHAIN = 65,
	OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_VECTOR_SHUFFLE = 79, OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
	OP_IMAGE_READ = 98, OP_FADD = 129, OP_FSUB = 131, OP_FMUL = 133,
};
enum { CAP_INPUT_ATTACHMENT = 40, DIM_2D = 1, DIM_SUBPASS = 6, SC_INPUT = 1, SC_UNIFORM = 2, EXEC_FRAGMENT = 4 };
enum { DEC_BLOCK = 2, DEC_BUILTIN = 11, DEC_BINDING = 33, DEC_SET = 34, DEC_OFFSET = 35, DEC_INPUT_INDEX = 43 };
enum { BUILTIN_FRAG_COORD = 15, GLSL_FLOOR = 8 };

#define MAX_IDS 16

typedef struct { uint32_t *w; size_t n, cap; bool oom; } Out;

static void put(Out *o, uint32_t v) {
	if (o->n == o->cap) {
		size_t cap = o->cap ? o->cap * 2 : 1024;
		uint32_t *w = realloc(o->w, cap * sizeof(*w));
		if (!w) { o->oom = true; return; }
		o->w = w;
		o->cap = cap;
	}
	o->w[o->n++] = v;
}

static void inst(Out *o, uint32_t op, const uint32_t *args, uint32_t argc) {
	put(o, ((argc + 1) << 16) | op);
	for (uint32_t i = 0; i < argc; i++) put(o, args[i]);
}

#define INST(o, op, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; inst((o), (op), a_, sizeof(a_) / 4); } while (0)

/* An instruction with a trailing literal string. */
static void inst_str(Out *o, uint32_t op, const uint32_t *args, uint32_t argc, const char *s) {
	uint32_t sw = (uint32_t)(strlen(s) / 4 + 1);
	put(o, ((argc + sw + 1) << 16) | op);
	for (uint32_t i = 0; i < argc; i++) put(o, args[i]);
	for (uint32_t i = 0; i < sw; i++) {
		uint32_t v = 0;
		for (uint32_t k = 0; k < 4; k++) {
			size_t at = i * 4 + k;
			if (at < strlen(s)) v |= (uint32_t)(uint8_t)s[at] << (8 * k);
		}
		put(o, v);
	}
}

static bool in_list(const uint32_t *l, uint32_t n, uint32_t v) {
	for (uint32_t i = 0; i < n; i++)
		if (l[i] == v) return true;
	return false;
}

static int index_of(const uint32_t *l, uint32_t n, uint32_t v) {
	for (uint32_t i = 0; i < n; i++)
		if (l[i] == v) return (int)i;
	return -1;
}

int vkv_spirv_lower_subpass(const uint32_t *w, size_t n, uint32_t **outWords, size_t *outCount) {
	*outWords = NULL;
	*outCount = 0;
	if (n < 5 || w[0] != VKV_SPIRV_MAGIC) return -1;

	/* --- pass 1: find what is there ---------------------------------------- */
	uint32_t img[MAX_IDS], imgN = 0, ptr[MAX_IDS], ptrImg[MAX_IDS], ptrN = 0, var[MAX_IDS], varN = 0;
	uint32_t floatId = 0, intId = 0, v2f = 0, v4f = 0, ptrInV4 = 0, ptrUniV2 = 0, fragCoord = 0, glsl = 0;
	for (size_t i = 5; i < n;) {
		uint32_t len = w[i] >> 16, op = w[i] & 0xFFFF;
		if (!len || i + len > n) return -1;
		const uint32_t *a = w + i + 1;
		switch (op) {
		case OP_TYPE_IMAGE:
			if (a[2] == DIM_SUBPASS && imgN < MAX_IDS) img[imgN++] = a[0];
			break;
		case OP_TYPE_FLOAT:
			if (a[1] == 32 && !floatId) floatId = a[0];
			break;
		case OP_TYPE_INT:
			if (a[1] == 32 && !intId) intId = a[0];
			break;
		case OP_EXT_INST_IMPORT:
			if (!strcmp((const char *)(a + 1), "GLSL.std.450")) glsl = a[0];
			break;
		case OP_DECORATE:
			if (len >= 4 && a[1] == DEC_BUILTIN && a[2] == BUILTIN_FRAG_COORD) fragCoord = a[0];
			break;
		default:
			break;
		}
		i += len;
	}
	if (!imgN) return 0; /* nothing to do */
	/* types that depend on the ones above */
	for (size_t i = 5; i < n;) {
		uint32_t len = w[i] >> 16, op = w[i] & 0xFFFF;
		const uint32_t *a = w + i + 1;
		if (op == OP_TYPE_VECTOR && a[1] == floatId && floatId) {
			if (a[2] == 2 && !v2f) v2f = a[0];
			if (a[2] == 4 && !v4f) v4f = a[0];
		} else if (op == OP_TYPE_POINTER && in_list(img, imgN, a[2]) && ptrN < MAX_IDS) {
			ptr[ptrN] = a[0];
			ptrImg[ptrN++] = a[2];
		}
		i += len;
	}
	for (size_t i = 5; i < n;) {
		uint32_t len = w[i] >> 16, op = w[i] & 0xFFFF;
		const uint32_t *a = w + i + 1;
		if (op == OP_TYPE_POINTER && a[1] == SC_INPUT && a[2] == v4f && v4f && !ptrInV4) ptrInV4 = a[0];
		if (op == OP_TYPE_POINTER && a[1] == SC_UNIFORM && a[2] == v2f && v2f && !ptrUniV2) ptrUniV2 = a[0];
		if (op == OP_VARIABLE && in_list(ptr, ptrN, a[0]) && varN < MAX_IDS) var[varN++] = a[1];
		i += len;
	}

	/* --- new ids ----------------------------------------------------------- */
	uint32_t bound = w[3];
	bool newFloat = !floatId, newInt = !intId, newV2 = !v2f, newV4 = !v4f, newGlsl = !glsl;
	bool newFragCoord = !fragCoord, newPtrInV4 = newFragCoord && !ptrInV4, newPtrUniV2 = !ptrUniV2;
	if (newFloat) floatId = bound++;
	if (newInt) intId = bound++;
	if (newV2) v2f = bound++;
	if (newV4) v4f = bound++;
	if (newGlsl) glsl = bound++;
	if (newPtrInV4) ptrInV4 = bound++;
	if (newFragCoord) fragCoord = bound++;
	if (newPtrUniV2) ptrUniV2 = bound++;
	uint32_t sampled[MAX_IDS];
	for (uint32_t k = 0; k < imgN; k++) sampled[k] = bound++;
	uint32_t cHalf = bound++, cHalf2 = bound++, cQuarter = bound++, cQuarter2 = bound++, cZeroI = bound++;
	uint32_t blockType = bound++, ptrBlock = bound++, ubo = bound++;
	bool v14 = w[1] >= 0x00010400u;

	/* --- pass 2: rewrite --------------------------------------------------- */
	Out o = { 0 };
	for (int k = 0; k < 5; k++) put(&o, w[k]);
	bool namesDone = false, globalsDone = false;
	uint32_t loaded[64], loadedN = 0;
	for (size_t i = 5; i < n;) {
		uint32_t len = w[i] >> 16, op = w[i] & 0xFFFF;
		const uint32_t *a = w + i + 1;
		i += len;

		if (op == OP_CAPABILITY && a[0] == CAP_INPUT_ATTACHMENT) continue;
		if (op == OP_DECORATE && a[1] == DEC_INPUT_INDEX) continue;
		if (op == OP_MEMORY_MODEL && newGlsl) {
			const uint32_t id[] = { glsl };
			inst_str(&o, OP_EXT_INST_IMPORT, id, 1, "GLSL.std.450");
		}
		if (!namesDone && (op == OP_DECORATE || op == OP_MEMBER_DECORATE || (op >= 19 && op <= 39) ||
		                   op == OP_CONSTANT || op == OP_VARIABLE || op == OP_FUNCTION)) {
			namesDone = true;
			const uint32_t bt[] = { blockType }, bm[] = { blockType, 0 }, bu[] = { ubo };
			inst_str(&o, OP_NAME, bt, 1, "vkv_fb");
			inst_str(&o, OP_MEMBER_NAME, bm, 2, "vkv_fb_inv");
			inst_str(&o, OP_NAME, bu, 1, "vkv_fb_ubo");
			INST(&o, OP_DECORATE, blockType, DEC_BLOCK);
			INST(&o, OP_MEMBER_DECORATE, blockType, 0, DEC_OFFSET, 0);
			INST(&o, OP_DECORATE, ubo, DEC_SET, VKV_SPIRV_INTERNAL_SET);
			INST(&o, OP_DECORATE, ubo, DEC_BINDING, 0);
			if (newFragCoord) INST(&o, OP_DECORATE, fragCoord, DEC_BUILTIN, BUILTIN_FRAG_COORD);
		}
		if (op == OP_FUNCTION && !globalsDone) {
			globalsDone = true;
			if (newFloat) INST(&o, OP_TYPE_FLOAT, floatId, 32);
			if (newInt) INST(&o, OP_TYPE_INT, intId, 32, 1);
			if (newV2) INST(&o, OP_TYPE_VECTOR, v2f, floatId, 2);
			if (newV4) INST(&o, OP_TYPE_VECTOR, v4f, floatId, 4);
			INST(&o, OP_CONSTANT, floatId, cHalf, 0x3F000000u);
			INST(&o, OP_CONSTANT_COMPOSITE, v2f, cHalf2, cHalf, cHalf);
			INST(&o, OP_CONSTANT, floatId, cQuarter, 0x3E800000u);
			INST(&o, OP_CONSTANT_COMPOSITE, v2f, cQuarter2, cQuarter, cQuarter);
			INST(&o, OP_CONSTANT, intId, cZeroI, 0);
			INST(&o, OP_TYPE_STRUCT, blockType, v2f);
			INST(&o, OP_TYPE_POINTER, ptrBlock, SC_UNIFORM, blockType);
			INST(&o, OP_VARIABLE, ptrBlock, ubo, SC_UNIFORM);
			if (newPtrUniV2) INST(&o, OP_TYPE_POINTER, ptrUniV2, SC_UNIFORM, v2f);
			if (newPtrInV4) INST(&o, OP_TYPE_POINTER, ptrInV4, SC_INPUT, v4f);
			if (newFragCoord) INST(&o, OP_VARIABLE, ptrInV4, fragCoord, SC_INPUT);
		}

		switch (op) {
		case OP_ENTRY_POINT: {
			/* name is a literal string after model and id; the interface follows it */
			uint32_t extra[2], ne = 0;
			if (a[0] == EXEC_FRAGMENT) {
				if (newFragCoord) extra[ne++] = fragCoord;
				if (v14) extra[ne++] = ubo;
			}
			put(&o, ((len + ne) << 16) | op);
			for (uint32_t k = 0; k < len - 1; k++) put(&o, a[k]);
			for (uint32_t k = 0; k < ne; k++) put(&o, extra[k]);
			continue;
		}
		case OP_TYPE_IMAGE: {
			int k = index_of(img, imgN, a[0]);
			if (k < 0) break;
			put(&o, w[i - len]);
			for (uint32_t m = 0; m < len - 1; m++) {
				uint32_t v = a[m];
				if (m == 2) v = DIM_2D;
				if (m == 6) v = 1; /* sampled */
				put(&o, v);
			}
			INST(&o, OP_TYPE_SAMPLED_IMAGE, sampled[k], a[0]);
			continue;
		}
		case OP_TYPE_POINTER: {
			int k = index_of(ptr, ptrN, a[0]);
			if (k < 0) break;
			INST(&o, OP_TYPE_POINTER, a[0], a[1], sampled[index_of(img, imgN, ptrImg[k])]);
			continue;
		}
		case OP_LOAD: {
			if (!in_list(var, varN, a[2])) break;
			/* the variable's pointer type now points at the sampled image */
			uint32_t type = a[0];
			int k = index_of(img, imgN, a[0]);
			if (k >= 0) type = sampled[k];
			INST(&o, OP_LOAD, type, a[1], a[2]);
			if (loadedN < 64) loaded[loadedN++] = a[1];
			continue;
		}
		case OP_IMAGE_READ: {
			if (!in_list(loaded, loadedN, a[2])) break;
			uint32_t fc = bound++, xy = bound++, sh = bound++, fl = bound++, c = bound++, pp = bound++, inv = bound++,
			         uv = bound++;
			INST(&o, OP_LOAD, v4f, fc, fragCoord);
			INST(&o, OP_VECTOR_SHUFFLE, v2f, xy, fc, fc, 0, 1);
			INST(&o, OP_FSUB, v2f, sh, xy, cQuarter2);
			INST(&o, OP_EXT_INST, v2f, fl, glsl, GLSL_FLOOR, sh);
			INST(&o, OP_FADD, v2f, c, fl, cHalf2);
			INST(&o, OP_ACCESS_CHAIN, ptrUniV2, pp, ubo, cZeroI);
			INST(&o, OP_LOAD, v2f, inv, pp);
			INST(&o, OP_FMUL, v2f, uv, c, inv);
			INST(&o, OP_IMAGE_SAMPLE_IMPLICIT_LOD, a[0], a[1], a[2], uv);
			continue;
		}
		default:
			break;
		}
		for (uint32_t m = 0; m < len; m++) put(&o, w[i - len + m]);
	}
	if (o.oom || !globalsDone) {
		free(o.w);
		return -1;
	}
	o.w[3] = bound;
	*outWords = o.w;
	*outCount = o.n;
	return 1;
}
