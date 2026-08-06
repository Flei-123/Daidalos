import io

# ===========================================================================
# 1) Die Text-Box: feste Groesse, Umbruch, und Autosize.
# 2) Die Sprite-Komponente TAT NICHTS. Jetzt ist sie ein Bild - im HUD.
# ===========================================================================
p = 'include/dai_doc.h'
s = io.open(p, encoding='utf-8').read()
old = """    char     text_font[96];"""
new = """    char     text_font[96];
    /* The box the text lives in, in pixels. 0,0 means "as wide as the words",
     * which is what a score wants; a real width is what a subtitle wants,
     * because a line that runs off the screen is not a subtitle.
     *
     * With a box, the text WRAPS at its width. With `text_autosize` on, it
     * also shrinks until it fits the height - the thing every UI toolkit
     * eventually grows, because a translated string is never the length the
     * layout was drawn for. German is famously a third longer than English,
     * and the box does not get bigger when it is. */
    float    text_w, text_h;
    int      text_autosize;     /* 1 = shrink to fit the box                 */

    /* ---- Image: the game's UI, in pictures --------------------------------
     * A screen space sprite, anchored exactly like Text. `image` is a project
     * path to a .png; `image_w/h` is its size in pixels (0 = the file's own).
     *
     * This is what the old `sprite` flag was supposed to be. It set a bit that
     * NOTHING read - not the sync layer, not the scene, not the renderer -
     * so a Sprite component was a checkbox with no effect for as long as it
     * has existed. */
    int      image_on;
    char     image[96];
    float    image_w, image_h;
    dai_vec3 image_color;       /* tint, 0,0,0 -> white                      */
    int      image_anchor;      /* same 3x3 grid as the text                 */
    float    image_x, image_y;"""
assert s.count(old) == 1, 'text_font field not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_doc_text.cpp'
s = io.open(p, encoding='utf-8').read()
old = """        if (r.text_font[0])                   put(s, "  textfont %s\\n", r.text_font);"""
new = """        if (r.text_font[0])                   put(s, "  textfont %s\\n", r.text_font);
        if (!feq(r.text_w, def.text_w) || !feq(r.text_h, def.text_h))
            put(s, "  textbox %s %s\\n", fstr(r.text_w).c_str(), fstr(r.text_h).c_str());
        if (r.text_autosize != def.text_autosize) put(s, "  textfit %d\\n", r.text_autosize);
        if (r.image_on != def.image_on)       put(s, "  image %d\\n", r.image_on);
        if (r.image[0])                       put(s, "  imagefile %s\\n", r.image);
        if (!feq(r.image_w, def.image_w) || !feq(r.image_h, def.image_h))
            put(s, "  imagesize %s %s\\n", fstr(r.image_w).c_str(), fstr(r.image_h).c_str());
        if (!feq(r.image_color.x, def.image_color.x) || !feq(r.image_color.y, def.image_color.y) ||
            !feq(r.image_color.z, def.image_color.z))
            put(s, "  imagecol %s %s %s\\n", fstr(r.image_color.x).c_str(),
                fstr(r.image_color.y).c_str(), fstr(r.image_color.z).c_str());
        if (r.image_anchor != def.image_anchor) put(s, "  imageanchor %d\\n", r.image_anchor);
        if (!feq(r.image_x, def.image_x) || !feq(r.image_y, def.image_y))
            put(s, "  imagepos %s %s\\n", fstr(r.image_x).c_str(), fstr(r.image_y).c_str());"""
assert s.count(old) == 1, 'textfont writer not found'
s = s.replace(old, new)

old = """        else if (key == "textfont") {"""
new = """        else if (key == "textbox")    { ok = parse_floats(after, &rec.text_w, 2); }
        else if (key == "textfit")    { ok = parse_i32(after, &rec.text_autosize); }
        else if (key == "image")      { ok = parse_i32(after, &rec.image_on); }
        else if (key == "imagesize")  { ok = parse_floats(after, &rec.image_w, 2); }
        else if (key == "imagecol")   { ok = parse_floats(after, &rec.image_color.x, 3); }
        else if (key == "imageanchor"){ ok = parse_i32(after, &rec.image_anchor); }
        else if (key == "imagepos")   { ok = parse_floats(after, &rec.image_x, 2); }
        else if (key == "imagefile") {
            std::string v = after;
            while (!v.empty() && (v.front() == ' ' || v.front() == '\\t')) v.erase(0, 1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\\t' || v.back() == '\\r')) v.pop_back();
            if (v.size() >= sizeof(rec.image)) ok = false;
            else std::snprintf(rec.image, sizeof(rec.image), "%s", v.c_str()); }
        else if (key == "textfont") {"""
assert s.count(old) == 1, 'textfont reader not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('doc: text box, autosize, image')
