// Daidalos - visual regression tests.
//
// A renderer that "runs" is not a renderer that is CORRECT. These tests render
// canonical scenes whose correct outcome can be computed on paper, then read
// the pixels back and check them. Every one of them caught a real bug at least
// once, and the failure message says what the picture would look like.
//
//   DAI_SHADER_DIR=shaders ./build/test_render_visual [outdir]
//
// Conventions being pinned down here:
//   right handed world, +Y up, camera looks down -Z from +Z
//   +X on screen is right, +Y on screen is up
//   front faces are the OUTSIDE of a closed mesh

#include "daidalos.h"
#include "dai_render.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

static int g_fail = 0, g_pass = 0;
static const char *g_outdir = ".";

#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

struct Frame {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    const uint8_t *at(uint32_t x, uint32_t y) const { return &px[((size_t)y * w + x) * 4]; }
    float lum(uint32_t x, uint32_t y) const {
        const uint8_t *p = at(x, y);
        return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
    }
    // which of r/g/b dominates - crude but enough to tell two test objects apart
    char hue(uint32_t x, uint32_t y) const {
        const uint8_t *p = at(x, y);
        if (p[0] < 60 && p[1] < 60 && p[2] < 60) return '.';         // background
        if (p[0] > p[1] + 25 && p[0] > p[2] + 25) return 'r';
        if (p[1] > p[0] + 25 && p[1] > p[2] + 25) return 'g';
        if (p[2] > p[0] + 25 && p[2] > p[1] + 25) return 'b';
        return '?';
    }
};

static Frame grab(dai_renderer *r) {
    Frame f;
    f.w = dai_render_width(r); f.h = dai_render_height(r);
    f.px.resize((size_t)f.w * f.h * 4);
    dai_render_readback(r, f.px.data(), f.px.size());
    return f;
}

// bounding box of every pixel that is not the background
struct Box2D { int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; int count = 0;
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    int w() const { return x1 - x0 + 1; }  int h() const { return y1 - y0 + 1; }
    float cx() const { return 0.5f * (x0 + x1); } float cy() const { return 0.5f * (y0 + y1); } };

static Box2D silhouette(const Frame &f, float thresh = 0.02f) {
    Box2D b;
    const uint8_t *bg = f.at(0, 0);
    for (uint32_t y = 0; y < f.h; ++y)
        for (uint32_t x = 0; x < f.w; ++x) {
            const uint8_t *p = f.at(x, y);
            float d = (std::abs(p[0]-bg[0]) + std::abs(p[1]-bg[1]) + std::abs(p[2]-bg[2])) / 255.0f;
            if (d > thresh) {
                if ((int)x < b.x0) b.x0 = x; if ((int)x > b.x1) b.x1 = x;
                if ((int)y < b.y0) b.y0 = y; if ((int)y > b.y1) b.y1 = y;
                ++b.count;
            }
        }
    return b;
}

static uint32_t f_w(dai_renderer *r) { return dai_render_width(r); }
static uint32_t f_h(dai_renderer *r) { return dai_render_height(r); }

static void save(dai_renderer *r, const char *name) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", g_outdir, name);
    dai_render_write_ppm(r, path);
}

static dai_render_instance box_inst(dai_vec3 p, dai_vec3 he, dai_vec3 c) {
    dai_render_instance i{};
    i.position = p; i.rotation = { 0,0,0,1 }; i.mesh = DAI_MESH_BOX; i.scale = he; i.color = c;
    return i;
}

