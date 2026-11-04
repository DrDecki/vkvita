#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() { vec2 p = vUv * 4.0 - 2.0; float a = atan(p.y, p.x); float b = mod(p.x * 3.0, 1.3); float c = clamp(p.y, -0.5, 0.5); vec3 n = normalize(vec3(p, 1.0)); float d = smoothstep(0.2, 0.8, vUv.x); float e = step(0.5, vUv.y); vec2 m = mix(vec2(0.1), p, 0.3); outColor = vec4(a * 0.1 + b * 0.1, c + d * 0.2, n.z + e * 0.1, dot(m, m) * 0.05 + length(p) * 0.01 + fract(p.x) * 0.1 + sign(p.y) * 0.01 + pow(vUv.x, 2.2) * 0.1 + inversesqrt(vUv.y + 1.0) * 0.1); }
