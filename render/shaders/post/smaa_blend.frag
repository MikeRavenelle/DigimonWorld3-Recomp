#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// SMAA pass 3: neighbourhood blending. Mixes each pixel with the neighbour the weights point at, using one bilinear
// read positioned so that the blend comes out right. See smaa_common.glsl for the origin and licence of the
// algorithm.

layout(set = 0, binding = 0) uniform texture2D uColor;
layout(set = 0, binding = 1) uniform texture2D uWeights;
#include "smaa_common.glsl"

void main() {
    vec2 texcoord = smaaTexcoord();
    vec4 offset = SMAA_RT_METRICS.xyxy * vec4(1.0, 0.0, 0.0, 1.0) + texcoord.xyxy;

    // Blend weights of this pixel (left and top) and of its right and bottom neighbours.
    vec4 a;
    a.x = sampleLinear(uWeights, offset.xy).a;
    a.y = sampleLinear(uWeights, offset.zw).g;
    a.wz = sampleLinear(uWeights, texcoord).xz;

    if (dot(a, vec4(1.0)) < 1e-5) {
        outColor = vec4(sampleLinear(uColor, texcoord).rgb, 1.0);
        return;
    }
    bool h = max(a.x, a.z) > max(a.y, a.w);  // blend horizontally or vertically

    vec4 blendingOffset = h ? vec4(a.x, 0.0, a.z, 0.0) : vec4(0.0, a.y, 0.0, a.w);
    vec2 blendingWeight = h ? a.xz : a.yw;
    blendingWeight /= dot(blendingWeight, vec2(1.0));

    vec4 blendingCoord = blendingOffset * vec4(SMAA_RT_METRICS.xy, -SMAA_RT_METRICS.xy) + texcoord.xyxy;
    vec3 color = blendingWeight.x * sampleLinear(uColor, blendingCoord.xy).rgb;
    color += blendingWeight.y * sampleLinear(uColor, blendingCoord.zw).rgb;
    outColor = vec4(color, 1.0);
}
