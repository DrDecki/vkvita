#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec2 views[3];
layout(location = 4) out vec4 rot[2];
layout(location = 10) out vec4 extra;
layout(location = 11) out vec4 extra2;
void main() {
    vUv = inUv;
    for (int i = 0; i < 3; i++) views[i] = inUv * float(i + 1) * 0.3;
    rot[0] = vec4(inUv, 0.2, 1.0); rot[1] = vec4(0.1, inUv.yx, 0.5);
    extra = vec4(inUv.x, 0.25, inUv.y, 1.0); extra2 = vec4(0.75, inUv, 0.5);
    gl_Position = vec4(inPos, 0.0, 1.0);
}
