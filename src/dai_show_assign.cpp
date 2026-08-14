// Stage 2: which drone flies to which point, without O(n^3) at 10,000 drones.
//
// Implements, from include/dai_show.h:
//     dai_show_assign
//     dai_show_assign_brute_force
//
// Three solvers behind one call, because the honest answer changes with n:
//
//   Jonker-Volgenant  exact, and fast in practice for the sizes a director
//                     actually reaches for a single figure. Still O(n^3) in
//                     the worst case, which is why it is not the only one.
//   auction           epsilon scaled, near optimal, and parallel over bidders
//                     - with the bids reduced in a fixed index order, because
//                     an assignment that depends on which thread bid first is
//                     not an assignment this engine is allowed to produce.
//   cluster           space is split, each cluster solved exactly, the seams
//                     repaired. The path 10,000 drones take.
//
// The stats report which one ran and, where the exact answer is computable,
// how much path length the fast one cost - a number the director sees rather
// than a promise the tool makes.
//
// THE COST MATRIX IS NEVER BUILT. 10,000 squared floats is 400 MB, and the
// bandwidth to read it once is worse than the arithmetic to recompute it: a
// cost here is three subtractions and three multiplies off two 16 byte points,
// which is cheaper than the cache miss it would replace. Every solver below
// takes the two formations and recomputes, and none of them allocates anything
// that grows faster than n.
//
// WHAT `iterations` MEANS, per method, since one word has to cover three
// algorithms: JV counts shortest path augmentations (the rows the greedy
// pre-passes could not place), auction counts bidding rounds, cluster counts
// the seam swaps that actually improved the answer.

#include "dai_show.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// ---- the cost ------------------------------------------------------------

// Squared metres. Squared because it orders the same way as distance for the
// nearest-point question, needs no square root in the inner loop, and stays
// exact for longer in double. The stats convert to real metres once, at the
// end, in index order.
inline double cost_of(const dai_show_point &a, const dai_show_point &b) {
    double dx = (double)a.x - (double)b.x;
    double dy = (double)a.y - (double)b.y;
    double dz = (double)a.z - (double)b.z;
    return dx * dx + dy * dy + dz * dz;
}

// Sum of real metres over the permutation, Kahan compensated and walked in
// drone index order. Both properties are the point: the same input has to
// produce the same double, bit for bit, whatever the solver did internally and
// however many threads it used getting there.
double total_metres(const dai_show_point *from, const dai_show_point *to,
                    uint32_t n, const uint32_t *perm) {
    double sum = 0.0, comp = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        double d = std::sqrt(cost_of(from[i], to[perm[i]]));
        double y = d - comp;
        double t = sum + y;
        comp = (t - sum) - y;
        sum = t;
    }
    return sum;
}

// The objective the solvers actually minimise, same fixed order. Only used to
// compare two permutations against each other (the seam repair and the tests).
double total_squared(const dai_show_point *from, const dai_show_point *to,
                     uint32_t n, const uint32_t *perm) {
    double sum = 0.0, comp = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        double c = cost_of(from[i], to[perm[i]]);
        double y = c - comp;
        double t = sum + y;
        comp = (t - sum) - y;
        sum = t;
    }
    return sum;
}

unsigned worker_count(uint32_t jobs) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    if (hw > 16) hw = 16;                       // more threads, same answer
    if (hw > jobs) hw = (unsigned)jobs;
    if (hw == 0) hw = 1;
    return hw;
}

