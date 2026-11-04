#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(gl_FragCoord.xy / 32.0, gl_FrontFacing ? 1.0 : 0.0, 1.0); }
