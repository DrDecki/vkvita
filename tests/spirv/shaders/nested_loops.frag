#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() { float a = 0.0; for (int i = 0; i < 3; i++) { for (int j = 0; j < 4; j++) { if (j == i) continue; a += vUv.x * 0.02 * float(i + j); } if (a > 0.2 * vUv.y + 0.1) break; } outColor = vec4(a, vUv.y, 0, 1); }
