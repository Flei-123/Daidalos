import io

p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ---------------------------------------------------------------------------
# Der Zustand, den die neue Console braucht: welche Zeile ausgewaehlt ist und
# wie hoch das Detail-Feld darunter steht.
# ---------------------------------------------------------------------------
old = """    int  log_collapse = 1;
    float log_scroll = 0.0f;"""
new = """    int  log_collapse = 1;
    float log_scroll = 0.0f;
    // Unity's console is two panels: the list, and the full text of the ONE
    // line you clicked. Messages are longer than a row - a script error
    // carries a file, a line and a reason - and a list that clips them is a
    // list you have to copy out of to read.
    int   log_sel = -1;            // index into `log`, -1 = nothing picked
    float log_detail = 0.0f;       // height of the detail pane, 0 = closed
    float log_detail_scroll = 0.0f;"""
assert s.count(old) == 1, 'log state not found'
s = s.replace(old, new)

# ---------------------------------------------------------------------------
# Die Zeilen selbst: Icon, zweizeiliger Text, Zaehler rechts, Auswahl-Balken.
# ---------------------------------------------------------------------------
old = """    const float ROW = 17.0f;
    float ly = py + BAR + 3.0f;
    float mx = 0, my = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, nullptr);
    if (mx >= px && mx < px + pw && my >= py && my < py + ph)
        p->log_scroll -= dai_ui_wheel(ui) * 32.0f;
    float total = 0.0f;
    for (const auto &l : p->log) if (p->log_show[l.level]) total += ROW;
    float maxs = total - (ph - BAR - 6.0f);
    if (maxs < 0.0f) maxs = 0.0f;
    if (p->log_scroll > maxs) p->log_scroll = maxs;
    if (p->log_scroll < 0.0f) p->log_scroll = 0.0f;

    dai_ui_clip_begin(ui, px, py + BAR + 1.0f, pw, ph - BAR - 1.0f);
    float ry = ly - p->log_scroll + 6.0f;   // air under the buttons - it read as stuck-on
    if (p->log.empty())
        dai_ui_text(ui, px + 8.0f, ry, "no messages - script print() and engine warnings land here", st->text_dim);
    int ddown = 0, dpressed = 0;
    float dmx = 0, dmy = 0;
    dai_ui_mouse(ui, &dmx, &dmy, &ddown, &dpressed);
    for (const auto &l : p->log) {
        if (!p->log_show[l.level]) continue;
        if (ry + ROW > py + BAR && ry < py + ph) {
            char line[400];
            if (l.count > 1) std::snprintf(line, sizeof(line), "(%u) %s", l.count, l.text.c_str());
            else             std::snprintf(line, sizeof(line), "%s", l.text.c_str());
            // A click copies the line - an error message you cannot copy is a
            // search you have to type by hand.
            if (dpressed && dmx >= px && dmx < px + pw && dmy >= ry && dmy < ry + ROW) {
                dai_editor_ui_clipboard_set(p, 0, line);
                dai_editor_ui_toast(p, "copied", 1.0f);
            }
            dai_ui_text(ui, px + 8.0f, ry, line, LEVEL_COL[l.level]);
        }
        ry += ROW;
    }
    dai_ui_clip_end(ui);
}"""

