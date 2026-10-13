#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// Scales the input into the destination rectangle, nearest or bilinear. a.x: 1 for bilinear.

layout(set = 0, binding = 0) uniform texture2D uTex;
#include "common.glsl"
#include "hdr.glsl"

void main() {
    vec2 pos = vFrac * params.srcSize;
    vec3 rgb;
    if (params.a.x > 0.5) {
        rgb = sampleLinear(uTex, vFrac).rgb;
    } else {
        rgb = fetchClamped(uTex, ivec2(pos)).rgb;
    }
    outColor = vec4(hdrOutput(rgb), 1.0);
}
