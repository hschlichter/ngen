#version 450

layout(location = 0) out vec2 fragTexCoord;

// Fullscreen triangle: generates 3 vertices from gl_VertexIndex that form a single
// oversized triangle covering the entire viewport. No vertex buffer needed.
// More efficient than a quad (no diagonal seam where fragments overlap).
//   V0(-1,-1)  V1(3,-1)  V2(-1,3)  — GPU clips to the screen rectangle.
void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragTexCoord = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
