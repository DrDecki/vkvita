/*
 * VkVita: configuration, logging, validation reporting and profiling.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

VkvGlobals g_vkv;
static bool s_initDone;

#define VKV_DEFAULT_CONFIG_FILE "ux0:data/vkvita/config.txt"

uint64_t vkv_time_us(void) { return sceKernelGetProcessTimeWide(); }

static void vkv_copy_str(char *dst, size_t cap, const char *src) {
	if (!src) { dst[0] = 0; return; }
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = 0;
}

void vkvGetDefaultConfig(VkvConfig *c) {
	memset(c, 0, sizeof(*c));
#ifdef VKV_DEFAULT_DEBUG_FLAGS
	c->debugFlags = VKV_DEFAULT_DEBUG_FLAGS;
#else
	c->debugFlags = VKV_DEBUG_LOG_UNSUPPORTED;
#endif
	c->compatFlags        = 0;
	c->logPath            = NULL;
	c->shaderDir          = "ux0:data/vkvita/shaders";
	c->sharkModulePath    = NULL;
	c->profileInterval    = 120;
	c->parameterBufferSize= SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
	c->displayQueueDepth  = 2;
	c->poolBlockSize      = 4u * 1024 * 1024;
	c->poolMaxAllocSize   = 512u * 1024;
	c->scenesPerFrame     = 4;
	c->ffVertexBufferSize = 1024u * 1024;
	c->ffFrames           = 3;
	c->storageArraySize   = 64;
}

static void vkv_apply_config(const VkvConfig *c) {
	g_vkv.cfg = *c;
	vkv_copy_str(g_vkv.logPath, sizeof(g_vkv.logPath), c->logPath);
	vkv_copy_str(g_vkv.shaderDir, sizeof(g_vkv.shaderDir), c->shaderDir);
	vkv_copy_str(g_vkv.sharkPath, sizeof(g_vkv.sharkPath), c->sharkModulePath);
	g_vkv.cfg.logPath         = g_vkv.logPath[0] ? g_vkv.logPath : NULL;
	g_vkv.cfg.shaderDir       = g_vkv.shaderDir;
	g_vkv.cfg.sharkModulePath = g_vkv.sharkPath[0] ? g_vkv.sharkPath : NULL;
	if (!g_vkv.cfg.profileInterval) g_vkv.cfg.profileInterval = 120;
	if (g_vkv.cfg.parameterBufferSize < 0x40000) g_vkv.cfg.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
	if (!g_vkv.cfg.displayQueueDepth) g_vkv.cfg.displayQueueDepth = 2;
	if (g_vkv.cfg.poolBlockSize < 256 * 1024) g_vkv.cfg.poolBlockSize = 256 * 1024;
	if (g_vkv.cfg.poolMaxAllocSize > g_vkv.cfg.poolBlockSize / 2) g_vkv.cfg.poolMaxAllocSize = g_vkv.cfg.poolBlockSize / 2;
	if (!g_vkv.cfg.scenesPerFrame) g_vkv.cfg.scenesPerFrame = 4;
	if (g_vkv.cfg.ffVertexBufferSize < 64 * 1024) g_vkv.cfg.ffVertexBufferSize = 64 * 1024;
	if (!g_vkv.cfg.ffFrames) g_vkv.cfg.ffFrames = 3;
	if (!g_vkv.cfg.storageArraySize) g_vkv.cfg.storageArraySize = 64;
	if (g_vkv.cfg.storageArraySize > 4096) g_vkv.cfg.storageArraySize = 4096;
	/* a new log path takes effect on the next message */
	if (g_vkv.logFdTried && g_vkv.logFd >= 0) sceIoClose(g_vkv.logFd);
	g_vkv.logFd = -1;
	g_vkv.logFdTried = false;
}

void vkv_config_init_once(void) {
	if (s_initDone) return;
	s_initDone = true;
	VkvConfig c;
	vkvGetDefaultConfig(&c);
	g_vkv.logFd = -1;
	vkv_apply_config(&c);
}

void vkvSetConfig(const VkvConfig *config) {
	s_initDone = true;
	if (!config) { VkvConfig c; vkvGetDefaultConfig(&c); vkv_apply_config(&c); return; }
	vkv_apply_config(config);
}

