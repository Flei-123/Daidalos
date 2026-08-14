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
//        exact one at n = 512 (assert the gap, and print it).
//   [2d] AUTO picks exact below DAI_SHOW_EXACT_MAX and a scaling method above.
//   [2e] the stats' total_cost_m is the real summed distance of the returned
//        permutation - recomputed independently here, because a solver that
//        reports its own cost is a solver marking its own homework.
//   [2f] a translated formation (every point moved by the same vector) assigns
//        identity - the case where the right answer is obvious and a broken
//        cost matrix is not.
#include "droneshow_cases.hpp"
