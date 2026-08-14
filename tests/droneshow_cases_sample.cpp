// Stage 1 under test: the minimum distance is a guarantee, not an aspiration.
//
// Implements show_cases_sample() - see tests/droneshow_cases.hpp.
//
// What has to be asserted here, and why each one is the assertion that would
// actually catch a regression:
//
//   [1a] exactly `count` points come back. Fewer is a solver that gave up and
//        did not say so.
//   [1b] EVERY pair is at least min_distance apart - measured over all pairs
//        for a small N, so the check itself cannot be the thing that is wrong.
//   [1c] surface mode puts points ON the surface (distance to the nearest
//        triangle below a tolerance), volume mode puts them inside.
//   [1d] silhouette mode concentrates points near the outline seen from
//        view_dir: measurably closer to the 2D contour than surface mode is.
//   [1e] an impossible request - N drones at d metres in a figure too small -
//        FAILS, before sampling, and names the size the figure needs to be.
//        This is the case that separates a tool from a crash generator.
//   [1f] colour arrives: a vertex-coloured mesh gives varied point colours, a
//        textured one samples the texture, an untextured one the base colour.
//   [1g] the same seed twice is bit identical; a different seed is not.
#include "droneshow_cases.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

// The descriptor every case starts from: one figure, one fleet, one spacing.
// Filling it in one place keeps the cases about what they test rather than
// about twenty lines of struct setup each.
dai_show_sample_desc desc_for(int mesh_kind, int mode, uint32_t count,
                              float min_d, float scale, uint64_t seed) {
    dai_show_test_mesh m = show_test_mesh(mesh_kind);
    dai_show_sample_desc d;
    std::memset(&d, 0, sizeof(d));
    d.positions    = m.positions;
    d.normals      = m.normals;
    d.uvs          = m.uvs;
    d.vertex_count = m.vertex_count;
    d.indices      = m.indices;
    d.index_count  = m.index_count;
    d.base_rgba    = 0xFF20C040u;              /* r=0x40 g=0xC0 b=0x20        */
    d.mode         = mode;
    d.count        = count;
    d.min_distance_m = min_d;
    d.scale        = scale;
    d.centre       = dai_vec3{ 0.0f, 80.0f, 0.0f };
    d.view_dir     = dai_vec3{ 0.0f, 0.0f, 1.0f };
    d.relax_iterations = 6;
    d.seed         = seed;
    return d;
}

// The measurement that has to be done the slow way. Every pair, no grid, no
// cleverness - a broadphase bug in the sampler would otherwise be checked by
// the same broadphase that caused it.
float closest_pair(const dai_show_point *p, uint32_t n) {
    float best = 1e30f;
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t j = i + 1; j < n; ++j) {
            float dx = p[i].x - p[j].x, dy = p[i].y - p[j].y, dz = p[i].z - p[j].z;
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < best) best = d;
        }
    }
    return best;
}

float radius_from(const dai_show_point &p, dai_vec3 c) {
    float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Distance from a point to the outline of a sphere seen along +Z: the circle of
// radius R in the XY plane through the centre.
float contour_error(const dai_show_point &p, dai_vec3 c, float R) {
    float dx = p.x - c.x, dy = p.y - c.y;
    return std::fabs(std::sqrt(dx * dx + dy * dy) - R);
}

uint32_t distinct_colours(const dai_show_point *p, uint32_t n) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = 0;
        for (; j < i; ++j)
            if (p[i].r == p[j].r && p[i].g == p[j].g && p[i].b == p[j].b) break;
        if (j == i) ++seen;
    }
    return seen;
}

}  // namespace