int main(int argc, char **argv) {
    if (argc > 1) g_outdir = argv[1];
    char err[256] = {0};
    dai_render_desc rd{}; rd.width = 512; rd.height = 512;      // square: aspect bugs cannot hide
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }
    std::printf("visual tests on: %s\n", dai_render_device_name(r));
    dai_render_clear_color(r, 0.0f, 0.0f, 0.0f);
    dai_render_sky(r, 0);                                        // flat clear, easier to threshold
    // light towards the camera and a generous exposure: these tests read raw
    // pixel colours, so the test objects have to be unambiguously lit
    dai_render_light(r, dai_vec3{ 0.30f, 0.45f, 0.84f });
    dai_render_ambient(r, dai_vec3{ 0.35f, 0.40f, 0.5f }, dai_vec3{ 0.2f, 0.2f, 0.2f }, 0.35f);
    dai_render_exposure(r, 1.2f);
    dai_render_fog(r, 0.0f, dai_vec3{ 0, 0, 0 });

    // ---------------------------------------------------------------- 1
    // A unit cube head on. The projected size is exact arithmetic:
    //   half_size_ndc = he / (dist * tan(fov/2))   ->  pixels = ndc * height/2
    // A square viewport means width and height must come out EQUAL. If the
    // aspect ratio is applied to the wrong axis, this is where it shows.
    {
        std::printf("[1] cube silhouette / aspect\n");
        const float dist = 6.0f, fov = 60.0f, he = 1.0f;
        dai_render_camera(r, dai_vec3{ 0, 0, dist }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, fov, 0.1f, 100.0f);
        dai_render_instance in = box_inst({0,0,0}, {he,he,he}, {0.9f,0.2f,0.2f});
        dai_render_frame(r, &in, 1);
        Frame f = grab(r); save(r, "vis_1_cube.ppm");
        Box2D b = silhouette(f);
        float t = tanf(fov * 3.14159265f / 360.0f);
        float expect = 2.0f * (he / ((dist - he) * t)) * (f.h * 0.5f);  // near face is closer
        CHECK(b.valid(), "nothing was drawn at all");
        CHECK(std::abs(b.w() - b.h()) <= 3, "cube is not square on screen: %dx%d px (aspect ratio applied wrong)", b.w(), b.h());
        CHECK(std::abs(b.w() - expect) < 0.06f * expect, "cube is %d px wide, projection maths says %.0f px", b.w(), expect);
        CHECK(std::abs(b.cx() - f.w * 0.5f) < 3 && std::abs(b.cy() - f.h * 0.5f) < 3,
              "cube at origin is not centred: centre (%.0f,%.0f)", b.cx(), b.cy());
    }

    // ---------------------------------------------------------------- 2
    // Screen orientation. +Y must be UP in the image (row index small),
    // +X must be RIGHT. Vulkan's y-down clip space makes this a classic bug.
    {
        std::printf("[2] screen orientation (+Y up, +X right)\n");
        dai_render_camera(r, dai_vec3{ 0, 0, 10 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_instance in[2] = {
            box_inst({ 0, 2.0f, 0 }, {0.5f,0.5f,0.5f}, {0.9f,0.1f,0.1f}),   // red, up
            box_inst({ 2.0f, 0, 0 }, {0.5f,0.5f,0.5f}, {0.1f,0.3f,0.9f}),   // blue, right
        };
        dai_render_frame(r, in, 2);
        Frame f = grab(r); save(r, "vis_2_orientation.ppm");
        // find the centroid of red and of blue
        double rx=0, ry=0, bx=0, by=0; int rn=0, bn=0;
        for (uint32_t y = 0; y < f.h; ++y) for (uint32_t x = 0; x < f.w; ++x) {
            char h = f.hue(x, y);
            if (h == 'r') { rx += x; ry += y; ++rn; }
            if (h == 'b') { bx += x; by += y; ++bn; }
        }
        CHECK(rn > 100 && bn > 100, "expected a red and a blue box, got %d/%d pixels", rn, bn);
        if (rn && bn) {
            rx/=rn; ry/=rn; bx/=bn; by/=bn;
            CHECK(ry < f.h * 0.45, "the box at +Y renders in the LOWER half (y=%.0f) - image is upside down", ry);
            CHECK(bx > f.w * 0.55, "the box at +X renders on the LEFT (x=%.0f) - image is mirrored", bx);
        }
    }

    // ---------------------------------------------------------------- 3
    // Depth and occlusion. A small red box in front of a big blue wall.
    // The centre of the image MUST be red. If the depth test or the winding
    // is wrong, the wall wins and the centre turns blue.
    {
        std::printf("[3] depth test / occlusion\n");
        dai_render_camera(r, dai_vec3{ 0, 0, 10 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_instance in[2] = {
            box_inst({ 0, 0, -6 }, {6,6,0.5f}, {0.1f,0.3f,0.9f}),           // wall behind
            box_inst({ 0, 0, 0 },  {1,1,1},    {0.9f,0.1f,0.1f}),           // box in front
        };
        dai_render_frame(r, in, 2);
        Frame f = grab(r); save(r, "vis_3_depth.ppm");
        CHECK(f.hue(f.w/2, f.h/2) == 'r', "centre pixel is '%c', expected red: the near box is not occluding the far wall",
              f.hue(f.w/2, f.h/2));
        // a quarter down the frame is still inside the wall (half extent 6 at
        // 16 m covers 65% of the half height), so that must be blue
        CHECK(f.hue(f.w/2, f.h/4) == 'b', "upper part of the frame should be the blue wall, got '%c'", f.hue(f.w/2, f.h/4));
    }

    // ---------------------------------------------------------------- 4
    // Winding. Put the camera INSIDE a big box. With correct back face culling
    // every triangle of that box is a back face, so the frame stays at the
    // clear colour. NOTE: this must NOT use the corner pixel as the reference,
    // because when the bug is present the corner is covered too and the test
    // would pass while looking at a wall.
    {
        std::printf("[4] back face culling (camera inside a box)\n");
        dai_render_camera(r, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0,0,-1 }, dai_vec3{ 0,1,0 }, 60.0f, 0.05f, 100.0f);
        dai_render_instance in = box_inst({0,0,0}, {8,8,8}, {0.9f,0.9f,0.2f});
        dai_render_frame(r, &in, 1);
        Frame f = grab(r); save(r, "vis_4_inside.ppm");
        int lit = 0;
        for (uint32_t y = 0; y < f.h; ++y)
            for (uint32_t x = 0; x < f.w; ++x)
                if (f.lum(x, y) > 0.02f) ++lit;
        CHECK(lit < (int)(f.w * f.h) / 100,
              "%d px lit while inside a closed box - back faces are drawn, i.e. you are seeing "
              "the inside of every object in the scene", lit);
    }

    // ---------------------------------------------------------------- 5
    // Lighting sanity: a slab lit from straight above must be much brighter
    // seen from above than from below. The camera is deliberately off axis -
    // looking straight down the up vector degenerates any look-at matrix.
    {
        std::printf("[5] lighting / normals\n");
        dai_render_light(r, dai_vec3{ 0, 1, 0 });
        dai_render_instance in = box_inst({0,0,0}, {5,0.5f,5}, {0.6f,0.6f,0.6f});
        dai_render_camera(r, dai_vec3{ 0, 6, 4 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_frame(r, &in, 1);
        Frame f = grab(r); save(r, "vis_5_lit_top.ppm");
        float top = f.lum(f.w/2, f.h/2);
        dai_render_camera(r, dai_vec3{ 0, -6, 4 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_frame(r, &in, 1);
        Frame f2 = grab(r); save(r, "vis_5_lit_bottom.ppm");
        float bottom = f2.lum(f2.w/2, f2.h/2);
        CHECK(top > bottom + 0.1f, "lit top (%.3f) is not clearly brighter than the shadowed underside (%.3f)", top, bottom);
        CHECK(bottom > 0.01f, "the underside is pure black (%.3f) - no ambient term at all", bottom);
    }

    // ---------------------------------------------------------------- 6
    // A sphere must actually be round. Rendering every body as a box is the
    // other half of "the pictures look odd".
    {
        std::printf("[6] sphere mesh is round\n");
        dai_render_camera(r, dai_vec3{ 0, 0, 8 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_instance in{};
        in.position = {0,0,0}; in.rotation = {0,0,0,1}; in.mesh = DAI_MESH_SPHERE;
        in.scale = {2,2,2}; in.color = {0.9f,0.5f,0.2f};
        dai_render_frame(r, &in, 1);
        Frame f = grab(r); save(r, "vis_6_sphere.ppm");
        Box2D b = silhouette(f);
        CHECK(b.valid() && std::abs(b.w() - b.h()) <= 4, "sphere silhouette %dx%d px is not square", b.w(), b.h());
        // area of a disc is pi/4 of its bounding box; a box would fill ~1.0
        float fill = b.valid() ? (float)b.count / (float)(b.w() * b.h()) : 0.0f;
        CHECK(fill > 0.70f && fill < 0.83f, "silhouette fills %.2f of its bounding box (a disc is 0.785, a square is 1.0)", fill);
    }

    // ---------------------------------------------------------------- 7
    // Rotation: a cube turned 45 degrees around Z becomes a diamond, its
    // silhouette grows by sqrt(2). Catches quaternion convention mistakes.
    {
        std::printf("[7] instance rotation\n");
        dai_render_camera(r, dai_vec3{ 0, 0, 8 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_instance a = box_inst({0,0,0}, {1,1,1}, {0.8f,0.8f,0.8f});
        dai_render_frame(r, &a, 1);
        Box2D ba = silhouette(grab(r));
        dai_render_instance b = a;
        float s = sinf(3.14159265f / 8.0f), c = cosf(3.14159265f / 8.0f);   // 45 deg -> half angle 22.5
        b.rotation = { 0, 0, s, c };
        dai_render_frame(r, &b, 1);
        Frame f = grab(r); save(r, "vis_7_rotated.ppm");
        Box2D bb = silhouette(f);
        float ratio = (float)bb.w() / (float)ba.w();
        CHECK(std::abs(ratio - 1.41421f) < 0.08f, "45deg rotated cube is %.3fx wider, expected 1.414x", ratio);
    }

    // ---------------------------------------------------------------- 8
    // Many instances, and the frame must be stable: rendering the same scene
    // twice has to produce identical pixels (no uninitialised memory, no
    // frame-to-frame leakage in the offscreen target).
    {
        std::printf("[8] determinism of the frame itself\n");
        std::vector<dai_render_instance> in;
        for (int i = 0; i < 200; ++i) {
            float a = i * 0.31f;
            in.push_back(box_inst({ cosf(a) * (1.0f + i * 0.03f), sinf(a * 0.7f) * 2.0f, sinf(a) * (1.0f + i * 0.03f) },
                                  {0.3f,0.3f,0.3f}, { 0.2f + 0.003f*i, 0.6f, 0.9f - 0.003f*i }));
        }
        dai_render_camera(r, dai_vec3{ 0, 6, 16 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 200.0f);
        dai_render_frame(r, in.data(), (uint32_t)in.size());
        Frame f1 = grab(r);
        dai_render_frame(r, in.data(), (uint32_t)in.size());
        Frame f2 = grab(r); save(r, "vis_8_many.ppm");
        CHECK(f1.px == f2.px, "the same scene rendered twice produced different pixels");
        CHECK(silhouette(f1).count > 5000, "200 instances only lit %d pixels", silhouette(f1).count);
    }

    // ---------------------------------------------------------------- 9
    // The surface facing the sun must be the bright one. With inverted culling
    // you see the far side of every object, so this reads inside out - it is
    // the cheapest possible detector for that whole class of bug.
    {
        std::printf("[9] the sun lights the side facing it\n");
        dai_render_sky(r, 0);
        dai_render_instance in{};
        in = dai_render_instance_default();
        in.mesh = DAI_MESH_SPHERE; in.scale = { 2,2,2 }; in.color = { 0.6f,0.6f,0.6f };
        dai_render_camera(r, dai_vec3{ 0, 0, 8 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_light(r, dai_vec3{ 0, 0, 1 });          // straight at the camera side
        dai_render_frame(r, &in, 1);
        float front = grab(r).lum(256, 256);
        dai_render_light(r, dai_vec3{ 0, 0, -1 });         // from behind the sphere
        dai_render_frame(r, &in, 1);
        Frame f = grab(r); save(r, "vis_9_sunside.ppm");
        float back = f.lum(256, 256);
        CHECK(front > back + 0.15f,
              "sphere lit from the camera side is %.3f, lit from behind %.3f - the shading is inside out",
              front, back);
        dai_render_light(r, dai_vec3{ 0, 1, 0 });
    }

    // ---------------------------------------------------------------- 10
    // Shadows, measured by difference: render the floor alone, then the floor
    // with a sphere hovering off to one side of the frame. The sun is at an
    // angle, so the shadow lands on floor the sphere does not cover, and the
    // same pixels must get darker. Comparing two renders is far more robust
    // than guessing which pixel the shadow should hit.
    {
        std::printf("[10] shadow map\n");
        dai_render_sky(r, 0);
        dai_render_light(r, dai_vec3{ 0.5f, 0.85f, 0.0f });
        dai_render_shadow_extent(r, 14.0f);
        dai_render_camera(r, dai_vec3{ 0, 5, 14 }, dai_vec3{ 0, 1, 0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);

        dai_render_instance floor_i = dai_render_instance_default();
        floor_i.position = { 0, -1, 0 }; floor_i.scale = { 14, 1, 14 }; floor_i.color = { 0.7f,0.7f,0.7f };
        dai_render_frame(r, &floor_i, 1);
        Frame a = grab(r); save(r, "vis_10_noshadow.ppm");

        dai_render_instance in[2];
        in[0] = floor_i;
        in[1] = dai_render_instance_default();
        in[1].mesh = DAI_MESH_SPHERE; in[1].position = { 0, 6, 0 };
        in[1].scale = { 1.5f,1.5f,1.5f }; in[1].color = { 0.85f,0.3f,0.25f };
        dai_render_frame(r, in, 2);
        Frame b = grab(r); save(r, "vis_10_shadow.ppm");

        // lower half of the frame is floor in both renders
        double sa = 0, sb = 0; int n = 0;
        int darkest_x = -1, darkest_y = -1; float drop = 0.0f;
        for (uint32_t y = b.h / 2; y < b.h; ++y)   // floor only: the sphere sits high in the frame
            for (uint32_t x = 0; x < b.w; ++x) {
                float la = a.lum(x, y), lb = b.lum(x, y);
                sa += la; sb += lb; ++n;
                if (la - lb > drop) { drop = la - lb; darkest_x = x; darkest_y = y; }
            }
        float mean_a = (float)(sa / n), mean_b = (float)(sb / n);
        CHECK(drop > 0.10f, "no pixel of the floor got darker when the caster appeared (max drop %.3f) - shadow map is not working", drop);
        CHECK(mean_b < mean_a - 0.002f, "mean floor brightness %.4f -> %.4f, the shadow covers nothing", mean_a, mean_b);
        std::printf("     darkest shadow pixel at (%d,%d), %.0f%% darker\n",
                    darkest_x, darkest_y, 100.0 * drop);
    }

    // ---------------------------------------------------------------- 11
    // Capsule: one mesh, proportions from the instance. A capsule with
    // radius 0.5 and shaft half height 1.0 must be 3 units tall and 1 wide.
    {
        std::printf("[11] capsule proportions\n");
        dai_render_sky(r, 0);
        dai_render_camera(r, dai_vec3{ 0, 0, 10 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_instance in = dai_render_instance_default();
        in.mesh = DAI_MESH_CAPSULE; in.scale = { 0.5f, 0.5f, 0.5f }; in.param = 1.0f;
        in.color = { 0.3f, 0.7f, 0.9f };
        dai_render_frame(r, &in, 1);
        Frame f = grab(r); save(r, "vis_11_capsule.ppm");
        Box2D b = silhouette(f);
        float ratio = b.valid() ? (float)b.h() / (float)b.w() : 0.0f;
        CHECK(ratio > 2.7f && ratio < 3.3f,
              "capsule is %dx%d px, height/width = %.2f, expected 3.0 (param is not moving the caps)",
              b.w(), b.h(), ratio);
    }

    // ---------------------------------------------------------------- 12
    // Sky: the procedural sky must produce a real gradient (not a flat clear
    // colour) and the sun has to be visible when you look at it.
    {
        std::printf("[12] procedural sky\n");
        dai_render_sky(r, 1);
        dai_render_clear_color(r, 0, 0, 0);
        dai_render_light(r, dai_vec3{ 0, 0.35f, -1.0f });        // sun towards -Z
        dai_render_camera(r, dai_vec3{ 0, 2, 0 }, dai_vec3{ 0, 2.2f, -1 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_frame(r, nullptr, 0);
        Frame f = grab(r); save(r, "vis_12_sky.ppm");
        float zenith = f.lum(f.w/2, 10), middle = f.lum(f.w/2, f.h/2), low = f.lum(f.w/2, f.h - 10);
        CHECK(zenith > 0.05f && middle > 0.05f, "sky is black (%.3f / %.3f) - the sky pass did not run", zenith, middle);
        CHECK(fabsf(zenith - low) > 0.03f, "sky is flat (%.3f vs %.3f) - no gradient, just a clear colour", zenith, low);
        float towards = f.lum(f.w/2, (uint32_t)(f.h * 0.42f));
        dai_render_camera(r, dai_vec3{ 0, 2, 0 }, dai_vec3{ 0, 2.2f, 1 }, dai_vec3{ 0,1,0 }, 60.0f, 0.1f, 100.0f);
        dai_render_frame(r, nullptr, 0);
        float away = grab(r).lum(f.w/2, (uint32_t)(f.h * 0.42f));
        CHECK(towards > away + 0.02f, "looking at the sun (%.3f) is not brighter than looking away (%.3f)", towards, away);
        dai_render_sky(r, 0);
        dai_render_light(r, dai_vec3{ 0.30f, 0.45f, 0.84f });
    }

    // ---------------------------------------------------------------- 13
    // "The pictures look odd" as a regression test. A representative scene
    // rendered with the SHIPPING defaults has to come out like a photograph
    // rather than like grey soup: real contrast, no crushed blacks or blown
    // highlights, colour that survives the tonemapper, and visible detail.
    {
        std::printf("[13] image quality with shipping defaults\n");
        dai_render_sky(r, 1);
        dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
        dai_render_ambient(r, dai_vec3{ 0.24f, 0.40f, 0.72f }, dai_vec3{ 0.26f, 0.24f, 0.20f }, 0.38f);
        dai_render_fog(r, 0.0035f, dai_vec3{ 0.56f, 0.64f, 0.74f });
        dai_render_exposure(r, 0.58f);
        dai_render_shadow_extent(r, 20.0f);

        std::vector<dai_render_instance> in;
        dai_render_instance g = dai_render_instance_default();
        g.position = { 0, -1, 0 }; g.scale = { 40, 1, 40 };
        g.color = { 0.30f, 0.34f, 0.27f };
        g.flags = DAI_RI_CHECKER | DAI_RI_NO_SHADOW;
        in.push_back(g);
        const float col[4][3] = { {0.78f,0.42f,0.18f}, {0.30f,0.62f,0.85f}, {0.90f,0.78f,0.25f}, {0.45f,0.70f,0.35f} };
        for (int i = 0; i < 12; ++i) {
            dai_render_instance b = dai_render_instance_default();
            b.mesh = (uint32_t)(i % 4 == 0 ? DAI_MESH_BOX : i % 4 == 1 ? DAI_MESH_SPHERE :
                                i % 4 == 2 ? DAI_MESH_CYLINDER : DAI_MESH_CAPSULE);
            b.position = { -6.0f + (i % 4) * 4.0f, 1.0f + (i / 4) * 0.5f, -4.0f + (i / 4) * 4.0f };
            b.scale = { 0.8f, 0.8f, 0.8f };
            if (b.mesh == DAI_MESH_CAPSULE) b.param = 0.7f;
            b.color = { col[i % 4][0], col[i % 4][1], col[i % 4][2] };
            b.roughness = 0.4f + 0.15f * (i % 4);
            in.push_back(b);
        }
        dai_render_camera(r, dai_vec3{ 9, 6, 12 }, dai_vec3{ 0, 1, -1 }, dai_vec3{ 0,1,0 }, 52.0f, 0.1f, 300.0f);
        dai_render_frame(r, in.data(), (uint32_t)in.size());
        Frame f = grab(r); save(r, "vis_13_quality.ppm");

        std::vector<float> l;
        l.reserve((size_t)f.w * f.h);
        double sat = 0, grad = 0;
        int dark = 0, bright = 0;
        for (uint32_t y = 0; y < f.h; ++y)
            for (uint32_t x = 0; x < f.w; ++x) {
                float v = f.lum(x, y);
                l.push_back(v);
                if (v < 0.02f) ++dark;
                if (v > 0.98f) ++bright;
                const uint8_t *p = f.at(x, y);
                int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
                int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
                sat += (mx - mn) / 255.0;
                if (x + 1 < f.w) grad += fabsf(f.lum(x + 1, y) - v);
            }
        size_t n = l.size();
        std::sort(l.begin(), l.end());
        float p2 = l[n * 2 / 100], p50 = l[n / 2], p98 = l[n * 98 / 100];
        float contrast = p98 - p2;
        float meansat = (float)(sat / n), detail = (float)(grad / n);
        std::printf("     median %.3f | contrast %.3f | saturation %.3f | detail %.4f | clip %.2f%%/%.2f%%\n",
                    p50, contrast, meansat, detail, 100.0 * dark / n, 100.0 * bright / n);
        CHECK(contrast > 0.30f, "frame contrast is only %.3f (p2 %.3f, p98 %.3f) - flat, washed out image", contrast, p2, p98);
        CHECK(p50 > 0.25f && p50 < 0.75f, "median brightness %.3f is outside a sane exposure window", p50);
        CHECK(100.0 * dark / n < 3.0 && 100.0 * bright / n < 3.0,
              "clipping: %.2f%% pure black, %.2f%% pure white", 100.0 * dark / n, 100.0 * bright / n);
        CHECK(meansat > 0.06f, "mean saturation %.3f - the tonemapper is eating all the colour", meansat);
        CHECK(detail > 0.0015f, "detail measure %.4f - the frame is nearly featureless", detail);
    }

    // ---------------------------------------------------------------- 14
    // Textures and materials. A 2x2 texture drawn on a quad has to show four
    // distinct colours in the right corners (catches flipped UVs), sRGB has to
    // be decoded (a 0.5 grey sRGB texture must render darker than a linear
    // one), and metal must look different from dielectric.
    {
        std::printf("[14] textures and materials\n");
        dai_render_sky(r, 0);
        dai_render_clear_color(r, 0, 0, 0);
        dai_render_light(r, dai_vec3{ 0.0f, 0.0f, 1.0f });
        dai_render_ambient(r, dai_vec3{ 0.5f, 0.5f, 0.5f }, dai_vec3{ 0.5f, 0.5f, 0.5f }, 0.5f);
        dai_render_exposure(r, 1.0f);

        const uint8_t quad_px[16] = {
            255,0,0,255,     0,255,0,255,      // top row:    red,  green
            0,0,255,255,     255,255,0,255     // bottom row: blue, yellow
        };
        dai_texture tex = dai_render_texture_create(r, quad_px, 2, 2, 0);
        dai_material_desc md = dai_material_desc_default();
        md.base_color_tex = tex;
        md.roughness = 1.0f;
        md.name = "test_quad";
        dai_material mat = dai_render_material_create(r, &md);
        CHECK(mat != 0, "material creation failed: %s", dai_render_last_error(r));

        dai_render_instance in = dai_render_instance_default();
        in.mesh = DAI_MESH_PLANE;            // XZ quad, UV (0,0)..(1,1)
        in.scale = { 3, 1, 3 };
        in.material = mat;
        // straight above, +Z pointing down the screen
        dai_render_camera(r, dai_vec3{ 0, 8, 0.001f }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,0,-1 }, 45.0f, 0.1f, 100.0f);
        dai_render_frame(r, &in, 1);
        Frame f = grab(r); save(r, "vis_14_texture.ppm");
        // sample the four quadrants
        char q[4] = { f.hue(f.w/4, f.h/4), f.hue(f.w*3/4, f.h/4), f.hue(f.w/4, f.h*3/4), f.hue(f.w*3/4, f.h*3/4) };
        int distinct = 0;
        for (int i = 0; i < 4; ++i) { bool seen = false; for (int j = 0; j < i; ++j) if (q[j] == q[i]) seen = true; if (!seen && q[i] != '.') ++distinct; }
        CHECK(distinct >= 3, "textured quad shows %d distinct colours (%c%c%c%c) - UVs or sampling are wrong",
              distinct, q[0], q[1], q[2], q[3]);

        // sRGB vs linear: same bytes, different decode
        const uint8_t grey[4] = { 128, 128, 128, 255 };
        dai_texture t_lin = dai_render_texture_create(r, grey, 1, 1, 0);
        dai_texture t_srgb = dai_render_texture_create(r, grey, 1, 1, 1);
        dai_material_desc a = dai_material_desc_default(); a.base_color_tex = t_lin;
        dai_material_desc b = dai_material_desc_default(); b.base_color_tex = t_srgb;
        dai_material m_lin = dai_render_material_create(r, &a);
        dai_material m_srgb = dai_render_material_create(r, &b);
        dai_render_camera(r, dai_vec3{ 0, 0, 6 }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,1,0 }, 45.0f, 0.1f, 100.0f);
        in = dai_render_instance_default();
        in.scale = { 2,2,2 }; in.material = m_lin;
        dai_render_frame(r, &in, 1);
        float lin = grab(r).lum(f.w/2, f.h/2);
        in.material = m_srgb;
        dai_render_frame(r, &in, 1);
        float srgb = grab(r).lum(f.w/2, f.h/2);
        CHECK(srgb < lin - 0.05f,
              "sRGB texture (%.3f) is not darker than the same bytes read as linear (%.3f) - colour space is ignored",
              srgb, lin);

        // metal vs dielectric under the same light
        dai_material_desc mm = dai_material_desc_default(); mm.metallic = 1.0f; mm.roughness = 0.25f;
        dai_material_desc dd = dai_material_desc_default(); dd.metallic = 0.0f; dd.roughness = 0.25f;
        dai_material m_metal = dai_render_material_create(r, &mm);
        dai_material m_diel = dai_render_material_create(r, &dd);
        in = dai_render_instance_default();
        in.mesh = DAI_MESH_SPHERE; in.scale = { 2,2,2 }; in.color = { 0.8f, 0.8f, 0.8f };
        in.material = m_metal;
        dai_render_frame(r, &in, 1);
        float metal = grab(r).lum(f.w/2, (uint32_t)(f.h * 0.62f));
        in.material = m_diel;
        dai_render_frame(r, &in, 1);
        Frame fd = grab(r); save(r, "vis_14_dielectric.ppm");
        float diel = fd.lum(fd.w/2, (uint32_t)(fd.h * 0.62f));
        CHECK(fabsf(metal - diel) > 0.03f,
              "metallic (%.3f) and dielectric (%.3f) render the same - the metallic term does nothing", metal, diel);
        std::printf("     texture count %u, materials %u\n", dai_render_texture_count(r), dai_render_material_count(r));
        dai_render_exposure(r, 1.2f);
    }

    // ---------------------------------------------------------------- 15
    // Cascades. One shadow map over the whole view distance is either sharp up
    // close or present far away, never both. Two casters, one at 6 m and one at
    // 70 m, must BOTH darken the floor under them.
    {
        std::printf("[15] cascaded shadows near and far\n");
        dai_render_sky(r, 0);
        dai_render_clear_color(r, 0, 0, 0);
        dai_render_light(r, dai_vec3{ 0.35f, 0.90f, 0.25f });
        dai_render_shadow_extent(r, 60.0f);
        dai_render_exposure(r, 1.0f);
        dai_render_fog(r, 0.0f, dai_vec3{ 0,0,0 });

        dai_render_instance floor_i = dai_render_instance_default();
        floor_i.position = { 0, -1, -40 }; floor_i.scale = { 40, 1, 90 }; floor_i.color = { 0.7f,0.7f,0.7f };

        dai_render_camera(r, dai_vec3{ 0, 6, 12 }, dai_vec3{ 0, 0, -40 }, dai_vec3{ 0,1,0 }, 55.0f, 0.1f, 400.0f);
        dai_render_frame(r, &floor_i, 1);
        Frame empty = grab(r);

        dai_render_instance in[3];
        in[0] = floor_i;
        in[1] = dai_render_instance_default();
        in[1].mesh = DAI_MESH_SPHERE; in[1].position = { 0, 3, -6 }; in[1].scale = { 1.5f,1.5f,1.5f };
        in[1].color = { 0.85f,0.3f,0.25f };
        in[2] = dai_render_instance_default();
        in[2].mesh = DAI_MESH_SPHERE; in[2].position = { 0, 6, -70 }; in[2].scale = { 3.0f,3.0f,3.0f };
        in[2].color = { 0.3f,0.5f,0.85f };
        dai_render_frame(r, in, 3);
        Frame lit = grab(r); save(r, "vis_15_cascades.ppm");

        // near shadow lands low in the frame, far shadow high up
        int near_drop = 0, far_drop = 0;
        for (uint32_t y = 0; y < lit.h; ++y)
            for (uint32_t x = 0; x < lit.w; ++x) {
                float d = empty.lum(x, y) - lit.lum(x, y);
                if (d > 0.08f) { if (y > lit.h * 3 / 5) ++near_drop; else ++far_drop; }
            }
        CHECK(near_drop > 200, "the caster 6 m away casts no shadow (%d px darkened)", near_drop);
        CHECK(far_drop > 100, "the caster 70 m away casts no shadow (%d px darkened) - only one cascade is working", far_drop);
        std::printf("     darkened pixels: near %d, far %d\n", near_drop, far_drop);
        dai_render_exposure(r, 1.2f);
    }

    // ---------------------------------------------------------------- 16
    // Punctual lights: a point light must brighten what is near it and leave
    // what is far alone, a spot must stay inside its cone, and both must be
    // dark behind an object rather than shining through it.
    {
        std::printf("[16] point and spot lights\n");
        dai_render_sky(r, 0);
        dai_render_clear_color(r, 0, 0, 0);
        dai_render_sun(r, dai_vec3{ 0, 1, 0 }, dai_vec3{ 1,1,1 }, 0.0f);      // sun off
        dai_render_ambient(r, dai_vec3{ 0.02f,0.02f,0.03f }, dai_vec3{ 0.02f,0.02f,0.02f }, 0.05f);
        dai_render_exposure(r, 1.0f);
        dai_render_fog(r, 0.0f, dai_vec3{0,0,0});

        dai_render_instance floor_i = dai_render_instance_default();
        floor_i.position = { 0, -1, 0 }; floor_i.scale = { 20, 1, 20 };
        floor_i.color = { 0.8f, 0.8f, 0.8f };
        dai_render_camera(r, dai_vec3{ 0, 9, 9 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0,1,0 }, 55.0f, 0.1f, 100.0f);

        dai_render_lights(r, nullptr, 0);
        dai_render_frame(r, &floor_i, 1);
        Frame dark = grab(r);
        float unlit = dark.lum(f_w(r) / 2, f_h(r) / 2);

        dai_light lights[2];
        lights[0] = dai_light_point(dai_vec3{ 0, 1.5f, 0 }, dai_vec3{ 1.0f, 0.9f, 0.7f }, 3.0f, 6.0f);
        dai_render_lights(r, lights, 1);
        dai_render_frame(r, &floor_i, 1);
        Frame lit = grab(r); save(r, "vis_16_point.ppm");
        float centre = lit.lum(lit.w / 2, lit.h / 2);
        float corner = lit.lum(20, lit.h - 20);
        std::printf("     unlit %.3f | under the light %.3f | far corner %.3f\n", unlit, centre, corner);
        CHECK(centre > unlit + 0.1f, "point light did not brighten the floor (%.3f vs %.3f)", centre, unlit);
        CHECK(centre > corner + 0.1f, "point light reaches as far as the frame corner (%.3f vs %.3f) - no falloff",
              centre, corner);

        // spot: aimed straight down, narrow cone
        lights[0] = dai_light_spot(dai_vec3{ 0, 4.0f, 0 }, dai_vec3{ 0, -1, 0 },
                                   dai_vec3{ 0.6f, 0.8f, 1.0f }, 40.0f, 12.0f, 10.0f, 18.0f);
        dai_render_lights(r, lights, 1);
        dai_render_frame(r, &floor_i, 1);
        Frame spot = grab(r); save(r, "vis_16_spot.ppm");
        float in_cone = spot.lum(spot.w / 2, spot.h / 2);
        float out_cone = spot.lum(spot.w / 6, spot.h / 2);
        std::printf("     spot: inside %.3f | outside %.3f\n", in_cone, out_cone);
        CHECK(in_cone > out_cone + 0.15f, "spot cone does not fall off (%.3f inside, %.3f outside)",
              in_cone, out_cone);

        dai_render_lights(r, nullptr, 0);
        dai_render_sun(r, dai_vec3{ 0.42f, 0.80f, 0.42f }, dai_vec3{ 1.0f, 0.95f, 0.86f }, 1.3f);
    }

    // ---------------------------------------------------------------- 17
    // Frustum culling: turning the camera away must drop the instances, and
    // the visible image must be identical with culling on and off.
    {
        std::printf("[17] frustum culling\n");
        std::vector<dai_render_instance> in;
        for (int i = 0; i < 400; ++i) {
            dai_render_instance b = dai_render_instance_default();
            float a = i * 0.157f;
            b.position = { cosf(a) * (2.0f + i * 0.1f), 1.0f, sinf(a) * (2.0f + i * 0.1f) };
            b.scale = { 0.4f, 0.4f, 0.4f };
            in.push_back(b);
        }
        dai_render_camera(r, dai_vec3{ 0, 3, 6 }, dai_vec3{ 0, 1, -20 }, dai_vec3{ 0,1,0 }, 45.0f, 0.1f, 200.0f);
        dai_render_culling(r, 1);
        dai_render_frame(r, in.data(), (uint32_t)in.size());
        uint32_t culled_forward = dai_render_last_culled(r);
        Frame culled_img = grab(r);

        dai_render_culling(r, 0);
        dai_render_frame(r, in.data(), (uint32_t)in.size());
        Frame full_img = grab(r);
        CHECK(culled_forward > 50, "only %u of 400 instances were culled looking forward", culled_forward);
        CHECK(culled_img.px == full_img.px, "culling changed the image - something visible was dropped");

        dai_render_culling(r, 1);
        dai_render_camera(r, dai_vec3{ 0, 3, 6 }, dai_vec3{ 0, 3, 60 }, dai_vec3{ 0,1,0 }, 45.0f, 0.1f, 200.0f);
        dai_render_frame(r, in.data(), (uint32_t)in.size());
        std::printf("     looking at the scene: %u culled | looking away: %u culled of 400\n",
                    culled_forward, dai_render_last_culled(r));
        CHECK(dai_render_last_culled(r) > 350, "looking away only culled %u of 400", dai_render_last_culled(r));
    }


    // UV transform: tiling per axis, scrolling, and the instance overriding the
    // material. The old model was a single uv_scale float, which could not tile
    // 4x1 (a conveyor belt) and could not scroll at all. These checks pin the
    // three rules: instance tiling wins when set, offsets ADD, and a zero
    // initialised instance changes nothing.
    {
        std::printf("[18] uv tiling, scrolling and per instance override\n");
        dai_render_sky(r, 0);
        dai_render_clear_color(r, 0, 0, 0);
        dai_render_light(r, dai_vec3{ 0.0f, 0.0f, 1.0f });
        dai_render_ambient(r, dai_vec3{ 0.5f, 0.5f, 0.5f }, dai_vec3{ 0.5f, 0.5f, 0.5f }, 0.5f);
        dai_render_exposure(r, 1.0f);
        dai_render_camera(r, dai_vec3{ 0, 8, 0.001f }, dai_vec3{ 0,0,0 }, dai_vec3{ 0,0,-1 }, 45.0f, 0.1f, 100.0f);

        const uint8_t px4[16] = {
            255,0,0,255,     0,255,0,255,
            0,0,255,255,     255,255,0,255
        };
        dai_texture tex = dai_render_texture_create(r, px4, 2, 2, 0);

        // colour changes along the middle scanline, counted only where the
        // plane actually is - a proxy for "how many times does the texture
        // repeat across the quad"
        auto stripes = [](const Frame &f) {
            uint32_t y = f.h / 2, n = 0;
            char prev = f.hue(f.w / 4, y);
            for (uint32_t x = f.w / 4; x < f.w * 3 / 4; ++x) {
                char h = f.hue(x, y);
                if (h != '.' && prev != '.' && h != prev) ++n;
                prev = h;
            }
            return n;
        };

        dai_material_desc md = dai_material_desc_default();
        md.base_color_tex = tex;
        md.name = "uv_test";
        dai_material mat = dai_render_material_create(r, &md);
        CHECK(mat != 0, "material creation failed: %s", dai_render_last_error(r));

        dai_render_instance in = dai_render_instance_default();
        in.mesh = DAI_MESH_PLANE;
        in.scale = { 3, 1, 3 };
        in.material = mat;

        dai_render_frame(r, &in, 1);
        Frame base = grab(r); save(r, "vis_18_uv_1x1.ppm");
        uint32_t s_base = stripes(base);

        // ---- material tiling, per axis ----
        md.uv_scale = { 4, 1 };
        CHECK(dai_render_material_update(r, mat, &md) == DAI_OK, "material_update failed");
        dai_render_frame(r, &in, 1);
        Frame tiled = grab(r); save(r, "vis_18_uv_4x1.ppm");
        uint32_t s_tiled = stripes(tiled);
        CHECK(s_tiled > s_base + 1, "4x1 tiling gave %u stripes, 1x1 gave %u - uv_scale.x does nothing",
              s_tiled, s_base);

        // the Y axis must be independent, or this is still a single float
        md.uv_scale = { 1, 4 };
        dai_render_material_update(r, mat, &md);
        dai_render_frame(r, &in, 1);
        Frame tiled_y = grab(r);
        CHECK(stripes(tiled_y) <= s_base + 1,
              "tiling 1x4 changed the horizontal stripe count to %u (1x1: %u) - the axes are coupled",
              stripes(tiled_y), s_base);
        CHECK(tiled_y.px != tiled.px, "tiling 4x1 and 1x4 render identically - one axis is ignored");

        // ---- scrolling ----
        md.uv_scale = { 1, 1 };
        md.uv_offset = { 0.5f, 0.0f };
        dai_render_material_update(r, mat, &md);
        dai_render_frame(r, &in, 1);
        Frame scrolled = grab(r); save(r, "vis_18_uv_scroll.ppm");
        CHECK(scrolled.px != base.px, "a 0.5 uv_offset changed nothing - scrolling is not applied");
        CHECK(stripes(scrolled) <= s_base + 1,
              "scrolling changed the stripe count (%u vs %u) - offset is being scaled, not added",
              stripes(scrolled), s_base);

        // half a texture across a 2x2 texture swaps the columns: what was on
        // the left is now on the right
        CHECK(base.hue(base.w/4, base.h/2) == scrolled.hue(scrolled.w*3/4, scrolled.h/2),
              "scrolling by 0.5 did not move the left half to the right (%c -> %c)",
              base.hue(base.w/4, base.h/2), scrolled.hue(scrolled.w*3/4, scrolled.h/2));

        // ---- instance overrides material tiling ----
        md.uv_scale = { 1, 1 };
        md.uv_offset = { 0, 0 };
        dai_render_material_update(r, mat, &md);
        dai_render_instance ov = in;
        ov.uv_scale = { 4, 1 };
        dai_render_frame(r, &ov, 1);
        Frame inst_tiled = grab(r);
        CHECK(inst_tiled.px == tiled.px,
              "instance uv_scale 4x1 does not match material uv_scale 4x1 - the override path differs");

        // ---- a zero initialised instance must not change anything ----
        dai_render_instance zero = in;
        zero.uv_scale = { 0, 0 };
        zero.uv_offset = { 0, 0 };
        dai_render_frame(r, &zero, 1);
        CHECK(grab(r).px == base.px,
              "an instance with uv_scale 0,0 changed the image - the fallback to the material is broken");

        // ---- offsets add: material 0.25 + instance 0.25 == material 0.5 ----
        md.uv_offset = { 0.25f, 0.0f };
        dai_render_material_update(r, mat, &md);
        dai_render_instance half = in;
        half.uv_offset = { 0.25f, 0.0f };
        dai_render_frame(r, &half, 1);
        Frame added = grab(r);
        CHECK(added.px == scrolled.px,
              "material 0.25 + instance 0.25 is not the same as material 0.5 - offsets do not add");

        // ---- two instances, one material, different phase ----
        md.uv_offset = { 0, 0 };
        dai_render_material_update(r, mat, &md);
        dai_render_instance pair[2];
        pair[0] = in; pair[0].position = { -1.8f, 0, 0 }; pair[0].scale = { 1.5f, 1, 1.5f };
        pair[1] = in; pair[1].position = {  1.8f, 0, 0 }; pair[1].scale = { 1.5f, 1, 1.5f };
        dai_render_frame(r, pair, 2);
        Frame same_phase = grab(r);

        pair[1].uv_offset = { 0.5f, 0.0f };
        dai_render_frame(r, pair, 2);
        Frame two = grab(r); save(r, "vis_18_uv_two_phases.ppm");

        CHECK(two.px != same_phase.px,
              "offsetting one of two instances changed nothing - the offset is not read per instance");
        // The decisive part: the OTHER instance must be untouched. Comparing the
        // left half of the image is independent of where exactly the quads land,
        // which a hue sample on a texel boundary is not.
        bool left_untouched = true;
        for (uint32_t y = 0; y < two.h && left_untouched; ++y)
            for (uint32_t x = 0; x < two.w / 2; ++x)
                if (std::memcmp(two.at(x, y), same_phase.at(x, y), 4) != 0) { left_untouched = false; break; }
        CHECK(left_untouched,
              "scrolling the right instance also changed the left one - the offset leaked across instances");

        uint32_t before = dai_render_material_count(r);
        for (int i = 0; i < 32; ++i) {
            md.uv_offset = { i * 0.03f, 0.0f };
            dai_render_material_update(r, mat, &md);
        }
        CHECK(dai_render_material_count(r) == before,
              "32 material updates created %u new materials - animating a material leaks",
              dai_render_material_count(r) - before);
        std::printf("     stripes 1x1: %u | 4x1: %u | materials still %u\n",
                    s_base, s_tiled, dai_render_material_count(r));
    }

    // ---------------------------------------------------------------- 19
    // Shadow acne. A surface that nothing shadows must come out EVEN. When the
    // depth bias is too small for the angle, the shadow map shadows the very
    // surface that wrote it, in stripes - which is what a wall lit at a
    // glancing angle looked like: dark diagonal bands across a flat face.
    //
    // Measured as banding, not as brightness: walk each scanline across the
    // face and count how often it swings dark and light again. A clean face
    // does that never; an acned one does it every few pixels.
    {
        // HONEST NOTE: this test is a guard, not the thing that caught the bug.
        // Justin saw dark bands across a floating crate on his machine; every
        // scene built here to reproduce it came out perfectly even, with the
        // old shader as well as the new one. So it stands as a regression
        // guard - if a future change makes a flat, unshadowed face stripe, this
        // fails - and the actual fix (normal offset bias + 5x5 PCF) was made
        // because the shader was missing a standard mechanism, not because
        // this number moved.
        std::printf("[19] shadow acne on a glancing surface\n");
        dai_render_sky(r, 0);
        dai_render_ambient(r, dai_vec3{ 0.2f, 0.3f, 0.5f }, dai_vec3{ 0.2f, 0.2f, 0.2f }, 0.25f);
        // Sun almost along the wall: ndl is small, which is exactly where the
        // constant part of the bias stops being enough.
        // Exactly the sun the editor ships with, and a face turned 65 degrees
        // away from it - the angle a crate's front face has in a normal scene.
        dai_render_light(r, dai_vec3{ 0.42f, 0.80f, 0.42f });
        // A far cascade, because that is where a shadow texel covers a metre of
        // wall and the constant bias runs out. Testing this at three metres
        // with a tight extent proves nothing - it was green before the fix.
        dai_render_shadow_extent(r, 70.0f);
        dai_render_camera(r, dai_vec3{ 0, 6.0f, 26.0f }, dai_vec3{ 0, 2.0f, 0 }, dai_vec3{ 0,1,0 },
                          55.0f, 0.1f, 300.0f);

        dai_render_instance in[3];
        in[0] = dai_render_instance_default();
        in[0].position = { 0, -1, 0 }; in[0].scale = { 40, 1, 40 }; in[0].color = { 0.6f,0.6f,0.6f };
        in[1] = dai_render_instance_default();      // the wall we look at
        in[1].position = { 0, 2.0f, 0 }; in[1].scale = { 3.0f, 3.0f, 3.0f };
        in[1].color = { 0.30f, 0.72f, 0.70f };
        in[2] = dai_render_instance_default();      // a caster, so the map is busy
        in[2].position = { 6.0f, 2.6f, 3.0f }; in[2].scale = { 1.2f, 1.2f, 1.2f };
        in[2].color = { 0.88f, 0.72f, 0.28f };
        dai_render_frame(r, in, 3);
        Frame f = grab(r); save(r, "vis_19_acne.ppm");

        // The face fills the middle of the frame. Sample well inside it so no
        // silhouette edge is counted as a swing.
        uint32_t x0 = f.w * 38 / 100, x1 = f.w * 62 / 100;
        uint32_t y0 = f.h * 40 / 100, y1 = f.h * 58 / 100;
        int worst_line = 0, total = 0;
        float lo = 1.0f, hi = 0.0f;
        for (uint32_t y = y0; y < y1; ++y) {
            int swings = 0;
            bool dark = false;
            float ref = f.lum(x0, y);
            for (uint32_t x = x0; x < x1; ++x) {
                float l = f.lum(x, y);
                if (l < lo) lo = l;
                if (l > hi) hi = l;
                if (!dark && l < ref * 0.80f) { dark = true; ++swings; }
                else if (dark && l > ref * 0.92f) dark = false;
                ref = ref * 0.9f + l * 0.1f;
            }
            total += swings;
            if (swings > worst_line) worst_line = swings;
        }
        std::printf("     face luminance %.3f..%.3f, %d dark bands (worst line %d)\n",
                    lo, hi, total, worst_line);
        CHECK(hi > 0.05f, "the test face is not lit at all (max %.3f) - nothing was measured", hi);
        CHECK(worst_line <= 1, "%d dark bands on one scanline of a flat, unshadowed face - "
              "the surface is shadowing itself", worst_line);
        CHECK(total <= 12, "%d dark bands over the face - shadow acne", total);
        dai_render_ambient(r, dai_vec3{ 0.2f, 0.3f, 0.5f }, dai_vec3{ 0.2f, 0.2f, 0.2f }, 0.35f);
    }

    dai_render_destroy(r);
    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
