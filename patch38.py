#!/usr/bin/env python3
# patch38 - icons that are drawings again, a Window menu with something in it,
# and two buttons that are buttons.
import sys, os, re
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p38'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ======================================================== 1. the icon tiles
# The "component tile" icons were a coloured plate with a white glyph on it -
# except dai_svg reads `fill` as a yes/no, not as a colour. Every one of them
# rasterised as ONE solid rounded square, which is the blue box in the
# inspector. Colour comes from the CALLER here; the icon is a drawing.
s = rd('src/dai_icons.cpp')
start = s.index('// ---- component tiles ---')
end = s.index('{ "c-prefab", FILL_HEAD')
end2 = s.index('{ "folder", STROKE_HEAD', end)
s = s[:start] + """// ---- component glyphs ---------------------------------------------------
// Line drawings, tinted by whoever draws them. A 16 px square filled with one
// colour is not an icon, it is a swatch - and that is what the previous set
// rasterised to, because this SVG reader treats `fill` as a yes/no.
{ "transform", STROKE_HEAD
  "<polyline points='12 3 12 21'/><polyline points='3 12 21 12'/>"
  "<polyline points='9.6 5.4 12 3 14.4 5.4'/><polyline points='9.6 18.6 12 21 14.4 18.6'/>"
  "<polyline points='5.4 9.6 3 12 5.4 14.4'/><polyline points='18.6 9.6 21 12 18.6 14.4'/>"
  "<circle cx='12' cy='12' r='2.4'/>" TAIL },
{ "mesh", STROKE_HEAD
  "<path d='M12 2.8 20.6 7.4v9.2L12 21.2 3.4 16.6V7.4z'/>"
  "<path d='M3.4 7.4 12 12l8.6-4.6'/><line x1='12' y1='12' x2='12' y2='21.2'/>"
  "<path d='M7.7 5.1 16.3 9.7'/>" TAIL },
{ "collider", STROKE_HEAD
  "<path d='M12 2.8 20.6 7.4v9.2L12 21.2 3.4 16.6V7.4z' stroke-dasharray='2.6 2'/>"
  "<circle cx='12' cy='12' r='1.6'/>" TAIL },
{ "body", STROKE_HEAD
  "<circle cx='12' cy='7.5' r='3.6'/>"
  "<line x1='12' y1='12.4' x2='12' y2='19'/><polyline points='8.6 15.8 12 19.4 15.4 15.8'/>" TAIL },
{ "braces", STROKE_HEAD
  "<path d='M9.4 3.5C7 3.5 7.6 8.6 6 10.2c-.7.7-1.4 1-2 1.8.6.8 1.3 1.1 2 1.8 1.6 1.6 1 6.7 3.4 6.7'/>"
  "<path d='M14.6 3.5c2.4 0 1.8 5.1 3.4 6.7.7.7 1.4 1 2 1.8-.6.8-1.3 1.1-2 1.8-1.6 1.6-1 6.7-3.4 6.7'/>" TAIL },
{ "cam", STROKE_HEAD
  "<rect x='2.6' y='7.4' width='12.6' height='9.2' rx='2'/>"
  "<path d='M15.2 11 21.4 7.6v8.8L15.2 13z'/>" TAIL },
{ "bulb", STROKE_HEAD
  "<path d='M9 17.5a6.5 6.5 0 1 1 6 0'/><line x1='9.4' y1='19.4' x2='14.6' y2='19.4'/>"
  "<line x1='10.2' y1='21.6' x2='13.8' y2='21.6'/>" TAIL },
{ "speaker", STROKE_HEAD
  "<path d='M11 5 6.4 9H3v6h3.4L11 19z'/>"
  "<path d='M15 9.4a4 4 0 0 1 0 5.2'/><path d='M17.8 6.8a8 8 0 0 1 0 10.4'/>" TAIL },
{ "surface", STROKE_HEAD
  "<circle cx='12' cy='12' r='8.6'/><path d='M4.6 16.2A8.6 8.6 0 0 1 16.2 4.6'/>"
  "<circle cx='15.4' cy='8.6' r='1.5'/>" TAIL },
{ "quad", STROKE_HEAD
  "<rect x='3.2' y='4.6' width='17.6' height='14.8' rx='2'/>"
  "<circle cx='8.4' cy='9.4' r='1.6'/><path d='M20.8 15.6 15.2 10 6 19.4'/>" TAIL },
{ "package", STROKE_HEAD
  "<path d='M12 2.8 20.6 7.4v9.2L12 21.2 3.4 16.6V7.4z'/>"
  "<path d='M3.4 7.4 12 12l8.6-4.6'/><line x1='12' y1='12' x2='12' y2='21.2'/>"
  "<circle cx='12' cy='12' r='2'/>" TAIL },
""" + s[end2:]
wr('src/dai_icons.cpp', s)

