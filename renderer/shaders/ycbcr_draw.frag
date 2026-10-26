// SPDX-License-Identifier: GPL-2.0-or-later
#version 460

// Y' in luma.r; Cb in cb.r; Cr in cr.r, each a view of one plane, swizzled on the host.
layout(set = 0, binding = 0) uniform sampler2D luma;
layout(set = 0, binding = 1) uniform sampler2D cb;
layout(set = 0, binding = 2) uniform sampler2D cr;

// Fixed by the picture's shape, so each renderer bakes its own into its pipeline. Luma is clamped
// to the visible picture's edge texels, [0.5, lumaMax], and moved into the luma image by
// lumaShift; chroma is clamped to the texels the visible picture uses. The inverse image sizes are
// constants rather than textureSize(), which costs more than the rest of the draw's arithmetic.
layout(constant_id = 0) const float lumaMaxX = 0.5;
layout(constant_id = 1) const float lumaMaxY = 0.5;
layout(constant_id = 2) const float lumaShiftX = 0.0;
layout(constant_id = 3) const float lumaShiftY = 0.0;
layout(constant_id = 4) const float lumaInvX = 1.0;
layout(constant_id = 5) const float lumaInvY = 1.0;
layout(constant_id = 6) const float chromaScaleX = 1.0;
layout(constant_id = 7) const float chromaScaleY = 1.0;
layout(constant_id = 8) const float chromaMinX = 0.5;
layout(constant_id = 9) const float chromaMinY = 0.5;
layout(constant_id = 10) const float chromaMaxX = 0.5;
layout(constant_id = 11) const float chromaMaxY = 0.5;
layout(constant_id = 12) const float chromaInvX = 1.0;
layout(constant_id = 13) const float chromaInvY = 1.0;

// What each picture and each target changes. Offsets are explicit so the host struct and this
// block cannot disagree on padding.
layout(push_constant) uniform Push {
    // rgb = matrix * sampled + offset, with range, depth and sample alignment folded in.
    layout(offset = 0) mat3 matrix;
    layout(offset = 48) vec3 offset;
    // The picture position a fragment shows, in luma pixels from the visible picture's corner:
    // gl_FragCoord.xy * srcPerDst + srcOffset.
    layout(offset = 64) vec2 srcPerDst;
    layout(offset = 72) vec2 srcOffset;
    // Chroma image texels are position * chromaScale + chromaShift; the shift carries the
    // picture's chroma siting.
    layout(offset = 80) vec2 chromaShift;
} push;

layout(location = 0) out vec4 outColor;

void main() {
    const vec2 position = gl_FragCoord.xy * push.srcPerDst + push.srcOffset;
    const vec2 lumaTexel = clamp(position, vec2(0.5), vec2(lumaMaxX, lumaMaxY)) + vec2(lumaShiftX, lumaShiftY);
    const vec2 chromaTexel = clamp(position * vec2(chromaScaleX, chromaScaleY) + push.chromaShift,
                                   vec2(chromaMinX, chromaMinY), vec2(chromaMaxX, chromaMaxY));
    const vec2 chromaUv = chromaTexel * vec2(chromaInvX, chromaInvY);
    const vec3 sampled = vec3(texture(luma, lumaTexel * vec2(lumaInvX, lumaInvY)).r, texture(cb, chromaUv).r,
                              texture(cr, chromaUv).r);
    outColor = vec4(clamp(push.matrix * sampled + push.offset, 0.0, 1.0), 1.0);
}
