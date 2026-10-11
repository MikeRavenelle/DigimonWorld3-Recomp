#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Pixel stage of the primitive pipeline. Reproduces the PS1 pixel path on integers: texture fetch through the
// CLUT from the VRAM copy, colour modulation, dithering, 5-bit semi-transparent blending against the previous
// VRAM contents and mask-bit handling. VRAM is stored as RGBA8: the 5-bit channels expanded to 8 bits and the
// mask bit in alpha, so every 16-bit word can be rebuilt exactly.
//
// With an internal resolution above 1 the target is scale times larger; blending and mask handling work per
// subpixel on the scaled destination, dithering uses the native pixel position.
//
// state0: bit0 textured, 1 raw texture, 2 semi-transparent, 3 dither, 4 check mask, 5 set mask,
//         6-7 blend mode, 8-9 texture depth (0: 4 bit, 1: 8 bit, 2: 15 bit)
// state1: bits 0-15 CLUT attribute, 16-19 texture page x / 64, 20 texture page y / 256
// state2: texture window: bits 0-4 mask x, 5-9 mask y, 10-14 offset x, 15-19 offset y (8-pixel units)

layout(set = 0, binding = 0) uniform texture2D uVram;  // copy of VRAM as of the last synchronisation

layout(push_constant) uniform Params {
    int scale;  // internal resolution multiplier (1 = native)
} params;

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uvec3 vState;

layout(location = 0) out vec4 outColor;

const int kDither[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

// Word at a position of the scaled VRAM (used for the destination, one value per subpixel).
uint vramWord(ivec2 p) {
    uvec4 b = uvec4(texelFetch(uVram, p, 0) * 255.0 + 0.5);
    return (b.r >> 3) | ((b.g >> 3) << 5) | ((b.b >> 3) << 10) | ((b.a >> 7) << 15);
}

// Word at a native VRAM position: texture and CLUT data is read at the top-left subpixel, so indices and
// colours are exactly what the game stored and are never filtered.
uint nativeWord(ivec2 p) { return vramWord(p * params.scale); }

uint expand5(uint v) { return (v << 3) | (v >> 2); }

uint fetchTexel(uint s0, uint s1, uint s2, ivec2 uv) {
    uint depth = (s0 >> 8) & 3u;
    int baseX = int((s1 >> 16) & 15u) * 64;
    int baseY = int((s1 >> 20) & 1u) * 256;
    uint maskX = s2 & 31u, maskY = (s2 >> 5) & 31u, offX = (s2 >> 10) & 31u, offY = (s2 >> 15) & 31u;
    uint u = (uint(uv.x) & ~(maskX * 8u) & 255u) | ((offX & maskX) * 8u & 255u);
    uint v = (uint(uv.y) & ~(maskY * 8u) & 255u) | ((offY & maskY) * 8u & 255u);
    int row = (baseY + int(v)) & 511;
    if (depth == 2u) return nativeWord(ivec2((baseX + int(u)) & 1023, row));
    int clutX = int(s1 & 63u) * 16, clutY = int((s1 >> 6) & 511u);
    uint index;
    if (depth == 0u) {
        uint w = nativeWord(ivec2((baseX + int(u >> 2)) & 1023, row));
        index = (w >> ((u & 3u) * 4u)) & 15u;
    } else {
        uint w = nativeWord(ivec2((baseX + int(u >> 1)) & 1023, row));
        index = (w >> ((u & 1u) * 8u)) & 255u;
    }
    return nativeWord(ivec2((clutX + int(index)) & 1023, clutY));
}

void main() {
    uint s0 = vState.x, s1 = vState.y, s2 = vState.z;
    ivec2 fc = ivec2(gl_FragCoord.xy);
    ivec2 p = fc / params.scale;  // native pixel
    // Texel coordinates of the native pixel's top-left subpixel, so every subpixel of a native pixel picks the
    // same texel and sprite and tile edges stay on native pixel boundaries. Evaluated before any discard.
    vec2 uvAnchor = vUv;
    if (params.scale > 1) {
        ivec2 sub = fc - p * params.scale;
        uvAnchor = vUv - vec2(sub.x) * dFdxFine(vUv) - vec2(sub.y) * dFdyFine(vUv);
    }
    bool textured = (s0 & 1u) != 0u, raw = (s0 & 2u) != 0u, semi = (s0 & 4u) != 0u;
    bool dither = (s0 & 8u) != 0u, check = (s0 & 16u) != 0u, setMask = (s0 & 32u) != 0u;
    uint mode = (s0 >> 6) & 3u;

    uint dest = 0u;
    if (check || semi) {
        dest = vramWord(fc);
        if (check && (dest & 0x8000u) != 0u) discard;
    }

    ivec3 c = clamp(ivec3(floor(vColor + 0.5)), 0, 255);
    bool blend = semi;
    uint bit15 = 0u;
    if (textured) {
        ivec2 uv = ivec2(floor(uvAnchor + 0.5)) & 255;
        uint t = fetchTexel(s0, s1, s2, uv);
        if (t == 0u) discard;
        ivec3 tc = ivec3(int(t & 31u), int((t >> 5) & 31u), int((t >> 10) & 31u)) << 3;
        c = raw ? tc : min((tc * c) >> 7, ivec3(255));
        bit15 = t & 0x8000u;
        blend = semi && bit15 != 0u;
    }
    if (dither) c = clamp(c + kDither[(p.y & 3) * 4 + (p.x & 3)], 0, 255);
    ivec3 q = c >> 3;
    if (blend) {
        ivec3 b = ivec3(int(dest & 31u), int((dest >> 5) & 31u), int((dest >> 10) & 31u));
        if (mode == 0u) q = (b + q) >> 1;
        else if (mode == 1u) q = min(b + q, ivec3(31));
        else if (mode == 2u) q = max(b - q, ivec3(0));
        else q = min(b + (q >> 2), ivec3(31));
    }
    if (setMask) bit15 = 0x8000u;
    uvec3 e = uvec3(expand5(uint(q.r)), expand5(uint(q.g)), expand5(uint(q.b)));
    outColor = vec4(vec3(e) / 255.0, bit15 != 0u ? 1.0 : 0.0);
}
