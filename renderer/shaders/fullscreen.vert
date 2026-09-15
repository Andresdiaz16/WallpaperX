#version 460

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;

// The region of the texture the quad shows, in normalized texture coordinates.
layout(push_constant) uniform PushData {
    layout(offset = 0) vec2 uvOrigin;
    layout(offset = 8) vec2 uvExtent;
} pushData;

layout(location = 0) out vec2 outUV;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    outUV = pushData.uvOrigin + inUV * pushData.uvExtent;
}
