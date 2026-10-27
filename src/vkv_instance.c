/*
 * VkVita: instance, physical device, surface queries.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

void vkv_load_default_config_file(void);

static const VkExtensionProperties k_instanceExts[] = {
	{ VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION },
	{ VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_SPEC_VERSION },
	{ VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION },
};

static const VkExtensionProperties k_deviceExts[] = {
	{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_SPEC_VERSION },
};

static const VkLayerProperties k_layers[] = {
	{ "VK_LAYER_KHRONOS_validation", VK_MAKE_API_VERSION(0, 1, 0, 0), 1,
	  "Maps to VkVita built-in validation (VKV_DEBUG_VALIDATION)" },
};

static VkResult vkv_enum_props(const void *src, size_t elemSize, uint32_t total, uint32_t *count, void *out) {
	if (!out) { *count = total; return VK_SUCCESS; }
	uint32_t n = VKV_MIN(*count, total);
	memcpy(out, src, n * elemSize);
	*count = n;
	return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
	*pApiVersion = VK_API_VERSION_1_0;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                      VkExtensionProperties *pProps) {
	if (pLayerName) { *pCount = 0; return VK_SUCCESS; }
	return vkv_enum_props(k_instanceExts, sizeof(k_instanceExts[0]), VKV_ARRAY_SIZE(k_instanceExts), pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProps) {
	return vkv_enum_props(k_layers, sizeof(k_layers[0]), VKV_ARRAY_SIZE(k_layers), pCount, pProps);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                VkInstance *pInstance) {
	(void)alloc;
	vkv_load_default_config_file();
	VKV_TRACE("vkCreateInstance");

	for (uint32_t i = 0; i < ci->enabledLayerCount; i++) {
		if (!strcmp(ci->ppEnabledLayerNames[i], "VK_LAYER_KHRONOS_validation")) {
			g_vkv.validationLayer = true;
			vkv_log(VKV_LOG_INFO, "validation layer requested: built-in checks enabled");
		} else {
			VKV_WARN("layer %s is not available", ci->ppEnabledLayerNames[i]);
			return VK_ERROR_LAYER_NOT_PRESENT;
		}
	}
	for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
		bool found = false;
		for (size_t j = 0; j < VKV_ARRAY_SIZE(k_instanceExts); j++)
			if (!strcmp(ci->ppEnabledExtensionNames[i], k_instanceExts[j].extensionName)) found = true;
		if (!found) {
			/* Debug utils / report are commonly requested; accept them silently-ish. */
			if (!strcmp(ci->ppEnabledExtensionNames[i], "VK_EXT_debug_utils") ||
			    !strcmp(ci->ppEnabledExtensionNames[i], "VK_EXT_debug_report")) {
				VKV_INFO("%s requested: messages go to the VkVita log instead", ci->ppEnabledExtensionNames[i]);
				continue;
			}
			VKV_ERROR("instance extension %s is not supported", ci->ppEnabledExtensionNames[i]);
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}
	}

	struct VkInstance_T *inst = calloc(1, sizeof(*inst));
	if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
	inst->obj.magic = VKV_MAGIC_INSTANCE;
	inst->phys.obj.magic = VKV_MAGIC_PHYSDEV;
	inst->phys.instance = inst;
	inst->apiVersion = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
	if (ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName)
		VKV_INFO("instance created for '%s'", ci->pApplicationInfo->pApplicationName);
	*pInstance = inst;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
	(void)alloc;
	if (!instance) return;
	instance->obj.magic = VKV_MAGIC_DEAD;
	free(instance);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pCount,
                                                          VkPhysicalDevice *pDevices) {
	if (!pDevices) { *pCount = 1; return VK_SUCCESS; }
	if (*pCount < 1) return VK_INCOMPLETE;
	pDevices[0] = &instance->phys;
	*pCount = 1;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
	(void)pd;
	memset(p, 0, sizeof(*p));
	p->apiVersion    = VK_API_VERSION_1_0;
	p->driverVersion = VK_MAKE_API_VERSION(0, VKVITA_VERSION_MAJOR, VKVITA_VERSION_MINOR, VKVITA_VERSION_PATCH);
	p->vendorID      = 0x1010;   /* Imagination Technologies */
	p->deviceID      = 0x0543;   /* SGX543MP4+ */
	p->deviceType    = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	strcpy(p->deviceName, "PS Vita SGX543MP4+ (VkVita over GXM)");
	VkPhysicalDeviceLimits *l = &p->limits;
	l->maxImageDimension1D = 4096;
	l->maxImageDimension2D = 4096;
	l->maxImageDimension3D = 1;
	l->maxImageDimensionCube = 4096;
	l->maxImageArrayLayers = 6;
	l->maxTexelBufferElements = 0;
	l->maxUniformBufferRange = 65536;
	l->maxStorageBufferRange = 65536;           /* read-only use, fed like uniform buffers */
	l->maxPerStageDescriptorStorageBuffers = 8;
	l->maxDescriptorSetStorageBuffers = 8;
	l->maxDescriptorSetStorageBuffersDynamic = 4;
	l->maxPushConstantsSize = VKV_MAX_PUSH;
	l->maxMemoryAllocationCount = 4096;
	l->maxSamplerAllocationCount = 4000;
	l->bufferImageGranularity = 1;
	l->maxBoundDescriptorSets = VKV_MAX_SETS;
	l->maxPerStageDescriptorSamplers = SCE_GXM_MAX_TEXTURE_UNITS;
	l->maxPerStageDescriptorUniformBuffers = SCE_GXM_MAX_UNIFORM_BUFFERS;
	l->maxPerStageDescriptorSampledImages = SCE_GXM_MAX_TEXTURE_UNITS;
	l->maxPerStageResources = SCE_GXM_MAX_TEXTURE_UNITS + SCE_GXM_MAX_UNIFORM_BUFFERS;
	l->maxDescriptorSetSamplers = SCE_GXM_MAX_TEXTURE_UNITS;
	l->maxDescriptorSetUniformBuffers = SCE_GXM_MAX_UNIFORM_BUFFERS;
	l->maxDescriptorSetUniformBuffersDynamic = 8;
	l->maxDescriptorSetSampledImages = SCE_GXM_MAX_TEXTURE_UNITS;
	l->maxVertexInputAttributes = SCE_GXM_MAX_VERTEX_ATTRIBUTES;
	l->maxVertexInputBindings = SCE_GXM_MAX_VERTEX_STREAMS;
	l->maxVertexInputAttributeOffset = 65535;
	l->maxVertexInputBindingStride = 65535;
	l->maxVertexOutputComponents = 40;  /* TEXCOORD0 to TEXCOORD9 */
	l->maxFragmentInputComponents = 40;
	l->maxFragmentOutputAttachments = 1;
	l->maxColorAttachments = 1;
	l->maxDrawIndexedIndexValue = 0xFFFFFFFFu;
	l->maxViewports = 1;
	l->maxViewportDimensions[0] = 4096;
	l->maxViewportDimensions[1] = 4096;
	l->viewportBoundsRange[0] = -8192.0f;
	l->viewportBoundsRange[1] = 8191.0f;
	l->minMemoryMapAlignment = 64;
	l->minUniformBufferOffsetAlignment = 16;
	l->minTexelBufferOffsetAlignment = 16;
	l->minStorageBufferOffsetAlignment = 16;
	l->maxFramebufferWidth = 4096;
	l->maxFramebufferHeight = 4096;
	l->maxFramebufferLayers = 1;
	l->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
	l->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
	l->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
	l->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
	l->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	l->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	l->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	l->maxSampleMaskWords = 1;
	l->timestampComputeAndGraphics = VK_TRUE;
	l->timestampPeriod = 1.0f;
	l->maxSamplerLodBias = 0.0f;
	l->maxSamplerAnisotropy = 1.0f;
	l->maxDrawIndirectCount = 0xFFFFFFFFu;
	l->maxClipDistances = 0;
	l->maxCullDistances = 0;
	l->discreteQueuePriorities = 2;
	l->pointSizeRange[0] = 1.0f;
	l->pointSizeRange[1] = 1.0f;
	l->lineWidthRange[0] = 1.0f;
	l->lineWidthRange[1] = 16.0f;
	l->lineWidthGranularity = 1.0f;
	l->optimalBufferCopyOffsetAlignment = 16;
	l->optimalBufferCopyRowPitchAlignment = 16;
	l->nonCoherentAtomSize = 64;
	p->sparseProperties = (VkPhysicalDeviceSparseProperties){ 0 };
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
	vkGetPhysicalDeviceProperties(pd, &p->properties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
	(void)pd;
	memset(f, 0, sizeof(*f));
	f->fillModeNonSolid = VK_TRUE;   /* LINE / POINT polygon modes */
	f->depthBiasClamp = VK_FALSE;
	f->samplerAnisotropy = VK_FALSE;
	f->fullDrawIndexUint32 = VK_TRUE;
	f->textureCompressionBC = VK_TRUE;
	f->wideLines = VK_TRUE;
	f->multiDrawIndirect = VK_TRUE;
	f->drawIndirectFirstInstance = VK_TRUE;
	f->occlusionQueryPrecise = VK_TRUE;
	f->independentBlend = VK_FALSE;
	f->shaderClipDistance = VK_FALSE;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
	vkGetPhysicalDeviceFeatures(pd, &f->features);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd,
                                                               VkPhysicalDeviceMemoryProperties *p) {
	(void)pd;
	memset(p, 0, sizeof(*p));
	p->memoryHeapCount = 2;
	p->memoryHeaps[0].size  = 112u * 1024 * 1024;   /* CDRAM visible to user apps (approx.) */
	p->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	p->memoryHeaps[1].size  = 256u * 1024 * 1024;   /* main memory share (approx.) */
	p->memoryHeaps[1].flags = 0;
	p->memoryTypeCount = VKV_MEMTYPE_COUNT;
	/* CDRAM: GPU-fast, CPU-mapped (uncached writes, slow reads). */
	p->memoryTypes[VKV_MEMTYPE_CDRAM].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	p->memoryTypes[VKV_MEMTYPE_CDRAM].heapIndex = 0;
	/* Uncached LPDDR: good for data the CPU rewrites every frame. */
	p->memoryTypes[VKV_MEMTYPE_RAM].propertyFlags =
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	p->memoryTypes[VKV_MEMTYPE_RAM].heapIndex = 1;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd,
                                                                VkPhysicalDeviceMemoryProperties2 *p) {
	vkGetPhysicalDeviceMemoryProperties(pd, &p->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t *pCount,
                                                                    VkQueueFamilyProperties *p) {
	(void)pd;
	if (!p) { *pCount = 1; return; }
	if (*pCount < 1) return;
	*pCount = 1;
	p[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
	p[0].queueCount = 1;
	p[0].timestampValidBits = 64;
	p[0].minImageTransferGranularity = (VkExtent3D){ 1, 1, 1 };
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format,
                                                               VkFormatProperties *p) {
	(void)pd;
	memset(p, 0, sizeof(*p));
	const VkvFormatInfo *fi = vkv_format(format);
	if (!fi) return;
	VkFormatFeatureFlags f = 0;
	if (vkv_format_sampleable(fi)) f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
	                  VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if (vkv_format_sampleable(fi) && !fi->block) f |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if (fi->renderable) f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	if (fi->isDepth) f |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	p->linearTilingFeatures  = f;
	p->optimalTilingFeatures = f;
	if (fi->attribComps) p->bufferFeatures = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat format,
	VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags,
	VkImageFormatProperties *p) {
	(void)pd; (void)tiling;
	const VkvFormatInfo *fi = vkv_format(format);
	memset(p, 0, sizeof(*p));
	if (!fi || type != VK_IMAGE_TYPE_2D) return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !vkv_format_sampleable(fi)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !fi->renderable) return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !fi->isDepth) return VK_ERROR_FORMAT_NOT_SUPPORTED;
	p->maxExtent = (VkExtent3D){ 4096, 4096, 1 };
	p->maxMipLevels = VKV_MAX_LEVELS;
	p->maxArrayLayers = (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? 6 : 1;
	if (fi->block && (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
	/* multisampling is for render targets that are only resolved, not read */
	if ((usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
	    !(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) && tiling == VK_IMAGE_TILING_OPTIMAL &&
	    !(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
		p->sampleCounts |= VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
	p->maxResourceSize = 64u * 1024 * 1024;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *pLayerName,
                                                                    uint32_t *pCount, VkExtensionProperties *p) {
	(void)pd;
	if (pLayerName) { *pCount = 0; return VK_SUCCESS; }
	return vkv_enum_props(k_deviceExts, sizeof(k_deviceExts[0]), VKV_ARRAY_SIZE(k_deviceExts), pCount, p);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice pd, uint32_t *pCount,
                                                                VkLayerProperties *p) {
	(void)pd;
	return vkv_enum_props(k_layers, sizeof(k_layers[0]), VKV_ARRAY_SIZE(k_layers), pCount, p);
}

/* --- Surface --- */
static VkvSurface s_surface = { { VKV_MAGIC_SURFACE } };

VkResult vkvCreateDisplaySurface(VkInstance instance, VkSurfaceKHR *surface) {
	(void)instance;
	*surface = VKV_H(VkSurfaceKHR, &s_surface);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateHeadlessSurfaceEXT(VkInstance instance,
	const VkHeadlessSurfaceCreateInfoEXT *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *surface) {
	(void)ci; (void)alloc;
	return vkvCreateDisplaySurface(instance, surface);
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                               const VkAllocationCallbacks *alloc) {
	(void)instance; (void)surface; (void)alloc;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t family,
                                                                    VkSurfaceKHR surface, VkBool32 *supported) {
	(void)pd; (void)surface;
	*supported = family == 0 ? VK_TRUE : VK_FALSE;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                         VkSurfaceCapabilitiesKHR *c) {
	(void)pd; (void)surface;
	memset(c, 0, sizeof(*c));
	c->minImageCount = 2;
	c->maxImageCount = VKV_MAX_SWAP_IMAGES;
	c->currentExtent = (VkExtent2D){ VKV_DISPLAY_WIDTH, VKV_DISPLAY_HEIGHT };
	c->minImageExtent = c->currentExtent;
	c->maxImageExtent = c->currentExtent;
	c->maxImageArrayLayers = 1;
	c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                    uint32_t *pCount, VkSurfaceFormatKHR *p) {
	(void)pd; (void)surface;
	/* The display scans out RGBA8. BGRA is accepted too (rendered as RGBA,
	 * so colors stay correct) because many Vulkan apps only look for it. */
	static const VkSurfaceFormatKHR fmts[] = {
		{ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
		{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
		{ VK_FORMAT_R8G8B8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
		{ VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
	};
	return vkv_enum_props(fmts, sizeof(fmts[0]), VKV_ARRAY_SIZE(fmts), pCount, p);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                         uint32_t *pCount, VkPresentModeKHR *p) {
	(void)pd; (void)surface;
	static const VkPresentModeKHR modes[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
	                                          VK_PRESENT_MODE_MAILBOX_KHR };
	return vkv_enum_props(modes, sizeof(modes[0]), VKV_ARRAY_SIZE(modes), pCount, p);
}

/* --- Proc addresses --- */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
	(void)instance;
	PFN_vkVoidFunction f = vkv_lookup_proc(pName);
	if (!f && vkv_dbg(VKV_DEBUG_LOG_UNSUPPORTED)) VKV_INFO("vkGetInstanceProcAddr: %s not implemented", pName);
	return f;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {
	(void)device;
	return vkGetInstanceProcAddr(NULL, pName);
}
