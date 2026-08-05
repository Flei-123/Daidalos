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

s = sub1(s, '    if (e == "js" || e == "ts")                       return DAI_ICON_SCRIPT;',
'''    if (e == "js" || e == "ts")                       return DAI_ICON_SCRIPT;
    if (e == "cpp" || e == "cc" || e == "cxx" || e == "h" || e == "hpp") return DAI_ICON_SCRIPT;''',
"cpp icon")

# One predicate for "this file is a behaviour", used everywhere .js was
# hardcoded. The three places that spelled out strcmp(".js") each had to be
# found by hand every time a language was added; now there is one.
s = sub1(s,
"static bool script_name_ok(const std::string &s) {",
'''// A file the engine can run on an object: QuickJS, or a native C++ behaviour
// (see dai_native.h). Both attach the same way and both are components.
static bool is_behaviour_file(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e == "js" || e == "cpp" || e == "cc" || e == "cxx";
}

static bool script_name_ok(const std::string &s) {''', "is_behaviour_file")

s = s.replace(
"""    size_t plen = pick ? std::strlen(pick) : 0;
    if (plen > 3 && std::strcmp(pick + plen - 3, ".js") == 0) {""",
"""    if (pick && is_behaviour_file(pick)) {""", 1)
print("ok assign filter")

s = s.replace(
"""            size_t plen = pick ? std::strlen(pick) : 0;
            if (plen > 3 && std::strcmp(pick + plen - 3, ".js") == 0) {""",
"""            if (pick && is_behaviour_file(pick)) {""", 1)
print("ok browser filter")

s = s.replace(
"""                    if (fl2 > 3 && full.compare(fl2 - 3, 3, ".js") == 0 && over &&""",
"""                    if (is_behaviour_file(full) && over &&""", 1)
print("ok drag filter")

s = sub1(s,
'''            items[6] = { DAI_ICON_SCRIPT, "Script (drag a .js here)", nullptr };''',
'''            items[6] = { DAI_ICON_SCRIPT, "Script (drag a .js or .cpp here)", nullptr };''',
"addcomp label")

s = sub1(s,
'''            dai_ui_label(p->ui, "no scripts - drag a .js from the Project window onto this object");''',
'''            dai_ui_label(p->ui, "no scripts - drag a .js or .cpp from Project onto this object");''',
"no scripts label")

wr(P, s)

# the create menu learns about C++ files
s = rw(P)
s = sub1(s,
"""    if (ppick == 0 && p->script_create) {""",
"""    // "New C++ Script" is the same flow one entry down: the host writes the
    // file, the row goes straight into rename. The extension is what tells
    // the runner which engine to hand it to.
    if (ppick == 5 && p->script_create) {
        p->proj_tab = 0;
        std::string base5 = p->proj_dir.empty() ? std::string() : p->proj_dir + "/";
        for (int i = 0; i < 20; ++i) {
            char nm[64], rel[224];
            if (i == 0) std::snprintf(nm, sizeof(nm), "NewBehaviour.cpp");
            else        std::snprintf(nm, sizeof(nm), "NewBehaviour%d.cpp", i + 1);
            std::snprintf(rel, sizeof(rel), "%s%s", base5.c_str(), nm);
            if (p->script_create(rel, p->script_user)) {
                p->rename_asset = rel;
                std::string stem = nm;
                stem.resize(stem.size() - 4);
                std::snprintf(p->rename_asset_buf, sizeof(p->rename_asset_buf), "%s", stem.c_str());
                p->rename_seen_active = 0;
                project_expand_to(p, p->proj_dir);
                p->want_refresh = 1;
                break;
            }
        }
    }
    if (ppick == 0 && p->script_create) {""", "new cpp script")
wr(P, s)
print("patch12 done")
