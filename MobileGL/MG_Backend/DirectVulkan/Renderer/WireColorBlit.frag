#version 450
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

void main() {
    // Source framebuffer clipping preserves the original scale, rather than
    // extending the edge texels into pixels outside the source framebuffer.
    if (any(lessThan(texCoord, vec2(0.0))) || any(greaterThan(texCoord, vec2(1.0)))) discard;
    // Explicit LOD is required on Adreno 650: implicit LOD through a rotated
    // single-mip UBWC render target can read outside its allocation.
    outColor = textureLod(sourceImage, texCoord, 0.0);
}
