// Stage 5: the whole timeline, checked, without ever squaring the fleet.
//
// Implements, from include/dai_show.h:
//     dai_show_validate
//
// Two numbers decide the design. 10,000 drones is 5*10^7 pairs; a five minute
// show at 25 fps is 7,500 ticks. Multiplied, that is a number no laptop will
// finish, so the pairwise test goes through a uniform grid whose cell edge is
// the minimum distance - a drone can only be too close to something in its own
// cell or the 26 around it, and the grid is rebuilt per tick from positions
// that were never stored.
//
// The second number is memory: the fleet exists for one tick at a time,
// sampled out of the keyframes, so the check streams and its peak is two
// frames of positions plus the grid.
//
// Speed and acceleration come from finite differences over the same stream,
// which is also what makes a deliberately broken plan detectable - the test
// builds one and this file has to find it.
//
// Conflicts come out sorted by (time, a, b). Not because it is tidy: an
// unsorted list built by several threads is a different list every run, and
// this program's answers have to be reproducible.
