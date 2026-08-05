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

# ---- 1. audio: the mute state is an icon, not the word "muted" ------------
s = sub1(s,
"""        if (browser_row(p, px + 70.0f, y, 44.0f, 20.0f, nullptr,
                        p->bus_mute[i] ? "muted" : "on", p->bus_mute[i]))
            p->bus_mute[i] = !p->bus_mute[i];""",
"""        // A speaker with a cross through it needs no translation and no
        // column width; "muted"/"on" needed both.
        if (dai_ui_icon_button_at(ui, p->bus_mute[i] ? DAI_ICON_VOLUME_X : DAI_ICON_VOLUME,
                                  px + 70.0f, y, 24.0f, 20.0f, p->bus_mute[i]))
            p->bus_mute[i] = !p->bus_mute[i];""", "audio mute icon")

# ---- 2. console levels get their icons ------------------------------------
s = sub1(s,
"""    for (int i = 0; i < 3; ++i) {
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "%s %u", LEVEL_NAME[i], counts[i]);
        float w = dai_ui_text_width(ui, lbl) + 18.0f;
        if (browser_row(p, bx, py + 4.0f, w, BTN_H, nullptr, lbl, p->log_show[i]))
            p->log_show[i] = !p->log_show[i];
        bx += w + 4.0f;
    }""",
"""    static const char *LEVEL_ICON[3] = { DAI_ICON_INFO, DAI_ICON_WARNING, DAI_ICON_ERROR };
    for (int i = 0; i < 3; ++i) {
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "%u", counts[i]);
        float w = dai_ui_text_width(ui, lbl) + 34.0f;
        if (browser_row(p, bx, py + 4.0f, w, BTN_H, LEVEL_ICON[i], lbl, p->log_show[i]))
            p->log_show[i] = !p->log_show[i];
        bx += w + 4.0f;
    }""", "console level icons")

# ---- 3. play mode dims everything that is not the game --------------------
s = sub1(s,
"""// The project window's body, so it can live in a dock panel of any size.
// The console: engine messages and script print(), filterable by level.""",
"""// While the game runs, everything that is NOT the game goes behind glass.
// Unity does this for one reason and it is a good one: edits made in play mode
// are thrown away on Stop, and an editor that looks identical either way will
// eat an afternoon of work exactly once per user.
//
// Drawn INSIDE the panel's own layer (before dock_panel_end pops it), so it
// covers that panel and nothing else - the scene and game views never call it.
static void play_dim(dai_editor_ui *p, float px, float py, float pw, float ph) {
    if (dai_editor_state_get(p->ed) == DAI_EDITOR_EDIT) return;
    dai_ui_rect(p->ui, px, py, pw, ph, 0x66000000u);
}

// The project window's body, so it can live in a dock panel of any size.
// The console: engine messages and script print(), filterable by level.""", "play_dim")

for name, body in [("Hierarchy", "hierarchy_body(p, ph - 8.0f);"),
                   ("Project", "project_body(p, px, py, pw, ph);"),
                   ("Console", "console_body(p, px, py, pw, ph);"),
                   ("Audio", "audio_body(p, px, py, pw, ph);")]:
    if body not in s: print("MISS dim", name); sys.exit(1)
    s = s.replace(body, body + "\n        play_dim(p, px, py, pw, ph);", 1)
    print("ok dim", name)

s = sub1(s,
"""        inspector_body(p);
        dai_ui_scroll_end(ui);""",
"""        inspector_body(p);
        dai_ui_scroll_end(ui);
        play_dim(p, px, py, pw, ph);""", "dim Inspector")

s = sub1(s,
"""        settings_body(p);
        dai_ui_panel_end(ui);""",
"""        settings_body(p);
        play_dim(p, px, py, pw, ph);
        dai_ui_panel_end(ui);""", "dim Settings")

wr(P, s)

# --------------------------------------------------------- editor_demo.cpp
P = 'examples/editor_demo.cpp'
s = rw(P)
s = sub1(s,
"""    // Order IS dai_physics_backend: TALOS=0, NULL=1, JOLT=2.
    static const char *const BACKENDS[] = { "Talos", "None", "Jolt" };
    dai_ui_option(ui, "Physics", &ps.physics_backend, BACKENDS, 3);
    dai_ui_num_field(ui, "Friction", &ps.default_friction, 0.01f, 0.0f, 10.0f, "psfric");
    dai_ui_num_field(ui, "Bounce", &ps.default_restitution, 0.01f, 0.0f, 1.0f, "psrest");""",
"""    // Order IS dai_physics_backend: TALOS=0, NULL=1, JOLT=2. The labels say
    // what each one DOES, because "None" was read as "physics off" and it is
    // not: the null backend still integrates gravity and still has a floor at
    // y = 0, which is why turning it on and watching everything fall looked
    // like the setting was ignored.
    static const char *const BACKENDS[] = {
        "Talos (full)", "Kein Solver (nur Fall)", "Jolt (extern)"
    };
    dai_ui_option(ui, "Physics", &ps.physics_backend, BACKENDS, 3);
    dai_ui_help(ui, "Talos: collisions, joints, friction. Kein Solver: gravity and a "
                    "floor at y=0 only, bodies pass through each other. Jolt: only in a "
                    "build made with WITH_JOLT=1.");
    // The honest part. A dropdown that offers a backend this binary cannot
    // load, then reports success, is worse than one that offers nothing.
    if (!dai_physics_available(ps.physics_backend)) {
        dai_ui_label(ui, "!! not in this build - you would get 'Kein Solver'");
    }
    if (g_world_for_settings) {
        const char *live = dai_backend_name(g_world_for_settings);
        dai_ui_label_fmt(ui, "running: %s%s", live ? live : "?",
                         ps.physics_backend != g_active_backend ? "  (restart to switch)" : "");
    }
    dai_ui_separator(ui);
    dai_ui_label(ui, "Defaults for NEW rigidbodies");
    dai_ui_num_field(ui, "Def. friction", &ps.default_friction, 0.01f, 0.0f, 10.0f, "psfric");
    dai_ui_help(ui, "Coefficient mu given to every Rigidbody component added from now on. "
                    "0 = ice, 0.6 = wood, 1.0 = rubber. Existing objects are not touched.");
    dai_ui_num_field(ui, "Def. bounce", &ps.default_restitution, 0.01f, 0.0f, 1.0f, "psrest");
    dai_ui_help(ui, "Restitution 0..1 given to every new Rigidbody component. "
                    "Existing objects are not touched.");
    dai_ui_separator(ui);""", "project settings physics")

s = sub1(s,
"""static dai_world *g_world_for_settings = nullptr;""",
"""static dai_world *g_world_for_settings = nullptr;
// What the running world was actually created with, so the panel can say
// "restart to switch" instead of pretending the dropdown took effect.
static int g_active_backend = 0;""", "active backend var")

wr(P, s)
print("patch6 done")
