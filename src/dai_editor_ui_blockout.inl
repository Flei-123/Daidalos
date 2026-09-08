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

// Where a socket's numbers put it: the position is the middle of the opening's
// SILL - a door stands on the floor, and a socket measured from the middle of
// the air is a socket nobody can place by hand. The normal is which way you
// walk through it.
static void blockout_socket_frame(const dai_node_desc &r, dai_vec3 wp, dai_quat wr,
                                  dai_vec3 ws, dai_vec3 *centre, dai_vec3 *right,
                                  dai_vec3 *up, dai_vec3 *fwd, float *w, float *h) {
    dai_vec3 off{ r.door_offset.x * ws.x, r.door_offset.y * ws.y, r.door_offset.z * ws.z };
    *centre = v_add(wp, qrot_v(wr, off));

    dai_vec3 nrm = r.door_normal;
    if (nrm.x == 0.0f && nrm.y == 0.0f && nrm.z == 0.0f) nrm = dai_vec3{ 0, 0, 1 };
    dai_vec3 f = qrot_v(wr, nrm);
    float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    f = fl > 1e-6f ? v_mul(f, 1.0f / fl) : dai_vec3{ 0, 0, 1 };

    // World up, unless the socket looks straight up or down - a hatch in a
    // ceiling is still a socket, and a zero length cross product would draw it
    // as a dot.
    dai_vec3 u{ 0, 1, 0 };
    if (std::fabs(f.y) > 0.99f) u = dai_vec3{ 0, 0, 1 };
    dai_vec3 rt{ u.y * f.z - u.z * f.y, u.z * f.x - u.x * f.z, u.x * f.y - u.y * f.x };
    float rl = std::sqrt(rt.x * rt.x + rt.y * rt.y + rt.z * rt.z);
    rt = rl > 1e-6f ? v_mul(rt, 1.0f / rl) : dai_vec3{ 1, 0, 0 };
    u = dai_vec3{ f.y * rt.z - f.z * rt.y, f.z * rt.x - f.x * rt.z, f.x * rt.y - f.y * rt.x };

    *right = rt;
    *up = u;
    *fwd = f;
    *w = (r.door_width > 0 ? r.door_width : 0.9f);
    *h = (r.door_height > 0 ? r.door_height : 2.05f);
}

// A socket with a size IS a socket, even when nobody ticked the box: a script
// or the bridge that sets door.width and door.height has said what it means,
// and a gizmo that stays invisible until a second, hidden field is also set is
// a gizmo that reads as broken.
static bool blockout_has_socket(const dai_node_desc &r) {
    return r.door_socket || r.door_width > 0.0f || r.door_height > 0.0f;
}

static void dai_blockout_draw_sockets(dai_editor_ui *p) {
    if (!p || p->view != DAI_VIEW_SCENE) return;
    dai_doc *d = dai_editor_doc(p->ed);
    uint32_t count = dai_doc_count(d);
    if (!count) return;
    std::vector<dai_node> ids(count);
    dai_doc_nodes(d, ids.data(), count);

    for (dai_node id : ids) {
        dai_node_desc r{};
        if (dai_doc_get(d, id, &r) != DAI_OK) continue;
        if (!blockout_has_socket(r) || r.disabled) continue;

        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        if (!dai_editor_live_transform(p->ed, id, &wp, &wr, &ws))
            dai_doc_world_transform(d, id, &wp, &wr, &ws);

        dai_vec3 c{}, rt{}, up{}, f{};
        float w = 0, h = 0;
        blockout_socket_frame(r, wp, wr, ws, &c, &rt, &up, &f, &w, &h);

        // The selected node's socket is drawn bright, everything else dim -
        // the rule the collider wireframes follow, and the reason a scene full
        // of doorways still reads.
        bool sel = dai_editor_is_selected(p->ed, id) != 0;
        uint32_t col = sel ? 0xFF6FD8FFu : 0xB040A0C0u;   // 0xAABBGGRR
        float thick = sel ? 2.0f : 1.5f;

        dai_vec3 hr = v_mul(rt, w * 0.5f);
        dai_vec3 a = v_add(c, hr);                          // sill, right
        dai_vec3 b = v_add(c, v_mul(hr, -1.0f));            // sill, left
        dai_vec3 a2 = v_add(a, v_mul(up, h));               // head, right
        dai_vec3 b2 = v_add(b, v_mul(up, h));               // head, left
        wire_line(p, a, b, col, thick);
        wire_line(p, a, a2, col, thick);
        wire_line(p, b, b2, col, thick);
        wire_line(p, a2, b2, col, thick);

        // The arrow: which way the door faces, read at a glance instead of by
        // reading three numbers in the inspector.
        dai_vec3 mid = v_add(c, v_mul(up, h * 0.5f));
        float len = w * 0.6f;
        dai_vec3 tip = v_add(mid, v_mul(f, len));
        wire_line(p, mid, tip, col, thick);
        wire_line(p, tip, v_add(v_add(mid, v_mul(f, len * 0.6f)), v_mul(rt, len * 0.22f)),
                  col, thick);
        wire_line(p, tip, v_add(v_add(mid, v_mul(f, len * 0.6f)), v_mul(rt, -len * 0.22f)),
                  col, thick);
        // A cross on the sill point itself, so "position" is a place you can
        // see and not a value you have to trust.
        wire_line(p, v_add(c, v_mul(rt, -0.08f)), v_add(c, v_mul(rt, 0.08f)), col, thick);
        wire_line(p, v_add(c, v_mul(up, -0.08f)), v_add(c, v_mul(up, 0.08f)), col, thick);
    }
}
