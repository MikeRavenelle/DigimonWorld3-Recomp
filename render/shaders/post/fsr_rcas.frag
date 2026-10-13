#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// FidelityFX Super Resolution 1.0, robust contrast adaptive sharpening (RCAS). Ported from AMD's ffx_fsr1.h (32-bit
// path, no denoise) to plain GLSL with exact reciprocals.
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
// a.x: sharpness in stops (0 is the sharpest, 2 is the weakest); the input has the size of the destination.

layout(set = 0, binding = 0) uniform texture2D uTex;
#include "common.glsl"

void main() {
    ivec2 sp = ivec2(vFrac * params.srcSize);
    vec3 b = fetchClamped(uTex, sp + ivec2(0, -1)).rgb;
    vec3 d = fetchClamped(uTex, sp + ivec2(-1, 0)).rgb;
    vec3 e = fetchClamped(uTex, sp).rgb;
    vec3 f = fetchClamped(uTex, sp + ivec2(1, 0)).rgb;
    vec3 h = fetchClamped(uTex, sp + ivec2(0, 1)).rgb;

    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    const vec2 peakC = vec2(1.0, -4.0);
    vec3 hitMin = min(mn4, e) / (4.0 * max(mx4, vec3(1e-5)));
    vec3 hitMax = (peakC.x - max(mx4, e)) / min(4.0 * mn4 + peakC.y, vec3(-1e-5));
    vec3 lobeRgb = max(-hitMin, hitMax);
    const float limit = 0.25 - 1.0 / 16.0;
    float lobe = max(-limit, min(max(lobeRgb.r, max(lobeRgb.g, lobeRgb.b)), 0.0)) * exp2(-params.a.x);
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    outColor = vec4((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL, 1.0);
}
