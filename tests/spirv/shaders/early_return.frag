#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
float f(float x) { for (int i = 0; i < 8; i++) { if (x > 0.9) return 1.0; x += 0.1; } return x; }
void main() { if (vUv.y > 0.8) { outColor = vec4(1, 0, 0, 1); return; } outColor = vec4(f(vUv.x), vUv.y, 0, 1); }
