// SPDX-License-Identifier: GPL-2.0-or-later
#version 460

layout(set = 0, binding = 0) uniform sampler2D tex;

// Follows fullscreen.vert's block, which the two stages share. Non-zero to repeat the texture
// across the quad, at whole texels.
layout(push_constant) uniform PushData {
    layout(offset = 20) uint tile;
} pushData;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = pushData.tile != 0u ? textureLod(tex, fract(inUV), 0.0) : texture(tex, inUV);
}
