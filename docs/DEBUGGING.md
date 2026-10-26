# Debugging, profiling and compatibility options

All options can be set three ways. Later ones win:

1. In code, before `vkCreateInstance`: `vkvGetDefaultConfig` + `vkvSetConfig`.
2. `ux0:data/vkvita/config.txt` on the memory card, read by
   `vkCreateInstance`. No rebuild needed, works on shipped VPKs.
3. At runtime: `vkvSetDebugFlags()` / `vkvSetCompatFlags()` (safe between
   frames, for example bound to buttons like the `textured_cube` sample).

Requesting the `VK_LAYER_KHRONOS_validation` layer in `vkCreateInstance`
also switches validation on.

## Where the log goes

Every message goes to `sceClibPrintf` (visible with a kernel log plugin
such as PrincessLog/catlog over the network) and, if `log_path` is set, to
that file (folders are created). `vkvSetLogCallback()` lets you draw
messages on screen or forward them elsewhere.

Lines look like `[vkvita WARN] unsupported: mipmapped images (...)`.

## config.txt

```ini
# comments start with '#'
debug      = info, validation, profile      # replaces the debug flags
debug_add  = shaders                        # adds to them
compat     = shader_cache, gl_clip_space
compat_add = no_vsync
log_path   = ux0:data/vkvita/log.txt
shader_dir = ux0:data/vkvita/shaders
shark_module = ur0:data/libshacccg.suprx
profile_interval    = 120     # frames per profile report
parameter_buffer_kb = 16384   # GXM parameter buffer
display_queue_depth = 2       # max pending flips
pool_block_kb       = 4096    # sub-allocator block size
pool_max_alloc_kb   = 512     # allocations up to this size are pooled
scenes_per_frame    = 4       # render target hint
ff_buffer_kb        = 1024    # fixed-function streaming buffer per frame
ff_frames           = 3       # fixed-function frames in flight
```

Flag values can also be numbers (`debug = 0x0d`).

## Debug flags

| config name | constant | What it does |
|---|---|---|
| `info` | `VKV_DEBUG_LOG_INFO` | Init/teardown details, memory fallbacks, swapchain setup |
| `shaders` | `VKV_DEBUG_LOG_SHADERS` | Shader parameter tables, compile times, compiler output, location and push constant mapping |
| `validation` | `VKV_DEBUG_VALIDATION` | Extra error checks (list below); failing calls return an error instead of crashing |
| `profile` | `VKV_DEBUG_PROFILE` | Per-frame counters, report every `profile_interval` frames |
| `calls` | `VKV_DEBUG_LOG_CALLS` | Logs key API calls and every command replayed on the GPU (slow; for "where does it crash") |
| `gpu_sync` | `VKV_DEBUG_GPU_SYNC` | Waits for the GPU after every submit and logs CPU vs. GPU time. Also isolates GPU crashes to one submit |
| `dump_shaders` | `VKV_DEBUG_DUMP_SHADERS` | Writes every runtime-compiled shader to `shader_dir` |
| `unsupported` | `VKV_DEBUG_LOG_UNSUPPORTED` | Reports each unsupported feature once (on by default) |
| `break_on_error` | `VKV_DEBUG_BREAK_ON_ERROR` | First validation error traps, so you get a crash dump at the bad call |
| `all` | | Everything except `calls`, `gpu_sync`, `break_on_error` |

Building with `-DVKV_ENABLE_DEBUG=OFF` compiles the debug code out. Errors
and warnings are still logged; validation, profiling and tracing are gone.

### What validation checks

- Handles: wrong or destroyed objects (double free, use after destroy).
- Memory: binds that do not fit or are misaligned, map offsets.
- Render passes: draws outside a pass, nested passes, a pipeline used with an
  incompatible color format, ending a command buffer inside a pass.
- Draws: no pipeline, missing vertex buffer for a binding, missing index
  buffer, index ranges past the buffer end, texture units or uniform buffers
  the shaders read with nothing bound, uniforms reading push constant bytes
  that were never pushed.
- Pipelines: shader inputs no vertex attribute feeds, undeclared bindings.
- Descriptors: type mismatches, unknown bindings, misaligned uniform offsets.
- Copies: regions past buffer ends.
- Sync: resetting in-flight fences, waiting on never-submitted fences,
  submitting command buffers that were not ended, wrong dynamic offset counts.

## Profiling

With `profile` on, every `profile_interval` frames:

