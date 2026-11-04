#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
//@ubo 0 u1 u0 u1 u0 i-2 i5 _ _
layout(set = 0, binding = 0) uniform U { bvec4 b; ivec2 iv; } u;
void main() { outColor = vec4(u.b.x ? 1.0 : 0.0, u.b.y ? 1.0 : 0.0, float(u.iv.x + u.iv.y) * 0.1, any(u.b.zw) ? vUv.x : vUv.y); }