// ---- 1: Jonker-Volgenant, exact ------------------------------------------
//
// Shortest augmenting paths on the reduced cost, with the three cheap passes
// that make the difference between "O(n^3) in theory" and "milliseconds at
// 1,000" in practice:
//
//   column reduction        every target offers itself to its cheapest drone,
//                           which places most of the assignment for free,
//   reduction transfer      the dual of an already placed row is pushed as far
//                           as it will go, so the paths that follow are short,
//   augmenting row reduction two greedy sweeps over what is left, each of which
//                           may bump one row out and re-place it,
//   augmentation            Dijkstra over columns for the handful of rows the
//                           sweeps could not place. This is the only part that
//                           can go cubic, and by then it rarely runs at all.
//
// Duals live in v[]; the row duals are implicit in the reduced costs, which is
// why nothing here needs an u[] array. Ties inside the scans resolve to the
// lower column index, so the permutation - not just its cost - is reproducible.
void jv_solve(const dai_show_point *f, const dai_show_point *t, uint32_t n_u,
              uint32_t *out_perm, uint32_t *augmentations) {
    const int n = (int)n_u;
    if (augmentations) *augmentations = 0;
    if (n <= 0) return;
    if (n == 1) { out_perm[0] = 0; return; }

    std::vector<double> v((size_t)n, 0.0), d((size_t)n, 0.0);
    std::vector<int>    rowsol((size_t)n, -1), colsol((size_t)n, -1);
    std::vector<int>    matches((size_t)n, 0), freerow((size_t)n, 0);
    std::vector<int>    collist((size_t)n, 0), pred((size_t)n, 0);
    int nfree = 0;

    // The points are unpacked into three plain arrays first. Same arithmetic to
    // the last bit - a float widens to double exactly - but the inner loops
    // below walk them with a stride of 8 bytes instead of picking coordinates
    // out of a 16 byte record, and that alone is a third off the solve time.
    // This is also why no cost matrix is needed: recomputing a cost costs less
    // than the cache line that reading one would have cost.
    std::vector<double> fx((size_t)n), fy((size_t)n), fz((size_t)n);
    std::vector<double> tx((size_t)n), ty((size_t)n), tz((size_t)n);
    for (int i = 0; i < n; ++i) {
        fx[i] = f[i].x; fy[i] = f[i].y; fz[i] = f[i].z;
        tx[i] = t[i].x; ty[i] = t[i].y; tz[i] = t[i].z;
    }
    auto cost = [&](int i, int j) {
        const double dx = fx[i] - tx[j], dy = fy[i] - ty[j], dz = fz[i] - tz[j];
        return dx * dx + dy * dy + dz * dz;
    };

    // A tolerance rather than an exact compare, because two legs of identical
    // length are the normal case in a grid formation and a strict < there
    // makes the greedy passes chase a difference of one ulp.
    const double tol = 1e-12;

    // column reduction
    for (int j = n - 1; j >= 0; --j) {
        double mn = cost(0, j);
        int    imin = 0;
        for (int i = 1; i < n; ++i) {
            double h = cost(i, j);
            if (h < mn) { mn = h; imin = i; }
        }
        v[j] = mn;
        if (++matches[imin] == 1) { rowsol[imin] = j; colsol[j] = imin; }
    }

    // reduction transfer, and the free rows fall out of it
    for (int i = 0; i < n; ++i) {
        if (matches[i] == 0) { freerow[nfree++] = i; continue; }
        if (matches[i] > 1) continue;
        const int j1 = rowsol[i];
        double mn = 1e308;
        for (int j = 0; j < n; ++j) {
            if (j == j1) continue;
            double h = cost(i, j) - v[j];
            if (h < mn) mn = h;
        }
        v[j1] -= mn;
    }

    // Augmenting row reduction, twice, and each pass on a budget.
    //
    // A row that is bumped out of its column is retried immediately, which is
    // what makes this pass place nearly everything - but the retry chain is a
    // game of musical chairs that can run for a thousand sweeps per row before
    // it settles. Measured at n = 2,000 on random formations: unbounded it
    // needs 1.4 million sweeps to leave six rows for the augmentation phase
    // and takes 15.5 seconds; a budget of 4n needs 8,000 sweeps, leaves 167
    // rows, and takes 0.35 seconds, because 167 shortest paths are far cheaper
    // than 1.4 million sweeps. Nothing about optimality rides on where the
    // budget sits - this pass only warm starts the duals, and the augmentation
    // below is what proves the answer.
    const long chain_budget = 4L * (long)n;
    for (int pass = 0; pass < 2 && nfree > 0; ++pass) {
        const int prev = nfree;
        int  k = 0;
        long steps = 0;
        nfree = 0;
        while (k < prev) {
            const int i = freerow[k++];
            ++steps;
            double umin = cost(i, 0) - v[0], usub = 1e308;
            int    j1 = 0, j2 = -1;
            for (int j = 1; j < n; ++j) {
                double h = cost(i, j) - v[j];
                if (h < usub) {
                    if (h >= umin) { usub = h; j2 = j; }
                    else { usub = umin; j2 = j1; umin = h; j1 = j; }
                }
            }
            int i0 = colsol[j1];
            const bool strict = (usub - umin) > tol;
            if (strict)          v[j1] -= (usub - umin);
            else if (i0 >= 0)  { j1 = j2; i0 = colsol[j2]; }
            rowsol[i] = j1;
            colsol[j1] = i;
            if (i0 >= 0) {
                rowsol[i0] = -1;
                if (strict && steps < chain_budget) freerow[--k] = i0;  // retry now
                else                                freerow[nfree++] = i0;
            }
        }
    }

    // augmentation: one shortest path per row still without a column
    for (int fi = 0; fi < nfree; ++fi) {
        const int start = freerow[fi];
        for (int j = 0; j < n; ++j) {
            d[j]       = cost(start, j) - v[j];
            pred[j]    = start;
            collist[j] = j;
        }
        int    low = 0, up = 0, last = 0, endpath = -1;
        double mn = 0.0;
        bool   found = false;

        while (!found) {
            if (up == low) {                     // the scanned set ran dry
                last = low;
                mn = d[collist[up++]];
                for (int k = up; k < n; ++k) {
                    const int j = collist[k];
                    const double h = d[j];
                    if (h <= mn) {
                        if (h < mn) { up = low; mn = h; }
                        collist[k] = collist[up];
                        collist[up++] = j;
                    }
                }
                for (int k = low; k < up; ++k) {
                    if (colsol[collist[k]] < 0) { endpath = collist[k]; found = true; break; }
                }
            }
            if (found) break;

            const int j1 = collist[low++];
            const int i  = colsol[j1];
            const double h = cost(i, j1) - v[j1] - mn;
            for (int k = up; k < n; ++k) {
                const int j = collist[k];
                const double v2 = cost(i, j) - v[j] - h;
                if (v2 < d[j]) {
                    pred[j] = i;
                    if (v2 <= mn) {              // already on the shortest front
                        if (colsol[j] < 0) { d[j] = v2; endpath = j; found = true; break; }
                        collist[k] = collist[up];
                        collist[up++] = j;
                    }
                    d[j] = v2;
                }
            }
        }

        for (int k = 0; k < last; ++k) {
            const int j1 = collist[k];
            v[j1] += d[j1] - mn;
        }
        int i;
        int j = endpath;
        do {
            i = pred[j];
            colsol[j] = i;
            const int jn = rowsol[i];
            rowsol[i] = j;
            j = jn;
        } while (i != start);
        if (augmentations) ++(*augmentations);
    }

    for (int i = 0; i < n; ++i) out_perm[i] = (uint32_t)rowsol[i];
}

