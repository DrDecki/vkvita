#version 450

layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput scene;

layout(location = 0) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = subpassLoad(scene);
    if (gl_FragCoord.x < 160.0) {
        /* green where the input attachment gave back exactly this pixel */
        float p = mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0);
        float ok = step(abs(c.r - p), 0.5);
        outColor = mix(vec4(0.95, 0.1, 0.1, 1.0), vec4(0.1, 0.85, 0.2, 1.0), ok);
    } else if (gl_FragCoord.x < 560.0) {
        outColor = c;
    } else {
        outColor = vec4(1.0 - c.rgb, 1.0);
    }
}
