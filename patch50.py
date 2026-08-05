#!/usr/bin/env python3
# patch50 - serialized fields with TYPES, and an Add Component menu that only
# adds.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p50'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ===================================== 1. Add Component only adds components
s = rd('src/dai_editor_ui.cpp')
s = sub1(s,
"""            // Attachments flip to "Remove" entries, the way Unity greys out
            // what is already there - but removal keeps the same list shape.
            if (!ar2.no_rigidbody) entries.push_back({ "Remove Rigidbody", "Physics", 10, "" });
            if (!ar2.no_collider)  entries.push_back({ "Remove Collider", "Physics", 11, "" });
            if (ar2.camera)        entries.push_back({ "Remove Camera", "Rendering", 12, "" });
            if (ar2.light)         entries.push_back({ "Remove Light", "Rendering", 13, "" });
            if (ar2.sprite)        entries.push_back({ "Remove Sprite (2D)", "Rendering", 14, "" });
            if (ar2.audio_event[0]) entries.push_back({ "Remove Audio Source", "Audio", 15, "" });""",
"""            // NO "Remove X" entries. A menu called Add Component that offers
            // to remove things is a menu you have to read twice, and the
            // component is already removable where it lives: the header's
            // tick box switches it off, its context menu takes it away. A
            // component that is on the object simply does not appear here -
            // that IS the feedback, and it is the same one Unity gives.
            (void)0;""",
    'add-only component list')

# The remove branches go with them; leaving dead cases in a switch is how the
# next person re-adds the menu entry by accident.
s = sub1(s,
"""                case 10: ar2.no_rigidbody = 1; if (ar2.no_collider) ar2.no_body = 1; break;
                case 11: ar2.no_collider = 1; if (ar2.no_rigidbody) ar2.no_body = 1; break;
                case 12: ar2.camera = 0; break;
                case 13: ar2.light = 0; break;
                case 14: ar2.sprite = 0; break;
                case 15: ar2.audio_event[0] = 0; break;
                }""",
"""                }   /* nothing removes from here any more - see above */""",
    'drop remove cases')

s = sub1(s,
"""                dai_doc_begin(d, sel->kind >= 10 ? "Remove component" : "Add component");""",
"""                dai_doc_begin(d, "Add component");""",
    'undo label')

s = sub1(s,
"""                        if (e.cat == "Physics") ic = DAI_ICON_SETTINGS;
                        else if (e.cat == "Audio") ic = DAI_ICON_AUDIO;
                        else if (e.cat == "Rendering")
                            ic = e.kind == 2 || e.kind == 12 ? DAI_ICON_CAMERA
                               : e.kind == 3 || e.kind == 13 ? DAI_ICON_LIGHT : DAI_ICON_SPRITE;""",
"""                        if (e.cat == "Physics") ic = DAI_ICON_SETTINGS;
                        else if (e.cat == "Audio") ic = DAI_ICON_AUDIO;
                        else if (e.cat == "Rendering")
                            ic = e.kind == 2 ? DAI_ICON_CAMERA
                               : e.kind == 3 ? DAI_ICON_LIGHT : DAI_ICON_SPRITE;""",
    'icon picker without removes')

