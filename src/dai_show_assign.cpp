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
