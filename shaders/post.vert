#version 450
// Fullscreen triangle for every post pass, no vertex buffer - the same trick
// sky.vert uses. Three vertices out of gl_VertexIndex cover the viewport, and
// a triangle rather than a quad means no diagonal seam where two triangles
// meet and no fragment is shaded twice along it.

layout(location = 0) out vec2 vUv;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    // Vulkan clip space has y DOWN and the images are stored top row first, so
    // uv.y maps straight from clip y without a flip. Flipping here instead
    // would turn the vignette and the aberration upside down while a symmetric
    // blur kernel hid the mistake completely.
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
