#version 450

layout(location = 0) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    /* a one pixel checker on the left, the post pass checks every pixel of it */
    if (gl_FragCoord.x < 160.0) {
        float p = mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0);
        outColor = vec4(vec3(p), 1.0);
    } else {
        outColor = vec4(vColor, 1.0);
    }
}
