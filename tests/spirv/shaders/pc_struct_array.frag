#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@pc 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 i1
struct S { vec2 a; vec2 b; };
layout(push_constant) uniform P { S s[2]; int k; } pc;
void main() { outColor = vec4(pc.s[pc.k].a + pc.s[1 - pc.k].b * vUv, pc.s[0].b); }
