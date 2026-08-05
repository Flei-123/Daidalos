import io

p = 'src/dai_editor.cpp'
s = io.open(p, encoding='utf-8').read()

# ---------------------------------------------------------------------------
# Ein Knoten OHNE Rigidbody war waehrend Play unbeweglich - fuer Scripts wie
# fuer alles andere. Genau das trifft eine Kamera.
# ---------------------------------------------------------------------------
old = """    int   cam_mode = 0;                // 0 none, 1 look, 2 pan, 3 orbit, 4 dolly"""
new = """    // Where a node WITHOUT a physics body sits while the game runs.
    //
    // During play the truth about a moving object is its body - that is why
    // Stop can restore the scene exactly. But a camera, an empty, a marker:
    // nothing simulates them, so both live_transform and live_set_transform
    // simply gave up on them. A script calling node.setPos on the camera it
    // was handed did nothing at all, silently, and "the camera does not
    // follow the player" had no error to go with it.
    //
    // They get a pose of their own, held here for the length of one play
    // session: written through to the scene so the frame draws it, read back
    // so the script sees what it just set, and dropped on Play and Stop so
    // nothing leaks into the next run.
    struct LivePose { dai_vec3 pos; dai_quat rot; };
    std::unordered_map<uint32_t, LivePose> live_pose;

    int   cam_mode = 0;                // 0 none, 1 look, 2 pan, 3 orbit, 4 dolly"""
assert s.count(old) == 1, 'cam_mode member not found'
s = s.replace(old, new)

# --- Getter ---------------------------------------------------------------
old = """    dai_body b = dai_scene_body(sc, ent);
    if (!b) return 0;       // render-only node: nothing simulates it, the doc pose stands
    dai_transform t{};
    if (dai_body_get(editor_world(e), b, &t) != DAI_OK) return 0;"""
new = """    dai_body b = dai_scene_body(sc, ent);
    if (!b) {
        // Render-only node. If something moved it during this run, that is
        // where it is; otherwise the document pose still stands.
        auto it = e->live_pose.find((uint32_t)n);
        if (it == e->live_pose.end()) return 0;
        if (pos) *pos = it->second.pos;
        if (rot) *rot = it->second.rot;
        return 1;
    }
    dai_transform t{};
    if (dai_body_get(editor_world(e), b, &t) != DAI_OK) return 0;"""
assert s.count(old) == 1, 'live_transform body check not found'
s = s.replace(old, new)

# --- Setter, beide Stellen -------------------------------------------------
old = """    dai_body b = dai_scene_body(sc, ent);
    if (b == DAI_INVALID_BODY) return;      // render-only node: nothing to move"""
new = """    dai_body b = dai_scene_body(sc, ent);
    if (b == DAI_INVALID_BODY) {
        // No body: the scene entity carries the pose, and we remember it so
        // the next read gives back what was just written.
        dai_scene_set_transform(sc, ent, pos, rot);
        e->live_pose[(uint32_t)n] = dai_editor::LivePose{ pos, rot };
        return;
    }"""
assert s.count(old) == 1, 'set_live_transform body check not found'
s = s.replace(old, new)

old = """    dai_body b = dai_scene_body(sc, ent);
    if (!b) return;
    dai_transform t{};
    if (dai_body_get(editor_world(e), b, &t) != DAI_OK) return;
    if (rot) t.rotation = *rot;"""
new = """    dai_body b = dai_scene_body(sc, ent);
    if (!b) {
        // Same rule as set_live_transform: a bodiless node still moves.
        // Partial writes have to keep the other half of the pose, so start
        // from where the node currently is.
        dai_vec3 cp{}; dai_quat cr{ 0, 0, 0, 1 };
        auto it = e->live_pose.find((uint32_t)n);
        if (it != e->live_pose.end()) { cp = it->second.pos; cr = it->second.rot; }
        else dai_doc_world_transform(e->doc, n, &cp, &cr, nullptr);
        if (pos) cp = *pos;
        if (rot) cr = *rot;
        dai_scene_set_transform(sc, ent, cp, cr);
        e->live_pose[(uint32_t)n] = dai_editor::LivePose{ cp, cr };
        return;
    }
    dai_transform t{};
    if (dai_body_get(editor_world(e), b, &t) != DAI_OK) return;
    if (rot) t.rotation = *rot;"""
assert s.count(old) == 1, 'dai_editor_live_set_transform body check not found'
s = s.replace(old, new)

# --- Play und Stop raeumen auf --------------------------------------------
old = """        e->play_start_tick = dai_current_tick(w) + 1;
    }
    e->state = DAI_EDITOR_PLAY;"""
new = """        e->play_start_tick = dai_current_tick(w) + 1;
        e->live_pose.clear();          // a new run starts where the doc says
    }
    e->state = DAI_EDITOR_PLAY;"""
assert s.count(old) == 1, 'play start not found'
s = s.replace(old, new)

old = """    if (!e || e->state == DAI_EDITOR_EDIT) return;
    e->state = DAI_EDITOR_EDIT;
    if (e->dragging) dai_editor_drag_cancel(e);"""
new = """    if (!e || e->state == DAI_EDITOR_EDIT) return;
    e->state = DAI_EDITOR_EDIT;
    e->live_pose.clear();              // Stop puts everything back, this too
    if (e->dragging) dai_editor_drag_cancel(e);"""
assert s.count(old) == 1, 'stop not found'
s = s.replace(old, new)

if '#include <unordered_map>' not in s:
    s = s.replace('#include <vector>', '#include <vector>\n#include <unordered_map>', 1)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_editor.cpp: a node without a body can move during play')
