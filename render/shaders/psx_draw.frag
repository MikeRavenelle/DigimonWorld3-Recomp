#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Pixel stage of the primitive pipeline. Reproduces the PS1 pixel path on integers: texture fetch through the
// CLUT from the VRAM copy, colour modulation, dithering, 5-bit semi-transparent blending against the previous
// VRAM contents and mask-bit handling. VRAM is stored as RGBA8: the 5-bit channels expanded to 8 bits and the
// mask bit in alpha, so every 16-bit word can be rebuilt exactly.
//
// Colour and texel coordinates come from fixed-point planes evaluated at the native pixel with the arithmetic
// of the software rasterizer, so texel picks and colours are the same as the reference.
//
// With an internal resolution above 1 the target is scale times larger; blending and mask handling work per
// subpixel on the scaled destination, dithering uses the native pixel position, and all subpixels of a native
// pixel pick the same texel.
//
// Display layer (compiled with DISPLAY_LAYER). When enabled, a second target receives the colour that is shown,
// while the first stays the exact PS1 state that the game can read back and sample. The second target holds 16 bits
// per channel; its alpha holds the 16-bit VRAM word the pixel produced, and the presenter only trusts it while that
// word is still the one in VRAM. The layer carries the options that must not leak into game-visible state:
//   true colour: no dithering and no 5-bit quantisation of the shaded colour, blends in full precision
//   bilinear:    texels of triangles that do not map 1:1 to pixels are blended, transparent texels excluded
//   xBR:         edge-directed smoothing of 1:1 sprites and tiles, from the neighbouring texels
// Filtered taps stay inside the texel bounds of the primitive, so neighbouring sprites of an atlas never bleed in.
//
// Precise primitives (state0 bit 11, GTE sub-pixel geometry) take colour and texel coordinates from the interpolated
// float varyings instead of the planes: every subpixel samples its own position.
//
// Replacement textures (display layer only). A sprite that shows a known background image carries bit 12 of state0:
// the texel colour of the display layer then comes from the replacement atlas (binding 2) instead of the CLUT
// lookup, at (texel position * replacement scale + offset), where the scale is in bits 13-15 of state0 (minus one)
// and the offset in the vertex's hd offset word (two signed 16-bit values). The colour keeps full precision (no
// 5-bit quantisation, no dithering) and is modulated by the vertex colour like any texel. The exact PS1 state in the
// first target is untouched: it still holds what the game drew, so read-backs and later draws see the original.
//
// Direct sprites (display layer variant only). A sprite with bit 16 of state0 does not read VRAM at all: its texels come
// from the replacement atlas, at (texel position * replacement scale + offset) like a replacement texture, and an atlas
// pixel with no alpha is transparent. The exact target receives the colour of the atlas pixel at the middle of the
// texel, reduced to 5 bits, so what the game can read back is a picture of the same thing. The renderer draws
// background pieces this way when the game has not (see texrep/field_backdrop.h). Without a replacement scale above
// 1 the texels are shown as they are, like the game's own sprites.
//
// state0: bit0 textured, 1 raw texture, 2 semi-transparent, 3 dither, 4 check mask, 5 set mask,
//         6-7 blend mode, 8-9 texture depth (0: 4 bit, 1: 8 bit, 2: 15 bit), 10 texels map 1:1 to pixels, 11 precise (float attributes),
//         12 replacement texture, 13-15 replacement scale - 1, 16 direct sprite
// state1: bits 0-15 CLUT attribute, 16-19 texture page x / 64, 20 texture page y / 256
// state2: texture window: bits 0-4 mask x, 5-9 mask y, 10-14 offset x, 15-19 offset y (8-pixel units)

layout(set = 0, binding = 0) uniform texture2D uVram;  // copy of VRAM as of the last synchronisation
layout(set = 0, binding = 1) uniform texture2D uHi;    // copy of the display layer
#ifdef DISPLAY_LAYER
layout(set = 0, binding = 2) uniform texture2D uReplace;  // replacement texture atlas, RGBA8
#endif

layout(push_constant) uniform Params {
    int scale;    // internal resolution multiplier (1 = native)
    uint mode;    // bit1 true colour, bits 2-3 texture filter (0 nearest, 1 bilinear, 2 xBR); display layer variant only
    int width;    // render target width in native pixels (used by the vertex stage)
    int pad;
} params;

