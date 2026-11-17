#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@tex 0 1
layout(set = 0, binding = 0) uniform sampler2D a;
void main() { outColor = textureGrad(a, vUv, vec2(0.02, 0.0), vec2(0.0, 0.02)) * 0.5 + textureGrad(a, vUv * 2.0, dFdx(vUv), dFdy(vUv)) * 0.5; }