void vkvGetConfig(VkvConfig *config) {
	vkv_config_init_once();
	if (config) *config = g_vkv.cfg;
}

void vkvSetDebugFlags(VkvDebugFlags flags) { vkv_config_init_once(); g_vkv.cfg.debugFlags = flags; }
void vkvSetCompatFlags(VkvCompatFlags flags) { vkv_config_init_once(); g_vkv.cfg.compatFlags = flags; }

/* --- Config file --- */
typedef struct { const char *name; uint32_t bit; } VkvFlagName;

static const VkvFlagName k_debugNames[] = {
	{ "info", VKV_DEBUG_LOG_INFO }, { "shaders", VKV_DEBUG_LOG_SHADERS },
	{ "validation", VKV_DEBUG_VALIDATION }, { "profile", VKV_DEBUG_PROFILE },
	{ "calls", VKV_DEBUG_LOG_CALLS }, { "gpu_sync", VKV_DEBUG_GPU_SYNC },
	{ "dump_shaders", VKV_DEBUG_DUMP_SHADERS }, { "unsupported", VKV_DEBUG_LOG_UNSUPPORTED },
	{ "break_on_error", VKV_DEBUG_BREAK_ON_ERROR },
	{ "all", 0xFFFFFFFFu & ~(VKV_DEBUG_LOG_CALLS | VKV_DEBUG_BREAK_ON_ERROR | VKV_DEBUG_GPU_SYNC) },
};
static const VkvFlagName k_compatNames[] = {
	{ "gl_clip_space", VKV_COMPAT_GL_CLIP_SPACE }, { "invert_cull", VKV_COMPAT_INVERT_CULL },
	{ "disable_cull", VKV_COMPAT_DISABLE_CULL }, { "cpu_clear", VKV_COMPAT_CPU_CLEAR },
	{ "shader_cache", VKV_COMPAT_SHADER_CACHE }, { "force_vsync", VKV_COMPAT_FORCE_VSYNC },
	{ "no_vsync", VKV_COMPAT_NO_VSYNC }, { "ignore_scissor", VKV_COMPAT_IGNORE_SCISSOR },
	{ "fast_transfers", VKV_COMPAT_FAST_TRANSFERS }, { "cpu_copy", VKV_COMPAT_CPU_COPY },
};

