#version 450
layout(location = 0) in vec2 vUv;
layout(location = 1) in Data { vec3 a; float b; } inData;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(inData.a, inData.b) + vec4(vUv, 0, 0) * 0.1; }
