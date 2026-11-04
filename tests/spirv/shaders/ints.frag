#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 i7 i-3 u5 i1
layout(set = 0, binding = 0) uniform U { int a; int b; uint c; bool d; } u;
void main() { int x = int(vUv.x * 16.0); int y = (x << 1) + u.a * u.b - int(u.c); float f = u.d ? 1.0 : 0.0; outColor = vec4(float(y) * 0.01, float(x / 3) * 0.1, float(x % 5) * 0.1, f); }
