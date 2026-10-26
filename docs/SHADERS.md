# Shaders

GXM runs GXP programs compiled from Sony's Cg dialect. It cannot run
SPIR-V directly, so `VkShaderModuleCreateInfo::pCode` takes one of these:

| Input | How VkVita detects it | Needs |
|---|---|---|
| GXP binary | starts with `GXP\0` | nothing at runtime |
| SPIR-V | magic `0x07230203` | `VKV_WITH_SPIRV` build, plus what Cg needs |
| Cg source | anything else (or `VKV_SHADER_SOURCE_CG`) | vitaShaRK build + `ur0:data/libshacccg.suprx` |

For Cg source `codeSize` is the string length; the multiple-of-4 rule is not
enforced. Helpers:

```c
vkvCreateShaderModuleFromCg(device, source, "debug_name", &module);
vkvCreateShaderModuleFromFile(device, "app0:shaders/sprite_v.gxp", &module); /* .gxp or Cg text */
vkvShaderCompilerAvailable(device);   /* 1 if runtime Cg compilation works */
```

Cg is compiled when the first pipeline uses the module, because the stage
is only known then. The same Cg module can be used as vertex and fragment
shader (it is compiled once per stage); a GXP module has a fixed stage.

## SPIR-V

SPIR-V modules are translated to Cg when the first pipeline uses them, then
compiled like Cg source. The translation uses the bundled SPIRV-Cross (HLSL
backend, shader model 3.0) and rewrites the result into Sony's Cg dialect.
Nothing from the sections below has to be done by hand:

- Vertex input `location = n` becomes the Cg semantic `ATTRn`.
- `sampler2D` / `samplerCube` at binding `n` become `TEXUNITn`.
- Push constant members become default uniforms at their SPIR-V offsets.
- Uniform buffer members become default uniforms too. VkVita reads them from
  the bound buffer when the command buffer is submitted, so the buffer can be
  rewritten every frame without double buffering.
- Integer push constant and uniform buffer members are converted to float on
  upload and back to int in the shader.
- Integer varyings (`flat out int`) are carried as floats and rounded.
- Separate `texture2D` / `sampler` objects are combined. Every image and
  sampler pair used in the shader gets its own texture unit, and VkVita
  builds the texture from both descriptors at draw time.

Not supported (the translation fails with a message naming the resource):
sampler arrays, storage buffers and images, subpass inputs,
`gl_VertexIndex` / `gl_InstanceIndex`, compute and geometry stages. Struct
members inside uniform blocks are declared but not fed yet.

With `debug = shaders` the log contains the generated Cg (lines starting with
`cg-src[name:line]`), which is what to look at when the Cg compiler reports
an error. The `spirv2cg` tool prints the same Cg on the PC:

```sh
tools/build_spirv2cg.sh
./spirv2cg shader.vert.spv
```

Compiled SPIR-V shaders go through the shader cache like Cg, so a release can
ship the dumped GXP files and skip both the translation and the compiler.

## Writing shaders for VkVita

Write `main` with its inputs as parameters. A minimal pair:

```c
// vertex
void main(float3 aPosition, float2 aTexcoord, float4 aColor,   // locations 0, 1, 2
          uniform float4x4 uMvp,                                // push constants [0..64)
          float4 out vPosition : POSITION,
          float2 out vTexcoord : TEXCOORD0,
          float4 out vColor : COLOR)
{
    vPosition = mul(float4(aPosition, 1.0f), uMvp);
    vTexcoord = aTexcoord;
    vColor = aColor;
}

// fragment
float4 main(float2 vTexcoord : TEXCOORD0, float4 vColor : COLOR,
            uniform sampler2D uTex : TEXUNIT0,                  // descriptor binding 0
            uniform float4 uTint) : COLOR                       // push constants of the fragment range
{
    return tex2D(uTex, vTexcoord) * vColor * uTint;
}
```

Global variables in Cg are uniforms by default. Mark helper globals `static`.

### Vertex inputs (Vulkan location -> Cg attribute)

For each `VkVertexInputAttributeDescription`, VkVita looks for the shader
attribute in this order:

