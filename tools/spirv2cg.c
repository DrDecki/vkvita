/*
 * spirv2cg: translates a SPIR-V module into the Cg VkVita compiles at
 * runtime, for checking shaders on the PC or shipping them pretranslated.
 *
 *   spirv2cg [-v|-f] shader.spv [out.cg]
 *
 * The stage is taken from -v/-f, or from ".vert"/".frag" in the file name.
 * Push constant and uniform buffer offsets are printed as comments.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../src/vkv_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_cb(VkvSpirvLogLevel level, const char *msg, void *user) {
	(void)user;
	fprintf(stderr, "%s: %s\n", level == VKV_SPIRV_LOG_ERROR ? "error" : level == VKV_SPIRV_LOG_WARN ? "warning" : "info",
	        msg);
}

int main(int argc, char **argv) {
	int stage = -1, a = 1;
	if (a < argc && !strcmp(argv[a], "-v")) { stage = VKV_SPIRV_VERTEX; a++; }
	else if (a < argc && !strcmp(argv[a], "-f")) { stage = VKV_SPIRV_FRAGMENT; a++; }
	if (a >= argc) {
		fprintf(stderr, "usage: %s [-v|-f] shader.spv [out.cg]\n", argv[0]);
		return 2;
	}
	const char *in = argv[a], *outPath = a + 1 < argc ? argv[a + 1] : NULL;
	if (stage < 0) {
		if (strstr(in, ".vert") || strstr(in, ".vs")) stage = VKV_SPIRV_VERTEX;
		else if (strstr(in, ".frag") || strstr(in, ".fs")) stage = VKV_SPIRV_FRAGMENT;
		else {
			fprintf(stderr, "error: cannot tell the stage of %s, pass -v or -f\n", in);
			return 2;
		}
	}

	FILE *f = fopen(in, "rb");
	if (!f) { perror(in); return 1; }
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *words = malloc(size > 0 ? (size_t)size : 4);
	if (!words || size < 20 || fread(words, 1, (size_t)size, f) != (size_t)size) {
		fprintf(stderr, "error: cannot read %s\n", in);
		fclose(f);
		return 1;
	}
	fclose(f);

	VkvSpirvResult r;
	if (vkv_spirv_translate(words, (size_t)size / 4, (VkvSpirvStage)stage, log_cb, NULL, &r) != 0) {
		free(words);
		return 1;
	}
	free(words);

	FILE *o = outPath ? fopen(outPath, "w") : stdout;
	if (!o) { perror(outPath); vkv_spirv_free(&r); return 1; }
	for (uint32_t i = 0; i < r.uniCount; i++) {
		const VkvSpirvUniform *u = &r.unis[i];
		if (u->set < 0) fprintf(o, "/* uniform %s <- push constants +%u */\n", u->name, (unsigned)u->offset);
		else
			fprintf(o, "/* uniform %s <- set %d binding %u +%u */\n", u->name, (int)u->set, (unsigned)u->binding,
			        (unsigned)u->offset);
	}
	fputs(r.cg, o);
	if (outPath) fclose(o);
	vkv_spirv_free(&r);
	return 0;
}
