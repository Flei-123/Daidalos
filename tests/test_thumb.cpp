// Asset thumbnails: a picture of a mesh, checked by reading the pixels.
//
//   ./build/test_thumb
//
// No renderer, no window, no GPU - which is the whole point of doing this on
// the CPU. The checks below are the properties that make a thumbnail useful as
// an ICON rather than as a screenshot:
//
//   - it does not care how big the model is, or where it is. A 1 m crate and a
//     100 m crate at the far end of the level must give the same picture, or a
//     folder of models is a folder of random sizes.
//   - up is up. Getting the sign of the screen y wrong is invisible on a cube
//     and instantly obvious on a character, so a tall box is checked to come
//     out tall.
//   - it is shaded, not a silhouette. A flat blob tells you as little as the
//     amber square it replaced.
//   - an empty or broken mesh produces an empty frame and no crash.

#include "dai_thumb.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static const uint32_t N = 64;

// A box centred at `c`, half extents `h`, as a plain triangle list.
static std::vector<float> box(float cx, float cy, float cz, float hx, float hy, float hz) {
    const float p[8][3] = {
        { cx - hx, cy - hy, cz - hz }, { cx + hx, cy - hy, cz - hz },
        { cx + hx, cy + hy, cz - hz }, { cx - hx, cy + hy, cz - hz },
        { cx - hx, cy - hy, cz + hz }, { cx + hx, cy - hy, cz + hz },
        { cx + hx, cy + hy, cz + hz }, { cx - hx, cy + hy, cz + hz },
    };
    static const int F[6][4] = {
        { 4, 5, 6, 7 }, { 1, 0, 3, 2 }, { 0, 4, 7, 3 },
        { 5, 1, 2, 6 }, { 3, 7, 6, 2 }, { 0, 1, 5, 4 },
    };
    std::vector<float> v;
    auto push = [&](int i) { v.push_back(p[i][0]); v.push_back(p[i][1]); v.push_back(p[i][2]); };
    for (const auto &f : F) {
        push(f[0]); push(f[1]); push(f[2]);
        push(f[0]); push(f[2]); push(f[3]);
    }
    return v;
}

static int render(const std::vector<float> &v, uint8_t *out, uint32_t tint = 0) {
    dai_thumb_mesh m{ v.data(), (uint32_t)(v.size() / 3), nullptr, 0 };
    return dai_thumb_render(&m, out, N, tint);
}

struct Cover {
    int pixels = 0;
    int rows = 0, cols = 0;          // how many lines have anything on them
    int width = 0, height = 0;       // the bounding box of what was drawn
    int lo = 255, hi = 0;
};

static Cover measure(const uint8_t *px) {
    Cover c;
    std::vector<int> row(N, 0), col(N, 0);
    int x0 = (int)N, x1 = -1, y0 = (int)N, y1 = -1;
    for (uint32_t y = 0; y < N; ++y) {
        for (uint32_t x = 0; x < N; ++x) {
            const uint8_t *p = px + ((size_t)y * N + x) * 4;
            if (p[3] < 200) continue;              // only solidly covered pixels
            ++c.pixels; row[y] = 1; col[x] = 1;
            if ((int)x < x0) x0 = (int)x;
            if ((int)x > x1) x1 = (int)x;
            if ((int)y < y0) y0 = (int)y;
            if ((int)y > y1) y1 = (int)y;
            int lum = (p[0] + p[1] + p[2]) / 3;
            if (lum < c.lo) c.lo = lum;
            if (lum > c.hi) c.hi = lum;
        }
    }
    for (uint32_t i = 0; i < N; ++i) { c.rows += row[i]; c.cols += col[i]; }
    c.width  = x1 >= x0 ? x1 - x0 + 1 : 0;
    c.height = y1 >= y0 ? y1 - y0 + 1 : 0;
    return c;
}

