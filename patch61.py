#!/usr/bin/env python3
# patch61 - survive a fault instead of dying of it, stop wrapping prefabs, save
# them at their own origin, and let a child be dragged out of its parent.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p61'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ================================================ 1. a fault is not the end
# GCC on Windows has no __try/__except - that is an MSVC extension and mingw
# never implemented it, in C or in C++. What it does have is the same thing
# every long-running program has used since before SEH existed: a jump target
# taken before the risky work, and a handler that jumps back to it.
#
# So: setjmp at the top of the frame, and the exception filter longjmps into
# it instead of writing a tombstone. The frame is abandoned - whatever it was
# drawing is gone, and with -fno-exceptions nothing is unwound, so that frame's
# temporaries leak - and the editor keeps running with the scene still in
# memory and the console saying what happened. A leaked frame is a rounding
# error; a lost afternoon is not.
#
# It is a net, not a licence: the same fault repeating means the state is
# genuinely broken, so after a handful of consecutive catches it writes the
# report and stops for real.
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""static char g_crash_path[600] = { 0 };""",
"""static char g_crash_path[600] = { 0 };

// The recovery point, armed only around the work we are willing to abandon.
#include <csetjmp>
static jmp_buf  g_guard_jmp;
static int      g_guard_armed = 0;
static int      g_guard_faults = 0;        // consecutive, reset by a clean frame
static unsigned long g_guard_last_code = 0;
static void    *g_guard_last_addr = nullptr;

