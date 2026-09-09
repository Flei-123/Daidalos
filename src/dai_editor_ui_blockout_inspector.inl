// MODULE 1 (blockout) OWNS THIS FILE. Statement seam, see include/dai_ext.h.
//
// Included INSIDE inspector_body() in src/dai_editor_ui.cpp, after the Audio
// section and before Remove/Add Component - among the other components, not
// below the Add Component button, where a section is never seen. In scope here:
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

    // A status line in this section is written short, and then MEASURED: the
    // inspector is a 200 px column in the 1100x700 layout, dai_ui_label clips
    // at the panel's inner edge, and a hard clip ends a sentence in the middle
    // of a glyph. This ends it in an ellipsis instead, at the last whole
    // character that still fits into panel_w - 2 * padding. Nothing else in
    // the seam prints text a field does not carry.
    auto fit_label = [&](const char *text) {
        const dai_ui_style *st = dai_ui_style_of(p->ui);
        float room = dai_ui_panel_width(p->ui) - (st ? st->padding : 8.0f) * 2.0f;
        if (room <= 0.0f || dai_ui_text_width(p->ui, text) <= room) { dai_ui_label(p->ui, text); return; }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s", text);
        size_t n = std::strlen(buf);
        while (n > 0) {
            --n;
            char keep[164];
            size_t k = n < sizeof(keep) - 4 ? n : sizeof(keep) - 4;
            std::memcpy(keep, buf, k);
            std::strcpy(keep + k, "...");
            if (dai_ui_text_width(p->ui, keep) <= room) { dai_ui_label(p->ui, keep); return; }
        }
        dai_ui_label(p->ui, "...");
    };
    auto fit_label_fmt = [&](const char *fmt, auto... args) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        fit_label(buf);
    };

    if (r.blockout) {
        // The mesh is built in metres and the host divides it by the entity's
        // render scale (entity_render_scale in include/dai_blockout_host.inl),
        // so a blockout node's render_extent is 1,1,1 - "the model is already
        // the size it says it is" - and nothing else. Add Component sets it;
        // a node that arrived another way (a script, an older file) is pinned
        // here, once, as part of the same edit that inspects it. The Mesh
        // Renderer's own Size row is not drawn for these nodes for the same
        // reason.
        if (r.render_extent.x != 1.0f || r.render_extent.y != 1.0f || r.render_extent.z != 1.0f)
            r.render_extent = dai_vec3{ 1.0f, 1.0f, 1.0f };
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
                fit_label_fmt("Rise %.3f, run %.3f m", rise,
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
            // Short on purpose: the inspector is a 200 px column at 1100 px
            // and a sentence that does not fit is a sentence that is cut.
            uint32_t kids = dai_doc_children(d, n, nullptr, 0);
            if (!kids)
                fit_label("No children - add a Box below");
            else if (r.csg == DAI_CSG_SUBTRACT)
                fit_label_fmt("%u %s cut out", kids, kids == 1 ? "child" : "children");
            else
                fit_label_fmt("%u %s combined", kids, kids == 1 ? "child" : "children");
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

    // ---- Modifiers -------------------------------------------------------
    // The list of rules that turns the rough shape into a detailed one, run
    // top down. The ORDER is what the list is for - an array of a bevelled
    // step is nine bevelled steps, a bevel of an arrayed step is one long
    // bevel down the joins - so every entry carries its two arrows, and the
    // tick box in its header switches it off without losing its numbers.
    //
    // Only the fields of THIS entry's type are drawn. A bevel has no copy
    // count and an array has no angle threshold, and a row that means nothing
    // is a row somebody will eventually type a number into.
    if (r.modifier_count > 0) {
        if (r.modifier_count > DAI_MODIFIER_MAX) r.modifier_count = DAI_MODIFIER_MAX;
        static int fold_mods[DAI_MODIFIER_MAX] = { 1, 1, 1, 1, 1, 1, 1, 1 };
        static const char *const MOD_NAMES[DAI_MOD_TYPE_COUNT] = {
            "Modifier", "Bevel", "Subdivide", "Solidify", "Array", "Mirror"
        };
        static const char *const AXES[] = { "X", "Y", "Z" };
        // What the list is about to do to itself. Applied AFTER the loop:
        // moving an entry while the loop that draws it is still running would
        // draw one entry twice and skip its neighbour.
        int move_from = -1, move_to = -1, remove_at = -1;

        for (int mi = 0; mi < r.modifier_count && mi < DAI_MODIFIER_MAX; ++mi) {
            dai_modifier &m = r.modifiers[mi];
            int t = (m.type > 0 && m.type < DAI_MOD_TYPE_COUNT) ? m.type : 0;
            char title[48];
            std::snprintf(title, sizeof(title), "%d  %s", mi + 1, MOD_NAMES[t]);
            // One hue per type, the way every other component header here is
            // told apart by colour before it is read.
            uint32_t tint = rgba(0xB0, 0x9C, 0xE0, 255);
            switch (t) {
            case DAI_MOD_BEVEL:     tint = rgba(0xE0, 0xB8, 0x6A, 255); break;
            case DAI_MOD_SUBDIVIDE: tint = rgba(0x8A, 0xC8, 0xD8, 255); break;
            case DAI_MOD_SOLIDIFY:  tint = rgba(0xC8, 0x9A, 0xD8, 255); break;
            case DAI_MOD_ARRAY:     tint = rgba(0x9C, 0xD6, 0x7A, 255); break;
            case DAI_MOD_MIRROR:    tint = rgba(0xE0, 0x8B, 0x5A, 255); break;
            default: break;
            }
            int on = m.off ? 0 : 1;
            // The tick box does NOT delete the entry - that is what the minus
            // button is for. An entry that is off keeps every number it had,
            // which is the only way "switch it off and see" is useful.
            if (dai_ui_header_icon_col(p->ui, DAI_ICON_C_MATERIAL, tint, title,
                                       &fold_mods[mi], &on) == 2)
                m.off = on ? 0 : 1;
            if (!fold_mods[mi]) continue;

            // Up, down, remove. Icons rather than words: three text buttons
            // do not fit into a 200 px inspector column at 1100x700, and an
            // arrow that is 18 px wide is still an arrow.
            dai_ui_row(p->ui, 20.0f);
            if (dai_ui_icon_button(p->ui, DAI_ICON_ARROW_UP, "Move up", 0) && mi > 0) {
                move_from = mi; move_to = mi - 1;
            }
            if (dai_ui_icon_button(p->ui, DAI_ICON_ARROW_DOWN, "Move down", 0) &&
                mi + 1 < r.modifier_count) {
                move_from = mi; move_to = mi + 1;
            }
            if (dai_ui_icon_button(p->ui, DAI_ICON_MINUS, "Remove", 0)) remove_at = mi;
            dai_ui_row_end(p->ui);

            // A zero is "the default" in the document, and the number the
            // panel shows is the one the geometry reads - bevel_of() and its
            // siblings in include/dai_modifier.h, not a second opinion.
            switch (t) {
            case DAI_MOD_BEVEL: {
                float w = m.amount > 0 ? m.amount : 0.02f;
                if (dai_ui_num_field(p->ui, "Width (m)", &w, 0.005f, 0.0005f, 10.0f, "modw"))
                    m.amount = w;
                float seg = (float)(m.count > 0 ? (m.count > 4 ? 4 : m.count) : 1);
                if (dai_ui_num_field(p->ui, "Segments", &seg, 1.0f, 1.0f, 4.0f, "modseg"))
                    m.count = (int)(seg + 0.5f);
                float ang = m.angle > 0 ? m.angle : 30.0f;
                if (dai_ui_num_field(p->ui, "Angle (deg)", &ang, 1.0f, 1.0f, 180.0f, "modang"))
                    m.angle = ang;
                break;
            }
            case DAI_MOD_SUBDIVIDE: {
                float lv = (float)(m.count > 0 ? (m.count > 3 ? 3 : m.count) : 1);
                if (dai_ui_num_field(p->ui, "Level", &lv, 1.0f, 1.0f, 3.0f, "modlvl"))
                    m.count = (int)(lv + 0.5f);
                dai_ui_checkbox(p->ui, "Smooth", &m.smooth);
                break;
            }
            case DAI_MOD_SOLIDIFY: {
                float th = m.amount > 0 ? m.amount : 0.05f;
                if (dai_ui_num_field(p->ui, "Thickness (m)", &th, 0.005f, 0.0005f, 10.0f, "modth"))
                    m.amount = th;
                float sh = m.param;
                if (dai_ui_num_field(p->ui, "Shift", &sh, 0.1f, -1.0f, 1.0f, "modsh"))
                    m.param = sh;
                break;
            }
            case DAI_MOD_ARRAY: {
                float cp = (float)(m.count > 0 ? m.count : 2);
                if (dai_ui_num_field(p->ui, "Copies", &cp, 1.0f, 1.0f, 64.0f, "modcp"))
                    m.count = (int)(cp + 0.5f);
                float off[3] = { m.offset.x, m.offset.y, m.offset.z };
                if (off[0] == 0.0f && off[1] == 0.0f && off[2] == 0.0f) off[0] = 1.0f;
                if (dai_ui_num_vec3(p->ui, "Offset", off, 0.05f))
                    m.offset = dai_vec3{ off[0], off[1], off[2] };
                dai_ui_checkbox(p->ui, "Relative", &m.relative);
                float rot = m.angle;
                if (dai_ui_num_field(p->ui, "Rotation", &rot, 1.0f, -360.0f, 360.0f, "modrot"))
                    m.angle = rot;
                int ax = (m.axis >= 0 && m.axis <= 2) ? m.axis : 1;
                if (dai_ui_option(p->ui, "Axis", &ax, AXES, 3)) m.axis = ax;
                break;
            }
            case DAI_MOD_MIRROR: {
                int ax = (m.axis >= 0 && m.axis <= 2) ? m.axis : 0;
                if (dai_ui_option(p->ui, "Axis", &ax, AXES, 3)) m.axis = ax;
                float wd = m.amount > 0 ? m.amount : 0.001f;
                if (dai_ui_num_field(p->ui, "Weld (m)", &wd, 0.0005f, 0.0f, 1.0f, "modweld"))
                    m.amount = wd;
                break;
            }
            default:
                fit_label("Empty slot - pick a type");
                break;
            }
        }

        if (move_from >= 0 && move_to >= 0 && move_to < r.modifier_count) {
            dai_modifier tmp = r.modifiers[move_from];
            r.modifiers[move_from] = r.modifiers[move_to];
            r.modifiers[move_to] = tmp;
            int f = fold_mods[move_from];
            fold_mods[move_from] = fold_mods[move_to];
            fold_mods[move_to] = f;
        } else if (remove_at >= 0) {
            for (int mi = remove_at; mi + 1 < DAI_MODIFIER_MAX; ++mi)
                r.modifiers[mi] = r.modifiers[mi + 1];
            r.modifiers[DAI_MODIFIER_MAX - 1] = dai_modifier{};
            if (r.modifier_count > 0) --r.modifier_count;
        }
    }
}
