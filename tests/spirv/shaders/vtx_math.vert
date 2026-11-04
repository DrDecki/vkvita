#version 450
//@ubo 0 0.5 0 0 0 0 0.5 0 0 0 0 1 0 0.1 0.2 0 1
layout(set = 0, binding = 0) uniform U { mat4 mvp; } u;
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 vUv;

layout(location = 2) out vec3 vExtra;
void main() { vUv = inUv; vExtra = vec3(inUv.x * 2.0, inUv.y * inUv.x, 0.5); gl_Position = u.mvp * vec4(inPos, 0.0, 1.0); }
