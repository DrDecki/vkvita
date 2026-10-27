#version 450

layout(set = 0, binding = 0) uniform samplerCube env;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vDir;

layout(location = 0) out vec4 outColor;

void main() {
    /* four bands per face reading levels 0, 2, 4 and 6 */
    float lod = floor(clamp(vUV.x, 0.0, 0.999) * 4.0) * 2.0;
    outColor = textureLod(env, normalize(vDir), lod);
}
