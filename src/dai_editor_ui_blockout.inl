// MODULE 1 (blockout) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included by src/dai_editor_ui.cpp after the collider wireframe section, so
// everything that file already has for drawing in 3D is in scope:
//
//   wire_line(p, a, b, colour, thickness)      a world space segment
//   wire_circle / wire_arc                     the same, curved
//   qrot_v(q, v), v_add(a, b)                  the vector helpers
//   dai_editor_project(p->ed, world, &x, &y)   world point -> panel pixel
//   dai_editor_project_seg(...)                world segment -> panel pixels
//
// What belongs here: the DoorSocket gizmo. A socket is a position, a normal, a
// width and a height, and it has to be VISIBLE - an anchor point you cannot
// see is an anchor point nobody trusts. Draw the opening as a rectangle in the
// wall plane plus an arrow along the normal, so which way the door faces is
// readable at a glance rather than by reading the numbers.
//
// The selected node's sockets are drawn brighter than the rest, the same rule
// the collider wireframes follow.

static void dai_blockout_draw_sockets(dai_editor_ui *p) {
    (void)p;   /* module 1 fills this in */
}
