<p align="center">
  <img src="docs/vkvita-logo.png" alt="VkVita" width="480">
</p>

VkVita is a wrapper between Vulkan and sceGxm. It lets you use a large part of the Vulkan 1.0 API on the PS Vita, with everything rendered on the SGX543MP4+ GPU. It is a static library (`libvkvita.a`) that implements the `vk*` functions directly, so there is no loader and no ICD. SPIR-V shaders are translated to Cg and compiled on the console.

There are no releases. Whatever is on `main` is the current build.

## Prerequisites

To compile SPIR-V or Cg shaders at runtime, VkVita needs vitaShaRK and a decrypted `libshacccg.suprx` in `ur0:data/` on your console. You can install it with ShaRKBR33D or the PIB configuration tool. Most vitaGL ports need the same file, so you probably have it already.

If you build with `VKV_WITH_SHARK=OFF`, you can ship precompiled GXP shaders instead and don't need the file at all.

Not every SPIR-V feature has a GXM counterpart. What works and what doesn't is listed in [docs/SHADERS.md](docs/SHADERS.md).

## Help and Troubleshooting

If you run into a problem, open an issue on GitHub. Most problems can be narrowed down with the debug options below, and [docs/DEBUGGING.md](docs/DEBUGGING.md) has a list of common symptoms and what to try.

## Build Instructions

Both the normal (hardfp) and the softfp VitaSDK work.

```sh
cmake -S . -B build
cmake --build build -j
cmake --install build
```

This installs `libvkvita.a`, the headers (including the Khronos `vulkan/*.h`), a CMake package and a `vkvita.pc` file into your VitaSDK. The sample VPKs end up in `build/samples/`.

These CMake options are available:

| Option | Description |
|---|---|
| `VKV_WITH_SHARK=AUTO` | Runtime Cg compiler through vitaShaRK. `AUTO` uses it if it's installed, `ON` requires it, `OFF` removes it |
| `VKV_WITH_SPIRV=ON` | SPIR-V support through the bundled SPIRV-Cross. Adds about 850 KB of code and needs `-lstdc++` |
| `VKV_ENABLE_DEBUG=ON` | Compiles in logging, validation and profiling. `OFF` removes them for release builds |
| `VKV_BUILD_SAMPLES=ON` | Builds the sample VPKs |
| `VKV_INSTALL_VULKAN_HEADERS=ON` | Installs the bundled Khronos Vulkan headers |
| `VKV_DEFAULT_DEBUG_FLAGS=` | Default debug flags as a number |

Everything below can be changed at runtime without rebuilding, either in code (`vkvSetConfig`, `vkvSetDebugFlags`, `vkvSetCompatFlags`) or by creating `ux0:data/vkvita/config.txt` on the console:

```ini
debug    = info, validation, profile
compat   = shader_cache
log_path = ux0:data/vkvita/log.txt
```

The config file also works with an already finished VPK.

### Debug Flags

| Flag | Description |
|---|---|
| `info` | Logs init, teardown, memory fallbacks and swapchain setup |
| `shaders` | Logs shader parameters, compile times, compiler output and how vertex inputs and push constants are mapped |
| `validation` | Extra error checks. Failing calls return an error instead of crashing |
| `profile` | Per-frame counters, reported every `profile_interval` frames |
| `calls` | Logs every API call and every replayed command. Slow, but shows where a crash happens |
| `gpu_sync` | Waits for the GPU after every submit and logs CPU and GPU time |
| `dump_shaders` | Writes every runtime compiled shader to `shader_dir` |
| `unsupported` | Reports every unsupported feature once. On by default |
| `break_on_error` | Crashes on the first validation error, so the crash dump points at the bad call |
| `all` | Everything except `calls`, `gpu_sync` and `break_on_error` |

### Compatibility Flags

| Flag | Description |
|---|---|
| `gl_clip_space` | For code written for OpenGL (Y up, depth from -1 to 1). Flips the viewport and remaps depth |
| `invert_cull` | Swaps front and back face culling |
| `disable_cull` | Turns culling off completely |
| `cpu_clear` | Clears with the CPU instead of the GXM transfer unit. Slower |
| `shader_cache` | Loads compiled shaders from `shader_dir` before compiling, and saves them after |
| `force_vsync` | Every present waits for vblank |
| `no_vsync` | No present waits for vblank. Can tear |
| `ignore_scissor` | Ignores scissor rectangles |
| `fast_transfers` | Skips the GPU wait before copies. Faster, but can cause corruption |

### Misc Flags

| Key | Description |
|---|---|
| `log_path` | File the log is written to |
| `shader_dir` | Folder for dumped and cached shaders |
| `shark_module` | Path to `libshacccg.suprx` |
| `profile_interval=120` | Frames between profile reports |
| `parameter_buffer_kb=16384` | Size of the GXM parameter buffer |
| `display_queue_depth=2` | Maximum number of pending flips |
| `pool_block_kb=4096` | Block size of the memory sub-allocator |
| `pool_max_alloc_kb=512` | Allocations up to this size are sub-allocated |
| `scenes_per_frame=4` | Render target hint for GXM |
| `ff_buffer_kb=1024` | Streaming buffer size of the fixed function layer per frame |
| `ff_frames=3` | Frames in flight for the fixed function layer |

## Using VkVita

With CMake:

```cmake
find_package(VkVita REQUIRED)
target_link_libraries(my_game PRIVATE VkVita::vkvita)
```

With a Makefile, use pkg-config like `samples/standalone_template/Makefile` does.

Most Vulkan code works unchanged. The parts that are different on the Vita:

- **Surface:** use `VK_EXT_headless_surface` or `vkvCreateDisplaySurface()`. The swapchain is always 960x544.
- **Shaders:** SPIR-V works for the usual vertex and fragment shaders. Storage buffers and compute do not exist on GXM.
- **Limits:** one color attachment per subpass, no compute. MSAA works with 2x and 4x. Mipmaps need power-of-two textures. The full list is in [docs/COVERAGE.md](docs/COVERAGE.md).

There is also a small fixed function layer (`vkvita_ff.h`) for old GL 1.x style code, see [docs/FIXED_FUNCTION.md](docs/FIXED_FUNCTION.md).

## Samples

Samples can be found in the `samples` folder.

| Sample | Shows |
|---|---|
| `hello_triangle` | The complete Vulkan flow with no helpers |
| `textured_cube` | Textures, depth, instancing, push constants and debug toggles on buttons |
| `fixed_function` | GL 1.x style drawing through the fixed function layer |
| `spirv_cube` | The cube with GLSL shaders compiled to SPIR-V, push constants, a uniform buffer and a texture |
| `gpu_features` | Mipmaps (uploaded and blitted), a mipmapped cube map, a BC1 texture with a separate sampler, wide lines, indirect draws and occlusion queries |
| `subpasses` | A render pass with three subpasses, one of them reading the first through an input attachment |
| `msaa` | 4x and 2x MSAA with a resolve attachment, next to the same scene without multisampling |
| `standalone_template` | A starting point for your own project |

## Tools

`tools/spirv2cg` translates a SPIR-V file into the Cg VkVita would generate, so you can check a shader on the PC. Build it with `tools/build_spirv2cg.sh`.

## License

VkVita is licensed under the GPL 3.0 (or any later version), see `LICENSE`. The bundled Vulkan headers are Apache 2.0 or MIT, SPIRV-Cross is Apache 2.0, and vitaShaRK is LGPL 3.0.
