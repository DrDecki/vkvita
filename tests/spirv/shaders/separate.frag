#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@img 0 4
//@smp 1
layout(set = 0, binding = 0) uniform texture2D img;
layout(set = 0, binding = 1) uniform sampler smp;
void main() { outColor = texture(sampler2D(img, smp), vUv); }
