// Stage 1: a mesh becomes exactly N points that are never too close together.
//
// Implements, from include/dai_show.h:
//     dai_show_sample_feasible
//     dai_show_sample
//
// The order of the work is the part worth writing down. Feasibility FIRST,
// because "N drones at d metres" is a packing question with a closed form
// answer and a tool that discovers the impossibility by failing to converge
// has already wasted the operator's afternoon. Then dart throwing against a
// spatial hash for the Poisson disk property, then Lloyd relaxation to even
// the result out - and the relaxation has to re-check the minimum distance
// afterwards, because moving a point to its cell's centroid can move it
// towards a neighbour.
//
// Everything draws from the seed in the descriptor. There is no rand() here.
