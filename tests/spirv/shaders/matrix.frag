#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 0.5 0.25 -1 2
layout(set = 0, binding = 0) uniform U { mat4 m; vec4 v; } u;
void main() { vec4 p = vec4(vUv, 1.0, 0.5); outColor = (u.m * p) * 0.01 + (p * u.m) * 0.02 + u.m[1] * 0.03 + vec4(u.m[2][3]) * 0.001 + transpose(u.m)[0] * 0.004; }
