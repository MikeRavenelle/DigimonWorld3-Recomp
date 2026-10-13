// Declarations shared by the post-processing passes (see docs/ARCHITECTURE.md, "Post-processing").
//
// Every pass draws a rectangle of its target (`dest`, in target pixels) from one or more input textures. `vFrac` runs
// 0..1 over that rectangle with (0, 0) at the top-left on every graphics API.

layout(push_constant) uniform Params {
    vec4 dest;     // x, y, w, h of the rectangle in target pixels
    vec2 window;   // size of the target in pixels
    vec2 srcSize;  // size of the main input texture in pixels
    vec4 a;        // pass parameters
    vec4 b;
    vec4 c;        // HDR output (hdr.glsl); zero for every pass but the last
} params;

layout(location = 0) in vec2 vFrac;
layout(location = 0) out vec4 outColor;

ivec2 texSize(texture2D t) { return textureSize(t, 0); }

vec4 fetchClamped(texture2D t, ivec2 p) {
    return texelFetch(t, clamp(p, ivec2(0), texSize(t) - 1), 0);
}

// Bilinear sample at a normalised coordinate, clamped to the edge. Texel centres are at (i + 0.5) / size.
vec4 sampleLinear(texture2D t, vec2 uv) {
    vec2 p = uv * vec2(texSize(t)) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    return mix(mix(fetchClamped(t, i), fetchClamped(t, i + ivec2(1, 0)), f.x),
               mix(fetchClamped(t, i + ivec2(0, 1)), fetchClamped(t, i + ivec2(1, 1)), f.x), f.y);
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
