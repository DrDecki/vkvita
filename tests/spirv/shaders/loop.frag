#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@pc i5 0.1
layout(push_constant) uniform P { int n; float k; } pc;
void main() { float a = 0.0; for (int i = 0; i < pc.n; i++) { a += vUv.x * pc.k * float(i); if (a > 0.6) break; } outColor = vec4(a, 0.0, 0.0, 1.0); }
