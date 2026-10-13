// Output transform of the HDR swap chains (mirrors runtime/include/dw3/hdr.h, which the tests pin down). Included by the
// passes that can be the last one; every other pass is given params.c = 0 and passes the picture through.
//
// params.c: x output (0 SDR, 1 scRGB, 2 HDR10 PQ, 3 EDR), y peak nits, z paper white nits.
//
// The picture is sRGB encoded SDR. It is linearised, placed so that SDR white lands on paper white, and only the part
// above the knee is lifted, towards the peak for white; the gain is the same for all three channels so hue and
// saturation are those of the SDR picture.

vec3 srgbToLinear(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(0.04045)));
}

vec3 pqEncode(vec3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    vec3 y = pow(clamp(nits, 0.0, 10000.0) / 10000.0, vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

vec3 hdrOutput(vec3 srgb) {
    int mode = int(params.c.x + 0.5);
    if (mode == 0) return srgb;
    float peak = params.c.y, paper = params.c.z;
    float headroom = max(peak / paper, 1.0);
    vec3 lin = srgbToLinear(srgb);
    float t = clamp((dot(lin, vec3(0.2126, 0.7152, 0.0722)) - 0.75) / 0.25, 0.0, 1.0);
    vec3 rel = min(lin * (1.0 + (headroom - 1.0) * t * t), vec3(headroom));  // relative to paper white
    if (mode == 1) return rel * (paper / 80.0);
    if (mode == 3) return rel;
    const mat3 rec709ToRec2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                                      0.3292820, 0.9195400, 0.0880132,
                                      0.0433136, 0.0113612, 0.8955950);  // columns, as GLSL stores them
    return pqEncode(rec709ToRec2020 * (rel * paper));
}
