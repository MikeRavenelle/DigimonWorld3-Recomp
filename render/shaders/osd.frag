#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Blends the notification image 1:1 over the window; the image is already sized for the window.

layout(set = 0, binding = 0) uniform texture2D uOsd;

layout(push_constant) uniform Params {
    vec4 dest;
    vec2 window;
    float opacity;
    float hdrScale;  // 0 for an SDR swap chain; otherwise the multiplier of linear SDR white (paper white / 80 nits for scRGB, 1 for EDR)
} params;

layout(location = 0) in vec2 vFrac;
layout(location = 0) out vec4 outColor;

void main() {
    ivec2 size = textureSize(uOsd, 0);
    vec4 c = texelFetch(uOsd, clamp(ivec2(vFrac * vec2(size)), ivec2(0), size - 1), 0);
    vec3 rgb = c.rgb;
    if (params.hdrScale > 0.0) {
        rgb = clamp(rgb, 0.0, 1.0);
        rgb = mix(pow((rgb + 0.055) / 1.055, vec3(2.4)), rgb / 12.92, lessThanEqual(rgb, vec3(0.04045))) * params.hdrScale;
    }
    outColor = vec4(rgb, c.a * params.opacity);
}
