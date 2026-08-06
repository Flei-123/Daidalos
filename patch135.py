#!/usr/bin/env python3
# Runde 27 - drei Meldungen vom Boss:
#   1. Constraints ist ein eigener Block im Inspector -> gehoert UNTER Rigidbody.
#   2. Autocomplete kennt "self", aber nicht "self.transform".
#   3. Rechtsklick IRGENDWO im Inspector kopiert den Transform.
import io, os, re, sys, shutil

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src")

def read(p):
    with io.open(p, encoding="utf-8") as f:
        return f.read()

def write(p, s, tag):
    shutil.copyfile(p, p + ".bak_" + tag)
    with io.open(p, "w", encoding="utf-8") as f:
        f.write(s)

def need(cond, msg):
    if not cond:
        sys.exit("!! " + msg)

# ---------------------------------------------------------------- 1 + 3
eui = os.path.join(SRC, "dai_editor_ui.cpp")
t = read(eui)

# ---- 3: der Block, der bei JEDEM Rechtsklick den Transform kopiert -------
# dai_ui_right_pressed() ist GLOBAL - es fragt "wurde die rechte Taste
# gedrueckt", nicht "ueber DIESER Zeile". Der Kommentar behauptet, die
# Header-Zeile sei getroffen; geprueft wurde das nie. Der Header meldet sein
# eigenes Rechtsklick-Ergebnis (== 3) schon zwei Zeilen darueber, also ist der
# ganze Block ersatzlos falsch.
anchor = "// header rows are full width; a right click over them while this"
i = t.find(anchor)
need(i >= 0, "Rechtsklick-Block nicht gefunden")
start = t.rfind("    {\n", 0, i)
need(start >= 0, "Blockanfang nicht gefunden")
end = t.find("\n    }\n", i)
need(end >= 0, "Blockende nicht gefunden")
end += len("\n    }\n")
need("transform copied" in t[start:end], "falscher Block erwischt")
t = t[:start] + t[end:]

# ---- 1: Constraints unter den Rigidbody --------------------------------
cstart = t.find("    // ---- Constraints ---")
need(cstart >= 0, "Constraints-Block nicht gefunden")
cend = t.find('    // ---- Image (UI) ---', cstart)
need(cend > cstart, "Ende des Constraints-Blocks nicht gefunden")
old = t[cstart:cend]
need("Clear constraints" in old, "Constraints-Block sieht anders aus")
t = t[:cstart] + t[cend:]

# der Rumpf, jetzt EINGERUECKT im Rigidbody-Aufklapper
NEW = '''
            // ---- Constraints ------------------------------------------
            // Unity's, and in Unity's PLACE: a foldout inside the Rigidbody,
            // not a block of its own. Freezing an axis is a property of the
            // BODY - a heading of its own made it read as one more component
            // to add, and it appeared under objects whose Rigidbody was
            // three sections further up.
            //
            // Frozen means the SOLVER may not move it; a script setting the
            // transform still can.
            if (r.motion == DAI_DYNAMIC) {
                dai_ui_spacing(p->ui, 2.0f);
                dai_ui_header(p->ui, "Constraints", &p->fold_freeze, nullptr);
                if (!p->fold_freeze) {
                    struct Bit { const char *label; uint32_t bit; };
                    static const Bit POS[3] = { { "X##fpx", DAI_FREEZE_POS_X },
                                                { "Y##fpy", DAI_FREEZE_POS_Y },
                                                { "Z##fpz", DAI_FREEZE_POS_Z } };
                    static const Bit ROT[3] = { { "X##frx", DAI_FREEZE_ROT_X },
                                                { "Y##fry", DAI_FREEZE_ROT_Y },
                                                { "Z##frz", DAI_FREEZE_ROT_Z } };
                    dai_ui_row(p->ui, 0.0f);
                    dai_ui_label(p->ui, "Freeze Position");
                    for (const Bit &b : POS) {
                        int fon = (r.freeze & b.bit) != 0;
                        if (dai_ui_checkbox(p->ui, b.label, &fon))
                            r.freeze = fon ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
                    }
                    dai_ui_row_end(p->ui);
                    dai_ui_row(p->ui, 0.0f);
                    dai_ui_label(p->ui, "Freeze Rotation");
                    for (const Bit &b : ROT) {
                        int fon = (r.freeze & b.bit) != 0;
                        if (dai_ui_checkbox(p->ui, b.label, &fon))
                            r.freeze = fon ? (r.freeze | b.bit) : (r.freeze & ~b.bit);
                    }
                    dai_ui_row_end(p->ui);
                    // The two combinations anyone actually types out by hand.
                    dai_ui_row(p->ui, 0.0f);
                    if (dai_ui_button_fit(p->ui, "Upright")) r.freeze |= DAI_FREEZE_UPRIGHT;
                    if (dai_ui_button_fit(p->ui, "2D plane")) r.freeze |= DAI_FREEZE_2D;
                    dai_ui_row_end(p->ui);
                    if (r.freeze && dai_ui_button_fit(p->ui, "Clear constraints")) r.freeze = 0;
                }
            }
'''

