#version 450
/* SDL_GPU puts fragment samplers in set 2 and fragment uniform buffers in set 3 */
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Tint {
    vec4 color;
} u;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tex, vUv) * u.color;
}
