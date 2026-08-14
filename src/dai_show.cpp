// The show document: formations, the storyboard, and the plan they solve into.
//
// Implements, from include/dai_show.h:
//     dai_show_settings_default
//     dai_show_create / destroy / get_settings / set_settings
//     dai_show_formation_* and dai_show_transition_get/set
//     dai_show_solve, dai_show_get_plan, dai_show_get_timings
//     dai_show_validate_show, dai_show_conflict_count, dai_show_conflict_at
//     dai_show_save / dai_show_load
//     the dai_show_plan container: from_keys, sample, sample_all, key_at,
//     drone_count, duration, keyframe_count, bytes, destroy
//
// This is dai_doc's argument one domain over: the document is the truth and
// everything else is derived from it. A formation is a record with a stable
// index, a transition belongs to the formation it arrives at, and the plan is
// a cache that is thrown away the moment a setting that could invalidate it
// changes - because a stale "no conflicts" badge is the one bug in this
// program that could hurt somebody.
//
// THE PLAN IS KEYFRAMES. Per drone: an ordered (time, position, colour, how we
// got here) list, one key per formation plus at most two for a layered detour.
// 10,000 drones through 20 formations is under 20 MB. Materialising 30,000
// ticks instead would be 3.6 GB, which is the whole reason dai_show_plan_sample
// is the only reader.
//
// Clocks are read HERE and nowhere else: dai_show_solve times the stages from
// the outside with a monotonic clock and writes the numbers into the timings.
// Nothing inside a solver ever asks what time it is.
