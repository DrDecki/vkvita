#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
float f(float x, inout vec2 acc) { acc += vec2(x, x * 2.0); if (x > 0.5) return x * x; return x * 0.5; }
vec3 g(vec3 v) { vec2 a = vec2(0); float r = f(v.x, a) + f(v.y, a); return vec3(r, a); }
void main() { outColor = vec4(g(vec3(vUv, 0.3)), 1.0); }
