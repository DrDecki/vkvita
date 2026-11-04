#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
const vec2 pts[4] = vec2[](vec2(0.1, 0.2), vec2(0.3, 0.4), vec2(0.5, 0.6), vec2(0.7, 0.8));
void main() { int i = int(vUv.x * 3.99); vec2 a[3]; a[0] = vUv; a[1] = pts[i]; a[2] = a[0] + a[1]; outColor = vec4(a[2], pts[3 - i]); }
