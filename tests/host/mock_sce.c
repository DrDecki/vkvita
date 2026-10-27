/*
 * Host-side mock of the PS Vita system/GXM functions VkVita and its
 * samples import. It lets the real library + samples run on a PC under
 * AddressSanitizer/UBSan. The mock does not render anything; it checks
 * how the library drives GXM:
 *   - memory blocks: size alignment rules (256 KiB CDRAM, 4 KiB RAM)
 *   - draws: every vertex attribute fetch and index read touches real
 *     buffer memory (ASan catches out-of-bounds), index width matches the
 *     vertex program's index source, all attributes/textures are bound
 *   - default uniform writes stay inside the reserved buffer
 *   - color/depth surfaces, textures and display buffers fit their memory
 *   - scene nesting, fence notifications, shader patcher refcounts
 * Shaders: shark_compile_shader_extended() parses the Cg main() signature
 * to fabricate a program with plausible reflection data.
 *
 * Exit code: 0 if no MOCK ERROR was reported.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE

#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitashark.h>

/* POSIX after vita-headers: glibc defines st_ctime etc. as macros */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------------ */
static int g_errors, g_draws, g_scenes, g_presents, g_fills, g_indices, g_uniformWrites;

static void mock_fail(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "MOCK ERROR: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	g_errors++;
}

static volatile uint8_t g_sink;
static void touch(const void *p, size_t n) { /* reads first and last byte: ASan checks the range ends */
	if (!n) return;
	g_sink ^= ((const volatile uint8_t *)p)[0];
	g_sink ^= ((const volatile uint8_t *)p)[n - 1];
}
static void touch_all(const void *p, size_t n) {
	for (size_t i = 0; i < n; i++) g_sink ^= ((const volatile uint8_t *)p)[i];
}

/* ------------------------------------------------------------------------ */
/* Kernel / clib / io                                                       */
/* ------------------------------------------------------------------------ */
int sceClibPrintf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int r = vprintf(fmt, ap);
	va_end(ap);
	if (strstr(fmt, "%s") && 0) {}
	return r;
}

SceUInt64 sceKernelGetProcessTimeWide(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (SceUInt64)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

int sceKernelDelayThread(SceUInt delay) { (void)delay; return 0; }

#define MAX_BLOCKS 512
static struct { void *base; size_t size; int used; int mapped; } g_blocks[MAX_BLOCKS];
static size_t g_cdram, g_ram;

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt *opt) {
	(void)name; (void)opt;
	size_t align = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096;
	if (size % align) mock_fail("sceKernelAllocMemBlock: size %u not aligned to %zu for type 0x%x", size, align, type);
	for (int i = 1; i < MAX_BLOCKS; i++) {
		if (g_blocks[i].used) continue;
		void *p = NULL;
		if (posix_memalign(&p, 4096, size)) return -1;
		memset(p, 0xCD, size);
		g_blocks[i].base = p; g_blocks[i].size = size; g_blocks[i].used = 1; g_blocks[i].mapped = 0;
		if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW) g_cdram += size; else g_ram += size;
		return i;
	}
	return -1;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base) {
	if (uid <= 0 || uid >= MAX_BLOCKS || !g_blocks[uid].used) { mock_fail("GetMemBlockBase: bad uid %d", uid); return -1; }
	*base = g_blocks[uid].base;
	return 0;
}

int sceKernelFreeMemBlock(SceUID uid) {
	if (uid <= 0 || uid >= MAX_BLOCKS || !g_blocks[uid].used) { mock_fail("FreeMemBlock: bad uid %d", uid); return -1; }
	if (g_blocks[uid].mapped) mock_fail("FreeMemBlock: block %d is still GPU-mapped", uid);
	free(g_blocks[uid].base);
	g_blocks[uid].used = 0;
	return 0;
}

static int find_block(const void *base) {
	for (int i = 1; i < MAX_BLOCKS; i++)
		if (g_blocks[i].used && g_blocks[i].base == base) return i;
	return -1;
}

static void report_and_exit(void);
int sceKernelExitProcess(int res) { (void)res; report_and_exit(); return 0; }

static char g_root[512];
static void host_path(char *out, size_t cap, const char *p) {
	const char *colon = strchr(p, ':');
	if (colon) snprintf(out, cap, "%s/%.*s%s%s", g_root, (int)(colon - p), p, colon[1] == '/' ? "" : "/", colon + 1);
	else snprintf(out, cap, "%s/%s", g_root, p);
}

SceUID sceIoOpen(const char *file, int flags, SceMode mode) {
	char p[1024];
	host_path(p, sizeof(p), file);
	int f = 0;
	if ((flags & SCE_O_RDWR) == SCE_O_RDWR) f = O_RDWR;
	else if (flags & SCE_O_WRONLY) f = O_WRONLY;
	else f = O_RDONLY;
	if (flags & SCE_O_CREAT) f |= O_CREAT;
	if (flags & SCE_O_TRUNC) f |= O_TRUNC;
	if (flags & SCE_O_APPEND) f |= O_APPEND;
	int fd = open(p, f, 0644);
	(void)mode;
	return fd < 0 ? (SceUID)0x80010002 : fd;
}
int sceIoClose(SceUID fd) { return close(fd); }
int sceIoRead(SceUID fd, void *data, SceSize size) { return (int)read(fd, data, size); }
int sceIoWrite(SceUID fd, const void *data, SceSize size) { return (int)write(fd, data, size); }
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence) {
	return lseek(fd, offset, whence == SCE_SEEK_END ? SEEK_END : whence == SCE_SEEK_CUR ? SEEK_CUR : SEEK_SET);
}
int sceIoMkdir(const char *dir, SceMode mode) {
	char p[1024];
	host_path(p, sizeof(p), dir);
	(void)mode;
	return mkdir(p, 0755);
}

/* ------------------------------------------------------------------------ */
/* Ctrl: scripted button presses, START after MOCK_FRAMES                   */
/* ------------------------------------------------------------------------ */
static int g_frame, g_maxFrames = 90;

int sceCtrlSetSamplingMode(SceCtrlPadInputMode mode) { (void)mode; return 0; }

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count) {
	(void)port; (void)count;
	memset(pad, 0, sizeof(*pad));
	pad->lx = pad->ly = 128;
	int f = g_frame++;
	static const struct { int frame; unsigned buttons; } script[] = {
		{ 10, SCE_CTRL_SELECT }, { 20, SCE_CTRL_TRIANGLE }, { 22, SCE_CTRL_TRIANGLE }, { 30, SCE_CTRL_SQUARE },
		{ 32, SCE_CTRL_SQUARE }, { 40, SCE_CTRL_CIRCLE }, { 50, SCE_CTRL_CROSS }, { 60, SCE_CTRL_CIRCLE },
		{ 62, SCE_CTRL_CROSS },
	};
	for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++)
		if (script[i].frame == f) pad->buttons |= script[i].buttons;
	if (f > 15 && f < 25) { pad->lx = 255; pad->ly = 0; }
	if (f >= g_maxFrames) pad->buttons |= SCE_CTRL_START;
	return 1;
}

