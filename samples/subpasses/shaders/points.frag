#version 450

layout(location = 0) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    /* round points like the vkQuake2 particles */
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    if (dot(c, c) > 1.0)
        discard;
    outColor = vec4(vColor, 1.0);
}
