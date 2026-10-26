#version 450

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vDir;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vUV = inUV;
    vDir = inPos;
}
