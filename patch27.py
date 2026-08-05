# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# =========================================================== dai_ui.cpp
P = 'src/dai_ui.cpp'
s = rw(P)

# ---- BUG 1: spacing always moved DOWN, even inside a row ------------------
# Inside dai_ui_row the layout runs sideways, so "leave 200 px" has to leave
# them sideways too. It did not, which pushed the Materials header and its
# +/- buttons a row DOWN instead of to the right - they landed on top of the
# next widget.
s = sub1(s,
"""void dai_ui_spacing(dai_ui *ui, float px) { if (ui) ui->cursor_y += px; }""",
"""void dai_ui_spacing(dai_ui *ui, float px) {
    if (!ui) return;
    // A row runs sideways: leaving space in one has to leave it sideways too.
    // Always adding to cursor_y pushed everything after the gap onto the next
    // line instead of along the current one - which is what made the
    // Materials header and its +/- buttons land on top of their neighbours.
    if (ui->in_row) ui->cursor_x += px;
    else            ui->cursor_y += px;
}""", "spacing row-aware")

# ---- BUG 2: field widgets ignored the row -------------------------------
# field_rect always took the FULL panel width, so two fields in one row were
# drawn on top of each other and the second one ran off the right edge.
s = sub1(s,
"""void field_rect(dai_ui *ui, const char *label, float *x, float *y, float *w, float h) {
    float rx, ry;
    next_rect(ui, 0, h, &rx, &ry);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);""",
"""void field_rect(dai_ui *ui, const char *label, float *x, float *y, float *w, float h) {
    float rx, ry;
    next_rect(ui, 0, h, &rx, &ry);
    float full = (ui->in_panel ? ui->panel_w - ui->style.padding * 2 : ui->width);
    // Inside a row, "full width" means "what is LEFT of the row", not the
    // whole panel: the second field of a row was drawn at full width from
    // where the first one ended, i.e. mostly off the right edge.
    if (ui->in_row) {
        float left_edge = (ui->in_panel ? ui->panel_x + ui->style.padding : 0.0f);
        float used = rx - left_edge;
        full -= used;
        if (full < 24.0f) full = 24.0f;
    }""", "field_rect row-aware")

# ---- BUG 3: the scroll region corrected itself one frame too late ---------
# The wheel was applied in scroll_begin and only clamped in scroll_end - so a
# panel whose content fits was drawn SHIFTED for one frame and snapped back on
# the next. That is exactly "scrolling does nothing but flicker".
s = sub1(s,
"""    struct ScrollFrame { uint64_t id; float x, y, w, h, start_y; };
    std::vector<ScrollFrame> scroll_stack;""",
"""    struct ScrollFrame { uint64_t id; float x, y, w, h, start_y; };
    std::vector<ScrollFrame> scroll_stack;
    // How far the content of each region reached LAST frame. The wheel needs
    // it before the content is laid out; without it a region can only find
    // out it should not have scrolled after it already has.
    std::vector<std::pair<uint64_t, float>> scroll_max;
    float &scroll_max_of(uint64_t id) {
        for (auto &e : scroll_max) if (e.first == id) return e.second;
        scroll_max.push_back({ id, 0.0f });
        return scroll_max.back().second;
    }""", "scroll_max store")

s = sub1(s,
"""    if (inside_chk(ui, x, y, w, height)) {
        ui->mouse_over_ui = true;
        if (ui->input.wheel != 0.0f) off -= ui->input.wheel * 32.0f;
    }
    if (off < 0.0f) off = 0.0f;""",
"""    // Only scroll what CAN scroll. The limit is last frame's - one frame of
    // lag on a value that only changes when the panel's contents change, and
    // the alternative is a visible jump every time the wheel is touched over
    // a list that already fits.
    float limit = ui->scroll_max_of(id);
    if (inside_chk(ui, x, y, w, height)) {
        ui->mouse_over_ui = true;
        if (ui->input.wheel != 0.0f && limit > 0.0f) off -= ui->input.wheel * 32.0f;
    }
    if (off < 0.0f) off = 0.0f;
    if (off > limit) off = limit;""", "scroll wheel gated")