// ---- 2: auction with epsilon scaling -------------------------------------
//
// Every unassigned drone bids for the target that leaves it best off, the
// target goes to the highest bid, and the price it went for makes it less
// attractive to everyone else. With epsilon shrinking geometrically the answer
// is optimal to within n*epsilon, and epsilon ends small enough that the last
// digit of a metre is not in question.
//
// The bids are computed in parallel - that is the O(u*n) part - and reduced
// SERIALLY, walking the unassigned drones in ascending index and keeping a bid
// only on a strict improvement. A tie therefore always goes to the lower index,
// on one core and on sixteen. That single rule is what makes a threaded solver
// admissible in an engine whose whole promise is reproducibility.
struct AuctionBid { double price; int obj; };

void auction_solve(const dai_show_point *f, const dai_show_point *t, uint32_t n_u,
                   uint32_t *out_perm, uint32_t *rounds_out) {
    const int n = (int)n_u;
    if (rounds_out) *rounds_out = 0;
    if (n <= 0) return;
    if (n == 1) { out_perm[0] = 0; return; }

    // The price scale: the largest cost that can occur is the squared diagonal
    // of the box holding both formations. Derived from the data, so a show in
    // centimetres and a show in kilometres get the same number of phases.
    double lo[3] = { 1e308, 1e308, 1e308 }, hi[3] = { -1e308, -1e308, -1e308 };
    for (int i = 0; i < n; ++i) {
        const float *p[2] = { &f[i].x, &t[i].x };
        for (int s = 0; s < 2; ++s)
            for (int a = 0; a < 3; ++a) {
                if (p[s][a] < lo[a]) lo[a] = p[s][a];
                if (p[s][a] > hi[a]) hi[a] = p[s][a];
            }
    }
    double span = 0.0;
    for (int a = 0; a < 3; ++a) { double e = hi[a] - lo[a]; span += e * e; }
    if (!(span > 0.0)) {                        // every point in one place
        for (int i = 0; i < n; ++i) out_perm[i] = (uint32_t)i;
        return;
    }

    std::vector<double> price((size_t)n, 0.0), best_bid((size_t)n, 0.0);
    std::vector<int>    rowsol((size_t)n, -1), colsol((size_t)n, -1), best_bidder((size_t)n, -1);
    std::vector<int>    unassigned;
    std::vector<AuctionBid> bid((size_t)n);
    unassigned.reserve((size_t)n);

    const double eps_final = span / (4.0 * (double)n * (double)n);
    const int    round_cap = 64 * n + 4096;
    uint32_t     rounds = 0;

    for (double eps = span * 0.25; ; eps *= 0.25) {
        if (eps < eps_final) eps = eps_final;
        std::fill(rowsol.begin(), rowsol.end(), -1);
        std::fill(colsol.begin(), colsol.end(), -1);

        int phase_rounds = 0;
        for (;;) {
            unassigned.clear();
            for (int i = 0; i < n; ++i) if (rowsol[i] < 0) unassigned.push_back(i);
            if (unassigned.empty()) break;
            if (++phase_rounds > round_cap) break;
            ++rounds;

            const uint32_t u = (uint32_t)unassigned.size();
            auto bid_range = [&](uint32_t begin, uint32_t end) {
                for (uint32_t k = begin; k < end; ++k) {
                    const int i = unassigned[k];
                    double b1 = -1e308, b2 = -1e308;
                    int    j1 = 0;
                    for (int j = 0; j < n; ++j) {
                        const double val = -(cost_of(f[i], t[j]) + price[j]);
                        if (val > b1) { b2 = b1; b1 = val; j1 = j; }
                        else if (val > b2) { b2 = val; }
                    }
                    bid[(size_t)i].obj   = j1;
                    bid[(size_t)i].price = price[j1] + (b1 - b2) + eps;
                }
            };

            // Threads only where they pay: below this the spawn costs more
            // than the scan. The answer is identical either way, which is the
            // reason the switch is allowed to exist at all.
            if ((double)u * (double)n > 200000.0) {
                const unsigned w = worker_count(u);
                std::vector<std::thread> pool;
                pool.reserve(w);
                for (unsigned wi = 0; wi < w; ++wi) {
                    const uint32_t b = (uint32_t)((uint64_t)u * wi / w);
                    const uint32_t e = (uint32_t)((uint64_t)u * (wi + 1) / w);
                    if (b < e) pool.emplace_back(bid_range, b, e);
                }
                for (auto &th : pool) th.join();
            } else {
                bid_range(0, u);
            }

            // fixed order reduction: ascending bidder, strict improvement only
            for (uint32_t k = 0; k < u; ++k) {
                const int i = unassigned[k];
                const int j = bid[(size_t)i].obj;
                if (best_bidder[j] < 0 || bid[(size_t)i].price > best_bid[j]) {
                    best_bid[j]    = bid[(size_t)i].price;
                    best_bidder[j] = i;
                }
            }
            for (int j = 0; j < n; ++j) {
                const int win = best_bidder[j];
                if (win < 0) continue;
                best_bidder[j] = -1;
                if (colsol[j] >= 0) rowsol[colsol[j]] = -1;
                colsol[j]     = win;
                rowsol[win]   = j;
                price[j]      = best_bid[j];
            }
        }
        if (eps <= eps_final) break;
    }

    // The cap above is a safety net, not a plan: it is out of reach for any
    // finite instance, since every round raises one price by at least epsilon.
    // If it ever were hit the remainder is finished EXACTLY on the leftover
    // sub-problem rather than papered over with a greedy match.
    std::vector<int> rows_left, cols_left;
    for (int i = 0; i < n; ++i) if (rowsol[i] < 0) rows_left.push_back(i);
    for (int j = 0; j < n; ++j) if (colsol[j] < 0) cols_left.push_back(j);
    if (!rows_left.empty()) {
        const uint32_t m = (uint32_t)rows_left.size();
        std::vector<dai_show_point> sf(m), st(m);
        std::vector<uint32_t>       sp(m);
        for (uint32_t k = 0; k < m; ++k) { sf[k] = f[rows_left[k]]; st[k] = t[cols_left[k]]; }
        jv_solve(sf.data(), st.data(), m, sp.data(), nullptr);
        for (uint32_t k = 0; k < m; ++k) rowsol[rows_left[k]] = cols_left[sp[k]];
    }

    for (int i = 0; i < n; ++i) out_perm[i] = (uint32_t)rowsol[i];
    if (rounds_out) *rounds_out = rounds;
}

