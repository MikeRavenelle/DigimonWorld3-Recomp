#version 450
// Vertex stage of the primitive pipeline. Positions are VRAM pixel coordinates; the viewport spans the whole
// render target, so the mapping to clip space does not depend on the internal resolution. Colour and texel
// coordinates travel as flat fixed-point planes (see HwVertex) that the pixel stage evaluates. Primitives with the
// precise bit (state0 bit 11) instead carry float attributes in the plane slots, which the GPU interpolates:
// colour in screen space, texel coordinates perspective-correct through the clip-space w.

layout(location = 0) in vec2 inPos;
layout(location = 1) in ivec4 inPlanes0;
layout(location = 2) in ivec4 inPlanes1;
layout(location = 3) in ivec4 inPlanes2;
layout(location = 4) in ivec3 inPlanes3;
layout(location = 5) in uvec3 inState;  // packed draw state, constant per primitive
layout(location = 6) in uint inUvRect;
layout(location = 7) in uint inHdOffset;

layout(location = 0) flat out ivec4 vPlanes0;
layout(location = 1) flat out ivec4 vPlanes1;
layout(location = 2) flat out ivec4 vPlanes2;
layout(location = 3) flat out ivec3 vPlanes3;
layout(location = 4) flat out uvec3 vState;
layout(location = 5) flat out uint vUvRect;
layout(location = 6) noperspective out vec3 vColF;
layout(location = 7) out vec2 vUvF;
layout(location = 8) flat out uint vHdOffset;

void main() {
    float w = 1.0;
    vColF = vec3(0.0);
    vUvF = vec2(0.0);
    if ((inState.x & 2048u) != 0u) {
        // Float attributes: colour r, g, b, texel u, v and depth w (zero when the vertices have none).
        vColF = vec3(intBitsToFloat(inPlanes0.x), intBitsToFloat(inPlanes0.y), intBitsToFloat(inPlanes0.z));
        vUvF = vec2(intBitsToFloat(inPlanes0.w), intBitsToFloat(inPlanes1.x));
        float depth = intBitsToFloat(inPlanes1.y);
        if (depth > 0.0) w = depth;
    }
    gl_Position = vec4((inPos.x * (2.0 / 1024.0) - 1.0) * w, (inPos.y * (2.0 / 512.0) - 1.0) * w, 0.0, w);
    vPlanes0 = inPlanes0;
    vPlanes1 = inPlanes1;
    vPlanes2 = inPlanes2;
    vPlanes3 = inPlanes3;
    vState = inState;
    vUvRect = inUvRect;
    vHdOffset = inHdOffset;
}
