#version 450
#extension GL_EXT_samplerless_texture_functions : require
// VRAM-to-VRAM copy and fill. Each destination pixel looks up its source pixel in the sampled copy of VRAM,
// wrapping at the VRAM edges, so overlapping source and destination areas read the data as it was before the
// pass. The mask check discards pixels whose destination has the mask bit; the mask set forces it on.
//
// Two more modes move data between the scaled VRAM and a native-resolution texture bound in its place:
// replicate (every native pixel becomes scale x scale pixels) and downsample (the top-left subpixel of each
// native pixel is kept).
//
// flags: bit0 fill with `color`, bit1 check mask, bit2 set mask, bit3 replicate, bit4 downsample

layout(set = 0, binding = 0) uniform texture2D uVram;

layout(push_constant) uniform Params {
    ivec4 rect;   // source x, y, destination x, y (VRAM pixels)
    uint flags;
    uint color;   // RGBA8 for fills
    int scale;    // internal resolution multiplier
} params;

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 fp = ivec2(gl_FragCoord.xy);
    if ((params.flags & 1u) != 0u) {
        outColor = unpackUnorm4x8(params.color);
        return;
    }
    if ((params.flags & 8u) != 0u) {
        outColor = texelFetch(uVram, fp / params.scale, 0);
        return;
    }
    if ((params.flags & 16u) != 0u) {
        outColor = texelFetch(uVram, fp * params.scale, 0);
        return;
    }
    if ((params.flags & 2u) != 0u && texelFetch(uVram, fp, 0).a > 0.5) discard;
    ivec2 p = fp / params.scale;
    ivec2 sub = fp - p * params.scale;
    ivec2 src = (params.rect.xy + ((p - params.rect.zw) & ivec2(1023, 511))) & ivec2(1023, 511);
    vec4 c = texelFetch(uVram, src * params.scale + sub, 0);
    if ((params.flags & 4u) != 0u) c.a = 1.0;
    outColor = c;
}