layout(location = 0) flat in ivec4 vPlanes0;
layout(location = 1) flat in ivec4 vPlanes1;
layout(location = 2) flat in ivec4 vPlanes2;
layout(location = 3) flat in ivec3 vPlanes3;
layout(location = 4) flat in uvec3 vState;
layout(location = 5) flat in uint vUvRect;
layout(location = 6) noperspective in vec3 vColF;  // precise primitives: colour, floor() selects the 8-bit value
layout(location = 7) in vec2 vUvF;                 // precise primitives: texel coordinates, floor() selects the texel
layout(location = 8) flat in uint vHdOffset;       // replacement texture offset

layout(location = 0) out vec4 outColor;
#ifdef DISPLAY_LAYER
layout(location = 1) out vec4 outHi;
#endif

const int kDither[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

int gPlane[15];
uint s0, s1, s2;
bool gTrueColor;

// Word at a position of the scaled VRAM (used for the destination, one value per subpixel).
uint vramWord(ivec2 p) {
    uvec4 b = uvec4(texelFetch(uVram, p, 0) * 255.0 + 0.5);
    return (b.r >> 3) | ((b.g >> 3) << 5) | ((b.b >> 3) << 10) | ((b.a >> 7) << 15);
}

// Word at a native VRAM position: texture and CLUT data is read at the top-left subpixel, so indices and
// colours are exactly what the game stored and are never filtered.
uint nativeWord(ivec2 p) { return vramWord(p * params.scale); }

uint expand5(uint v) { return (v << 3) | (v >> 2); }

// Plane `i` (r 0, g 3, b 6, u 9, v 12) at a native pixel, 20.12 fixed point.
int planeAt(int i, ivec2 p) {
    return int(uint(gPlane[i]) + uint(gPlane[i + 1]) * uint(p.x) + uint(gPlane[i + 2]) * uint(p.y));
}

// The same plane at a fractional offset from the native pixel, as a float in 1/4096 units.
float planeF(int i, ivec2 p, vec2 off) {
    return float(planeAt(i, p)) + float(gPlane[i + 1]) * off.x + float(gPlane[i + 2]) * off.y;
}

uint fetchTexel(ivec2 uv) {
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

// Texel at integer coordinates, kept inside the texel bounds of the primitive.
uint tapWord(ivec2 t) {
    t.x = clamp(t.x, int(vUvRect & 255u), int((vUvRect >> 8) & 255u));
    t.y = clamp(t.y, int((vUvRect >> 16) & 255u), int(vUvRect >> 24));
    return fetchTexel(t);
}

// Texel colour on a 0..255 scale (5-bit channels scaled up; expanded to the full range in true colour).
vec3 texColor(uint t) {
    vec3 v = vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u));
    return v * (gTrueColor ? 255.0 / 31.0 : 8.0);
}

// Bilinear blend of the four texels around texel-space position t (texel centres at integers). Transparent
// texels do not contribute, so sprite edges take the colour of the opaque side instead of fading to black.
vec3 bilinearTexel(vec2 t, vec3 fallback) {
    vec2 b = floor(t);
    vec2 f = t - b;
    ivec2 i = ivec2(b);
    vec3 acc = vec3(0.0);
    float sum = 0.0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            float w = (dx == 0 ? 1.0 - f.x : f.x) * (dy == 0 ? 1.0 - f.y : f.y);
            uint tw = tapWord(i + ivec2(dx, dy));
            if (tw == 0u) continue;
            acc += w * texColor(tw);
            sum += w;
        }
    return sum > 1e-4 ? acc / sum : fallback;
}

const float kEdgeContrast = 24.0;

float texDistance(vec3 a, vec3 b) { return dot(abs(a - b), vec3(0.299, 0.587, 0.114)); }

// Neighbour for the xBR rules; transparent texels read as the centre colour so that silhouettes are not eaten.
vec3 xbrTap(ivec2 q, vec3 centre) {
    uint w = tapWord(q);
    return w == 0u ? centre : texColor(w);
}