s = rd('include/dai_icons.h')
i0 = s.index('/* The component tiles:')
i1 = s.index('#define DAI_ICON_C_PREFAB    "c-prefab"') + len('#define DAI_ICON_C_PREFAB    "c-prefab"')
s = s[:i0] + """/* The component glyphs. Line drawings, deliberately: the colour comes from
 * whoever draws them, so one icon serves the hover state, the dimmed state
 * and the component's own hue. */
#define DAI_ICON_C_TRANSFORM "transform"
#define DAI_ICON_C_MESH      "mesh"
#define DAI_ICON_C_COLLIDER  "collider"
#define DAI_ICON_C_BODY      "body"
#define DAI_ICON_C_SCRIPT    "braces"
#define DAI_ICON_C_CAMERA    "cam"
#define DAI_ICON_C_LIGHT     "bulb"
#define DAI_ICON_C_AUDIO     "speaker"
#define DAI_ICON_C_MATERIAL  "surface"
#define DAI_ICON_C_SPRITE    "quad"
#define DAI_ICON_C_PREFAB    "package\"""" + s[i1:]
wr('include/dai_icons.h', s)

# ==================================================== 2. a tint per component
s = rd('include/dai_ui.h')
s = sub1(s, "DAI_API int dai_ui_header_icon(dai_ui *ui, const char *icon, const char *title,",
"""/* The same header with the icon in the component's own colour. Unity tells
 * its components apart by hue before anything is read; one accent-blue glyph
 * per row is a list you have to spell out to yourself. */
DAI_API int dai_ui_header_icon_col(dai_ui *ui, const char *icon, uint32_t tint,
                                   const char *title, int *open, int *enabled);
DAI_API int dai_ui_header_icon(dai_ui *ui, const char *icon, const char *title,""",
'header col decl')
wr('include/dai_ui.h', s)

s = rd('src/dai_ui.cpp')
s = sub1(s,
"""int dai_ui_header_icon(dai_ui *ui, const char *icon, const char *title,
                       int *open, int *enabled) {
    if (!ui || !title) return 0;""",
"""int dai_ui_header_icon_col(dai_ui *ui, const char *icon, uint32_t tint,
                           const char *title, int *open, int *enabled);

int dai_ui_header_icon(dai_ui *ui, const char *icon, const char *title,
                       int *open, int *enabled) {
    return dai_ui_header_icon_col(ui, icon, 0, title, open, enabled);
}

int dai_ui_header_icon_col(dai_ui *ui, const char *icon, uint32_t tint,
                           const char *title, int *open, int *enabled) {
    if (!ui || !title) return 0;""",
'header col impl')
s = sub1(s,
"""        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz, ui->style.accent);
        tx += isz + 5.0f;""",
"""        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz,
                       tint ? tint : ui->style.accent);
        tx += isz + 5.0f;""",
'header tint use')
wr('src/dai_ui.cpp', s)

# ====================================================== 3. the editor's rows
s = rd('src/dai_editor_ui.cpp')
COL = {
    'DAI_ICON_C_TRANSFORM': 'rgba(0x6C, 0xA9, 0xF5, 255)',
    'DAI_ICON_C_MESH':      'rgba(0x4F, 0xD1, 0xC5, 255)',
    'DAI_ICON_C_COLLIDER':  'rgba(0x7A, 0xD9, 0x7A, 255)',
    'DAI_ICON_C_BODY':      'rgba(0xA7, 0x9B, 0xF0, 255)',
    'DAI_ICON_C_SCRIPT':    'rgba(0xF2, 0xC1, 0x4E, 255)',
    'DAI_ICON_C_CAMERA':    'rgba(0x9F, 0xB4, 0xD8, 255)',
    'DAI_ICON_C_LIGHT':     'rgba(0xF5, 0xD7, 0x6E, 255)',
    'DAI_ICON_C_AUDIO':     'rgba(0xF2, 0x9D, 0x5B, 255)',
    'DAI_ICON_C_SPRITE':    'rgba(0x6F, 0xC7, 0xEA, 255)',
}
for icon, col in COL.items():
    old = 'dai_ui_header_icon(p->ui, %s, ' % icon
    if old not in s: print('MISS header ' + icon); sys.exit(1)
    s = s.replace(old, 'dai_ui_header_icon_col(p->ui, %s, %s, ' % (icon, col))

