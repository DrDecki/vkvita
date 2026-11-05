#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 vUv;
layout(location = 1) out Data { vec3 a; float b; } outData;
void main() { vUv = inUv; outData.a = vec3(inUv, 0.25); outData.b = inUv.x * inUv.y; gl_Position = vec4(inPos, 0.0, 1.0); }