# ============================================ 2. typed serialized fields
# "// @param speed" gave a label you could only fill by dragging a node onto
# it. Everything else a behaviour is tuned with - a speed, a jump height, a
# name, a switch - had to be a constant in the file, which means editing code
# to balance a game. A type in front of the name is all it takes:
#
#     // @param float speed = 6
#     // @param int   lives = 3
#     // @param bool  canDoubleJump = false
#     // @param string label = Player One
#     // @param node  target
#
# The host reports "type:name=default" per field; everything below draws the
# widget that type deserves and stores the value in the same {k=v} block the
# node references already used, so old scenes keep working untouched.
s = sub1(s,
"""            // "// @param name" lines in the file show what they were given.
            if (p->params_fn) {
                char keys[512] = { 0 };
                p->params_fn(entry_path(slist[si]).c_str(), keys, sizeof(keys), p->params_user);
                std::string k;
                for (const char *c = keys; ; ++c) {
                    if (*c == ',' || !*c) {
                        if (!k.empty()) {
                            std::string val = entry_param(slist[si], k);
                            dai_ui_label_fmt(p->ui, "   %s: %s", k.c_str(),
                                             val.empty() ? "none" : val.c_str());
                            k.clear();
                        }
                        if (!*c) break;
                    } else k += *c;
                }
            }""",
"""            // "// @param [type] name [= default]" lines in the file are the
            // object's serialized fields: a widget each, edited here, stored
            // on the node, handed to the script at Play.
            if (p->params_fn) {
                char keys[1024] = { 0 };
                p->params_fn(entry_path(slist[si]).c_str(), keys, sizeof(keys), p->params_user);
                std::string entry_before = slist[si];
                for (const ParamDecl &pd : parse_params(keys)) {
                    std::string val = entry_param(slist[si], pd.name);
                    if (val.empty()) val = pd.def;      // the file's own default
                    char fid[96];
                    std::snprintf(fid, sizeof(fid), "p%zu_%s", si, pd.name.c_str());
                    if (pd.type == PARAM_FLOAT || pd.type == PARAM_INT) {
                        float fv = (float)std::atof(val.c_str());
                        float was = fv;
                        if (dai_ui_num_field(p->ui, pd.name.c_str(), &fv,
                                             pd.type == PARAM_INT ? 1.0f : 0.1f,
                                             0.0f, 0.0f, fid) || fv != was) {
                            char nb[48];
                            if (pd.type == PARAM_INT) std::snprintf(nb, sizeof(nb), "%d", (int)(fv + (fv < 0 ? -0.5f : 0.5f)));
                            else                      std::snprintf(nb, sizeof(nb), "%g", (double)fv);
                            entry_set_param(slist[si], pd.name, nb);
                        }
                    } else if (pd.type == PARAM_BOOL) {
                        int bv = (val == "true" || val == "1") ? 1 : 0;
                        if (dai_ui_checkbox(p->ui, pd.name.c_str(), &bv))
                            entry_set_param(slist[si], pd.name, bv ? "true" : "false");
                    } else if (pd.type == PARAM_STRING) {
                        char sb[160];
                        std::snprintf(sb, sizeof(sb), "%s", val.c_str());
                        if (dai_ui_input_text(p->ui, pd.name.c_str(), sb, sizeof(sb))) {
                            // ',' '=' '{' '}' and ';' are the separators this
                            // is stored between - a value carrying one would
                            // split the field list in half.
                            std::string clean;
                            for (char c : std::string(sb))
                                clean += (c == ',' || c == '=' || c == '{' ||
                                          c == '}' || c == ';') ? ' ' : c;
                            entry_set_param(slist[si], pd.name, clean);
                        }
                    } else {
                        // A node reference: still a drop target, because the
                        // only sane way to name an object is to point at it.
                        char pl[384];
                        std::snprintf(pl, sizeof(pl), "%s: %s", pd.name.c_str(),
                                      val.empty() ? "none (drag an object here)" : val.c_str());
                        if (dai_ui_button(p->ui, pl) && !val.empty())
                            entry_set_param(slist[si], pd.name, "");   // click clears it
                        const char *hot2 = dai_ui_hot_label(p->ui);
                        if (hot2 && std::strcmp(hot2, pl) == 0) {
                            p->param_hover_entry = (int)si;
                            std::snprintf(p->param_hover_key, sizeof(p->param_hover_key),
                                          "%s", pd.name.c_str());
                        }
                    }
                }
                if (slist[si] != entry_before) script_join(r.script, sizeof(r.script), slist);
            }""",
    'typed param widgets')

