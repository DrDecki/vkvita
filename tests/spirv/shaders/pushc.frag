#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@pc 0.5 i3 0.25 0.75
layout(push_constant) uniform P { float s; int n; vec2 off; } pc;
void main() { outColor = vec4(vUv * pc.s + pc.off, float(pc.n) * 0.1, 1.0); }
