#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@stex 0 6
layout(set = 0, binding = 0) uniform sampler2DShadow s;
void main() { float a = texture(s, vec3(vUv, vUv.x * 0.8)); float b = textureProj(s, vec4(vUv * 2.0, 0.6, 2.0)); outColor = vec4(a, b, a * b, 1.0); }
