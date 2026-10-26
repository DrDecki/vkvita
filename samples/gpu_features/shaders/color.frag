#version 450

layout(push_constant) uniform Push {
    layout(offset = 64) vec4 color;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vDir;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = pc.color;
}
