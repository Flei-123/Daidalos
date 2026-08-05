#!/usr/bin/env python3
# patch31 - colour in the icon set, component tiles, where a new object lands,
# and a drop target you can see.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))

def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p31'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ============================================================== dai_icons.h
s = rd('include/dai_icons.h')
s = sub1(s, '#define DAI_ICON_TARGET     "target"',
"""#define DAI_ICON_TARGET     "target"
#define DAI_ICON_MINUS      "minus"
#define DAI_ICON_REFRESH    "refresh"
#define DAI_ICON_FOLDER_OPEN "folder-open"
#define DAI_ICON_CODE       "code"
#define DAI_ICON_LOCK       "lock"
/* The component tiles: a rounded plate in the component's own colour with a
 * white glyph on it, the way every modern editor marks its component list.
 * They carry their colours, so they are never tinted. */
#define DAI_ICON_C_TRANSFORM "c-transform"
#define DAI_ICON_C_MESH      "c-mesh"
#define DAI_ICON_C_COLLIDER  "c-collider"
#define DAI_ICON_C_BODY      "c-body"
#define DAI_ICON_C_SCRIPT    "c-script"
#define DAI_ICON_C_CAMERA    "c-camera"
#define DAI_ICON_C_LIGHT     "c-light"
#define DAI_ICON_C_AUDIO     "c-audio"
#define DAI_ICON_C_MATERIAL  "c-material"
#define DAI_ICON_C_SPRITE    "c-sprite\"""",
'icons.h names')
wr('include/dai_icons.h', s)