ANCH = ('            dai_ui_help(p->ui, "Restitution 0..1. 0 = stays put, 0.8 = basketball, "\n'
        '                               "1 = keeps all its energy.");\n')
j = t.find(ANCH)
need(j >= 0, "Rigidbody-Anker (Bounce-Hilfe) nicht gefunden")
j += len(ANCH)
t = t[:j] + NEW + t[j:]

write(eui, t, "p135")
print("-- dai_editor_ui.cpp: Rechtsklick-Kopie raus, Constraints unter Rigidbody")

# ---------------------------------------------------------------- 2
ui = os.path.join(SRC, "dai_ui.cpp")
u = read(ui)

# --- die Node-Mitglieder-Tabelle, direkt hinter AC_CPP -------------------
MARK = "// Every identifier already in the file, so a variable you declared three"
k = u.find(MARK)
need(k >= 0, "Stelle hinter AC_CPP nicht gefunden")
TABLE = '''// What a NODE has, spelled the way the object model spells it (see the
// prelude in editor_demo.cpp). Offered after any dotted root that is not one
// of the API globals: `self.` is the common case, `player.transform.` is the
// same thing one level in, and both are the same kind of thing.
//
// This is why "self" completed and "self.transform" did not: the table only
// ever held whole names, and nothing in it began with "self.".
const AcEntry AC_NODE[] = {
    { "transform.position",   "[x, y, z] - and .x .y .z" },
    { "transform.position.x", "one axis; y and z stay" },
    { "transform.position.y", "one axis; x and z stay" },
    { "transform.position.z", "one axis; x and y stay" },
    { "transform.rotation",   "quaternion [x, y, z, w]" },
    { "transform.yaw",        "degrees around Y" },
    { "position",             "short for transform.position" },
    { "velocity",             "[x, y, z] - read and write" },
    { "grounded",             "standing on something?" },
    { "text",                 "write: the Text component" },
    { "impulse(",             "x, y, z - one push" },
    { "setVelocity(",         "x, y, z" },
    { "isValid()",            "does this node still exist?" },
    { "id",                   "the node's number" },
};

// The globals that are NOT nodes. Offering `Math.transform.position` would be
// noise, and noise in a completion list is what makes people turn it off.
bool ac_root_is_api(const std::string &r) {
    static const char *const API[] = { "input", "body", "node", "scene", "gui",
                                       "state", "params", "Math", "JSON",
                                       "console", "ui", "Object", "Array" };
    for (const char *a : API) if (r == a) return true;
    return false;
}

// One offered completion: the text that gets inserted plus its hint. A string
// and not a table pointer, because a member completion is BUILT ("self" + "."
// + "transform.position") and does not exist in any table.
struct AcHit { std::string text; const char *hint; };

'''
u = u[:k] + TABLE + u[k:]

# --- self.* auch beim Tippen von "sel" anbieten --------------------------
OLDSELF = '    { "self",              "the node this script is on" },\n'
NEWSELF = ('    { "self",              "the node this script is on" },\n'
           '    { "self.transform.position", "[x, y, z] of this node" },\n'
           '    { "self.transform.yaw", "degrees around Y" },\n'
           '    { "self.velocity",     "[x, y, z] - read and write" },\n'
           '    { "self.grounded",     "standing on something?" },\n'
           '    { "self.text",         "write: the Text component" },\n')
need(OLDSELF in u, "self-Eintrag in AC_JS nicht gefunden")
u = u.replace(OLDSELF, NEWSELF, 1)

# --- die Praefix-Kette: ueber ALLE Punkte zurueck, nicht nur ueber einen --
OLDPFX = '''        int dotted = a;
        if (dotted > 0 && buf[dotted - 1] == '.') {
            int b = dotted - 1;
            while (b > 0 && code_is_word(buf[b - 1])) --b;
            prefix = std::string(buf + b, buf + st->caret);
            a = b;
        } else if (dotted > 1 && buf[dotted - 1] == '>' && buf[dotted - 2] == '-') {
            int b = dotted - 2;
            while (b > 0 && code_is_word(buf[b - 1])) --b;
            prefix = std::string(buf + b, buf + st->caret);
            a = b;
        }
'''
NEWPFX = '''        // Walk back over EVERY `word.` in front of the caret, not just one:
        // `self.transform.pos` has to be matched whole. Stopping after the
        // first dot is why typing `self.` offered nothing - the prefix became
        // "self." and no entry in the table ever started with that, and
        // `self.transform.` became "transform." which matched nothing either.
        while (true) {
            if (a > 0 && buf[a - 1] == '.') {
                int b = a - 1;
                while (b > 0 && code_is_word(buf[b - 1])) --b;
                if (b == a - 1) break;                  // a lone dot, not a chain
                a = b;
            } else if (a > 1 && buf[a - 1] == '>' && buf[a - 2] == '-') {
                int b = a - 2;
                while (b > 0 && code_is_word(buf[b - 1])) --b;
                if (b == a - 2) break;
                a = b;
            } else {
                break;
            }
        }
        prefix = std::string(buf + a, buf + st->caret);
        // 1.5 is a number, not a chain, and must not complete to anything.
        if (!prefix.empty() && prefix[0] >= '0' && prefix[0] <= '9') {
            a = st->caret;
            prefix.clear();
        }
'''
need(OLDPFX in u, "Praefix-Logik nicht gefunden")
u = u.replace(OLDPFX, NEWPFX, 1)

