#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 1 2 3 _ 4 5 6 _ 0.5 0.25 -1 _
layout(set = 0, binding = 0) uniform U { mat2x3 m; vec4 v; } u;
void main() { vec3 r = u.m * vUv; vec2 s = u.v.xyz * u.m; outColor = vec4(r * 0.1, s.x * 0.1 + s.y * 0.05); }
