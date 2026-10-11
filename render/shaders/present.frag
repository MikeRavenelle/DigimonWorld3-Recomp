#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Shows the VRAM display area, nearest-neighbour scaled. In 24-bit mode the pixels are packed as three bytes
// each into the 16-bit VRAM words. VRAM is (1024 * scale) x (512 * scale): 15-bit output reads the full
// resolution, 24-bit output (which only ever holds uploaded data) reads the top-left subpixel of each word.

layout(set = 0, binding = 0) uniform texture2D uVram;

layout(push_constant) uniform Params {
    vec4 dest;
    vec2 window;
    ivec4 src;
    int color24;
    int linear;  // bilinear filtering
    int scale;   // internal resolution multiplier
} params;

layout(location = 0) in vec2 vFrac;
layout(location = 0) out vec4 outColor;

uint packWord(ivec2 t) {
    uvec4 b = uvec4(texelFetch(uVram, t, 0) * 255.0 + 0.5);
    return (b.r >> 3) | ((b.g >> 3) << 5) | ((b.b >> 3) << 10) | ((b.a >> 7) << 15);
}

// Word at a native VRAM position (top-left subpixel).
uint vramWord(ivec2 p) {
    return packWord(ivec2(p.x & 1023, p.y & 511) * params.scale);
}

uint vramByte(int y, int byteIndex) {
    uint w = vramWord(ivec2(byteIndex >> 1, y));
    return (byteIndex & 1) != 0 ? (w >> 8) & 255u : w & 255u;
}

vec3 pixelAt(ivec2 s) {
    if (params.color24 != 0) {
        s = clamp(s, ivec2(0), params.src.zw - 1);
        int y = params.src.y + s.y;
        int byteIndex = params.src.x * 2 + s.x * 3;
        return vec3(vramByte(y, byteIndex), vramByte(y, byteIndex + 1), vramByte(y, byteIndex + 2)) / 255.0;
    }
    // Scaled coordinates relative to the display origin.
    s = clamp(s, ivec2(0), params.src.zw * params.scale - 1);
    ivec2 t = (params.src.xy * params.scale + s) % ivec2(1024 * params.scale, 512 * params.scale);
    uint w = packWord(t);
    uvec3 c = uvec3(w & 31u, (w >> 5) & 31u, (w >> 10) & 31u);
    c = (c << 3) | (c >> 2);
    return vec3(c) / 255.0;
}

void main() {
    vec2 pos = vFrac * vec2(params.src.zw) * (params.color24 != 0 ? 1.0 : float(params.scale));
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
