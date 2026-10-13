#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// SMAA pass 1: colour edge detection. Writes the edges towards the left and top of each pixel into red and green.
// See smaa_common.glsl for the origin and licence of the algorithm.

layout(set = 0, binding = 0) uniform texture2D uColor;
#include "smaa_common.glsl"

void main() {
    ivec2 p = ivec2(floor(vFrac * params.srcSize));
    vec2 threshold = vec2(SMAA_THRESHOLD);

    vec3 C = fetchClamped(uColor, p).rgb;
    vec3 Cleft = fetchClamped(uColor, p + ivec2(-1, 0)).rgb;
    vec3 t = abs(C - Cleft);
    vec4 delta;
    delta.x = max(max(t.r, t.g), t.b);
    vec3 Ctop = fetchClamped(uColor, p + ivec2(0, -1)).rgb;
    t = abs(C - Ctop);
    delta.y = max(max(t.r, t.g), t.b);

    vec2 edges = step(threshold, delta.xy);
    if (dot(edges, vec2(1.0)) == 0.0) {
        outColor = vec4(0.0);
        return;
    }

    vec3 Cright = fetchClamped(uColor, p + ivec2(1, 0)).rgb;
    t = abs(C - Cright);
    delta.z = max(max(t.r, t.g), t.b);
    vec3 Cbottom = fetchClamped(uColor, p + ivec2(0, 1)).rgb;
    t = abs(C - Cbottom);
    delta.w = max(max(t.r, t.g), t.b);

    vec2 maxDelta = max(delta.xy, delta.zw);

    vec3 Cleftleft = fetchClamped(uColor, p + ivec2(-2, 0)).rgb;
    t = abs(C - Cleftleft);
    delta.z = max(max(t.r, t.g), t.b);
    vec3 Ctoptop = fetchClamped(uColor, p + ivec2(0, -2)).rgb;
    t = abs(C - Ctoptop);
    delta.w = max(max(t.r, t.g), t.b);

    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation: drop an edge that is much weaker than a neighbouring one.
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    outColor = vec4(edges, 0.0, 0.0);
}
