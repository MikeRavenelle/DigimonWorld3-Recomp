#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// FidelityFX Super Resolution 1.0, edge-adaptive spatial upsampling (EASU). Ported from AMD's ffx_fsr1.h (32-bit
// path) to plain GLSL: the constants are derived in main, the gather is emulated with texel fetches and the
// reciprocal and square-root approximations are replaced by exact operations.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//
// The target is exactly the output rectangle (dest.zw pixels); the whole input texture is the viewport.

layout(set = 0, binding = 0) uniform texture2D uTex;
#include "common.glsl"

// The 2x2 block of texels around p (textureGather order: lower-left, lower-right, upper-right, upper-left), as one
// vector per colour channel.
void gather(vec2 p, out vec4 r, out vec4 g, out vec4 b) {
    ivec2 i0 = ivec2(floor(p * params.srcSize - 0.5));
    vec3 t0 = fetchClamped(uTex, i0 + ivec2(0, 1)).rgb;
    vec3 t1 = fetchClamped(uTex, i0 + ivec2(1, 1)).rgb;
    vec3 t2 = fetchClamped(uTex, i0 + ivec2(1, 0)).rgb;
    vec3 t3 = fetchClamped(uTex, i0).rgb;
    r = vec4(t0.r, t1.r, t2.r, t3.r);
    g = vec4(t0.g, t1.g, t2.g, t3.g);
    b = vec4(t0.b, t1.b, t2.b, t3.b);
}

void tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * -dir.y + off.y * dir.x;
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void edgeSet(inout vec2 dir, inout float len, vec2 pp, bool biS, bool biT, bool biU, bool biV, float lA, float lB, float lC,
         float lD, float lE) {
    float w = 0.0;
    if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) w = pp.x * (1.0 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1e-20);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1e-20);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void main() {
    vec2 outSize = params.dest.zw;
    vec2 ip = floor(vFrac * outSize);
    vec2 inSize = params.srcSize;
    // Output pixel to input pixel position; the fractional part is the position inside the 2x2 cell.
    vec2 pp = ip * (inSize / outSize) + (0.5 * inSize / outSize - 0.5);
    vec2 fp = floor(pp);
    pp -= fp;
    vec2 rcp = 1.0 / inSize;
    vec2 p0 = fp * rcp + vec2(1.0, -1.0) * rcp;
    vec2 p1 = p0 + vec2(-1.0, 2.0) * rcp;
    vec2 p2 = p0 + vec2(1.0, 2.0) * rcp;
    vec2 p3 = p0 + vec2(0.0, 4.0) * rcp;
    vec4 bczzR, bczzG, bczzB, ijfeR, ijfeG, ijfeB, klhgR, klhgG, klhgB, zzonR, zzonG, zzonB;
    gather(p0, bczzR, bczzG, bczzB);
    gather(p1, ijfeR, ijfeG, ijfeB);
    gather(p2, klhgR, klhgG, klhgB);
    gather(p3, zzonR, zzonG, zzonB);

    vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x, cL = bczzL.y;
    float iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
    float oL = zzonL.z, nL = zzonL.w;

    // Direction and length of the local edge, from bilinearly weighted gradients of the four surrounding cells.
    vec2 dir = vec2(0.0);
    float len = 0.0;
    edgeSet(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    edgeSet(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    edgeSet(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    edgeSet(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = inversesqrt(max(dirR, 1e-20));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(max(abs(dir.x), abs(dir.y)), 1e-20);
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    // The four centre taps bound the result, which keeps the negative lobe from overshooting.
    vec3 f3 = vec3(ijfeR.z, ijfeG.z, ijfeB.z), g3 = vec3(klhgR.w, klhgG.w, klhgB.w);
    vec3 j3 = vec3(ijfeR.y, ijfeG.y, ijfeB.y), k3 = vec3(klhgR.x, klhgG.x, klhgB.x);
    vec3 min4 = min(min(f3, g3), min(j3, k3));
    vec3 max4 = max(max(f3, g3), max(j3, k3));

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));  // b
    tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));  // c
    tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));  // i
    tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.y, ijfeG.y, ijfeB.y));   // j
    tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.z, ijfeG.z, ijfeB.z));   // f
    tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));  // e
    tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.x, klhgG.x, klhgB.x));   // k
    tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));   // l
    tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));   // h
    tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.w, klhgG.w, klhgB.w));   // g
    tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));   // o
    tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));   // n
    vec3 pix = min(max4, max(min4, aC / aW));
    outColor = vec4(pix, 1.0);
}