/* ------------------------------------------------------------------------ */
/* Programs                                                                 */
/* ------------------------------------------------------------------------ */
#define MP_MAX 128
struct SceGxmProgramParameter {
	char     name[40];
	int      cat, type, semantic;
	unsigned semIdx, comps, arr, res, container;
};
struct SceGxmProgram {
	char     magic[4];
	uint32_t type, count, ubSize;
	struct SceGxmProgramParameter p[MP_MAX];
};

int sceGxmProgramCheck(const SceGxmProgram *p) { return p && !memcmp(p->magic, "GXP", 4) ? 0 : (int)0x805B0000; }
SceGxmProgramType sceGxmProgramGetType(const SceGxmProgram *p) { return (SceGxmProgramType)p->type; }
unsigned int sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram *p) { return p->ubSize; }
unsigned int sceGxmProgramGetParameterCount(const SceGxmProgram *p) { return p->count; }
const SceGxmProgramParameter *sceGxmProgramGetParameter(const SceGxmProgram *p, unsigned int i) {
	if (i >= p->count) { mock_fail("GetParameter index %u out of range", i); return NULL; }
	return &p->p[i];
}
const SceGxmProgramParameter *sceGxmProgramFindParameterByName(const SceGxmProgram *p, const char *name) {
	for (unsigned i = 0; i < p->count; i++) if (!strcmp(p->p[i].name, name)) return &p->p[i];
	return NULL;
}
const SceGxmProgramParameter *sceGxmProgramFindParameterBySemantic(const SceGxmProgram *p, SceGxmParameterSemantic s,
                                                                   unsigned int idx) {
	for (unsigned i = 0; i < p->count; i++)
		if (p->p[i].semantic == (int)s && p->p[i].semIdx == idx && s != SCE_GXM_PARAMETER_SEMANTIC_NONE) return &p->p[i];
	return NULL;
}
SceGxmParameterCategory sceGxmProgramParameterGetCategory(const SceGxmProgramParameter *p) { return (SceGxmParameterCategory)p->cat; }
const char *sceGxmProgramParameterGetName(const SceGxmProgramParameter *p) { return p->name; }
SceGxmParameterSemantic sceGxmProgramParameterGetSemantic(const SceGxmProgramParameter *p) { return (SceGxmParameterSemantic)p->semantic; }
unsigned int sceGxmProgramParameterGetSemanticIndex(const SceGxmProgramParameter *p) { return p->semIdx; }
SceGxmParameterType sceGxmProgramParameterGetType(const SceGxmProgramParameter *p) { return (SceGxmParameterType)p->type; }
unsigned int sceGxmProgramParameterGetComponentCount(const SceGxmProgramParameter *p) { return p->comps; }
unsigned int sceGxmProgramParameterGetArraySize(const SceGxmProgramParameter *p) { return p->arr; }
unsigned int sceGxmProgramParameterGetResourceIndex(const SceGxmProgramParameter *p) { return p->res; }
unsigned int sceGxmProgramParameterGetContainerIndex(const SceGxmProgramParameter *p) { return p->container; }

/* --- fake Cg compiler: parses `main(...)` parameters -------------------- */
static SceGxmProgram *g_sharkOut;
static int g_sharkReady;
static void (*g_sharkLog)(const char *, shark_log_level, int);

int shark_init(const char *path) { (void)path; if (getenv("MOCK_NO_SHARK")) return -1; g_sharkReady = 1; return 0; }
void shark_end(void) { g_sharkReady = 0; }
void shark_install_log_cb(void (*cb)(const char *, shark_log_level, int)) { g_sharkLog = cb; }
void shark_set_warnings_level(shark_warn_level level) { (void)level; }
void shark_clear_output(void) {
	if (g_sharkOut) { memset(g_sharkOut, 0xEE, sizeof(*g_sharkOut)); free(g_sharkOut); } /* use-after-clear shows up */
	g_sharkOut = NULL;
}

static unsigned type_comps(const char *t, unsigned *arr) {
	*arr = 1;
	if (!strcmp(t, "float4x4")) { *arr = 4; return 4; }
	if (!strcmp(t, "float3x3")) { *arr = 3; return 3; }
	if (!strncmp(t, "float", 5) || !strncmp(t, "half", 4)) {
		const char *d = t + (t[0] == 'f' ? 5 : 4);
		return isdigit((unsigned char)*d) ? (unsigned)(*d - '0') : 1;
	}
	return 1;
}

