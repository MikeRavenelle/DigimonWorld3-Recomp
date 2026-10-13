#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// Colour grading: contrast about mid grey, a per-channel tint, saturation, vibrance (saturation that favours dull
// colours), gamma and a brightness offset added last. With the neutral values (1, 1, 0, 1, tint 1, offset 0) the image
// comes out unchanged.
// a: saturation, contrast, vibrance, gamma; b.xyz: tint; b.w: brightness.

layout(set = 0, binding = 0) uniform texture2D uTex;
#include "common.glsl"

void main() {
    vec3 c = fetchClamped(uTex, ivec2(vFrac * params.srcSize)).rgb;
    c = (c - 0.5) * params.a.y + 0.5;
    c *= params.b.xyz;
    float l = luma(c);
    float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    float boost = params.a.x + params.a.z * (1.0 - chroma);
    c = mix(vec3(l), c, boost);
    c = clamp(pow(clamp(c, 0.0, 1.0), vec3(1.0 / params.a.w)) + params.b.w, 0.0, 1.0);
    outColor = vec4(c, 1.0);
}
