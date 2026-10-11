#version 450
// Vertex stage of the primitive pipeline. Positions are VRAM pixel coordinates; the viewport spans the whole
// render target, so the mapping to clip space does not depend on the internal resolution.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;   // unorm8, 0..255 after scaling
layout(location = 2) in vec2 inUv;
layout(location = 3) in uvec3 inState;  // packed draw state, constant per primitive

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uvec3 vState;

void main() {
    gl_Position = vec4(inPos.x * (2.0 / 1024.0) - 1.0, inPos.y * (2.0 / 512.0) - 1.0, 0.0, 1.0);
    vColor = inColor.rgb * 255.0;
    vUv = inUv;
    vState = inState;
}
