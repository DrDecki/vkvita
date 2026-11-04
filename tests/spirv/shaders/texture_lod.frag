#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@tex 0 3
layout(set = 0, binding = 0) uniform sampler2D a;
void main() { outColor = textureLod(a, vUv, 0.0) + texture(a, vUv, 0.0) * 0.5 + textureProj(a, vec3(vUv, 2.0)) * 0.25; }
