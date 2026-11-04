#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 0.5 0 0 0 0 0.5 0 0 0 0 0.5 0 0.1 0.2 0.3 1
layout(set = 0, binding = 0) uniform U { mat4 a; mat4 b; } u;
void main() { mat4 c = u.a * u.b; mat4 d = mat4(vec4(vUv,0,1), vec4(1,0,0,0), vec4(0,1,0,0), vec4(0,0,1,0)); outColor = (c * vec4(vUv, 0.5, 1.0)) * 0.01 + (d * vec4(1,2,3,4)) * 0.1; }