```
[vkvita info] profile f=600 fps=59.9 frame=16.69ms submit=1.42ms gpuwait=0.10ms draws=81 idx=2916
  scenes=1 pipe=2 sets=1 clears=1 copies=0 ff=0 cdram=12288KB ram=4608KB verr=0
```

| Field | Meaning |
|---|---|
| `fps`, `frame` | average over the interval, last present-to-present time |
| `submit` | CPU time inside `vkQueueSubmit` (command replay + GXM calls) |
| `gpuwait` | CPU time blocked on fences, idle waits and transfer syncs |
| `draws`, `idx` | draw calls and indices per frame (instances count separately) |
| `scenes` | render passes (GXM scenes) per frame |
| `pipe`, `sets` | pipeline state applications, descriptor set binds |
| `clears`, `copies` | color clears, CPU-executed copies |
| `ff` | fixed-function draws |
| `cdram`, `ram` | video memory and uncached main memory in use |
| `verr` | validation errors so far |

`vkvGetFrameStats()` returns the same numbers for an on-screen overlay (the
`fixed_function` sample draws its frame time bar from it).

Reading it:

- High `gpuwait` with copies > 0: a copy forced a GPU wait. Write per-frame
  data straight into mapped memory (one buffer per frame in flight), or try
  `compat = fast_transfers` if you know the data is not in use.
- High `submit`: too many draws or pipeline switches; batch, sort by pipeline.
- `frame` stuck at 33 ms with low `submit`: you are GPU bound (or vsync is
  halving the rate). `gpu_sync` shows the GPU time of each submit.

## Compatibility flags

| config name | constant | Use it when |
|---|---|---|
| `gl_clip_space` | `VKV_COMPAT_GL_CLIP_SPACE` | Code/shaders written for OpenGL: Y up, depth in [-1,1]. Flips the viewport and remaps depth |
| `invert_cull` | `VKV_COMPAT_INVERT_CULL` | Front faces disappear and back faces show |
| `disable_cull` | `VKV_COMPAT_DISABLE_CULL` | Geometry is missing and you suspect winding |
| `cpu_clear` | `VKV_COMPAT_CPU_CLEAR` | Clears look wrong; clears with the CPU instead of the GXM transfer unit (slower) |
| `shader_cache` | `VKV_COMPAT_SHADER_CACHE` | Load `<shader_dir>/<hash>.gxp` before compiling; dump after compiling |
| `force_vsync` | `VKV_COMPAT_FORCE_VSYNC` | Every present waits for vblank |
| `no_vsync` | `VKV_COMPAT_NO_VSYNC` | Measure uncapped frame rate (tearing possible) |
| `ignore_scissor` | `VKV_COMPAT_IGNORE_SCISSOR` | Scissor rectangles hide too much |
| `fast_transfers` | `VKV_COMPAT_FAST_TRANSFERS` | Skip the GPU wait before CPU-executed copies (faster, can race) |

## Common problems

| Symptom | Try |
|---|---|
| Black screen, no errors | `debug = info, validation`; check `ur0:data/libshacccg.suprx` if you use Cg |
| Upside-down or wrong depth | OpenGL-style projection: `compat = gl_clip_space` |
| Missing triangles | `disable_cull`, then `invert_cull`; check the winding |
| Garbage geometry | `debug = shaders`: compare the location mapping with your vertex layout |
| Wrong colors from shaders | check push constant mapping in the shader log; use `float4`/`float4x4` |
| Crash, no idea where | `debug = calls, gpu_sync, break_on_error` and read the last lines of the log |
| Stutter | `profile`: look at `gpuwait`, `copies` and shader compile messages (enable `shader_cache`) |

## First run on hardware

VkVita has been verified on a host mock, not on a Vita yet. Things to
confirm first, in this order, with `debug = all`:

1. `standalone_template` (no shaders): the screen cycles colors. Confirms
   GXM init, swapchain, display queue and transfer-unit clears. If the
   colors are wrong or flicker, try `compat = cpu_clear`.
2. `hello_triangle`: a spinning triangle. Confirms Cg compilation,
   attribute mapping, push constants and the viewport.
3. `textured_cube`: check that cubes look solid (culling direction, try
   `invert_cull` if inside faces show), the texture and the depth buffer.
4. `fixed_function`: alpha test edge (CROSS) and blending (CIRCLE).

Please keep the log of each run; the shader tables and profile lines are the
fastest way to pin down a mismatch.
