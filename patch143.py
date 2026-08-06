#!/usr/bin/env python3
# Runde 28d - zwei Meldungen vom Boss:
#  1. "warum hat der capsule collider die hitbox eines cylinder" -> hat er
#     nicht, aber GEZEICHNET wurde einer: zwei Kreise und vier senkrechte
#     Linien sind ein Zylinder. Die runden Kappen fehlten schlicht.
#  2. "ich aendere nur den collider, das mesh bleibt Capsule - warum aendert
#     sich trotzdem das Mesh in der Szene" -> weil die GROESSE und der
#     Kapsel-Parameter des Meshes aus der COLLIDER-Form abgeleitet wurden.
#     Ein Capsule-Mesh ueber einem Sphere-Collider bekam param = 0 und wurde
#     damit zur Kugel. Das Mesh-Feld log nicht, die Groesse hing am falschen
#     Ding.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

# ============================================== 1. die Kapsel-Drahtform
p = os.path.join(ROOT, "src", "dai_editor_ui.cpp")
t = io.open(p, encoding="utf-8").read()

t = sub(t, """// Unity's collider green. Not "a green": this exact one, because the point of
// the colour is that it is instantly recognisable as "collision, not model".""",
"""// Part of a circle. The capsule needs it: its caps are half circles, and
// drawing them as whole ones puts a ring through the middle of each dome.
void wire_arc(dai_editor_ui *p, dai_vec3 c, dai_quat rot, dai_vec3 ax, dai_vec3 ay,
              float r, float t0, float t1, uint32_t col, float thick) {
    const int N = 16;
    dai_vec3 prev{};
    for (int i = 0; i <= N; ++i) {
        float t = t0 + (t1 - t0) * (float)i / (float)N;
        dai_vec3 l = v_add(v_mul(ax, std::cos(t) * r), v_mul(ay, std::sin(t) * r));
        dai_vec3 wpt = v_add(c, qrot_v(rot, l));
        if (i) wire_line(p, prev, wpt, col, thick);
        prev = wpt;
    }
}

// Unity's collider green. Not "a green": this exact one, because the point of
// the colour is that it is instantly recognisable as "collision, not model".""",
    "wire_arc")

OLD = """        for (int i = 0; i < 4; ++i) {
            float sx = (i == 0) ? rad : (i == 1) ? -rad : 0.0f;
            float sz = (i == 2) ? rad : (i == 3) ? -rad : 0.0f;
            wire_line(p, v_add(top, qrot_v(c.rot, dai_vec3{ sx, 0, sz })),
                         v_add(bot, qrot_v(c.rot, dai_vec3{ sx, 0, sz })), col, thick);"""
NEW = """        // THE CAPS. Without these four arcs the outline is two circles joined
        // by four straight lines - which is a cylinder, drawn around a shape
        // that collides like a capsule. The wire said one thing and the
        // physics did another, and the wire is what you look at.
        const float PI = 3.14159265f;
        wire_arc(p, top, c.rot, dai_vec3{1,0,0}, dai_vec3{0,1,0},  rad, 0.0f, PI, col, thick);
        wire_arc(p, top, c.rot, dai_vec3{0,0,1}, dai_vec3{0,1,0},  rad, 0.0f, PI, col, thick);
        wire_arc(p, bot, c.rot, dai_vec3{1,0,0}, dai_vec3{0,-1,0}, rad, 0.0f, PI, col, thick);
        wire_arc(p, bot, c.rot, dai_vec3{0,0,1}, dai_vec3{0,-1,0}, rad, 0.0f, PI, col, thick);
        for (int i = 0; i < 4; ++i) {
            float sx = (i == 0) ? rad : (i == 1) ? -rad : 0.0f;
            float sz = (i == 2) ? rad : (i == 3) ? -rad : 0.0f;
            wire_line(p, v_add(top, qrot_v(c.rot, dai_vec3{ sx, 0, sz })),
                         v_add(bot, qrot_v(c.rot, dai_vec3{ sx, 0, sz })), col, thick);"""
t = sub(t, OLD, NEW, "Kapsel-Drahtform")
shutil.copyfile(p, p + ".bak_p143")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_editor_ui.cpp: die Kapsel hat wieder Kappen")

