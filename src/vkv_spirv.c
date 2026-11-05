/*
 * VkVita: SPIR-V to Cg translation. The module is first rewritten where GXM
 * has no direct counterpart (vkv_spirv_lower.c), then turned into Cg by
 * vkv_spirv_cg.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_spirv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_storageArraySize = 64;
void vkv_spirv_set_storage_array_size(uint32_t elements) { g_storageArraySize = elements ? elements : 64; }
static int g_scissorVariant;
void vkv_spirv_set_scissor_variant(int enabled) { g_scissorVariant = enabled; }

static void logf_(VkvSpirvLogFn log, void *user, VkvSpirvLogLevel lvl, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
static void logf_(VkvSpirvLogFn log, void *user, VkvSpirvLogLevel lvl, const char *fmt, ...) {
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (log) log(lvl, msg, user);
}

/* Takes the result of one lowering pass. A pass returns 1 with a new module,
 * 0 when nothing changed and -1 on malformed input. */
static int lower(int r, uint32_t **cur, size_t *count, uint32_t *next, size_t nextCount, uint32_t **owned, int slot) {
	if (r > 0) {
		owned[slot] = next;
		*cur = next;
		*count = nextCount;
	}
	return r;
}

int vkv_spirv_translate(const uint32_t *words, size_t wordCount, VkvSpirvStage stage, VkvSpirvLogFn log, void *user,
                        VkvSpirvResult *out) {
	memset(out, 0, sizeof(*out));
	if (!words || wordCount < 5 || words[0] != VKV_SPIRV_MAGIC) {
		logf_(log, user, VKV_SPIRV_LOG_ERROR, "not a SPIR-V module");
		return -1;
	}
	uint32_t *owned[4] = { 0 };
	uint32_t *cur = (uint32_t *)words, *next = NULL;
	size_t count = wordCount, nextCount = 0;
	VkvSpirvStride strides[64];
	uint32_t strideCount = 0;

	int r = vkv_spirv_lower_builtins(cur, count, &next, &nextCount);
	r = lower(r, &cur, &count, next, nextCount, owned, 0);
	if (r >= 0) {
		r = vkv_spirv_lower_ssbo(cur, count, g_storageArraySize, &next, &nextCount, strides, &strideCount, 64);
		r = lower(r, &cur, &count, next, nextCount, owned, 1);
		if (r > 0)
			logf_(log, user, VKV_SPIRV_LOG_INFO, "storage buffers and packed arrays are read as uniform blocks "
			      "(%u elements per runtime array)", (unsigned)g_storageArraySize);
	}
	if (r >= 0) {
		r = vkv_spirv_lower_intdiv(cur, count, &next, &nextCount);
		r = lower(r, &cur, &count, next, nextCount, owned, 2);
		if (r > 0) logf_(log, user, VKV_SPIRV_LOG_INFO, "integer division is done in float");
	}
	if (r >= 0 && stage == VKV_SPIRV_FRAGMENT) {
		r = vkv_spirv_lower_subpass(cur, count, &next, &nextCount);
		r = lower(r, &cur, &count, next, nextCount, owned, 3);
		if (r > 0) logf_(log, user, VKV_SPIRV_LOG_INFO, "subpass inputs are read as textures at the current pixel");
	}

	int result = -1;
	if (r < 0) {
		logf_(log, user, VKV_SPIRV_LOG_ERROR, "malformed SPIR-V module");
	} else {
		vkv_spirv_native_set_options(g_storageArraySize, g_scissorVariant);
		result = vkv_spirv_translate_native(cur, count, stage, log, user, strides, strideCount, out) == 0 ? 0 : -1;
	}
	for (int i = 0; i < 4; i++) free(owned[i]);
	return result;
}

void vkv_spirv_free(VkvSpirvResult *r) {
	if (!r) return;
	for (uint32_t i = 0; i < r->uniCount; i++) free(r->unis[i].name);
	free(r->unis);
	free(r->texs);
	free(r->cg);
	memset(r, 0, sizeof(*r));
}
