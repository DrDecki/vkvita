#version 450

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(set = 0, binding = 1) uniform Scene {
    vec4 lightDir;
    vec4 ambient;
    float time;
} scene;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec3 inOffset;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vLight;

void main() {
    vec3 p = inPos + inOffset;
    p.z += 0.25 * sin(scene.time * 2.0 + inOffset.x + inOffset.y);
    gl_Position = pc.mvp * vec4(p, 1.0);
    vUV = inUV;
    vColor = inColor;
    vLight = max(dot(normalize(inPos), normalize(scene.lightDir.xyz)), 0.0);
}
