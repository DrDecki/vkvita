#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(set = 0, binding = 1) uniform Scene {
    vec4 lightDir;
    vec4 ambient;
    float time;
} scene;

layout(push_constant) uniform Push {
    layout(offset = 64) vec4 tint;
    int mode;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vLight;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(tex, vUV) * vColor;
    if (pc.mode == 1)
        c = vec4(vUV, 0.5 + 0.5 * sin(scene.time), 1.0);
    else if (pc.mode == 2)
        c = vec4(vec3(dot(c.rgb, vec3(0.299, 0.587, 0.114))), 1.0);
    outColor = c * pc.tint * (scene.ambient + vec4(vec3(vLight), 0.0));
}
