#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// SMAA pass 2: blending weight calculation. Searches along every edge for its ends, recognises the pattern and reads
// the coverage from the precomputed area texture. Writes the weights for the four neighbours into red to alpha.
// See smaa_common.glsl for the origin and licence of the algorithm.

layout(set = 0, binding = 0) uniform texture2D uEdges;
layout(set = 0, binding = 1) uniform texture2D uArea;    // 160 x 560, red and green hold the area
layout(set = 0, binding = 2) uniform texture2D uSearch;  // 64 x 16, red holds the search offset
#include "smaa_common.glsl"

// Edge texture lookup at an arbitrary position, bilinear. The search relies on it to read two pixels at once.
vec4 edgeAt(vec2 coord) { return sampleLinear(uEdges, coord); }
vec4 edgeAtOffset(vec2 coord, ivec2 offset) { return edgeAt(coord + vec2(offset) * SMAA_RT_METRICS.xy); }

// Diagonal patterns ---------------------------------------------------------------------------------------------

vec2 decodeDiagBilinearAccess(vec2 e) {
    e.r = e.r * abs(5.0 * e.r - 5.0 * 0.75);
    return roundEven(e);
}

vec4 decodeDiagBilinearAccess(vec4 e) {
    e.rb = e.rb * abs(5.0 * e.rb - 5.0 * 0.75);
    return roundEven(e);
}

vec2 searchDiag1(vec2 texcoord, vec2 dir, out vec2 e) {
    vec4 coord = vec4(texcoord, -1.0, 1.0);
    vec3 t = vec3(SMAA_RT_METRICS.xy, 1.0);
    e = vec2(0.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9) {
        coord.xyz = t * vec3(dir, 1.0) + coord.xyz;
        e = edgeAt(coord.xy).rg;
        coord.w = dot(e, vec2(0.5));
    }
    return coord.zw;
}

vec2 searchDiag2(vec2 texcoord, vec2 dir, out vec2 e) {
    vec4 coord = vec4(texcoord, -1.0, 1.0);
    coord.x += 0.25 * SMAA_RT_METRICS.x;
    vec3 t = vec3(SMAA_RT_METRICS.xy, 1.0);
    e = vec2(0.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9) {
        coord.xyz = t * vec3(dir, 1.0) + coord.xyz;
        e = edgeAt(coord.xy).rg;
        e = decodeDiagBilinearAccess(e);
        coord.w = dot(e, vec2(0.5));
    }
    return coord.zw;
}

vec2 areaDiag(vec2 dist, vec2 e, float offset) {
    vec2 texcoord = float(SMAA_AREATEX_MAX_DISTANCE_DIAG) * e + dist;
    texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.x += 0.5;
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return sampleLinear(uArea, texcoord).rg;
}

vec2 calculateDiagWeights(vec2 texcoord, vec2 e, vec4 subsampleIndices) {
    vec2 weights = vec2(0.0);
    vec4 d;
    vec2 end;
    if (e.r > 0.0) {
        d.xz = searchDiag1(texcoord, vec2(-1.0, 1.0), end);
        d.x += float(end.y > 0.9);
    } else {
        d.xz = vec2(0.0);
    }
    d.yw = searchDiag1(texcoord, vec2(1.0, -1.0), end);

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * SMAA_RT_METRICS.xyxy + texcoord.xyxy;
        vec4 c;
        c.xy = edgeAtOffset(coords.xy, ivec2(-1, 0)).rg;
        c.zw = edgeAtOffset(coords.zw, ivec2(1, 0)).rg;
        c.yxwz = decodeDiagBilinearAccess(c.xyzw);
        vec2 cc = vec2(2.0) * c.xz + c.yw;
        cc = mix(cc, vec2(0.0), bvec2(step(0.9, d.z) > 0.5, step(0.9, d.w) > 0.5));
        weights += areaDiag(d.xy, cc, subsampleIndices.z);
    }

    d.xz = searchDiag2(texcoord, vec2(-1.0, -1.0), end);
    if (edgeAtOffset(texcoord, ivec2(1, 0)).r > 0.0) {
        d.yw = searchDiag2(texcoord, vec2(1.0, 1.0), end);
        d.y += float(end.y > 0.9);
    } else {
        d.yw = vec2(0.0);
    }

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x, -d.x, d.y, d.y) * SMAA_RT_METRICS.xyxy + texcoord.xyxy;
        vec4 c;
        c.x = edgeAtOffset(coords.xy, ivec2(-1, 0)).g;
        c.y = edgeAtOffset(coords.xy, ivec2(0, -1)).r;
        c.zw = edgeAtOffset(coords.zw, ivec2(1, 0)).gr;
        vec2 cc = vec2(2.0) * c.xz + c.yw;
        cc = mix(cc, vec2(0.0), bvec2(step(0.9, d.z) > 0.5, step(0.9, d.w) > 0.5));
        weights += areaDiag(d.xy, cc, subsampleIndices.w).gr;
    }
    return weights;
}

// Orthogonal patterns -------------------------------------------------------------------------------------------

// Where the edge ends, from the packed search texture; `e` is the last pair of edge values read.
float searchLength(vec2 e, float offset) {
    vec2 scale = SMAA_SEARCHTEX_SIZE * vec2(0.5, -1.0);
    vec2 bias = SMAA_SEARCHTEX_SIZE * vec2(offset, 1.0);
    scale += vec2(-1.0, 1.0);
    bias += vec2(0.5, -0.5);
    scale *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0 / SMAA_SEARCHTEX_PACKED_SIZE;
    return sampleLinear(uSearch, scale * e + bias).r;
}

