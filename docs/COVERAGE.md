# API coverage and limits

VkVita implements a Vulkan 1.0 subset (143 entry points plus 3 KHR aliases,
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
| `vkCreateBuffer`, `vkBindBufferMemory`, `vkGetBufferMemoryRequirements` | full | Vertex, index, uniform, indirect, transfer usages. Storage/texel usages are accepted but not functional |
| `vkCreateImage`, `vkBindImageMemory`, `vkGetImageMemoryRequirements`, `vkGetImageSubresourceLayout` | partial | 2D, 1 sample. Mipmaps on power-of-two sizes. Cube maps (6 layers + CUBE_COMPATIBLE, square power-of-two, 1 level). Other array layers are not stored. Normal images are linear (row pitch = width aligned to 8 texels), cube maps and compressed images use the GXM swizzled layout. Depth formats all map to D24S8 |
| `vkCreateImageView` | partial | 2D and cube views, any base level and level count. Component swizzles are ignored. Rendering into a mip level works |
| `vkCreateSampler` | partial | Nearest/linear, mipmap nearest/linear, repeat/mirror/clamp, `maxLod` limits the sampled levels. No border color, anisotropy, compare, minLod, LOD bias |

Formats usable as textures: RGBA8, BGRA8 (+SRGB, decoded by the texture
unit), RGB8, RG8, R8, RGB565, RGBA4444, RGBA5551, ARGB1555, RGBA16F, RG16F,
R32F, RG32F, RG16 (U/S norm), BC1 to BC3 (+SRGB), BC4, BC5.
Render targets: RGBA8, BGRA8 (+SRGB, encoded on write), RG8, R8, RGB565,
RGBA16F, RG16F, R16F, R32F. Compressed formats must be power-of-two sized.

## Pipelines and descriptors

| Function(s) | Status | Notes |
|---|---|---|
| `vkCreateShaderModule` | partial | SPIR-V (translated to Cg), Cg source or GXP binary, see SHADERS.md |
| `vkCreateGraphicsPipelines`, `vkDestroyPipeline` | partial | Vertex + fragment only. Topologies: point list, line list, triangle list/strip/fan. Blend ops add/sub/rev-sub/min/max, all factors except constant/dual-source. Polygon modes fill/line/point. Depth, stencil (two-sided), depth bias. Dynamic: viewport, scissor, depth bias, stencil masks/reference |
| `vkCreateComputePipelines` | stub | Returns `VK_ERROR_FEATURE_NOT_PRESENT` |
| `vkCreatePipelineLayout`, `vkCreatePipelineCache` (+ get/merge) | full | Pipeline cache data is empty (GXM patching is fast) |
| `vkCreateDescriptorSetLayout`, pools, `vkAllocate/Free/ResetDescriptorSets`, `vkUpdateDescriptorSets` | partial | Uniform buffer (+dynamic), combined image sampler, sampled image, sampler, immutable samplers. Up to 4 sets. For Cg shaders binding n = GXM texture unit n; SPIR-V shaders carry their own table |
| `vkCreateRenderPass`, `vkCreateFramebuffer` | partial | One subpass, at most one color attachment and one depth/stencil attachment. No input/resolve attachments |

## Commands

| Function(s) | Status | Notes |
|---|---|---|
| Pools, `vkAllocate/Free/Reset/Begin/EndCommandBuffer`, `vkCmdExecuteCommands` | full | Secondary command buffers are replayed inline |
| `vkCmdBegin/EndRenderPass` | partial | Render area is always the whole framebuffer |
| `vkCmdBindPipeline/VertexBuffers/IndexBuffer/DescriptorSets`, `vkCmdPushConstants` | full | 16-bit and 32-bit indices, 128 bytes of push constants |
| `vkCmdDraw`, `vkCmdDrawIndexed` | full | Instancing is emulated: one GXM draw per instance, per-instance bindings advance per draw |
| `vkCmdSetViewport/Scissor/DepthBias/Stencil*` | partial | Scissor uses GXM region clip, which works on 32x32 pixel tiles (see below) |
| `vkCmdCopyBuffer/BufferToImage/ImageToBuffer`, `vkCmdUpdateBuffer`, `vkCmdFillBuffer` | full | Executed by the CPU at submit, after a GPU wait if needed. Level 0 only |
| `vkCmdClearAttachments` | partial | Splits the GXM scene. Color rects are exact; depth/stencil clears always cover the whole attachment |
| `vkCmdClearColorImage` | full | Outside render passes |
| `vkCmdPipelineBarrier` | full | Layout transitions are no-ops; GPU-to-transfer/host barriers wait for the GPU |
| `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect` | full | The parameters are read from the buffer when the command buffer is submitted |
| `vkCmdCopyImage`, `vkCmdBlitImage` | full | On the CPU at submit. Blits scale and flip; LINEAR filters 8-bit formats, others use nearest. Mipmap generation with a blit chain works |
| `vkCmdClearDepthStencilImage` | full | Applied as the background depth/stencil of the next scene that uses the image |
| `vkCmdSetLineWidth` | full | Whole pixels, 1 to 16 |
| `vkCmdSetBlendConstants/DepthBounds`, `vkCmdNextSubpass` | stub | Logged once as unsupported |

## Synchronization

| Function(s) | Status | Notes |
|---|---|---|
| Fences (`vkCreateFence`, `vkWaitForFences` with timeout, `vkResetFences`, `vkGetFenceStatus`) | full | GXM notifications, up to 512 fences |
| Semaphores | full | One in-order queue, so they need no work |
| Events (`vkCreateEvent`, set/reset/status, `vkCmdSetEvent/ResetEvent/WaitEvents`) | full | One in-order queue, so waiting is a barrier |
| Occlusion queries | full | GXM visibility buffer, exact sample counts, up to 1024 queries. Reading results waits for the GPU |
| Timestamp queries | partial | CPU time when the command buffer is submitted, in nanoseconds, not GPU time |
| Pipeline statistics queries, `vkCmdCopyQueryPoolResults`, timeline semaphores | missing | |

## Known gaps that matter for ports

- **Mipmaps need power-of-two sizes.** Other sizes keep level 0 only.
- **Scissor is tile granular.** GXM region clipping drops whole 32x32 tiles,
  so a scissor that is not tile aligned clips too little (a warning is
  logged once). UI code that clips text with scissor will show overdraw.
  Pixel-exact scissor needs a stencil/mask pass and is on the to-do list.
- **One color attachment, one subpass.** MRT and subpass inputs are not
  available on GXM in the same form.
- **Copies run on the CPU.** Fine for loading; per-frame staging copies cost
  a GPU sync. Write per-frame data into mapped memory instead.
- **Normal textures are linear.** Large textures sample slower than GXM's
  swizzled layout would; only cube maps and compressed textures are swizzled.
- **REPEAT/MIRROR on non-power-of-two textures** may not wrap correctly on
  GXM; prefer power-of-two textures for tiling.
