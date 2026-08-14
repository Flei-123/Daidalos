// Stages 3 and 4: crossings are separated, and motion is shaped in time.
//
// Implements, from include/dai_show.h:
//     dai_show_layer
//     dai_show_min_duration
//     dai_show_transition_default
//
// Layering is the stage with the safety claim on it. Two straight legs that
// cross in space are found with the same uniform grid the validator uses -
// never by testing every pair - and separated by lifting one of them onto a
// height layer, or by delaying it, or both. The layers are handed out in a
// fixed order over pairs sorted by (lower index, higher index), so the result
// does not depend on how the work was divided.
//
// What cannot be separated inside the duration allowed is COUNTED and handed
// back, and dai_show_layer returns DAI_ERR_STATE rather than DAI_OK. A tool
// that reports success while leaving two drones on a collision course is worse
// than a tool that does nothing.
//
// The profiles are the other half: v = 0 at both ends is a cubic Bezier, eased
// at one end is the same curve with one control point moved, and the linear
// case is what a director asks for when the figure has to arrive on a beat.
// dai_show_min_duration is the inverse - given the leg and the limits, the
// shortest time that does not break v_max or a_max.
