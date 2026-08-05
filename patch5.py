# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'src/dai_editor_ui.cpp'
s = rw(P)

# ---- 1. the label stops carrying the type, an icon carries it -------------
s = sub1(s,
"""const char *node_label(const dai_node_desc &r, dai_node id, char *buf, size_t n) {
    if (r.tag[0] && r.name[0]) {
        // bounded by hand: snprintf's %s %s of two fixed arrays trips the
        // truncation warning even though the total fits
        size_t used = 0;
        for (const char *c = r.tag; *c && used + 1 < n; ++c) buf[used++] = *c;
        if (used + 1 < n) buf[used++] = ' ';
        for (const char *c = r.name; *c && used + 1 < n; ++c) buf[used++] = *c;
        buf[used] = 0;
        return buf;
    }
    if (r.name[0]) return r.name;
    if (r.tag[0])  return r.tag;
    std::snprintf(buf, n, "node %u", (unsigned)id);
    return buf;
}""",
"""// Just the name. The tag used to be glued in front of it ("MainCamera
// Camera"), which reads as part of the name, is not editable there, and costs
// the column its whole job. The tag lives in the inspector, where it is a
// field; what the object IS is now the row's icon.
const char *node_label(const dai_node_desc &r, dai_node id, char *buf, size_t n) {
    if (r.name[0]) return r.name;
    if (r.tag[0])  return r.tag;
    std::snprintf(buf, n, "node %u", (unsigned)id);
    return buf;
}

// The icon that says what a node is, in the order a person would answer the
// question: a camera is a camera even when it also has a script.
const char *node_icon(const dai_node_desc &r) {
    if (r.camera)         return DAI_ICON_CAMERA;
    if (r.light)          return DAI_ICON_LIGHT;
    if (r.audio_event[0]) return DAI_ICON_VOLUME;
    if (r.sprite)         return DAI_ICON_SPRITE;
    if (r.asset[0])       return DAI_ICON_MODEL;
    if (r.no_body && r.no_collider && r.no_rigidbody && !r.asset[0])
        return r.script[0] ? DAI_ICON_SCRIPT : DAI_ICON_EMPTY;
    switch (r.shape) {
    case DAI_SHAPE_SPHERE:  return DAI_ICON_SPHERE;
    case DAI_SHAPE_CAPSULE: return DAI_ICON_CAPSULE;
    default:                return DAI_ICON_CUBE;
    }
}""", "node_label + node_icon")

s = sub1(s,
"""        int rc = dai_ui_tree_item_ex(p->ui, label, depth, kids, kids ? &open : nullptr,
                                     dai_editor_is_selected(p->ed, n));""",
"""        int rc = dai_ui_tree_item_icon(p->ui, node_icon(r), label, depth, kids,
                                       kids ? &open : nullptr,
                                       dai_editor_is_selected(p->ed, n));""", "hierarchy icon row")

# ---- 2. units and meanings on every physics field -------------------------
s = sub1(s,
"""            dai_ui_option(p->ui, "Motion", &r.motion, MOTIONS, 3);
            // No mass field on purpose: mass = density x shape volume.
            // 0 falls back to water (1000) in dai_engine.
            dai_ui_num_field(p->ui, "Density", &r.density, 10.0f, 0.0f, 100000.0f, "density");
            dai_ui_num_field(p->ui, "Friction", &r.friction, 0.005f, 0.0f, 10.0f, "friction");
            dai_ui_num_field(p->ui, "Bounce", &r.restitution, 0.005f, 0.0f, 1.0f, "bounce");""",
"""            dai_ui_option(p->ui, "Motion", &r.motion, MOTIONS, 3);
            dai_ui_help(p->ui, "Dynamic: moved by physics. Kinematic: moved by "
                               "script, pushes others. Static: never moves.");
            // No mass field on purpose: mass = density x shape volume.
            // 0 falls back to water (1000) in dai_engine.
            // Every one of these is a physical quantity with a unit, and a
            // number with no unit is a number you have to guess at: "Friction
            // 10" was read as a percentage more than once.
            dai_ui_num_field(p->ui, "Density", &r.density, 10.0f, 0.0f, 100000.0f, "density");
            dai_ui_help(p->ui, "kg/m3. Mass = density x collider volume. 0 = water (1000). "
                               "Wood 600, concrete 2400, steel 7850.");
            dai_ui_num_field(p->ui, "Friction", &r.friction, 0.005f, 0.0f, 10.0f, "friction");
            dai_ui_help(p->ui, "Coefficient mu, not a percentage. 0 = ice, 0.3 = wet road, "
                               "0.6 = wood, 1.0 = rubber. Combined with the other body's mu.");
            dai_ui_num_field(p->ui, "Bounce", &r.restitution, 0.005f, 0.0f, 1.0f, "bounce");
            dai_ui_help(p->ui, "Restitution 0..1. 0 = stays put, 0.8 = basketball, "
                               "1 = keeps all its energy.");""", "rigidbody help")

wr(P, s)
print("patch5 done")
