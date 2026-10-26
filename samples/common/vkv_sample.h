/*
 * Small helper shared by the VkVita samples: instance, device, swapchain,
 * render pass (optional depth), framebuffers and per-frame sync.
 * hello_triangle does all of this by hand so you can see the full flow.
 * SPDX-License-Identifier: MIT
 */
#ifndef VKV_SAMPLE_H
#define VKV_SAMPLE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <vkvita/vkvita.h>

#define SAMPLE_FRAMES     2
#define SAMPLE_MAX_IMAGES 4

#define VK_CHECK(expr)                                                                 \
	do {                                                                               \
		VkResult r_ = (expr);                                                          \
		if (r_ != VK_SUCCESS) {                                                        \
			vkvLog(VKV_LOG_ERROR, "%s failed: %d (%s:%d)", #expr, (int)r_, __FILE__, __LINE__); \
			sample_fatal(#expr);                                                       \
		}                                                                              \
	} while (0)

typedef struct Sample {
	VkInstance       instance;
	VkPhysicalDevice phys;
	VkDevice         device;
	VkQueue          queue;
	VkSurfaceKHR     surface;

	VkSwapchainKHR   swapchain;
	VkFormat         format;
	VkExtent2D       extent;
	uint32_t         imageCount;
	VkImage          images[SAMPLE_MAX_IMAGES];
	VkImageView      views[SAMPLE_MAX_IMAGES];

	bool             withDepth;
	VkImage          depthImage;
	VkDeviceMemory   depthMemory;
	VkImageView      depthView;

	VkRenderPass     renderPass;
	VkFramebuffer    framebuffers[SAMPLE_MAX_IMAGES];

	VkCommandPool    cmdPool;
	VkCommandBuffer  cmd[SAMPLE_FRAMES];
	VkFence          fences[SAMPLE_FRAMES];
	uint32_t         frame;       /* frame-in-flight slot */
	uint32_t         imageIndex;  /* swapchain image being rendered */
	VkClearColorValue clearColor;

	uint32_t         buttons, pressed; /* SceCtrl state */
	int              lx, ly;           /* left stick, -128..127 */
} Sample;

/* Configure VkVita for a sample: logging to ux0:data/vkvita/<name>.log,
 * validation + profiling on, shader cache on. */
void     sample_configure(const char *name);
bool     sample_init(Sample *s, const char *appName, bool withDepth);
void     sample_shutdown(Sample *s);

/* Waits for the frame slot, acquires an image, begins the command buffer
 * and the render pass. Returns the command buffer to record into. */
VkCommandBuffer sample_begin_frame(Sample *s);
void            sample_end_frame(Sample *s);

/* Reads the pad; returns false when START is pressed. */
bool     sample_poll_input(Sample *s);

uint32_t sample_memory_type(Sample *s, uint32_t typeBits, VkMemoryPropertyFlags props);
void     sample_create_buffer(Sample *s, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                              VkBuffer *buffer, VkDeviceMemory *memory, void **mapped);
/* One-shot command buffer helpers for uploads. */
VkCommandBuffer sample_begin_once(Sample *s);
void            sample_end_once(Sample *s, VkCommandBuffer cb);

void     sample_fatal(const char *what) __attribute__((noreturn));

/* column-major 4x4 math for the samples */
void mat4_identity(float m[16]);
void mat4_mul(float out[16], const float a[16], const float b[16]);
void mat4_perspective(float m[16], float fovyRad, float aspect, float zn, float zf); /* Vulkan clip space */
void mat4_rotate_xyz(float m[16], float x, float y, float z);
void mat4_translate(float m[16], float x, float y, float z);

#endif
