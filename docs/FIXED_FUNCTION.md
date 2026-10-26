# Fixed-function layer

`#include <vkvita/vkvita_ff.h>`

A compatibility layer for OpenGL 1.x style code: matrix stacks,
`glBegin/glEnd`-style immediate mode (including quads), client arrays,
blending, alpha test, depth test, culling and one texture. It records into
an ordinary `VkCommandBuffer` inside a render pass, so it mixes freely with
real Vulkan draws. Use it to bring an old renderer up quickly, then move hot
paths to real pipelines.

## Setup

```c
VkvFFInitInfo info = { 0 };
info.renderPass = renderPass;           /* the pass the FF draws are recorded in */
VkvFFContext ff;
vkvffCreateContext(device, &info, &ff);  /* compiles 3 small built-in shaders */
...
vkvffDestroyContext(ff);
```

The built-in shaders are Cg compiled at runtime (needs
`libshacccg.suprx`). To avoid that, compile them once (with
`debug = dump_shaders` they are written to `ux0:data/vkvita/shaders`) and
pass the GXP blobs in `VkvFFInitInfo::vertexGxp`, `fragmentColorGxp`,
`fragmentTexGxp`.

## Per frame

```c
VkCommandBuffer cb = ...;                /* inside vkCmdBeginRenderPass */
vkvffBeginFrame(ff);                     /* once per frame: rotates the vertex buffer */

vkvffMatrixMode(ff, VKVFF_PROJECTION);
vkvffLoadIdentity(ff);
vkvffPerspective(ff, 60.0f, 960.0f / 544.0f, 0.1f, 100.0f);
vkvffMatrixMode(ff, VKVFF_MODELVIEW);
vkvffLoadIdentity(ff);
vkvffTranslatef(ff, 0, 0, -5);
vkvffRotatef(ff, angle, 0, 1, 0);

vkvffEnable(ff, VKVFF_DEPTH_TEST | VKVFF_TEXTURE_2D);
vkvffBindTexture(ff, imageView, sampler);
vkvffBegin(ff, cb, VKVFF_QUADS);
vkvffTexCoord2f(ff, 0, 0); vkvffVertex3f(ff, -1, -1, 0);
vkvffTexCoord2f(ff, 1, 0); vkvffVertex3f(ff,  1, -1, 0);
vkvffTexCoord2f(ff, 1, 1); vkvffVertex3f(ff,  1,  1, 0);
vkvffTexCoord2f(ff, 0, 1); vkvffVertex3f(ff, -1,  1, 0);
vkvffEnd(ff);
```

## Mapping from OpenGL

| OpenGL | VkVita |
|---|---|
| `glMatrixMode`, `glLoadIdentity`, `glLoadMatrixf`, `glMultMatrixf`, `glPushMatrix`, `glPopMatrix` | `vkvffMatrixMode` ... `vkvffPopMatrix` (32 deep per stack) |
| `glTranslatef`, `glRotatef`, `glScalef`, `glOrtho`, `glFrustum`, `gluPerspective` | same names with `vkvff` prefix |
| `glBegin(GL_POINTS/LINES/TRIANGLES/TRIANGLE_STRIP/TRIANGLE_FAN/QUADS)` | `vkvffBegin(ff, cb, VKVFF_...)` |
| `glColor4f/4ub`, `glTexCoord2f`, `glVertex2f/3f`, `glEnd` | same with `vkvff` prefix |
| `glDrawArrays` with client arrays | `vkvffDrawArrays(ff, cb, prim, count, xyz, uv, rgba)` |
| `glEnable/glDisable(GL_TEXTURE_2D/BLEND/DEPTH_TEST/CULL_FACE/ALPHA_TEST)` | `vkvffEnable/Disable(VKVFF_...)` |
| `glDepthMask` | `VKVFF_DEPTH_WRITE` (on by default) |
| `glBlendFunc` | `vkvffBlendFunc(ff, VK_BLEND_FACTOR_..., ...)` |
| `glAlphaFunc(GL_GEQUAL, ref)` | `vkvffAlphaFunc(ff, ref)` |
| `glCullFace` + `glFrontFace` | `vkvffCullFace(ff, VK_CULL_MODE_..., VK_FRONT_FACE_...)` |
| `glBindTexture` | `vkvffBindTexture(ff, view, sampler)` |
| texture matrix | applied to UVs on the CPU |

`vkvffOrtho/Frustum/Perspective` produce matrices for Vulkan clip space
(Y down, depth 0..1) that look the same as in OpenGL. With
`VKV_COMPAT_GL_CLIP_SPACE` they produce classic OpenGL matrices instead.

## Performance notes

- Each `vkvffEnd` / `vkvffDrawArrays` is one GXM draw. Batch geometry into
  few large Begin/End blocks.
- Every distinct state combination creates one internal pipeline the first
  time it is used (up to 64). Expect a short hitch the first time.
- Vertices stream through a ring buffer (`ff_buffer_kb` per frame,
  `ff_frames` frames). Raise `ff_buffer_kb` if the log reports it full.
  `ff_frames` must be larger than the frames you keep in flight.
- Max 65535 vertices per draw.
