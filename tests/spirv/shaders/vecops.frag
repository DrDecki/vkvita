#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() { vec4 v = vec4(vUv, 1.0 - vUv); bvec4 g = greaterThan(v, vec4(0.5)); vec4 s = mix(v, v.wzyx, bvec4(g.x, false, true, g.w)); float any_ = any(g) ? 1.0 : 0.0; float all_ = all(lessThan(v, vec4(2.0))) ? 1.0 : 0.0; vec3 c = cross(v.xyz, v.yzx); s.y = v.x; s.zw = v.yx * 2.0; v[int(vUv.x * 3.9)] = 0.75; outColor = s * 0.5 + vec4(any_, all_, c.x, v.w) * 0.1 + v * 0.1; }