new = """    // ---- the list, Unity's shape ------------------------------------------
    // A row is two text lines tall with the level's icon on the left and the
    // repeat count on the right: the first line is the message, the second is
    // where it came from. One click SELECTS (it used to copy, which meant the
    // clipboard changed every time you tried to read something), and the
    // selected message is written out in full underneath.
    const float TH = dai_ui_text_height(ui);
    const float ROW = TH * 2.0f + 9.0f;
    float mx = 0, my = 0;
    int ddown = 0, dpressed = 0;
    dai_ui_mouse(ui, &mx, &my, &ddown, &dpressed);
    bool inside = mx >= px && mx < px + pw && my >= py && my < py + ph;

    // The detail pane takes the bottom third, but never more than half and
    // never so much that fewer than two rows are left to pick from.
    float detail_h = 0.0f;
    if (p->log_sel >= 0 && p->log_sel < (int)p->log.size()) {
        detail_h = (ph - BAR) * 0.34f;
        float max_h = ph - BAR - ROW * 2.0f - 6.0f;
        if (detail_h > max_h) detail_h = max_h;
        if (detail_h < TH * 3.0f) detail_h = 0.0f;    // no room: list wins
    }
    p->log_detail = detail_h;
    const float LIST_H = ph - BAR - detail_h;

    if (inside && my < py + BAR + LIST_H) p->log_scroll -= dai_ui_wheel(ui) * 40.0f;
    float total = 0.0f;
    for (const auto &l : p->log) if (p->log_show[l.level]) total += ROW;
    float maxs = total - (LIST_H - 6.0f);
    if (maxs < 0.0f) maxs = 0.0f;
    if (p->log_scroll > maxs) p->log_scroll = maxs;
    if (p->log_scroll < 0.0f) p->log_scroll = 0.0f;

    dai_ui_clip_begin(ui, px, py + BAR + 1.0f, pw, LIST_H - 1.0f);
    float ry = py + BAR + 4.0f - p->log_scroll;
    if (p->log.empty())
        dai_ui_text(ui, px + 8.0f, ry + 2.0f,
                    "no messages - script print() and engine warnings land here", st->text_dim);
    static const char *ROW_ICON[3] = { DAI_ICON_INFO, DAI_ICON_WARNING, DAI_ICON_ERROR };
    int click_at = -1;
    for (size_t li = 0; li < p->log.size(); ++li) {
        const auto &l = p->log[li];
        if (!p->log_show[l.level]) continue;
        if (ry + ROW > py + BAR && ry < py + BAR + LIST_H) {
            bool sel = (int)li == p->log_sel;
            bool over = inside && mx < px + pw && my >= ry && my < ry + ROW &&
                        my < py + BAR + LIST_H;
            // Alternating bands, the way every console does it: the eye needs
            // something to follow across a wide panel.
            if (sel)        dai_ui_rect(ui, px, ry, pw, ROW, st->button_active);
            else if (over)  dai_ui_rect(ui, px, ry, pw, ROW, st->button);
            else if (li & 1) dai_ui_rect(ui, px, ry, pw, ROW, st->track);

            if (dai_ui_has_icon(ui, ROW_ICON[l.level]))
                dai_ui_icon_at(ui, ROW_ICON[l.level], px + 7.0f,
                               ry + (ROW - 16.0f) * 0.5f, 16.0f, 0xFFFFFFFFu);

            // The message is one line here however long it is; the rest of it
            // is what the pane below is for. Split on the first newline, so a
            // two part message shows its second part as the context line.
            std::string first = l.text, secondl;
            size_t nl = first.find('\\n');
            if (nl != std::string::npos) { secondl = first.substr(nl + 1); first = first.substr(0, nl); }
            size_t nl2 = secondl.find('\\n');
            if (nl2 != std::string::npos) secondl = secondl.substr(0, nl2);

            float tx = px + 29.0f;
            float tw_avail = pw - 29.0f - 44.0f;
            dai_ui_clip_begin(ui, tx, ry, tw_avail, ROW);
            dai_ui_text(ui, tx, ry + 4.0f, first.c_str(),
                        sel ? 0xFFFFFFFFu : LEVEL_COL[l.level]);
            if (!secondl.empty())
                dai_ui_text(ui, tx, ry + 4.0f + TH + 1.0f, secondl.c_str(), st->text_dim);
            dai_ui_clip_end(ui);

            // The repeat badge, right hand end - "this happened 47 times" is
            // the difference between a bug and a loop.
            if (l.count > 1) {
                char cb[24];
                std::snprintf(cb, sizeof(cb), "%u", l.count);
                float cw = dai_ui_text_width(ui, cb) + 14.0f;
                float cx = px + pw - cw - 8.0f;
                dai_ui_rrect(ui, cx, ry + (ROW - TH - 6.0f) * 0.5f, cw, TH + 6.0f, 7.0f, st->titlebar);
                dai_ui_text(ui, cx + 7.0f, ry + (ROW - TH) * 0.5f, cb, st->text_dim);
            }
            if (dpressed && over && !dai_ui_popup_active(ui)) click_at = (int)li;
        }
        ry += ROW;
    }
    dai_ui_clip_end(ui);
    if (click_at >= 0) {
        // Clicking the selected row again closes the pane: the same key opens
        // and shuts, which is the only arrangement nobody has to be told.
        p->log_sel = (p->log_sel == click_at) ? -1 : click_at;
        p->log_detail_scroll = 0.0f;
    }

    // ---- the detail pane ---------------------------------------------------
    if (detail_h > 0.0f && p->log_sel >= 0 && p->log_sel < (int)p->log.size()) {
        float dy = py + BAR + LIST_H;
        dai_ui_rect(ui, px, dy, pw, 1.0f, st->panel_border);
        dai_ui_rect(ui, px, dy + 1.0f, pw, detail_h - 1.0f, st->track);
        const auto &l = p->log[(size_t)p->log_sel];

        // Copy sits in the pane, not the toolbar: what you want is THIS
        // message, and you want it right where you are reading it.
        float bw = dai_ui_text_width(ui, "Copy") + 18.0f;
        if (browser_button(p, px + pw - bw - 8.0f, dy + 5.0f, bw, TH + 8.0f, "Copy")) {
            dai_editor_ui_clipboard_set(p, 0, l.text.c_str());
            dai_editor_ui_toast(p, "copied", 1.0f);
        }

        if (inside && my >= dy) p->log_detail_scroll -= dai_ui_wheel(ui) * 40.0f;
        // Wrap the text to the pane, so a long line is readable instead of
        // running off the right edge into nothing.
        dai_ui_clip_begin(ui, px, dy + 1.0f, pw - bw - 14.0f, detail_h - 2.0f);
        float wrap_w = pw - bw - 26.0f;
        float ty = dy + 6.0f - p->log_detail_scroll;
        float used = 0.0f;
        std::string rest = l.text;
        while (!rest.empty()) {
            std::string lineone;
            size_t nl = rest.find('\\n');
            if (nl == std::string::npos) { lineone = rest; rest.clear(); }
            else { lineone = rest.substr(0, nl); rest = rest.substr(nl + 1); }
            // hard wrap on width, at a space where there is one
            for (;;) {
                if (dai_ui_text_width(ui, lineone.c_str()) <= wrap_w) {
                    dai_ui_text(ui, px + 8.0f, ty, lineone.c_str(), st->text);
                    ty += TH + 2.0f; used += TH + 2.0f;
                    break;
                }
                size_t cut = lineone.size();
                while (cut > 1 && dai_ui_text_width(ui, lineone.substr(0, cut).c_str()) > wrap_w) --cut;
                size_t sp = lineone.rfind(' ', cut);
                if (sp != std::string::npos && sp > cut / 2) cut = sp;
                dai_ui_text(ui, px + 8.0f, ty, lineone.substr(0, cut).c_str(), st->text);
                ty += TH + 2.0f; used += TH + 2.0f;
                lineone = lineone.substr(cut);
                while (!lineone.empty() && lineone[0] == ' ') lineone.erase(0, 1);
                if (lineone.empty()) break;
            }
        }
        dai_ui_clip_end(ui);
        float dmax = used - (detail_h - 12.0f);
        if (dmax < 0.0f) dmax = 0.0f;
        if (p->log_detail_scroll > dmax) p->log_detail_scroll = dmax;
        if (p->log_detail_scroll < 0.0f) p->log_detail_scroll = 0.0f;
    }
}"""
assert s.count(old) == 1, 'console list body not found'
s = s.replace(old, new)

# Clear muss die Auswahl mitnehmen, sonst zeigt das Detail-Feld eine Zeile,
# die es nicht mehr gibt.
old = """    if (browser_button(p, bx, py + 4.0f, 56.0f, BTN_H, "Clear")) dai_editor_ui_log_clear(p);"""
new = """    if (browser_button(p, bx, py + 4.0f, 56.0f, BTN_H, "Clear")) {
        dai_editor_ui_log_clear(p);
        p->log_sel = -1;              // or the detail pane outlives its message
    }"""
assert s.count(old) == 1, 'clear button not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('dai_editor_ui.cpp: console with icons, bands, counts and a detail pane')