# ============================================================ dai_icons.cpp
s = rd('src/dai_icons.cpp')
NEW = r"""{ "minus", STROKE_HEAD
  "<line x1='5' y1='12' x2='19' y2='12'/>" TAIL },
{ "refresh", STROKE_HEAD
  "<polyline points='21 4 21 10 15 10'/><polyline points='3 20 3 14 9 14'/>"
  "<path d='M5.6 9a8 8 0 0 1 13.2-3L21 8'/><path d='M18.4 15a8 8 0 0 1-13.2 3L3 16'/>" TAIL },
{ "folder-open", STROKE_HEAD
  "<path d='M2 19V5a2 2 0 0 1 2-2h5l2 3h7a2 2 0 0 1 2 2v2'/>"
  "<path d='M2 19l3.2-8h17L19 19a2 2 0 0 1-1.9 1.4H4A2 2 0 0 1 2 19z'/>" TAIL },
{ "code", STROKE_HEAD
  "<polyline points='9 6 3.5 12 9 18'/><polyline points='15 6 20.5 12 15 18'/>" TAIL },
{ "lock", STROKE_HEAD
  "<rect x='4' y='10.5' width='16' height='10.5' rx='2'/>"
  "<path d='M8 10.5V7a4 4 0 0 1 8 0v3.5'/>" TAIL },

// ---- component tiles ----------------------------------------------------
// A plate in the component's colour with a white glyph. Unity's inspector is
// readable at a glance because the components are told apart by HUE before
// anything is read; a column of identical grey outlines is not.
{ "c-transform", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#4F7BD9'/>"
  "<rect x='6' y='11.2' width='12' height='1.6' fill='#FFFFFF'/>"
  "<rect x='11.2' y='6' width='1.6' height='12' fill='#FFFFFF'/>"
  "<circle cx='12' cy='12' r='2.4' fill='#FFFFFF'/>" TAIL },
{ "c-mesh", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#2FA49F'/>"
  "<path d='M12 5.5 18.5 9v6L12 18.5 5.5 15V9z' fill='#FFFFFF'/>"
  "<path d='M12 8.2 15.9 10.4v4.2L12 16.8 8.1 14.6v-4.2z' fill='#2FA49F'/>" TAIL },
{ "c-collider", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#4CAF50'/>"
  "<path d='M5.5 5.5h13v13h-13z' fill='#FFFFFF'/>"
  "<path d='M7.6 7.6h8.8v8.8H7.6z' fill='#4CAF50'/>" TAIL },
{ "c-body", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#6C63C7'/>"
  "<circle cx='12' cy='9' r='3.2' fill='#FFFFFF'/>"
  "<path d='M12 13.5 16 19H8z' fill='#FFFFFF'/>" TAIL },
{ "c-script", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#D9A441'/>"
  "<path d='M10.2 7.4 6.2 12l4 4.6 1.5-1.3L8.7 12l3-3.3z' fill='#FFFFFF'/>"
  "<path d='M13.8 7.4 17.8 12l-4 4.6-1.5-1.3 2.9-3.3-2.9-3.3z' fill='#FFFFFF'/>" TAIL },
{ "c-camera", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#5A6B8C'/>"
  "<rect x='5.4' y='8.4' width='9.4' height='7.4' rx='1.4' fill='#FFFFFF'/>"
  "<path d='M15.6 11 19 8.8v6.6L15.6 13z' fill='#FFFFFF'/>" TAIL },
{ "c-light", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#E0B33A'/>"
  "<circle cx='12' cy='11' r='4' fill='#FFFFFF'/>"
  "<rect x='9.8' y='15.4' width='4.4' height='1.5' rx='0.7' fill='#FFFFFF'/>"
  "<rect x='10.4' y='17.5' width='3.2' height='1.4' rx='0.7' fill='#FFFFFF'/>" TAIL },
{ "c-audio", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#D97C41'/>"
  "<path d='M11 6.5 7 10H5v4h2l4 3.5z' fill='#FFFFFF'/>"
  "<rect x='13.2' y='9.4' width='1.6' height='5.2' rx='0.8' fill='#FFFFFF'/>"
  "<rect x='16.2' y='7.4' width='1.6' height='9.2' rx='0.8' fill='#FFFFFF'/>" TAIL },
{ "c-material", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#B85CC7'/>"
  "<circle cx='12' cy='12' r='6' fill='#FFFFFF'/>"
  "<circle cx='9.8' cy='9.8' r='1.7' fill='#B85CC7'/>" TAIL },
{ "c-sprite", FILL_HEAD
  "<rect x='2' y='2' width='20' height='20' rx='5' fill='#3FA9D9'/>"
  "<rect x='5.6' y='5.6' width='12.8' height='12.8' rx='1.6' fill='#FFFFFF'/>"
  "<circle cx='9.2' cy='9.2' r='1.5' fill='#3FA9D9'/>"
  "<path d='M18.4 15.6 14 11.2l-8.4 7.2h12.8z' fill='#3FA9D9'/>" TAIL },
{ "folder", STROKE_HEAD"""
s = sub1(s, '{ "folder", STROKE_HEAD', NEW, 'icons new set')
wr('src/dai_icons.cpp', s)

