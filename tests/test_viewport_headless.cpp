// The viewport's ARITHMETIC - the half of tests/test_viewport.cpp that never
// needed a swapchain.
//
//   ./build/test_viewport_headless
//
// test_viewport opens a Vulkan device in its first ten lines to prove that
// dai_render_world_clip keeps the world inside the Scene panel, and because of
// that it is on run_tests.sh's "needs GPU" list and its second half - picking,
// projection and the viewport offset - is never run either. That half is pure
// arithmetic on a camera, it is what every gizmo, every click and every wire
// overlay in the editor stands on, and it is exactly the kind of thing that
// rots unwatched: the offset bug it guards against (a click at the panel's
// (10,10) being read as the window's) shipped twice.
//
// So the offset is checked here, in one place, over a GRID of points and three
// camera setups rather than at the single centre pixel, plus the two round
// trips that make picking meaningful:
//
//   project(ray(p))   == p      for every pixel inside the rectangle
//   ray(project(P))   hits P    for every world point in front of the camera
//
// No renderer, no window, no display.

#include "dai_editor.h"
#include "dai_doc.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static float len3(dai_vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

int main() {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 2;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_editor *ed = dai_editor_create(doc, sync);

    // Three ways a Scene panel can sit in a window: dead centre of a big one,
    // pushed right and down by a dock, and the whole window with no dock at
    // all. The third is the case that hides the bug - with a rectangle at the
    // origin, forgetting the offset costs nothing.
    struct View { const char *name; float x, y, w, h, fov; };
    const View views[3] = {
        { "docked",     300.0f, 100.0f,  800.0f, 600.0f, 60.0f },
        { "narrow",     184.0f,  60.0f,  620.0f, 430.0f, 48.0f },
        { "fullscreen",   0.0f,   0.0f, 1600.0f, 900.0f, 70.0f },
    };
    const dai_vec3 up{ 0, 1, 0 };
    struct Cam { dai_vec3 eye, target; };
    const Cam cams[3] = {
        { { 0, 0, 10 },      { 0, 0, 0 } },
        { { 6.2f, 4.1f, 8.8f }, { 0.5f, 1.0f, -0.4f } },
        { { -3.0f, 12.0f, 0.2f }, { 0.0f, 0.0f, 0.0f } },
    };

    for (int vi = 0; vi < 3; ++vi) {
        const View &v = views[vi];
        for (int ci = 0; ci < 3; ++ci) {
            const Cam &c = cams[ci];
            dai_editor_camera(ed, c.eye, c.target, up, v.fov, 0.05f, 500.0f, v.w, v.h);
            dai_editor_camera_viewport_rect(ed, v.x, v.y, v.w, v.h);

            // What the camera says it is, after being told twice.
            dai_vec3 ge{}, gt{};
            float gf = 0.0f;
            dai_editor_camera_get(ed, &ge, &gt, &gf);
            CHECK(std::fabs(ge.x - c.eye.x) + std::fabs(ge.y - c.eye.y) +
                  std::fabs(ge.z - c.eye.z) < 1e-4f,
                  "[%s/%d] the camera forgot its eye: (%.2f %.2f %.2f)",
                  v.name, ci, (double)ge.x, (double)ge.y, (double)ge.z);
            CHECK(std::fabs(gf - v.fov) < 1e-3f,
                  "[%s/%d] the camera's fov came back as %.2f, was given %.2f",
                  v.name, ci, (double)gf, (double)v.fov);

            // ---- the centre of the RECTANGLE is the camera axis -------------
            // Not the centre of the window: the target must land on the middle
            // of the panel the scene is drawn in, wherever that panel is.
            float sx = 0, sy = 0;
            CHECK(dai_editor_project(ed, c.target, &sx, &sy) == 1,
                  "[%s/%d] the camera's own target projects behind it", v.name, ci);
            CHECK(std::fabs(sx - (v.x + v.w * 0.5f)) < 1.0f &&
                  std::fabs(sy - (v.y + v.h * 0.5f)) < 1.0f,
                  "[%s/%d] the target projects to (%.1f, %.1f), the panel's centre is (%.1f, %.1f)",
                  v.name, ci, (double)sx, (double)sy,
                  (double)(v.x + v.w * 0.5f), (double)(v.y + v.h * 0.5f));

            // ---- ray -> project, over the whole rectangle -------------------
            // A ray is shot through a pixel and a point is put on it; the point
            // has to project back onto the pixel it came from. That is picking
            // and gizmo drawing agreeing with each other, and they are two
            // different code paths - which is why this is worth asking at the
            // corners and not only in the middle.
            float worst = 0.0f;
            int    behind = 0, off = 0;
            for (int gy = 0; gy <= 4; ++gy)
                for (int gx = 0; gx <= 4; ++gx) {
                    float px = v.x + v.w * (0.06f + 0.22f * (float)gx);
                    float py = v.y + v.h * (0.06f + 0.22f * (float)gy);
                    dai_vec3 o{}, d{};
                    dai_editor_ray(ed, px, py, &o, &d);
                    float dl = len3(d);
                    if (std::fabs(dl - 1.0f) > 1e-3f) ++off;
                    dai_vec3 hit{ o.x + d.x * 7.5f, o.y + d.y * 7.5f, o.z + d.z * 7.5f };
                    float bx = 0, by = 0;
                    if (!dai_editor_project(ed, hit, &bx, &by)) { ++behind; continue; }
                    float e = std::fabs(bx - px) + std::fabs(by - py);
                    if (e > worst) worst = e;
                }
            CHECK(off == 0, "[%s/%d] %d of 25 picking rays are not unit length", v.name, ci, off);
            CHECK(behind == 0, "[%s/%d] %d of 25 points on a picking ray projected as behind the camera",
                  v.name, ci, behind);
            CHECK(worst < 0.75f,
                  "[%s/%d] a point on the ray through a pixel projects %.2f px away from that pixel",
                  v.name, ci, (double)worst);

            // ---- the ray through the panel's centre looks at the target -----
            {
                dai_vec3 o{}, d{};
                dai_editor_ray(ed, v.x + v.w * 0.5f, v.y + v.h * 0.5f, &o, &d);
                dai_vec3 f{ c.target.x - c.eye.x, c.target.y - c.eye.y, c.target.z - c.eye.z };
                float fl = len3(f);
                float dot = (d.x * f.x + d.y * f.y + d.z * f.z) / (fl > 0 ? fl : 1.0f);
                CHECK(dot > 0.9995f,
                      "[%s/%d] the ray through the panel centre is %.4f off the view direction",
                      v.name, ci, (double)dot);
                CHECK(std::fabs(o.x - c.eye.x) + std::fabs(o.y - c.eye.y) +
                      std::fabs(o.z - c.eye.z) < 1e-3f,
                      "[%s/%d] the picking ray does not start at the eye", v.name, ci);
            }

            // ---- a click OUTSIDE the panel is outside the frustum -----------
            // The dock puts other panels around the scene; a ray shot through
            // one of them must not come back as if it were inside.
            {
                dai_vec3 o{}, d{};
                dai_editor_ray(ed, v.x - 30.0f, v.y + v.h * 0.5f, &o, &d);
                float ox = 0, oy = 0;
                dai_vec3 hit{ o.x + d.x * 7.5f, o.y + d.y * 7.5f, o.z + d.z * 7.5f };
                dai_editor_project(ed, hit, &ox, &oy);
                CHECK(ox < v.x + 1.0f,
                      "[%s/%d] a click 30 px left of the panel came back at x=%.1f, inside it",
                      v.name, ci, (double)ox);
            }

            // ---- project -> ray, the other way round ------------------------
            // Eight world points around the target: each one projects to a
            // pixel, and the ray through that pixel has to pass through the
            // point again. Distance from the ray, not from the eye - a camera
            // that is off by a scale factor still puts the point on the line.
            {
                const float R = 2.4f;
                float worst_d = 0.0f;
                int   missed = 0;
                for (int k = 0; k < 8; ++k) {
                    float a = (float)k * 0.7853981634f;
                    dai_vec3 P{ c.target.x + std::cos(a) * R,
                                c.target.y + std::sin(a) * R * 0.6f,
                                c.target.z + std::sin(a) * R };
                    float qx = 0, qy = 0;
                    if (!dai_editor_project(ed, P, &qx, &qy)) { ++missed; continue; }
                    dai_vec3 o{}, d{};
                    dai_editor_ray(ed, qx, qy, &o, &d);
                    dai_vec3 rel{ P.x - o.x, P.y - o.y, P.z - o.z };
                    float t = rel.x * d.x + rel.y * d.y + rel.z * d.z;
                    dai_vec3 perp{ rel.x - d.x * t, rel.y - d.y * t, rel.z - d.z * t };
                    float dist = len3(perp);
                    if (dist > worst_d) worst_d = dist;
                }
                CHECK(missed == 0, "[%s/%d] %d of 8 points around the target projected as behind",
                      v.name, ci, missed);
                CHECK(worst_d < 0.02f,
                      "[%s/%d] the ray through a point's own pixel misses it by %.4f m",
                      v.name, ci, (double)worst_d);
            }

            // ---- a point behind the camera is reported as behind ------------
            {
                dai_vec3 f{ c.target.x - c.eye.x, c.target.y - c.eye.y, c.target.z - c.eye.z };
                float fl = len3(f);
                dai_vec3 back{ c.eye.x - f.x / fl * 5.0f, c.eye.y - f.y / fl * 5.0f,
                               c.eye.z - f.z / fl * 5.0f };
                float bx = 0, by = 0;
                CHECK(dai_editor_project(ed, back, &bx, &by) == 0,
                      "[%s/%d] a point five metres behind the camera projected onto the screen",
                      v.name, ci);
            }

            // ---- a segment that crosses the near plane is still drawable ----
            // dai_editor_project_seg clips instead of refusing, which is what
            // keeps a wireframe drawn while the camera flies through it.
            {
                dai_vec3 f{ c.target.x - c.eye.x, c.target.y - c.eye.y, c.target.z - c.eye.z };
                float fl = len3(f);
                dai_vec3 fwd{ f.x / fl, f.y / fl, f.z / fl };
                dai_vec3 a{ c.eye.x - fwd.x * 3.0f, c.eye.y - fwd.y * 3.0f, c.eye.z - fwd.z * 3.0f };
                dai_vec3 b{ c.eye.x + fwd.x * 9.0f, c.eye.y + fwd.y * 9.0f, c.eye.z + fwd.z * 9.0f };
                float ax = 0, ay = 0, bx = 0, by = 0;
                CHECK(dai_editor_project_seg(ed, a, b, &ax, &ay, &bx, &by) == 1,
                      "[%s/%d] a segment through the camera was dropped instead of clipped",
                      v.name, ci);
                CHECK(std::isfinite(ax) && std::isfinite(ay) && std::isfinite(bx) && std::isfinite(by),
                      "[%s/%d] the clipped segment came back non finite", v.name, ci);
                // ...and one entirely behind is dropped.
                dai_vec3 b2{ c.eye.x - fwd.x * 9.0f, c.eye.y - fwd.y * 9.0f, c.eye.z - fwd.z * 9.0f };
                CHECK(dai_editor_project_seg(ed, a, b2, &ax, &ay, &bx, &by) == 0,
                      "[%s/%d] a segment entirely behind the camera was drawn", v.name, ci);
            }

            // ---- the same question twice gives the same answer --------------
            // Projection reads no clock and no frame counter; a gizmo that
            // moved between two identical frames is the bug this forbids.
            {
                float ax = 0, ay = 0, bx = 0, by = 0;
                dai_editor_project(ed, c.target, &ax, &ay);
                dai_editor_project(ed, c.target, &bx, &by);
                CHECK(ax == bx && ay == by,
                      "[%s/%d] projecting the same point twice gave (%.4f,%.4f) and (%.4f,%.4f)",
                      v.name, ci, (double)ax, (double)ay, (double)bx, (double)by);
            }
        }
        std::printf("  %-11s %.0fx%.0f at %.0f,%.0f: projection, picking and clipping agree\n",
                    views[vi].name, (double)views[vi].w, (double)views[vi].h,
                    (double)views[vi].x, (double)views[vi].y);
    }

    // ---- the rectangle is remembered, the aspect comes from it --------------
    // dai_editor_camera_viewport_rect exists because the surface and the panel
    // are not the same shape. Told a 800x600 panel inside a 1600x900 window,
    // the projection has to use 4:3 - a scene drawn at the window's 16:9 is
    // stretched, and every gizmo lands beside what it moves.
    {
        dai_editor_camera(ed, dai_vec3{ 0, 0, 10 }, dai_vec3{ 0, 0, 0 }, up,
                          60.0f, 0.05f, 500.0f, 1600.0f, 900.0f);
        dai_editor_camera_viewport_rect(ed, 300.0f, 100.0f, 800.0f, 600.0f);
        float lx = 0, ly = 0, rx = 0, ry = 0, ty = 0, tx = 0;
        dai_editor_project(ed, dai_vec3{ -1, 0, 0 }, &lx, &ly);
        dai_editor_project(ed, dai_vec3{ 1, 0, 0 }, &rx, &ry);
        dai_editor_project(ed, dai_vec3{ 0, 1, 0 }, &tx, &ty);
        float px_per_m_x = (rx - lx) * 0.5f;
        float px_per_m_y = (100.0f + 300.0f - ty);
        CHECK(px_per_m_x > 1.0f && px_per_m_y > 1.0f,
              "the projection collapsed: %.2f px/m across, %.2f px/m up",
              (double)px_per_m_x, (double)px_per_m_y);
        CHECK(std::fabs(px_per_m_x - px_per_m_y) < 0.5f,
              "a metre is %.2f px across but %.2f px up - the panel's aspect was not used",
              (double)px_per_m_x, (double)px_per_m_y);
        CHECK(std::fabs(ly - ry) < 0.01f, "two points at the same height projected %.3f px apart",
              (double)std::fabs(ly - ry));
    }

    // ---- a 0x0 panel does not turn the camera -------------------------------
    // A hidden Scene tab reports no rectangle. The camera used to reconstruct
    // its direction from a ray through "the middle" of that rectangle, which
    // was the corner, and turned by half a field of view every time the tab
    // was hidden. It must simply keep what it had.
    {
        dai_editor_camera(ed, dai_vec3{ 4, 3, 9 }, dai_vec3{ 0, 1, 0 }, up,
                          55.0f, 0.05f, 500.0f, 1200.0f, 700.0f);
        dai_editor_camera_viewport_rect(ed, 40.0f, 20.0f, 1000.0f, 600.0f);
        dai_vec3 e0{}, t0{};
        float f0 = 0;
        dai_editor_camera_get(ed, &e0, &t0, &f0);
        dai_editor_camera_viewport_rect(ed, 0.0f, 0.0f, 0.0f, 0.0f);
        dai_vec3 e1{}, t1{};
        float f1 = 0;
        dai_editor_camera_get(ed, &e1, &t1, &f1);
        CHECK(std::fabs(e0.x - e1.x) + std::fabs(e0.y - e1.y) + std::fabs(e0.z - e1.z) < 1e-5f,
              "hiding the Scene panel moved the camera's eye");
        CHECK(std::fabs(t0.x - t1.x) + std::fabs(t0.y - t1.y) + std::fabs(t0.z - t1.z) < 1e-5f,
              "hiding the Scene panel turned the camera");
        CHECK(f0 == f1, "hiding the Scene panel changed the field of view");
    }

    // ---- flying the camera is deterministic ---------------------------------
    // Same input, same dt, same result - twice, from the same start. The
    // editor camera is the one piece of the editor that integrates over time,
    // and a camera that drifts is a screenshot nobody can reproduce.
    {
        auto fly = [&](dai_vec3 *out_eye, dai_vec3 *out_target) {
            dai_editor_camera(ed, dai_vec3{ 0, 2, 8 }, dai_vec3{ 0, 1, 0 }, up,
                              60.0f, 0.05f, 500.0f, 1280.0f, 720.0f);
            dai_editor_camera_viewport_rect(ed, 0.0f, 0.0f, 1280.0f, 720.0f);
            dai_editor_cam_speed(ed, 6.0f);
            // One frame with the button UP first, and the look anchor put
            // where the drag starts: mouse look is a DELTA, so a run that
            // begins while the previous run's button is still held turns by
            // the distance between the two - which is a difference in the
            // input, not in the camera.
            {
                dai_editor_cam_input rel{};
                rel.dt = 1.0f / 60.0f;
                rel.mouse_x = 640.0f; rel.mouse_y = 360.0f;
                dai_editor_cam_update(ed, &rel);
                dai_editor_cam_anchor(ed, 640.0f, 360.0f);
            }
            for (int i = 0; i < 30; ++i) {
                dai_editor_cam_input in{};
                in.dt = 1.0f / 60.0f;
                in.mouse_x = 640.0f + (float)i;
                in.mouse_y = 360.0f;
                in.mouse_right = 1;
                in.key_w = 1;
                dai_editor_cam_update(ed, &in);
            }
            float f = 0;
            dai_editor_camera_get(ed, out_eye, out_target, &f);
        };
        dai_vec3 e1{}, t1{}, e2{}, t2{};
        fly(&e1, &t1);
        fly(&e2, &t2);
        CHECK(std::memcmp(&e1, &e2, sizeof(dai_vec3)) == 0,
              "flying the same 30 frames twice ended at two different eyes "
              "(%.6f %.6f %.6f vs %.6f %.6f %.6f)",
              (double)e1.x, (double)e1.y, (double)e1.z, (double)e2.x, (double)e2.y, (double)e2.z);
        CHECK(std::memcmp(&t1, &t2, sizeof(dai_vec3)) == 0,
              "flying the same 30 frames twice ended looking at two different points");
        CHECK(std::fabs(e1.z - 8.0f) > 0.5f, "thirty frames of W moved the camera %.3f m",
              (double)std::fabs(e1.z - 8.0f));
        std::printf("  30 frames of W + mouse look: eye %.4f %.4f %.4f, twice\n",
                    (double)e1.x, (double)e1.y, (double)e1.z);
    }

    dai_editor_destroy(ed);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