// Runs fn under the net. 1 = it returned normally, 0 = it faulted and was
// abandoned. Never call anything from fn that must not be half-done: this is
// for a UI frame, not for a file write.
static int guard_run(void (*fn)(void *), void *user) {
    if (setjmp(g_guard_jmp) != 0) {
        g_guard_armed = 0;
        return 0;                          // came back through the handler
    }
    g_guard_armed = 1;
    fn(user);
    g_guard_armed = 0;
    return 1;
}""",
    'guard scaffolding')

s = sub1(s,
"""#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    void *frames[40];
    USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);""",
"""#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    // Armed and not yet hopeless: abandon the frame and carry on. The report
    // is not written for these - the console line is, every time, and that is
    // what turns "it crashed again" into something with a number in it.
    if (g_guard_armed && g_guard_faults < 8) {
        g_guard_last_code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
        g_guard_last_addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        ++g_guard_faults;
        longjmp(g_guard_jmp, 1);
    }
    void *frames[40];
    USHORT n = CaptureStackBackTrace(0, 40, frames, nullptr);""",
    'filter recovers')

s = sub1(s,
"""static void crash_signal(int sig) {
    void *frames[40];
    int n = 0;""",
"""static void crash_signal(int sig) {
    if (g_guard_armed && g_guard_faults < 8) {
        g_guard_last_code = (unsigned long)sig;
        g_guard_last_addr = nullptr;
        ++g_guard_faults;
        longjmp(g_guard_jmp, 1);
    }
    void *frames[40];
    int n = 0;""",
    'signal recovers')
wr('examples/editor_demo.cpp', s)

# ================================================ 2. a prefab has no wrapper
s = rd('src/dai_doc_text.cpp')
s = sub1(s,
"""    // The instance root is a transform node that points at the file. It gets
    // the prefab root's own transform so the instance lands where the original
    // was authored.
    dai_node_desc root = dai_node_desc_default();
    if (!sids.empty()) dai_doc_get(sub, sids[0], &root);
    root.parent = parent;
    root.no_body = 1;                 // the pieces carry the physics
    root.mesh = 0xFFFFFFFFu;
    root.asset[0] = 0;
    snprintf(root.prefab, sizeof(root.prefab), "%s", path);

    dai_doc_begin(d, "Instantiate prefab");
    dai_node made = dai_doc_add(d, &root);
    if (made) graft(d, sub, made);
    dai_doc_commit(d);
    dai_doc_destroy(sub);
    return made;""",
"""    // The prefab's OWN root is the instance root - never an extra node above
    // it. The wrapper was convenient (somewhere to hang the children and the
    // file reference) and wrong: instantiate a prefab twice, or make a prefab
    // of an instance, and you get a parent inside a parent inside a parent,
    // each one empty, each one the thing your click actually selects.
    //
    // The root keeps its mesh, its collider and its transform; it just also
    // carries the prefab path. Its children come from the file underneath it.
    dai_node_desc root = dai_node_desc_default();
    if (!sids.empty()) dai_doc_get(sub, sids[0], &root);
    root.parent = parent;
    snprintf(root.prefab, sizeof(root.prefab), "%s", path);

    dai_doc_begin(d, "Instantiate prefab");
    dai_node made = dai_doc_add(d, &root);
    // Everything except the root, re-parented under the new instance.
    if (made) graft_children(d, sub, sids.empty() ? 0 : sids[0], made);
    dai_doc_commit(d);
    dai_doc_destroy(sub);
    return made;""",
    'no wrapper node')

# graft() copies the whole sub document under a node; what is needed now is
# "everything except the root", because the root IS the node.
s = sub1(s,
"""// A prefab is just a scene file, and an instance is a node that points at one.""",
"""// Copies every node of `sub` EXCEPT `skip_root` under `into`, keeping the
// shape: a child of the skipped root becomes a child of `into`.
static void graft_children(dai_doc *d, const dai_doc *sub, dai_node skip_root, dai_node into) {
    if (!d || !sub || !into) return;
    std::vector<dai_node> ids((size_t)dai_doc_count(sub));
    if (ids.empty()) return;
    dai_doc_nodes(sub, ids.data(), (uint32_t)ids.size());
    std::unordered_map<dai_node, dai_node> map;
    map[skip_root] = into;              // the root maps onto the instance itself
    for (dai_node id : ids) {
        if (id == skip_root) continue;
        dai_node_desc rec{};
        if (dai_doc_get(sub, id, &rec) != DAI_OK) continue;
        auto it = map.find(rec.parent);
        rec.parent = it == map.end() ? into : it->second;
        rec.prefab[0] = 0;              // only the instance root points at the file
        dai_node made = dai_doc_add(d, &rec);
        if (made) map[id] = made;
    }
}

// A prefab is just a scene file, and an instance is a node that points at one.""",
    'graft_children')

# ==================================== 3. a prefab is authored at its own origin
s = sub1(s,
"""        if (id == n) {
            rec.parent = 0;
            rec.prefab[0] = 0;     // the original is not an instance of itself
        } else {""",
"""        if (id == n) {
            rec.parent = 0;
            rec.prefab[0] = 0;     // the original is not an instance of itself
            // At its OWN origin, never where it happened to be standing when
            // it was made. A prefab that carries the world position of the
            // crate you dragged it from drops every future instance in that
            // one spot, and the children - which are stored relative to this
            // root - are the only things that were ever meant to be offsets.
            rec.position = dai_vec3{ 0, 0, 0 };
        } else {""",
    'prefab root at origin')
wr('src/dai_doc_text.cpp', s)

# =========================== 4. dragging a child out of a parent releases it
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""                reparent_node(p, p->drag_node,
                              p->hover_node == DAI_SCENE_ROOT_NODE ? DAI_INVALID_NODE
                                                                   : p->hover_node);""",
"""                reparent_node(p, p->drag_node,
                              p->hover_node == DAI_SCENE_ROOT_NODE ? DAI_INVALID_NODE
                                                                   : p->hover_node);
            } else if (p->drag_node != DAI_INVALID_NODE &&
                       p->hover_node == DAI_INVALID_NODE &&
                       dai_ui_root_hovered(ui, "Hierarchy")) {
                // Let go over the empty part of the hierarchy: the object
                // leaves its parent and becomes a root. Without this there is
                // no gesture for "out" at all - only "into something else" -
                // and a child that was dragged into a group by mistake can
                // never be dragged back out of it.
                reparent_node(p, p->drag_node, DAI_INVALID_NODE);""",
    'unparent on empty drop')
wr('src/dai_editor_ui.cpp', s)
print('patch61 ok')
