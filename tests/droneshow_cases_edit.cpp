// The document under an editor's hands: moving a figure, and colouring it.
//
// Implements show_cases_edit() - see tests/droneshow_cases.hpp.
//
// Everything the other four suites test is a pure function of a fixed input.
// This one is about the two operations a user performs with a mouse - drag the
// gizmo, click the swatch - and those are the operations where an engine
// quietly loses a guarantee, because they run between the solve and the sign
// off rather than inside either.
//
//   [7a] IDENTITY IS A COPY, not arithmetic. A formation that has never been
//        moved has world points bit for bit equal to what was handed in. Every
//        show written before the transform existed has to solve to the plan it
//        solved to before; (x - p) * 1 + p is not x for every float x, so this
//        is a real check and not a tautology.
//   [7b] a translation moves every point by exactly the same vector, and the
//        LOCAL points are untouched - the move is non destructive, which is the
//        whole reason the split exists.
//   [7c] scaling happens about the figure's own centroid: the centre of the
//        cloud does not move, and every pairwise distance scales by the factor.
//        A scale that dragged the figure towards the show origin would be a
//        gizmo nobody could aim.
//   [7d] rotating by 90 degrees about Y maps (x, z) -> (z, -x), and rotating
//        by 360 degrees comes back to where it started within a float's worth
//        of rounding. The order is X then Y then Z, and a rotation composed the
//        other way round is a figure that faces the wrong way.
//   [7e] THE TRAP THE GIZMO OPENS. min_distance is a world distance, so a
//        figure legal at scale 1.0 is a collision at 0.5.
//        dai_show_formation_min_spacing has to say so - and it has to agree
//        with an O(n^2) reference, because a grid that misses the closest pair
//        is worse than no check at all.
//   [7f] a zero scale is REFUSED. It is not a very small figure, it is the
//        whole fleet at one coordinate.
//   [7g] moving a figure throws the plan away; colouring one does NOT. That
//        asymmetry is the point: geometry invalidates a validation, a swatch
//        does not.
//   [7h] the figure colour override paints every drone and is reversible - the
//        points underneath survive it.
//   [7i] painting a point UNDER an override flattens first: nothing on screen
//        jumps, the override is gone afterwards, and the painted point is the
//        colour that was asked for while every other point keeps what the
//        override had given it.
//   [7j] RECOLOUR EQUALS RE-SOLVE. Repaint a figure and patch the existing
//        plan, then solve the same document from scratch: every keyframe of
//        every drone has to come out identical, colour included. If the fast
//        path and the honest path disagree, the fast path is a lie the
//        preview tells.
//   [7k] the file round trips: transform, pivot and colour survive save and
//        load, the local points are what was stored, and a version 1 file - one
//        with no transform line at all - still loads and still has world points
//        equal to its stored points.
#include "droneshow_cases.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

dai_show_settings edit_settings(uint32_t drones, float min_d) {
    dai_show_settings s = dai_show_settings_default();
    s.drone_count    = drones;
    s.min_distance_m = min_d;
    s.fence_half_x   = 0.0f;      // unfenced: this suite is about the document
    s.fence_half_z   = 0.0f;
    s.fence_top_m    = 0.0f;
    return s;
}

// The closest pair the slow way, so the grid has something to be wrong against.
float closest_pair_n2(const dai_show_point *p, uint32_t n) {
    double best = -1.0;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = i + 1; j < n; ++j) {
            double dx = (double)p[j].x - (double)p[i].x;
            double dy = (double)p[j].y - (double)p[i].y;
            double dz = (double)p[j].z - (double)p[i].z;
            double d2 = dx * dx + dy * dy + dz * dz;
            if (best < 0.0 || d2 < best) best = d2;
        }
    return best < 0.0 ? -1.0f : (float)std::sqrt(best);
}

