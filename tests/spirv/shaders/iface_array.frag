#version 450
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 views[3];
layout(location = 4) in vec4 rot[2];
layout(location = 10) in vec4 extra;
layout(location = 11) in vec4 extra2;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 s = vec2(0.0);
    for (int i = 0; i < 3; i++) s += views[i];
    outColor = vec4(s * 0.3, rot[0].x * rot[1].z, 1.0) * 0.5 + extra * 0.25 + extra2 * 0.25;
}
