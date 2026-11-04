#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@tex 0 5
//@pc i1
layout(set = 0, binding = 0) uniform sampler2D t[1];
layout(push_constant) uniform P { int i; } pc;
void main() { outColor = texture(t[0], vUv) + texture(t[pc.i - 1], vUv * 3.0); }
