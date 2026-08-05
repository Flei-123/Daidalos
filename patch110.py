import io

p = 'examples/runtime_main.cpp'
s = io.open(p, encoding='utf-8').read()

# ---------------------------------------------------------------------------
# Das HUD im ausgelieferten Spiel. Ohne das waere die Text-Komponente eine
# Editor-Attrappe: sichtbar beim Bauen, weg beim Spielen.
# ---------------------------------------------------------------------------
old = """#include "dai_doc.h\""""
new = """#include "dai_doc.h"
// The game's UI. dai_hud_draw is the SAME function the editor's Game view
// calls - what you place is what ships, or the Game view is a rehearsal for a
// different play. dai_ui is only here to own the font atlas and build the
// vertex batches; no editor panel comes with it.
#include "dai_editor_ui.h"
#include "dai_ui.h"
#include "dai_font.h"
#include "dai_strings.h\""""
assert s.count(old) == 1, 'doc include not found'
s = s.replace(old, new)

old = """static dai_doc       *g_doc = nullptr;"""
new = """// ---- the HUD ------------------------------------------------------------
static dai_ui      *g_ui = nullptr;
static dai_font    *g_hud_font = nullptr;
static dai_strings *g_hud_strings = nullptr;

// The exported game reads its string table out of the pack, like everything
// else: dai_vfs, not the file system. A game that looks for Assets/Strings on
// the player's disk is a game that has no strings on the player's disk.
static void hud_strings_load(const char *lang) {
    if (!g_hud_strings) g_hud_strings = dai_strings_create();
    if (!lang || !lang[0]) return;
    char vpath[128];
    std::snprintf(vpath, sizeof(vpath), "Strings/%s.daistr", lang);
    size_t n = 0;
    const void *bytes = dai_vfs_read(vpath, &n);
    if (!bytes || !n) { rt_log("no string table %s", vpath); return; }
    char err[256] = { 0 };
    if (dai_strings_parse(g_hud_strings, (const char *)bytes, n, err, sizeof(err)) != DAI_OK)
        rt_log("string table %s: %s", vpath, err);
    else
        rt_log("language %s: %u entries", lang, (unsigned)dai_strings_count(g_hud_strings));
}

static const char *hud_resolve(const char *text, void *) {
    static char buf[256];
    return dai_strings_resolve(g_hud_strings, text, buf, sizeof(buf));
}

static dai_doc       *g_doc = nullptr;"""
assert s.count(old) == 1, 'g_doc anchor not found'
s = s.replace(old, new)

# --- pro Frame zeichnen ----------------------------------------------------
old = """            uint32_t n = dai_scene_instances(g_scene, inst.data(), (uint32_t)inst.size(), alpha);
            dai_render_frame(r, inst.data(), n);
            dai_window_present(win);"""
new = """            uint32_t n = dai_scene_instances(g_scene, inst.data(), (uint32_t)inst.size(), alpha);

            // The HUD, over the whole window - the game IS the viewport here.
            if (g_ui) {
                dai_ui_input uin{};
                uin.width = (float)ww; uin.height = (float)wh;
                dai_ui_begin(g_ui, (float)ww, (float)wh, &uin);
                dai_hud_draw(g_ui, g_doc, 0.0f, 0.0f, (float)ww, (float)wh, 1.0f,
                             hud_resolve, nullptr);
                dai_ui_end(g_ui);
                const dai_ui_draw *draws = nullptr;
                uint32_t nb = dai_ui_draws(g_ui, &draws);
                std::vector<dai_ui_vertex> verts;
                std::vector<uint32_t> counts;
                std::vector<dai_texture> texes;
                for (uint32_t i = 0; i < nb; ++i) {
                    if (!draws[i].vertices || !draws[i].count) continue;
                    verts.insert(verts.end(), draws[i].vertices, draws[i].vertices + draws[i].count);
                    counts.push_back(draws[i].count);
                    texes.push_back(draws[i].texture);
                }
                dai_render_ui(r, verts.data(), (uint32_t)verts.size(),
                              counts.data(), texes.data(), (uint32_t)counts.size());
            }

            dai_render_frame(r, inst.data(), n);
            dai_window_present(win);"""
assert s.count(old) == 1, 'frame present anchor not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('runtime_main: the same HUD the editor shows')
