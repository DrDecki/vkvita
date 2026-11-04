#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() { if (vUv.x < 0.3) discard; outColor = vec4(vUv, 1, 1); }