# The declaration parser, next to the other script-entry helpers.
s = sub1(s,
"""static void entry_set_param(std::string &e, const std::string &key, const std::string &val) {""",
"""// One "// @param" declaration, as the host reports it: "float:speed=6".
// A missing type means node, which is what every declaration meant before
// types existed - so every script written until now keeps its fields.
enum ParamType { PARAM_NODE = 0, PARAM_FLOAT, PARAM_INT, PARAM_BOOL, PARAM_STRING };
struct ParamDecl { std::string name, def; int type = PARAM_NODE; };

static std::vector<ParamDecl> parse_params(const char *csv) {
    std::vector<ParamDecl> out;
    std::string cur;
    for (const char *c = csv ? csv : ""; ; ++c) {
        if (*c == ',' || !*c) {
            if (!cur.empty()) {
                ParamDecl d;
                std::string rest = cur;
                size_t colon = rest.find(':');
                if (colon != std::string::npos) {
                    std::string t = rest.substr(0, colon);
                    rest = rest.substr(colon + 1);
                    if (t == "float" || t == "number") d.type = PARAM_FLOAT;
                    else if (t == "int")               d.type = PARAM_INT;
                    else if (t == "bool")              d.type = PARAM_BOOL;
                    else if (t == "string" || t == "text") d.type = PARAM_STRING;
                    else                               d.type = PARAM_NODE;
                }
                size_t eq = rest.find('=');
                if (eq != std::string::npos) { d.def = rest.substr(eq + 1); rest = rest.substr(0, eq); }
                d.name = rest;
                if (!d.name.empty()) out.push_back(d);
            }
            cur.clear();
            if (!*c) break;
        } else cur += *c;
    }
    return out;
}

static void entry_set_param(std::string &e, const std::string &key, const std::string &val) {""",
    'param decl parser')

# The "assign to" block that appears while a node is dragged wants the same
# parser, and it must only offer the fields that TAKE a node.
s = sub1(s,
"""                p->params_fn(entry_path(tlist[si]).c_str(), keys, sizeof(keys), p->params_user);
                std::string k;
                for (const char *c = keys; ; ++c) {
                    if (*c == ',' || !*c) {
                        if (!k.empty()) {
                            std::string val = entry_param(tlist[si], k);
                            char pl[384];
                            std::snprintf(pl, sizeof(pl), "%s: %s", k.c_str(),
                                          val.empty() ? "none" : val.c_str());
                            dai_ui_button(p->ui, pl);
                            if (hot && std::strcmp(hot, pl) == 0) {
                                p->param_hover_entry = (int)si;
                                std::snprintf(p->param_hover_key, sizeof(p->param_hover_key), "%s", k.c_str());
                            }
                            k.clear();
                        }
                        if (!*c) break;
                    } else k += *c;
                }""",
"""                p->params_fn(entry_path(tlist[si]).c_str(), keys, sizeof(keys), p->params_user);
                for (const ParamDecl &pd : parse_params(keys)) {
                    if (pd.type != PARAM_NODE) continue;   // a float takes no object
                    std::string val = entry_param(tlist[si], pd.name);
                    char pl[384];
                    std::snprintf(pl, sizeof(pl), "%s: %s", pd.name.c_str(),
                                  val.empty() ? "none" : val.c_str());
                    dai_ui_button(p->ui, pl);
                    if (hot && std::strcmp(hot, pl) == 0) {
                        p->param_hover_entry = (int)si;
                        std::snprintf(p->param_hover_key, sizeof(p->param_hover_key),
                                      "%s", pd.name.c_str());
                    }
                }""",
    'assign block uses parser')

s = sub1(s,
"""                char keys[512] = { 0 };
                p->params_fn(entry_path(tlist[si]).c_str(), keys, sizeof(keys), p->params_user);""",
"""                char keys[1024] = { 0 };
                p->params_fn(entry_path(tlist[si]).c_str(), keys, sizeof(keys), p->params_user);""",
    'assign block buffer')
wr('src/dai_editor_ui.cpp', s)

# ==================================================== 3. the host reports types
s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""// "// @param name" lines declare the drop fields the inspector draws.""",
"""// "// @param [type] name [= default]" lines declare the serialized fields the
// inspector draws. Reported as "type:name=default", comma separated:
//
//     // @param float speed = 6      ->  float:speed=6
//     // @param target               ->  node:target=
//
// The type is optional and defaults to node, so every script written before
// types existed reports exactly what it used to.""",
    'params comment')