SceGxmProgram *shark_compile_shader_extended(const char *src, uint32_t *size, shark_type type, shark_opt opt,
                                             int32_t fm, int32_t fp, int32_t fi) {
	(void)opt; (void)fm; (void)fp; (void)fi;
	if (!g_sharkReady) return NULL;
	if (strlen(src) != *size) mock_fail("shark: size %u != strlen %zu", *size, strlen(src));
	const char *m = strstr(src, "main(");
	while (m && m > src && (isalnum((unsigned char)m[-1]) || m[-1] == '_')) m = strstr(m + 1, "main(");
	if (!m) { if (g_sharkLog) g_sharkLog("no main()", SHARK_LOG_ERROR, 1); return NULL; }
	/* like the real compiler: no integer divides with a variable denominator.
	 * Cg has no float %, so any % is an integer remainder. */
	if (strstr(src, " % ")) {
		if (g_sharkLog)
			g_sharkLog("does not support integer divides with arbitrary denominators", SHARK_LOG_ERROR, 1);
		return NULL;
	}
	/* like the real compiler: there is no uint type */
	for (const char *k = strstr(src, "uint"); k; k = strstr(k + 1, "uint")) {
		int before = k > src && (isalnum((unsigned char)k[-1]) || k[-1] == '_');
		int after = k[4] && strchr("234", k[4]) ? isalnum((unsigned char)k[5]) || k[5] == '_'
		                                        : isalnum((unsigned char)k[4]) || k[4] == '_';
		if (!before && !after) {
			if (g_sharkLog) g_sharkLog("expected type specifier, but found 'identifier' instead (uint)", SHARK_LOG_ERROR, 1);
			return NULL;
		}
	}
	/* like the real compiler: "row" is a keyword */
	for (const char *k = strstr(src, "row"); k; k = strstr(k + 1, "row")) {
		int before = k > src && (isalnum((unsigned char)k[-1]) || k[-1] == '_');
		int after = isalnum((unsigned char)k[3]) || k[3] == '_';
		if (!before && !after) {
			if (g_sharkLog) g_sharkLog("expected an identifier but found keyword 'row'", SHARK_LOG_ERROR, 1);
			return NULL;
		}
	}
	SceGxmProgram *p = calloc(1, sizeof(*p));
	memcpy(p->magic, "GXP", 4);
	p->type = type == SHARK_VERTEX_SHADER ? SCE_GXM_VERTEX_PROGRAM : SCE_GXM_FRAGMENT_PROGRAM;
	const char *s = m + 5;
	int depth = 0;
	unsigned attrIdx = 0, ub = 0, tex = 0;

	/* top-level "uniform TYPE NAME[n] : SEM;" declarations */
	for (const char *l = src; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		if (strncmp(l, "uniform ", 8)) continue;
		char ty[40], nm[64], sem[32] = "";
		if (sscanf(l + 8, "%39s %63[^ ;:]", ty, nm) != 2) continue;
		const char *colon = strchr(l, ':'), *eol = strchr(l, ';');
		if (colon && eol && colon < eol) sscanf(colon + 1, " %31[^ ;]", sem);
		unsigned arrDecl = 1;
		char *br = strchr(nm, '[');
		if (br) { arrDecl = (unsigned)atoi(br + 1); *br = 0; }
		if (p->count >= MP_MAX) break;
		struct SceGxmProgramParameter *pp = &p->p[p->count];
		memset(pp, 0, sizeof(*pp));
		snprintf(pp->name, sizeof(pp->name), "%s", nm);
		pp->type = SCE_GXM_PARAMETER_TYPE_F32;
		if (!strncmp(ty, "sampler", 7)) {
			pp->cat = SCE_GXM_PARAMETER_CATEGORY_SAMPLER;
			pp->res = !strncmp(sem, "TEXUNIT", 7) ? (unsigned)atoi(sem + 7) : tex;
			tex++;
			pp->comps = 4; pp->arr = 1;
			p->count++;
		} else if (!strncmp(ty, "float", 5) || !strncmp(ty, "half", 4)) {
			pp->cat = SCE_GXM_PARAMETER_CATEGORY_UNIFORM;
			pp->comps = type_comps(ty, &pp->arr);
			pp->arr *= arrDecl;
			pp->container = 14;
			pp->res = ub;
			ub += pp->comps * pp->arr;
			p->count++;
		}
	}
	char param[256];
	size_t n = 0;
	for (;; s++) {
		char c = *s;
		if (!c) break;
		if (c == '(') depth++;
		if ((c == ',' && depth == 0) || (c == ')' && depth == 0)) {
			param[n] = 0;
			n = 0;
			/* tokenize */
			char *sem = strchr(param, ':');
			char semantic[32] = "";
			if (sem) { *sem = 0; sscanf(sem + 1, " %31s", semantic); }
			char tok[8][40];
			int nt = 0;
			for (char *t = strtok(param, " \t\r\n"); t && nt < 8; t = strtok(NULL, " \t\r\n")) snprintf(tok[nt++], 40, "%s", t);
			if (nt >= 2) {
				int isUniform = 0, isOut = 0;
				for (int i = 0; i < nt; i++) { if (!strcmp(tok[i], "uniform")) isUniform = 1; if (!strcmp(tok[i], "out")) isOut = 1; }
				const char *name = tok[nt - 1], *ty = tok[nt - 2];
				if (!isOut && p->count < MP_MAX) {
					struct SceGxmProgramParameter *pp = &p->p[p->count];
					snprintf(pp->name, sizeof(pp->name), "%s", name);
					pp->type = SCE_GXM_PARAMETER_TYPE_F32;
					pp->container = 0;
					if (isUniform && !strncmp(ty, "sampler", 7)) {
						pp->cat = SCE_GXM_PARAMETER_CATEGORY_SAMPLER;
						pp->res = !strncmp(semantic, "TEXUNIT", 7) ? (unsigned)atoi(semantic + 7) : tex;
						tex++;
						pp->comps = 4; pp->arr = 1;
						p->count++;
					} else if (isUniform) {
						pp->cat = SCE_GXM_PARAMETER_CATEGORY_UNIFORM;
						pp->comps = type_comps(ty, &pp->arr);
						pp->container = 14;
						pp->res = ub;
						ub += pp->comps * pp->arr;
						p->count++;
					} else if (type == SHARK_VERTEX_SHADER && (!strcmp(semantic, "INDEX") || !strcmp(semantic, "INSTANCE"))) {
						/* filled by the GPU, like on hardware no attribute feeds it */
					} else if (type == SHARK_VERTEX_SHADER) {
						pp->cat = SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE;
						pp->comps = type_comps(ty, &pp->arr);
						pp->res = attrIdx++ * 4;
						if (!strncmp(semantic, "ATTR", 4)) { pp->semantic = SCE_GXM_PARAMETER_SEMANTIC_ATTR; pp->semIdx = atoi(semantic + 4); }
						p->count++;
					}
				}
			}
			if (c == ')') break;
			continue;
		}
		if (c == ')') depth--;
		if (n < sizeof(param) - 1) param[n++] = c;
	}
	p->ubSize = ub * 4;
	*size = sizeof(*p);
	g_sharkOut = p;
	return p;
}

/* ------------------------------------------------------------------------ */
/* GXM objects                                                              */
/* ------------------------------------------------------------------------ */
struct SceGxmContext { int dummy; };
struct SceGxmShaderPatcher { int registered, vps, fps; };
struct SceGxmRegisteredProgram { const SceGxmProgram *prog; int refs; struct SceGxmShaderPatcher *owner; };
struct SceGxmVertexProgram {
	const SceGxmProgram *prog; struct SceGxmRegisteredProgram *reg;
	SceGxmVertexAttribute attrs[16]; unsigned na; SceGxmVertexStream streams[16]; unsigned ns;
};
struct SceGxmFragmentProgram { const SceGxmProgram *prog; struct SceGxmRegisteredProgram *reg; SceGxmMultisampleMode ms; };
struct SceGxmRenderTarget { unsigned w, h; SceGxmMultisampleMode ms; };
struct SceGxmSyncObject { int dummy; };

static SceGxmDisplayQueueCallback *g_displayCb;
static unsigned g_displayDataSize;
static int g_gxmInit;
static volatile unsigned int g_notif[512];

int sceGxmInitialize(const SceGxmInitializeParams *p) {
	if (g_gxmInit) mock_fail("sceGxmInitialize twice");
	if (p->parameterBufferSize < 0x40000) mock_fail("parameter buffer too small");
	g_displayCb = p->displayQueueCallback;
	g_displayDataSize = p->displayQueueCallbackDataSize;
	g_gxmInit = 1;
	return 0;
}
int sceGxmTerminate(void) { g_gxmInit = 0; return 0; }
volatile unsigned int *sceGxmGetNotificationRegion(void) { return g_notif; }

static int map_check(void *base, SceSize size, const char *what) {
	int b = find_block(base);
	if (b < 0) { mock_fail("%s: %p is not a memblock base", what, base); return -1; }
	if (size > g_blocks[b].size) mock_fail("%s: size %u > block %zu", what, size, g_blocks[b].size);
	g_blocks[b].mapped = 1;
	return 0;
}
static int unmap(void *base) { int b = find_block(base); if (b < 0) { mock_fail("unmap: unknown %p", base); return -1; } g_blocks[b].mapped = 0; return 0; }
int sceGxmMapMemory(void *base, SceSize size, SceGxmMemoryAttribFlags attr) { (void)attr; return map_check(base, size, "sceGxmMapMemory"); }
int sceGxmUnmapMemory(void *base) { return unmap(base); }
int sceGxmMapVertexUsseMemory(void *base, SceSize size, unsigned int *off) { *off = 0; return map_check(base, size, "MapVertexUsse"); }
int sceGxmMapFragmentUsseMemory(void *base, SceSize size, unsigned int *off) { *off = 0; return map_check(base, size, "MapFragmentUsse"); }
int sceGxmUnmapVertexUsseMemory(void *base) { return unmap(base); }
int sceGxmUnmapFragmentUsseMemory(void *base) { return unmap(base); }

