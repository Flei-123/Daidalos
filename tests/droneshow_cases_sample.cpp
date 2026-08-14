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
