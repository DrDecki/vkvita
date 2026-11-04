#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() { float x = vUv.x; float r; if (x < 0.25) r = 0.1; else if (x < 0.5) r = 0.2; else if (x < 0.75) r = 0.3; else r = 0.4; outColor = vec4(r, x > 0.5 && vUv.y < 0.5 ? 1.0 : 0.0, x < 0.2 || vUv.y > 0.8 ? 1.0 : 0.0, 1); }