// ---- 3: spatial clustering, exact inside, seams repaired ------------------
//
// The 10,000 drone path. Space is split recursively on the longest axis of the
// box holding both formations, and both formations are split at their own
// median along that axis - so each half gets exactly as many drones as targets
// and the recursion never produces a cluster that cannot be a permutation.
//
// The split rule is fixed: longest axis, median by (coordinate, index). No
// random centres, no k-means, no thread order anywhere in the decision. Two
// runs produce the same tree, and therefore the same clusters, on any machine.
//
// What it costs is the seams: a drone near a cluster boundary may have a better
// target one cluster over, and the exact solve inside its own cluster cannot
// see it. That is repaired afterwards by 2-opt over the boundary, found through
// a uniform grid rather than by testing every pair - the same broadphase idea
// the validator uses, and for the same reason.

const uint32_t CLUSTER_TARGET = 512;            // exact JV is cheap at this size

struct Leaf { uint32_t begin, end; };

// nth_element by (coordinate, index): the index tie-break is what makes the
// median unique when a formation is a grid and a hundred points share a plane.
struct AxisLess {
    const dai_show_point *p;
    int axis;
    bool operator()(uint32_t a, uint32_t b) const {
        const float ca = (&p[a].x)[axis], cb = (&p[b].x)[axis];
        if (ca != cb) return ca < cb;
        return a < b;
    }
};

