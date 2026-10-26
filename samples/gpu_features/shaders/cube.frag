#version 450

layout(set = 0, binding = 0) uniform samplerCube env;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vDir;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(env, normalize(vDir));
}