int show_cases_sample(void) {
    int before = g_show_fail;
    show_section("stage 1 - sampling: the minimum distance is a guarantee");

    const float MIN_D = 2.0f;
    const float R     = 40.0f;
    const dai_vec3 C  = dai_vec3{ 0.0f, 80.0f, 0.0f };

    // ---- [1a] exactly `count`, and [1b] every pair -------------------------
    std::vector<dai_show_point> surf(400);
    {
        dai_show_sample_desc d = desc_for(2, DAI_SHOW_SAMPLE_SURFACE, 400, MIN_D, R, 0xC0FFEEu);
        char err[256] = { 0 };
        uint32_t n = dai_show_sample(&d, surf.data(), (uint32_t)surf.size(), err, sizeof(err));
        CHECK(n == 400, "[1a] surface sampling returned %u of 400 points (%s)", n, err);
        if (n == 400) {
            float closest = closest_pair(surf.data(), n);
            CHECK(closest >= MIN_D - 1e-3f,
                  "[1b] the closest pair is %.4f m apart, the floor is %.2f m", (double)closest, (double)MIN_D);
        }

        // The output buffer is part of the contract too: fewer points than the
        // formation asked for must fail loudly rather than fill what fits.
        char err2[256] = { 0 };
        uint32_t n2 = dai_show_sample(&d, surf.data(), 100u, err2, sizeof(err2));
        CHECK(n2 == 0 && err2[0] != '\0', "[1a] a too small output buffer was not refused");
    }

    // ---- [1c] surface points are on the surface, volume points inside ------
    {
        float worst_off = 0.0f;
        for (uint32_t i = 0; i < 400; ++i)
            worst_off = std::max(worst_off, std::fabs(radius_from(surf[i], C) - R));
        // The sphere fixture is 24x32 facets: a point in the middle of a facet
        // sits a chord's sagitta inside the true radius, which is 0.09 m at
        // R = 40 m. Half a metre is that with room, and far tighter than any
        // point that had drifted off the surface would be.
        CHECK(worst_off < 0.5f, "[1c] a surface point is %.3f m off the sphere", (double)worst_off);

        std::vector<dai_show_point> vol(400);
        dai_show_sample_desc d = desc_for(2, DAI_SHOW_SAMPLE_VOLUME, 400, MIN_D, R, 0xC0FFEEu);
        char err[256] = { 0 };
        uint32_t n = dai_show_sample(&d, vol.data(), (uint32_t)vol.size(), err, sizeof(err));
        CHECK(n == 400, "[1c] volume sampling returned %u of 400 points (%s)", n, err);
        if (n == 400) {
            float outside = 0.0f, deepest = R;
            double mean = 0.0;
            for (uint32_t i = 0; i < n; ++i) {
                float r = radius_from(vol[i], C);
                outside = std::max(outside, r - R);
                deepest = std::min(deepest, r);
                mean += (double)r;
            }
            mean /= (double)n;
            CHECK(outside < 0.5f, "[1c] a volume point is %.3f m outside the sphere", (double)outside);
            CHECK(deepest < 0.4f * R, "[1c] the innermost volume point is still at r = %.1f m", (double)deepest);
            // Uniform in a ball has mean radius 3R/4; a shell would sit at R.
            CHECK(mean < 0.85 * (double)R, "[1c] volume mode filled a shell, mean radius %.1f m", mean);
            CHECK(closest_pair(vol.data(), n) >= MIN_D - 1e-3f, "[1b] volume mode broke the minimum distance");
        }
    }

    // ---- [1d] silhouette hugs the outline ----------------------------------
    {
        std::vector<dai_show_point> sil(400);
        dai_show_sample_desc d = desc_for(2, DAI_SHOW_SAMPLE_SILHOUETTE, 400, MIN_D, R, 0xC0FFEEu);
        char err[256] = { 0 };
        uint32_t n = dai_show_sample(&d, sil.data(), (uint32_t)sil.size(), err, sizeof(err));
        CHECK(n == 400, "[1d] silhouette sampling returned %u of 400 points (%s)", n, err);
        if (n == 400) {
            double sil_err = 0.0, surf_err = 0.0;
            for (uint32_t i = 0; i < n; ++i) {
                sil_err  += (double)contour_error(sil[i], C, R);
                surf_err += (double)contour_error(surf[i], C, R);
            }
            sil_err /= (double)n; surf_err /= (double)n;
            CHECK(sil_err < 0.4 * surf_err,
                  "[1d] silhouette points sit %.2f m off the outline, surface points %.2f m - no concentration",
                  sil_err, surf_err);
            CHECK(closest_pair(sil.data(), n) >= MIN_D - 1e-3f, "[1b] silhouette mode broke the minimum distance");
        }
    }

    // ---- [1e] the impossible request is refused, with a number -------------
    {
        dai_show_sample_desc d = desc_for(2, DAI_SHOW_SAMPLE_SURFACE, 500, MIN_D, 1.0f, 7u);
        dai_show_feasibility f;
        std::memset(&f, 0, sizeof(f));
        int ok = dai_show_sample_feasible(&d, &f);
        CHECK(ok == 0 && f.ok == 0, "[1e] 500 drones at 2 m on a 2 m sphere were called feasible");
        CHECK(f.max_points < 500u, "[1e] the packing bound says %u points fit on a 2 m sphere", f.max_points);
        CHECK(f.required_scale > 1.0f, "[1e] no scale up was asked for (%.3f)", (double)f.required_scale);
        CHECK(f.required_size_m > 2.0f, "[1e] the required size is %.2f m, no larger than the figure",
              (double)f.required_size_m);
        CHECK(f.achievable_spacing_m > 0.0f && f.achievable_spacing_m < MIN_D,
              "[1e] the achievable spacing is %.3f m", (double)f.achievable_spacing_m);

        std::vector<dai_show_point> pts(500);
        char err[256] = { 0 };
        uint32_t n = dai_show_sample(&d, pts.data(), (uint32_t)pts.size(), err, sizeof(err));
        CHECK(n == 0, "[1e] an impossible formation was sampled anyway (%u points)", n);
        char needed[32];
        std::snprintf(needed, sizeof(needed), "%.1f", (double)f.required_size_m);
        CHECK(err[0] != '\0' && std::strstr(err, needed) != nullptr,
              "[1e] the refusal does not name the size the figure needs: \"%s\"", err);

        // ...and the same figure at a size that works is accepted, so the test
        // is measuring the bound rather than a sampler that always says no.
        dai_show_sample_desc big = desc_for(2, DAI_SHOW_SAMPLE_SURFACE, 500, MIN_D, 30.0f, 7u);
        dai_show_feasibility bf;
        CHECK(dai_show_sample_feasible(&big, &bf) == 1 && bf.ok == 1,
              "[1e] 500 drones at 2 m on a 60 m sphere were called impossible");

        // The second way to be too small: geometrically possible, but so close
        // to the packing limit that no placement finds room. That has to be a
        // refusal with a size as well - and the size it names has to work, or
        // it is an apology rather than an answer.
        dai_show_sample_desc tight = desc_for(2, DAI_SHOW_SAMPLE_SURFACE, 1000, MIN_D, 17.5f, 3u);
        dai_show_feasibility tf;
        dai_show_sample_feasible(&tight, &tf);
        CHECK(tf.ok == 1 && tf.max_points >= 1000u,
              "[1e] the tight case was meant to pass the packing bound (%u fit)", tf.max_points);
        std::vector<dai_show_point> many(1000);
        char terr[256] = { 0 };
        uint32_t tn = dai_show_sample(&tight, many.data(), 1000, terr, sizeof(terr));
        CHECK(tn == 0 && terr[0] != '\0',
              "[1e] 90%% of the packing limit was filled after all, or failed silently");
        float grown = 0.0f;
        if (std::sscanf(std::strstr(terr, "about ") ? std::strstr(terr, "about ") + 6 : "", "%f", &grown) != 1)
            grown = 0.0f;
        CHECK(grown > 35.0f, "[1e] the refusal names no usable figure size: \"%s\"", terr);
        if (grown > 35.0f) {
            // The message is in metres across; the sphere fixture has radius 1,
            // so its scale is half of that.
            dai_show_sample_desc fixed = desc_for(2, DAI_SHOW_SAMPLE_SURFACE, 1000, MIN_D,
                                                  grown * 0.5f, 3u);
            uint32_t fn = dai_show_sample(&fixed, many.data(), 1000, terr, sizeof(terr));
            CHECK(fn == 1000, "[1e] the size the refusal asked for still only fits %u of 1000 (%s)",
                  fn, terr);
            CHECK(fn == 1000 && closest_pair(many.data(), fn) >= MIN_D - 1e-3f,
                  "[1b] the repaired figure broke the minimum distance");
        }
    }

    // ---- [1f] the colour arrives -------------------------------------------
    {
        dai_show_test_mesh m = show_test_mesh(0);
        std::vector<uint32_t> vcol(m.vertex_count);
        for (uint32_t i = 0; i < m.vertex_count; ++i) {
            // A different colour per box face, so a barycentric blend across a
            // triangle still lands on that face's hue.
            uint32_t face = i / 6u;
            vcol[i] = 0xFF000000u | ((face * 37u + 30u) & 0xFFu)
                    | (((face * 61u + 80u) & 0xFFu) << 8) | (((face * 91u + 20u) & 0xFFu) << 16);
        }
        std::vector<dai_show_point> pts(120);

        dai_show_sample_desc d = desc_for(0, DAI_SHOW_SAMPLE_SURFACE, 120, MIN_D, 30.0f, 11u);
        d.vertex_rgba = vcol.data();
        char err[256] = { 0 };
        uint32_t n = dai_show_sample(&d, pts.data(), (uint32_t)pts.size(), err, sizeof(err));
        CHECK(n == 120, "[1f] vertex coloured sampling returned %u of 120 (%s)", n, err);
        CHECK(distinct_colours(pts.data(), n) >= 4u,
              "[1f] a six coloured box produced %u distinct point colours", distinct_colours(pts.data(), n));

        // A 2x2 texture, no vertex colours: the point colours have to come out
        // of the texels rather than out of base_rgba.
        const uint8_t tex[16] = { 255, 0, 0, 255,   0, 255, 0, 255,
                                    0, 0, 255, 255, 255, 255, 0, 255 };
        dai_show_sample_desc t = desc_for(0, DAI_SHOW_SAMPLE_SURFACE, 120, MIN_D, 30.0f, 11u);
        t.tex_rgba = tex; t.tex_w = 2; t.tex_h = 2;
        uint32_t nt = dai_show_sample(&t, pts.data(), (uint32_t)pts.size(), err, sizeof(err));
        CHECK(nt == 120, "[1f] textured sampling returned %u of 120 (%s)", nt, err);
        uint32_t base_like = 0;
        for (uint32_t i = 0; i < nt; ++i)
            if (pts[i].r == 0x40 && pts[i].g == 0xC0 && pts[i].b == 0x20) ++base_like;
        CHECK(base_like == 0, "[1f] %u textured points fell back to the base colour", base_like);
        CHECK(distinct_colours(pts.data(), nt) >= 4u, "[1f] the texture produced one flat colour");

        // Neither: every point is the material's base colour, exactly.
        dai_show_sample_desc b = desc_for(0, DAI_SHOW_SAMPLE_SURFACE, 120, MIN_D, 30.0f, 11u);
        uint32_t nb = dai_show_sample(&b, pts.data(), (uint32_t)pts.size(), err, sizeof(err));
        uint32_t exact = 0;
        for (uint32_t i = 0; i < nb; ++i)
            if (pts[i].r == 0x40 && pts[i].g == 0xC0 && pts[i].b == 0x20 && pts[i].w == 0) ++exact;
        CHECK(nb == 120 && exact == nb, "[1f] %u of %u points carry the base colour", exact, nb);
    }

    // ---- [1g] the same seed twice, bit for bit -----------------------------
    {
        std::vector<dai_show_point> a(300), b(300), c(300);
        char err[256] = { 0 };
        dai_show_sample_desc d = desc_for(2, DAI_SHOW_SAMPLE_SURFACE, 300, MIN_D, R, 0xD1CEu);
        uint32_t na = dai_show_sample(&d, a.data(), 300, err, sizeof(err));
        uint32_t nb = dai_show_sample(&d, b.data(), 300, err, sizeof(err));
        d.seed += 1u;
        uint32_t nc = dai_show_sample(&d, c.data(), 300, err, sizeof(err));
        CHECK(na == 300 && nb == 300 && nc == 300, "[1g] a run returned %u/%u/%u points", na, nb, nc);
        CHECK(std::memcmp(a.data(), b.data(), 300 * sizeof(dai_show_point)) == 0,
              "[1g] the same seed produced a different formation");
        CHECK(std::memcmp(a.data(), c.data(), 300 * sizeof(dai_show_point)) != 0,
              "[1g] a different seed produced the identical formation");
    }

    return g_show_fail - before;
}