// Edge-directed smoothing in the manner of the xBR filters: in the corner of a texel nearest to the sample, a
// diagonal edge between the two neighbours F and H on that side cuts the corner off and fills it with the
// closer of their colours. The edge test compares the weighted colour distances along and across the diagonal.
vec3 xbrTexel(vec2 t, vec3 fallback) {
    ivec2 e = ivec2(floor(t + 0.5));
    vec2 o = t - vec2(e);
    ivec2 sg = ivec2(o.x >= 0.0 ? 1 : -1, o.y >= 0.0 ? 1 : -1);
    uint we = tapWord(e);
    if (we == 0u) return fallback;
    vec3 E = texColor(we);
    vec3 F = xbrTap(e + ivec2(1, 0) * sg, E), H = xbrTap(e + ivec2(0, 1) * sg, E);
    // Shaded and dithered textures are full of small steps that are not edges: only strong contrast counts.
    if (texDistance(E, F) < kEdgeContrast || texDistance(E, H) < kEdgeContrast) return E;
    vec3 I = xbrTap(e + ivec2(1, 1) * sg, E), B = xbrTap(e + ivec2(0, -1) * sg, E), D = xbrTap(e + ivec2(-1, 0) * sg, E);
    vec3 C = xbrTap(e + ivec2(1, -1) * sg, E), G = xbrTap(e + ivec2(-1, 1) * sg, E);
    vec3 F4 = xbrTap(e + ivec2(2, 0) * sg, E), H5 = xbrTap(e + ivec2(0, 2) * sg, E);
    vec3 I4 = xbrTap(e + ivec2(2, 1) * sg, E), I5 = xbrTap(e + ivec2(1, 2) * sg, E);
    float along = texDistance(E, C) + texDistance(E, G) + texDistance(I, I5) + texDistance(I, I4) + 4.0 * texDistance(H, F);
    float across = texDistance(H, D) + texDistance(H, H5) + texDistance(F, F4) + texDistance(F, B) + 4.0 * texDistance(E, I);
    if (along >= across) return E;
    vec3 closer = texDistance(E, F) <= texDistance(E, H) ? F : H;
    // Fraction of the corner triangle (beyond the line through the edge midpoints) covered by this subpixel.
    float w = clamp((abs(o.x) + abs(o.y) - 0.5) * float(params.scale) + 0.5, 0.0, 1.0);
    return mix(E, closer, w);
}

#ifdef DISPLAY_LAYER
// The VRAM word that a texel of a direct sprite stands for: the atlas pixel at the middle of the texel, in 5 bits per
// channel; zero when the pixel is transparent. Opaque black is 0x8000, as in the game's own palettes.
uint directTexel(ivec2 uv) {
    const int rs = int((s0 >> 13) & 7u) + 1;
    const ivec2 origin = ivec2(int(vHdOffset << 16) >> 16, int(vHdOffset) >> 16);
    vec4 c = texelFetch(uReplace, origin + uv * rs + rs / 2, 0);
    if (c.a < 0.5) return 0u;
    uvec3 b = uvec3(c.rgb * 255.0 + 0.5) >> 3;
    uint w = b.r | (b.g << 5) | (b.b << 10);
    return w == 0u ? 0x8000u : w;
}

// Replacement colour (0..255) at continuous atlas position `a` (texel centres at half integers), bilinear, with every
// tap kept inside [lo, hi]. When the internal resolution equals the replacement scale the samples fall on texel
// centres and nothing is blended.
vec3 replaceTexel(vec2 a, ivec2 lo, ivec2 hi) {
    vec2 q = a - 0.5;
    vec2 b = floor(q);
    vec2 f = q - b;
    ivec2 i = ivec2(b);
    vec3 c00 = texelFetch(uReplace, clamp(i, lo, hi), 0).rgb;
    vec3 c10 = texelFetch(uReplace, clamp(i + ivec2(1, 0), lo, hi), 0).rgb;
    vec3 c01 = texelFetch(uReplace, clamp(i + ivec2(0, 1), lo, hi), 0).rgb;
    vec3 c11 = texelFetch(uReplace, clamp(i + ivec2(1, 1), lo, hi), 0).rgb;
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y) * 255.0;
}
#endif

