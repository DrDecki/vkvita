# SDL3 GPU on VkVita

SDL3's GPU API (`SDL_gpu.h`) has a Vulkan driver. With VkVita as the Vulkan
implementation it runs on the PS Vita, so SDL_GPU programs and SDL's 2D
renderer on top of it (`SDL_RENDER_DRIVER=gpu`) can be built for the Vita
without a Vita specific renderer.

The `sdl3_gpu` sample draws a green checkerboard quad that moves left and
right. It covers shaders, vertex and index buffers, texture uploads, samplers
and uniform data.

## Building SDL3 with Vulkan

SDL's Vita video driver gets Vulkan support from a small patch,
`patches/sdl3-vita-vulkan.patch` in this repository (until it is part of SDL).
It adds `src/video/vita/SDL_vitavulkan.c`, which takes `vkGetInstanceProcAddr`
from the linked VkVita and creates the display surface with
`VK_EXT_headless_surface`.

```sh
git clone https://github.com/libsdl-org/SDL.git sdl3 && cd sdl3
git apply /path/to/vkvita/patches/sdl3-vita-vulkan.patch
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release -DSDL_VULKAN=ON -DSDL_SHARED=OFF -DSDL_STATIC=ON \
      -DCMAKE_INSTALL_PREFIX=$HOME/sdl3-vita
cmake --build build -j && cmake --install build
```

Vulkan stays off on the Vita unless `-DSDL_VULKAN=ON` is given, because every
program linking that SDL then has to link VkVita too.

## Using it in a program

Link SDL first, then VkVita, and keep VkVita's entry point, which only SDL
calls:

```cmake
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3-static)
target_link_libraries(game PRIVATE SDL3::SDL3-static VkVita::vkvita)
target_link_options(game PRIVATE -Wl,--undefined=vkGetInstanceProcAddr)
```

Create the device with SPIR-V shaders:

```c
SDL_GPUDevice *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
```

Shaders follow SDL's SPIR-V layout (vertex resources in sets 0 and 1,
fragment resources in sets 2 and 3) and are translated to Cg like any other
SPIR-V shader, see SHADERS.md. SPIR-V made by SDL_shadercross from HLSL works
as well as GLSL compiled with glslang.

For SDL's 2D renderer set `SDL_HINT_RENDER_DRIVER` to `gpu`. The `vita_gxm`
renderer starts GXM itself and can't run next to VkVita.

## What does not work

- Compute pipelines, storage textures and writes to storage buffers (GXM has
  no compute). Read-only storage buffers work.
- Texture arrays and cube map arrays in shaders, writing `gl_FragDepth` /
  `SV_Depth`, clip distances. Such shaders fail with a message in the log.
- Anisotropic filtering, depth clamp and blend constants are ignored.
- One window, one GPU device.

SDL writes its own log to `ux0:data/SDL_Log.txt`. VkVita logs to the file set
with `log_path` in `ux0:data/vkvita/config.txt`, see DEBUGGING.md.
