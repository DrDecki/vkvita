#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() { mat3x2 m = outerProduct(vUv, vec3(1.0, vUv.y, 0.5)); vec2 r = m * vec3(0.5, 1.0, 2.0); outColor = vec4(r, m[1]); }
