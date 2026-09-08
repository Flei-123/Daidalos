// MODULE 2 (triplanar) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE asset_inspector_body() in src/dai_editor_ui.cpp, in the
// material branch, under the four rows the panel already draws. In scope:
//
//   dai_editor_ui *p     the panel      (p->inspect_mat is the parsed view)
//   dai_ui        *ui    the widgets    (dai_ui_num_field, dai_ui_check, ...)
//   const dai_ui_style *st
//   const std::string &path   the .daimat, relative to the project's assets
//   int changed          OR your rows into this; it is what lights up Save
//
// What belongs here: the three map slots (base colour, ORM, normal) as asset
// fields, the "Triplanar" tick box, the tiling size IN METRES and the blend
// sharpness. Two things to get right, both of them things a reviewer will
// look for:
//
//   * The tiling is metres, not repeats. A world projected texture has no UV
//     set to repeat against - "2 m" is a number an artist can hold in their
//     head and a number that stays true when the wall gets longer.
//   * The panel must say what triplanar costs (three samples per map) where
//     the user can read it, so nobody switches it on for every material in the
//     project and then wonders where the frame rate went.
//
// State that has to survive between frames goes in a function-local `static`
// keyed by `path`, or in the parse seam's own storage - do NOT add fields to
// struct dai_editor_ui, which is not this module's file.

/* module 2 fills this in */