static char *vkv_trim(char *s) {
	while (*s && isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

/* Parses "a, b, c" or a number. */
static uint32_t vkv_parse_flags(char *v, const VkvFlagName *names, size_t n, const char *key) {
	if (isdigit((unsigned char)v[0])) return (uint32_t)strtoul(v, NULL, 0);
	uint32_t flags = 0;
	char *tok = strtok(v, ",| ");
	while (tok) {
		size_t i;
		for (i = 0; i < n; i++)
			if (!strcmp(tok, names[i].name)) { flags |= names[i].bit; break; }
		if (i == n && strcmp(tok, "none")) VKV_WARN("config: unknown %s flag '%s'", key, tok);
		tok = strtok(NULL, ",| ");
	}
	return flags;
}

int vkvLoadConfigFile(const char *path) {
	vkv_config_init_once();
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return -1;
	char buf[2048];
	int len = sceIoRead(fd, buf, sizeof(buf) - 1);
	sceIoClose(fd);
	if (len < 0) return -2;
	buf[len] = 0;

	VkvConfig c = g_vkv.cfg;
	char logPath[256], shaderDir[256], shark[256];
	vkv_copy_str(logPath, sizeof(logPath), g_vkv.cfg.logPath);
	vkv_copy_str(shaderDir, sizeof(shaderDir), g_vkv.cfg.shaderDir);
	vkv_copy_str(shark, sizeof(shark), g_vkv.cfg.sharkModulePath);

	char *save = NULL;
	for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		char *hash = strchr(line, '#');
		if (hash) *hash = 0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = vkv_trim(line), *v = vkv_trim(eq + 1);
		if (!*k) continue;
		if (!strcmp(k, "debug"))                 c.debugFlags = vkv_parse_flags(v, k_debugNames, VKV_ARRAY_SIZE(k_debugNames), k);
		else if (!strcmp(k, "debug_add"))        c.debugFlags |= vkv_parse_flags(v, k_debugNames, VKV_ARRAY_SIZE(k_debugNames), k);
		else if (!strcmp(k, "compat"))           c.compatFlags = vkv_parse_flags(v, k_compatNames, VKV_ARRAY_SIZE(k_compatNames), k);
		else if (!strcmp(k, "compat_add"))       c.compatFlags |= vkv_parse_flags(v, k_compatNames, VKV_ARRAY_SIZE(k_compatNames), k);
		else if (!strcmp(k, "log_path"))         vkv_copy_str(logPath, sizeof(logPath), v);
		else if (!strcmp(k, "shader_dir"))       vkv_copy_str(shaderDir, sizeof(shaderDir), v);
		else if (!strcmp(k, "shark_module"))     vkv_copy_str(shark, sizeof(shark), v);
		else if (!strcmp(k, "profile_interval")) c.profileInterval = strtoul(v, NULL, 0);
		else if (!strcmp(k, "parameter_buffer_kb")) c.parameterBufferSize = strtoul(v, NULL, 0) * 1024;
		else if (!strcmp(k, "display_queue_depth")) c.displayQueueDepth = strtoul(v, NULL, 0);
		else if (!strcmp(k, "pool_block_kb"))    c.poolBlockSize = strtoul(v, NULL, 0) * 1024;
		else if (!strcmp(k, "pool_max_alloc_kb"))c.poolMaxAllocSize = strtoul(v, NULL, 0) * 1024;
		else if (!strcmp(k, "scenes_per_frame")) c.scenesPerFrame = strtoul(v, NULL, 0);
		else if (!strcmp(k, "ff_buffer_kb"))     c.ffVertexBufferSize = strtoul(v, NULL, 0) * 1024;
		else if (!strcmp(k, "ff_frames"))        c.ffFrames = strtoul(v, NULL, 0);
		else if (!strcmp(k, "storage_array_size")) c.storageArraySize = strtoul(v, NULL, 0);
		else VKV_WARN("config: unknown key '%s'", k);
	}
	c.logPath = logPath[0] ? logPath : NULL;
	c.shaderDir = shaderDir;
	c.sharkModulePath = shark[0] ? shark : NULL;
	vkv_apply_config(&c);
	vkv_log(VKV_LOG_INFO, "config loaded from %s (debug=0x%x compat=0x%x)", path,
	        (unsigned)g_vkv.cfg.debugFlags, (unsigned)g_vkv.cfg.compatFlags);
	return 0;
}

/* Called by vkCreateInstance. */
void vkv_load_default_config_file(void) {
	vkv_config_init_once();
	vkvLoadConfigFile(VKV_DEFAULT_CONFIG_FILE);
}

/* --- Logging --- */
void vkvSetLogCallback(PFN_vkvLogCallback cb, void *user) {
	vkv_config_init_once();
	g_vkv.logCb = cb;
	g_vkv.logUser = user;
}

static const char *k_levelNames[] = { "debug", "info", "WARN", "ERROR" };

static void vkv_mkdirs_for(const char *path) {
	char tmp[256];
	vkv_copy_str(tmp, sizeof(tmp), path);
	for (char *p = tmp; *p; p++) {
		if (*p == '/' && p > tmp && p[-1] != ':') {
			*p = 0;
			sceIoMkdir(tmp, 0777);
			*p = '/';
		}
	}
}

static void vkv_logv(VkvLogLevel lvl, const char *fmt, va_list ap) {
	vkv_config_init_once();
	char msg[512];
	int n = snprintf(msg, sizeof(msg), "[vkvita %s] ", k_levelNames[lvl & 3]);
	if (n < 0) n = 0;
	vsnprintf(msg + n, sizeof(msg) - n - 1, fmt, ap);
	size_t len = strlen(msg);
	if (g_vkv.logCb) g_vkv.logCb(lvl, msg + n, g_vkv.logUser);
	if (len < sizeof(msg) - 1) { msg[len++] = '\n'; msg[len] = 0; }
	sceClibPrintf("%s", msg);
	if (g_vkv.cfg.logPath) {
		if (!g_vkv.logFdTried) {
			g_vkv.logFdTried = true;
			vkv_mkdirs_for(g_vkv.cfg.logPath);
			g_vkv.logFd = sceIoOpen(g_vkv.cfg.logPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
		}
		if (g_vkv.logFd >= 0) sceIoWrite(g_vkv.logFd, msg, len);
	}
}

void vkv_log(VkvLogLevel lvl, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vkv_logv(lvl, fmt, ap);
	va_end(ap);
}

void vkvLog(VkvLogLevel lvl, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vkv_logv(lvl, fmt, ap);
	va_end(ap);
}

bool vkv_fail(const char *func, const char *fmt, ...) {
	char msg[384];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	g_vkv.validationErrors++;
	vkv_log(VKV_LOG_ERROR, "validation: %s: %s", func, msg);
	if (vkv_dbg(VKV_DEBUG_BREAK_ON_ERROR)) {
		vkv_log(VKV_LOG_ERROR, "break_on_error is set, trapping now");
		if (g_vkv.logFd >= 0) sceIoClose(g_vkv.logFd);
		__builtin_trap();
	}
	return true;
}

#define VKV_MAX_ONCE 128
static const char *s_onceKeys[VKV_MAX_ONCE];
static uint32_t s_onceCount;

void vkv_unsupported_once(const char *key, const char *what) {
	if (!vkv_dbg(VKV_DEBUG_LOG_UNSUPPORTED)) return;
	for (uint32_t i = 0; i < s_onceCount; i++)
		if (s_onceKeys[i] == key) return;
	if (s_onceCount < VKV_MAX_ONCE) s_onceKeys[s_onceCount++] = key;
	vkv_log(VKV_LOG_WARN, "unsupported: %s", what);
}

/* --- Profiling --- */
void vkvGetFrameStats(VkvFrameStats *stats) {
	if (stats) *stats = g_vkv.last;
}

void vkv_stats_frame_end(void) {
	uint64_t now = vkv_time_us();
	VkvFrameStats *s = &g_vkv.last;
	VkvStatsCounters *c = &g_vkv.cur;

	s->frame++;
	s->drawCalls       = c->drawCalls;
	s->indices         = c->indices;
	s->scenes          = c->scenes;
	s->pipelineBinds   = c->pipelineBinds;
	s->descriptorBinds = c->descriptorBinds;
	s->clears          = c->clears;
	s->copies          = c->copies;
	s->ffDraws         = c->ffDraws;
	s->submitCpuMs     = c->submitUs / 1000.0f;
	s->gpuWaitMs       = c->gpuWaitUs / 1000.0f;
	s->frameMs         = g_vkv.lastPresentUs ? (now - g_vkv.lastPresentUs) / 1000.0f : 0.0f;
	s->cdramBytes      = g_vkv.cdramBytes;
	s->ramBytes        = g_vkv.ramBytes;
	s->validationErrors= g_vkv.validationErrors;
	g_vkv.lastPresentUs = now;

	if (!g_vkv.intervalStartUs) g_vkv.intervalStartUs = now;
	g_vkv.intervalFrames++;
	if (g_vkv.intervalFrames >= g_vkv.cfg.profileInterval) {
		uint64_t dt = now - g_vkv.intervalStartUs;
		s->fps = dt ? (float)g_vkv.intervalFrames * 1000000.0f / (float)dt : 0.0f;
		if (vkv_dbg(VKV_DEBUG_PROFILE)) {
			vkv_log(VKV_LOG_INFO,
			        "profile f=%u fps=%.1f frame=%.2fms submit=%.2fms gpuwait=%.2fms draws=%u idx=%u scenes=%u "
			        "pipe=%u sets=%u clears=%u copies=%u ff=%u cdram=%uKB ram=%uKB verr=%u",
			        (unsigned)s->frame, s->fps, s->frameMs, s->submitCpuMs, s->gpuWaitMs, (unsigned)s->drawCalls,
			        (unsigned)s->indices, (unsigned)s->scenes, (unsigned)s->pipelineBinds,
			        (unsigned)s->descriptorBinds, (unsigned)s->clears, (unsigned)s->copies, (unsigned)s->ffDraws,
			        (unsigned)(s->cdramBytes / 1024), (unsigned)(s->ramBytes / 1024),
			        (unsigned)s->validationErrors);
		}
		g_vkv.intervalStartUs = now;
		g_vkv.intervalFrames = 0;
	}
	memset(c, 0, sizeof(*c));
}
