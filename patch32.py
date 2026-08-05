#!/usr/bin/env python3
# patch32 - double click in the project browser, and the physics probe fix.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p32'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    icon_for_asset(full), label.c_str(), selected) && clicks_ok)
                        p->asset_sel = fi;""",
"""                    if (browser_row(p, list_x + 2.0f, ry, list_w - 4.0f, ROW,
                                    icon_for_asset(full), label.c_str(), selected) && clicks_ok) {
                        // Double click is what everyone tries first: a script
                        // opens in the external editor, a prefab drops into
                        // the scene, a model is placed. A browser where the
                        // only way in is a button at the bottom is a browser
                        // people call broken.
                        if (dai_ui_double_click(ui)) {
                            if (is_behaviour_file(full) || is_text_file(full)) {
                                if (p->open_asset)
                                    p->open_asset(nullptr, full.c_str(), p->open_asset_user);
                            } else {
                                p->pending_asset = p->assets[(size_t)fi];
                                p->pending_as_tree = 0;
                            }
                        }
                        p->asset_sel = fi;
                    }""",
'browser double click')

s = sub1(s, "static bool is_behaviour_file(const std::string &path) {",
"""// Anything an external editor can open, as opposed to something the scene
// places. A .cpp is both a behaviour and text; the check above wins.
static bool is_text_file(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return e == "txt" || e == "md" || e == "json" || e == "h" || e == "hpp" ||
           e == "glsl";
}

static bool is_behaviour_file(const std::string &path) {""",
'is_text_file')
wr('src/dai_editor_ui.cpp', s)
print('patch32 ok')
