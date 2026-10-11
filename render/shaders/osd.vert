#version 450
// Draws the on-screen notification rectangle (window pixels) as two triangles.

layout(push_constant) uniform Params {
    vec4 dest;    // x, y, w, h in window pixels
    vec2 window;  // window size in pixels
    float opacity;
} params;

layout(location = 0) out vec2 vFrac;

void main() {
    const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 c = corners[gl_VertexIndex];
    vFrac = c;
    vec2 px = params.dest.xy + c * params.dest.zw;
    gl_Position = vec4(px / params.window * 2.0 - 1.0, 0.0, 1.0);
}
