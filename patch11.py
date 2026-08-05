# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# dai_entity already exists in daidalos.h - a behaviour header that redefines
# it cannot be included next to the engine.
P = 'include/dai_native.h'
s = rw(P)
s = s.replace("dai_entity", "dai_nentity")
s = s.replace("""/* The object the behaviour is attached to. Opaque on purpose - it is the
 * editor's node id, and a behaviour that does arithmetic on it is a behaviour
 * that breaks when node ids change. */
typedef uint32_t dai_nentity;""",
"""/* The object the behaviour is attached to. Opaque on purpose - it is the
 * editor's node id, and a behaviour that does arithmetic on it is a behaviour
 * that breaks when node ids change.
 *
 * Named dai_nentity and not dai_entity because the engine already has a
 * dai_entity meaning something else (a scene entity, not a document node), and
 * the host includes both headers. */
typedef uint32_t dai_nentity;""")
wr(P, s)
print("ok native entity rename")

# live velocity / impulse, so a behaviour can do more than teleport
P = 'include/dai_editor.h'
s = rw(P)
s = sub1(s,
"DAI_API void dai_editor_live_set_transform(dai_editor *e, dai_node n,",
"""/* The body's velocity while playing. 0 when the node has no body (or the
 * editor is not playing), and the out parameters are left alone - a script
 * that reads the velocity of a static wall gets zero, not garbage. */
DAI_API int  dai_editor_live_velocity(const dai_editor *e, dai_node n,
                                      dai_vec3 *linear, dai_vec3 *angular);
DAI_API void dai_editor_live_set_velocity(dai_editor *e, dai_node n, dai_vec3 linear);
DAI_API void dai_editor_live_impulse(dai_editor *e, dai_node n, dai_vec3 impulse);

DAI_API void dai_editor_live_set_transform(dai_editor *e, dai_node n,""", "live vel decl")
wr(P, s)

P = 'src/dai_editor.cpp'
s = rw(P)
s = sub1(s,
"void dai_editor_live_set_transform(dai_editor *e, dai_node n,",
"""int dai_editor_live_velocity(const dai_editor *e, dai_node n,
                             dai_vec3 *linear, dai_vec3 *angular) {
    if (!e || !e->sync || e->state == DAI_EDITOR_EDIT) return 0;
    dai_entity ent = dai_doc_sync_entity(e->sync, n);
    dai_scene *sc = dai_doc_sync_scene(e->sync);
    if (!ent || !sc) return 0;
    dai_body b = dai_scene_body(sc, ent);
    if (!b) return 0;
    return dai_body_get_velocity(editor_world(const_cast<dai_editor *>(e)), b,
                                 linear, angular) == DAI_OK ? 1 : 0;
}

void dai_editor_live_set_velocity(dai_editor *e, dai_node n, dai_vec3 linear) {
    if (!e || !e->sync || e->state == DAI_EDITOR_EDIT) return;
    dai_entity ent = dai_doc_sync_entity(e->sync, n);
    dai_scene *sc = dai_doc_sync_scene(e->sync);
    if (!ent || !sc) return;
    dai_body b = dai_scene_body(sc, ent);
    if (!b) return;
    dai_vec3 ang{ 0, 0, 0 };
    dai_body_get_velocity(editor_world(e), b, nullptr, &ang);
    dai_body_set_velocity(editor_world(e), b, linear, ang);
}

void dai_editor_live_impulse(dai_editor *e, dai_node n, dai_vec3 impulse) {
    if (!e || !e->sync || e->state == DAI_EDITOR_EDIT) return;
    dai_entity ent = dai_doc_sync_entity(e->sync, n);
    dai_scene *sc = dai_doc_sync_scene(e->sync);
    if (!ent || !sc) return;
    dai_body b = dai_scene_body(sc, ent);
    if (b) dai_body_add_impulse(editor_world(e), b, impulse);
}

void dai_editor_live_set_transform(dai_editor *e, dai_node n,""", "live vel impl")
wr(P, s)

# editor_demo: rename in the patch10 additions, run the behaviours per frame
P = 'examples/editor_demo.cpp'
s = rw(P)
s = s.replace("dai_entity e)", "dai_nentity e)")
s = s.replace("dai_entity nv_find", "dai_nentity nv_find")
s = s.replace("(dai_entity)0", "(dai_nentity)0")
s = s.replace("(dai_entity)(uint32_t)id", "(dai_nentity)(uint32_t)id")
s = s.replace("return id < 0 ? (dai_nentity)0 : (dai_nentity)(uint32_t)id;",
              "return id < 0 ? (dai_nentity)0 : (dai_nentity)(uint32_t)id;")

s = sub1(s,
"""            if (g_scripts_live)
                for (RunningScript &rs : g_running) {""",
"""            if (g_scripts_live && g_native) {
                g_native_time += 1.0f / 60.0f;
                for (RunningNative &rn : g_natives)
                    dai_native_frame(g_native, rn.id, &g_native_api,
                                     (dai_nentity)(uint32_t)rn.node, 1.0f / 60.0f);
            }
            if (g_scripts_live)
                for (RunningScript &rs : g_running) {""", "native frame")

s = sub1(s,
'#include "dai_script.h"',
'#include "dai_script.h"\n#include "dai_native.h"', "native include")
wr(P, s)
print("patch11 done")
