#version 450
// A YUV shared image converted into an RGBA texture (WireYuvImage.inc): one triangle over the
// whole target, its first row at the image's first row.
layout(location = 0) out vec2 texCoord;

void main() {
    const vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = corners[gl_VertexIndex];
    gl_Position = vec4(p, 0.0, 1.0);
    texCoord = (p + 1.0) * 0.5;
}