s = sub1(s,
"""        const char *p = std::strstr(line, "// @param");
        if (!p) continue;
        p += 9;
        while (*p == ' ') ++p;
        char key[64];
        int ki = 0;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
            if (ki < 63) key[ki++] = *p;
            ++p;
        }
        key[ki] = 0;
        if (!ki) continue;
        size_t kl = std::strlen(key);
        if (used + kl + 2 >= n) break;
        if (used) buf[used++] = ',';
        std::memcpy(buf + used, key, kl); used += kl; buf[used] = 0;
    }
    std::fclose(f);
}""",
"""        const char *p = std::strstr(line, "// @param");
        if (!p) continue;
        p += 9;
        auto word = [&p](char *out, int cap) {
            while (*p == ' ' || *p == '\\t') ++p;
            int i = 0;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
                if (i < cap - 1) out[i++] = *p;
                ++p;
            }
            out[i] = 0;
            return i;
        };
        char first[64], second[64];
        if (!word(first, sizeof(first))) continue;
        const char *type = "node";
        char *key = first;
        // Two words means the first was a type. One word is a node reference,
        // which is what this line meant before types existed.
        if (word(second, sizeof(second))) {
            static const char *TYPES[] = { "float", "number", "int", "bool", "string", "text", "node" };
            for (const char *t : TYPES)
                if (std::strcmp(first, t) == 0) { type = first; break; }
            key = second;
        }
        // "= default", to the end of the line, trimmed. A default is what
        // makes a field usable without touching it - Unity gets this from the
        // field initialiser, and a comment is the only place a .js has one.
        char def[96] = { 0 };
        {
            const char *e = std::strchr(p, '=');
            if (e) {
                ++e;
                while (*e == ' ' || *e == '\\t') ++e;
                int i = 0;
                while (*e && *e != '\\n' && *e != '\\r' && i < (int)sizeof(def) - 1) {
                    // The report is comma separated; a default may not carry one.
                    def[i++] = (*e == ',') ? ' ' : *e;
                    ++e;
                }
                while (i > 0 && (def[i - 1] == ' ' || def[i - 1] == '\\t')) --i;
                def[i] = 0;
            }
        }
        char one[256];
        int wrote = std::snprintf(one, sizeof(one), "%s:%s=%s", type, key, def);
        if (wrote <= 0) continue;
        size_t kl = std::strlen(one);
        if (used + kl + 2 >= n) break;
        if (used) buf[used++] = ',';
        std::memcpy(buf + used, one, kl); used += kl; buf[used] = 0;
    }
    std::fclose(f);
}""",
    'typed params reader')

# ============================ 4. the values reach the script as the right type
s = sub1(s,
"""                    if (eq != std::string::npos && eq > 0)
                        params_js += \"\"\" + kv.substr(0, eq) + \"\":\"\" + kv.substr(eq + 1) + \"\",\";""",
"""                    if (eq != std::string::npos && eq > 0) {
                        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                        // This line used to have its quote escapes eaten:
                        //     params_js += \"\"\" + kv.substr(0, eq) + ...
                        // which C++ happily reads as ONE adjacent-literal
                        // string, so every script got the TEXT
                        // "+ kv.substr(0, eq) +:..." instead of its values,
                        // the eval failed, and `params` was undefined in every
                        // behaviour. Nothing said so - the error from
                        // dai_script_eval was never looked at.
                        //
                        // While it is being written properly: a number stays a
                        // number and a bool stays a bool. Quoting everything
                        // makes params.speed * dt string arithmetic, and in JS
                        // that is a silent NaN.
                        bool numeric = !v.empty();
                        int dots = 0;
                        for (size_t ci = 0; ci < v.size(); ++ci) {
                            char c = v[ci];
                            if (c == '.') { if (++dots > 1) { numeric = false; break; } }
                            else if (c == '-' || c == '+') { if (ci) { numeric = false; break; } }
                            else if (c < '0' || c > '9') { numeric = false; break; }
                        }
                        if (v == "true" || v == "false" || numeric)
                            params_js += "\\"" + k + "\\":" + v + ",";
                        else
                            params_js += "\\"" + k + "\\":\\"" + v + "\\",";
                    }""",
    'typed params to js')
wr('examples/editor_demo.cpp', s)
print('patch50 ok')
