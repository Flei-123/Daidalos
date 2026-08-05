// The animation system, checked as arithmetic.
//
//   ./build/test_anim                     the player alone, no GPU, no model
//   ./build/test_anim_gltf assets/test    the same plus the glTF import path
//
// Nothing here looks at a picture. A pose that is wrong by a quaternion sign
// or a lerp instead of a slerp still renders a character - just one whose
// limbs shrink mid stride - so every claim below is a number with a tolerance.
//
// The last section (compiled in with -DDAI_ANIM_WITH_GLTF) is the one that
// matters most for trust: it poses the same model twice, once through the OLD
// dai_model_pose and once through the NEW clip -> player -> dai_model_pose_local
// path, and requires the joint matrices to agree to 1e-5. If the new layer
// disagrees with the code that already shipped, one of them is wrong.

#include "dai_anim.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#ifdef DAI_ANIM_WITH_GLTF
#include "dai_gltf.h"
#include "dai_render.h"
#endif

#ifdef DAI_ANIM_WITH_SCRIPT
#include "dai_script.h"
#endif

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static bool near_f(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Angle between two orientations, in degrees. q and -q are the same rotation,
// so the sign of the dot product is ignored - a test that does not know that
// fails at random.
static float quat_angle_deg(const dai_quat &a, const dai_quat &b) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    d = std::fabs(d);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * std::acos(d) * 57.2957795f;
}

static float quat_len(const dai_quat &q) {
    return std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
}

// ---- the event recorder every section below shares
struct Log {
    std::vector<std::string> names;
    std::vector<float>       times;
    std::vector<std::string> clips;
    void clear() { names.clear(); times.clear(); clips.clear(); }
    int count(const char *n) const {
        int c = 0;
        for (const std::string &s : names) if (s == n) ++c;
        return c;
    }
};
static void on_event(const char *ev, const char *clip, float time, void *user) {
    Log *l = (Log *)user;
    l->names.push_back(ev);
    l->clips.push_back(clip ? clip : "");
    l->times.push_back(time);
}

// A clip that moves target 1 from `from` to `to` on X over `dur` seconds and
// rotates it from identity to `deg` about Z.
static dai_anim_clip *make_clip(const char *name, float from, float to, float dur, float deg) {
    dai_anim_clip *c = dai_anim_clip_create(name);
    const float times[2] = { 0.0f, dur };
    const float pos[6]   = { from, 0, 0,  to, 0, 0 };
    const float half = deg * 0.5f * 3.14159265f / 180.0f;
    const float rot[8]   = { 0, 0, 0, 1,   0, 0, std::sin(half), std::cos(half) };
    dai_anim_clip_add_channel(c, 1, DAI_ANIM_TRANSLATION, DAI_ANIM_LINEAR, times, pos, 2);
    dai_anim_clip_add_channel(c, 1, DAI_ANIM_ROTATION,    DAI_ANIM_LINEAR, times, rot, 2);
    return c;
}

#ifdef DAI_ANIM_WITH_SCRIPT
// The host half of the `anim` binding: thin forwarding, which is the whole
// point - the script layer must not grow its own idea of what playing means.
static int    h_play(const char *c, double f, void *u) { return dai_anim_play((dai_anim *)u, c, (float)f); }
static int    h_restart(const char *c, double f, double st, void *u) { return dai_anim_restart((dai_anim *)u, c, (float)f, (float)st); }
static void   h_stop(double f, void *u) { dai_anim_stop((dai_anim *)u, (float)f); }
static int    h_playing(const char *c, void *u) { return dai_anim_is_playing((dai_anim *)u, c); }
static const char *h_current(void *u) { return dai_anim_current((dai_anim *)u); }
static double h_weight(const char *c, void *u) { return dai_anim_weight((dai_anim *)u, c); }
static int    h_finished(void *u) { return dai_anim_finished((dai_anim *)u); }
static double h_getspeed(void *u) { return dai_anim_speed((dai_anim *)u); }
static void   h_setspeed(double v, void *u) { dai_anim_set_speed((dai_anim *)u, (float)v); }
static double h_gettime(void *u) { return dai_anim_time((dai_anim *)u); }
static void   h_settime(double v, void *u) { dai_anim_set_time((dai_anim *)u, (float)v); }
static double h_getnorm(void *u) { return dai_anim_normalized_time((dai_anim *)u); }
static double h_getparam(void *u) { return dai_anim_parameter((dai_anim *)u); }
static void   h_setparam(double v, void *u) { dai_anim_set_parameter((dai_anim *)u, (float)v); }

// The player calls this on the C side; the host forwards into JS. QuickJS
// never sits inside the update loop.
static dai_script *g_script = nullptr;
static void forward_to_script(const char *ev, const char *clip, float t, void *) {
    if (g_script) dai_script_anim_event(g_script, ev, clip, t);
}
#endif

// A clip that holds ONE pose for `dur` seconds: x on the translation, `deg`
// about Z. A crossfade test needs this - with a clip that is itself moving you
// cannot tell a wrong blend weight from a right one at the wrong time.
static dai_anim_clip *make_static(const char *name, float x, float deg, float dur) {
    dai_anim_clip *c = dai_anim_clip_create(name);
    const float times[2] = { 0.0f, dur };
    const float pos[6]   = { x, 0, 0,  x, 0, 0 };
    const float half = deg * 0.5f * 3.14159265f / 180.0f;
    const float q = std::sin(half), w = std::cos(half);
    const float rot[8]   = { 0, 0, q, w,  0, 0, q, w };
    dai_anim_clip_add_channel(c, 1, DAI_ANIM_TRANSLATION, DAI_ANIM_LINEAR, times, pos, 2);
    dai_anim_clip_add_channel(c, 1, DAI_ANIM_ROTATION,    DAI_ANIM_LINEAR, times, rot, 2);
    return c;
}

