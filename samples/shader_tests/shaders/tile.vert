#version 450

layout(push_constant) uniform Push {
    int tile;
    int which;
    float weights[4];
} pc;

layout(location = 0) out vec2 vUv;

void main() {
    int i = gl_VertexIndex;
    float x = (i == 1 || i == 2 || i == 4) ? 1.0 : 0.0;
    float y = (i == 2 || i == 4 || i == 5) ? 1.0 : 0.0;
    float col = float(pc.tile % 4), row = float(pc.tile / 4);
    vec2 origin = vec2(-0.92 + col * 0.47, -0.9 + row * 0.62);
    gl_Position = vec4(origin + vec2(x * 0.42, y * 0.54), 0.5, 1.0);
    vUv = vec2(x, y);
}