void cluster_split(const dai_show_point *f, const dai_show_point *t,
                   uint32_t *ridx, uint32_t *cidx, uint32_t begin, uint32_t end,
                   std::vector<Leaf> &leaves) {
    const uint32_t m = end - begin;
    if (m <= CLUSTER_TARGET) { leaves.push_back(Leaf{ begin, end }); return; }

    double lo[3] = { 1e308, 1e308, 1e308 }, hi[3] = { -1e308, -1e308, -1e308 };
    for (uint32_t k = begin; k < end; ++k) {
        const float *pa = &f[ridx[k]].x, *pb = &t[cidx[k]].x;
        for (int a = 0; a < 3; ++a) {
            if (pa[a] < lo[a]) lo[a] = pa[a];
            if (pa[a] > hi[a]) hi[a] = pa[a];
            if (pb[a] < lo[a]) lo[a] = pb[a];
            if (pb[a] > hi[a]) hi[a] = pb[a];
        }
    }
    int axis = 0;
    for (int a = 1; a < 3; ++a) if (hi[a] - lo[a] > hi[axis] - lo[axis]) axis = a;

    const uint32_t half = m / 2;
    AxisLess lr{ f, axis }, lc{ t, axis };
    std::nth_element(ridx + begin, ridx + begin + half, ridx + end, lr);
    std::nth_element(cidx + begin, cidx + begin + half, cidx + end, lc);

    cluster_split(f, t, ridx, cidx, begin, begin + half, leaves);
    cluster_split(f, t, ridx, cidx, begin + half, end, leaves);
}

