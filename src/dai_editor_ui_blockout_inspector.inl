// MODULE 1 (blockout) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE inspector_body() in src/dai_editor_ui.cpp, immediately
// before the clamp block that ends it. In scope here:
//
//   dai_editor_ui *p        the panel
//   dai_doc       *d        the document
//   dai_node       n        the node being inspected
//   dai_node_desc &r        its record - CHANGE THIS, nothing else
//   dai_node_desc  before   what it looked like when the panel started
//
// Do not call dai_doc_set here. The code right after this seam diffs `r`
// against `before`, writes it in ONE undo step, and pushes the same change
// into a multiple selection. Writing the document from inside the seam would
// give every drag its own undo step and would skip the multi-edit entirely -
// which is exactly the bug the surrounding code exists to prevent.
//
// What belongs here: the Box / Cylinder / Stairs / Arch / Wedge section (size,
// segments, pivot), the CSG section (union / subtract / intersect, and what
// the children of this node do), and the DoorSocket section (position,
// normal, width, height). Same shape as the sections above: a fold header, a
// tick box, then rows of dai_ui_num_field / dai_ui_vec3_field.
//
// The one thing that is easy to get wrong: a field that changes the SHAPE
// must not be clamped or rounded here differently from the way the mesh
// builder reads it, or the panel and the geometry disagree about what "2.5 m"
// means.

/* module 1 fills this in */
