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
| `vkCreateBuffer`, `vkBindBufferMemory`, `vkGetBufferMemoryRequirements` | full | Vertex, index, uniform, storage (read in shaders only), indirect, transfer usages. Texel buffer usages are accepted but not functional |
| `vkCreateImage`, `vkBindImageMemory`, `vkGetImageMemoryRequirements`, `vkGetImageSubresourceLayout` | partial | 2D, 1, 2 or 4 samples. Mipmaps on power-of-two sizes. Cube maps (6 layers + CUBE_COMPATIBLE, square power-of-two, with or without mipmaps). Array images store every layer (up to 256), color and depth; cube map arrays keep only the first cube. Normal images are linear (row pitch = width aligned to 8 texels), cube maps and compressed images use the GXM swizzled layout. Depth formats all map to D24S8 |
| `vkCreateImageView` | partial | 2D and cube views, any base level and level count (cube views of a mipmapped cube must start at level 0). A 2D view of one array layer can be sampled and rendered into; array views of several layers use their first layer (GXM has no array textures). Component swizzles map to the fixed GXM swizzles of the format (BGRA, RGB with alpha 1, R8 as alpha or gray and more), others are ignored with a message. Rendering into a mip level works |
| `vkCreateSampler` | partial | Nearest/linear, mipmap nearest/linear, repeat/mirror/clamp, `maxLod` limits the sampled levels, `mipLodBias` from -3.875 to 4. No border color, anisotropy, compare, minLod |

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
| `vkCreateDescriptorSetLayout`, pools, `vkAllocate/Free/ResetDescriptorSets`, `vkUpdateDescriptorSets` | partial | Uniform buffer (+dynamic), combined image sampler, sampled image, sampler, immutable samplers, input attachments (color only, SPIR-V shaders), storage buffers (+dynamic, read only, SPIR-V shaders), descriptor arrays. Up to 4 sets. For Cg shaders binding n = GXM texture unit n; SPIR-V shaders carry their own table |
| `vkCreateRenderPass`, `vkCreateFramebuffer` | partial | Up to 8 subpasses with one color and one depth/stencil attachment each, up to 4 color input attachments per subpass. A subpass gets its own GXM scene unless it renders to the same attachments as the one before and reads no inputs. Dependencies are implied by scene order. 1, 2 or 4 samples, the same for all color and depth attachments of the pass; a resolve attachment receives the averaged samples directly (see Multisampling) |

## Commands

| Function(s) | Status | Notes |
|---|---|---|
| Pools, `vkAllocate/Free/Reset/Begin/EndCommandBuffer`, `vkCmdExecuteCommands` | full | Secondary command buffers are replayed inline |
| `vkCmdBegin/EndRenderPass` | partial | Render area is always the whole framebuffer |
| `vkCmdBindPipeline/VertexBuffers/IndexBuffer/DescriptorSets`, `vkCmdPushConstants` | full | 16-bit and 32-bit indices, 128 bytes of push constants |
| `vkCmdDraw`, `vkCmdDrawIndexed` | full | Instancing is emulated: one GXM draw per instance, per-instance bindings advance per draw |
| `vkCmdSetViewport/Scissor/DepthBias/Stencil*` | full | Scissor is pixel exact for SPIR-V shaders, tile granular for Cg shaders (see below) |
| `vkCmdCopyBuffer/BufferToImage/ImageToBuffer`, `vkCmdUpdateBuffer`, `vkCmdFillBuffer` | full | Executed by the CPU at submit, after a GPU wait if needed. Level 0 only |
| `vkCmdClearAttachments` | full | Draws a quad per rectangle inside the scene, so color, depth and stencil rectangles are exact and the scene is not split. Without the Cg compiler and shader cache it falls back to splitting the scene, where depth/stencil clears cover the whole attachment |
| `vkCmdClearColorImage` | full | Outside render passes |
| `vkCmdPipelineBarrier` | full | Layout transitions are no-ops; GPU-to-transfer/host barriers wait for the GPU |
| `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect` | full | The parameters are read from the buffer when the command buffer is submitted |
| `vkCmdResolveImage` | full | A copy, because multisampled color images already hold the averaged pixels |
| `vkCmdCopyImage` | full | Linear color images are copied by the GXM transfer unit (`sceGxmTransferCopy`), so the CPU neither waits for the GPU nor touches the pixels; the transfer waits for scenes still drawing the source. Cube maps, compressed and depth images are copied by the CPU. `compat = cpu_copy` turns the transfer unit off |
| `vkCmdBlitImage` | full | On the CPU at submit. Blits scale and flip; LINEAR filters 8-bit formats, others use nearest. Mipmap generation with a blit chain works |
| `vkCmdClearDepthStencilImage` | full | Applied as the background depth/stencil of the next scene that uses the image |
| `vkCmdSetLineWidth` | full | Whole pixels, 1 to 16 |
| `vkCmdNextSubpass` | full | |
| `vkCmdSetBlendConstants/DepthBounds` | stub | Logged once as unsupported |

