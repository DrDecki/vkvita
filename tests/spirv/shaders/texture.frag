#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@tex 0 1
//@tex 1 2
layout(set = 0, binding = 0) uniform sampler2D a;
layout(set = 0, binding = 1) uniform sampler2D b;
void main() { outColor = texture(a, vUv) * 0.5 + texture(b, vUv * 2.0) * 0.5; }
