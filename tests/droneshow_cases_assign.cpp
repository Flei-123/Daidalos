// Stage 2 under test: the fast answer has to be measured against the true one.
//
// Implements show_cases_assign() - see tests/droneshow_cases.hpp.
//
//   [2a] against BRUTE FORCE for n = 1..8, over several random-but-seeded
//        formations: the exact solver's cost equals the enumerated optimum.
//        This is the only assertion in the file that proves optimality rather
//        than plausibility, which is why it is first.
//   [2b] the result is always a permutation - every target used exactly once.
//   [2c] the auction and clustered solvers stay within a stated gap of the
//        exact one (assert the gap, and print it). The auction is measured at
//        n = 512; the clustered one at n = 1,536, because at 512 it is a single
//        cluster solved exactly and a gap of zero would prove nothing about
//        the seams - which are the only place it can lose anything.
//   [2d] AUTO picks exact below DAI_SHOW_EXACT_MAX and a scaling method above.
//   [2e] the stats' total_cost_m is the real summed distance of the returned
//        permutation - recomputed independently here, because a solver that
//        reports its own cost is a solver marking its own homework.
//   [2f] a translated formation (every point moved by the same vector) assigns
//        identity - the case where the right answer is obvious and a broken
//        cost matrix is not.
#include "droneshow_cases.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

// The test's own random: seeded, splitmix, and never the platform's. A case
// that only fails on Tuesdays is a case nobody fixes.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float unit() { return (float)((next() >> 11) * (1.0 / 9007199254740992.0)); }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

void random_cloud(dai_show_point *out, uint32_t n, float extent, Rng &rng) {
    for (uint32_t i = 0; i < n; ++i) {
        out[i].x = rng.range(-extent, extent);
        out[i].y = rng.range(20.0f, 20.0f + extent);
        out[i].z = rng.range(-extent, extent);
        out[i].r = (uint8_t)(rng.next() & 0xFF);
        out[i].g = (uint8_t)(rng.next() & 0xFF);
        out[i].b = (uint8_t)(rng.next() & 0xFF);
        out[i].w = 0;
    }
}

// The objective the header says is minimised, recomputed here from nothing but
// the returned permutation.
double squared_cost(const dai_show_point *f, const dai_show_point *t,
                    uint32_t n, const uint32_t *perm) {
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        double dx = (double)f[i].x - (double)t[perm[i]].x;
        double dy = (double)f[i].y - (double)t[perm[i]].y;
        double dz = (double)f[i].z - (double)t[perm[i]].z;
        sum += dx * dx + dy * dy + dz * dz;
    }
    return sum;
}

double metre_cost(const dai_show_point *f, const dai_show_point *t,
                  uint32_t n, const uint32_t *perm) {
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        double dx = (double)f[i].x - (double)t[perm[i]].x;
        double dy = (double)f[i].y - (double)t[perm[i]].y;
        double dz = (double)f[i].z - (double)t[perm[i]].z;
        sum += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return sum;
}

int is_permutation(const uint32_t *perm, uint32_t n) {
    std::vector<uint8_t> seen(n, 0);
    for (uint32_t i = 0; i < n; ++i) {
        if (perm[i] >= n) return 0;
        if (seen[perm[i]]) return 0;
        seen[perm[i]] = 1;
    }
    return 1;
}

// ---- [2a] --------------------------------------------------------------

