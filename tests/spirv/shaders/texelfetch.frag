#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@tex 0 2
//@img 1 5
layout(set = 0, binding = 0) uniform sampler2D a;
layout(set = 0, binding = 1) uniform texture2D b;
void main() { ivec2 p = ivec2(vUv * 8.0); ivec2 sa = textureSize(a, 0); ivec2 sb = textureSize(b, 0); outColor = texelFetch(a, p, 0) * 0.5 + texelFetch(b, ivec2(7) - p, 0) * 0.25 + vec4(sa, sb) * 0.01 + vec4(float(textureQueryLevels(a)) * 0.1); }