# the material row's icon colour
s = sub1(s, "                                                DAI_ICON_C_MATERIAL)) {",
            "                                                DAI_ICON_C_MATERIAL)) {",
         'noop mat')

# Settings and Window are actions, not toggles - a lit button says "this is
# on", and neither of them is a state.
s = sub1(s, 'dai_ui_icon_button(p->ui, DAI_ICON_SETTINGS, "Settings", p->settings_open)',
            'dai_ui_icon_button(p->ui, DAI_ICON_SETTINGS, "Settings", 0)',
         'settings not toggle')
s = sub1(s, 'dai_ui_icon_button(p->ui, DAI_ICON_WINDOW, "Window", p->menu_window.open)',
            'dai_ui_icon_button(p->ui, DAI_ICON_WINDOW, "Window", 0)',
         'window not toggle')

# ...and the Window menu lists the panels this editor HAS, whether or not the
# dock happened to be told about them by name this session.
s = sub1(s,
"""    if (p->menu_window.open) {
        const char *names[16];
        uint32_t n = dai_dock_panels(p->dock, names, 16);
        if (n > 16) n = 16;""",
"""    if (p->menu_window.open) {
        // The dock's own register, plus the set this editor always has. A
        // layout loaded from disk restores the TREE, not the register - so
        // after the first restart the menu was empty and the button did
        // nothing at all, which is exactly what it looked like.
        static const char *const ALWAYS[] = { "Scene", "Game", "Hierarchy", "Inspector",
                                              "Project", "Console", "Audio", "Settings" };
        const char *names[16];
        uint32_t n = dai_dock_panels(p->dock, names, 16);
        if (n > 16) n = 16;
        for (uint32_t k = 0; k < 8 && n < 16; ++k) {
            bool have = false;
            for (uint32_t i = 0; i < n; ++i)
                if (std::strcmp(names[i], ALWAYS[k]) == 0) { have = true; break; }
            if (!have) names[n++] = ALWAYS[k];
        }""",
'window menu fallback')
wr('src/dai_editor_ui.cpp', s)

# ============================================ 4. a popup with nothing in it
# ...closed itself never, so the button that opened it looked dead for the
# rest of the session.
s = rd('src/dai_ui.cpp')
s = sub1(s,
"""int dai_ui_popup_menu(dai_ui *ui, dai_ui_popup *m,
                      const dai_ui_menu_item *items, uint32_t count) {
    if (!ui || !m || !m->open || !items || !count) return -2;""",
"""int dai_ui_popup_menu(dai_ui *ui, dai_ui_popup *m,
                      const dai_ui_menu_item *items, uint32_t count) {
    if (!ui || !m || !m->open) return -2;
    // An empty menu is not a menu. Closing it here is the difference between
    // "nothing happened" and a button that stays stuck open for ever.
    if (!items || !count) { m->open = 0; return -1; }""",
'empty popup closes')
wr('src/dai_ui.cpp', s)

# ...and the dock remembers every title a loaded layout mentions.
s = rd('src/dai_dock.cpp')
s = sub1(s,
"""    for (;;) {
        p = skip_ws(p);
        if (std::strncmp(p, "float", 5) == 0) {""",
"""    // Every title the file mentions is now a panel this dock KNOWS - the
    // register is what the Window menu is built from, and loading a layout
    // used to leave it empty.
    {
        std::vector<Node *> all;
        collect_leaves(d->root, &all);
        for (auto &f : d->floats) collect_leaves(f.root, &all);
        for (Node *leaf : all)
            for (const std::string &t : leaf->tabs) {
                bool known = false;
                for (const auto &r : d->regs) if (r.title == t) { known = true; break; }
                if (!known) d->regs.push_back(dai_dock::Reg{ t, DAI_DOCK_NONE, 0.22f, "" });
            }
    }

    for (;;) {
        p = skip_ws(p);
        if (std::strncmp(p, "float", 5) == 0) {""",
'dock regs from text')
wr('src/dai_dock.cpp', s)
print('patch38 ok')
