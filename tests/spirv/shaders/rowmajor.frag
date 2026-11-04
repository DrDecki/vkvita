#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16
layout(set = 0, binding = 0, row_major) uniform U { mat4 m; } u;
void main() { outColor = (u.m * vec4(vUv, 1.0, 0.5)) * 0.01 + u.m[2] * 0.02; }
