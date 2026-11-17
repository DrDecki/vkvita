#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@tex 0 1
//@tex 1 2
//@img 2 3
//@smp 3
layout(set = 0, binding = 0) uniform sampler2D a;
layout(set = 0, binding = 1) uniform sampler2D b;
layout(set = 0, binding = 2) uniform texture2D c;
layout(set = 0, binding = 3) uniform sampler s;
/* textures passed to functions, also through a second function and as separate image and sampler */
vec4 blur(sampler2D t, vec2 uv) { return (texture(t, uv) + texture(t, uv + vec2(0.1, 0.0))) * 0.5; }
vec4 twice(sampler2D t, vec2 uv) { return blur(t, uv) * 0.5 + blur(t, uv * 2.0) * 0.5; }
vec4 sep(texture2D i, sampler sm, vec2 uv) { return texture(sampler2D(i, sm), uv); }
void main() { outColor = blur(a, vUv) * 0.4 + twice(b, vUv) * 0.4 + sep(c, s, vUv) * 0.2; }