void main() {
    gPlane = int[15](vPlanes0.x, vPlanes0.y, vPlanes0.z, vPlanes0.w, vPlanes1.x, vPlanes1.y, vPlanes1.z, vPlanes1.w,
                     vPlanes2.x, vPlanes2.y, vPlanes2.z, vPlanes2.w, vPlanes3.x, vPlanes3.y, vPlanes3.z);
    s0 = vState.x;
    s1 = vState.y;
    s2 = vState.z;
#ifdef DISPLAY_LAYER
    const bool layer = true;
#else
    const bool layer = false;
#endif
    gTrueColor = (params.mode & 2u) != 0u;
    const uint filterMode = (params.mode >> 2) & 3u;

    // Precise primitives. Derivatives are taken before any discard. The display layer samples at the centre of the
    // subpixel in pixel-centred coordinates, a fixed offset from the sample the varyings give.
    const bool isPrecise = (s0 & 2048u) != 0u;
    vec3 colL = vColF;
    vec2 uvL = vUvF;
    if (isPrecise && layer) {
        float shift = 0.5 - 0.5 * float(params.scale);
        colL += (dFdx(vColF) + dFdy(vColF)) * shift;
        uvL += (dFdx(vUvF) + dFdy(vUvF)) * shift;
    }

    ivec2 fc = ivec2(gl_FragCoord.xy);
    ivec2 p = fc / params.scale;  // native pixel
    ivec2 sub = fc - p * params.scale;
    bool textured = (s0 & 1u) != 0u, raw = (s0 & 2u) != 0u, semi = (s0 & 4u) != 0u;
    bool dither = (s0 & 8u) != 0u, check = (s0 & 16u) != 0u, setMask = (s0 & 32u) != 0u;
    uint mode = (s0 >> 6) & 3u;

    uint dest = 0u;
    if (check || semi) {
        dest = vramWord(fc);
        if (check && (dest & 0x8000u) != 0u) discard;
    }

    // Shading colour. Without the display layer the subpixels of a native pixel follow the plane smoothly.
    ivec3 c;
    if (isPrecise) {
        c = clamp(ivec3(floor(vColF)), 0, 255);
    } else if (params.scale > 1 && !layer) {
        vec2 off = vec2(sub) / float(params.scale);
        vec3 f = vec3(planeF(0, p, off), planeF(3, p, off), planeF(6, p, off));
        c = clamp(ivec3(floor(f / 4096.0)), 0, 255);
    } else {
        c = clamp(ivec3(planeAt(0, p) >> 12, planeAt(3, p) >> 12, planeAt(6, p) >> 12), 0, 255);
    }
    bool blend = semi;
    uint bit15 = 0u;
    uint t = 0u;
    if (textured) {
        ivec2 uv = isPrecise ? ivec2(floor(vUvF)) : ivec2(planeAt(9, p) >> 12, planeAt(12, p) >> 12);
        // A native pixel only partly covered at a higher resolution lies outside the polygon, where the plane runs past
        // the texels its vertices name; those texels belong to a neighbouring image.
        uv = clamp(uv, ivec2(int(vUvRect & 255u), int((vUvRect >> 16) & 255u)), ivec2(int((vUvRect >> 8) & 255u), int(vUvRect >> 24))) & 255;
#ifdef DISPLAY_LAYER
        t = (s0 & 65536u) != 0u ? directTexel(uv) : fetchTexel(uv);
#else
        t = fetchTexel(uv);
#endif
        if (t == 0u) discard;
        ivec3 tc = ivec3(int(t & 31u), int((t >> 5) & 31u), int((t >> 10) & 31u)) << 3;
        c = raw ? tc : min((tc * c) >> 7, ivec3(255));
        bit15 = t & 0x8000u;
        blend = semi && bit15 != 0u;
    }
    if (dither) c = clamp(c + kDither[(p.y & 3) * 4 + (p.x & 3)], 0, 255);
    ivec3 q = c >> 3;
    ivec3 b = ivec3(int(dest & 31u), int((dest >> 5) & 31u), int((dest >> 10) & 31u));
    if (blend) {
        if (mode == 0u) q = (b + q) >> 1;
        else if (mode == 1u) q = min(b + q, ivec3(31));
        else if (mode == 2u) q = max(b - q, ivec3(0));
        else q = min(b + (q >> 2), ivec3(31));
    }
    if (setMask) bit15 = 0x8000u;
    uvec3 e = uvec3(expand5(uint(q.r)), expand5(uint(q.g)), expand5(uint(q.b)));
    outColor = vec4(vec3(e) / 255.0, bit15 != 0u ? 1.0 : 0.0);
#ifdef DISPLAY_LAYER
    // Display layer: shaded colour at the centre of this subpixel.
    vec2 off = (vec2(sub) + 0.5) / float(params.scale) - 0.5;
    vec3 sh = isPrecise ? colL : vec3(planeF(0, p, off), planeF(3, p, off), planeF(6, p, off)) / 4096.0;
    vec3 cf = gTrueColor ? clamp(sh - 0.5, 0.0, 255.0) : vec3(clamp(ivec3(floor(sh)), 0, 255));
    vec3 hc = cf;
    bool replaced = false;
    if (textured) {
        vec3 tcf = texColor(t);
        bool unit = (s0 & 1024u) != 0u;
        vec2 tuv = (isPrecise ? uvL : vec2(planeF(9, p, off), planeF(12, p, off)) / 4096.0) - 0.5;
        if (filterMode == 1u && !unit) tcf = bilinearTexel(tuv, tcf);
        else if (filterMode == 2u && params.scale > 1) tcf = unit ? xbrTexel(tuv, tcf) : bilinearTexel(tuv, tcf);
        if ((s0 & 4096u) != 0u) {
            const int rs = int((s0 >> 13) & 7u) + 1;
            const ivec2 origin = ivec2(int(vHdOffset << 16) >> 16, int(vHdOffset) >> 16);
            const ivec2 lo = ivec2(int(vUvRect & 255u), int((vUvRect >> 16) & 255u)) * rs + origin;
            const ivec2 hi = ivec2(int((vUvRect >> 8) & 255u) + 1, int(vUvRect >> 24) + 1) * rs - 1 + origin;
            vec2 cc = vec2(planeF(9, p, off), planeF(12, p, off)) / 4096.0;
            if ((s0 & 65536u) != 0u && rs == 1) tcf = texelFetch(uReplace, clamp(ivec2(floor(cc)) + origin, lo, hi), 0).rgb * 255.0;
            else tcf = replaceTexel(cc * float(rs) + vec2(origin), lo, hi);
            replaced = true;
        }
        // The small offset keeps a blend of equal texels (division by the weight sum) from rounding down a step.
        if (replaced) hc = raw ? tcf : min(tcf * cf / 128.0, 255.0);
        else if (raw) hc = gTrueColor ? tcf : floor(tcf + 0.002);
        else hc = gTrueColor ? min(tcf * cf / 128.0, 255.0) : min(floor(tcf * cf / 128.0 + 0.002), 255.0);
    }
    vec3 hiDest = vec3(0.0);
    if (blend) {
        vec4 h = texelFetch(uHi, fc, 0);
        bool valid = uint(h.a * 65535.0 + 0.5) == dest;
        hiDest = valid ? h.rgb * 255.0 : vec3(expand5(uint(b.r)), expand5(uint(b.g)), expand5(uint(b.b)));
    }
    vec3 shown;
    if (replaced) {
        shown = clamp(hc, 0.0, 255.0);
    } else if (gTrueColor) {
        shown = clamp(hc, 0.0, 255.0);
        if (blend) {
            if (mode == 0u) shown = (hiDest + shown) * 0.5;
            else if (mode == 1u) shown = min(hiDest + shown, 255.0);
            else if (mode == 2u) shown = max(hiDest - shown, 0.0);
            else shown = min(hiDest + shown * 0.25, 255.0);
        }
    } else {
        ivec3 hq = clamp(ivec3(floor(hc)), 0, 255);
        if (dither) hq = clamp(hq + kDither[(p.y & 3) * 4 + (p.x & 3)], 0, 255);
        hq >>= 3;
        if (blend) {
            ivec3 hb = ivec3(floor(hiDest + 0.5)) >> 3;
            if (mode == 0u) hq = (hb + hq) >> 1;
            else if (mode == 1u) hq = min(hb + hq, ivec3(31));
            else if (mode == 2u) hq = max(hb - hq, ivec3(0));
            else hq = min(hb + (hq >> 2), ivec3(31));
        }
        shown = vec3(expand5(uint(hq.r)), expand5(uint(hq.g)), expand5(uint(hq.b)));
    }
    uint word = uint(q.r) | (uint(q.g) << 5) | (uint(q.b) << 10) | bit15;
    outHi = vec4(shown / 255.0, float(word) / 65535.0);
#endif
}
