// MODULE 1 (blockout) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE the Add Component list in src/dai_editor_ui.cpp, where the
// built-in entries are collected. In scope here:
//
//   std::vector<CompEntry> entries    { label, category, kind, path }
//   dai_node_desc          ar2        the node the menu was opened for
//   int                    have_node  is ar2 filled in?
//
// `kind` values 0..9 are taken (rigidbody, collider, camera, light, sprite,
// audio, behaviour, text, image, button). Module 1 takes 20 and up, and
// handles them in the switch that follows the list - which lives in this
// module's second statement seam if one is needed, or in the existing switch
// through a `kind >= 20` branch. Keep the numbers out of the middle of the
// existing range: a renumbering that silently turns "add a Light" into "add a
// Wedge" is the kind of bug that survives a review.
//
// Categories: use "Blockout" for the shapes and the CSG node, so the search
// list groups them the way "Physics" and "Rendering" group theirs.
//
// A component that is already on the node does NOT appear - that is the rule
// the entries above follow, and it is the only feedback the menu gives.

/* module 1 fills this in */