void case_brute_force(void) {
    int worst_mismatch = 0;
    double worst_rel = 0.0;

    for (uint32_t n = 1; n <= 8; ++n) {
        for (uint32_t trial = 0; trial < 6; ++trial) {
            Rng rng(0xA55A0000ull + n * 97ull + trial);
            std::vector<dai_show_point> from(n), to(n);
            random_cloud(from.data(), n, 30.0f, rng);
            random_cloud(to.data(),   n, 30.0f, rng);

            std::vector<uint32_t> got(n), opt(n);
            dai_show_assign_stats st{};
            dai_result r = dai_show_assign(from.data(), to.data(), n,
                                           DAI_SHOW_ASSIGN_EXACT, got.data(), &st);
            double opt_metres = dai_show_assign_brute_force(from.data(), to.data(), n, opt.data());

            if (r != DAI_OK) { ++worst_mismatch; continue; }

            // Optimality is asserted on the objective that was minimised, so a
            // tie between two equally good permutations cannot fail the test
            // for the wrong reason.
            double got_sq = squared_cost(from.data(), to.data(), n, got.data());
            double opt_sq = squared_cost(from.data(), to.data(), n, opt.data());
            if (got_sq > opt_sq + 1e-6 * (1.0 + opt_sq)) ++worst_mismatch;

            double rel = (opt_metres > 0.0)
                       ? std::fabs(st.total_cost_m - opt_metres) / opt_metres : 0.0;
            if (got_sq <= opt_sq + 1e-9 && rel > worst_rel) worst_rel = rel;
        }
    }
    CHECK(worst_mismatch == 0,
          "[2a] the exact solver missed the enumerated optimum on %d of 48 instances",
          worst_mismatch);
    CHECK(worst_rel < 1e-9,
          "[2a] exact cost and brute force cost differ by %.3e relative", worst_rel);

    // And the brute force itself has to be an assignment, or it proves nothing.
    Rng rng(0xBEEF01);
    std::vector<dai_show_point> from(8), to(8);
    random_cloud(from.data(), 8, 12.0f, rng);
    random_cloud(to.data(),   8, 12.0f, rng);
    uint32_t bf[8];
    double c = dai_show_assign_brute_force(from.data(), to.data(), 8, bf);
    CHECK(c > 0.0 && is_permutation(bf, 8), "[2a] brute force did not return a permutation");
    CHECK(dai_show_assign_brute_force(from.data(), to.data(), 9, bf) < 0.0,
          "[2a] brute force accepted n = 9, which it cannot enumerate honestly");
}

// ---- [2b] --------------------------------------------------------------

void case_permutation(void) {
    const int methods[3] = { DAI_SHOW_ASSIGN_EXACT, DAI_SHOW_ASSIGN_AUCTION,
                             DAI_SHOW_ASSIGN_CLUSTER };
    const char *names[3] = { "exact", "auction", "cluster" };
    const uint32_t sizes[3] = { 1, 137, 1024 };

    for (int m = 0; m < 3; ++m) {
        for (int s = 0; s < 3; ++s) {
            const uint32_t n = sizes[s];
            if (m == 1 && n > 512) continue;      // auction is O(n^2) per round
            Rng rng(0x1234500ull + (uint64_t)m * 31 + (uint64_t)s);
            std::vector<dai_show_point> from(n), to(n);
            random_cloud(from.data(), n, 60.0f, rng);
            random_cloud(to.data(),   n, 60.0f, rng);
            std::vector<uint32_t> perm(n, 0xFFFFFFFFu);
            dai_show_assign_stats st{};
            dai_result r = dai_show_assign(from.data(), to.data(), n, methods[m],
                                           perm.data(), &st);
            CHECK(r == DAI_OK, "[2b] %s failed at n = %u", names[m], n);
            CHECK(is_permutation(perm.data(), n),
                  "[2b] %s at n = %u did not return a permutation", names[m], n);
            CHECK(st.method_used == methods[m],
                  "[2b] %s reported method %d", names[m], st.method_used);
        }
    }

    // Degenerate inputs answer with an error rather than with a wrong show.
    dai_show_point p{};
    uint32_t one = 0;
    CHECK(dai_show_assign(nullptr, &p, 1, DAI_SHOW_ASSIGN_AUTO, &one, nullptr)
              == DAI_ERR_INVALID_ARG, "[2b] a null formation was accepted");
    CHECK(dai_show_assign(&p, &p, 0, DAI_SHOW_ASSIGN_AUTO, &one, nullptr)
              == DAI_ERR_INVALID_ARG, "[2b] an empty fleet was accepted");
}

// ---- [2c] --------------------------------------------------------------

