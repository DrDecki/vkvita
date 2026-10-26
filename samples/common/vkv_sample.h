// SPDX-License-Identifier: GPL-3.0-or-later
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
	uint32_t         frame;
	uint32_t         imageIndex;
	VkClearColorValue clearColor;

	uint32_t         buttons, pressed;
	int              lx, ly;
} Sample;

void     sample_configure(const char *name);
bool     sample_init(Sample *s, const char *appName, bool withDepth);
void     sample_shutdown(Sample *s);

VkCommandBuffer sample_begin_frame(Sample *s);
void            sample_end_frame(Sample *s);

bool     sample_poll_input(Sample *s);

uint32_t sample_memory_type(Sample *s, uint32_t typeBits, VkMemoryPropertyFlags props);
void     sample_create_buffer(Sample *s, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                              VkBuffer *buffer, VkDeviceMemory *memory, void **mapped);
VkCommandBuffer sample_begin_once(Sample *s);
void            sample_end_once(Sample *s, VkCommandBuffer cb);

void     sample_fatal(const char *what) __attribute__((noreturn));

void mat4_identity(float m[16]);
void mat4_mul(float out[16], const float a[16], const float b[16]);
void mat4_perspective(float m[16], float fovyRad, float aspect, float zn, float zf);
void mat4_rotate_xyz(float m[16], float x, float y, float z);
void mat4_translate(float m[16], float x, float y, float z);

#endif