// The seam pass. Neighbours come out of a uniform grid over the source
// formation; only pairs in different clusters are worth looking at, and a swap
// is taken only when it strictly shortens the pair. Walked in drone index
// order, so the greedy sequence - and therefore the result - is fixed.
uint32_t cluster_repair(const dai_show_point *f, const dai_show_point *t,
                        uint32_t n, const std::vector<uint32_t> &cluster_of,
                        uint32_t *perm) {
    if (n < 2) return 0;

    double lo[3] = { 1e308, 1e308, 1e308 }, hi[3] = { -1e308, -1e308, -1e308 };
    for (uint32_t i = 0; i < n; ++i) {
        const float *p = &f[i].x;
        for (int a = 0; a < 3; ++a) {
            if (p[a] < lo[a]) lo[a] = p[a];
            if (p[a] > hi[a]) hi[a] = p[a];
        }
    }
    double ext[3];
    for (int a = 0; a < 3; ++a) ext[a] = hi[a] - lo[a];
    double longest = ext[0] > ext[1] ? ext[0] : ext[1];
    if (ext[2] > longest) longest = ext[2];
    if (!(longest > 0.0)) return 0;

    // About one drone per cell, and never more cells than there are drones to
    // put in them - a grid is only a broadphase while it fits in cache.
    double cell = longest / std::cbrt((double)n);
    if (!(cell > 0.0)) return 0;
    uint32_t dim[3];
    for (int guard = 0; guard < 32; ++guard) {
        double cells = 1.0;
        for (int a = 0; a < 3; ++a) {
            double c = std::floor(ext[a] / cell) + 1.0;
            if (c < 1.0) c = 1.0;
            if (c > 1024.0) c = 1024.0;
            dim[a] = (uint32_t)c;
            cells *= c;
        }
        if (cells <= (double)n * 4.0 + 64.0) break;
        cell *= 1.5;
    }
    const size_t ncell = (size_t)dim[0] * dim[1] * dim[2];
    const double inv   = 1.0 / cell;

    std::vector<uint32_t> cell_of(n), start(ncell + 1, 0), items(n);
    for (uint32_t i = 0; i < n; ++i) {
        const float *p = &f[i].x;
        uint32_t g[3];
        for (int a = 0; a < 3; ++a) {
            double q = std::floor(((double)p[a] - lo[a]) * inv);
            if (q < 0.0) q = 0.0;
            if (q > (double)(dim[a] - 1)) q = (double)(dim[a] - 1);
            g[a] = (uint32_t)q;
        }
        cell_of[i] = (g[2] * dim[1] + g[1]) * dim[0] + g[0];
        ++start[cell_of[i] + 1];
    }
    for (size_t c = 0; c < ncell; ++c) start[c + 1] += start[c];
    {
        std::vector<uint32_t> fill(start.begin(), start.begin() + (ptrdiff_t)ncell);
        for (uint32_t i = 0; i < n; ++i) items[fill[cell_of[i]]++] = i;   // ascending
    }

    uint32_t swaps = 0;
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t pass_swaps = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t ci = cell_of[i];
            const uint32_t gx = ci % dim[0];
            const uint32_t gy = (ci / dim[0]) % dim[1];
            const uint32_t gz = ci / (dim[0] * dim[1]);
            bool done = false;
            for (int dz = -1; dz <= 1 && !done; ++dz) {
                const int z = (int)gz + dz;
                if (z < 0 || z >= (int)dim[2]) continue;
                for (int dy = -1; dy <= 1 && !done; ++dy) {
                    const int y = (int)gy + dy;
                    if (y < 0 || y >= (int)dim[1]) continue;
                    for (int dx = -1; dx <= 1 && !done; ++dx) {
                        const int x = (int)gx + dx;
                        if (x < 0 || x >= (int)dim[0]) continue;
                        const size_t c = ((size_t)z * dim[1] + (size_t)y) * dim[0] + (size_t)x;
                        for (uint32_t s = start[c]; s < start[c + 1]; ++s) {
                            const uint32_t k = items[s];
                            if (k <= i) continue;
                            if (cluster_of[k] == cluster_of[i]) continue;
                            const double now  = cost_of(f[i], t[perm[i]]) + cost_of(f[k], t[perm[k]]);
                            const double swap = cost_of(f[i], t[perm[k]]) + cost_of(f[k], t[perm[i]]);
                            if (now - swap > 1e-9) {
                                const uint32_t tmp = perm[i];
                                perm[i] = perm[k];
                                perm[k] = tmp;
                                ++pass_swaps;
                                done = true;                 // one swap per drone per pass
                                break;
                            }
                        }
                    }
                }
            }
        }
        swaps += pass_swaps;
        if (pass_swaps == 0) break;
    }
    return swaps;
}

