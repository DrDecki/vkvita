#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() {
    int i = int(vUv.x * 6.0);
    float r = 0.0;
    switch (i) {
    case 0: r = 0.1; break;
    case 1: case 2: r = 0.2; break;
    case 3: r = 0.3;
    case 4: r += 0.4; break;
    default: r = 0.9;
    }
    outColor = vec4(r, vUv.y, 0, 1);
}