s = sub1(s,
"""    float &off = ui->scroll_of(f.id);
    float content = (ui->cursor_y + off) - f.start_y;
    float max_off = content - f.h;
    if (max_off < 0.0f) max_off = 0.0f;
    if (off > max_off) off = max_off;

    // No dead travel: while the content is shorter than the region, any
    // scroll offset is invalid - so the wheel does nothing instead of
    // scrolling into empty space and being corrected next frame.
    if (content < f.h) off = 0.0f;""",
"""    float &off = ui->scroll_of(f.id);
    float content = (ui->cursor_y + off) - f.start_y;
    float max_off = content - f.h;
    if (max_off < 0.0f) max_off = 0.0f;
    if (off > max_off) off = max_off;
    // What the NEXT frame's wheel is allowed to do.
    ui->scroll_max_of(f.id) = max_off;""", "scroll_end stores max")
wr(P, s)

# ==================================================== dai_editor_ui.cpp
P = 'src/dai_editor_ui.cpp'
s = rw(P)

# ---- BUG 4: the name was written back every single frame -----------------
# r.name = name_buf ran unconditionally, so a rename made anywhere else (F2 in
# the hierarchy, an undo, reloading the scene) was overwritten by the
# inspector's stale buffer on the very next frame.
s = sub1(s,
"""    if (p->name_buf_node != n) {
        std::snprintf(p->name_buf, sizeof(p->name_buf), "%s", r.name);
        p->name_buf_node = n;
    }""",
"""    // Refreshed when the DOCUMENT's name no longer matches the buffer and the
    // field is not being typed into: a rename from the hierarchy or an undo
    // has to reach the inspector, and the buffer must not fight the user's
    // keystrokes while it does.
    if (p->name_buf_node != n ||
        (std::strcmp(p->name_buf, r.name) != 0 && !dai_ui_text_active(p->ui))) {
        std::snprintf(p->name_buf, sizeof(p->name_buf), "%s", r.name);
        p->name_buf_node = n;
    }""", "name buffer refresh")

s = sub1(s,
"""        dai_ui_text_field(ui2, "objname", hx + 48.0f, hy + 4.0f, hw - 56.0f, 18.0f,
                          p->name_buf, sizeof(p->name_buf), nullptr);
        std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);""",
"""        // Written back only when it actually changed, or every frame would
        // count as an edit and every rename from elsewhere would be undone.
        if (dai_ui_text_field(ui2, "objname", hx + 48.0f, hy + 4.0f, hw - 56.0f, 18.0f,
                              p->name_buf, sizeof(p->name_buf), nullptr))
            std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);""",
"name write on change")

# ---- the tag/asset line: one field per line, no row ----------------------
s = sub1(s,
"""    dai_ui_row(p->ui, 18.0f);
    {
        float pw2 = dai_ui_panel_width(p->ui) - dai_ui_style_of(p->ui)->padding * 2;
        if (r.asset[0]) {
            if (dai_ui_input_text(p->ui, "Tag", p->tag_buf, sizeof(p->tag_buf)))
                std::snprintf(r.tag, sizeof(r.tag), "%s", p->tag_buf);
            if (dai_ui_input_text(p->ui, "Asset", p->asset_buf, sizeof(p->asset_buf)))
                std::snprintf(r.asset, sizeof(r.asset), "%s", p->asset_buf);
        } else {
            if (dai_ui_input_text(p->ui, "Tag", p->tag_buf, sizeof(p->tag_buf)))
                std::snprintf(r.tag, sizeof(r.tag), "%s", p->tag_buf);
            (void)pw2;
        }
    }
    dai_ui_row_end(p->ui);
    dai_ui_separator(p->ui);""",
"""    // Tag on its own line, asset only when there is one. Two full width
    // fields squeezed into one row is how the asset field ended up off the
    // right edge of the panel.
    if (dai_ui_input_text(p->ui, "Tag", p->tag_buf, sizeof(p->tag_buf)))
        std::snprintf(r.tag, sizeof(r.tag), "%s", p->tag_buf);
    if (r.asset[0]) {
        if (dai_ui_input_text(p->ui, "Asset", p->asset_buf, sizeof(p->asset_buf)))
            std::snprintf(r.asset, sizeof(r.asset), "%s", p->asset_buf);
    }
    dai_ui_separator(p->ui);""", "tag line")

