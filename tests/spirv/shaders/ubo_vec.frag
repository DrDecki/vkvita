#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 0.5 0.25 0.125 1.0 2.0 3.0 _ _ 0.1 0.2 0.3 0.4
layout(set = 0, binding = 0) uniform U { vec4 a; vec2 b; vec4 c; } u;
void main() { outColor = u.a * vUv.x + vec4(u.b, u.b.yx) * 0.1 + u.c * vUv.y; }