float searchXLeft(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0) {
        e = edgeAt(texcoord).rg;
        texcoord = -vec2(2.0, 0.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * searchLength(e, 0.0) + 3.25;
    return SMAA_RT_METRICS.x * offset + texcoord.x;
}

float searchXRight(vec2 texcoord, float end) {
    vec2 e = vec2(0.0, 1.0);
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0) {
        e = edgeAt(texcoord).rg;
        texcoord = vec2(2.0, 0.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * searchLength(e, 0.5) + 3.25;
    return -SMAA_RT_METRICS.x * offset + texcoord.x;
}

float searchYUp(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0) {
        e = edgeAt(texcoord).rg;
        texcoord = -vec2(0.0, 2.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * searchLength(e.gr, 0.0) + 3.25;
    return SMAA_RT_METRICS.y * offset + texcoord.y;
}

float searchYDown(vec2 texcoord, float end) {
    vec2 e = vec2(1.0, 0.0);
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0) {
        e = edgeAt(texcoord).rg;
        texcoord = vec2(0.0, 2.0) * SMAA_RT_METRICS.xy + texcoord;
    }
    float offset = -(255.0 / 127.0) * searchLength(e.gr, 0.5) + 3.25;
    return -SMAA_RT_METRICS.y * offset + texcoord.y;
}

// Coverage of the pixel for the pattern with end distances `dist` and crossing edges e1, e2.
vec2 areaLookup(vec2 dist, float e1, float e2, float offset) {
    vec2 texcoord = float(SMAA_AREATEX_MAX_DISTANCE) * roundEven(4.0 * vec2(e1, e2)) + dist;
    texcoord = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5 * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.y = SMAA_AREATEX_SUBTEX_SIZE * offset + texcoord.y;
    return sampleLinear(uArea, texcoord).rg;
}

void detectHorizontalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0);
    factor.x -= rounding.x * edgeAtOffset(texcoord.xy, ivec2(0, 1)).r;
    factor.x -= rounding.y * edgeAtOffset(texcoord.zw, ivec2(1, 1)).r;
    factor.y -= rounding.x * edgeAtOffset(texcoord.xy, ivec2(0, -2)).r;
    factor.y -= rounding.y * edgeAtOffset(texcoord.zw, ivec2(1, -2)).r;
    weights *= clamp(factor, 0.0, 1.0);
}

void detectVerticalCornerPattern(inout vec2 weights, vec4 texcoord, vec2 d) {
    vec2 leftRight = step(d.xy, d.yx);
    vec2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    vec2 factor = vec2(1.0);
    factor.x -= rounding.x * edgeAtOffset(texcoord.xy, ivec2(1, 0)).g;
    factor.x -= rounding.y * edgeAtOffset(texcoord.zw, ivec2(1, 1)).g;
    factor.y -= rounding.x * edgeAtOffset(texcoord.xy, ivec2(-2, 0)).g;
    factor.y -= rounding.y * edgeAtOffset(texcoord.zw, ivec2(-2, 1)).g;
    weights *= clamp(factor, 0.0, 1.0);
}

void main() {
    vec2 texcoord = smaaTexcoord();
    vec2 pixcoord = texcoord * SMAA_RT_METRICS.zw;
    vec4 offset0 = SMAA_RT_METRICS.xyxy * vec4(-0.25, -0.125, 1.25, -0.125) + texcoord.xyxy;
    vec4 offset1 = SMAA_RT_METRICS.xyxy * vec4(-0.125, -0.25, -0.125, 1.25) + texcoord.xyxy;
    vec4 offset2 = SMAA_RT_METRICS.xxyy * vec4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS) +
                   vec4(offset0.xz, offset1.yw);
    const vec4 subsampleIndices = vec4(0.0);  // no temporal or multisample variation in 1x

    vec4 weights = vec4(0.0);
    vec2 e = edgeAt(texcoord).rg;

    if (e.g > 0.0) {  // edge at the top
        weights.rg = calculateDiagWeights(texcoord, e, subsampleIndices);
        if (weights.r == -weights.g) {  // no diagonal pattern found
            vec2 d;
            vec3 coords;
            coords.x = searchXLeft(offset0.xy, offset2.x);
            coords.y = offset1.y;
            d.x = coords.x;
            float e1 = edgeAt(coords.xy).r;
            coords.z = searchXRight(offset0.zw, offset2.y);
            d.y = coords.z;
            d = abs(roundEven(SMAA_RT_METRICS.zz * d - pixcoord.xx));
            vec2 sqrt_d = sqrt(d);
            float e2 = edgeAtOffset(coords.zy, ivec2(1, 0)).r;
            weights.rg = areaLookup(sqrt_d, e1, e2, subsampleIndices.y);
            coords.y = texcoord.y;
            detectHorizontalCornerPattern(weights.rg, coords.xyzy, d);
        } else {
            e.r = 0.0;  // skip the vertical search
        }
    }

    if (e.r > 0.0) {  // edge at the left
        vec2 d;
        vec3 coords;
        coords.y = searchYUp(offset1.xy, offset2.z);
        coords.x = offset0.x;
        d.x = coords.y;
        float e1 = edgeAt(coords.xy).g;
        coords.z = searchYDown(offset1.zw, offset2.w);
        d.y = coords.z;
        d = abs(roundEven(SMAA_RT_METRICS.ww * d - pixcoord.yy));
        vec2 sqrt_d = sqrt(d);
        float e2 = edgeAtOffset(coords.xz, ivec2(0, 1)).g;
        weights.ba = areaLookup(sqrt_d, e1, e2, subsampleIndices.x);
        coords.x = texcoord.x;
        detectVerticalCornerPattern(weights.ba, coords.xyxz, d);
    }
    outColor = weights;
}
