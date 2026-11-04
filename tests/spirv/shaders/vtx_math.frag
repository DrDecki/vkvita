#version 450
layout(location = 0) in vec2 vUv;

layout(location = 2) in vec3 vExtra;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vUv, 0.5, vExtra.x + vExtra.y); }