# ---- the materials array: laid out by hand, not by a row + spacing --------
s = sub1(s,
"""            // Header: "Materials" left, size field right.
            dai_ui_row(p->ui, 20.0f);
            float hdr_w = dai_ui_panel_width(p->ui) - dai_ui_style_of(p->ui)->padding * 2 - 46.0f;
            dai_ui_spacing(p->ui, hdr_w - dai_ui_text_width(p->ui, "Materials"));
            dai_ui_label(p->ui, "Materials");
            float mcount = (float)mats.size();
            if (dai_ui_num_field(p->ui, "", &mcount, 1.0f, 1.0f, 7.0f, "matsize")) {
                size_t want = (size_t)(mcount + 0.5f);
                while (mats.size() < want) mats.push_back("Default");
                while (mats.size() > want) mats.pop_back();
                script_join(r.materials, sizeof(r.materials), mats);
            }
            dai_ui_row_end(p->ui);""",
"""            // Header: "Materials" on the left, the size on the right - placed
            // by hand, because a row of full width widgets is not a layout.
            {
                dai_ui *mu = p->ui;
                const dai_ui_style *ms = dai_ui_style_of(mu);
                float ax, ay;
                dai_ui_cursor_pos(mu, &ax, &ay);
                dai_ui_advance(mu, 0, 20.0f);
                float aw = dai_ui_panel_width(mu) - ms->padding * 2;
                dai_ui_text(mu, ax, ay + 3.0f, "Materials", ms->text);
                char cnt[8];
                std::snprintf(cnt, sizeof(cnt), "%d", (int)mats.size());
                float bw2 = 44.0f;
                dai_ui_rrect(mu, ax + aw - bw2, ay + 1.0f, bw2, 17.0f, 3.0f, ms->track);
                dai_ui_rect_outline(mu, ax + aw - bw2, ay + 1.0f, bw2, 17.0f, 1.0f, ms->panel_border);
                dai_ui_text(mu, ax + aw - bw2 + 6.0f, ay + 3.0f, cnt, ms->text);
            }""", "materials header")

s = sub1(s,
"""            // + / - on the right, under the list.
            dai_ui_row(p->ui, 20.0f);
            float btn_w = 26.0f;
            float pad = dai_ui_panel_width(p->ui) - dai_ui_style_of(p->ui)->padding * 2
                      - btn_w * 2 - dai_ui_style_of(p->ui)->spacing;
            if (pad > 0) dai_ui_spacing(p->ui, pad);
            if (dai_ui_button(p->ui, "+") && mats.size() < 7) {
                mats.push_back("Default");
                script_join(r.materials, sizeof(r.materials), mats);
            }
            if (dai_ui_button(p->ui, "-") && mats.size() > 1) {
                mats.pop_back();
                script_join(r.materials, sizeof(r.materials), mats);
            }
            dai_ui_row_end(p->ui);""",
"""            // + / - bottom right, under the list. Placed, not laid out.
            {
                dai_ui *mu = p->ui;
                float ax, ay;
                dai_ui_cursor_pos(mu, &ax, &ay);
                dai_ui_advance(mu, 0, 20.0f);
                float aw = dai_ui_panel_width(mu) - dai_ui_style_of(mu)->padding * 2;
                float bw2 = 24.0f;
                if (dai_ui_icon_button_at(mu, DAI_ICON_PLUS, ax + aw - bw2 * 2 - 3.0f, ay,
                                          bw2, 18.0f, 0) && mats.size() < 7) {
                    mats.push_back("Default");
                    script_join(r.materials, sizeof(r.materials), mats);
                }
                if (dai_ui_icon_button_at(mu, DAI_ICON_MORE, ax + aw - bw2, ay, bw2, 18.0f, 0)
                    && mats.size() > 1) {
                    mats.pop_back();
                    script_join(r.materials, sizeof(r.materials), mats);
                }
            }""", "materials buttons")
wr(P, s)
print("patch27 done")
