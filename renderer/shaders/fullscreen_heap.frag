// SPDX-License-Identifier: GPL-2.0-or-later
#version 460
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require

layout(descriptor_heap) uniform sampler heapSampler[];
layout(descriptor_heap) uniform texture2D heapTexture2D[];

// Follows fullscreen.vert's block, which the two stages share.
layout(push_constant) uniform PushData {
    layout(offset = 16) uint index;
    // Non-zero to repeat the texture across the quad, at whole texels.
    layout(offset = 20) uint tile;
} pushData;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// Opaque types cannot be held in a local, hence the repetition.
#define IMAGE sampler2D(heapTexture2D[nonuniformEXT(pushData.index)], heapSampler[nonuniformEXT(pushData.index)])

void main() {
    outColor = pushData.tile != 0u ? textureLod(IMAGE, fract(inUV), 0.0) : texture(IMAGE, inUV);
}
