// Isolates "is the projection right" from "is the mesh right": renders a
// handful of shapes whose on screen size can be computed exactly, and prints
// measured vs expected. Used while chasing a 15% size error - kept because
// this is the fastest way to re-check the camera maths after any change.
//
//   DAI_SHADER_DIR=shaders ./build/diag_projection

#include "dai_render.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int measure_width(dai_renderer *r, std::vector<uint8_t> &px) {
    uint32_t w = dai_render_width(r), h = dai_render_height(r);
    px.resize((size_t)w * h * 4);
    dai_render_readback(r, px.data(), px.size());
    int x0 = 1 << 30, x1 = -1;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t *p = &px[((size_t)y * w + x) * 4];
            if (p[0] + p[1] + p[2] > 12) { if ((int)x < x0) x0 = x; if ((int)x > x1) x1 = x; }
        }
    return x1 >= x0 ? x1 - x0 + 1 : 0;
}

int main() {
    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 512; rd.height = 512; rd.msaa = 1; rd.shadow_size = -1;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("no renderer: %s\n", err); return 77; }
    dai_render_sky(r, 0);
    dai_render_clear_color(r, 0, 0, 0);
    dai_render_fog(r, 0.0f, dai_vec3{0,0,0});
    dai_render_ambient(r, dai_vec3{1,1,1}, dai_vec3{1,1,1}, 1.0f);
    dai_render_light(r, dai_vec3{0,0,1});

    std::vector<uint8_t> px;
    const float PI = 3.14159265f;

    struct Case { const char *what; uint32_t mesh; float scale; float dist; float fov; };
    Case cases[] = {
        { "box  s=1 d=6  fov60", DAI_MESH_BOX,    1.0f, 6.0f, 60.0f },
        { "box  s=2 d=6  fov60", DAI_MESH_BOX,    2.0f, 6.0f, 60.0f },
        { "box  s=1 d=12 fov60", DAI_MESH_BOX,    1.0f, 12.0f, 60.0f },
        { "box  s=1 d=6  fov30", DAI_MESH_BOX,    1.0f, 6.0f, 30.0f },
        { "sph  s=1 d=6  fov60", DAI_MESH_SPHERE, 1.0f, 6.0f, 60.0f },
        { "sph  s=2 d=8  fov60", DAI_MESH_SPHERE, 2.0f, 8.0f, 60.0f },
    };

    for (const Case &c : cases) {
        dai_render_instance in = dai_render_instance_default();
        in.mesh = c.mesh;
        in.scale = { c.scale, c.scale, c.scale };
        in.color = { 1, 1, 1 };
        dai_render_camera(r, dai_vec3{ 0, 0, c.dist }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                          c.fov, 0.05f, 100.0f);
        dai_render_frame(r, &in, 1);
        int got = measure_width(r, px);

        float t = tanf(c.fov * PI / 360.0f);
        float expect;
        if (c.mesh == DAI_MESH_BOX) {
            // widest silhouette edge: the front face, at distance dist - scale
            expect = 2.0f * (c.scale / ((c.dist - c.scale) * t)) * 256.0f;
        } else {
            // tangent cone of a sphere
            float th = asinf(c.scale / c.dist);
            expect = 2.0f * (tanf(th) / t) * 256.0f;
        }
        std::printf("%-22s measured %4d px   expected %6.1f px   ratio %.4f\n",
                    c.what, got, expect, got / expect);
    }
    dai_render_destroy(r);
    return 0;
}
