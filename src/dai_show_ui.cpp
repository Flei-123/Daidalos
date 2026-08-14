// The drone show panels: storyboard, parameters, validation, timeline.
//
// Implements include/dai_show_ui.h. This is the one file that knows about both
// dai_show and dai_ui - the same division dai_editor / dai_editor_ui exists
// for, and for the same reason: the pipeline has to be drivable from a script,
// a test or another frontend without dragging an interface in.
//
// Immediate mode, like everything else here. The panels are rebuilt from the
// document every frame, so a formation deleted in the storyboard cannot leave
// a stale row in the validation list - which in this program is not a cosmetic
// bug but a conflict that has stopped being shown.
