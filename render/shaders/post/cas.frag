#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// Contrast Adaptive Sharpening (sharpen only, no resize), after AMD FidelityFX CAS 1.1.
//
// Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
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
// a.x: sharpness 0..1 (the CAS sharpness parameter; flat areas are never changed).

layout(set = 0, binding = 0) uniform texture2D uTex;
#include "common.glsl"

void main() {
    ivec2 sp = ivec2(vFrac * params.srcSize);
    vec3 a = fetchClamped(uTex, sp + ivec2(-1, -1)).rgb;
    vec3 b = fetchClamped(uTex, sp + ivec2(0, -1)).rgb;
    vec3 c = fetchClamped(uTex, sp + ivec2(1, -1)).rgb;
    vec3 d = fetchClamped(uTex, sp + ivec2(-1, 0)).rgb;
    vec3 e = fetchClamped(uTex, sp).rgb;
    vec3 f = fetchClamped(uTex, sp + ivec2(1, 0)).rgb;
    vec3 g = fetchClamped(uTex, sp + ivec2(-1, 1)).rgb;
    vec3 h = fetchClamped(uTex, sp + ivec2(0, 1)).rgb;
    vec3 i = fetchClamped(uTex, sp + ivec2(1, 1)).rgb;

    // Local extremes of the plus-shaped neighbourhood, widened by the diagonals.
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mn2 = min(min(min(mn, a), min(c, g)), i);
    mn += mn2;
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 mx2 = max(max(max(mx, a), max(c, g)), i);
    mx += mx2;

    // Amount of headroom in both directions sets the sharpening weight per pixel.
    vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0);
    amp = sqrt(amp);
    float peak = -1.0 / mix(8.0, 5.0, clamp(params.a.x, 0.0, 1.0));
    float w = amp.g * peak;
    float rcpWeight = 1.0 / (1.0 + 4.0 * w);
    vec3 rgb = clamp((b * w + d * w + f * w + h * w + e) * rcpWeight, 0.0, 1.0);
    outColor = vec4(rgb, 1.0);
}
