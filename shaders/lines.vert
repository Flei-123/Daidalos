#version 450
// World-space lines with depth testing: editor floor grid, debug geometry.
// Same Frame UBO as the mesh pipeline (set 0, binding 0), so the per-view
// dynamic offset trick works unchanged.

layout(location = 0) in vec3 inPos;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewproj;
    mat4 invviewproj;
    mat4 lightviewproj[3];
    vec4 cascade_split;
    vec4 sun_dir;
    vec4 sun_color;
    vec4 sky_color;
    vec4 ground_color;
    vec4 fog_color;
    vec4 cam_pos;
    vec4 cam_right;
    vec4 cam_up;
} F;

void main() {
    gl_Position = F.viewproj * vec4(inPos, 1.0);
}
