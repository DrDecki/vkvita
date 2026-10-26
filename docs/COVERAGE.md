# API coverage and limits

VkVita implements a Vulkan 1.0 subset (126 entry points plus 3 KHR aliases,
all reachable through `vkGetInstanceProcAddr`). Functions that are not
implemented are not defined at all, so a missing feature shows up as a link
error (or a NULL proc address) instead of silent misbehavior.

Legend: **full** works as specified (within the hardware limits below),
**partial** works with the noted restriction, **stub** accepted and logged
as unsupported.

## Instance, device, presentation

| Function(s) | Status | Notes |
|---|---|---|
| `vkCreateInstance`, `vkDestroyInstance`, `vkEnumerateInstance*` | full | Extensions: `VK_KHR_surface`, `VK_EXT_headless_surface`, `VK_KHR_get_physical_device_properties2`. `VK_EXT_debug_utils/report` are accepted, messages go to the VkVita log |
| `vkEnumeratePhysicalDevices`, `vkGetPhysicalDevice*` (+ `2`/`2KHR`) | full | One device, one queue family (graphics + transfer) |
| `vkCreateDevice`, `vkDestroyDevice`, `vkGetDeviceQueue`, `vkDeviceWaitIdle`, `vkQueueWaitIdle` | full | One device per process (GXM is process-global) |
| `vkCreateHeadlessSurfaceEXT`, `vkDestroySurfaceKHR`, `vkGetPhysicalDeviceSurface*` | full | The surface is the Vita display |
| `vkCreateSwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImage(2)KHR`, `vkQueuePresentKHR` | full | 960x544, 2 to 4 images, RGBA8/BGRA8 (+SRGB, treated as UNORM). FIFO and MAILBOX wait for vblank, IMMEDIATE does not. One swapchain at a time |

## Memory and resources

| Function(s) | Status | Notes |
|---|---|---|
| `vkAllocateMemory`, `vkFreeMemory`, `vkMapMemory`, `vkUnmapMemory`, `vkFlush/InvalidateMappedMemoryRanges` | full | Type 0: CDRAM (device local + host visible). Type 1: uncached main RAM. Both coherent. Allocations up to 512 KiB are sub-allocated |
| `vkCreateBuffer`, `vkBindBufferMemory`, `vkGetBufferMemoryRequirements` | full | Vertex, index, uniform, transfer usages. Storage/texel/indirect usages are accepted but not functional |
| `vkCreateImage`, `vkBindImageMemory`, `vkGetImageMemoryRequirements`, `vkGetImageSubresourceLayout` | partial | 2D only, 1 mip level, 1 layer, 1 sample. Stored linear (row pitch = width aligned to 8 texels). Depth formats all map to D24S8 |
| `vkCreateImageView` | partial | 2D views of level 0 / layer 0; component swizzles ignored |
| `vkCreateSampler` | partial | Nearest/linear, repeat/mirror/clamp. No border color, anisotropy, compare, LOD |

Formats usable as textures: RGBA8, BGRA8 (+SRGB), RGB8, RG8, R8, RGB565,
RGBA4444, RGBA5551, ARGB1555, RGBA16F, RG16F, R32F, RG32F, RG16 (U/S norm).
Render targets: RGBA8, BGRA8, RG8, R8, RGB565, RGBA16F, RG16F, R16F, R32F.
Compressed formats (BC/ETC/ASTC) are not supported.

## Pipelines and descriptors

| Function(s) | Status | Notes |
|---|---|---|
| `vkCreateShaderModule` | partial | SPIR-V (translated to Cg), Cg source or GXP binary, see SHADERS.md |
| `vkCreateGraphicsPipelines`, `vkDestroyPipeline` | partial | Vertex + fragment only. Topologies: point list, line list, triangle list/strip/fan. Blend ops add/sub/rev-sub/min/max, all factors except constant/dual-source. Polygon modes fill/line/point. Depth, stencil (two-sided), depth bias. Dynamic: viewport, scissor, depth bias, stencil masks/reference |
| `vkCreateComputePipelines` | stub | Returns `VK_ERROR_FEATURE_NOT_PRESENT` |
| `vkCreatePipelineLayout`, `vkCreatePipelineCache` (+ get/merge) | full | Pipeline cache data is empty (GXM patching is fast) |
| `vkCreateDescriptorSetLayout`, pools, `vkAllocate/Free/ResetDescriptorSets`, `vkUpdateDescriptorSets` | partial | Uniform buffer (+dynamic) and combined image sampler. Up to 4 sets. Binding n = GXM texture unit / buffer slot n |
| `vkCreateRenderPass`, `vkCreateFramebuffer` | partial | One subpass, at most one color attachment and one depth/stencil attachment. No input/resolve attachments |

## Commands

| Function(s) | Status | Notes |
|---|---|---|
| Pools, `vkAllocate/Free/Reset/Begin/EndCommandBuffer` | partial | Primary command buffers only |
| `vkCmdBegin/EndRenderPass` | partial | Render area is always the whole framebuffer |
| `vkCmdBindPipeline/VertexBuffers/IndexBuffer/DescriptorSets`, `vkCmdPushConstants` | full | 16-bit and 32-bit indices, 128 bytes of push constants |
| `vkCmdDraw`, `vkCmdDrawIndexed` | full | Instancing is emulated: one GXM draw per instance, per-instance bindings advance per draw |
| `vkCmdSetViewport/Scissor/DepthBias/Stencil*` | partial | Scissor uses GXM region clip, which works on 32x32 pixel tiles (see below) |
| `vkCmdCopyBuffer/BufferToImage/ImageToBuffer`, `vkCmdUpdateBuffer`, `vkCmdFillBuffer` | full | Executed by the CPU at submit, after a GPU wait if needed. Level 0 only |
| `vkCmdClearAttachments` | partial | Splits the GXM scene. Color rects are exact; depth/stencil clears always cover the whole attachment |
| `vkCmdClearColorImage` | full | Outside render passes |
| `vkCmdPipelineBarrier` | full | Layout transitions are no-ops; GPU-to-transfer/host barriers wait for the GPU |
| `vkCmdSetLineWidth/BlendConstants/DepthBounds`, `vkCmdNextSubpass`, `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect`, `vkCmdCopyImage`, `vkCmdBlitImage`, `vkCmdClearDepthStencilImage` | stub | Logged once as unsupported |

## Synchronization

| Function(s) | Status | Notes |
|---|---|---|
| Fences (`vkCreateFence`, `vkWaitForFences` with timeout, `vkResetFences`, `vkGetFenceStatus`) | full | GXM notifications, up to 512 fences |
| Semaphores | full | One in-order queue, so they need no work |
| Events, query pools, timeline semaphores | missing | |

## Known gaps that matter for ports

- **No mipmaps.** Extra levels are ignored (only level 0 is stored and
  sampled). Distant textures alias.
- **Scissor is tile granular.** GXM region clipping drops whole 32x32 tiles,
  so a scissor that is not tile aligned clips too little (a warning is
  logged once). UI code that clips text with scissor will show overdraw.
  Pixel-exact scissor needs a stencil/mask pass and is on the to-do list.
- **One color attachment, one subpass.** MRT and subpass inputs are not
  available on GXM in the same form.
- **Copies run on the CPU.** Fine for loading; per-frame staging copies cost
  a GPU sync. Write per-frame data into mapped memory instead.
- **Linear textures only.** Large textures sample slower than GXM's swizzled
  layout would; swizzled upload is a possible future optimization.
- **REPEAT/MIRROR on non-power-of-two textures** may not wrap correctly on
  GXM; prefer power-of-two textures for tiling.
