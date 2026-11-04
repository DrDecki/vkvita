#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 0.1 0.2 0.3 0.4 0.5 _ _ _ 0.6 0.7 0.8 0.9 1.0 _ _ _ 2.0 _ _ _ 3.0 _ _ _ 0.25
struct L { vec4 c; float i; };
layout(set = 0, binding = 0) uniform U { L l[2]; float w[2]; float z; } u;
struct T2 { vec2 p; float q; };
T2 mk(vec2 p) { T2 t; t.p = p * 2.0; t.q = p.x + p.y; return t; }
void main() { T2 t = mk(vUv); int k = vUv.x > 0.5 ? 1 : 0; outColor = u.l[k].c * u.l[k].i + vec4(t.p, t.q, 0) * 0.1 + vec4(u.w[k] * 0.1, u.z, 0, 0); }
