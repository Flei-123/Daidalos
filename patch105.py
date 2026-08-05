import io

# ===========================================================================
# 1) Die Text-Komponente im Dokument.
# ===========================================================================
p = 'include/dai_doc.h'
s = io.open(p, encoding='utf-8').read()
old = """    char     audio_event[64];   /* AudioSource: event name in the sound bank    */"""
new = """    /* ---- Text: the game's own UI ---------------------------------------
     * On screen, not in the world. A node with one draws a label in the Game
     * view and in the exported game, anchored to a corner of the picture
     * rather than to a place in the scene - which is what a score, a timer or
     * a "press any key" is.
     *
     * `text` holds either the words or a KEY: a leading '@' means look it up
     * in the project's string table (see dai_strings.h). Both spellings work
     * everywhere, so a label starts as "Score" and becomes "@hud.score" the
     * day a second language exists, without anything else changing. */
    int      text_on;           /* 1 = this node draws a label                  */
    char     text[192];         /* the words, or "@key"                         */
    float    text_size;         /* pixels, 0 -> 24                              */
    dai_vec3 text_color;        /* 0,0,0 -> white, the same "unset" rule as the
                                   light colour                                */
    int      text_anchor;       /* 0..8, reading order: 0 top left, 4 centre,
                                   8 bottom right. An anchor rather than a
                                   position because the picture changes size
                                   and a score pinned to 1920 is off screen on
                                   a 1280 window.                              */
    float    text_x, text_y;    /* pixels from the anchor, + is right and down  */

    char     audio_event[64];   /* AudioSource: event name in the sound bank    */"""
assert s.count(old) == 1, 'audio_event field not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

# ===========================================================================
# 2) Das Textformat schreibt nur, was vom Default abweicht.
# ===========================================================================
p = 'src/dai_doc_text.cpp'
s = io.open(p, encoding='utf-8').read()
old = """        if (r.audio_event[0])            put(s, "  audio %s\\n", r.audio_event);"""
new = """        // Text. The words go last on their line, so they may contain spaces -
        // and they must, because "Press any key" is one string, not three.
        if (r.text_on != def.text_on)     put(s, "  text %d\\n", r.text_on);
        if (r.text[0])                    put(s, "  textstr %s\\n", r.text);
        if (!feq(r.text_size, def.text_size)) put(s, "  textsize %s\\n", fstr(r.text_size).c_str());
        if (!feq(r.text_color.x, def.text_color.x) || !feq(r.text_color.y, def.text_color.y) ||
            !feq(r.text_color.z, def.text_color.z))
            put(s, "  textcol %s %s %s\\n", fstr(r.text_color.x).c_str(),
                fstr(r.text_color.y).c_str(), fstr(r.text_color.z).c_str());
        if (r.text_anchor != def.text_anchor) put(s, "  textanchor %d\\n", r.text_anchor);
        if (!feq(r.text_x, def.text_x) || !feq(r.text_y, def.text_y))
            put(s, "  textpos %s %s\\n", fstr(r.text_x).c_str(), fstr(r.text_y).c_str());
        if (r.audio_event[0])            put(s, "  audio %s\\n", r.audio_event);"""
assert s.count(old) == 1, 'audio writer not found'
s = s.replace(old, new)

old = """        else if (key == "sprite")  { ok = parse_i32(after, &rec.sprite); }"""
new = """        else if (key == "text")    { ok = parse_i32(after, &rec.text_on); }
        else if (key == "textstr") {
            // Everything after the keyword, spaces included, trailing
            // whitespace off. A label is one value, not a word list.
            std::string v = after;
            while (!v.empty() && (v.back() == ' ' || v.back() == '\\t' || v.back() == '\\r')) v.pop_back();
            if (v.size() >= sizeof(rec.text)) ok = false;
            else std::snprintf(rec.text, sizeof(rec.text), "%s", v.c_str()); }
        else if (key == "textsize")   { ok = parse_floats(after, &rec.text_size, 1); }
        else if (key == "textcol")    { ok = parse_floats(after, &rec.text_color.x, 3); }
        else if (key == "textanchor") { ok = parse_i32(after, &rec.text_anchor); }
        else if (key == "textpos")    { ok = parse_floats(after, &rec.text_x, 2); }
        else if (key == "sprite")  { ok = parse_i32(after, &rec.sprite); }"""
assert s.count(old) == 1, 'sprite reader not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_doc: text component, written and read')
