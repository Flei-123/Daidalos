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

// kind 20..24 are the five shapes, 25 the CSG node, 26 the DoorSocket. The
// numbers start at 20 so nothing here can ever collide with the built-in
// range (0..9) by being renumbered - a menu that silently turns "add a Light"
// into "add a Wedge" is the kind of bug that survives a review.
//
// A node carries ONE shape: five entries while it has none, and none of them
// once it has one, exactly like Camera and Light above. Changing a Box into a
// Wedge is the Shape option in the inspector, not a second component.
if (!ar2.blockout) {
    entries.push_back({ "Box",      "Blockout", 20, "" });
    entries.push_back({ "Cylinder", "Blockout", 21, "" });
    entries.push_back({ "Stairs",   "Blockout", 22, "" });
    entries.push_back({ "Arch",     "Blockout", 23, "" });
    entries.push_back({ "Wedge",    "Blockout", 24, "" });
}
if (!ar2.csg)         entries.push_back({ "CSG (boolean)", "Blockout", 25, "" });
if (!ar2.door_socket) entries.push_back({ "Door Socket",   "Blockout", 26, "" });

// kind 30..34 are the five modifiers. A node may carry EIGHT of them and the
// same type more than once - two arrays at right angles is a grid, two bevels
// is a wide break and a fine one - so unlike the shapes above these entries
// stay in the menu until the stack is full.
//
// A modifier without a shape under it has nothing to modify, so they are
// offered on a node that is a blockout or a CSG node, which is the same rule
// the inspector's list draws itself by.
if ((ar2.blockout || ar2.csg) && ar2.modifier_count < DAI_MODIFIER_MAX) {
    entries.push_back({ "Modifier > Bevel",     "Blockout", 30, "" });
    entries.push_back({ "Modifier > Subdivide", "Blockout", 31, "" });
    entries.push_back({ "Modifier > Solidify",  "Blockout", 32, "" });
    entries.push_back({ "Modifier > Array",     "Blockout", 33, "" });
    entries.push_back({ "Modifier > Mirror",    "Blockout", 34, "" });
}
