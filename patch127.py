import io

# ===========================================================================
# Autocomplete. Zwei Quellen, und beide sind ehrlich zu haben:
#
#   1. die ENGINE-API - eine feste Liste, weil sie feststeht
#   2. jeder BEZEICHNER, der in dieser Datei schon vorkommt
#
# Was es NICHT ist: ein Parser. Ein halber Parser, der bei einer unfertigen
# Zeile - und beim Tippen ist jede Zeile unfertig - falsche Vorschlaege macht,
# ist schlechter als eine Liste, von der jeder weiss, dass sie nur Namen kennt.
# ===========================================================================
p = 'include/dai_ui.h'
s = io.open(p, encoding='utf-8').read()
old = """    int   follow_caret; /* one shot: bring the caret into view on this draw  */
} dai_ui_code_state;"""
new = """    int   follow_caret; /* one shot: bring the caret into view on this draw  */
    /* Autocomplete. `ac_open` is the list being shown, `ac_sel` the row the
     * arrows are on, `ac_start` where the word being completed begins. Kept
     * in the state rather than the widget so two open scripts each keep their
     * own popup - and so closing a tab closes its list with it. */
    int   ac_open;
    int   ac_sel;
    int   ac_start;
} dai_ui_code_state;"""
assert s.count(old) == 1, 'code state not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()

old = """int dai_ui_code_edit(dai_ui *ui, const char *id, float x, float y, float w, float h,"""
new = """// The engine's own API, spelled once. It is a LIST, not a parse of the
// headers: the headers are C and the scripts are JavaScript, and a generator
// that mapped one to the other would be a build step that breaks silently the
// day someone renames a binding.
namespace {
struct AcEntry { const char *text; const char *hint; };
const AcEntry AC_JS[] = {
    { "input.key(",        "\\"w\\", \\"space\\", \\"shift\\" - held?" },
    { "input.mouseDX()",   "pixels moved this frame" },
    { "input.mouseDY()",   "pixels moved this frame" },
    { "input.mouseButton(","0 left, 1 right, 2 middle" },
    { "body.getVel(",      "self -> [x, y, z]" },
    { "body.setVel(",      "self, x, y, z" },
    { "body.impulse(",     "self, x, y, z" },
    { "body.grounded(",    "self -> standing on something?" },
    { "node.getPos(",      "id -> [x, y, z]" },
    { "node.setPos(",      "id, x, y, z" },
    { "node.getRot(",      "id -> [x, y, z, w]" },
    { "node.setRot(",      "id, x, y, z, w" },
    { "node.setText(",     "id, \\"...\\" - the Text component" },
    { "scene.find(",       "\\"name\\" -> id, or -1" },
    { "gui.text(",         "x, y, text, size, colour" },
    { "gui.rect(",         "x, y, w, h, colour" },
    { "gui.image(",        "x, y, w, h, path, tint" },
    { "gui.button(",       "x, y, w, h, label -> clicked?" },
    { "gui.size()",        "[width, height] of the view" },
    { "print(",            "one line into the Console" },
    { "state.dt",          "seconds this frame" },
    { "self",              "the node this script is on" },
    { "params",            "what the inspector stored" },
    { "function init() {", "runs once at Play" },
    { "function frame() {","runs every frame" },
    { "Math.sqrt(",        nullptr },
    { "Math.atan2(",       nullptr },
    { "Math.floor(",       nullptr },
    { "Math.random()",     nullptr },
};
const AcEntry AC_CPP[] = {
    { "api->log(api, ",          "one line into the Console" },
    { "api->get_position(api, ", "self -> dai_nvec3" },
    { "api->set_position(api, ", "self, dai_nvec3" },
    { "api->get_velocity(api, ", "self -> dai_nvec3" },
    { "api->set_velocity(api, ", "self, dai_nvec3" },
    { "api->add_impulse(api, ",  "self, dai_nvec3" },
    { "api->get_rotation(api, ", "self, float xyzw[4]" },
    { "api->set_rotation(api, ", "self, const float xyzw[4]" },
    { "api->get_scale(api, ",    "self -> dai_nvec3" },
    { "api->set_scale(api, ",    "self, dai_nvec3" },
    { "api->find(api, ",         "\\"name\\" -> entity, 0 if none" },
    { "api->name_of(api, ",      "entity -> const char *" },
    { "api->time(api)",          "seconds since Play" },
    { "api->key_down(api, ",     "DAI_KEY_* or a letter" },
    { "DAI_BEHAVIOUR_INIT(api, self) {",  "runs once at Play" },
    { "DAI_BEHAVIOUR_FRAME(api, self, dt) {", "runs every frame" },
    { "DAI_KEY_SPACE",  nullptr },
    { "DAI_KEY_SHIFT_L", nullptr },
    { "DAI_KEY_LEFT",   nullptr },
    { "DAI_KEY_RIGHT",  nullptr },
    { "DAI_KEY_UP",     nullptr },
    { "DAI_KEY_DOWN",   nullptr },
};

// Every identifier already in the file, so a variable you declared three
// lines up can be completed too. Cheap enough at every keystroke: a behaviour
// is a few hundred lines, and this is a single pass over them.
void ac_identifiers(const char *buf, const std::string &prefix, int skip_at,
                    std::vector<std::string> &out) {
    if (prefix.empty()) return;
    int i = 0;
    while (buf[i]) {
        if (!code_is_word(buf[i]) || (buf[i] >= '0' && buf[i] <= '9')) { ++i; continue; }
        int a = i;
        while (buf[i] && code_is_word(buf[i])) ++i;
        if (a == skip_at) continue;                 // the word being typed
        std::string word(buf + a, buf + i);
        if (word.size() <= prefix.size()) continue;
        // Case sensitive: JavaScript is, C++ is, and a completion that
        // changes the case of what you typed is a completion you retype.
        if (word.compare(0, prefix.size(), prefix) != 0) continue;
        bool have = false;
        for (const std::string &e : out) if (e == word) { have = true; break; }
        if (!have) out.push_back(word);
        if (out.size() > 40) return;
    }
}
} // namespace

int dai_ui_code_edit(dai_ui *ui, const char *id, float x, float y, float w, float h,"""
assert s.count(old) == 1, 'code_edit def not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('autocomplete: the tables and the identifier scan')