int main(int argc, char **argv) {
    std::printf("animation\n");

    // ================================================================ 1. clips
    {
        std::printf("-- clip sampling\n");
        dai_anim_clip *c = dai_anim_clip_create("move");
        // three keys, unevenly spaced on purpose: an interpolation that
        // divides by the wrong span is right at the keys and wrong between.
        const float times[3]  = { 0.0f, 1.0f, 3.0f };
        const float pos[9]    = { 0,0,0,   10,0,0,   10,20,0 };
        const float scale[9]  = { 1,1,1,   2,2,2,    4,4,4 };
        int ch = dai_anim_clip_add_channel(c, 0, DAI_ANIM_TRANSLATION, DAI_ANIM_LINEAR, times, pos, 3);
        CHECK(ch == 0, "add_channel returned %d", ch);
        dai_anim_clip_add_channel(c, 0, DAI_ANIM_SCALE, DAI_ANIM_LINEAR, times, scale, 3);

        CHECK(near_f(dai_anim_clip_duration(c), 3.0f), "duration is %.3f, expected 3.0", dai_anim_clip_duration(c));
        CHECK(dai_anim_clip_channel_count(c) == 2, "channel count is %u", dai_anim_clip_channel_count(c));
        CHECK(dai_anim_clip_target_count(c) == 1, "target count is %u", dai_anim_clip_target_count(c));
        CHECK(dai_anim_clip_loop(c) == 1, "clips should loop by default");

        std::vector<dai_anim_trs> pose(2, dai_anim_trs_identity());

        // exact keyframes
        dai_anim_clip_sample(c, 0.0f, pose.data(), 2);
        CHECK(near_f(pose[0].t.x, 0.0f), "t=0 x is %.4f, expected 0", pose[0].t.x);
        dai_anim_clip_sample(c, 1.0f, pose.data(), 2);
        CHECK(near_f(pose[0].t.x, 10.0f) && near_f(pose[0].t.y, 0.0f),
              "t=1 is (%.4f %.4f), expected (10 0)", pose[0].t.x, pose[0].t.y);
        dai_anim_clip_sample(c, 3.0f, pose.data(), 2);
        CHECK(near_f(pose[0].t.y, 20.0f), "t=3 y is %.4f, expected 20", pose[0].t.y);

        // between them: the second span is 2 s long, so half way through it is t=2
        dai_anim_clip_sample(c, 0.5f, pose.data(), 2);
        CHECK(near_f(pose[0].t.x, 5.0f), "t=0.5 x is %.4f, expected 5", pose[0].t.x);
        dai_anim_clip_sample(c, 2.0f, pose.data(), 2);
        CHECK(near_f(pose[0].t.y, 10.0f) && near_f(pose[0].t.x, 10.0f),
              "t=2 is (%.4f %.4f), expected (10 10)", pose[0].t.x, pose[0].t.y);
        CHECK(near_f(pose[0].s.x, 3.0f), "t=2 scale is %.4f, expected 3", pose[0].s.x);

        // outside the clip: clamped, not wrapped - the PLAYER loops, not the clip
        dai_anim_clip_sample(c, -5.0f, pose.data(), 2);
        CHECK(near_f(pose[0].t.x, 0.0f), "t=-5 x is %.4f, expected the first key", pose[0].t.x);
        dai_anim_clip_sample(c, 99.0f, pose.data(), 2);
        CHECK(near_f(pose[0].t.y, 20.0f), "t=99 y is %.4f, expected the last key", pose[0].t.y);

        // targets the clip does not drive are left alone
        CHECK(near_f(pose[1].t.x, 0.0f) && near_f(pose[1].s.x, 1.0f),
              "an untouched target was written to");

        // unsorted keys are refused rather than binary-searched into nonsense
        const float bad[3] = { 0.0f, 2.0f, 1.0f };
        CHECK(dai_anim_clip_add_channel(c, 0, DAI_ANIM_TRANSLATION, DAI_ANIM_LINEAR, bad, pos, 3) == -1,
              "unsorted keyframe times were accepted");
        dai_anim_clip_destroy(c);
    }

    // ============================================================ 2. step mode
    {
        std::printf("-- step interpolation\n");
        dai_anim_clip *c = dai_anim_clip_create("blink");
        const float times[2] = { 0.0f, 1.0f };
        const float pos[6]   = { 0,0,0,  100,0,0 };
        dai_anim_clip_add_channel(c, 0, DAI_ANIM_TRANSLATION, DAI_ANIM_STEP, times, pos, 2);
        std::vector<dai_anim_trs> pose(1, dai_anim_trs_identity());
        dai_anim_clip_sample(c, 0.99f, pose.data(), 1);
        CHECK(near_f(pose[0].t.x, 0.0f), "STEP at 0.99 is %.4f, expected to still hold 0", pose[0].t.x);
        dai_anim_clip_sample(c, 1.0f, pose.data(), 1);
        CHECK(near_f(pose[0].t.x, 100.0f), "STEP at 1.0 is %.4f, expected 100", pose[0].t.x);
        dai_anim_clip_destroy(c);
    }

    // ================================================================ 3. slerp
    {
        std::printf("-- rotation: slerp, not lerp\n");
        dai_anim_clip *c = dai_anim_clip_create("turn");
        const float times[2] = { 0.0f, 1.0f };
        // identity -> 170 degrees about Z. A wide arc, because that is where
        // lerp and slerp actually differ; at 10 degrees both look fine.
        const float half = 85.0f * 3.14159265f / 180.0f;
        const float rot[8] = { 0,0,0,1,  0, 0, std::sin(half), std::cos(half) };
        dai_anim_clip_add_channel(c, 0, DAI_ANIM_ROTATION, DAI_ANIM_LINEAR, times, rot, 2);
        std::vector<dai_anim_trs> pose(1, dai_anim_trs_identity());

        dai_anim_clip_sample(c, 0.5f, pose.data(), 1);
        const dai_quat id{ 0,0,0,1 };
        const dai_quat end{ 0, 0, std::sin(half), std::cos(half) };
        const float to_start = quat_angle_deg(pose[0].r, id);
        const float to_end   = quat_angle_deg(pose[0].r, end);
        std::printf("   half way: %.3f deg from the start, %.3f deg from the end, |q| = %.6f\n",
                    to_start, to_end, quat_len(pose[0].r));
        // constant angular velocity is the whole point of slerp
        CHECK(near_f(to_start, 85.0f, 0.05f), "half way is %.3f deg from the start, expected 85", to_start);
        CHECK(near_f(to_end, 85.0f, 0.05f), "half way is %.3f deg from the end, expected 85", to_end);
        CHECK(near_f(quat_len(pose[0].r), 1.0f, 1e-5f), "the result is not unit length: %.6f", quat_len(pose[0].r));

        // A quarter of the way. This is where slerp earns its name: at exactly
        // half way a normalised lerp and a slerp are the same quaternion
        // (both are the normalised sum), so probing there proves nothing at all.
        dai_anim_clip_sample(c, 0.25f, pose.data(), 1);
        const float slerp_q = quat_angle_deg(pose[0].r, id);
        float lx = 0.75f * 0 + 0.25f * end.z, lw = 0.75f * 1 + 0.25f * end.w;
        float ll = std::sqrt(lx*lx + lw*lw);
        const float lerp_q = quat_angle_deg(dai_quat{ 0, 0, lx/ll, lw/ll }, id);
        std::printf("   quarter way: slerp %.3f deg, a normalised lerp would give %.3f\n", slerp_q, lerp_q);
        CHECK(std::fabs(lerp_q - 42.5f) > 1.0f,
              "the lerp and the slerp agree at a quarter (%.3f), so this proves nothing", lerp_q);
        CHECK(near_f(slerp_q, 42.5f, 0.05f),
              "quarter way is %.3f deg, expected 42.5", slerp_q);
        dai_anim_clip_sample(c, 0.75f, pose.data(), 1);
        CHECK(near_f(quat_angle_deg(pose[0].r, id), 127.5f, 0.05f),
              "three quarters is %.3f deg, expected 127.5", quat_angle_deg(pose[0].r, id));

        // shortest arc: interpolating towards -q must take the SHORT way round
        dai_anim_clip *c2 = dai_anim_clip_create("flip");
        const float rot2[8] = { 0,0,0,1,  0, 0, -std::sin(half), -std::cos(half) };   // same rotation, negated
        dai_anim_clip_add_channel(c2, 0, DAI_ANIM_ROTATION, DAI_ANIM_LINEAR, times, rot2, 2);
        dai_anim_clip_sample(c2, 0.5f, pose.data(), 1);
        CHECK(near_f(quat_angle_deg(pose[0].r, id), 85.0f, 0.05f),
              "with a negated end key the midpoint is %.3f deg, expected the same 85 (shortest arc)",
              quat_angle_deg(pose[0].r, id));
        dai_anim_clip_destroy(c2);
        dai_anim_clip_destroy(c);
    }

    // ================================================================= 4. loop
    {
        std::printf("-- looping\n");
        dai_anim *a = dai_anim_create(2);
        dai_anim_clip *c = make_clip("cycle", 0.0f, 4.0f, 2.0f, 0.0f);
        dai_anim_add(a, c, 1);
        dai_anim_play(a, "cycle", 0.0f);

        dai_anim_update(a, 0.0f);
        uint32_t n = 0;
        const dai_anim_trs *p = dai_anim_pose(a, &n);
        CHECK(n == 2 && p != nullptr, "the pose is %u targets", n);
        CHECK(near_f(p[1].t.x, 0.0f), "at t=0 x is %.4f, expected 0", p[1].t.x);

        dai_anim_update(a, 1.0f);                     // half way
        p = dai_anim_pose(a, &n);
        CHECK(near_f(p[1].t.x, 2.0f), "at t=1 x is %.4f, expected 2", p[1].t.x);
        CHECK(near_f(dai_anim_time(a), 1.0f), "time() is %.4f, expected 1.0", dai_anim_time(a));
        CHECK(near_f(dai_anim_normalized_time(a), 0.5f), "normalized is %.4f", dai_anim_normalized_time(a));

        dai_anim_update(a, 1.5f);                     // 2.5 s: wrapped to 0.5
        p = dai_anim_pose(a, &n);
        CHECK(near_f(dai_anim_time(a), 0.5f, 1e-3f), "after 2.5 s time() is %.4f, expected 0.5", dai_anim_time(a));
        CHECK(near_f(p[1].t.x, 1.0f, 1e-3f), "after the loop x is %.4f, expected 1", p[1].t.x);
        CHECK(dai_anim_finished(a) == 0, "a looping clip reported finished");

        // seven full cycles later it must be in exactly the same place
        dai_anim_update(a, 14.0f);
        CHECK(near_f(dai_anim_time(a), 0.5f, 1e-2f),
              "after 7 more cycles time() is %.4f, expected 0.5 - the wrap drifts", dai_anim_time(a));

        // a clip that does not loop stops at the end and says so
        dai_anim_clip *once = make_clip("once", 0.0f, 4.0f, 2.0f, 0.0f);
        dai_anim_clip_set_loop(once, 0);
        dai_anim_add(a, once, 1);
        dai_anim_play(a, "once", 0.0f);
        dai_anim_update(a, 1.0f);
        CHECK(dai_anim_finished(a) == 0, "a non looping clip finished half way through");
        dai_anim_update(a, 5.0f);
        p = dai_anim_pose(a, &n);
        CHECK(dai_anim_finished(a) == 1, "a non looping clip did not report finished");
        CHECK(near_f(p[1].t.x, 4.0f), "a finished clip holds x = %.4f, expected the last key 4", p[1].t.x);
        dai_anim_destroy(a);
    }

    // ============================================================ 5. crossfade
    {
        std::printf("-- crossfade\n");
        dai_anim *a = dai_anim_create(2);
        dai_anim_add(a, make_static("A", 0.0f, 0.0f, 1.0f), 1);    // holds x = 0, no rotation
        dai_anim_add(a, make_static("B", 10.0f, 90.0f, 1.0f), 1);  // holds x = 10, 90 deg about Z
        dai_anim_play(a, "A", 0.0f);
        dai_anim_update(a, 0.0f);
        CHECK(near_f(dai_anim_weight(a, "A"), 1.0f), "A alone weighs %.3f", dai_anim_weight(a, "A"));
        CHECK(std::strcmp(dai_anim_current(a), "A") == 0, "current() is '%s'", dai_anim_current(a));

        dai_anim_crossfade(a, "B", 0.4f);
        CHECK(near_f(dai_anim_weight(a, "B"), 0.0f), "B starts at weight %.3f, expected 0", dai_anim_weight(a, "B"));
        CHECK(std::strcmp(dai_anim_current(a), "B") == 0, "current() after crossfade is '%s'", dai_anim_current(a));

        dai_anim_update(a, 0.1f);
        float wb = dai_anim_weight(a, "B"), wa = dai_anim_weight(a, "A");
        std::printf("   0.1/0.4 through: A %.3f  B %.3f\n", wa, wb);
        CHECK(near_f(wb, 0.25f, 1e-3f), "a quarter through, B weighs %.4f, expected 0.25", wb);
        CHECK(near_f(wa + wb, 1.0f, 1e-4f), "the weights sum to %.4f, not 1", wa + wb);
        uint32_t n = 0;
        const dai_anim_trs *p = dai_anim_pose(a, &n);
        CHECK(near_f(p[1].t.x, 2.5f, 1e-3f), "at weight 0.25 the pose is x = %.4f, expected 2.5", p[1].t.x);
        CHECK(near_f(quat_angle_deg(p[1].r, dai_quat{0,0,0,1}), 22.5f, 0.05f),
              "at weight 0.25 the rotation is %.3f deg, expected 22.5 (a slerp of the two)",
              quat_angle_deg(p[1].r, dai_quat{0,0,0,1}));

        dai_anim_update(a, 0.1f);
        p = dai_anim_pose(a, &n);
        std::printf("   half way:        A %.3f  B %.3f  ->  x = %.3f\n",
                    dai_anim_weight(a, "A"), dai_anim_weight(a, "B"), p[1].t.x);
        CHECK(near_f(dai_anim_weight(a, "B"), 0.5f, 1e-3f), "half way B weighs %.4f", dai_anim_weight(a, "B"));
        CHECK(near_f(p[1].t.x, 5.0f, 1e-3f), "half way the pose is x = %.4f, expected 5", p[1].t.x);

        dai_anim_update(a, 0.2f);
        p = dai_anim_pose(a, &n);
        CHECK(near_f(dai_anim_weight(a, "B"), 1.0f, 1e-4f), "at the end B weighs %.4f", dai_anim_weight(a, "B"));
        CHECK(near_f(dai_anim_weight(a, "A"), 0.0f, 1e-4f), "at the end A still weighs %.4f", dai_anim_weight(a, "A"));
        CHECK(near_f(p[1].t.x, 10.0f, 1e-3f), "at the end the pose is x = %.4f, expected 10", p[1].t.x);
        CHECK(dai_anim_is_playing(a, "A") == 0, "A is still playing after fading out");
        CHECK(dai_anim_is_playing(a, "B") == 1, "B is not playing");

        // playing what is already playing must not restart it
        dai_anim_update(a, 0.3f);
        float t_before = dai_anim_time(a);
        dai_anim_play(a, "B", 0.2f);
        dai_anim_update(a, 0.0f);
        CHECK(near_f(dai_anim_time(a), t_before, 1e-4f),
              "play() on the current clip restarted it (%.4f -> %.4f)", t_before, dai_anim_time(a));
        // ... but restart() must
        dai_anim_restart(a, "B", 0.0f, 0.0f);
        dai_anim_update(a, 0.0f);
        CHECK(near_f(dai_anim_time(a), 0.0f, 1e-4f), "restart() left the head at %.4f", dai_anim_time(a));

        // a snap fade is instant
        dai_anim_play(a, "A", 0.0f);
        dai_anim_update(a, 0.0f);
        CHECK(near_f(dai_anim_weight(a, "A"), 1.0f, 1e-4f),
              "a zero-length fade left A at %.4f", dai_anim_weight(a, "A"));
        dai_anim_destroy(a);
    }

    // =============================================================== 6. events
    {
        std::printf("-- events\n");
        Log log;
        dai_anim *a = dai_anim_create(2);
        dai_anim_clip *walk = make_clip("walk", 0.0f, 1.0f, 1.0f, 0.0f);
        dai_anim_clip_add_event(walk, 0.25f, "left");
        dai_anim_clip_add_event(walk, 0.75f, "right");
        dai_anim_add(a, walk, 1);
        dai_anim_on_event(a, on_event, &log);

        CHECK(dai_anim_clip_event_count(walk) == 2, "the clip has %u events", dai_anim_clip_event_count(walk));
        float et = -1.0f;
        const char *en = dai_anim_clip_event_at(walk, 0, &et);
        CHECK(en && std::strcmp(en, "left") == 0 && near_f(et, 0.25f),
              "events are not kept sorted: [0] is '%s' at %.3f", en ? en : "(null)", et);

        // one pass, ten small steps: each event exactly once
        dai_anim_play(a, "walk", 0.0f);
        for (int i = 0; i < 10; ++i) dai_anim_update(a, 0.1f);
        std::printf("   one pass in 10 steps: left %d, right %d\n", log.count("left"), log.count("right"));
        CHECK(log.count("left") == 1, "'left' fired %d times in one pass, expected 1", log.count("left"));
        CHECK(log.count("right") == 1, "'right' fired %d times in one pass, expected 1", log.count("right"));
        CHECK(log.clips.size() == 2 && log.clips[0] == "walk", "the event did not name its clip");

        // three more passes, still small steps
        log.clear();
        for (int i = 0; i < 30; ++i) dai_anim_update(a, 0.1f);
        std::printf("   three more passes: left %d, right %d\n", log.count("left"), log.count("right"));
        CHECK(log.count("left") == 3, "'left' fired %d times over three loops, expected 3", log.count("left"));
        CHECK(log.count("right") == 3, "'right' fired %d times over three loops, expected 3", log.count("right"));

        // ONE update that swallows three whole loops - the case a naive
        // "is the time between then and now" check gets wrong every time
        log.clear();
        dai_anim_restart(a, "walk", 0.0f, 0.0f);
        dai_anim_update(a, 3.0f);
        std::printf("   one 3.0 s update: left %d, right %d\n", log.count("left"), log.count("right"));
        CHECK(log.count("left") == 3, "a 3 s jump fired 'left' %d times, expected 3", log.count("left"));
        CHECK(log.count("right") == 3, "a 3 s jump fired 'right' %d times, expected 3", log.count("right"));

        // The loop boundary. dt is 0.125 so eight steps land on exactly 1.0 -
        // with 0.1 the head arrives at 0.99999994 and the wrap is a coin flip.
        {
            Log l2;
            dai_anim *b = dai_anim_create(2);
            dai_anim_clip *c = make_clip("edge", 0.0f, 1.0f, 1.0f, 0.0f);
            dai_anim_clip_add_event(c, 0.0f, "start");
            dai_anim_clip_add_event(c, 0.5f, "mid");
            dai_anim_add(b, c, 1);
            dai_anim_on_event(b, on_event, &l2);
            dai_anim_play(b, "edge", 0.0f);
            dai_anim_update(b, 0.125f);
            CHECK(l2.count("start") == 1, "'start' at t=0 fired %d times on the first update, expected 1",
                  l2.count("start"));
            CHECK(l2.count("mid") == 0, "'mid' fired at t=0.125");
            for (int i = 0; i < 3; ++i) dai_anim_update(b, 0.125f);   // head is at exactly 0.5
            CHECK(l2.count("mid") == 1, "'mid' at exactly its own timestamp fired %d times, expected 1",
                  l2.count("mid"));
            CHECK(l2.count("start") == 1, "'start' fired again mid pass (%d)", l2.count("start"));
            for (int i = 0; i < 4; ++i) dai_anim_update(b, 0.125f);   // head is at exactly 1.0: the wrap
            std::printf("   over the wrap: start %d, mid %d\n", l2.count("start"), l2.count("mid"));
            CHECK(l2.count("start") == 2,
                  "'start' fired %d times, expected 2 - once at t=0 and once when the loop wrapped",
                  l2.count("start"));
            CHECK(l2.count("mid") == 1, "'mid' fired %d times over one pass, expected 1", l2.count("mid"));
            // and one more pass adds exactly one of each
            for (int i = 0; i < 8; ++i) dai_anim_update(b, 0.125f);
            CHECK(l2.count("start") == 3 && l2.count("mid") == 2,
                  "a second pass gave start %d / mid %d, expected 3 / 2", l2.count("start"), l2.count("mid"));
            dai_anim_destroy(b);
        }

        // a seek is a jump: it must not machine-gun the events it skipped
        log.clear();
        dai_anim_restart(a, "walk", 0.0f, 0.0f);
        dai_anim_set_time(a, 0.9f);
        dai_anim_update(a, 0.0f);
        std::printf("   after seeking 0 -> 0.9: left %d, right %d\n", log.count("left"), log.count("right"));
        CHECK(log.count("left") == 0 && log.count("right") == 0,
              "a seek fired %d/%d events", log.count("left"), log.count("right"));
        dai_anim_update(a, 0.2f);                        // 0.9 -> 1.1: 0.25 and 0.75 are both behind it
        CHECK(log.count("left") == 0 && log.count("right") == 0,
              "0.9 -> 1.1 fired %d/%d, expected nothing: both events are behind the head",
              log.count("left"), log.count("right"));
        dai_anim_update(a, 0.4f);                        // 1.1 -> 1.5: crosses 1.25, i.e. 'left'
        std::printf("   playing on to 1.5: left %d, right %d\n", log.count("left"), log.count("right"));
        CHECK(log.count("left") == 1, "after the seek 'left' fired %d times, expected 1", log.count("left"));
        CHECK(log.count("right") == 0, "after the seek 'right' fired %d times, expected 0", log.count("right"));

        // a non looping clip fires each event once and then never again
        {
            Log l3;
            dai_anim *b = dai_anim_create(2);
            dai_anim_clip *c = make_clip("hit", 0.0f, 1.0f, 1.0f, 0.0f);
            dai_anim_clip_set_loop(c, 0);
            dai_anim_clip_add_event(c, 0.5f, "impact");
            dai_anim_add(b, c, 1);
            dai_anim_on_event(b, on_event, &l3);
            dai_anim_play(b, "hit", 0.0f);
            for (int i = 0; i < 40; ++i) dai_anim_update(b, 0.1f);   // 4 s over a 1 s clip
            std::printf("   one-shot over 4 s: impact %d\n", l3.count("impact"));
            CHECK(l3.count("impact") == 1, "a one-shot clip fired 'impact' %d times, expected 1",
                  l3.count("impact"));
            dai_anim_destroy(b);
        }

        // backwards
        log.clear();
        dai_anim_restart(a, "walk", 0.0f, 0.9f);
        dai_anim_set_speed(a, -1.0f);
        dai_anim_update(a, 0.8f);                        // 0.9 -> 0.1: crosses right and left, once each
        std::printf("   backwards 0.9 -> 0.1: left %d, right %d\n", log.count("left"), log.count("right"));
        CHECK(log.count("left") == 1 && log.count("right") == 1,
              "backwards playback fired %d/%d, expected 1/1", log.count("left"), log.count("right"));
        dai_anim_set_speed(a, 1.0f);

        dai_anim_destroy(a);
    }

    // ================================================================ 7. speed
    {
        std::printf("-- speed\n");
        dai_anim *a = dai_anim_create(2);
        dai_anim_add(a, make_clip("run", 0.0f, 10.0f, 1.0f, 0.0f), 1);
        dai_anim_play(a, "run", 0.0f);
        dai_anim_set_speed(a, 2.0f);
        CHECK(near_f(dai_anim_speed(a), 2.0f), "speed reads back as %.3f", dai_anim_speed(a));
        dai_anim_update(a, 0.25f);
        CHECK(near_f(dai_anim_time(a), 0.5f, 1e-4f),
              "at speed 2, 0.25 s of dt advanced the head to %.4f, expected 0.5", dai_anim_time(a));
        uint32_t n = 0;
        const dai_anim_trs *p = dai_anim_pose(a, &n);
        CHECK(near_f(p[1].t.x, 5.0f, 1e-3f), "the pose is x = %.4f, expected 5", p[1].t.x);
        dai_anim_destroy(a);
    }

    // ============================================================= 8. 1D blend
    {
        std::printf("-- 1D locomotion blend\n");
        dai_anim *a = dai_anim_create(2);
        // Idle parks at 0, Walk at 10, Run at 20, with different cycle lengths
        // so the time synchronisation has something to prove.
        dai_anim_add(a, make_clip("Idle", 0.0f, 0.0f, 1.0f, 0.0f), 1);
        dai_anim_add(a, make_clip("Walk", 10.0f, 10.0f, 1.2f, 0.0f), 1);
        dai_anim_add(a, make_clip("Run",  20.0f, 20.0f, 0.6f, 0.0f), 1);
        const char *clips[3] = { "Idle", "Walk", "Run" };
        const float speeds[3] = { 0.0f, 2.0f, 6.0f };
        CHECK(dai_anim_blend1d(a, "Locomotion", clips, speeds, 3) == 1, "defining the blend failed");

        dai_anim_play(a, "Locomotion", 0.0f);
        uint32_t n = 0;

        dai_anim_set_parameter(a, 0.0f);
        dai_anim_update(a, 0.016f);
        const dai_anim_trs *p = dai_anim_pose(a, &n);
        CHECK(near_f(p[1].t.x, 0.0f, 1e-3f), "at speed 0 the blend is x = %.4f, expected 0", p[1].t.x);

        dai_anim_set_parameter(a, 1.0f);                 // half way between Idle and Walk
        dai_anim_update(a, 0.016f);
        p = dai_anim_pose(a, &n);
        std::printf("   parameter 1.0 -> x = %.3f (Idle %.2f, Walk %.2f)\n", p[1].t.x,
                    dai_anim_member_weight(a, "Idle"), dai_anim_member_weight(a, "Walk"));
        CHECK(near_f(p[1].t.x, 5.0f, 1e-2f), "at speed 1 the blend is x = %.4f, expected 5", p[1].t.x);
        CHECK(near_f(dai_anim_member_weight(a, "Idle"), 0.5f, 1e-2f),
              "Idle weighs %.3f", dai_anim_member_weight(a, "Idle"));

        dai_anim_set_parameter(a, 4.0f);                 // half way between Walk and Run
        dai_anim_update(a, 0.016f);
        p = dai_anim_pose(a, &n);
        CHECK(near_f(p[1].t.x, 15.0f, 1e-2f), "at speed 4 the blend is x = %.4f, expected 15", p[1].t.x);
        CHECK(near_f(dai_anim_member_weight(a, "Idle"), 0.0f, 1e-3f),
              "Idle is still in the pose at speed 4 (%.3f)", dai_anim_member_weight(a, "Idle"));

        dai_anim_set_parameter(a, 99.0f);                // past the end: pure Run
        dai_anim_update(a, 0.016f);
        p = dai_anim_pose(a, &n);
        CHECK(near_f(p[1].t.x, 20.0f, 1e-2f), "past the last threshold x = %.4f, expected 20", p[1].t.x);
        CHECK(dai_anim_is_playing(a, "Run") == 1, "isPlaying does not see a blend member");
        CHECK(dai_anim_is_playing(a, "Locomotion") == 1, "isPlaying does not see the blend itself");

        // time synchronisation: the head is normalised, so at parameter 2
        // (pure Walk, a 1.2 s cycle) 0.6 s of dt is half a cycle
        dai_anim_set_parameter(a, 2.0f);
        dai_anim_restart(a, "Locomotion", 0.0f, 0.0f);
        dai_anim_update(a, 0.6f);
        CHECK(near_f(dai_anim_normalized_time(a), 0.5f, 1e-3f),
              "0.6 s into a 1.2 s member is %.4f of a cycle, expected 0.5", dai_anim_normalized_time(a));

        // a blend crossfades like anything else
        dai_anim_play(a, "Idle", 0.2f);
        dai_anim_update(a, 0.1f);
        CHECK(near_f(dai_anim_weight(a, "Idle"), 0.5f, 1e-3f),
              "fading out of a blend, Idle weighs %.4f, expected 0.5", dai_anim_weight(a, "Idle"));
        dai_anim_destroy(a);
    }

    // ============================================================= 9. rest pose
    {
        std::printf("-- rest pose\n");
        dai_anim *a = dai_anim_create(3);
        std::vector<dai_anim_trs> rest(3, dai_anim_trs_identity());
        rest[2].t = dai_vec3{ 7, 8, 9 };
        rest[2].s = dai_vec3{ 2, 2, 2 };
        dai_anim_set_rest(a, rest.data(), 3);
        dai_anim_add(a, make_clip("A", 0.0f, 1.0f, 1.0f, 0.0f), 1);   // drives target 1 only
        dai_anim_play(a, "A", 0.0f);
        dai_anim_update(a, 0.5f);
        uint32_t n = 0;
        const dai_anim_trs *p = dai_anim_pose(a, &n);
        CHECK(near_f(p[2].t.x, 7.0f) && near_f(p[2].s.y, 2.0f),
              "a target no clip drives lost its rest transform: (%.3f, scale %.3f)", p[2].t.x, p[2].s.y);
        CHECK(near_f(p[1].t.x, 0.5f, 1e-3f), "the driven target is x = %.4f, expected 0.5", p[1].t.x);

        // stopping settles back to rest
        dai_anim_stop(a, 0.0f);
        dai_anim_update(a, 0.0f);
        p = dai_anim_pose(a, &n);
        CHECK(near_f(p[1].t.x, 0.0f), "after stop the pose is x = %.4f, expected the rest pose", p[1].t.x);
        CHECK(std::strcmp(dai_anim_current(a), "") == 0, "after stop current() is '%s'", dai_anim_current(a));
        dai_anim_destroy(a);
    }

    // ======================================================= 10. glTF -> clips
#ifdef DAI_ANIM_WITH_GLTF
    {
        std::printf("-- glTF import\n");
        const std::string dir = argc > 1 ? argv[1] : "assets/test";
        char err[256] = {0};
        dai_render_desc rd{}; rd.width = 64; rd.height = 64;
        dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
        if (!r) {
            std::printf("   renderer unavailable (%s) - skipping the import section\n", err);
        } else {
            dai_model *m = dai_gltf_load(r, (dir + "/skinned.glb").c_str(), err, sizeof(err));
            CHECK(m != nullptr, "loading skinned.glb failed: %s", err);
            if (m) {
                const dai_model_info info = dai_model_get_info(m);
                const uint32_t nodes = dai_model_hierarchy_count(m);
                std::printf("   %u hierarchy nodes, %u joints, %u animations\n",
                            nodes, info.joints, info.animations);
                CHECK(nodes > 0, "the model has no hierarchy");

                // the raw channel view the player is built from
                CHECK(dai_model_animation_channel_count(m, 0) >= 1, "animation 0 has no channels");
                dai_animation_channel ch{};
                CHECK(dai_model_animation_channel_at(m, 0, 0, &ch) == 1, "channel 0 could not be read");
                std::printf("   channel 0: node %d, path %u, interp %u, %u keys\n",
                            ch.node, ch.path, ch.interpolation, ch.keys);
                CHECK(ch.keys >= 2, "channel 0 has %u keys", ch.keys);
                CHECK(ch.times != nullptr && ch.values != nullptr, "the channel arrays are null");

                // clips
                dai_anim_clip *clips[8] = {0};
                const uint32_t nclips = dai_anim_clips_from_model(m, clips, 8);
                CHECK(nclips == info.animations, "%u clips out of %u animations", nclips, info.animations);
                CHECK(nclips >= 1 && clips[0] != nullptr, "no clip was built");
                if (nclips && clips[0]) {
                    const dai_animation_info ai = dai_model_animation_at(m, 0);
                    std::printf("   clip \"%s\": %.3f s, %u channels, %u targets\n",
                                dai_anim_clip_name(clips[0]), dai_anim_clip_duration(clips[0]),
                                dai_anim_clip_channel_count(clips[0]), dai_anim_clip_target_count(clips[0]));
                    CHECK(std::strcmp(dai_anim_clip_name(clips[0]), ai.name) == 0,
                          "the clip is called '%s', the model says '%s'", dai_anim_clip_name(clips[0]), ai.name);
                    CHECK(near_f(dai_anim_clip_duration(clips[0]), ai.duration, 1e-3f),
                          "the clip is %.4f s, the model says %.4f", dai_anim_clip_duration(clips[0]), ai.duration);
                    CHECK(dai_anim_clip_channel_count(clips[0]) == ai.channels,
                          "%u channels imported, %u in the file", dai_anim_clip_channel_count(clips[0]), ai.channels);
                    for (uint32_t i = 0; i < nclips; ++i) dai_anim_clip_destroy(clips[i]);
                }

                // the whole path, against the code that already shipped
                dai_anim *a = dai_anim_from_model(m);
                CHECK(a != nullptr, "dai_anim_from_model returned nothing");
                if (a) {
                    CHECK(dai_anim_targets(a) == nodes, "the player has %u targets, the model %u",
                          dai_anim_targets(a), nodes);
                    const dai_animation_info ai = dai_model_animation_at(m, 0);
                    dai_anim_play(a, ai.name, 0.0f);

                    const uint32_t maxj = info.joints ? info.joints : 1;
                    std::vector<float> mine(maxj * 16, 0.0f), theirs(maxj * 16, 0.0f);
                    float worst = 0.0f;
                    const float probes[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
                    for (float u : probes) {
                        const float t = u * ai.duration;
                        dai_anim_set_time(a, t);
                        const uint32_t na = dai_anim_apply(a, m, mine.data(), maxj);
                        const uint32_t nb = dai_model_pose(m, 0, t, theirs.data(), maxj);
                        CHECK(na == nb && na == info.joints,
                              "at t=%.2f the new path wrote %u matrices and the old one %u", t, na, nb);
                        for (uint32_t i = 0; i < maxj * 16; ++i) {
                            const float d = std::fabs(mine[i] - theirs[i]);
                            if (d > worst) worst = d;
                        }
                    }
                    std::printf("   clip -> player -> joints vs dai_model_pose: worst element differs by %.3e\n", worst);
                    CHECK(worst < 1e-5f,
                          "the new animation path disagrees with dai_model_pose by %.3e - one of them is wrong", worst);

                    // and it is not trivially identical because everything is zero
                    float spread = 0.0f;
                    dai_anim_set_time(a, ai.duration * 0.5f);
                    dai_anim_apply(a, m, mine.data(), maxj);
                    dai_anim_set_time(a, 0.0f);
                    dai_anim_apply(a, m, theirs.data(), maxj);
                    for (uint32_t i = 0; i < maxj * 16; ++i)
                        spread = std::fmax(spread, std::fabs(mine[i] - theirs[i]));
                    std::printf("   the pose actually moves between t=0 and t=half: %.4f\n", spread);
                    CHECK(spread > 0.01f, "the animation does not move the joints at all (%.5f)", spread);

                    // crossfading a real rig produces a real pose, not NaN
                    dai_anim_play(a, ai.name, 0.0f);
                    dai_anim_update(a, 0.1f);
                    dai_anim_apply(a, m, mine.data(), maxj);
                    bool finite = true;
                    for (uint32_t i = 0; i < maxj * 16; ++i) if (!std::isfinite(mine[i])) finite = false;
                    CHECK(finite, "the joint matrices contain NaN or inf");

                    dai_anim_destroy(a);
                }
                dai_model_release(r, m);
            }

            // a model with no animations must say so rather than hand back an
            // empty player that silently poses nothing
            dai_model *still = dai_gltf_load(r, (dir + "/blender_scene.glb").c_str(), err, sizeof(err));
            if (still) {
                if (dai_model_animation_count(still) == 0)
                    CHECK(dai_anim_from_model(still) == nullptr,
                          "a model with no animations produced a player anyway");
                dai_model_release(r, still);
            }
            dai_render_destroy(r);
        }
    }
#else
    (void)argc; (void)argv;
    std::printf("-- glTF import: not compiled in (build with -DDAI_ANIM_WITH_GLTF)\n");
#endif

    // ============================================================ 11. scripting
#ifdef DAI_ANIM_WITH_SCRIPT
    {
        std::printf("-- script binding\n");
        char err[512] = {0};
        dai_script *sc = dai_script_create(err, sizeof(err));
        CHECK(sc != nullptr, "script runtime creation failed: %s", err);
        if (sc) {
            g_script = sc;
            dai_anim *a = dai_anim_create(2);
            dai_anim_add(a, make_static("Idle", 0.0f, 0.0f, 1.0f), 1);
            dai_anim_clip *run = make_clip("Run", 0.0f, 10.0f, 1.0f, 0.0f);
            dai_anim_clip_add_event(run, 0.5f, "footstep");
            dai_anim_add(a, run, 1);
            dai_anim_on_event(a, forward_to_script, nullptr);

            dai_script_anim_host host{};
            host.play = h_play; host.restart = h_restart; host.stop = h_stop;
            host.is_playing = h_playing; host.current = h_current; host.weight = h_weight;
            host.finished = h_finished;
            host.get_speed = h_getspeed; host.set_speed = h_setspeed;
            host.get_time = h_gettime;   host.set_time = h_settime;
            host.get_normalized = h_getnorm;
            host.get_parameter = h_getparam; host.set_parameter = h_setparam;
            host.user = a;
            dai_script_bind_anim(sc, &host);

            // exactly the API the header advertises
            CHECK(dai_script_eval(sc, "state.ok = anim.play('Idle', 0.0);", "t", err, sizeof(err)) == DAI_OK,
                  "anim.play threw: %s", err);
            CHECK(dai_script_get_number(sc, "ok", -1) == 1, "anim.play returned %g",
                  dai_script_get_number(sc, "ok", -1));
            dai_anim_update(a, 0.0f);
            CHECK(std::strcmp(dai_anim_current(a), "Idle") == 0,
                  "the script did not start the clip (current is '%s')", dai_anim_current(a));

            CHECK(dai_script_eval(sc, "anim.speed = 1.5;", "t", err, sizeof(err)) == DAI_OK,
                  "anim.speed = threw: %s", err);
            CHECK(near_f(dai_anim_speed(a), 1.5f), "anim.speed = 1.5 gave %.3f", dai_anim_speed(a));
            dai_script_eval(sc, "state.sp = anim.speed;", "t", err, sizeof(err));
            CHECK(near_f((float)dai_script_get_number(sc, "sp", 0), 1.5f),
                  "reading anim.speed back gave %g", dai_script_get_number(sc, "sp", 0));
            dai_anim_set_speed(a, 1.0f);

            dai_script_eval(sc, "anim.crossfade('Run', 0.2); state.cur = anim.current();", "t", err, sizeof(err));
            dai_script_eval(sc, "state.p1 = anim.isPlaying('Run') ? 1 : 0;"
                                "state.p2 = anim.isPlaying('Nope') ? 1 : 0;", "t", err, sizeof(err));
            CHECK(dai_script_get_number(sc, "p1", -1) == 1, "anim.isPlaying('Run') said no");
            CHECK(dai_script_get_number(sc, "p2", -1) == 0, "anim.isPlaying('Nope') said yes");

            dai_anim_update(a, 0.1f);
            dai_script_eval(sc, "state.w = anim.weight('Run'); state.t = anim.time;", "t", err, sizeof(err));
            const double w = dai_script_get_number(sc, "w", -1);
            std::printf("   from JS: weight('Run') = %.3f, time = %.3f\n", w, dai_script_get_number(sc, "t", -1));
            CHECK(near_f((float)w, 0.5f, 1e-2f), "anim.weight('Run') is %.4f, expected 0.5", w);

            // anim.onEvent - by name, and the catch-all
            dai_script_eval(sc,
                "state.steps = 0; state.any = 0; state.lastClip = '';"
                "anim.onEvent('footstep', function(ev, clip, t) { state.steps++; state.lastClip = clip; });"
                "anim.onEvent(function(ev) { state.any++; });", "t", err, sizeof(err));
            dai_anim_restart(a, "Run", 0.0f, 0.0f);
            for (int i = 0; i < 20; ++i) dai_anim_update(a, 0.1f);   // two passes
            const double steps = dai_script_get_number(sc, "steps", -1);
            const double any   = dai_script_get_number(sc, "any", -1);
            std::printf("   two passes fired footstep %g times into JS (catch-all %g)\n", steps, any);
            CHECK(steps == 2, "the script handler ran %g times over two passes, expected 2", steps);
            CHECK(any == 2, "the catch-all handler ran %g times, expected 2", any);
            dai_script_eval(sc, "state.lc = state.lastClip === 'Run' ? 1 : 0;", "t", err, sizeof(err));
            CHECK(dai_script_get_number(sc, "lc", -1) == 1, "the handler was not told which clip fired");

            // a handler that throws is counted, and the others still run
            const uint32_t before = dai_script_error_count(sc);
            dai_script_eval(sc, "anim.onEvent('footstep', function() { null.x = 1; });", "t", err, sizeof(err));
            dai_script_eval(sc, "state.steps = 0;", "t", err, sizeof(err));
            dai_anim_restart(a, "Run", 0.0f, 0.0f);
            for (int i = 0; i < 10; ++i) dai_anim_update(a, 0.1f);
            CHECK(dai_script_error_count(sc) > before, "a throwing event handler was not counted");
            CHECK(dai_script_get_number(sc, "steps", -1) == 1,
                  "a throwing handler stopped the good one (%g)", dai_script_get_number(sc, "steps", -1));

            // the 1D blend parameter, and stop()
            dai_script_eval(sc, "anim.parameter = 3.5; state.pp = anim.parameter;", "t", err, sizeof(err));
            CHECK(near_f((float)dai_script_get_number(sc, "pp", -1), 3.5f),
                  "anim.parameter round trip gave %g", dai_script_get_number(sc, "pp", -1));
            dai_script_eval(sc, "anim.stop(0);", "t", err, sizeof(err));
            dai_anim_update(a, 0.0f);
            CHECK(std::strcmp(dai_anim_current(a), "") == 0, "anim.stop() left '%s' playing", dai_anim_current(a));

            // calling into an unbound runtime must not crash
            char e2[128] = {0};
            dai_script *bare = dai_script_create(e2, sizeof(e2));
            if (bare) {
                dai_script_anim_event(bare, "footstep", "Run", 0.5);   // no binding at all
                CHECK(dai_script_eval(bare, "1+1;", "t", e2, sizeof(e2)) == DAI_OK,
                      "the unbound runtime broke");
                dai_script_destroy(bare);
            }

            g_script = nullptr;
            dai_anim_destroy(a);
            dai_script_destroy(sc);
        }
    }
#endif

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