## Synchronization

| Function(s) | Status | Notes |
|---|---|---|
| Fences (`vkCreateFence`, `vkWaitForFences` with timeout, `vkResetFences`, `vkGetFenceStatus`) | full | GXM notifications, up to 512 fences |
| Semaphores | full | One in-order queue, so they need no work |
| Events (`vkCreateEvent`, set/reset/status, `vkCmdSetEvent/ResetEvent/WaitEvents`) | full | One in-order queue, so waiting is a barrier |
| Occlusion queries | full | GXM visibility buffer, exact sample counts, up to 1024 queries. Reading results waits for the GPU. Clears inside a query are not counted |
| `vkCmdCopyQueryPoolResults` | full | Written by the CPU at submit, after waiting for the scenes that ended the queries; all result flags |
| Timestamp queries | partial | CPU time when the command buffer is submitted, in nanoseconds, not GPU time |
| Pipeline statistics queries, timeline semaphores | missing | GXM has no counters for pipeline statistics |

## Multisampling

2x and 4x MSAA use GXM multisampled render targets. Color and depth
attachments with 2 or 4 samples render their samples in the on-chip tile
memory, and GXM averages them when it writes a tile back, so a multisampled
color image only ever holds the resolved pixels and takes no more memory
than a normal one. When the subpass has a resolve attachment, the scene
writes straight into it and nothing is copied. Depth images keep every
sample (twice the size for 2x, four times for 4x).

Pipelines take the sample count of their render pass. Not available: sample
shading, alpha to coverage, sample masks, and reading single samples of a
multisampled image (`sampler2DMS`, `subpassLoad` of a multisampled input).

## Known gaps that matter for ports

- **Mipmaps need power-of-two sizes.** Other sizes keep level 0 only.
- **Scissor between tiles costs a shader variant.** GXM region clipping
  works on 32x32 tiles. When a scissor does not line up with them, SPIR-V
  pipelines switch to a second fragment program that discards the pixels
  outside the rectangle; it is built the first time a draw needs it. Hand
  written Cg shaders cannot get that variant and keep the tile granular
  clip (a warning is logged once).
- **One color attachment per subpass.** GXM has a single color output, so
  MRT is not available. Subpasses work, but every subpass that reads an input
  attachment ends the scene before it and reads the result back from memory,
  so they cost what separate render passes cost. Depth attachments cannot be
  read as inputs.
- **Copies run on the CPU.** Fine for loading; per-frame staging copies cost
  a GPU sync. Write per-frame data into mapped memory instead.
- **Normal textures are linear.** Large textures sample slower than GXM's
  swizzled layout would; only cube maps and compressed textures are swizzled.
- **REPEAT/MIRROR on non-power-of-two textures** may not wrap correctly on
  GXM; prefer power-of-two textures for tiling.