dai_vec3 centroid(const dai_show_point *p, uint32_t n) {
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (uint32_t i = 0; i < n; ++i) { sx += p[i].x; sy += p[i].y; sz += p[i].z; }
    dai_vec3 c;
    c.x = (float)(sx / (double)n);
    c.y = (float)(sy / (double)n);
    c.z = (float)(sz / (double)n);
    return c;
}

// Two grids, far enough apart that the transition is a real move and close
// enough that it stays inside every default limit.
dai_show *two_figure_show(uint32_t n, float spacing) {
    dai_show_settings s = edit_settings(n, spacing * 0.9f);
    dai_show *sh = dai_show_create(&s);
    std::vector<dai_show_point> a(n), b(n);
    show_grid_formation(a.data(), n, spacing, dai_vec3{ 0.0f, 50.0f, 0.0f });
    show_grid_formation(b.data(), n, spacing, dai_vec3{ 30.0f, 70.0f, 12.0f });
    dai_show_formation_add(sh, "start", "", a.data(), n, 3.0f);
    dai_show_formation_add(sh, "next",  "", b.data(), n, 3.0f);
    return sh;
}

int keys_identical(const dai_show_plan *p, const dai_show_plan *q, uint32_t *bad_drone) {
    if (!p || !q) return 0;
    uint32_t n = dai_show_plan_drone_count(p);
    if (n != dai_show_plan_drone_count(q)) return 0;
    for (uint32_t d = 0; d < n; ++d) {
        uint32_t kp = dai_show_plan_keyframe_count(p, d);
        if (kp != dai_show_plan_keyframe_count(q, d)) { if (bad_drone) *bad_drone = d; return 0; }
        for (uint32_t k = 0; k < kp; ++k) {
            dai_show_key a, b;
            dai_show_plan_key_at(p, d, k, &a);
            dai_show_plan_key_at(q, d, k, &b);
            if (a.t != b.t || a.profile != b.profile ||
                a.p.x != b.p.x || a.p.y != b.p.y || a.p.z != b.p.z ||
                a.p.r != b.p.r || a.p.g != b.p.g || a.p.b != b.p.b || a.p.w != b.p.w) {
                if (bad_drone) *bad_drone = d;
                return 0;
            }
        }
    }
    return 1;
}

} // namespace

