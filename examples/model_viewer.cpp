// Loads any .glb/.gltf and renders turntable frames of it.
//
//   DAI_SHADER_DIR=shaders ./build/model_viewer assets/test/blender_scene.glb 6 /tmp
//
// This is the whole Blender round trip in one command: export a GLB, point
// this at it, look at the PNGs. No import settings, no material fixing.

#include "dai_gltf.h"
#include "dai_render.h"
#include "dai_scene.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "assets/test/blender_scene.glb";
    int frames = argc > 2 ? std::atoi(argv[2]) : 4;
    const char *outdir = argc > 3 ? argv[3] : ".";

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 1280; rd.height = 720; rd.msaa = 4; rd.shadow_size = 2048;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("no renderer: %s\n", err); return 2; }

    dai_model *m = dai_gltf_load(r, path, err, sizeof(err));
    if (!m) { std::printf("import failed: %s\n", err); dai_render_destroy(r); return 1; }

    dai_model_info info = dai_model_get_info(m);
    std::printf("%s\n  %u nodes, %u meshes, %u materials, %u textures, %u triangles\n",
                path, info.nodes, info.meshes, info.materials, info.textures, info.triangles);

    // Framing from the bounding box is wrong for anything with a ground plane:
    // one 24 m quad pushes the camera so far back that the actual props become
    // three pixels. Frame the spread of the object PIVOTS instead, which is
    // what a person means by "look at the model".
    dai_vec3 c{ 0, 0, 0 };
    uint32_t nn = dai_model_node_count(m);
    for (uint32_t i = 0; i < nn; ++i) {
        const dai_model_node *node = dai_model_node_at(m, i);
        c.x += node->position.x; c.y += node->position.y; c.z += node->position.z;
    }
    if (nn) { c.x /= (float)nn; c.y /= (float)nn; c.z /= (float)nn; }
    float radius = 0.0f;
    for (uint32_t i = 0; i < nn; ++i) {
        const dai_model_node *node = dai_model_node_at(m, i);
        float dx = node->position.x - c.x, dy = node->position.y - c.y, dz = node->position.z - c.z;
        float d = sqrtf(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    // a single object has no pivot spread at all, so fall back to its extent
    float ex = 0.5f * (info.bounds_max.x - info.bounds_min.x);
    float ey = 0.5f * (info.bounds_max.y - info.bounds_min.y);
    float ez = 0.5f * (info.bounds_max.z - info.bounds_min.z);
    float extent = sqrtf(ex*ex + ey*ey + ez*ez);
    radius = radius * 1.5f + 1.0f;
    if (nn <= 2 && extent > radius) { radius = extent; c = { (info.bounds_min.x + info.bounds_max.x) * 0.5f,
                                                            (info.bounds_min.y + info.bounds_max.y) * 0.5f,
                                                            (info.bounds_min.z + info.bounds_max.z) * 0.5f }; }
    std::printf("  framing centre (%.2f %.2f %.2f) radius %.2f\n", c.x, c.y, c.z, radius);

    dai_render_sun(r, dai_vec3{ 0.40f, 0.78f, 0.48f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.35f);
    dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.36f);
    dai_render_fog(r, 0.0025f, dai_vec3{ 0.56f, 0.64f, 0.74f });
    dai_render_exposure(r, 0.58f);
    dai_render_shadow_extent(r, radius * 1.6f);

    dai_camera cam = dai_camera_default();
    dai_camera_frame(&cam, dai_vec3{ c.x, c.y + radius * 0.05f, c.z }, radius * 0.55f);
    cam.pitch = 0.22f;

    std::vector<dai_render_instance> inst(4096);
    std::vector<float> joints((size_t)dai_render_max_joints(r) * 16, 0.0f);
    int anim = dai_model_animation_count(m) ? 0 : -1;
    float duration = 0.0f;
    if (anim >= 0) {
        dai_animation_info ai = dai_model_animation_at(m, 0);
        duration = ai.duration;
        std::printf("  animation \"%s\": %.2f s, %u channels\n", ai.name, ai.duration, ai.channels);
    }

    uint32_t n = dai_model_instances(m, inst.data(), (uint32_t)inst.size(),
                                     dai_vec3{ 0,0,0 }, dai_quat{ 0,0,0,1 }, 1.0f);
    for (int i = 0; i < frames; ++i) {
        if (anim >= 0) {
            float t = duration * (float)i / (float)(frames > 1 ? frames : 1);
            uint32_t jn = dai_model_pose(m, anim, t, joints.data(), (uint32_t)(joints.size() / 16));
            dai_render_joints(r, joints.data(), jn);
            n = dai_model_instances(m, inst.data(), (uint32_t)inst.size(),
                                    dai_vec3{ 0,0,0 }, dai_quat{ 0,0,0,1 }, 1.0f);
        }
        cam.yaw = 0.6f + (float)i * (6.2831853f / (float)(frames > 1 ? frames : 1)) * 0.5f;
        dai_render_camera(r, dai_camera_eye(&cam), cam.target, dai_vec3{ 0,1,0 }, cam.fov, cam.znear, cam.zfar);
        dai_render_frame(r, inst.data(), n);
        char out[512];
        std::snprintf(out, sizeof(out), "%s/model_%02d.png", outdir, i);
        dai_render_write_png(r, out);
        std::printf("  frame %d: %u instances, %u draws, %.1f ms -> %s\n",
                    i, n, dai_render_last_draws(r), dai_render_last_ms(r), out);
    }

    dai_model_free(m);
    dai_render_destroy(r);
    return 0;
}
