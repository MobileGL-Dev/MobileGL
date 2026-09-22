#version 450
// Native Vulkan multisample depth/stencil resolve: no GL program, registry entry, or runtime
// transpilation.
//
// THIS STAGE CARRIES NOTHING, and that is the whole design. The scratch this pass writes has
// the SAME extent and the SAME coordinate space as the multisample source it reads - both are
// native images, neither is flipped against the other - so the fragment stage can take its
// coordinate straight out of gl_FragCoord and needs no varying, no push constant and no rect.
// The requested rectangle is expressed by the scissor instead, which is also what the
// VK_KHR_depth_stencil_resolve arm next door expresses with its renderArea.
//
// An oversized triangle rather than WireDepthMipmap.vert's quad: there is no destination
// rectangle to clip to here, only the scissor, and a triangle rasterizes the same area with
// one fewer primitive and no shared edge.
void main() {
    const vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
