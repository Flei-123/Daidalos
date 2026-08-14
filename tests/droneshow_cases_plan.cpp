// Stages 3 and 4 under test: constructed crossings, and limits that hold.
//
// Implements show_cases_plan() - see tests/droneshow_cases.hpp.
//
//   [3a] THE CONSTRUCTED CROSSING: two drones swapping places on a straight
//        line pass through the same point at the same instant. After layering,
//        sample the legs densely and assert the separation never drops below
//        min_distance. Then the same for four drones crossing at one point,
//        and for a formation rotated 180 degrees, where every path crosses
//        every other.
//   [3b] the layering reports what it could not fix: an impossible transition
//        (two formations one metre apart with a 5 m minimum) returns
//        DAI_ERR_STATE and a non-zero unresolved count. Silence here would be
//        the worst failure this program has.
//   [3c] SMOOTH really has v = 0 at both ends, and LINEAR really does not.
//   [3d] v_max and a_max hold across the whole leg for every profile, measured
//        by differencing the sampled positions - and dai_show_min_duration is
//        the boundary: one percent under it breaks a limit, one percent over
//        it does not.
//   [3e] STAGGERED spreads the departures over stagger_s and every drone still
//        arrives before the transition ends.
//   [3f] the plan is keyframes: a 10,000 drone show reports a byte count far
//        below the materialised alternative, and sampling at a keyframe time
//        returns that keyframe's position exactly.
#include "droneshow_cases.hpp"
