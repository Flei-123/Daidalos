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


// ---- Blockout ------------------------------------------------------------
// The shape, its size in metres and where its origin sits. The fold states are
// function statics rather than fields on dai_editor_ui: this seam owns them,
// and a panel's fold state is not something another module should have to know
// about to compile.
{
    static int fold_blockout = 1, fold_csg = 1, fold_door = 1;

    // A socket with a size IS a socket, even when nobody ticked a box: a
    // script or the bridge that sets door.width and door.height has said what
    // it means. Same rule as blockout_has_socket() in
    // src/dai_editor_ui_blockout.inl - that file is included further down, so
    // the rule is written here as a lambda rather than called across a seam
    // that does not exist yet at this point in the translation unit.
    auto has_socket = [](const dai_node_desc &q) {
        return q.door_socket || q.door_width > 0.0f || q.door_height > 0.0f;
    };

    if (r.blockout) {
        int on = 1;
        if (dai_ui_header_icon_col(p->ui, DAI_ICON_C_MESH, rgba(0x9C, 0xD6, 0x7A, 255),
                                   "Blockout", &fold_blockout, &on) == 2 && !on)
            r.blockout = DAI_BLOCKOUT_NONE;
        if (fold_blockout && r.blockout) {
            static const char *const KINDS[] = { "Box", "Cylinder", "Stairs", "Arch", "Wedge" };
            int kind = r.blockout - 1;
            if (kind < 0) kind = 0;
            if (kind > 4) kind = 4;
            if (dai_ui_option(p->ui, "Shape", &kind, KINDS, 5)) r.blockout = kind + 1;

            // FULL size, in metres - the number on a tape measure, not half of
            // it. A zero on any axis would build nothing, so the field floor is
            // a millimetre and the mesh builder reads the same value back.
            float size[3] = { r.blockout_size.x > 0 ? r.blockout_size.x : 1.0f,
                              r.blockout_size.y > 0 ? r.blockout_size.y : 1.0f,
                              r.blockout_size.z > 0 ? r.blockout_size.z : 1.0f };
            if (dai_ui_num_vec3(p->ui, "Size (m)", size, 0.05f))
                r.blockout_size = dai_vec3{ size[0], size[1], size[2] };

            if (r.blockout == DAI_BLOCKOUT_CYLINDER || r.blockout == DAI_BLOCKOUT_ARCH) {
                float seg = (float)(r.blockout_segments > 0 ? r.blockout_segments : 16);
                if (dai_ui_num_field(p->ui, "Segments", &seg, 1.0f, 3.0f, 256.0f, "boseg"))
                    r.blockout_segments = (int)(seg + 0.5f);
            }
            if (r.blockout == DAI_BLOCKOUT_STAIRS) {
                float st = (float)(r.blockout_steps > 0 ? r.blockout_steps : 8);
                if (dai_ui_num_field(p->ui, "Steps", &st, 1.0f, 1.0f, 128.0f, "bosteps"))
                    r.blockout_steps = (int)(st + 0.5f);
                float rise = (r.blockout_size.y > 0 ? r.blockout_size.y : 1.0f) /
                             (float)(r.blockout_steps > 0 ? r.blockout_steps : 8);
                dai_ui_label_fmt(p->ui, "Rise %.3f m, going %.3f m", rise,
                                 (r.blockout_size.z > 0 ? r.blockout_size.z : 1.0f) /
                                 (float)(r.blockout_steps > 0 ? r.blockout_steps : 8));
            }
            if (r.blockout == DAI_BLOCKOUT_ARCH)
                dai_ui_num_field(p->ui, "Thickness (m)", &r.blockout_thickness,
                                 0.01f, 0.0f, 100.0f, "bothick");

            // -1 puts the origin on the min face, +1 on the max, 0 in the
            // middle. A wall is built from the floor up, so its Y is -1 - and
            // the mesh builder reads these three numbers exactly as they are
            // shown here, no rounding in between.
            float pv[3] = { r.blockout_pivot.x, r.blockout_pivot.y, r.blockout_pivot.z };
            if (dai_ui_num_vec3(p->ui, "Pivot", pv, 0.1f)) {
                for (int i = 0; i < 3; ++i) pv[i] = pv[i] < -1.0f ? -1.0f : (pv[i] > 1.0f ? 1.0f : pv[i]);
                r.blockout_pivot = dai_vec3{ pv[0], pv[1], pv[2] };
            }
        }
    }

    // ---- CSG -------------------------------------------------------------
    // The node's blockout children, combined in hierarchy order. The children
    // stop drawing themselves the moment this is switched on - they are the
    // cutters now, not objects in their own right.
    if (r.csg) {
        int on = 1;
        if (dai_ui_header_icon_col(p->ui, DAI_ICON_C_COLLIDER, rgba(0xE0, 0x8B, 0x5A, 255),
                                   "CSG", &fold_csg, &on) == 2 && !on)
            r.csg = DAI_CSG_NONE;
        if (fold_csg && r.csg) {
            static const char *const OPS[] = { "Union", "Subtract", "Intersect" };
            int op = r.csg - 1;
            if (op < 0) op = 0;
            if (op > 2) op = 2;
            if (dai_ui_option(p->ui, "Operation", &op, OPS, 3)) r.csg = op + 1;
            uint32_t kids = dai_doc_children(d, n, nullptr, 0);
            if (!kids)
                dai_ui_label(p->ui, "No children - add a Box under this node");
            else if (r.csg == DAI_CSG_SUBTRACT)
                dai_ui_label_fmt(p->ui, "%u child shape(s) cut out of this one", kids);
            else
                dai_ui_label_fmt(p->ui, "%u child shape(s) combined with this one", kids);
        }
    }

    // ---- DoorSocket ------------------------------------------------------
    // Where the next room may be docked on: a place, a facing, and an opening.
    // Drawn in the viewport by dai_blockout_draw_sockets - an anchor point
    // nobody can see is an anchor point nobody trusts.
    if (has_socket(r)) {
        int on = 1;
        if (dai_ui_header_icon_col(p->ui, DAI_ICON_C_PREFAB, rgba(0x7A, 0xB8, 0xE6, 255),
                                   "Door Socket", &fold_door, &on) == 2 && !on) {
            // Removing it takes the SIZE away too, or the gizmo would keep
            // drawing an opening the header says is not there.
            r.door_socket = 0;
            r.door_width = 0;
            r.door_height = 0;
        }
        if (fold_door && has_socket(r)) {
            float off[3] = { r.door_offset.x, r.door_offset.y, r.door_offset.z };
            if (dai_ui_num_vec3(p->ui, "Position", off, 0.05f))
                r.door_offset = dai_vec3{ off[0], off[1], off[2] };
            float nrm[3] = { r.door_normal.x, r.door_normal.y, r.door_normal.z };
            if (nrm[0] == 0.0f && nrm[1] == 0.0f && nrm[2] == 0.0f) nrm[2] = 1.0f;
            if (dai_ui_num_vec3(p->ui, "Normal", nrm, 0.05f))
                r.door_normal = dai_vec3{ nrm[0], nrm[1], nrm[2] };
            float dw = r.door_width > 0 ? r.door_width : 0.9f;
            float dh = r.door_height > 0 ? r.door_height : 2.0f;
            if (dai_ui_num_field(p->ui, "Width (m)", &dw, 0.05f, 0.1f, 20.0f, "doorw"))
                r.door_width = dw;
            if (dai_ui_num_field(p->ui, "Height (m)", &dh, 0.05f, 0.1f, 20.0f, "doorh"))
                r.door_height = dh;
        }
    }
}