# ======================================================= dai_editor_ui.cpp
s = rd('src/dai_editor_ui.cpp')
for old, new, what in [
    ('dai_ui_header_icon(p->ui, DAI_ICON_MOVE, "Transform"',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_TRANSFORM, "Transform"', 'hdr transform'),
    ('dai_ui_header_icon(p->ui, visible ? DAI_ICON_EYE : DAI_ICON_EYE_OFF, "Mesh Renderer"',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_MESH, "Mesh Renderer"', 'hdr mesh'),
    ('dai_ui_header_icon(p->ui, DAI_ICON_BOX, collider_title(r.shape)',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_COLLIDER, collider_title(r.shape)', 'hdr collider'),
    ('dai_ui_header_icon(p->ui, DAI_ICON_SETTINGS, "Rigidbody"',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_BODY, "Rigidbody"', 'hdr body'),
    ('dai_ui_header_icon(p->ui, DAI_ICON_SCRIPT, label.c_str()',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_SCRIPT, label.c_str()', 'hdr script'),
    ('dai_ui_header_icon(p->ui, DAI_ICON_CAMERA, "Camera"',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_CAMERA, "Camera"', 'hdr camera'),
    ('dai_ui_header_icon(p->ui, DAI_ICON_SUN, "Light"',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_LIGHT, "Light"', 'hdr light'),
    ('dai_ui_header_icon(p->ui, DAI_ICON_IMAGE, "Sprite"',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_SPRITE, "Sprite"', 'hdr sprite'),
    ('dai_ui_header_icon(p->ui, DAI_ICON_AUDIO, "Audio Source"',
     'dai_ui_header_icon(p->ui, DAI_ICON_C_AUDIO, "Audio Source"', 'hdr audio'),
    ('dai_ui_array_object_row(p->ui, (int)mi, mats[mi].c_str(),\n                                                DAI_ICON_MATERIAL)',
     'dai_ui_array_object_row(p->ui, (int)mi, mats[mi].c_str(),\n                                                DAI_ICON_C_MATERIAL)', 'mat row icon'),
]:
    s = sub1(s, old, new, what)

# ---- where a new object lands -------------------------------------------
# Every primitive was created at 0, 0.5, 0. Make a cube and a sphere from the
# menu and they are inside each other at the same spot - which reads as "the
# sphere and the cube do not collide", and with nothing under them both just
# fall together for ever. New objects land in front of the camera, and never
# on top of something that is already there.
s = sub1(s,
"""            std::snprintf(r.name, sizeof(r.name), "%s", names[cpick]);
            r.shape = shapes[cpick];
            r.motion = DAI_DYNAMIC;
            r.half_extent = { 0.5f, 0.5f, 0.5f };
            r.position = { 0, 0.5f, 0 };""",
"""            std::snprintf(r.name, sizeof(r.name), "%s", names[cpick]);
            r.shape = shapes[cpick];
            r.motion = DAI_DYNAMIC;
            r.half_extent = { 0.5f, 0.5f, 0.5f };
            r.position = spawn_point(p, d, 0.5f);""",
'spawn primitive')

s = sub1(s,
"""    if (cpick >= 0) {
        dai_node_desc r = dai_node_desc_default();""",
"""    if (cpick >= 0) {
        dai_node_desc r = dai_node_desc_default();
        (void)0;""",
'spawn marker')

# the helper itself, right above the create menu handler
s = sub1(s,
"""// ---- the project window, Unity's two column browser -------------------------""",
"""// Where a newly created object goes. Two rules, both of them Unity's: in
// FRONT of the camera, not at the origin behind you, and never inside
// something that is already there - two primitives at the same coordinates
// look exactly like two primitives that refuse to collide.
static dai_vec3 spawn_point(dai_editor_ui *p, dai_doc *d, float half_y) {
    dai_vec3 o{ 0, 0, 0 }, dir{ 0, 0, -1 };
    if (p->view_w > 1.0f && p->view_h > 1.0f)
        dai_editor_ray(p->ed, p->view_x + p->view_w * 0.5f,
                       p->view_y + p->view_h * 0.5f, &o, &dir);
    // The ground plane if the camera is looking down at it, eight metres out
    // otherwise - the same answer Unity gives when you drop a cube in.
    float t = 8.0f;
    if (dir.y < -0.05f) {
        float tg = (half_y - o.y) / dir.y;
        if (tg > 0.5f && tg < 60.0f) t = tg;
    }
    dai_vec3 pos{ o.x + dir.x * t, o.y + dir.y * t, o.z + dir.z * t };
    if (pos.y < half_y) pos.y = half_y;
    // Round to a tenth so the numbers in the inspector are readable.
    pos.x = (float)((int)(pos.x * 10.0f + (pos.x < 0 ? -0.5f : 0.5f))) * 0.1f;
    pos.z = (float)((int)(pos.z * 10.0f + (pos.z < 0 ? -0.5f : 0.5f))) * 0.1f;

    // Step aside until the spot is free. Sixteen tries, then take it anyway:
    // a create that silently does nothing would be worse than an overlap.
    uint32_t n = dai_doc_count(d);
    std::vector<dai_node> ids(n);
    if (n) dai_doc_nodes(d, ids.data(), n);
    for (int attempt = 0; attempt < 16; ++attempt) {
        bool clash = false;
        for (dai_node id : ids) {
            dai_node_desc e{};
            if (dai_doc_get(d, id, &e) != DAI_OK) continue;
            if (e.no_body && e.hidden) continue;         // empties do not block
            dai_vec3 wp{};
            dai_quat wr{ 0, 0, 0, 1 };
            dai_vec3 ws{ 1, 1, 1 };
            if (dai_doc_world_transform(d, id, &wp, &wr, &ws) != DAI_OK) wp = e.position;
            float dx = wp.x - pos.x, dy = wp.y - pos.y, dz = wp.z - pos.z;
            if (dx * dx + dy * dy + dz * dz < 1.1f * 1.1f) { clash = true; break; }
        }
        if (!clash) break;
        pos.x += 1.3f;
    }
    return pos;
}

// ---- the project window, Unity's two column browser -------------------------""",
'spawn helper')
wr('src/dai_editor_ui.cpp', s)

# ============================================================== dai_dock.cpp
# The drop zones were a third of the panel, which sounds generous until the
# panel is a 200 px inspector: the centre "make me a tab" zone then covers
# most of it and dropping the console UNDER the inspector was a pixel hunt.
s = rd('src/dai_dock.cpp')
s = sub1(s,
"""const float DROP_ZONE_FRAC = 0.34f;
const float DROP_ZONE_MAX  = 160.0f;""",
"""const float DROP_ZONE_FRAC = 0.42f;
const float DROP_ZONE_MAX  = 220.0f;""",
'drop zone size')

# ...and say which of the five it is going to be, in words. A translucent
# rectangle is not an answer when four of them look alike.
s = sub1(s,
"""    dai_ui_rect(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w, d->drag_preview.h, tint);
    dai_ui_rect_outline(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w,
                        d->drag_preview.h, 2.0f, st->accent);""",
"""    dai_ui_rect(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w, d->drag_preview.h, tint);
    dai_ui_rect_outline(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w,
                        d->drag_preview.h, 2.0f, st->accent);
    // The tab being carried, and what will happen when it is let go. Four of
    // the five previews are rectangles of the same colour; only a word tells
    // "as a tab" from "underneath".
    {
        const char *what = d->drag_kind == 0 ? "as a tab"
                         : d->drag_kind == 1 ? "left of"
                         : d->drag_kind == 2 ? "right of"
                         : d->drag_kind == 3 ? "above"
                         : d->drag_kind == 4 ? "below"
                         : "as a window";
        char note[128];
        std::snprintf(note, sizeof(note), "%s  -  %s", d->drag_title.c_str(), what);
        float mx2 = 0, my2 = 0;
        dai_ui_mouse(ui, &mx2, &my2, nullptr, nullptr);
        float tw = dai_ui_text_width(ui, note) + 14.0f;
        float th = dai_ui_text_height(ui) + 8.0f;
        dai_ui_rrect(ui, mx2 + 14.0f, my2 + 10.0f, tw, th, 4.0f, 0xF01E1E1Eu);
        dai_ui_rect_outline(ui, mx2 + 14.0f, my2 + 10.0f, tw, th, 1.0f, st->accent);
        dai_ui_text(ui, mx2 + 21.0f, my2 + 14.0f, note, st->text);
    }""",
'drop preview label')
wr('src/dai_dock.cpp', s)

# ================================================================== dai_tr
s = rd('src/dai_tr.cpp')
s = sub1(s, '    { "Materials", "Materialien" },',
"""    { "Materials", "Materialien" },
    { "Static", "Statisch" },
    { "as a tab", "als Tab" },
    { "left of", "links davon" },
    { "right of", "rechts davon" },
    { "above", "darüber" },
    { "below", "darunter" },
    { "as a window", "als Fenster" },
    { "Create: JS Script", "Neu: JS-Skript" },
    { "Create: C++ Behaviour", "Neu: C++-Verhalten" },
    { "Create: Folder", "Neu: Ordner" },
    { "Open in VS Code", "In VS Code öffnen" },
    { "prefab placed", "Prefab platziert" },
    { "prefab saved", "Prefab gespeichert" },""",
'tr additions')
wr('src/dai_tr.cpp', s)
print('patch31 ok')
