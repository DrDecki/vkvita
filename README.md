# VkVita

VkVita lets you write Vulkan style code for the PS Vita. You call the normal
`vk*` functions from `<vulkan/vulkan.h>` and VkVita turns them into GXM calls,
so everything actually renders on the Vita's GPU.

It's a static library (`libvkvita.a`) you link into your homebrew. No loader,
no ICD, the functions are just implemented directly.

I started this because I wanted to port an open source Vulkan game to the
Vita (vkQuake2 is the plan) and there was nothing that let me do that.

## Current state

This is version 0.1.0, so please keep your expectations low for now.

The library and all samples build with VitaSDK, and they run fine on a PC
against a mock of the Vita APIs (with AddressSanitizer). **It has not been
tested on a real Vita yet.** I'm pretty sure the first run on hardware will
show some bugs. That's also why there are so many debug options built in.

If you try it on your console, I'd love to hear how it went, good or bad.

## What works

- A good chunk of Vulkan 1.0: instance, device, swapchain, memory, buffers,
  images, samplers, render passes, pipelines, descriptor sets, push
  constants, command buffers, fences and semaphores. The full list is in
  [docs/COVERAGE.md](docs/COVERAGE.md).
- Shaders as Cg source (compiled at runtime with vitaShaRK) or as
  precompiled GXP. SPIR-V does not work, the Vita simply can't use it.
- Debug options you can switch on without rebuilding: logging, validation,
  profiling, call tracing, shader dumps and more.
- Compatibility options for code written for desktop GPUs, like OpenGL clip
  space or turning culling off.
- A small fixed function layer (`vkvita_ff.h`) for old GL 1.x style code.

What doesn't work: compute, mipmaps, MSAA, more than one color attachment or
subpass. The Vita just isn't built for most of that.

## Building

You need [VitaSDK](https://vitasdk.org) and CMake 3.16 or newer.

```sh
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH

cmake -S . -B build
cmake --build build -j
cmake --install build
```

This installs the library, the headers and a CMake package straight into
your VitaSDK. The sample VPKs end up in `build/samples/`.

For runtime Cg compiling you also want vitaShaRK:

```sh
vdpm install vitaShaRK SceShaccCgExt
```

and `ur0:data/libshacccg.suprx` on your Vita. If you've ever installed a
vitaGL port you probably already have it.

For a release build without debug stuff:

```sh
cmake -S . -B build-rel -DVKV_ENABLE_DEBUG=OFF -DVKV_BUILD_SAMPLES=OFF
```

## Using it in your project

With CMake:

```cmake
find_package(VkVita 0.1 REQUIRED)
target_link_libraries(my_game PRIVATE VkVita::vkvita)
```

With a Makefile, take a look at `samples/standalone_template/Makefile`, it
uses pkg-config.

Most Vulkan code should port without big changes. The things that are
different on the Vita:

1. **Surface:** use `VK_EXT_headless_surface` or `vkvCreateDisplaySurface()`.
   The screen is 960x544.
2. **Shaders:** give it Cg source or GXP instead of SPIR-V. More about that
   in [docs/SHADERS.md](docs/SHADERS.md).
3. **Limits:** one color attachment, one subpass, no compute.

A tiny example:

```c
#include <vulkan/vulkan.h>
#include <vkvita/vkvita.h>

VkvConfig cfg;
vkvGetDefaultConfig(&cfg);
cfg.debugFlags |= VKV_DEBUG_VALIDATION | VKV_DEBUG_PROFILE;
cfg.logPath = "ux0:data/vkvita/mygame.log";
vkvSetConfig(&cfg);

/* normal Vulkan from here on */
VkShaderModule vs;
vkvCreateShaderModuleFromCg(device, my_vertex_cg, "main_vs", &vs);
```

## Samples

| Sample | Title ID | What it shows |
|---|---|---|
| `hello_triangle` | VKVT00001 | The classic triangle, plain Vulkan, no helpers |
| `textured_cube` | VKVT00002 | Textures, depth, push constants, debug toggles on buttons |
| `fixed_function` | VKVT00003 | Old GL 1.x style drawing through the fixed function layer |
| `standalone_template` | MYGAME001 | A starting point for your own project |

Install them with VitaShell. Logs land in `ux0:data/vkvita/`.

In `textured_cube` the stick rotates the cube, SELECT writes the profiler
log, TRIANGLE toggles validation, CROSS toggles wireframe and START quits.

## Debugging on the Vita

You can change options without rebuilding by creating
`ux0:data/vkvita/config.txt`:

```ini
debug  = info, validation, profile, shaders
compat = shader_cache
log_path = ux0:data/vkvita/log.txt
```

This even works with an already finished VPK. All the options are explained
in [docs/DEBUGGING.md](docs/DEBUGGING.md), together with a checklist for
the first run on real hardware.

## Tests

There's a host test that builds everything for your PC against a fake Vita
API and runs every sample for a few frames:

```sh
VITA_HEADERS=$VITASDK/arm-vita-eabi/include ./tests/host/run_host_tests.sh
```

It catches crashes and memory bugs, but it can't tell you if the picture
looks right. For that you need a real Vita or Vita3K.

## License

VkVita is licensed under the GPL 3.0 (or any later version), see `LICENSE`.
If you use it in your own project, that project has to be GPL 3.0 compatible too.

The bundled Vulkan headers are Apache 2.0 or MIT, and vitaShaRK is LGPL 3.0.
Both work fine together with the GPL.
