import io

# ===========================================================================
# 1) dai_ui_toggle_button spannte IMMER die ganze Panelbreite - "Edit Collider"
#    war deshalb noch riesig, obwohl Add Component schon sass.
# ===========================================================================
p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()
old = """int dai_ui_toggle_button(dai_ui *ui, const char *utf8, int active) {
    if (!ui || !utf8) return 0;
    float h = widget_height(ui), x, y;
    next_rect(ui, 0, h, &x, &y);
    float w = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);"""
new = """int dai_ui_toggle_button(dai_ui *ui, const char *utf8, int active) {
    if (!ui || !utf8) return 0;
    float h = widget_height(ui), x, y;
    // As wide as its label, like every other button here. Full width is what
    // a SECTION looks like: "Edit Collider" stretched across the inspector
    // read as a heading with a box drawn round it, not as something to press.
    float w = dai_ui_text_width(ui, utf8) + ui->style.padding * 3.0f;
    float avail = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    if (w > avail) w = avail;
    next_rect(ui, w, h, &x, &y);"""
assert s.count(old) == 1, 'toggle_button head not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_ui.cpp: toggle buttons are text wide')

# ===========================================================================
# 2) Datei-Icons duerfen Farbe tragen - dieselbe wie die Komponente, zu der
#    die Datei wird.
# ===========================================================================
p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

old = """// A file the engine can run on an object: QuickJS, or a native C++ behaviour"""
new = """// The colour that goes with the icon above. The component headers in the
// inspector are colour coded already - a script header is gold, a camera is
// pale blue - and a file in the Project window is the same thing before it is
// attached to anything. Grey-on-grey rows make you read every name; colour
// lets the eye find the material among forty textures without reading at all.
static uint32_t icon_color_for_asset(const std::string &path, uint32_t fallback) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return fallback;
    std::string e = path.substr(dot + 1);
    for (char &c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (e == "js" || e == "ts" || e == "cpp" || e == "cc" || e == "cxx" ||
        e == "h" || e == "hpp")                       return rgba(0xF2, 0xC1, 0x4E, 255);  // script gold
    if (e == "daimat")                                return rgba(0xC8, 0x8A, 0xE0, 255);  // material violet
    if (e == "daidalos" || e == "prefab")             return rgba(0x6C, 0xB2, 0xF0, 255);  // prefab blue
    if (e == "png" || e == "jpg" || e == "jpeg" || e == "tga" || e == "svg")
                                                      return rgba(0x6F, 0xCB, 0x9F, 255);  // texture green
    if (e == "glb" || e == "gltf" || e == "obj")      return rgba(0xE0, 0x93, 0x6C, 255);  // model amber
    if (e == "wav" || e == "ogg" || e == "mp3" || e == "flac")
                                                      return rgba(0xE8, 0x84, 0x9B, 255);  // audio rose
    return fallback;
}

// A file the engine can run on an object: QuickJS, or a native C++ behaviour"""
assert s.count(old) == 1, 'anchor for icon_color not found'
s = s.replace(old, new)

# browser_row nimmt eine Farbe entgegen
old = """static int browser_row(dai_editor_ui *p, float x, float y, float w, float h,
                       const char *icon, const char *label, int selected) {"""
new = """static int browser_row(dai_editor_ui *p, float x, float y, float w, float h,
                       const char *icon, const char *label, int selected,
                       uint32_t icon_col) {"""
assert s.count(old) == 1, 'browser_row signature not found'
s = s.replace(old, new)

old = """        dai_ui_icon_at(ui, icon, tx, y + (h - 13.0f) * 0.5f, 13.0f,
                       selected ? st->text : st->text_dim);"""
new = """        // On a selected row the fill is already the accent colour; a hue on
        // top of it fights the fill instead of naming the file, so the icon
        // goes plain white there and keeps its colour everywhere else.
        dai_ui_icon_at(ui, icon, tx, y + (h - 13.0f) * 0.5f, 13.0f,
                       selected ? 0xFFFFFFFFu : (icon_col ? icon_col : st->text_dim));"""
assert s.count(old) == 1, 'browser_row icon draw not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_editor_ui.cpp: browser rows carry a colour')
