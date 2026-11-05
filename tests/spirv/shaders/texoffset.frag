#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@tex 0 3
layout(set = 0, binding = 0) uniform sampler2D a;
void main() { outColor = textureOffset(a, vUv, ivec2(1, -2)) * 0.5 + textureLodOffset(a, vUv, 0.0, ivec2(-1, 0)) * 0.5; }