static struct SceGxmContext g_ctx;
int sceGxmCreateContext(const SceGxmContextParams *p, SceGxmContext **ctx) {
	if (!p->hostMem || p->hostMemSize < SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE) mock_fail("context host mem");
	touch(p->vdmRingBufferMem, p->vdmRingBufferMemSize);
	touch(p->vertexRingBufferMem, p->vertexRingBufferMemSize);
	touch(p->fragmentRingBufferMem, p->fragmentRingBufferMemSize);
	touch(p->fragmentUsseRingBufferMem, p->fragmentUsseRingBufferMemSize);
	*ctx = &g_ctx;
	return 0;
}
int sceGxmDestroyContext(SceGxmContext *ctx) { (void)ctx; return 0; }

int sceGxmShaderPatcherCreate(const SceGxmShaderPatcherParams *p, SceGxmShaderPatcher **out) {
	touch(p->bufferMem, p->bufferMemSize);
	touch(p->vertexUsseMem, p->vertexUsseMemSize);
	touch(p->fragmentUsseMem, p->fragmentUsseMemSize);
	*out = calloc(1, sizeof(**out));
	return 0;
}
int sceGxmShaderPatcherDestroy(SceGxmShaderPatcher *sp) {
	if (sp->registered || sp->vps || sp->fps)
		mock_fail("patcher destroyed with %d programs, %d vertex, %d fragment programs alive", sp->registered, sp->vps, sp->fps);
	free(sp);
	return 0;
}
int sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher *sp, const SceGxmProgram *prog, SceGxmShaderPatcherId *id) {
	if (sceGxmProgramCheck(prog) < 0) { mock_fail("RegisterProgram: invalid program"); return -1; }
	struct SceGxmRegisteredProgram *r = calloc(1, sizeof(*r));
	r->prog = prog; r->owner = sp;
	sp->registered++;
	*id = r;
	return 0;
}
int sceGxmShaderPatcherUnregisterProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id) {
	if (id->refs) return (int)0x805B0010; /* still in use, like the real patcher */
	sp->registered--;
	free(id);
	return 0;
}

int sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
	const SceGxmVertexAttribute *attrs, unsigned int na, const SceGxmVertexStream *streams, unsigned int ns,
	SceGxmVertexProgram **out) {
	const SceGxmProgram *prog = id->prog;
	if (prog->type != SCE_GXM_VERTEX_PROGRAM) { mock_fail("CreateVertexProgram with a fragment program"); return -1; }
	if (na > 16 || ns > 16) { mock_fail("too many attributes/streams"); return -1; }
	for (unsigned i = 0; i < na; i++) {
		if (attrs[i].streamIndex >= ns) mock_fail("attribute %u uses stream %u >= %u", i, attrs[i].streamIndex, ns);
		if (attrs[i].componentCount < 1 || attrs[i].componentCount > 4) mock_fail("attribute %u bad component count", i);
		if (attrs[i].format > SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED) mock_fail("attribute %u bad format", i);
		int found = 0;
		for (unsigned k = 0; k < prog->count; k++)
			if (prog->p[k].cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE && prog->p[k].res == attrs[i].regIndex) found = 1;
		if (!found) mock_fail("attribute %u regIndex %u matches no shader attribute", i, attrs[i].regIndex);
	}
	for (unsigned k = 0; k < prog->count; k++) {
		if (prog->p[k].cat != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) continue;
		int fed = 0;
		for (unsigned i = 0; i < na; i++) if (attrs[i].regIndex == prog->p[k].res) fed = 1;
		if (!fed) mock_fail("shader attribute '%s' is not fed by any vertex attribute", prog->p[k].name);
	}
	for (unsigned s = 0; s + 1 < ns; s++)
		if (streams[s].indexSource != streams[s + 1].indexSource) mock_fail("mixed stream index sources");
	struct SceGxmVertexProgram *vp = calloc(1, sizeof(*vp));
	vp->prog = prog; vp->reg = id; vp->na = na; vp->ns = ns;
	if (na) memcpy(vp->attrs, attrs, na * sizeof(*attrs));
	if (ns) memcpy(vp->streams, streams, ns * sizeof(*streams));
	id->refs++;
	sp->vps++;
	*out = vp;
	return 0;
}

int sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher *sp, SceGxmShaderPatcherId id,
	SceGxmOutputRegisterFormat fmt, SceGxmMultisampleMode ms, const SceGxmBlendInfo *blend,
	const SceGxmProgram *vprog, SceGxmFragmentProgram **out) {
	(void)fmt; (void)blend;
	if (ms != SCE_GXM_MULTISAMPLE_NONE && ms != SCE_GXM_MULTISAMPLE_2X && ms != SCE_GXM_MULTISAMPLE_4X)
		mock_fail("fragment program multisample mode %d", (int)ms);
	if (id->prog->type != SCE_GXM_FRAGMENT_PROGRAM) { mock_fail("CreateFragmentProgram with a vertex program"); return -1; }
	if (!vprog || vprog->type != SCE_GXM_VERTEX_PROGRAM) mock_fail("CreateFragmentProgram: missing vertex program");
	struct SceGxmFragmentProgram *fp = calloc(1, sizeof(*fp));
	fp->prog = id->prog; fp->reg = id; fp->ms = ms;
	id->refs++;
	sp->fps++;
	*out = fp;
	return 0;
}
int sceGxmShaderPatcherReleaseVertexProgram(SceGxmShaderPatcher *sp, SceGxmVertexProgram *vp) {
	vp->reg->refs--; sp->vps--; free(vp); return 0;
}
int sceGxmShaderPatcherReleaseFragmentProgram(SceGxmShaderPatcher *sp, SceGxmFragmentProgram *fp) {
	fp->reg->refs--; sp->fps--; free(fp); return 0;
}

int sceGxmSyncObjectCreate(SceGxmSyncObject **s) { *s = calloc(1, sizeof(**s)); return 0; }
int sceGxmSyncObjectDestroy(SceGxmSyncObject *s) { free(s); return 0; }
int sceGxmCreateRenderTarget(const SceGxmRenderTargetParams *p, SceGxmRenderTarget **rt) {
	if (!p->width || !p->height || p->width > 4096 || p->height > 4096) mock_fail("render target size %ux%u", p->width, p->height);
	if (!p->scenesPerFrame) mock_fail("scenesPerFrame 0");
	struct SceGxmRenderTarget *r = calloc(1, sizeof(*r));
	r->w = p->width; r->h = p->height; r->ms = p->multisampleMode;
	if (r->ms != SCE_GXM_MULTISAMPLE_NONE && r->ms != SCE_GXM_MULTISAMPLE_2X && r->ms != SCE_GXM_MULTISAMPLE_4X)
		mock_fail("render target multisample mode %d", (int)r->ms);
	*rt = r;
	return 0;
}
int sceGxmDestroyRenderTarget(SceGxmRenderTarget *rt) { free(rt); return 0; }