uint32_t cluster_solve(const dai_show_point *f, const dai_show_point *t, uint32_t n,
                       uint32_t *out_perm, uint32_t *swaps_out) {
    if (n == 0) return 0;
    std::vector<uint32_t> ridx(n), cidx(n);
    for (uint32_t i = 0; i < n; ++i) { ridx[i] = i; cidx[i] = i; }

    std::vector<Leaf> leaves;
    leaves.reserve((size_t)(n / CLUSTER_TARGET + 2));
    cluster_split(f, t, ridx.data(), cidx.data(), 0, n, leaves);

    std::vector<uint32_t> cluster_of(n, 0);
    for (uint32_t l = 0; l < (uint32_t)leaves.size(); ++l)
        for (uint32_t k = leaves[l].begin; k < leaves[l].end; ++k) cluster_of[ridx[k]] = l;

    // Every leaf writes to its own slice of out_perm and reads nothing another
    // leaf writes, so the parallelism has no reduction and therefore no order.
    auto run_leaves = [&](uint32_t begin, uint32_t end) {
        std::vector<dai_show_point> sf, st;
        std::vector<uint32_t>       sp;
        for (uint32_t l = begin; l < end; ++l) {
            const uint32_t m = leaves[l].end - leaves[l].begin;
            sf.resize(m); st.resize(m); sp.resize(m);
            for (uint32_t k = 0; k < m; ++k) {
                sf[k] = f[ridx[leaves[l].begin + k]];
                st[k] = t[cidx[leaves[l].begin + k]];
            }
            jv_solve(sf.data(), st.data(), m, sp.data(), nullptr);
            for (uint32_t k = 0; k < m; ++k)
                out_perm[ridx[leaves[l].begin + k]] = cidx[leaves[l].begin + sp[k]];
        }
    };

    const uint32_t nl = (uint32_t)leaves.size();
    if (nl > 1) {
        const unsigned w = worker_count(nl);
        std::vector<std::thread> pool;
        pool.reserve(w);
        for (unsigned wi = 0; wi < w; ++wi) {
            const uint32_t b = (uint32_t)((uint64_t)nl * wi / w);
            const uint32_t e = (uint32_t)((uint64_t)nl * (wi + 1) / w);
            if (b < e) pool.emplace_back(run_leaves, b, e);
        }
        for (auto &th : pool) th.join();
    } else {
        run_leaves(0, nl);
    }

    const uint32_t sw = cluster_repair(f, t, n, cluster_of, out_perm);
    if (swaps_out) *swaps_out = sw;
    return nl;
}

double steady_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

} // namespace

// ---- the one call --------------------------------------------------------

