#!/usr/bin/env python3
# patch48b - declaration order: open_project_path runs before the scene and
# file helpers are defined, so it needs to have heard of them.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p48b'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('examples/editor_demo.cpp')

# The console pointer is read by the scene migration, which happens on project
# open - long before the console section of this file. It is one pointer; it
# belongs with the other globals.
s = sub1(s,
"""static dai_project *g_project = nullptr;
static char g_assets_dir[512] = { 0 };""",
"""static dai_project *g_project = nullptr;
static char g_assets_dir[512] = { 0 };

// The console panel, so anything in this file can report into the editor
// instead of only onto a stdout nobody is looking at.
static dai_editor_ui *g_panels_for_log = nullptr;

// Defined further down, used by open_project_path right below: where scenes
// live, how old projects get theirs moved, and the two file questions the
// migration asks.
static std::string scene_dir();
static void        migrate_scenes_into_assets();
static const char *scene_list(uint32_t index, void *user);
static int         path_exists(const char *p);
static int         copy_one_file(const char *src, const char *dst);""",
    'forward decls')

s = sub1(s,
"""// The console panel, reachable from the script runner below: a script error
// belongs where the user is looking, not only on stdout.
static dai_editor_ui *g_panels_for_log = nullptr;""",
"""// (g_panels_for_log is declared with the other globals at the top - the scene
// migration reports through it, and that runs before this line.)""",
    'drop duplicate g_panels_for_log')
wr('examples/editor_demo.cpp', s)
print('patch48b ok')