void case_gap(void) {
    const uint32_t n = 512;
    Rng rng(0xC0FFEE11);
    std::vector<dai_show_point> from(n), to(n);
    random_cloud(from.data(), n, 80.0f, rng);
    random_cloud(to.data(),   n, 80.0f, rng);

    std::vector<uint32_t> pe(n), pa(n);
    dai_show_assign_stats se{}, sa{}, sc{};
    dai_show_assign(from.data(), to.data(), n, DAI_SHOW_ASSIGN_EXACT,   pe.data(), &se);
    dai_show_assign(from.data(), to.data(), n, DAI_SHOW_ASSIGN_AUCTION, pa.data(), &sa);

    // A size that really splits: 1,536 drones become three clusters, so the
    // number below is the price of the seams and not of nothing.
    const uint32_t nc = 1536;
    std::vector<dai_show_point> cfrom(nc), cto(nc);
    Rng crng(0xC0FFEE22);
    random_cloud(cfrom.data(), nc, 120.0f, crng);
    random_cloud(cto.data(),   nc, 120.0f, crng);
    std::vector<uint32_t> pc(nc);
    dai_show_assign(cfrom.data(), cto.data(), nc, DAI_SHOW_ASSIGN_CLUSTER, pc.data(), &sc);

    std::printf("  n = %u   exact %.2f m (%.1f ms)   auction %.2f m (+%.2f%%, %.1f ms)\n"
                "  n = %u   cluster %.2f m (+%.2f%%, %.1f ms, %u clusters, %u seam swaps)\n",
                n, se.total_cost_m, se.solve_ms,
                sa.total_cost_m, (double)sa.gap_percent, sa.solve_ms,
                nc, sc.total_cost_m, (double)sc.gap_percent, sc.solve_ms,
                sc.clusters, sc.iterations);

    // The auction is an approximation with a bound, so it is held to a tight
    // one; the clustered solver trades more away at the seams and is held to
    // the number a director can budget for.
    CHECK(sa.exact_cost_m > 0.0 && sc.exact_cost_m > 0.0,
          "[2c] no exact reference was computed, so the gap is unmeasured");
    CHECK(sa.gap_percent >= 0.0f && sa.gap_percent < 1.0f,
          "[2c] the auction lost %.3f%% against the optimum", (double)sa.gap_percent);
    CHECK(sc.gap_percent >= 0.0f && sc.gap_percent < 15.0f,
          "[2c] the clustered solver lost %.3f%% against the optimum", (double)sc.gap_percent);
    CHECK(se.gap_percent == 0.0f, "[2c] the exact solver reported a gap of %.3f%%",
          (double)se.gap_percent);
    CHECK(sc.clusters > 1u,
          "[2c] the clustered solver used %u cluster(s) at n = %u, so nothing was "
          "measured about its seams", sc.clusters, nc);
    CHECK(is_permutation(pc.data(), nc), "[2c] the clustered solver broke the permutation");

    // The auction bids on several threads at this size, so it is the one place
    // in stage 2 where a reduction could pick up thread order. Run it twice and
    // compare the bytes - not the numbers, the bytes.
    std::vector<uint32_t> pa2(n);
    dai_show_assign_stats sa2{};
    dai_show_assign(from.data(), to.data(), n, DAI_SHOW_ASSIGN_AUCTION, pa2.data(), &sa2);
    CHECK(std::memcmp(pa.data(), pa2.data(), pa.size() * sizeof(uint32_t)) == 0,
          "[2c] two auction runs on the same input disagree");
    CHECK(std::memcmp(&sa.total_cost_m, &sa2.total_cost_m, sizeof(double)) == 0,
          "[2c] the auction's cost sum is not bit identical (%.9f vs %.9f)",
          sa.total_cost_m, sa2.total_cost_m);
}

// ---- [2d] --------------------------------------------------------------

void case_auto(void) {
    {
        const uint32_t n = 64;
        Rng rng(0xAA01);
        std::vector<dai_show_point> from(n), to(n);
        random_cloud(from.data(), n, 20.0f, rng);
        random_cloud(to.data(),   n, 20.0f, rng);
        std::vector<uint32_t> perm(n);
        dai_show_assign_stats st{};
        dai_show_assign(from.data(), to.data(), n, DAI_SHOW_ASSIGN_AUTO, perm.data(), &st);
        CHECK(st.method_used == DAI_SHOW_ASSIGN_EXACT,
              "[2d] AUTO chose method %d at n = %u, below DAI_SHOW_EXACT_MAX",
              st.method_used, n);
        CHECK(st.clusters == 1u, "[2d] the exact path reported %u clusters", st.clusters);
    }
    {
        const uint32_t n = DAI_SHOW_EXACT_MAX + 1u;
        Rng rng(0xAA02);
        std::vector<dai_show_point> from(n), to(n);
        random_cloud(from.data(), n, 120.0f, rng);
        random_cloud(to.data(),   n, 120.0f, rng);
        std::vector<uint32_t> perm(n);
        dai_show_assign_stats st{};
        dai_show_assign(from.data(), to.data(), n, DAI_SHOW_ASSIGN_AUTO, perm.data(), &st);
        CHECK(st.method_used == DAI_SHOW_ASSIGN_CLUSTER,
              "[2d] AUTO chose method %d at n = %u, above DAI_SHOW_EXACT_MAX",
              st.method_used, n);
        CHECK(st.method_used != DAI_SHOW_ASSIGN_AUTO, "[2d] AUTO was reported back as AUTO");
        CHECK(st.clusters > 1u,
              "[2d] the clustered path at n = %u used %u cluster(s)", n, st.clusters);
        CHECK(st.exact_cost_m < 0.0 && st.gap_percent < 0.0f,
              "[2d] an exact reference was computed at n = %u, which is the O(n^3) "
              "path this method exists to avoid", n);
        CHECK(is_permutation(perm.data(), n), "[2d] AUTO at n = %u broke the permutation", n);
    }
}