int main() {
    std::vector<uint8_t> a((size_t)N * N * 4), b((size_t)N * N * 4);

    // ---- 1. a cube looks like a cube --------------------------------------
    std::printf("a box\n");
    {
        CHECK(render(box(0, 0, 0, 0.5f, 0.5f, 0.5f), a.data()) == 1,
              "rendering a box drew nothing");
        Cover c = measure(a.data());
        CHECK(c.pixels > (int)(N * N) / 5,
              "the box covers only %d of %d pixels - it did not fill the frame",
              c.pixels, N * N);
        CHECK(c.pixels < (int)(N * N) * 9 / 10,
              "the box covers %d of %d pixels - there is no margin left",
              c.pixels, N * N);
        // The corners of the frame are background: a three quarter view of a
        // cube is a hexagon, and a full square would mean the fit is wrong.
        CHECK(a[3] == 0, "the top left corner is not transparent (alpha %d)", a[3]);
        CHECK(a[((size_t)(N - 1) * N + (N - 1)) * 4 + 3] == 0,
              "the bottom right corner is not transparent");
        // The middle is solid.
        CHECK(a[((size_t)(N / 2) * N + N / 2) * 4 + 3] > 200,
              "the middle of the frame is empty - nothing was drawn there");
        // Three faces of a cube, three different angles to the light.
        CHECK(c.hi - c.lo > 25,
              "the brightest and darkest pixel differ by %d - that is a silhouette, "
              "not a shaded model", c.hi - c.lo);
    }

    // ---- 2. size does not matter ------------------------------------------
    std::printf("scale and position do not change the picture\n");
    {
        render(box(0, 0, 0, 0.5f, 0.5f, 0.5f), a.data());
        render(box(0, 0, 0, 50.0f, 50.0f, 50.0f), b.data());
        CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0,
              "a 100 m cube renders differently from a 1 m one - the fit is not "
              "scale invariant");

        render(box(120.0f, -40.0f, 7.5f, 0.5f, 0.5f, 0.5f), b.data());
        CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0,
              "a cube far from the origin renders differently - the fit centres on "
              "the wrong thing");
    }

    // ---- 3. up is up ------------------------------------------------------
    std::printf("orientation\n");
    {
        render(box(0, 0, 0, 0.08f, 0.5f, 0.08f), a.data());     // a tall post
        Cover tall = measure(a.data());
        render(box(0, 0, 0, 0.5f, 0.08f, 0.5f), b.data());      // a flat slab
        Cover flat = measure(b.data());
        CHECK(tall.rows > tall.cols * 2,
              "a tall post came out %d rows by %d columns - it is not standing up",
              tall.rows, tall.cols);
        CHECK(flat.cols > flat.rows,
              "a flat slab came out %d rows by %d columns", flat.rows, flat.cols);
    }

    // ---- 4. the tint reaches the pixels -----------------------------------
    std::printf("tint\n");
    {
        render(box(0, 0, 0, 0.5f, 0.5f, 0.5f), a.data(), 0xFF3020);
        int reds = 0, wrong = 0;
        for (uint32_t i = 0; i < N * N; ++i) {
            const uint8_t *p = a.data() + (size_t)i * 4;
            if (p[3] < 200) continue;
            if (p[0] > p[2]) ++reds; else ++wrong;
        }
        CHECK(reds > 0 && wrong == 0,
              "%d pixels came out red and %d did not - the tint is not applied "
              "everywhere", reds, wrong);
    }

    // ---- 5. nothing to draw draws nothing ---------------------------------
    std::printf("empty and broken input\n");
    {
        std::memset(a.data(), 0xAB, a.size());
        CHECK(dai_thumb_render(nullptr, a.data(), N, 0) == 0, "a null mesh claimed success");
        CHECK(a[0] == 0 && a[3] == 0, "a refused render left the buffer dirty");

        // Every vertex in one place: no extent to fit, nothing to show.
        std::vector<float> point(9, 0.0f);
        std::memset(a.data(), 0xAB, a.size());
        CHECK(render(point, a.data()) == 0, "a degenerate mesh claimed to draw something");
        CHECK(measure(a.data()).pixels == 0, "a degenerate mesh drew pixels");

        // Two vertices is not a triangle.
        std::vector<float> two(6, 1.0f);
        CHECK(render(two, a.data()) == 0, "six floats claimed to be a triangle");

        // A size of zero must not write anywhere.
        CHECK(dai_thumb_render(nullptr, a.data(), 0, 0) == 0, "size 0 was accepted");
    }

    // ---- 6. an index buffer says the same thing ---------------------------
    std::printf("indexed and non indexed agree\n");
    {
        std::vector<float> v = box(0, 0, 0, 0.5f, 0.5f, 0.5f);
        render(v, a.data());
        std::vector<uint32_t> idx(v.size() / 3);
        for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
        dai_thumb_mesh m{ v.data(), (uint32_t)(v.size() / 3), idx.data(), (uint32_t)idx.size() };
        CHECK(dai_thumb_render(&m, b.data(), N, 0) == 1, "the indexed mesh drew nothing");
        CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0,
              "the same triangles through an index buffer came out different");

        // An index past the end must be clamped, not read out of bounds.
        std::vector<uint32_t> bad = idx;
        bad[0] = 999999;
        dai_thumb_mesh m2{ v.data(), (uint32_t)(v.size() / 3), bad.data(), (uint32_t)bad.size() };
        dai_thumb_render(&m2, b.data(), N, 0);     // must simply not crash
        ++g_pass;
    }

    // ---- 7. the built in shapes ------------------------------------------
    std::printf("primitive shapes\n");
    {
        const int SHAPES[4] = { DAI_SHAPE_BOX, DAI_SHAPE_SPHERE,
                                DAI_SHAPE_CAPSULE, DAI_SHAPE_CYLINDER };
        for (int shape : SHAPES) {
            CHECK(dai_thumb_render_shape(shape, a.data(), N, 0) == 1,
                  "shape %d drew nothing", shape);
            Cover c = measure(a.data());
            CHECK(c.pixels > (int)(N * N) / 8, "shape %d covers only %d pixels",
                  shape, c.pixels);
            CHECK(c.hi - c.lo > 15, "shape %d came out flat (%d..%d)", shape, c.lo, c.hi);
        }
        // A sphere is round: it must not fill its corners the way a box does.
        dai_thumb_render_shape(DAI_SHAPE_SPHERE, a.data(), N, 0);
        CHECK(a[3] == 0 && a[((size_t)(N - 1) * N + (N - 1)) * 4 + 3] == 0,
              "the sphere reaches the corners of the frame");
    }

    // ---- 8. a prefab is several shapes, fitted together -------------------
    // The lid has to sit ON the crate, not fill the icon on its own: the fit
    // is over the WHOLE set, once, and every part goes through the same
    // transform it was given.
    std::printf("prefab parts\n");
    {
        auto ident = [](dai_thumb_part &p2, float sx, float sy, float sz,
                        float tx, float ty, float tz) {
            float m[12] = { sx, 0, 0, tx,  0, sy, 0, ty,  0, 0, sz, tz };
            std::memcpy(p2.xform, m, sizeof(m));
        };
        dai_thumb_part two[2]{};
        two[0].shape = DAI_SHAPE_BOX; ident(two[0], 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
        two[1].shape = DAI_SHAPE_BOX; ident(two[1], 1.0f, 0.2f, 1.0f, 0.0f, 0.6f, 0.0f);
        CHECK(dai_thumb_render_parts(two, 2, a.data(), N, 0) == 1,
              "a two part prefab drew nothing");
        Cover both = measure(a.data());

        // The same crate ALONE must be a different picture - if the second
        // part were ignored, or if each part were fitted on its own, these
        // would come out identical.
        CHECK(dai_thumb_render_parts(two, 1, b.data(), N, 0) == 1,
              "the crate alone drew nothing");
        CHECK(std::memcmp(a.data(), b.data(), a.size()) != 0,
              "adding a lid changed nothing - the second part was dropped");
        Cover alone = measure(b.data());
        // Both are fitted to the frame, so neither is "bigger" - but a crate
        // with a lid on it is TALLER for its width than a bare crate, and that
        // survives the fit.
        CHECK(both.height * alone.width > alone.height * both.width,
              "crate+lid is %dx%d, the crate alone %dx%d - putting a lid on it did "
              "not make it taller", both.width, both.height, alone.width, alone.height);

        // A part far off to the side pulls the fit with it.
        dai_thumb_part far2[2]{};
        far2[0].shape = DAI_SHAPE_BOX; ident(far2[0], 1, 1, 1, 0, 0, 0);
        far2[1].shape = DAI_SHAPE_SPHERE; ident(far2[1], 1, 1, 1, 6.0f, 0, 0);
        CHECK(dai_thumb_render_parts(far2, 2, a.data(), N, 0) == 1,
              "two separated parts drew nothing");
        Cover spread = measure(a.data());
        CHECK(spread.width > spread.height,
              "two parts six units apart along x came out %d wide by %d tall - the "
              "set was not fitted as one", spread.width, spread.height);
        // ...and there is a GAP between them: two objects, not one blob.
        int empty_cols = spread.width - spread.cols;
        CHECK(empty_cols > 4,
              "no gap between two parts six units apart (%d of %d columns empty)",
              empty_cols, spread.width);

        // No parts is an empty frame, not a crash.
        std::memset(a.data(), 0xAB, a.size());
        CHECK(dai_thumb_render_parts(nullptr, 0, a.data(), N, 0) == 0,
              "no parts claimed to draw something");
        CHECK(a[0] == 0 && a[3] == 0, "no parts left the buffer dirty");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
