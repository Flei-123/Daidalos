// The numbers the drone show stages have to agree on, in one place.
//
// Not part of include/dai_show.h on purpose: none of this is a promise to a
// caller, it is a promise the stages make to each other. A tolerance that lives
// twice lives at two values sooner or later, and the day the separator's limit
// and the validator's limit differ by a millimetre is the day the tool reports
// eleven conflicts in a transition it has just declared clean - which is
// exactly how this file came to exist.
//
// Everything in here is a compile time constant, so no stage pays for it.

#ifndef DAI_SHOW_INTERNAL_HPP
#define DAI_SHOW_INTERNAL_HPP

namespace daishow {

// The slack around min_distance, in metres.
//
// A formation is normally built AT the minimum distance - Poisson sampling
// packs to the limit and Lloyd relaxation pushes it back to it - so a pair
// sitting at exactly min_distance is the ordinary case, not a fault. Without a
// tolerance every such pair is reported the moment the arithmetic rounds the
// wrong way, and a list of a thousand phantom conflicts is a list nobody reads.
//
// A tenth of a millimetre is far below anything a GPS-guided drone can hold
// station to, and far above the rounding of a float distance computed over a
// field a few hundred metres across. Stage 3 separates to this bound and stage
// 5 judges by it - the same number, from here.
const float SEP_EPS = 1e-4f;

} // namespace daishow

#endif // DAI_SHOW_INTERNAL_HPP