/* --- surfaces (pointer + dims stashed in the opaque words) --------------- */
static unsigned color_bpp(SceGxmColorFormat f) {
	switch (f) {
	case SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR: case SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB: return 4;
	case SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR: return 8;
	case SCE_GXM_COLOR_FORMAT_U5U6U5_RGB: return 2;
	default: return 4;
	}
}
int sceGxmColorSurfaceInit(SceGxmColorSurface *s, SceGxmColorFormat fmt, SceGxmColorSurfaceType type,
	SceGxmColorSurfaceScaleMode scale, SceGxmOutputRegisterSize ors, unsigned int w, unsigned int h,
	unsigned int stride, void *data) {
	(void)type; (void)ors;
	memset(s, 0, sizeof(*s));
	if (!data || ((uintptr_t)data & 3)) mock_fail("color surface data %p", data);
	if (stride < w) mock_fail("color surface stride %u < width %u", stride, w);
	memcpy(&s->pbeEmitWords[0], &data, sizeof(void *));
	s->pbeEmitWords[2] = w; s->pbeEmitWords[3] = h; s->pbeEmitWords[4] = stride; s->pbeEmitWords[5] = color_bpp(fmt);
	s->pbeSidebandWord = scale;
	return 0;
}
int sceGxmColorSurfaceInitDisabled(SceGxmColorSurface *s) { memset(s, 0, sizeof(*s)); return 0; }
int sceGxmDepthStencilSurfaceInit(SceGxmDepthStencilSurface *s, SceGxmDepthStencilFormat fmt,
	SceGxmDepthStencilSurfaceType type, unsigned int stride, void *depth, void *stencil) {
	(void)fmt; (void)type; (void)stencil;
	memset(s, 0, sizeof(*s));
	if (stride % 32) mock_fail("depth stride %u not tile aligned", stride);
	s->depthData = depth;
	s->zlsControl = stride;
	s->backgroundDepth = 1.0f;
	return 0;
}
void sceGxmDepthStencilSurfaceSetBackgroundDepth(SceGxmDepthStencilSurface *s, float d) {
	if (d < 0.0f || d > 1.0f) mock_fail("background depth %f", d);
	s->backgroundDepth = d;
}
void sceGxmDepthStencilSurfaceSetBackgroundStencil(SceGxmDepthStencilSurface *s, unsigned char st) { (void)s; (void)st; }
void sceGxmDepthStencilSurfaceSetForceLoadMode(SceGxmDepthStencilSurface *s, SceGxmDepthStencilForceLoadMode m) { (void)s; (void)m; }
void sceGxmDepthStencilSurfaceSetForceStoreMode(SceGxmDepthStencilSurface *s, SceGxmDepthStencilForceStoreMode m) { (void)s; (void)m; }

/* --- textures ------------------------------------------------------------ */
static unsigned tex_bpp(SceGxmTextureFormat f) {
	switch (f) {
	case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR: case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB: return 4;
	case SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB: return 2;
	default: return 0;
	}
}
typedef struct { void *data; uint32_t wh; uint32_t fmt; } MockTex;
_Static_assert(sizeof(MockTex) <= sizeof(SceGxmTexture), "mock texture stash");

