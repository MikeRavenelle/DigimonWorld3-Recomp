#version 450
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require
// CRT look: scanlines whose beams widen with brightness, an aperture-grille mask, optional barrel curvature with
// rounded corners and a vignette. Written for this project in the spirit of the usual scanline-and-mask shaders; the
// beam model is a gaussian per scanline, summed over the two nearest lines and filtered in linear light.
//
// Runs last, over the picture rectangle of the window. The input is the picture at any resolution: the scanlines
// are tied to the console's own lines (b.y), not to the input's rows.
// a: scanline strength, mask strength, curvature, vignette (all 0..1)
// b.y: number of console lines in the picture

layout(set = 0, binding = 0) uniform texture2D uTex;
#include "common.glsl"
#include "hdr.glsl"

const float kGamma = 2.2;

vec3 toLinear(vec3 c) { return pow(c, vec3(kGamma)); }

// Beam width grows with the brightness of the line, like the spot of a real tube.
float beamSigma(vec3 c) { return mix(0.18, 0.36, clamp(luma(c), 0.0, 1.0)); }

float beam(float dist, float sigma) { return exp(-dist * dist / (2.0 * sigma * sigma)); }

void main() {
    float curve = params.a.z * 0.12;
    vec2 c = vFrac * 2.0 - 1.0;
    c *= vec2(1.0 + curve * c.y * c.y, 1.0 + curve * c.x * c.x);
    vec2 uv = c * 0.5 + 0.5;

    // Pixels outside the curved screen are black; the edge is softened over about a pixel.
    vec2 px = vec2(2.0) / params.dest.zw;
    vec2 inside = smoothstep(vec2(0.0), px * 1.5, vec2(1.0) - abs(c));
    float edge = inside.x * inside.y;

    float lines = params.b.y;
    float y = uv.y * lines;
    float line0 = floor(y - 0.5);
    float d0 = y - (line0 + 0.5);
    vec3 c0 = toLinear(sampleLinear(uTex, vec2(uv.x, (line0 + 0.5) / lines)).rgb);
    vec3 c1 = toLinear(sampleLinear(uTex, vec2(uv.x, (line0 + 1.5) / lines)).rgb);

    float s0 = beamSigma(c0), s1 = beamSigma(c1);
    vec3 scan = c0 * beam(d0, s0) + c1 * beam(d0 - 1.0, s1);
    // A gaussian of this width covers sigma * sqrt(2 pi) of a line's height; scale the sum back up to keep the
    // average brightness.
    float gain = clamp(1.0 / (mix(s0, s1, clamp(d0, 0.0, 1.0)) * 2.5066), 1.0, 1.8);
    vec3 smooth_ = mix(c0, c1, clamp(d0, 0.0, 1.0));
    vec3 rgb = mix(smooth_, scan * gain, params.a.x);

    // Aperture grille: one red, one green and one blue column per three window pixels, averaging to 1.
    float s = params.a.y;
    float lo = 1.0 - 0.5 * s, hi = 1.0 + s;
    int col = int(floor(vFrac.x * params.dest.z)) % 3;
    vec3 mask = vec3(lo);
    mask[col] = hi;
    rgb *= mask;

    rgb *= 1.0 - params.a.w * 0.35 * dot(c, c);
    rgb = pow(clamp(rgb, 0.0, 1.0), vec3(1.0 / kGamma)) * edge;
    outColor = vec4(hdrOutput(rgb), 1.0);
}