// ---- [2e] --------------------------------------------------------------

void case_reported_cost(void) {
    const int methods[3] = { DAI_SHOW_ASSIGN_EXACT, DAI_SHOW_ASSIGN_AUCTION,
                             DAI_SHOW_ASSIGN_CLUSTER };
    const char *names[3] = { "exact", "auction", "cluster" };
    for (int m = 0; m < 3; ++m) {
        const uint32_t n = 300;
        Rng rng(0xE5E5 + (uint64_t)m);
        std::vector<dai_show_point> from(n), to(n);
        random_cloud(from.data(), n, 45.0f, rng);
        random_cloud(to.data(),   n, 45.0f, rng);
        std::vector<uint32_t> perm(n);
        dai_show_assign_stats st{};
        dai_show_assign(from.data(), to.data(), n, methods[m], perm.data(), &st);

        const double mine = metre_cost(from.data(), to.data(), n, perm.data());
        CHECK(std::fabs(mine - st.total_cost_m) <= 1e-6 * (1.0 + mine),
              "[2e] %s reported %.6f m for a permutation that flies %.6f m",
              names[m], st.total_cost_m, mine);
        CHECK(st.solve_ms >= 0.0 && st.solve_ms < 60000.0,
              "[2e] %s reported a solve time of %.3f ms", names[m], st.solve_ms);
    }
}

// ---- [2f] --------------------------------------------------------------

void case_translation(void) {
    struct { int method; uint32_t n; const char *name; } cases[3] = {
        { DAI_SHOW_ASSIGN_EXACT,   200,  "exact"   },
        { DAI_SHOW_ASSIGN_AUCTION, 128,  "auction" },
        { DAI_SHOW_ASSIGN_CLUSTER, 1024, "cluster" }
    };
    for (int c = 0; c < 3; ++c) {
        const uint32_t n = cases[c].n;
        Rng rng(0x7A7A00ull + (uint64_t)c);
        std::vector<dai_show_point> from(n), to(n);
        random_cloud(from.data(), n, 50.0f, rng);
        for (uint32_t i = 0; i < n; ++i) {
            to[i] = from[i];
            to[i].x += 17.0f;
            to[i].y += 4.0f;
            to[i].z += -9.0f;
        }
        std::vector<uint32_t> perm(n);
        dai_show_assign_stats st{};
        dai_show_assign(from.data(), to.data(), n, cases[c].method, perm.data(), &st);

        uint32_t wrong = 0;
        for (uint32_t i = 0; i < n; ++i) if (perm[i] != i) ++wrong;
        CHECK(wrong == 0, "[2f] %s moved %u of %u drones off the obvious identity",
              cases[c].name, wrong, n);

        const double ideal = (double)n * std::sqrt(17.0 * 17.0 + 4.0 * 4.0 + 9.0 * 9.0);
        CHECK(std::fabs(st.total_cost_m - ideal) < 1e-2,
              "[2f] %s reported %.4f m where the translation costs %.4f m",
              cases[c].name, st.total_cost_m, ideal);
    }
}

} // namespace

int show_cases_assign(void) {
    const int before = g_show_fail;
    show_section("stage 2 - assignment: exact, auction, clustered");
    case_brute_force();
    case_permutation();
    case_gap();
    case_auto();
    case_reported_cost();
    case_translation();
    return g_show_fail - before;
}
