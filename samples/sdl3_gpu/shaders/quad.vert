#version 450
/* SDL_GPU puts vertex uniform buffers in set 1 */
layout(set = 1, binding = 0) uniform Placement {
    vec4 offsetScale; /* xy offset, zw scale */
} u;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 vUv;

void main() {
    vUv = inUv;
    gl_Position = vec4(inPos * u.offsetScale.zw + u.offsetScale.xy, 0.0, 1.0);
}