extern "C" DAI_API dai_result dai_show_assign(const dai_show_point *from,
                                              const dai_show_point *to, uint32_t n,
                                              int method, uint32_t *out_perm,
                                              dai_show_assign_stats *stats) {
    if (stats) std::memset(stats, 0, sizeof(*stats));
    if (!from || !to || !out_perm || n == 0) return DAI_ERR_INVALID_ARG;

    int used = method;
    if (used == DAI_SHOW_ASSIGN_AUTO)
        used = (n <= DAI_SHOW_EXACT_MAX) ? DAI_SHOW_ASSIGN_EXACT : DAI_SHOW_ASSIGN_CLUSTER;
    if (used != DAI_SHOW_ASSIGN_EXACT && used != DAI_SHOW_ASSIGN_AUCTION &&
        used != DAI_SHOW_ASSIGN_CLUSTER)
        return DAI_ERR_INVALID_ARG;

    uint32_t iterations = 0, clusters = 1;

    // The clock is read here, around the solver, and the number it produces
    // goes into the stats and nowhere else. No branch below depends on it -
    // that is the whole rule this engine is built on, and a solver that took a
    // shortcut when the machine was slow would break it.
    const double t0 = steady_ms();
    switch (used) {
    case DAI_SHOW_ASSIGN_EXACT:
        jv_solve(from, to, n, out_perm, &iterations);
        break;
    case DAI_SHOW_ASSIGN_AUCTION:
        auction_solve(from, to, n, out_perm, &iterations);
        break;
    default:
        clusters = cluster_solve(from, to, n, out_perm, &iterations);
        break;
    }
    const double t1 = steady_ms();

    if (!stats) return DAI_OK;

    stats->method_used = used;
    stats->clusters    = clusters;
    stats->iterations  = iterations;
    stats->solve_ms    = t1 - t0;
    stats->total_cost_m = total_metres(from, to, n, out_perm);

    if (used == DAI_SHOW_ASSIGN_EXACT) {
        stats->exact_cost_m = stats->total_cost_m;
        stats->gap_percent  = 0.0f;
    } else if (n <= DAI_SHOW_EXACT_MAX) {
        // Small enough that the truth is affordable, so the director is told
        // what the fast path cost instead of being asked to trust it. Measured
        // outside the timing above: this work is not part of the solve.
        std::vector<uint32_t> ref(n);
        jv_solve(from, to, n, ref.data(), nullptr);
        stats->exact_cost_m = total_metres(from, to, n, ref.data());
        // The objective is squared distance, so a heuristic can come out a hair
        // SHORTER in plain metres while being worse by the measure that was
        // minimised. That is a zero gap, not "not computed" - which is what a
        // negative number means in this field.
        double g = (stats->exact_cost_m > 0.0)
                 ? 100.0 * (stats->total_cost_m - stats->exact_cost_m) / stats->exact_cost_m
                 : 0.0;
        stats->gap_percent = (g > 0.0) ? (float)g : 0.0f;
    } else {
        stats->exact_cost_m = -1.0;              // never computed, never guessed
        stats->gap_percent  = -1.0f;
    }
    return DAI_OK;
}

extern "C" DAI_API double dai_show_assign_brute_force(const dai_show_point *from,
                                                      const dai_show_point *to,
                                                      uint32_t n, uint32_t *out_perm) {
    // Enumeration, so the test has an answer that owes the solver nothing. It
    // minimises the same objective the solvers minimise - squared distance -
    // and returns the metres that permutation flies, which is exactly what
    // dai_show_assign_stats::total_cost_m reports.
    if (!from || !to || n == 0 || n > 8) return -1.0;

    uint32_t perm[8], best[8];
    for (uint32_t i = 0; i < n; ++i) { perm[i] = i; best[i] = i; }
    double best_sq = total_squared(from, to, n, perm);
    while (std::next_permutation(perm, perm + n)) {
        const double sq = total_squared(from, to, n, perm);
        if (sq < best_sq) {
            best_sq = sq;
            for (uint32_t i = 0; i < n; ++i) best[i] = perm[i];
        }
    }
    if (out_perm) for (uint32_t i = 0; i < n; ++i) out_perm[i] = best[i];
    return total_metres(from, to, n, best);
}
