// Separates the three things that can make a face come out the wrong
// brightness: the normal, the shadow map, and the lighting maths.
//   DAI_SHADER_DIR=shaders ./build/diag_shading

#include "dai_render.h"
#include <cstdio>
#include <vector>

static float centre_lum(dai_renderer *r) {
    uint32_t w = dai_render_width(r), h = dai_render_height(r);
    std::vector<uint8_t> px((size_t)w * h * 4);
    dai_render_readback(r, px.data(), px.size());
    const uint8_t *p = &px[(((size_t)h / 2) * w + w / 2) * 4];
    return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
}

static void run(int shadows) {
    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 256; rd.height = 256; rd.msaa = 1;
    rd.shadow_size = shadows ? 1024 : -1;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("no renderer: %s\n", err); return; }
    dai_render_sky(r, 0);
    dai_render_clear_color(r, 0, 0, 0);
    dai_render_fog(r, 0.0f, dai_vec3{0,0,0});
    dai_render_light(r, dai_vec3{ 0, 1, 0 });

    dai_render_instance in = dai_render_instance_default();
    in.scale = { 5, 0.5f, 5 };
    in.color = { 0.6f, 0.6f, 0.6f };

    dai_render_camera(r, dai_vec3{ 0, 6, 4 }, dai_vec3{0,0,0}, dai_vec3{0,1,0}, 60, 0.1f, 100);
    dai_render_frame(r, &in, 1);
    float top = centre_lum(r);
    dai_render_camera(r, dai_vec3{ 0, -6, 4 }, dai_vec3{0,0,0}, dai_vec3{0,1,0}, 60, 0.1f, 100);
    dai_render_frame(r, &in, 1);
    float bottom = centre_lum(r);

    // a lone sphere cannot shadow itself on the side facing the sun
    dai_render_instance s = dai_render_instance_default();
    s.mesh = DAI_MESH_SPHERE; s.scale = { 2,2,2 }; s.color = { 0.6f,0.6f,0.6f };
    dai_render_camera(r, dai_vec3{ 0, 6, 4 }, dai_vec3{0,0,0}, dai_vec3{0,1,0}, 60, 0.1f, 100);
    dai_render_frame(r, &s, 1);
    float sphere_top = centre_lum(r);

    std::printf("shadows=%d  slab from above %.3f | slab from below %.3f | sphere from above %.3f\n",
                shadows, top, bottom, sphere_top);
    dai_render_destroy(r);
}

int main() { run(0); run(1); return 0; }
