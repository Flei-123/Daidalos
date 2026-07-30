#version 450
// Fullscreen triangle, no vertex buffer. Covers the whole viewport with
// three vertices; clip coordinates come straight out of gl_VertexIndex.

layout(location = 0) out vec2 vNdc;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);   // z = 1 -> behind everything
}
