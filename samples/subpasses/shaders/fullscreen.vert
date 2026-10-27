#version 450

/* a full screen triangle without a vertex buffer, the way vkQuake2 draws its
 * post processing pass */
layout(location = 0) out vec3 vColor;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vColor = vec3(1.0);
}
