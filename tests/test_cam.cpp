// Viewport camera - Unity bindings, not Blender's.
//
//   ./build/test_cam
//
// The bindings being pinned down here:
//   right held        look in place; WASD/QE move; shift boosts; wheel = speed
//   wheel alone       dolly
//   middle held       pan
//   alt + left        orbit the pivot
//   alt + right       dolly by dragging
//   F                 frame the selection

#include "dai_editor.h"
#include <cstdio>
#include <cmath>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static float len3(dai_vec3 a) { return std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z); }
static dai_vec3 sub3(dai_vec3 a, dai_vec3 b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
static bool near3(dai_vec3 a, dai_vec3 b, float eps = 1e-3f) {
    return std::fabs(a.x-b.x) < eps && std::fabs(a.y-b.y) < eps && std::fabs(a.z-b.z) < eps;
}

// The editor keeps eye/target private, so read the direction back through the
// ray at the exact centre of the screen - that is the view direction.
static dai_vec3 view_dir(dai_editor *e) {
    dai_vec3 o, d;
    dai_editor_ray(e, 640, 360, &o, &d);
    return d;
}
static dai_vec3 eye_of(dai_editor *e) {
    dai_vec3 o, d;
    dai_editor_ray(e, 640, 360, &o, &d);
    return o;
}

int main() {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 64; cfg.physics_threads = 1; cfg.seed = 2;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_editor *ed = dai_editor_create(doc, sync);
    std::printf("viewport camera\n");

    dai_node_desc r = dai_node_desc_default();
    r.motion = DAI_KINEMATIC;
    r.position = { 0, 0, 0 };
    std::snprintf(r.name, sizeof(r.name), "Target");
    dai_node target = dai_doc_add(doc, &r);
    r.position = { 30, 0, 0 };
    std::snprintf(r.name, sizeof(r.name), "Far");
    dai_doc_add(doc, &r);
    dai_doc_sync_apply(sync);

    auto reset = [&]() {
        dai_editor_camera(ed, dai_vec3{ 0, 0, 10 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                          60.0f, 0.1f, 200.0f, 1280.0f, 720.0f);
    };
    dai_editor_cam_input in{};
    auto blank = [&]() { std::memset(&in, 0, sizeof(in)); in.dt = 1.0f / 60.0f; };

    reset();
    blank();
    CHECK(near3(view_dir(ed), dai_vec3{ 0, 0, -1 }), "the reset camera does not look down -Z");
    CHECK(near3(eye_of(ed), dai_vec3{ 0, 0, 10 }), "the reset camera is in the wrong place");

    // ---- 1. right mouse looks around without moving -----------------------
    blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1;
    dai_editor_cam_update(ed, &in);            // press
    CHECK(dai_editor_cam_active(ed), "holding right did not start a camera gesture");
    dai_vec3 eye_before = eye_of(ed);
    in.mouse_x = 740;                          // drag right
    dai_editor_cam_update(ed, &in);
    dai_vec3 d = view_dir(ed);
    CHECK(d.x > 0.2f, "dragging right did not turn the view right (dir.x %.3f)", d.x);
    CHECK(near3(eye_of(ed), eye_before), "looking around moved the camera");
    in.mouse_right = 0;
    dai_editor_cam_update(ed, &in);
    CHECK(!dai_editor_cam_active(ed), "releasing right did not end the gesture");

    // pitch is clamped, so the basis never degenerates
    reset(); blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1;
    dai_editor_cam_update(ed, &in);
    for (int i = 0; i < 40; ++i) { in.mouse_y -= 50.0f; dai_editor_cam_update(ed, &in); }
    d = view_dir(ed);
    CHECK(d.y < 0.9999f && d.y > 0.9f, "pitch was not clamped below vertical (dir.y %.5f)", d.y);
    dai_vec3 o2, d2;
    dai_editor_ray(ed, 900, 200, &o2, &d2);
    CHECK(len3(d2) > 0.99f && len3(d2) < 1.01f,
          "the camera basis degenerated at full pitch (|dir| %.4f)", len3(d2));

    // ---- 2. WASD only while right is held ---------------------------------
    reset(); blank();
    in.key_w = 1;
    for (int i = 0; i < 30; ++i) dai_editor_cam_update(ed, &in);
    CHECK(near3(eye_of(ed), dai_vec3{ 0, 0, 10 }),
          "W moved the camera without the right button held - that is Blender, not Unity");

    reset(); blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1; in.key_w = 1;
    dai_editor_cam_update(ed, &in);
    for (int i = 0; i < 30; ++i) dai_editor_cam_update(ed, &in);
    dai_vec3 flown = eye_of(ed);
    CHECK(flown.z < 10.0f - 1.0f, "W did not fly forwards (z %.3f)", flown.z);
    CHECK(std::fabs(flown.x) < 1e-3f && std::fabs(flown.y) < 1e-3f, "W drifted sideways");

    // shift is a boost, not a different direction
    reset(); blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1; in.key_w = 1; in.key_shift = 1;
    dai_editor_cam_update(ed, &in);
    for (int i = 0; i < 30; ++i) dai_editor_cam_update(ed, &in);
    float boosted = 10.0f - eye_of(ed).z;
    float plain = 10.0f - flown.z;
    CHECK(boosted > plain * 2.0f, "shift did not speed movement up (%.2f vs %.2f)", boosted, plain);

    // Q down, E up - the Unity way round
    reset(); blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1; in.key_e = 1;
    dai_editor_cam_update(ed, &in);
    for (int i = 0; i < 30; ++i) dai_editor_cam_update(ed, &in);
    CHECK(eye_of(ed).y > 0.5f, "E did not go up (y %.3f)", eye_of(ed).y);
    reset(); blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1; in.key_q = 1;
    dai_editor_cam_update(ed, &in);
    for (int i = 0; i < 30; ++i) dai_editor_cam_update(ed, &in);
    CHECK(eye_of(ed).y < -0.5f, "Q did not go down (y %.3f)", eye_of(ed).y);

    // ---- 3. the wheel: speed while flying, dolly otherwise ----------------
    reset(); blank();
    dai_editor_cam_speed(ed, 6.0f);
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1;
    dai_editor_cam_update(ed, &in);
    dai_vec3 before_wheel = eye_of(ed);
    in.wheel = 3.0f;
    dai_editor_cam_update(ed, &in);
    CHECK(dai_editor_cam_speed_get(ed) > 6.5f,
          "the wheel while flying should raise the move speed, it is %.2f",
          dai_editor_cam_speed_get(ed));
    CHECK(near3(eye_of(ed), before_wheel),
          "the wheel while flying moved the camera instead of changing speed");
    in.wheel = -6.0f;
    dai_editor_cam_update(ed, &in);
    CHECK(dai_editor_cam_speed_get(ed) < 6.0f, "the wheel down did not lower the speed");

    reset(); blank();
    in.wheel = 2.0f;                            // nothing held
    dai_editor_cam_update(ed, &in);
    CHECK(eye_of(ed).z < 10.0f - 0.5f, "the wheel alone did not dolly forwards (z %.3f)",
          eye_of(ed).z);
    CHECK(!dai_editor_cam_active(ed), "the wheel counted as an ongoing gesture");

    // ---- 4. middle mouse pans ---------------------------------------------
    reset(); blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_middle = 1;
    dai_editor_cam_update(ed, &in);
    dai_vec3 dir_before = view_dir(ed);
    in.mouse_x = 540;
    dai_editor_cam_update(ed, &in);
    dai_vec3 panned = eye_of(ed);
    CHECK(panned.x > 0.1f, "dragging left with the middle button did not pan right (x %.3f)",
          panned.x);
    CHECK(near3(view_dir(ed), dir_before), "panning rotated the camera");

    // ---- 5. alt + left orbits, keeping the distance to the pivot ----------
    reset(); blank();
    dai_vec3 pivot0 = dai_editor_cam_pivot(ed);
    CHECK(near3(pivot0, dai_vec3{ 0, 0, 0 }, 0.01f),
          "the pivot should start at the look-at point, it is (%.2f %.2f %.2f)",
          pivot0.x, pivot0.y, pivot0.z);

    in.mouse_x = 640; in.mouse_y = 360; in.key_alt = 1; in.mouse_left = 1;
    dai_editor_cam_update(ed, &in);
    float dist_before = len3(sub3(eye_of(ed), pivot0));
    in.mouse_x = 840;
    dai_editor_cam_update(ed, &in);
    dai_vec3 orbited = eye_of(ed);
    float dist_after = len3(sub3(orbited, pivot0));
    CHECK(std::fabs(dist_after - dist_before) < 0.05f,
          "orbiting changed the distance to the pivot (%.3f -> %.3f)", dist_before, dist_after);
    CHECK(std::fabs(orbited.x) > 1.0f, "orbiting did not move the camera around (x %.3f)",
          orbited.x);
    CHECK(near3(dai_editor_cam_pivot(ed), pivot0, 0.05f), "the pivot drifted while orbiting");

    // switching button mid-gesture must re-anchor, not jump: hold middle, then
    // press alt+left without releasing
    reset(); blank();
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_middle = 1;
    dai_editor_cam_update(ed, &in);
    in.mouse_x = 500;
    dai_editor_cam_update(ed, &in);
    dai_vec3 mid_gesture = eye_of(ed);
    in.mouse_middle = 0; in.key_alt = 1; in.mouse_left = 1;   // switch, same position
    dai_editor_cam_update(ed, &in);
    CHECK(near3(eye_of(ed), mid_gesture, 0.01f),
          "switching from pan to orbit without releasing jumped the camera");

    // ---- 6. F frames the selection ----------------------------------------
    reset(); blank();
    dai_editor_select(ed, target, 0);
    // start far away and pointing elsewhere
    dai_editor_camera(ed, dai_vec3{ 60, 40, 60 }, dai_vec3{ 61, 40, 60 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 500.0f, 1280.0f, 720.0f);
    in.key_focus = 1;
    dai_editor_cam_update(ed, &in);
    float px = 0, py = 0;
    CHECK(dai_editor_project(ed, dai_vec3{ 0, 0, 0 }, &px, &py) == 1,
          "after F the selection is not even in front of the camera");
    CHECK(std::fabs(px - 640.0f) < 40.0f && std::fabs(py - 360.0f) < 40.0f,
          "F did not centre the selection - it projects to (%.0f %.0f)", px, py);
    float dist = len3(sub3(eye_of(ed), dai_vec3{ 0, 0, 0 }));
    CHECK(dist > 0.6f && dist < 12.0f, "F left the camera %.2f m away from a 0.5 m box", dist);

    // holding F must not keep re-framing every frame
    dai_vec3 after_focus = eye_of(ed);
    for (int i = 0; i < 5; ++i) dai_editor_cam_update(ed, &in);
    CHECK(near3(eye_of(ed), after_focus), "holding F kept moving the camera");

    // F with nothing selected frames the whole scene, including the far node
    blank();
    dai_editor_deselect_all(ed);
    in.key_focus = 1;
    dai_editor_cam_update(ed, &in);
    CHECK(dai_editor_project(ed, dai_vec3{ 0, 0, 0 }, &px, &py) == 1 &&
          dai_editor_project(ed, dai_vec3{ 30, 0, 0 }, &px, &py) == 1,
          "F with an empty selection did not frame the whole scene");

    // ---- 7. an explicit camera call still wins ----------------------------
    reset(); blank();
    CHECK(near3(view_dir(ed), dai_vec3{ 0, 0, -1 }),
          "setting the camera explicitly did not take after camera gestures");
    in.mouse_x = 640; in.mouse_y = 360; in.mouse_right = 1;
    dai_editor_cam_update(ed, &in);
    in.mouse_x = 660;
    dai_editor_cam_update(ed, &in);
    d = view_dir(ed);
    CHECK(std::fabs(d.x) > 0.01f && std::fabs(d.x) < 0.5f,
          "the first look after an explicit camera set jumped (dir.x %.3f)", d.x);

    dai_editor_destroy(ed);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
