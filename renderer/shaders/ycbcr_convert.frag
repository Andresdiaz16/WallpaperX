#version 460

// Y' in luma.r; Cb in cb.r; Cr in cr.r. Which image each view reads, and through which
// swizzle, is decided on the host, so a two-plane and a three-plane picture run this same code.
layout(set = 0, binding = 0) uniform sampler2D luma;
layout(set = 0, binding = 1) uniform sampler2D cb;
layout(set = 0, binding = 2) uniform sampler2D cr;

// Offsets are explicit so the host struct and this block cannot disagree on padding.
layout(push_constant) uniform Push {
    // rgb = matrix * sampled + offset, with range, depth and sample alignment folded in.
    layout(offset = 0) mat3 matrix;
    layout(offset = 48) vec3 offset;
    // 1 / the extent each plane image covers in luma pixels. The images hold only the visible
    // rectangle's samples, so the output pixel is the luma texel and clamping stops at the
    // visible edge.
    layout(offset = 64) vec2 lumaScale;
    layout(offset = 72) vec2 chromaScale;
    // Where a chroma sample sits relative to the luma sample it is stored with, plus how far into
    // its chroma texel the visible rectangle starts.
    layout(offset = 80) vec2 chromaShift;
} push;

layout(location = 0) out vec4 outColor;

void main() {
    const vec2 pixel = gl_FragCoord.xy;
    const vec2 uvLuma = pixel * push.lumaScale;
    const vec2 uvChroma = pixel * push.chromaScale + push.chromaShift;
    const vec3 sampled = vec3(texture(luma, uvLuma).r, texture(cb, uvChroma).r, texture(cr, uvChroma).r);
    outColor = vec4(clamp(push.matrix * sampled + push.offset, 0.0, 1.0), 1.0);
}
