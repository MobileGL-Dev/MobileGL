#version 450
// The discard state probe's draws (WireDiscardStateProbe.h): vertices 0-3 are a full-viewport strip,
// vertices 4-7 the same strip far outside the clip volume (the control's first draw).
void main() {
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    const vec2 offset = gl_VertexIndex >= 4 ? vec2(8.0) : vec2(0.0);
    gl_Position = vec4(corners[gl_VertexIndex & 3] + offset, 0.0, 1.0);
}
