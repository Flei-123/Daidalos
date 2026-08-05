# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# ------------------------------------------------------------- dai_icons.cpp
P = 'src/dai_icons.cpp'
s = rw(P)
NEW = r'''// ---- what a hierarchy row IS -------------------------------------------
// An editor that writes the object's type into its NAME ("MainCamera") is
// spending the one column the user reads on information a 16 px glyph carries
// for free - and then the name column no longer holds the name.
{ "light", STROKE_HEAD
  "<line x1='12' y1='1' x2='12' y2='4'/><line x1='12' y1='20' x2='12' y2='23'/>"
  "<line x1='4.2' y1='4.2' x2='6.3' y2='6.3'/><line x1='17.7' y1='17.7' x2='19.8' y2='19.8'/>"
  "<line x1='1' y1='12' x2='4' y2='12'/><line x1='20' y1='12' x2='23' y2='12'/>"
  "<circle cx='12' cy='12' r='4.5'/>" TAIL },
{ "empty", STROKE_HEAD
  "<path d='M12 3 3 7.5v9L12 21l9-4.5v-9z' stroke-dasharray='3 2.4'/>" TAIL },
{ "cube", STROKE_HEAD
  "<path d='M12 2.6 21 7v10l-9 4.4L3 17V7z'/><path d='M3 7l9 4.4L21 7'/>"
  "<line x1='12' y1='11.4' x2='12' y2='21.4'/>" TAIL },
{ "sprite", STROKE_HEAD
  "<rect x='3' y='3' width='18' height='18' rx='2'/><circle cx='8.5' cy='8.5' r='1.8'/>"
  "<path d='M21 15l-5-5L5 21'/>" TAIL },
{ "volume", STROKE_HEAD
  "<polygon points='11 5 6 9 2 9 2 15 6 15 11 19'/>"
  "<path d='M15.5 8.5a5 5 0 0 1 0 7'/><path d='M18.5 5.5a9 9 0 0 1 0 13'/>" TAIL },
{ "volume-x", STROKE_HEAD
  "<polygon points='11 5 6 9 2 9 2 15 6 15 11 19'/>"
  "<line x1='22' y1='9' x2='16' y2='15'/><line x1='16' y1='9' x2='22' y2='15'/>" TAIL },
{ "material", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><path d='M12 3a9 9 0 0 0 0 18'/>"
  "<circle cx='15.5' cy='8.5' r='1.3'/><circle cx='17' cy='13' r='1.3'/>" TAIL },
{ "prefab", STROKE_HEAD
  "<path d='M12 2.6 21 7v10l-9 4.4L3 17V7z'/><path d='M3 7l9 4.4L21 7'/>"
  "<line x1='12' y1='11.4' x2='12' y2='21.4'/><circle cx='12' cy='11.6' r='2.2'/>" TAIL },
{ "info", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><line x1='12' y1='11' x2='12' y2='16.5'/>"
  "<line x1='12' y1='7.6' x2='12' y2='7.7'/>" TAIL },
{ "warning", STROKE_HEAD
  "<path d='M10.3 3.6 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.6a2 2 0 0 0-3.4 0z'/>"
  "<line x1='12' y1='9' x2='12' y2='13.5'/><line x1='12' y1='17' x2='12' y2='17.1'/>" TAIL },
{ "error", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><line x1='15.5' y1='8.5' x2='8.5' y2='15.5'/>"
  "<line x1='8.5' y1='8.5' x2='15.5' y2='15.5'/>" TAIL },
{ "more", STROKE_HEAD
  "<circle cx='12' cy='5' r='1.4'/><circle cx='12' cy='12' r='1.4'/>"
  "<circle cx='12' cy='19' r='1.4'/>" TAIL },
{ "reset", STROKE_HEAD
  "<polyline points='2.5 5 2.5 11 8.5 11'/>"
  "<path d='M4.6 15a9 9 0 1 0 2.1-9.4L2.5 11'/>" TAIL },
{ "arrow-up", STROKE_HEAD
  "<line x1='12' y1='19' x2='12' y2='5'/><polyline points='5 12 12 5 19 12'/>" TAIL },
{ "arrow-down", STROKE_HEAD
  "<line x1='12' y1='5' x2='12' y2='19'/><polyline points='19 12 12 19 5 12'/>" TAIL },
{ "target", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><circle cx='12' cy='12' r='4'/>"
  "<line x1='12' y1='1.5' x2='12' y2='4'/><line x1='12' y1='20' x2='12' y2='22.5'/>"
  "<line x1='1.5' y1='12' x2='4' y2='12'/><line x1='20' y1='12' x2='22.5' y2='12'/>" TAIL },

// ---- assets -------------------------------------------------------------
'''
s = sub1(s, "// ---- assets -------------------------------------------------------------\n", NEW, "icons")
wr(P, s)

