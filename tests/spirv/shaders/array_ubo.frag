#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0 1.1 1.2 1.3 1.4 1.5 1.6 9 9 9 9
layout(set = 0, binding = 0) uniform U { vec4 c[4]; } u;
void main() { int i = int(vUv.y * 3.99); outColor = u.c[i] + u.c[3 - i] * 0.5; }
