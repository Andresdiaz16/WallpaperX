#version 460
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require

layout(descriptor_heap) uniform sampler heapSampler[];
layout(descriptor_heap) uniform texture2D heapTexture2D[];

layout(push_constant) uniform PushData {
    uint index;
} pushData;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(sampler2D(heapTexture2D[nonuniformEXT(pushData.index)],
                                  heapSampler[nonuniformEXT(pushData.index)]), inUV);
}
