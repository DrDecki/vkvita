#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@pc i8
layout(push_constant) uniform P { int n; } pc;
void main() { float a = 0.0; int i = 0; while (i < pc.n) { i++; if ((i & 1) == 0) continue; a += 0.05 * vUv.y; } do { a += 0.01; i--; } while (i > 4); outColor = vec4(a, float(i) * 0.1, 0.0, 1.0); }
