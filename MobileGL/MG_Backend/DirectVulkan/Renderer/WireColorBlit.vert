#version 450
// Native Vulkan blit: no GL program, registry entry, or runtime transpilation.
layout(push_constant) uniform BlitRect {
    vec4 srcRect;
    vec4 dstRect;
    int surfaceTransform;
} rect;
layout(location = 0) out vec2 texCoord;

void main() {
    // A quad, rather than an oversized triangle, also clips partial blits to
    // their destination rectangle. The Vulkan viewport covers the native image.
    const vec2 corners[6] = vec2[](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
        vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    vec2 uv = corners[gl_VertexIndex];
    vec2 p = (rect.dstRect.xy + uv * rect.dstRect.zw) * 2.0 - 1.0;
    // Same GL-bottom-left to native-image mapping as the monolithic blit.
    p.y = -p.y;
    if (rect.surfaceTransform == 1) p = vec2(-p.y, p.x);
    else if (rect.surfaceTransform == 2) p = -p;
    else if (rect.surfaceTransform == 3) p = vec2(p.y, -p.x);
    gl_Position = vec4(p, 0.0, 1.0);
    texCoord = rect.srcRect.xy + uv * rect.srcRect.zw;
}