P = 'include/dai_icons.h'
s = rw(P)
s = sub1(s, '#define DAI_ICON_CONSOLE    "console"',
'''#define DAI_ICON_CONSOLE    "console"
#define DAI_ICON_LIGHT      "light"
#define DAI_ICON_EMPTY      "empty"
#define DAI_ICON_CUBE       "cube"
#define DAI_ICON_SPRITE     "sprite"
#define DAI_ICON_VOLUME     "volume"
#define DAI_ICON_VOLUME_X   "volume-x"
#define DAI_ICON_MATERIAL   "material"
#define DAI_ICON_PREFAB     "prefab"
#define DAI_ICON_INFO       "info"
#define DAI_ICON_WARNING    "warning"
#define DAI_ICON_ERROR      "error"
#define DAI_ICON_MORE       "more"
#define DAI_ICON_RESET      "reset"
#define DAI_ICON_ARROW_UP   "arrow-up"
#define DAI_ICON_ARROW_DOWN "arrow-down"
#define DAI_ICON_TARGET     "target"''', "icon names")
wr(P, s)

# ---------------------------------------------------------------- dai_ui.cpp
P = 'src/dai_ui.cpp'
s = rw(P)
s = sub1(s,
"""int dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                        int has_children, int *open, int selected) {
    if (!ui || !label) return 0;""",
"""int dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                        int has_children, int *open, int selected) {
    return dai_ui_tree_item_icon(ui, nullptr, label, depth, has_children, open, selected);
}

int dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label, int depth,
                          int has_children, int *open, int selected) {
    if (!ui || !label) return 0;""", "tree_item_icon split")

s = sub1(s,
"""    dai_ui_text(ui, x + indent + arrow_w + 2.0f, y + 1.0f, label, ui->style.text);
    return clicked;
}

int dai_ui_tree_rename""",
"""    float tx = x + indent + arrow_w + 2.0f;
    if (icon && dai_ui_has_icon(ui, icon)) {
        float isz = dai_icons_size(ui->icons);
        if (isz <= 0.0f || isz > h) isz = h - 2.0f;
        // Tinted like the text, dimmer when the row is not selected: an icon
        // column that shouts is a column you read instead of the names.
        dai_ui_icon_at(ui, icon, tx, y + (h - isz) * 0.5f, isz,
                       selected ? ui->style.text : ui->style.text_dim);
        tx += isz + 4.0f;
    }
    dai_ui_text(ui, tx, y + 1.0f, label, ui->style.text);
    return clicked;
}

int dai_ui_tree_rename""", "tree icon draw")
wr(P, s)

P = 'include/dai_ui.h'
s = rw(P)
s = sub1(s,
"""DAI_API int  dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                                 int has_children, int *open, int selected);""",
"""DAI_API int  dai_ui_tree_item_ex(dai_ui *ui, const char *label, int depth,
                                 int has_children, int *open, int selected);
/* The same row with the object's KIND drawn in front of its name. A hierarchy
 * that has to spell "MainCamera" into the name to say "this is a camera" has
 * given up its only readable column; a glyph says it in 16 pixels and leaves
 * the name to be the name. */
DAI_API int  dai_ui_tree_item_icon(dai_ui *ui, const char *icon, const char *label,
                                   int depth, int has_children, int *open, int selected);""",
"tree_item_icon decl")
wr(P, s)
print("patch4 done")
