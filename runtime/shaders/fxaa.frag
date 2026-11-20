#version 450
// FXAA for runtime/vk_fxaa.c, after Timothy Lottes' FXAA 3.11 (quality variant): finds the local edge from the luma of
// the 3x3 neighbourhood, walks along it both ways to its ends, and reads the picture once, moved across the edge by the
// distance to the nearer end (plus a sub-pixel blend for one-pixel features). The source is read through an sRGB view
// and the result written to an sRGB target, so luma is taken from the encoded values (sqrt of linear luma, near enough).
// One full-screen triangle (hdr_fullscreen.vert); the target is the size of the source, texel for texel.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push { vec2 rcp; } pc;   // 1 / source size
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

const float EDGE_THRESHOLD = 0.063;       // the local contrast an edge needs, relative to its brightest luma
const float EDGE_THRESHOLD_MIN = 0.0156;  // and at least this (dark areas)
const float SUBPIX = 1.0;                 // sub-pixel blend strength
const int STEPS = 12;
const float STEP[STEPS] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float luma(vec3 c) { return sqrt(dot(c, vec3(0.299, 0.587, 0.114))); }
float luma_at(vec2 p) { return luma(textureLod(src, p, 0.0).rgb); }
#define luma_off(p, d) luma(textureLodOffset(src, p, 0.0, d).rgb)

void main() {
    vec2 p = gl_FragCoord.xy * pc.rcp;
    vec4 c = textureLod(src, p, 0.0);
    float lM = luma(c.rgb);
    float lU = luma_off(p, ivec2(0, -1)), lD = luma_off(p, ivec2(0, 1));
    float lL = luma_off(p, ivec2(-1, 0)), lR = luma_off(p, ivec2(1, 0));
    float lmin = min(lM, min(min(lU, lD), min(lL, lR)));
    float lmax = max(lM, max(max(lU, lD), max(lL, lR)));
    float range = lmax - lmin;
    if (range < max(EDGE_THRESHOLD_MIN, lmax * EDGE_THRESHOLD)) { o = c; return; }

    float lUL = luma_off(p, ivec2(-1, -1)), lUR = luma_off(p, ivec2(1, -1));
    float lDL = luma_off(p, ivec2(-1, 1)), lDR = luma_off(p, ivec2(1, 1));
    float lUD = lU + lD, lLR = lL + lR;
    float edgeH = abs(-2.0 * lL + lUL + lDL) + abs(-2.0 * lM + lUD) * 2.0 + abs(-2.0 * lR + lUR + lDR);
    float edgeV = abs(-2.0 * lU + lUL + lUR) + abs(-2.0 * lM + lLR) * 2.0 + abs(-2.0 * lD + lDL + lDR);
    bool horz = edgeH >= edgeV;   // a horizontal edge: the picture changes along y

    // The side of the edge with the steeper gradient.
    float l1 = horz ? lU : lL, l2 = horz ? lD : lR;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool neg = abs(g1) >= abs(g2);
    float gScaled = 0.25 * max(abs(g1), abs(g2));
    float stepLen = horz ? pc.rcp.y : pc.rcp.x;
    float lLocal;
    if (neg) stepLen = -stepLen, lLocal = 0.5 * (l1 + lM);
    else lLocal = 0.5 * (l2 + lM);

    // Walk along the edge, half a pixel over on that side, until the luma leaves the edge's on both ends.
    vec2 cur = p;
    if (horz) cur.y += stepLen * 0.5;
    else cur.x += stepLen * 0.5;
    vec2 off = horz ? vec2(pc.rcp.x, 0.0) : vec2(0.0, pc.rcp.y);
    vec2 u1 = cur - off, u2 = cur + off;
    float e1 = luma_at(u1) - lLocal, e2 = luma_at(u2) - lLocal;
    bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
    if (!r1) u1 -= off;
    if (!r2) u2 += off;
    for (int i = 2; i < STEPS && !(r1 && r2); i++) {
        if (!r1) e1 = luma_at(u1) - lLocal;
        if (!r2) e2 = luma_at(u2) - lLocal;
        r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
        if (!r1) u1 -= off * STEP[i];
        if (!r2) u2 += off * STEP[i];
    }
    float d1 = horz ? p.x - u1.x : p.y - u1.y;
    float d2 = horz ? u2.x - p.x : u2.y - p.y;
    bool near1 = d1 < d2;
    float edgeOffset = -min(d1, d2) / (d1 + d2) + 0.5;
    // Only when the luma at the nearer end varies the way the centre does is the pixel on the edge's blend side.
    bool ok = ((near1 ? e1 : e2) < 0.0) != (lM < lLocal);
    float finalOffset = ok ? edgeOffset : 0.0;

    float lAvg = (2.0 * (lUD + lLR) + (lUL + lUR + lDL + lDR)) / 12.0;
    float s = clamp(abs(lAvg - lM) / range, 0.0, 1.0);
    s = (-2.0 * s + 3.0) * s * s;
    finalOffset = max(finalOffset, s * s * SUBPIX);

    vec2 q = p;
    if (horz) q.y += finalOffset * stepLen;
    else q.x += finalOffset * stepLen;
    o = vec4(textureLod(src, q, 0.0).rgb, c.a);
}
