// Degrees and quaternions, and the one claim a level script rests on: that
// "rotation.x = 36" tips a thing the way a person means it.
//
// This is not maths for its own sake. include/dai_euler.h was made because the
// component table could not answer "transform.rotation" at all - the editor
// had the conversion privately, so every script that turned a node by name was
// writing into a name nobody stored, and the staircase in INNEN's hall came
// out lying flat while the level file said 36 degrees. A conversion two hosts
// disagree about is the same bug with more steps, so there is now one, and
// this is what pins its meaning down.
//
//   ./build/test_euler

#include "dai_euler.h"

#include <cmath>
#include <cstdio>

static int good = 0, bad = 0;

static void ck(bool cond, const char *what) {
    if (cond) ++good;
    else { ++bad; std::printf("  FAIL %s\n", what); }
}

// v turned by q, written out - the same q*v*conj(q) the blockout generator and
// the door behaviour use.
static void rot(dai_quat q, const float *v, float *out) {
    float ux = q.x, uy = q.y, uz = q.z, s = q.w;
    float dot = ux * v[0] + uy * v[1] + uz * v[2];
    float cx = uy * v[2] - uz * v[1];
    float cy = uz * v[0] - ux * v[2];
    float cz = ux * v[1] - uy * v[0];
    float uu = ux * ux + uy * uy + uz * uz;
    out[0] = 2.0f * dot * ux + (s * s - uu) * v[0] + 2.0f * s * cx;
    out[1] = 2.0f * dot * uy + (s * s - uu) * v[1] + 2.0f * s * cy;
    out[2] = 2.0f * dot * uz + (s * s - uu) * v[2] + 2.0f * s * cz;
}

static bool near3(const float *a, float x, float y, float z, float eps = 1e-3f) {
    return std::fabs(a[0] - x) < eps && std::fabs(a[1] - y) < eps &&
           std::fabs(a[2] - z) < eps;
}

int main() {
    const float fwd[3] = { 0, 0, -1 };      // where a camera looks at yaw 0
    const float up[3]  = { 0, 1, 0 };
    float r[3];

    // ---- the claim the staircase stands on -------------------------------
    // A ramp under a flight of stairs is a flat box tipped about X. Tipping it
    // by a POSITIVE angle has to lift the -Z end: the stairs of INNEN climb
    // away from the player, and if the sign were the other way the collider
    // would go through the floor and the whole flight would be decoration.
    float deg[3] = { 35.9f, 0, 0 };
    rot(dai_euler_to_quat(deg), fwd, r);
    ck(r[1] > 0.5f, "+X rotation lifts the -Z end (the stair ramp's pitch)");
    ck(std::fabs(r[0]) < 1e-4f, "an X rotation does not move anything sideways");

    // ---- yaw, the way every door in the level is placed ------------------
    float y90[3] = { 0, 90, 0 };
    rot(dai_euler_to_quat(y90), fwd, r);
    ck(near3(r, -1, 0, 0), "yaw 90 turns -Z into -X");
    float y180[3] = { 0, 180, 0 };
    rot(dai_euler_to_quat(y180), fwd, r);
    ck(near3(r, 0, 0, 1), "yaw 180 turns the staircase around");

    // ---- roll --------------------------------------------------------------
    float z90[3] = { 0, 0, 90 };
    rot(dai_euler_to_quat(z90), up, r);
    ck(near3(r, -1, 0, 0), "roll 90 lays +Y onto -X (the handset in the box)");

    // ---- there and back ----------------------------------------------------
    // Round tripping is the part that keeps the inspector honest: a node read
    // out of the document and written back unedited must not drift.
    const float cases[][3] = {
        { 0, 0, 0 }, { 35.9f, 0, 0 }, { 0, 180, 0 }, { 0, 0, 90 },
        { 12, -34, 56 }, { -80, 15, -170 }, { 45, 45, 45 }, { -12.5f, 0.25f, 179.f }
    };
    for (const auto &c : cases) {
        dai_quat q = dai_euler_to_quat(c);
        float back[3];
        dai_quat_to_euler(q, back);
        dai_quat q2 = dai_euler_to_quat(back);
        // The SPELLING may differ (two triples name the same turn); the
        // orientation may not. Compare what each does to two axes.
        float a1[3], a2[3], b1[3], b2[3];
        rot(q, fwd, a1); rot(q2, fwd, a2);
        rot(q, up, b1);  rot(q2, up, b2);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "round trip of (%.1f, %.1f, %.1f)",
                      c[0], c[1], c[2]);
        ck(near3(a1, a2[0], a2[1], a2[2], 2e-3f) &&
           near3(b1, b2[0], b2[1], b2[2], 2e-3f), msg);
    }

    // Gimbal lock is clamped, not NaN: a script that writes pitch 90 gets a
    // usable orientation back rather than a node that vanishes.
    float lock[3] = { 0, 90, 0 };
    dai_quat q = dai_euler_to_quat(lock);
    float out[3];
    dai_quat_to_euler(q, out);
    ck(out[0] == out[0] && out[1] == out[1] && out[2] == out[2],
       "pitch 90 comes back as numbers, not NaN");
    // asin() at exactly 1 loses the last digits: 89.98 is the same orientation,
    // and a tighter bound here would be a test of float, not of the conversion.
    ck(std::fabs(out[1] - 90.0f) < 0.05f, "pitch 90 comes back as pitch 90");

    std::printf("%s: %d euler checks, %d wrong\n", bad ? "FAILED" : "ok", good, bad);
    std::printf("%d passed, %d failed\n", good, bad);
    return bad ? 1 : 0;
}
