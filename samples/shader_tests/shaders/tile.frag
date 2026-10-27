#version 450

struct Light {
    vec4 pos;
    vec4 color;
    float intensity;
    int kind;
};

struct Inner {
    vec2 a;
    float b[2];
};

struct Outer {
    Inner inner;
    mat4 m;
};

layout(set = 0, binding = 0) uniform Tests {
    Light lights[2];
    Outer o;
    vec3 amb;
    bool flag;
} u;

layout(set = 0, binding = 1) uniform sampler2D texs[3];
layout(set = 0, binding = 2) uniform texture2D imgs[2];
layout(set = 0, binding = 3) uniform sampler smp;

layout(std430, set = 0, binding = 4) readonly buffer Bones {
    mat4 bones[];
} bones;

layout(std430, set = 0, binding = 5) readonly buffer Values {
    float v[8];
} values;

struct Item {
    vec4 a;
    float b;
};

layout(std430, set = 0, binding = 6) readonly buffer Items {
    vec4 header;
    Item items[3];
} items;

layout(push_constant) uniform Push {
    int tile;
    int which;
    float weights[4];
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

bool near(vec4 a, vec4 b) {
    vec4 d = abs(a - b);
    return max(max(d.x, d.y), max(d.z, d.w)) < 0.02;
}

void main() {
    bool ok = false;
    vec2 uv = vec2(0.5);
    if (pc.tile == 0) ok = near(u.lights[1].color, vec4(0.25, 0.5, 0.75, 1.0)) && abs(u.lights[0].intensity - 1.5) < 0.01;
    else if (pc.tile == 1) ok = u.lights[0].kind == 3 && u.lights[1].kind == 7;
    else if (pc.tile == 2) ok = abs(u.o.inner.b[1] - 2.5) < 0.01 && near(vec4(u.o.inner.a, 0.0, 0.0), vec4(1.0, 2.0, 0.0, 0.0));
    else if (pc.tile == 3) ok = near(u.o.m * vec4(1.0, 0.0, 0.0, 1.0), vec4(11.0, 20.0, 30.0, 1.0));
    else if (pc.tile == 4) ok = near(texture(texs[2], uv), vec4(0.0, 0.0, 1.0, 1.0));
    else if (pc.tile == 5) ok = near(texture(texs[pc.which], uv), vec4(0.0, 1.0, 0.0, 1.0));
    else if (pc.tile == 6) ok = near(texture(sampler2D(imgs[1], smp), uv), vec4(1.0, 1.0, 0.0, 1.0));
    else if (pc.tile == 7) ok = near(vec4(u.amb, 0.0), vec4(0.1, 0.2, 0.3, 0.0)) && u.flag;
    else if (pc.tile == 8) ok = near(bones.bones[1] * vec4(0.0, 0.0, 0.0, 1.0), vec4(4.0, 5.0, 6.0, 1.0));
    else if (pc.tile == 9) ok = abs(values.v[3] - 3.5) < 0.01 && abs(values.v[7] - 7.5) < 0.01;
    else if (pc.tile == 10) ok = abs(items.items[2].b - 0.75) < 0.01 && near(items.header, vec4(1.0, 2.0, 3.0, 4.0));
    else ok = abs(pc.weights[2] - 0.25) < 0.01 && abs(pc.weights[3] - 0.125) < 0.01;
    /* a darker border shows the tiles apart */
    float edge = min(min(vUv.x, 1.0 - vUv.x), min(vUv.y, 1.0 - vUv.y)) < 0.04 ? 0.6 : 1.0;
    outColor = (ok ? vec4(0.1, 0.85, 0.3, 1.0) : vec4(0.9, 0.15, 0.1, 1.0)) * vec4(vec3(edge), 1.0);
}