# --- Treffer: gebaute Strings statt Tabellenzeiger ------------------------
OLDHITS = '''        std::vector<const AcEntry *> hits;
        std::vector<std::string> words;
        if (st->focused && prefix.size() >= 2) {
            const AcEntry *table = lang == DAI_CODE_LANG_CPP ? AC_CPP : AC_JS;
            size_t count = lang == DAI_CODE_LANG_CPP
                         ? sizeof(AC_CPP) / sizeof(AC_CPP[0])
                         : sizeof(AC_JS) / sizeof(AC_JS[0]);
            for (size_t i = 0; i < count; ++i)
                if (std::strncmp(table[i].text, prefix.c_str(), prefix.size()) == 0)
                    hits.push_back(&table[i]);
            ac_identifiers(buf, prefix, a, words);
        }
'''
NEWHITS = '''        std::vector<AcHit> hits;
        std::vector<std::string> words;
        if (st->focused && prefix.size() >= 2) {
            const AcEntry *table = lang == DAI_CODE_LANG_CPP ? AC_CPP : AC_JS;
            size_t count = lang == DAI_CODE_LANG_CPP
                         ? sizeof(AC_CPP) / sizeof(AC_CPP[0])
                         : sizeof(AC_JS) / sizeof(AC_JS[0]);
            for (size_t i = 0; i < count; ++i)
                if (std::strncmp(table[i].text, prefix.c_str(), prefix.size()) == 0)
                    hits.push_back(AcHit{ table[i].text, table[i].hint });
            // Members of whatever is left of the first dot. No types are
            // known here and none are guessed: it is a list of names, and a
            // name that does not apply is one Escape away.
            size_t dot = prefix.find('.');
            if (lang != DAI_CODE_LANG_CPP && dot != std::string::npos) {
                std::string root = prefix.substr(0, dot);
                std::string rest = prefix.substr(dot + 1);
                if (!root.empty() && !ac_root_is_api(root)) {
                    for (const AcEntry &e : AC_NODE) {
                        if (std::strncmp(e.text, rest.c_str(), rest.size()) != 0) continue;
                        std::string full = root + "." + e.text;
                        bool dup = false;
                        for (const AcHit &h : hits) if (h.text == full) { dup = true; break; }
                        if (!dup) hits.push_back(AcHit{ full, e.hint });
                    }
                }
            }
            ac_identifiers(buf, prefix, a, words);
        }
'''
need(OLDHITS in u, "Trefferliste nicht gefunden")
u = u.replace(OLDHITS, NEWHITS, 1)

# --- die drei Benutzungsstellen auf AcHit umstellen ----------------------
reps = [
('''            const char *pick = st->ac_sel < (int)hits.size()
                             ? hits[(size_t)st->ac_sel]->text
                             : words[(size_t)(st->ac_sel - (int)hits.size())].c_str();''',
 '''            const char *pick = st->ac_sel < (int)hits.size()
                             ? hits[(size_t)st->ac_sel].text.c_str()
                             : words[(size_t)(st->ac_sel - (int)hits.size())].c_str();'''),
('''            for (const AcEntry *e : hits) {
                float tw2 = dai_ui_text_width(ui, e->text) +
                            (e->hint ? dai_ui_text_width(ui, e->hint) + 24.0f : 0.0f) + 24.0f;
                if (tw2 > lw) lw = tw2;
            }''',
 '''            for (const AcHit &e : hits) {
                float tw2 = dai_ui_text_width(ui, e.text.c_str()) +
                            (e.hint ? dai_ui_text_width(ui, e.hint) + 24.0f : 0.0f) + 24.0f;
                if (tw2 > lw) lw = tw2;
            }'''),
('''                const char *label = i < (int)hits.size() ? hits[(size_t)i]->text
                                                         : words[(size_t)(i - (int)hits.size())].c_str();
                const char *hint = i < (int)hits.size() ? hits[(size_t)i]->hint : "in this file";''',
 '''                const char *label = i < (int)hits.size() ? hits[(size_t)i].text.c_str()
                                                         : words[(size_t)(i - (int)hits.size())].c_str();
                const char *hint = i < (int)hits.size() ? hits[(size_t)i].hint : "in this file";'''),
]
for o, n in reps:
    need(o in u, "Benutzungsstelle nicht gefunden:\n" + o[:60])
    u = u.replace(o, n, 1)

write(ui, u, "p135")
print("-- dai_ui.cpp: Autocomplete kennt jetzt self.transform.* und jede Punkt-Kette")
print("OK")