1. An explicit name from `VkvShaderModuleInfoVITA::pAttributeBindings`.
2. A Cg `ATTRn` semantic where `n` is the location (`float3 p : ATTR0`).
3. Declaration order: the n-th vertex input of `main` is location n.

Attributes the compiler removed (unused inputs) are skipped with an info
message. With validation on, a shader input that no Vulkan attribute feeds is
reported.

Vertex formats: 8/16/32-bit integer, normalized and float types, 1 to 4
components. GXM has no BGRA vertex swizzle, so `B8G8R8A8_UNORM` attributes
are rejected; use `R8G8B8A8_UNORM`.

### Push constants (Vulkan push constants -> Cg default uniforms)

Plain `uniform` parameters (not samplers, not buffer members) live in GXM's
default uniform buffer. VkVita fills it from the push constant block on every
draw:

- The vertex shader's uniforms start at the `offset` of the first push
  constant range with `VK_SHADER_STAGE_VERTEX_BIT`, the fragment shader's at
  the first range with `VK_SHADER_STAGE_FRAGMENT_BIT`.
- Uniforms are laid out in declaration order with std140-like rules:
  `float` 4-byte aligned, `float2` 8, `float3`/`float4`/matrices 16; array
  elements are padded to 16 bytes.
- Override any of this with `VkvShaderModuleInfoVITA::pUniformBindings`
  (`{ "uMvp", 0 }, { "uTint", 64 }`).
- Only `float` uniforms are fed. Use `float4`/`float4x4` where you can; that
  layout is identical in C and Cg.
- Max push constant block: 128 bytes.

With `debug = shaders` the log shows every mapping, for example
`uniform uMvp <- push constant bytes [0..64)`. With validation on, a uniform
that reads bytes you never pushed is reported.

Matrices: upload column-major (OpenGL/GLM style) and multiply as
`mul(vector, matrix)` in Cg. VkVita copies the 16 floats as they are.

### Textures (descriptor binding -> TEXUNIT)

`VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` at binding `n` binds to texture
unit `n`, so declare the sampler with `: TEXUNITn`. Samplers used by the
vertex shader are bound for the vertex stage too. Bindings from all bound
sets share the 16 units, so keep binding numbers unique across sets.

Separate `SAMPLER` / `SAMPLED_IMAGE` descriptors only work with SPIR-V
shaders, because Cg samplers always combine both and a hand written Cg
shader has no way to say which two descriptors to pair.

### Uniform buffers (descriptor binding -> BUFFER[n])

`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER(_DYNAMIC)` at binding `n` is bound to GXM
uniform buffer slot `n` (0 to 13) for both stages. In Cg that is a uniform
block declared with `: BUFFER[n]`. GXM reads the memory directly, so its
layout must match the Cg block's packing; use `float4` and `float4x4`
members. This path is less exercised than push constants; prefer push
constants for small per-draw data.

## Shipping without the runtime compiler

1. Run your app once with `compat = shader_cache` (or `debug = dump_shaders`).
   Every compiled shader is written to `ux0:data/vkvita/shaders/<hash>_v.gxp`
   / `_f.gxp`; the log names each file.
2. Copy the `.gxp` files into your VPK (for example `shaders/`), then either:
   - keep your Cg sources in code, set `cfg.shaderDir = "app0:shaders"` and
     `VKV_COMPAT_SHADER_CACHE`; the hash lookup loads the GXP and the
     compiler is never touched, or
   - load them directly with `vkvCreateShaderModuleFromFile(device,
     "app0:shaders/xxx_v.gxp", ...)` and build with `VKV_WITH_SHARK=OFF`.

The hash covers the exact source text, so editing a shader produces a new
file. Offline compilation with Sony's `psp2cgc` also produces valid GXP.

## Logging

`debug = shaders` prints per module: source type and size, compile time,
compiler warnings/errors with line numbers, and the full parameter table
(name, category, type, components, semantic, resource and container index)
plus the location and push constant mapping for each pipeline. Paste that
table into a bug report when a shader misbehaves.
