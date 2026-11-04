#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
struct R { vec3 c; float w; int n; };
R make(vec2 p) { R r; r.c = vec3(p, p.x * p.y); r.w = length(p); r.n = int(p.x * 4.0); return r; }
void main() { R r = make(vUv); R q = r; q.c.x += 0.5; outColor = vec4(q.c * 0.5, r.w * 0.2 + float(r.n) * 0.1); }
