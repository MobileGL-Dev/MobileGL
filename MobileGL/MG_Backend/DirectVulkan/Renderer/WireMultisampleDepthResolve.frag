#version 450
// SAMPLE ZERO, and it is not an approximation: VK_RESOLVE_MODE_SAMPLE_ZERO_BIT is the mode the
// VK_KHR_depth_stencil_resolve arm asks for, and GL 4.6 core 18.3.1 says a depth resolve picks
// a value "in an implementation-dependent manner where the result will be between the minimum
// and maximum depth values in the pixel" - sample 0 is one of the samples, so it qualifies, and
// taking the same sample as the extension arm is what makes the two arms interchangeable on one
// device.
//
// AVERAGING WOULD NOT BE: a mean of four depths is a surface that exists nowhere, and GL's
// bound is on the per-pixel min/max, which a mean satisfies but a shadow comparison against it
// does not survive.
layout(set = 0, binding = 0) uniform sampler2DMS sourceImage;

void main() {
    gl_FragDepth = texelFetch(sourceImage, ivec2(gl_FragCoord.xy), 0).r;
}
