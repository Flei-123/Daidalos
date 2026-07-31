// Skinning and animation.
//
// The test does not trust the renderer to tell it the truth: it checks the
// joint matrices numerically first (they are arithmetic, so they can be wrong
// in a way that still looks plausible), and only then checks that the picture
// changes when the model is posed.
//
//   DAI_SHADER_DIR=shaders ./build/test_skinning [assets/test] [outdir]

#include "dai_gltf.h"
#include "dai_render.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

// apply a column major 4x4 to a point
static void xform(const float *m, const float *p, float *out) {
    for (int i = 0; i < 3; ++i)
        out[i] = m[0*4+i]*p[0] + m[1*4+i]*p[1] + m[2*4+i]*p[2] + m[3*4+i];
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "assets/test";
    std::string outdir = argc > 2 ? argv[2] : "/tmp";

    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 640; rd.height = 640; rd.msaa = 4;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }

    dai_model *m = dai_gltf_load(r, (dir + "/skinned.glb").c_str(), err, sizeof(err));
    CHECK(m != nullptr, "loading skinned.glb failed: %s", err);
    if (!m) { dai_render_destroy(r); return 1; }

    dai_model_info info = dai_model_get_info(m);
    std::printf("  nodes %u | skins %u | joints %u | animations %u | tris %u\n",
                info.nodes, info.skins, info.joints, info.animations, info.triangles);
    CHECK(info.skins == 1, "expected 1 skin, got %u", info.skins);
    CHECK(info.joints == 2, "expected 2 joints, got %u", info.joints);
    CHECK(info.animations == 1, "expected 1 animation, got %u", info.animations);

    dai_animation_info a = dai_model_animation_at(m, 0);
    std::printf("  animation \"%s\" %.2f s, %u channels\n", a.name, a.duration, a.channels);
    CHECK(std::strcmp(a.name, "bend") == 0, "animation name is '%s', expected 'bend'", a.name);
    CHECK(fabsf(a.duration - 2.0f) < 0.01f, "duration is %.3f s, expected 2.0", a.duration);

    // ---- joint matrices, checked as arithmetic
    float j0[32] = {0}, j1[32] = {0}, j2[32] = {0};
    uint32_t n0 = dai_model_pose(m, 0, 0.0f, j0, 2);          // rest
    uint32_t n1 = dai_model_pose(m, 0, 1.0f, j1, 2);          // 60 degrees
    uint32_t n2 = dai_model_pose(m, 0, 2.0f, j2, 2);          // back to rest
    CHECK(n0 == 2 && n1 == 2, "pose wrote %u/%u matrices, expected 2", n0, n1);

    // at rest both joints must be identity (world == bind pose)
    bool identity = true;
    for (int i = 0; i < 16; ++i) {
        float expect = (i % 5 == 0) ? 1.0f : 0.0f;
        if (fabsf(j0[i] - expect) > 1e-4f) identity = false;
    }
    CHECK(identity, "joint 0 at t=0 is not identity - the inverse bind matrices are wrong");

    // the tip of the limb: local (0,4,0) -> after 60 deg about Z at y=2
    // it should swing to x = -sin(60)*2 = -1.73, y = 2 + cos(60)*2 = 3.0
    float tip[3] = { 0, 4, 0 }, moved[3] = {0};
    xform(j1 + 16, tip, moved);
    std::printf("  tip at 60 deg: (%.3f %.3f %.3f)\n", moved[0], moved[1], moved[2]);
    CHECK(fabsf(moved[0] - (-1.732f)) < 0.05f && fabsf(moved[1] - 3.0f) < 0.05f,
          "posed tip is (%.3f %.3f), expected (-1.73 3.00)", moved[0], moved[1]);

    bool back = true;
    for (int i = 0; i < 32; ++i) if (fabsf(j0[i] - j2[i]) > 1e-4f) back = false;
    CHECK(back, "the animation does not return to the same pose after a full loop");
    CHECK(n2 == 2, "pose at the end wrote %u matrices", n2);

    // ---- and now in pixels
    dai_render_sun(r, dai_vec3{ 0.3f, 0.7f, 0.65f }, dai_vec3{ 1.0f, 0.95f, 0.9f }, 1.4f);
    dai_render_ambient(r, dai_vec3{ 0.3f, 0.4f, 0.6f }, dai_vec3{ 0.2f, 0.2f, 0.2f }, 0.45f);
    dai_render_sky(r, 0);
    dai_render_clear_color(r, 0.02f, 0.02f, 0.03f);
    dai_render_fog(r, 0.0f, dai_vec3{0,0,0});
    dai_render_exposure(r, 0.9f);
    dai_render_camera(r, dai_vec3{ 0, 2.2f, 9.0f }, dai_vec3{ 0, 2.0f, 0 }, dai_vec3{ 0,1,0 }, 45.0f, 0.1f, 100.0f);

    std::vector<dai_render_instance> inst(64);
    std::vector<uint8_t> rest_px, bent_px;
    auto shot = [&](float t, std::vector<uint8_t> &px, const char *name) {
        float joints[32] = {0};
        uint32_t jn = dai_model_pose(m, 0, t, joints, 2);
        dai_render_joints(r, joints, jn);
        uint32_t n = dai_model_instances(m, inst.data(), (uint32_t)inst.size(),
                                         dai_vec3{ 0,0,0 }, dai_quat{ 0,0,0,1 }, 1.0f);
        dai_render_frame(r, inst.data(), n);
        px.resize((size_t)dai_render_width(r) * dai_render_height(r) * 4);
        dai_render_readback(r, px.data(), px.size());
        dai_render_write_png(r, (outdir + "/" + name).c_str());
        return n;
    };
    uint32_t drawn = shot(0.0f, rest_px, "skin_rest.png");
    shot(1.0f, bent_px, "skin_bent.png");
    CHECK(drawn >= 1, "the skinned mesh produced %u instances", drawn);

    size_t lit_rest = 0, changed = 0;
    for (size_t i = 0; i < rest_px.size(); i += 4) {
        if (rest_px[i] + rest_px[i+1] + rest_px[i+2] > 40) ++lit_rest;
        int d = std::abs((int)rest_px[i] - (int)bent_px[i]) + std::abs((int)rest_px[i+1] - (int)bent_px[i+1]);
        if (d > 16) ++changed;
    }
    double covered = 100.0 * (double)lit_rest / (double)(rest_px.size() / 4);
    double moved_px = 100.0 * (double)changed / (double)(rest_px.size() / 4);
    std::printf("  limb covers %.1f%% of the frame, posing moved %.1f%% of pixels\n", covered, moved_px);
    CHECK(covered > 1.0, "the skinned mesh is not visible (%.2f%% coverage)", covered);
    CHECK(moved_px > 0.5, "posing the model changed only %.2f%% of the frame - the GPU is not skinning", moved_px);

    dai_model_free(m);
    dai_render_destroy(r);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
