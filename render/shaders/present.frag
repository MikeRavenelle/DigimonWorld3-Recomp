#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Shows the VRAM display area, nearest-neighbour scaled. In 24-bit mode the pixels are packed as three bytes
// each into the 16-bit VRAM words.

layout(set = 0, binding = 0) uniform texture2D uVram;

layout(push_constant) uniform Params {
    vec4 dest;
    vec2 window;
    ivec4 src;
    int color24;
    int linear;  // bilinear filtering
} params;

layout(location = 0) in vec2 vFrac;
layout(location = 0) out vec4 outColor;

uint vramWord(ivec2 p) {
    uvec4 b = uvec4(texelFetch(uVram, ivec2(p.x & 1023, p.y & 511), 0) * 255.0 + 0.5);
    return (b.r >> 3) | ((b.g >> 3) << 5) | ((b.b >> 3) << 10) | ((b.a >> 7) << 15);
}

uint vramByte(int y, int byteIndex) {
    uint w = vramWord(ivec2(byteIndex >> 1, y));
    return (byteIndex & 1) != 0 ? (w >> 8) & 255u : w & 255u;
}

vec3 pixelAt(ivec2 s) {
    s = clamp(s, ivec2(0), params.src.zw - 1);
    int y = params.src.y + s.y;
    if (params.color24 != 0) {
        int byteIndex = params.src.x * 2 + s.x * 3;
        return vec3(vramByte(y, byteIndex), vramByte(y, byteIndex + 1), vramByte(y, byteIndex + 2)) / 255.0;
    }
    uint w = vramWord(ivec2(params.src.x + s.x, y));
    uvec3 c = uvec3(w & 31u, (w >> 5) & 31u, (w >> 10) & 31u);
    c = (c << 3) | (c >> 2);
    return vec3(c) / 255.0;
}

void main() {
    vec2 pos = vFrac * vec2(params.src.zw);
    vec3 rgb;
    if (params.linear != 0) {
        vec2 q = pos - 0.5;
        ivec2 i = ivec2(floor(q));
        vec2 f = q - vec2(i);
        rgb = mix(mix(pixelAt(i), pixelAt(i + ivec2(1, 0)), f.x),
                  mix(pixelAt(i + ivec2(0, 1)), pixelAt(i + ivec2(1, 1)), f.x), f.y);
    } else {
        rgb = pixelAt(ivec2(pos));
    }
    outColor = vec4(rgb, 1.0);
}
