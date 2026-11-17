#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 2 0.5 0.1 0  0.3 1.5 0.2 0  0 0.4 3 0.1  0.2 0.1 0.3 1
layout(set = 0, binding = 0) uniform U { mat4 m; } u;
void main() {
    mat4 i4 = inverse(u.m);
    mat3 i3 = inverse(mat3(u.m));
    mat2 i2 = inverse(mat2(u.m));
    vec4 p = vec4(vUv, 0.5, 1.0);
    outColor = vec4((i4 * (u.m * p)).xy, (i3 * vec3(vUv, 1.0)).z * 0.2, (i2 * vUv).x * 0.3 + 0.5);
}
