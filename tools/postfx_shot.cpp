// The post processing chain, photographed - the same scene twice, once with
// the chain off and once with it on.
//
//   DAI_SHADER_DIR=shaders ./build/postfx_shot [OUTDIR] [W] [H]
//
// Writes <outdir>/postfx-off.png and <outdir>/postfx-on.png.
//
// Why a tool rather than a test: tests/test_postfx.cpp already MEASURES the
// chain, and a number is what proves it works. This exists because a number
// does not show whether it looks right - overlapping glow, a vignette that
// eats the frame, grain the size of gravel. Two pictures of one scene do.
//
// The scene is deliberately a neon one: emissive bars and spheres on a dark
// floor, which is the case the whole chain was built for. A grey test pattern
// would photograph a correct bloom as nothing at all.

#include "daidalos.h"
#include "dai_render.h"
#include "dai_material.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// A neon emitter: fully emissive, so it needs no light and reads as a tube
// rather than as a lit surface.
static dai_render_instance neon(dai_vec3 pos, dai_vec3 scale, dai_vec3 color) {
    dai_render_instance i{};
    i.mesh = DAI_MESH_BOX;
    i.position = pos;
    i.rotation = dai_quat{ 0, 0, 0, 1 };
    i.scale = scale;
    i.color = color;
    i.emissive = 1.0f;
    return i;
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : ".gauntlet-shots";
    uint32_t w = argc > 2 ? (uint32_t)std::atoi(argv[2]) : 1280;
    uint32_t h = argc > 3 ? (uint32_t)std::atoi(argv[3]) : 720;

    char err[256] = { 0 };
    dai_render_desc d{};
    d.width = w; d.height = h;
    d.msaa = 4;
    d.shadow_size = 1024;
    dai_renderer *r = dai_render_create(&d, err, sizeof(err));
    if (!r) { std::printf("postfx_shot: no renderer: %s\n", err); return 1; }
    std::printf("postfx_shot on %s\n", dai_render_device_name(r));

    // A dark room so the emitters are the only real light in the picture -
    // that is the condition a bloom is judged in.
    dai_render_sky(r, 0);
    dai_render_clear_color(r, 0.02f, 0.02f, 0.035f);
    dai_render_light(r, dai_vec3{ 0.3f, 0.7f, 0.5f });
    dai_render_ambient(r, dai_vec3{ 0.05f, 0.06f, 0.10f }, dai_vec3{ 0.02f, 0.02f, 0.03f }, 0.25f);
    dai_render_fog(r, 0.0f, dai_vec3{ 0, 0, 0 });
    dai_render_exposure(r, 1.0f);
    dai_render_camera(r, dai_vec3{ 0.0f, 2.2f, 9.0f }, dai_vec3{ 0.0f, 1.0f, 0.0f },
                      dai_vec3{ 0, 1, 0 }, 55.0f, 0.1f, 200.0f);

    std::vector<dai_render_instance> inst;

    // floor - the one non emissive surface, so the glow has something to sit
    // against and the vignette has something to darken
    {
        dai_render_instance f{};
        f.mesh = DAI_MESH_BOX;
        f.position = dai_vec3{ 0, -0.25f, 0 };
        f.rotation = dai_quat{ 0, 0, 0, 1 };
        f.scale = dai_vec3{ 14.0f, 0.25f, 14.0f };
        f.color = dai_vec3{ 0.10f, 0.10f, 0.13f };
        inst.push_back(f);
    }

    // a row of neon bars, receding, in three colours
    const dai_vec3 cols[3] = { { 1.0f, 0.15f, 0.55f },     // magenta
                               { 0.15f, 0.95f, 1.0f },     // cyan
                               { 0.65f, 0.25f, 1.0f } };   // violet
    for (int i = 0; i < 6; ++i) {
        float z = -1.5f * i;
        float x = (i % 2 == 0) ? -3.4f : 3.4f;
        inst.push_back(neon(dai_vec3{ x, 1.6f, z }, dai_vec3{ 0.09f, 1.1f, 0.09f }, cols[i % 3]));
    }
    // a horizontal bar across the back, the brightest thing in frame
    inst.push_back(neon(dai_vec3{ 0.0f, 3.1f, -8.0f }, dai_vec3{ 4.2f, 0.10f, 0.10f },
                        dai_vec3{ 0.2f, 1.0f, 0.85f }));
    // two glowing spheres at floor level
    for (int i = 0; i < 2; ++i) {
        dai_render_instance s{};
        s.mesh = DAI_MESH_SPHERE;
        s.position = dai_vec3{ i ? 1.7f : -1.7f, 0.55f, 1.0f };
        s.rotation = dai_quat{ 0, 0, 0, 1 };
        s.scale = dai_vec3{ 0.55f, 0.55f, 0.55f };
        s.color = i ? dai_vec3{ 1.0f, 0.45f, 0.1f } : dai_vec3{ 0.3f, 1.0f, 0.4f };
        s.emissive = 1.0f;
        inst.push_back(s);
    }

    std::string off = std::string(outdir) + "/postfx-off.png";
    std::string on  = std::string(outdir) + "/postfx-on.png";

    // ---- off: exactly what the renderer produced before this feature -------
    dai_render_postfx(r, nullptr);
    dai_render_frame(r, inst.data(), (uint32_t)inst.size());
    double ms_off = dai_render_last_ms(r);
    if (dai_render_write_png(r, off.c_str()) != DAI_OK) {
        std::printf("postfx_shot: could not write %s\n", off.c_str());
        dai_render_destroy(r); return 1;
    }

    // ---- on: the whole chain, at the settings the docs quote --------------
    dai_postfx fx{};
    fx.enabled = 1;
    fx.bloom_threshold = 0.65f;
    fx.bloom_knee = 0.3f;
    fx.bloom_intensity = 1.1f;
    fx.vignette = 0.40f;
    fx.grain = 0.035f;
    fx.aberration = 0.0045f;
    fx.scanlines = 0.06f;
    // A fixed frame index, not a counter: this picture is a reference image,
    // and a reference that grains differently every run cannot be compared
    // against anything.
    fx.frame_index = 4;
    dai_render_postfx(r, &fx);
    dai_render_frame(r, inst.data(), (uint32_t)inst.size());
    double ms_on = dai_render_last_ms(r);
    if (dai_render_write_png(r, on.c_str()) != DAI_OK) {
        std::printf("postfx_shot: could not write %s\n", on.c_str());
        dai_render_destroy(r); return 1;
    }

    // Deliberately NOT printing "ms_on - ms_off" as the chain's cost: the off
    // frame is the first frame this renderer ever drew and pays for the shadow
    // map, the pipeline warmup and the first touch of every image, so the
    // subtraction comes out NEGATIVE and would be a number that is worse than
    // none. The cost is measured properly - warmed up, best of many - by
    // build/test_postfx, and docs/POSTFX.md quotes that run.
    std::printf("postfx_shot: %s and %s at %ux%u "
                "(first frames, %.2f / %.2f ms - warmup, not a benchmark; "
                "run build/test_postfx for the cost)\n",
                off.c_str(), on.c_str(), w, h, ms_off, ms_on);
    dai_render_destroy(r);
    return 0;
}
