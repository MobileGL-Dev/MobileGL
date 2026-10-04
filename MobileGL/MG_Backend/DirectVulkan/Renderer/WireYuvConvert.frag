#version 450
// The image is bound through an immutable sampler that carries its VkSamplerYcbcrConversion, so
// a plain sample already returns RGB.
layout(set = 0, binding = 0) uniform sampler2D yuvImage;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(textureLod(yuvImage, texCoord, 0.0).rgb, 1.0);
}