# ============================================== 2. Mesh haengt am Mesh
p = os.path.join(ROOT, "src", "dai_scene.cpp")
t = io.open(p, encoding="utf-8").read()

t = sub(t, "} // namespace\n\nstruct dai_scene {",
"""// Which sizing rules a BUILTIN mesh follows. A Capsule mesh is sized like a
// capsule whatever the collider under it happens to be.
//
// This is the whole of "I only changed the collider and the model changed
// shape": the capsule mesh's `param` is its shaft length, and it was read out
// of the COLLIDER. Switch the collider to a sphere and param went to 0 - the
// capsule mesh collapsed into a ball while the Mesh field still said Capsule,
// and the field was telling the truth.
int shape_of_mesh(uint32_t mesh, int fallback) {
    switch (mesh) {
    case DAI_MESH_SPHERE:   return DAI_SHAPE_SPHERE;
    case DAI_MESH_CAPSULE:  return DAI_SHAPE_CAPSULE;
    case DAI_MESH_CYLINDER: return DAI_SHAPE_CYLINDER;
    case DAI_MESH_BOX:      return DAI_SHAPE_BOX;
    default:                return fallback;   /* a loaded model: no rule here */
    }
}

} // namespace

struct dai_scene {""", "shape_of_mesh")

t = sub(t, """    uint32_t mesh; dai_vec3 scale; float param;
    shape_to_mesh(desc->body.shape, desc->body.half_extent, &mesh, &scale, &param);
    r.mesh = (desc->mesh == 0xFFFFFFFFu) ? mesh : desc->mesh;""",
"""    uint32_t mesh; dai_vec3 scale; float param;
    shape_to_mesh(desc->body.shape, desc->body.half_extent, &mesh, &scale, &param);
    r.mesh = (desc->mesh == 0xFFFFFFFFu) ? mesh : desc->mesh;
    // A mesh that was CHOSEN is sized by its own rules, not by the collider's.
    // Only when the mesh is left on "from shape" do the two follow each other,
    // which is what "from shape" means.
    if (desc->mesh != 0xFFFFFFFFu && r.mesh != mesh) {
        uint32_t m2; dai_vec3 s2; float p2;
        shape_to_mesh(shape_of_mesh(r.mesh, desc->body.shape),
                      desc->body.half_extent, &m2, &s2, &p2);
        scale = s2;
        param = p2;
    }""", "dai_scene_attach")
shutil.copyfile(p, p + ".bak_p143")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_scene.cpp: das Mesh wird nach SEINER Form bemasst")

# ---- und dasselbe eine Ebene hoeher, in der Sync-Schicht ---------------
p = os.path.join(ROOT, "src", "dai_doc_sync.cpp")
t = io.open(p, encoding="utf-8").read()
t = sub(t, """dai_vec3 render_scale_of(const dai_node_desc &r, dai_vec3 ws) {
    dai_vec3 he = scaled_he(r.shape, r.render_extent, ws);
    switch (r.shape) {""",
"""// Which shape's sizing rules the DRAWN mesh follows. See shape_of_mesh in
// dai_scene.cpp - the same rule, and it has to be the same one, or the scale
// and the mesh parameter disagree about what they are describing.
int drawn_shape_of(const dai_node_desc &r) {
    switch (r.mesh) {
    case DAI_MESH_SPHERE:   return DAI_SHAPE_SPHERE;
    case DAI_MESH_CAPSULE:  return DAI_SHAPE_CAPSULE;
    case DAI_MESH_CYLINDER: return DAI_SHAPE_CYLINDER;
    case DAI_MESH_BOX:      return DAI_SHAPE_BOX;
    default:                return r.shape;
    }
}

dai_vec3 render_scale_of(const dai_node_desc &r, dai_vec3 ws) {
    int ds = drawn_shape_of(r);
    dai_vec3 he = scaled_he(ds, r.render_extent, ws);
    switch (ds) {""", "render_scale_of")
shutil.copyfile(p, p + ".bak_p143")
io.open(p, "w", encoding="utf-8").write(t)
print("-- dai_doc_sync.cpp: Render-Skalierung folgt dem Mesh")
print("OK")