int show_cases_edit(void) {
    const int before = g_show_fail;

    // ---- [7a] identity is a copy ------------------------------------------
    show_section("editing - a figure that was never moved is untouched");
    {
        const uint32_t n = 64;
        dai_show_settings s = edit_settings(n, 2.0f);
        dai_show *sh = dai_show_create(&s);
        std::vector<dai_show_point> a(n);
        show_grid_formation(a.data(), n, 3.0f, dai_vec3{ 7.125f, 51.375f, -3.0625f });
        dai_show_formation_add(sh, "f", "", a.data(), n, 2.0f);

        dai_show_transform xf;
        CHECK(dai_show_formation_get_transform(sh, 0, &xf), "[7a] a fresh formation has no transform");
        CHECK(dai_show_transform_is_identity(&xf),
              "[7a] a fresh formation did not start at identity");

        const dai_show_point *w = dai_show_formation_points(sh, 0);
        const dai_show_point *l = dai_show_formation_local_points(sh, 0);
        int world_exact = 1, local_exact = 1;
        for (uint32_t i = 0; i < n; ++i) {
            if (w[i].x != a[i].x || w[i].y != a[i].y || w[i].z != a[i].z) world_exact = 0;
            if (l[i].x != a[i].x || l[i].y != a[i].y || l[i].z != a[i].z) local_exact = 0;
        }
        CHECK(world_exact, "[7a] an untouched formation's world points are not bit for bit "
                           "what was handed in - every show saved before the transform "
                           "existed now solves to a different plan");
        CHECK(local_exact, "[7a] the local points are not what was handed in");
        dai_show_destroy(sh);
    }

    // ---- [7b] a translation, and what it leaves alone -----------------------
    show_section("editing - moving a figure moves the world, not the model");
    {
        const uint32_t n = 49;
        dai_show_settings s = edit_settings(n, 2.0f);
        dai_show *sh = dai_show_create(&s);
        std::vector<dai_show_point> a(n);
        show_grid_formation(a.data(), n, 3.0f, dai_vec3{ 0.0f, 50.0f, 0.0f });
        dai_show_formation_add(sh, "f", "", a.data(), n, 2.0f);

        dai_show_transform xf = dai_show_transform_identity();
        xf.position = dai_vec3{ 12.5f, -4.25f, 30.0f };
        CHECK(dai_show_formation_set_transform(sh, 0, &xf), "[7b] the move was refused");

        const dai_show_point *w = dai_show_formation_points(sh, 0);
        const dai_show_point *l = dai_show_formation_local_points(sh, 0);
        float worst = 0.0f;
        int   local_moved = 0;
        for (uint32_t i = 0; i < n; ++i) {
            worst = std::max(worst, std::fabs(w[i].x - (a[i].x + 12.5f)));
            worst = std::max(worst, std::fabs(w[i].y - (a[i].y - 4.25f)));
            worst = std::max(worst, std::fabs(w[i].z - (a[i].z + 30.0f)));
            if (l[i].x != a[i].x || l[i].y != a[i].y || l[i].z != a[i].z) local_moved = 1;
        }
        CHECK(worst < 1e-3f, "[7b] a translation is off by up to %.5f m", (double)worst);
        CHECK(!local_moved, "[7b] a move rewrote the sampled points - the edit is destructive");

        // And it is reversible, exactly: back to identity is back to the copy.
        dai_show_transform id = dai_show_transform_identity();
        dai_show_formation_set_transform(sh, 0, &id);
        const dai_show_point *w2 = dai_show_formation_points(sh, 0);
        int back_exact = 1;
        for (uint32_t i = 0; i < n; ++i)
            if (w2[i].x != a[i].x || w2[i].y != a[i].y || w2[i].z != a[i].z) back_exact = 0;
        CHECK(back_exact, "[7b] moving a figure and moving it back did not restore it exactly");
        dai_show_destroy(sh);
    }

    // ---- [7c] scale is about the figure's own centre ------------------------
    show_section("editing - a scale shrinks the figure where it stands");
    {
        const uint32_t n = 64;
        dai_show_settings s = edit_settings(n, 1.0f);
        dai_show *sh = dai_show_create(&s);
        std::vector<dai_show_point> a(n);
        show_grid_formation(a.data(), n, 4.0f, dai_vec3{ 40.0f, 60.0f, -20.0f });
        dai_show_formation_add(sh, "f", "", a.data(), n, 2.0f);
        const dai_vec3 c0 = centroid(a.data(), n);
        const float    d0 = closest_pair_n2(a.data(), n);

        dai_show_transform xf = dai_show_transform_identity();
        xf.scale = dai_vec3{ 0.5f, 0.5f, 0.5f };
        dai_show_formation_set_transform(sh, 0, &xf);
        const dai_show_point *w = dai_show_formation_points(sh, 0);
        const dai_vec3 c1 = centroid(w, n);
        const float    d1 = closest_pair_n2(w, n);

        float drift = std::max(std::fabs(c1.x - c0.x),
                      std::max(std::fabs(c1.y - c0.y), std::fabs(c1.z - c0.z)));
        CHECK(drift < 1e-3f,
              "[7c] scaling moved the figure's centre by %.4f m - the pivot is not the "
              "centroid, so the gizmo cannot be aimed", (double)drift);
        CHECK(std::fabs(d1 - d0 * 0.5f) < 1e-3f,
              "[7c] halving the figure gave a closest pair of %.4f m where %.4f m was due",
              (double)d1, (double)(d0 * 0.5f));
        dai_show_destroy(sh);
    }

    // ---- [7d] rotation, and which way round ---------------------------------
    show_section("editing - ninety degrees about Y is ninety degrees about Y");
    {
        const uint32_t n = 16;
        dai_show_settings s = edit_settings(n, 1.0f);
        dai_show *sh = dai_show_create(&s);
        std::vector<dai_show_point> a(n);
        show_grid_formation(a.data(), n, 5.0f, dai_vec3{ 0.0f, 50.0f, 0.0f });
        dai_show_formation_add(sh, "f", "", a.data(), n, 2.0f);
        const dai_vec3 c = centroid(a.data(), n);

        dai_show_transform xf = dai_show_transform_identity();
        xf.rotation_deg = dai_vec3{ 0.0f, 90.0f, 0.0f };
        dai_show_formation_set_transform(sh, 0, &xf);
        const dai_show_point *w = dai_show_formation_points(sh, 0);
        float worst = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            // Right handed, +Y up: (x, z) about the pivot goes to (z, -x).
            float lx = a[i].x - c.x, lz = a[i].z - c.z;
            worst = std::max(worst, std::fabs(w[i].x - (c.x + lz)));
            worst = std::max(worst, std::fabs(w[i].z - (c.z - lx)));
            worst = std::max(worst, std::fabs(w[i].y - a[i].y));
        }
        CHECK(worst < 1e-3f, "[7d] a 90 degree turn about Y is off by up to %.5f m", (double)worst);

        xf.rotation_deg = dai_vec3{ 0.0f, 360.0f, 0.0f };
        dai_show_formation_set_transform(sh, 0, &xf);
        const dai_show_point *w2 = dai_show_formation_points(sh, 0);
        float loop = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            loop = std::max(loop, std::fabs(w2[i].x - a[i].x));
            loop = std::max(loop, std::fabs(w2[i].y - a[i].y));
            loop = std::max(loop, std::fabs(w2[i].z - a[i].z));
        }
        CHECK(loop < 1e-3f, "[7d] a full turn came back %.5f m off", (double)loop);
        dai_show_destroy(sh);
    }

    // ---- [7e] the trap: a scale is a collision ------------------------------
    show_section("editing - the spacing after the move, measured not assumed");
    {
        const uint32_t n = 400;
        dai_show_settings s = edit_settings(n, 2.0f);
        dai_show *sh = dai_show_create(&s);
        std::vector<dai_show_point> a(n);
        show_grid_formation(a.data(), n, 3.0f, dai_vec3{ 0.0f, 60.0f, 0.0f });
        dai_show_formation_add(sh, "f", "", a.data(), n, 2.0f);

        float grid_said = dai_show_formation_min_spacing(sh, 0);
        float truth     = closest_pair_n2(dai_show_formation_points(sh, 0), n);
        CHECK(std::fabs(grid_said - truth) < 1e-4f,
              "[7e] the grid calls the closest pair %.5f m where every-pair finds %.5f m",
              (double)grid_said, (double)truth);
        CHECK(grid_said >= s.min_distance_m,
              "[7e] the figure was already illegal before it was touched");

        const float factors[] = { 0.5f, 0.61f, 1.7f };
        for (int fi = 0; fi < 3; ++fi) {
            dai_show_transform xf = dai_show_transform_identity();
            xf.scale = dai_vec3{ factors[fi], factors[fi], factors[fi] };
            dai_show_formation_set_transform(sh, 0, &xf);
            float g = dai_show_formation_min_spacing(sh, 0);
            float t = closest_pair_n2(dai_show_formation_points(sh, 0), n);
            CHECK(std::fabs(g - t) < 1e-3f,
                  "[7e] at scale %.2f the grid says %.5f m and every-pair says %.5f m",
                  (double)factors[fi], (double)g, (double)t);
        }
        // And the one that matters: at 0.5 the figure has become illegal, and
        // the number the panel would print says so.
        dai_show_transform half = dai_show_transform_identity();
        half.scale = dai_vec3{ 0.5f, 0.5f, 0.5f };
        dai_show_formation_set_transform(sh, 0, &half);
        CHECK(dai_show_formation_min_spacing(sh, 0) < s.min_distance_m,
              "[7e] halving a figure built at the minimum distance did not break it - "
              "then the check is measuring the wrong points");
        dai_show_destroy(sh);
    }

    // ---- [7f] a zero scale is refused ---------------------------------------
    {
        dai_show *sh = two_figure_show(25, 3.0f);
        dai_show_transform xf = dai_show_transform_identity();
        xf.scale = dai_vec3{ 1.0f, 0.0f, 1.0f };
        CHECK(!dai_show_formation_set_transform(sh, 0, &xf),
              "[7f] a zero scale was accepted - that is the whole fleet at one coordinate");
        dai_show_transform now;
        dai_show_formation_get_transform(sh, 0, &now);
        CHECK(dai_show_transform_is_identity(&now),
              "[7f] a refused transform was written anyway");
        dai_show_destroy(sh);
    }

    // ---- [7g] geometry invalidates, colour does not -------------------------
    show_section("editing - a move throws the solve away, a swatch does not");
    {
        dai_show *sh = two_figure_show(36, 4.0f);
        char err[256] = { 0 };
        dai_show_solve(sh, err, sizeof(err));
        CHECK(dai_show_get_plan(sh) != nullptr, "[7g] the show would not solve: %s", err);

        dai_show_formation_set_colour(sh, 1, 1, 255, 0, 0, 0);
        CHECK(dai_show_get_plan(sh) != nullptr,
              "[7g] colouring a figure destroyed the plan - then every swatch click costs "
              "a ten thousand drone solve");

        dai_show_transform xf = dai_show_transform_identity();
        xf.position = dai_vec3{ 1.0f, 0.0f, 0.0f };
        dai_show_formation_set_transform(sh, 1, &xf);
        CHECK(dai_show_get_plan(sh) == nullptr,
              "[7g] moving a figure kept the plan - and with it a 'no conflicts' badge "
              "for a show that no longer exists");
        dai_show_destroy(sh);
    }

    // ---- [7h] the figure override, and its way back -------------------------
    show_section("editing - three levels of colour, in the right order");
    {
        const uint32_t n = 25;
        dai_show *sh = two_figure_show(n, 4.0f);
        const dai_show_point *l = dai_show_formation_local_points(sh, 0);
        std::vector<dai_show_point> before_pts(l, l + n);

        dai_show_formation_set_colour(sh, 0, 1, 10, 200, 30, 7);
        const dai_show_point *w = dai_show_formation_points(sh, 0);
        int all = 1;
        for (uint32_t i = 0; i < n; ++i)
            if (w[i].r != 10 || w[i].g != 200 || w[i].b != 30 || w[i].w != 7) all = 0;
        CHECK(all, "[7h] the figure colour did not reach every drone");

        int on = 0; uint8_t rgbw[4] = { 0, 0, 0, 0 };
        dai_show_formation_get_colour(sh, 0, &on, rgbw);
        CHECK(on == 1 && rgbw[0] == 10 && rgbw[1] == 200 && rgbw[2] == 30 && rgbw[3] == 7,
              "[7h] the override does not read back as it was set");

        dai_show_formation_info info;
        dai_show_formation_get(sh, 0, &info);
        CHECK(info.colour_override == 1 && info.colour[1] == 200,
              "[7h] the formation info does not carry the override");

        // Off again: the points underneath were never touched.
        dai_show_formation_set_colour(sh, 0, 0, 10, 200, 30, 7);
        const dai_show_point *w2 = dai_show_formation_points(sh, 0);
        int restored = 1;
        for (uint32_t i = 0; i < n; ++i)
            if (w2[i].r != before_pts[i].r || w2[i].g != before_pts[i].g ||
                w2[i].b != before_pts[i].b || w2[i].w != before_pts[i].w) restored = 0;
        CHECK(restored, "[7h] switching the override off did not bring the mesh colours back");
        dai_show_destroy(sh);
    }

    // ---- [7i] painting under an override flattens ---------------------------
    {
        const uint32_t n = 25;
        dai_show *sh = two_figure_show(n, 4.0f);
        dai_show_formation_set_colour(sh, 0, 1, 8, 8, 200, 0);
        const uint32_t one = 3;
        dai_show_formation_set_point_colour(sh, 0, &one, 1, 255, 0, 0, 0);

        int on = 1;
        dai_show_formation_get_colour(sh, 0, &on, nullptr);
        CHECK(on == 0, "[7i] painting a point left the override on - the edit is invisible");

        const dai_show_point *w = dai_show_formation_points(sh, 0);
        CHECK(w[one].r == 255 && w[one].g == 0 && w[one].b == 0,
              "[7i] the painted point is %u %u %u, not the colour that was asked for",
              w[one].r, w[one].g, w[one].b);
        int others_kept = 1;
        for (uint32_t i = 0; i < n; ++i) {
            if (i == one) continue;
            if (w[i].r != 8 || w[i].g != 8 || w[i].b != 200) others_kept = 0;
        }
        CHECK(others_kept, "[7i] flattening the override did not keep the other drones' colour "
                           "- the picture jumped when the user clicked");

        // And "all of them" is the same call with no index list.
        dai_show_formation_set_point_colour(sh, 0, nullptr, 0, 1, 2, 3, 4);
        const dai_show_point *w3 = dai_show_formation_points(sh, 0);
        int painted_all = 1;
        for (uint32_t i = 0; i < n; ++i)
            if (w3[i].r != 1 || w3[i].g != 2 || w3[i].b != 3 || w3[i].w != 4) painted_all = 0;
        CHECK(painted_all, "[7i] painting with no index list did not reach every point");
        dai_show_destroy(sh);
    }

    // ---- [7j] recolour equals re-solve --------------------------------------
    show_section("editing - a repainted plan is the plan a fresh solve would build");
    {
        const uint32_t n = 36;
        char err[256] = { 0 };

        // One document: solve, then repaint, and keep the patched plan.
        dai_show *fast = two_figure_show(n, 4.0f);
        dai_show_solve(fast, err, sizeof(err));
        dai_show_formation_set_colour(fast, 0, 1, 200, 20, 60, 3);
        dai_show_formation_set_colour(fast, 1, 1, 20, 60, 200, 9);
        const uint32_t two[2] = { 4, 11 };
        dai_show_formation_set_point_colour(fast, 1, two, 2, 0, 255, 0, 1);
        const dai_show_plan *pf = dai_show_get_plan(fast);
        CHECK(pf != nullptr, "[7j] the repainted show lost its plan");

        // Another document: the same edits FIRST, then one honest solve.
        dai_show *slow = two_figure_show(n, 4.0f);
        dai_show_formation_set_colour(slow, 0, 1, 200, 20, 60, 3);
        dai_show_formation_set_colour(slow, 1, 1, 20, 60, 200, 9);
        dai_show_formation_set_point_colour(slow, 1, two, 2, 0, 255, 0, 1);
        dai_show_solve(slow, err, sizeof(err));
        const dai_show_plan *ps = dai_show_get_plan(slow);

        uint32_t bad = 0xFFFFFFFFu;
        CHECK(keys_identical(pf, ps, &bad),
              "[7j] the repainted plan and a fresh solve disagree (first at drone %u) - "
              "then the preview after a colour edit is not the show that would be exported",
              bad);
        dai_show_destroy(fast);
        dai_show_destroy(slow);
    }

    // ---- [7k] it all survives the file --------------------------------------
    show_section("editing - the transform and the colour go through the file");
    {
        const uint32_t n = 25;
        dai_show *sh = two_figure_show(n, 4.0f);
        dai_show_transform xf = dai_show_transform_identity();
        xf.position     = dai_vec3{ 3.5f, -2.25f, 11.0f };
        xf.rotation_deg = dai_vec3{ 0.0f, 33.0f, 0.0f };
        xf.scale        = dai_vec3{ 1.25f, 1.25f, 1.25f };
        dai_show_formation_set_transform(sh, 1, &xf);
        dai_show_formation_set_colour(sh, 1, 1, 40, 90, 250, 12);

        std::vector<dai_show_point> world_before(dai_show_formation_points(sh, 1),
                                                 dai_show_formation_points(sh, 1) + n);

        char err[256] = { 0 };
        const char *path = "build/edit_roundtrip.dshow";
        CHECK(dai_show_save(sh, path, err, sizeof(err)) == DAI_OK,
              "[7k] the show would not save: %s", err);
        dai_show *back = dai_show_load(path, err, sizeof(err));
        CHECK(back != nullptr, "[7k] the show would not load: %s", err);
        if (back) {
            dai_show_transform xb;
            dai_show_formation_get_transform(back, 1, &xb);
            CHECK(xb.position.x == xf.position.x && xb.position.y == xf.position.y &&
                  xb.position.z == xf.position.z && xb.rotation_deg.y == xf.rotation_deg.y &&
                  xb.scale.x == xf.scale.x,
                  "[7k] the transform did not survive the file");
            int on = 0; uint8_t rgbw[4] = { 0, 0, 0, 0 };
            dai_show_formation_get_colour(back, 1, &on, rgbw);
            CHECK(on == 1 && rgbw[0] == 40 && rgbw[1] == 90 && rgbw[2] == 250 && rgbw[3] == 12,
                  "[7k] the colour override did not survive the file");

            const dai_show_point *w = dai_show_formation_points(back, 1);
            float worst = 0.0f;
            for (uint32_t i = 0; i < n; ++i) {
                worst = std::max(worst, std::fabs(w[i].x - world_before[i].x));
                worst = std::max(worst, std::fabs(w[i].y - world_before[i].y));
                worst = std::max(worst, std::fabs(w[i].z - world_before[i].z));
            }
            CHECK(worst == 0.0f,
                  "[7k] a saved and reloaded figure stands up to %.6f m from where it stood - "
                  "the pivot or the local points did not round trip exactly", (double)worst);
            dai_show_destroy(back);
        }

        // The formation that was NOT moved wrote no transform line at all, and
        // that is what makes a version 1 file - and every diff - readable.
        FILE *f = fopen(path, "rb");
        std::string text;
        if (f) {
            char buf[4096]; size_t got;
            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
            fclose(f);
        }
        size_t first = text.find("formation");
        size_t second = text.find("formation", first + 1);
        CHECK(text.find("transform", first) != std::string::npos,
              "[7k] the moved figure wrote no transform line");
        CHECK(text.find("transform", first) > second,
              "[7k] the untouched figure wrote a transform line - then every old show "
              "file changes the moment it is opened");
        dai_show_destroy(sh);
    }

    // A file with no transform anywhere - what version 1 looked like - still
    // loads, and its points are still where they were written.
    {
        const uint32_t n = 9;
        dai_show *sh = two_figure_show(n, 5.0f);
        char err[256] = { 0 };
        const char *path = "build/edit_v1_like.dshow";
        dai_show_save(sh, path, err, sizeof(err));
        std::vector<dai_show_point> was(dai_show_formation_points(sh, 0),
                                        dai_show_formation_points(sh, 0) + n);
        dai_show *back = dai_show_load(path, err, sizeof(err));
        CHECK(back != nullptr, "[7k] a show with no transform in it would not load: %s", err);
        if (back) {
            const dai_show_point *w = dai_show_formation_points(back, 0);
            int exact = 1;
            for (uint32_t i = 0; i < n; ++i)
                if (w[i].x != was[i].x || w[i].y != was[i].y || w[i].z != was[i].z) exact = 0;
            CHECK(exact, "[7k] a show with no transform did not reload bit for bit");
            dai_show_destroy(back);
        }
        dai_show_destroy(sh);
    }

    return g_show_fail - before;
}
