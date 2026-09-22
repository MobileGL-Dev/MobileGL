#version 450
// The monolith arm's kDepthMipmapFragmentShaderSource (VulkanRenderer.cpp), ported to the
// baked pipeline: the same 2x2 box of texelFetches, the same clamp, the same average, the
// same gl_FragDepth. It is a PORT and not a rewrite on purpose - CONTRACT-P7 §3.2 retires a
// refusal by making the wire arm observably the monolith's, so the two arms have to agree
// texel for texel, including where they read.
//
// uSrcTexelSize arrives on the push constant rather than in a uniform block: the wire arm
// owns no GL program, so there is no default-block uniform to write and no global UBO to
// bind. That is the whole of OQ-13's narrow half for this program.
layout(push_constant) uniform DepthMipmapRect {
    vec4 srcRect;
    vec4 dstRect;
    int surfaceTransform;
    ivec2 srcTexelSize;
} rect;
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(location = 0) in vec2 texCoord;

void main() {
    ivec2 srcBase = ivec2(texCoord * vec2(rect.srcTexelSize));
    ivec2 srcMax = rect.srcTexelSize - ivec2(1);
    float depth0 = texelFetch(sourceImage, clamp(srcBase, ivec2(0), srcMax), 0).r;
    float depth1 = texelFetch(sourceImage, clamp(srcBase + ivec2(1, 0), ivec2(0), srcMax), 0).r;
    float depth2 = texelFetch(sourceImage, clamp(srcBase + ivec2(0, 1), ivec2(0), srcMax), 0).r;
    float depth3 = texelFetch(sourceImage, clamp(srcBase + ivec2(1, 1), ivec2(0), srcMax), 0).r;
    gl_FragDepth = 0.25 * (depth0 + depth1 + depth2 + depth3);
}
