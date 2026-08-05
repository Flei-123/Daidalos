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

# #42: Unity's object header, one line: active checkbox, the kind's icon, the
# name as a text field. The tag sits on the next line with the asset - both
# are secondary information and were spending a full row each.
s = sub1(s,
"""    if (p->name_buf_node != n) {
        std::snprintf(p->name_buf, sizeof(p->name_buf), "%s", r.name);
        p->name_buf_node = n;
    }
    if (dai_ui_input_text(p->ui, "Name", p->name_buf, sizeof(p->name_buf)))
        std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);

    if (p->tag_buf_node != n) {
        std::snprintf(p->tag_buf, sizeof(p->tag_buf), "%s", r.tag);
        p->tag_buf_node = n;
    }
    if (dai_ui_input_text(p->ui, "Tag", p->tag_buf, sizeof(p->tag_buf)))
        std::snprintf(r.tag, sizeof(r.tag), "%s", p->tag_buf);

    if (p->asset_buf_node != n) {
        std::snprintf(p->asset_buf, sizeof(p->asset_buf), "%s", r.asset);
        p->asset_buf_node = n;
    }
    if (dai_ui_input_text(p->ui, "Asset", p->asset_buf, sizeof(p->asset_buf)))
        std::snprintf(r.asset, sizeof(r.asset), "%s", p->asset_buf);""",
"""    // One line, Unity's object header: is it on, what is it, what is it
    // called. Three stacked fields for that read like a form, and the
    // inspector is not a form.
    if (p->name_buf_node != n) {
        std::snprintf(p->name_buf, sizeof(p->name_buf), "%s", r.name);
        p->name_buf_node = n;
    }
    if (p->tag_buf_node != n) {
        std::snprintf(p->tag_buf, sizeof(p->tag_buf), "%s", r.tag);
        p->tag_buf_node = n;
    }
    if (p->asset_buf_node != n) {
        std::snprintf(p->asset_buf, sizeof(p->asset_buf), "%s", r.asset);
        p->asset_buf_node = n;
    }
    {
        dai_ui *ui2 = p->ui;
        const dai_ui_style *st2 = dai_ui_style_of(ui2);
        float hx, hy;
        dai_ui_cursor_pos(ui2, &hx, &hy);
        dai_ui_advance(ui2, 0, 26.0f);
        float hw = dai_ui_panel_width(ui2) - st2->padding * 2;
        float mx2 = 0, my2 = 0;
        int d2 = 0, p2 = 0;
        dai_ui_mouse(ui2, &mx2, &my2, &d2, &p2);
        dai_ui_rrect(ui2, hx, hy, hw, 26.0f, 4.0f, rgba(0x34, 0x34, 0x34, 255));

        // the active checkbox
        float bx = hx + 6.0f, by = hy + 6.0f, bsz = 14.0f;
        bool over_box = mx2 >= bx && mx2 < bx + bsz && my2 >= by && my2 < by + bsz;
        dai_ui_rect(ui2, bx, by, bsz, bsz, over_box ? st2->button_hover : st2->track);
        dai_ui_rect_outline(ui2, bx, by, bsz, bsz, 1.0f, st2->panel_border);
        {
            int on = !r.hidden;
            if (on) {
                dai_ui_line(ui2, bx + 3.0f, by + 7.0f, bx + 6.0f, by + 10.5f, 2.0f, st2->text);
                dai_ui_line(ui2, bx + 6.0f, by + 10.5f, bx + 11.5f, by + 3.5f, 2.0f, st2->text);
            }
            if (over_box && p2)
                r.hidden = !r.hidden;
        }
        // the icon says the kind, the field says the name
        dai_ui_icon_at(ui2, node_icon(r), hx + 26.0f, hy + 5.0f, 16.0f, st2->accent);
        dai_ui_text_field(ui2, "objname", hx + 48.0f, hy + 4.0f, hw - 56.0f, 18.0f,
                          p->name_buf, sizeof(p->name_buf), nullptr);
        std::snprintf(r.name, sizeof(r.name), "%s", p->name_buf);
    }
    // Tag and asset share one secondary line; the asset field only exists
    // when the node has an asset to show.
    dai_ui_row(p->ui, 18.0f);
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
    dai_ui_separator(p->ui);""", "inspector header")

wr(P, s)
print("patch21 done")
