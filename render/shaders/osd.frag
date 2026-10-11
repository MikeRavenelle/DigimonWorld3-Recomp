#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Blends the notification image 1:1 over the window; the image is already sized for the window.

layout(set = 0, binding = 0) uniform texture2D uOsd;

layout(push_constant) uniform Params {
    vec4 dest;
    vec2 window;
    float opacity;
} params;

layout(location = 0) in vec2 vFrac;
layout(location = 0) out vec4 outColor;

void main() {
    ivec2 size = textureSize(uOsd, 0);
    vec4 c = texelFetch(uOsd, clamp(ivec2(vFrac * vec2(size)), ivec2(0), size - 1), 0);
    outColor = vec4(c.rgb, c.a * params.opacity);
}
