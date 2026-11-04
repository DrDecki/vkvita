#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
vec2 g = vec2(0.25, 0.5);
float h;
void bump() { g *= 2.0; h = g.x + g.y; }
void main() { bump(); bump(); outColor = vec4(g * 0.1, h * 0.1, 1); }