/* bytes each texture reads, by data pointer, so check_texture can touch the whole chain */
#define MOCK_TEX_SIZES 512
static struct { const void *data; size_t bytes; } g_texSizes[MOCK_TEX_SIZES];
static unsigned g_texSizeNext;
static void tex_record(const void *data, size_t bytes) {
	for (unsigned i = 0; i < MOCK_TEX_SIZES; i++)
		if (g_texSizes[i].data == data) { g_texSizes[i].bytes = bytes; return; }
	g_texSizes[g_texSizeNext].data = data;
	g_texSizes[g_texSizeNext].bytes = bytes;
	g_texSizeNext = (g_texSizeNext + 1) % MOCK_TEX_SIZES;
}
static size_t tex_lookup(const void *data) {
	for (unsigned i = 0; i < MOCK_TEX_SIZES; i++) if (g_texSizes[i].data == data) return g_texSizes[i].bytes;
	return 0;
}
static unsigned tex_block(SceGxmTextureFormat f) {
	switch ((uint32_t)f & 0x9F000000u) {
	case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1: case SCE_GXM_TEXTURE_BASE_FORMAT_UBC4: return 8;
	case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2: case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3:
	case SCE_GXM_TEXTURE_BASE_FORMAT_UBC5: return 16;
	default: return 0;
	}
}
static int tex_init(SceGxmTexture *t, const void *data, SceGxmTextureFormat fmt, unsigned w, unsigned h,
                    unsigned mips, int swizzled, unsigned faces) {
	if (!data || ((uintptr_t)data & 15)) mock_fail("texture data %p not 16-byte aligned", data);
	if (!w || !h || w > 4096 || h > 4096) mock_fail("texture size %ux%u", w, h);
	if (swizzled && ((w & (w - 1)) || (h & (h - 1)))) mock_fail("swizzled texture %ux%u is not power of two", w, h);
	if (!mips) mips = 1;
	unsigned bpp = tex_bpp(fmt), blk = tex_block(fmt);
	if (!swizzled && blk) mock_fail("compressed texture with a linear layout");
	size_t bytes = 0;
	for (unsigned l = 0; l < mips; l++) {
		unsigned lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
		size_t sz = blk ? (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * blk : (size_t)((lw + 7) & ~7u) * lh * bpp;
		bytes += (sz + 15) & ~(size_t)15;
	}
	MockTex m = { (void *)data, w | (h << 16), (uint32_t)fmt };
	memset(t, 0, sizeof(*t));
	memcpy(t, &m, sizeof(m));
	tex_record(data, bytes * faces);
	return 0;
}
int sceGxmTextureInitLinear(SceGxmTexture *t, const void *data, SceGxmTextureFormat fmt, unsigned int w,
                            unsigned int h, unsigned int mips) {
	return tex_init(t, data, fmt, w, h, mips, 0, 1);
}
int sceGxmTextureInitSwizzled(SceGxmTexture *t, const void *data, SceGxmTextureFormat fmt, unsigned int w,
                              unsigned int h, unsigned int mips) {
	return tex_init(t, data, fmt, w, h, mips, 1, 1);
}
int sceGxmTextureInitCube(SceGxmTexture *t, const void *data, SceGxmTextureFormat fmt, unsigned int w,
                          unsigned int h, unsigned int mips) {
	if (w != h) mock_fail("cube texture %ux%u is not square", w, h);
	return tex_init(t, data, fmt, w, h, mips, 1, 6);
}
int sceGxmTextureSetMipFilter(SceGxmTexture *t, SceGxmTextureMipFilter f) { (void)t; (void)f; return 0; }
int sceGxmTextureSetLodBias(SceGxmTexture *t, unsigned int bias) {
	(void)t;
	if (bias > 63) mock_fail("lod bias %u > 63", bias);
	return 0;
}
int sceGxmTextureSetMipmapCount(SceGxmTexture *t, unsigned int n) { (void)t; if (!n || n > 13) mock_fail("mip count %u", n); return 0; }
int sceGxmTextureSetGammaMode(SceGxmTexture *t, SceGxmTextureGammaMode g) { (void)t; (void)g; return 0; }
int sceGxmColorSurfaceSetGammaMode(SceGxmColorSurface *s, SceGxmColorSurfaceGammaMode g) { (void)s; (void)g; return 0; }
int sceGxmTextureSetMinFilter(SceGxmTexture *t, SceGxmTextureFilter f) { (void)t; (void)f; return 0; }
int sceGxmTextureSetMagFilter(SceGxmTexture *t, SceGxmTextureFilter f) { (void)t; (void)f; return 0; }
int sceGxmTextureSetUAddrMode(SceGxmTexture *t, SceGxmTextureAddrMode m) { (void)t; (void)m; return 0; }
int sceGxmTextureSetVAddrMode(SceGxmTexture *t, SceGxmTextureAddrMode m) { (void)t; (void)m; return 0; }

static void check_texture(const SceGxmTexture *t, const char *what) {
	MockTex m;
	memcpy(&m, t, sizeof(m));
	if (!m.data) { mock_fail("%s: texture has no data", what); return; }
	size_t bytes = tex_lookup(m.data);
	if (bytes) touch(m.data, bytes);
}

/* ------------------------------------------------------------------------ */
/* Context state + draws                                                    */
/* ------------------------------------------------------------------------ */
static int g_inScene;
static SceGxmMultisampleMode g_sceneMs;
static const struct SceGxmVertexProgram *g_vp;
static const struct SceGxmFragmentProgram *g_fp;
static const void *g_streams[16];
static SceGxmTexture g_ftex[16];
static int g_ftexSet[16];
static void *g_vub, *g_fub;
static unsigned g_vubSize, g_fubSize;
static int g_vubFresh, g_fubFresh;
#define MAX_UB 4096
static void *g_ubs[MAX_UB];
static int g_ubCount;

static void free_ubs(void) { for (int i = 0; i < g_ubCount; i++) free(g_ubs[i]); g_ubCount = 0; }

int sceGxmBeginScene(SceGxmContext *ctx, unsigned int flags, const SceGxmRenderTarget *rt,
	const SceGxmValidRegion *vr, SceGxmSyncObject *vs, SceGxmSyncObject *fs, const SceGxmColorSurface *cs,
	const SceGxmDepthStencilSurface *ds) {
	(void)ctx; (void)flags; (void)vr; (void)vs; (void)fs;
	if (g_inScene) mock_fail("BeginScene inside a scene");
	if (!rt) mock_fail("BeginScene without render target");
	void *data = NULL;
	memcpy(&data, &cs->pbeEmitWords[0], sizeof(void *));
	if (data) {
		unsigned w = cs->pbeEmitWords[2], h = cs->pbeEmitWords[3], stride = cs->pbeEmitWords[4], bpp = cs->pbeEmitWords[5];
		if (rt && (w != rt->w || h != rt->h)) mock_fail("color surface %ux%u != render target %ux%u", w, h, rt->w, rt->h);
		unsigned scale = cs->pbeSidebandWord;
		if (rt && rt->ms != SCE_GXM_MULTISAMPLE_NONE && scale != SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE)
			mock_fail("multisampled render target with a color surface that is not downscaled");
		if (rt && rt->ms == SCE_GXM_MULTISAMPLE_NONE && scale == SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE)
			mock_fail("downscaled color surface on a render target without multisampling");
		touch(data, (size_t)stride * bpp * (h - 1) + (size_t)w * bpp);
	}
	if (ds) {
		unsigned stride = ds->zlsControl;
		if (!ds->depthData) mock_fail("depth surface without data");
		else if (rt) {
			/* every sample is stored: 2x doubles the rows, 4x rows and columns */
			unsigned aw = (rt->w + 31) & ~31u, ah = (rt->h + 31) & ~31u;
			unsigned needW = rt->ms == SCE_GXM_MULTISAMPLE_4X ? aw * 2 : aw;
			unsigned rows = rt->ms == SCE_GXM_MULTISAMPLE_NONE ? ah : ah * 2;
			if (stride < needW) mock_fail("depth stride %u, the render target needs %u samples per row", stride, needW);
			touch(ds->depthData, (size_t)stride * rows * 4);
		}
	}
	g_inScene = 1;
	g_sceneMs = rt ? rt->ms : SCE_GXM_MULTISAMPLE_NONE;
	g_scenes++;
	memset(g_ftexSet, 0, sizeof(g_ftexSet));
	g_vp = NULL; g_fp = NULL;
	return 0;
}

int sceGxmEndScene(SceGxmContext *ctx, const SceGxmNotification *vn, const SceGxmNotification *fn) {
	(void)ctx;
	if (!g_inScene) mock_fail("EndScene outside a scene");
	g_inScene = 0;
	if (vn) *vn->address = vn->value;
	if (fn) *fn->address = fn->value; /* the "GPU" finishes instantly */
	free_ubs();
	return 0;
}

void sceGxmFinish(SceGxmContext *ctx) { (void)ctx; }
int sceGxmTransferFinish(void) { return 0; }
int sceGxmDisplayQueueFinish(void) { return 0; }

int sceGxmDisplayQueueAddEntry(SceGxmSyncObject *oldBuf, SceGxmSyncObject *newBuf, const void *data) {
	if (!oldBuf || !newBuf) mock_fail("display entry without sync objects");
	g_presents++;
	if (g_displayCb) g_displayCb(data);
	return 0;
}

int sceDisplaySetFrameBuf(const SceDisplayFrameBuf *fb, SceDisplaySetBufSync sync) {
	(void)sync;
	if (fb->size != sizeof(*fb)) mock_fail("frame buf size field");
	if (fb->pitch % 64) mock_fail("display pitch %u not a multiple of 64", fb->pitch);
	if (fb->pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) mock_fail("display pixel format");
	touch(fb->base, (size_t)fb->pitch * 4 * (fb->height - 1) + fb->width * 4);
	return 0;
}
int sceDisplayWaitVblankStart(void) { return 0; }

int sceGxmTransferFill(uint32_t color, SceGxmTransferFormat fmt, void *dst, uint32_t x, uint32_t y, uint32_t w,
	uint32_t h, int32_t stride, SceGxmSyncObject *sync, uint32_t flags, const SceGxmNotification *n) {
	(void)sync; (void)flags; (void)n;
	if (g_inScene) mock_fail("TransferFill inside a scene");
	if (fmt == SCE_GXM_TRANSFER_FORMAT_RAW16 || fmt == SCE_GXM_TRANSFER_FORMAT_RAW32 ||
	    fmt == SCE_GXM_TRANSFER_FORMAT_RAW64 || fmt == SCE_GXM_TRANSFER_FORMAT_RAW128)
		return SCE_GXM_ERROR_UNSUPPORTED;
	unsigned bpp = fmt == SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR ? 4 : 0;
	if (!bpp) { mock_fail("TransferFill format 0x%x", fmt); return -1; }
	for (uint32_t r = 0; r < h; r++) {
		uint8_t *row = (uint8_t *)dst + (size_t)(y + r) * stride + x * bpp;
		for (uint32_t i = 0; i < w; i++) memcpy(row + i * bpp, &color, bpp);
	}
	g_fills++;
	return 0;
}

static unsigned transfer_bpp(SceGxmTransferFormat f) {
	switch (f) {
	case SCE_GXM_TRANSFER_FORMAT_U8_R: return 1;
	case SCE_GXM_TRANSFER_FORMAT_U8U8_GR: case SCE_GXM_TRANSFER_FORMAT_RAW16: return 2;
	case SCE_GXM_TRANSFER_FORMAT_U8U8U8_BGR: return 3;
	case SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR: case SCE_GXM_TRANSFER_FORMAT_RAW32: return 4;
	case SCE_GXM_TRANSFER_FORMAT_RAW64: return 8;
	case SCE_GXM_TRANSFER_FORMAT_RAW128: return 16;
	default: return 0;
	}
}

static int g_transferCopies;
int sceGxmTransferCopy(uint32_t w, uint32_t h, uint32_t ckValue, uint32_t ckMask, SceGxmTransferColorKeyMode ckMode,
	SceGxmTransferFormat sf, SceGxmTransferType st, const void *src, uint32_t sx, uint32_t sy, int32_t sstride,
	SceGxmTransferFormat df, SceGxmTransferType dt, void *dst, uint32_t dx, uint32_t dy, int32_t dstride,
	SceGxmSyncObject *sync, uint32_t flags, const SceGxmNotification *n) {
	(void)ckValue; (void)ckMask; (void)sync; (void)flags; (void)n;
	if (g_inScene) mock_fail("TransferCopy inside a scene");
	if (ckMode != SCE_GXM_TRANSFER_COLORKEY_NONE) mock_fail("TransferCopy color key");
	if (st != SCE_GXM_TRANSFER_LINEAR || dt != SCE_GXM_TRANSFER_LINEAR) mock_fail("TransferCopy only tested linear");
	unsigned bpp = transfer_bpp(sf);
	if (!bpp || sf != df) { mock_fail("TransferCopy formats 0x%x 0x%x", sf, df); return -1; }
	if (!w || !h) mock_fail("TransferCopy empty");
	touch((const uint8_t *)src + (size_t)(sy + h - 1) * sstride + sx * bpp, (size_t)w * bpp);
	touch((uint8_t *)dst + (size_t)(dy + h - 1) * dstride + dx * bpp, (size_t)w * bpp);
	for (uint32_t r = 0; r < h; r++)
		memmove((uint8_t *)dst + (size_t)(dy + r) * dstride + dx * bpp,
		        (const uint8_t *)src + (size_t)(sy + r) * sstride + sx * bpp, (size_t)w * bpp);
	g_transferCopies++;
	return 0;
}

void sceGxmSetVertexProgram(SceGxmContext *c, const SceGxmVertexProgram *vp) { (void)c; g_vp = vp; g_vubFresh = 0; }
void sceGxmSetFragmentProgram(SceGxmContext *c, const SceGxmFragmentProgram *fp) { (void)c; g_fp = fp; g_fubFresh = 0; }
int sceGxmSetVertexStream(SceGxmContext *c, unsigned int i, const void *d) {
	(void)c;
	if (i >= 16) { mock_fail("stream index %u", i); return -1; }
	g_streams[i] = d;
	return 0;
}
int sceGxmSetFragmentTexture(SceGxmContext *c, unsigned int i, const SceGxmTexture *t) {
	(void)c;
	if (i >= 16) { mock_fail("texture unit %u", i); return -1; }
	g_ftex[i] = *t; g_ftexSet[i] = 1;
	check_texture(t, "SetFragmentTexture");
	return 0;
}
int sceGxmSetVertexTexture(SceGxmContext *c, unsigned int i, const SceGxmTexture *t) { (void)c; (void)i; check_texture(t, "SetVertexTexture"); return 0; }
int sceGxmSetVertexUniformBuffer(SceGxmContext *c, unsigned int i, const void *d) { (void)c; if (i >= 14 || !d) mock_fail("vertex UBO %u", i); return 0; }
int sceGxmSetFragmentUniformBuffer(SceGxmContext *c, unsigned int i, const void *d) { (void)c; if (i >= 14 || !d) mock_fail("fragment UBO %u", i); return 0; }

static int reserve(void **out, unsigned size) {
	if (g_ubCount >= MAX_UB) { mock_fail("too many uniform reservations in one scene"); return -1; }
	void *b = calloc(1, size ? size : 4);
	g_ubs[g_ubCount++] = b;
	*out = b;
	return 0;
}
int sceGxmReserveVertexDefaultUniformBuffer(SceGxmContext *c, void **ub) {
	(void)c;
	if (!g_inScene || !g_vp) { mock_fail("reserve vertex UB without scene/program"); return -1; }
	g_vubSize = g_vp->prog->ubSize;
	g_vubFresh = 1;
	return reserve(ub, g_vubSize) ? -1 : ((g_vub = *ub), 0);
}
int sceGxmReserveFragmentDefaultUniformBuffer(SceGxmContext *c, void **ub) {
	(void)c;
	if (!g_inScene || !g_fp) { mock_fail("reserve fragment UB without scene/program"); return -1; }
	g_fubSize = g_fp->prog->ubSize;
	g_fubFresh = 1;
	return reserve(ub, g_fubSize) ? -1 : ((g_fub = *ub), 0);
}
int sceGxmSetUniformDataF(void *ub, const SceGxmProgramParameter *p, unsigned int off, unsigned int count, const float *src) {
	if (!p || p->cat != SCE_GXM_PARAMETER_CATEGORY_UNIFORM) { mock_fail("SetUniformDataF on a non-uniform"); return -1; }
	unsigned size = ub == g_vub ? g_vubSize : ub == g_fub ? g_fubSize : 0;
	if (!size) { mock_fail("SetUniformDataF into an unknown buffer"); return -1; }
	if ((p->res + off + count) * 4 > size)
		mock_fail("uniform '%s' write [%u..%u) overflows %u-byte buffer", p->name, (p->res + off) * 4, (p->res + off + count) * 4, size);
	if (off + count > p->comps * p->arr) mock_fail("uniform '%s' write past the parameter", p->name);
	touch_all(src, count * 4);
	memcpy((uint8_t *)ub + (p->res + off) * 4, src, count * 4);
	g_uniformWrites++;
	return 0;
}

static unsigned attr_size(const SceGxmVertexAttribute *a) {
	static const unsigned sz[] = { 1, 1, 2, 2, 1, 1, 2, 2, 2, 4, 4 };
	return sz[a->format] * a->componentCount;
}

int sceGxmDraw(SceGxmContext *c, SceGxmPrimitiveType prim, SceGxmIndexFormat ifmt, const void *idx, unsigned int count) {
	(void)c; (void)prim;
	if (!g_inScene) { mock_fail("draw outside scene"); return -1; }
	if (!g_vp || !g_fp) { mock_fail("draw without programs"); return -1; }
	if (!idx || !count) { mock_fail("draw without indices"); return -1; }
	if (g_fp->ms != g_sceneMs) { mock_fail("draw: fragment program multisample mode %d, render target %d", (int)g_fp->ms, (int)g_sceneMs); return -1; }
	int want32 = ifmt == SCE_GXM_INDEX_FORMAT_U32;
	for (unsigned s = 0; s < g_vp->ns; s++) {
		int src32 = g_vp->streams[s].indexSource == SCE_GXM_INDEX_SOURCE_INDEX_32BIT;
		if (src32 != want32) mock_fail("draw with %s indices but vertex program streams use %s index source",
		                               want32 ? "32-bit" : "16-bit", src32 ? "32-bit" : "16-bit");
	}
	if (g_vp->prog->ubSize && !g_vubFresh) mock_fail("draw: vertex default uniform buffer not reserved");
	if (g_fp->prog->ubSize && !g_fubFresh) mock_fail("draw: fragment default uniform buffer not reserved");
	for (unsigned k = 0; k < g_fp->prog->count; k++)
		if (g_fp->prog->p[k].cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER && !g_ftexSet[g_fp->prog->p[k].res])
			mock_fail("draw: fragment sampler '%s' (unit %u) has no texture", g_fp->prog->p[k].name, g_fp->prog->p[k].res);
	for (unsigned i = 0; i < count; i++) {
		uint32_t v = want32 ? ((const uint32_t *)idx)[i] : ((const uint16_t *)idx)[i];
		for (unsigned a = 0; a < g_vp->na; a++) {
			const SceGxmVertexAttribute *at = &g_vp->attrs[a];
			const uint8_t *base = g_streams[at->streamIndex];
			if (!base) { mock_fail("draw: stream %u not set", at->streamIndex); return -1; }
			touch_all(base + (size_t)v * g_vp->streams[at->streamIndex].stride + at->offset, attr_size(at));
		}
	}
	g_vubFresh = g_fubFresh = 0;
	g_draws++;
	g_indices += count;
	return 0;
}

/* fixed-function state setters: just accepted */
void sceGxmSetCullMode(SceGxmContext *c, SceGxmCullMode m) { (void)c; (void)m; }
void sceGxmSetFrontDepthFunc(SceGxmContext *c, SceGxmDepthFunc f) { (void)c; (void)f; }
void sceGxmSetBackDepthFunc(SceGxmContext *c, SceGxmDepthFunc f) { (void)c; (void)f; }
void sceGxmSetFrontDepthWriteEnable(SceGxmContext *c, SceGxmDepthWriteMode m) { (void)c; (void)m; }
void sceGxmSetBackDepthWriteEnable(SceGxmContext *c, SceGxmDepthWriteMode m) { (void)c; (void)m; }
void sceGxmSetFrontPolygonMode(SceGxmContext *c, SceGxmPolygonMode m) { (void)c; (void)m; }
void sceGxmSetFrontPointLineWidth(SceGxmContext *c, unsigned int w) { (void)c; if (!w || w > 16) mock_fail("line width %u", w); }
void sceGxmSetBackPointLineWidth(SceGxmContext *c, unsigned int w) { (void)c; if (!w || w > 16) mock_fail("line width %u", w); }
static void *g_visBuf;
static unsigned g_visIndex, g_visOn;
int sceGxmSetVisibilityBuffer(SceGxmContext *c, void *base, unsigned int stride) {
	(void)c;
	if (!base || stride < 4) mock_fail("visibility buffer %p stride %u", base, stride);
	g_visBuf = base;
	touch(base, (size_t)stride * 4);
	return 0;
}
void sceGxmSetFrontVisibilityTestIndex(SceGxmContext *c, unsigned int i) { (void)c; g_visIndex = i; }
void sceGxmSetBackVisibilityTestIndex(SceGxmContext *c, unsigned int i) { (void)c; g_visIndex = i; }
void sceGxmSetFrontVisibilityTestOp(SceGxmContext *c, SceGxmVisibilityTestOp op) { (void)c; (void)op; }
void sceGxmSetBackVisibilityTestOp(SceGxmContext *c, SceGxmVisibilityTestOp op) { (void)c; (void)op; }
void sceGxmSetFrontVisibilityTestEnable(SceGxmContext *c, SceGxmVisibilityTestMode m) {
	(void)c;
	g_visOn = m == SCE_GXM_VISIBILITY_TEST_ENABLED;
	if (g_visOn && !g_visBuf) mock_fail("visibility test enabled without a visibility buffer");
}
void sceGxmSetBackVisibilityTestEnable(SceGxmContext *c, SceGxmVisibilityTestMode m) { (void)c; (void)m; }
void sceGxmSetBackPolygonMode(SceGxmContext *c, SceGxmPolygonMode m) { (void)c; (void)m; }
void sceGxmSetFrontStencilFunc(SceGxmContext *c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b,
                               SceGxmStencilOp d, unsigned char cm, unsigned char wm) { (void)c; (void)f; (void)a; (void)b; (void)d; (void)cm; (void)wm; }
void sceGxmSetBackStencilFunc(SceGxmContext *c, SceGxmStencilFunc f, SceGxmStencilOp a, SceGxmStencilOp b,
                              SceGxmStencilOp d, unsigned char cm, unsigned char wm) { (void)c; (void)f; (void)a; (void)b; (void)d; (void)cm; (void)wm; }
void sceGxmSetFrontStencilRef(SceGxmContext *c, unsigned int r) { (void)c; (void)r; }
void sceGxmSetBackStencilRef(SceGxmContext *c, unsigned int r) { (void)c; (void)r; }
void sceGxmSetFrontDepthBias(SceGxmContext *c, int f, int u) { (void)c; (void)f; (void)u; }
void sceGxmSetBackDepthBias(SceGxmContext *c, int f, int u) { (void)c; (void)f; (void)u; }
void sceGxmSetTwoSidedEnable(SceGxmContext *c, SceGxmTwoSidedMode m) { (void)c; (void)m; }
void sceGxmSetViewportEnable(SceGxmContext *c, SceGxmViewportMode m) { (void)c; (void)m; }
void sceGxmSetViewport(SceGxmContext *c, float xo, float xs, float yo, float ys, float zo, float zs) {
	(void)c; (void)xo; (void)yo; (void)zo;
	if (xs == 0.0f || ys == 0.0f) mock_fail("degenerate viewport scale");
	if (zs < 0.0f) mock_fail("negative depth range");
}
void sceGxmSetRegionClip(SceGxmContext *c, SceGxmRegionClipMode m, unsigned int x0, unsigned int y0, unsigned int x1,
                         unsigned int y1) {
	(void)c; (void)m;
	if (x1 < x0 || y1 < y0) mock_fail("region clip %u,%u-%u,%u inverted", x0, y0, x1, y1);
}

/* ------------------------------------------------------------------------ */
static void report_and_exit(void) {
	size_t leaked = 0;
	int blocks = 0;
	for (int i = 1; i < MAX_BLOCKS; i++) if (g_blocks[i].used) { leaked += g_blocks[i].size; blocks++; }
	printf("\n[mock] frames=%d presents=%d scenes=%d draws=%d indices=%d fills=%d copies=%d uniformWrites=%d "
	       "memblocks_left=%d (%zu KB) peak_cdram=%zu KB\n",
	       g_frame, g_presents, g_scenes, g_draws, g_indices, g_fills, g_transferCopies, g_uniformWrites, blocks, leaked / 1024, g_cdram / 1024);
	if (blocks) mock_fail("%d memory blocks were not freed", blocks);
	if (g_gxmInit) mock_fail("sceGxmTerminate was not called");
	if (!g_presents) mock_fail("nothing was presented");
	printf("[mock] %s (%d errors)\n", g_errors ? "FAILED" : "PASSED", g_errors);
	fflush(stdout);
	_exit(g_errors ? 1 : 0);
}

__attribute__((constructor)) static void mock_init(void) {
	const char *root = getenv("MOCK_ROOT");
	snprintf(g_root, sizeof(g_root), "%s", root ? root : "/tmp/vkvita_mockfs");
	char p[600];
	snprintf(p, sizeof(p), "%s", g_root); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/ux0", g_root); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/ux0/data", g_root); mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/ux0/data/vkvita", g_root); mkdir(p, 0755);
	const char *f = getenv("MOCK_FRAMES");
	if (f) g_maxFrames = atoi(f);
}
