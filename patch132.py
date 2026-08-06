import io

# ===========================================================================
# Das Anfassen selbst. Im Host, weil dort der Zeigerzustand und die Ansicht
# zusammenkommen - und weil eine Bearbeitung ins DOKUMENT schreibt, was der
# Host ohnehin besitzt.
# ===========================================================================
p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

old = """        // ---- the game's own UI ------------------------------------------"""
new = """        // ---- dragging the selected UI element ----------------------------
        //
        // A frame around what is selected, and a handle in its corner. Moving
        // writes the OFFSET, not a position: the anchor is what makes a HUD
        // survive a different window size, and a drag that replaced it with
        // absolute pixels would quietly undo that.
        static dai_node ui_drag_node = DAI_INVALID_NODE;
        static int   ui_drag_kind = 0;         // 1 move, 2 resize
        static float ui_drag_x0 = 0, ui_drag_y0 = 0, ui_drag_ox = 0, ui_drag_oy = 0;
        if (dai_editor_selection_count(ed) > 0) {
            dai_node sel_n = dai_editor_selected(ed, 0);
            float rx, ry, rw, rh;
            if (dai_hud_rect_of(sel_n, &rx, &ry, &rw, &rh)) {
                dai_node_desc sr{};
                int have = dai_doc_get(doc, sel_n, &sr) == DAI_OK;
                float mx2 = 0, my2 = 0;
                int mdown = 0, mpress = 0;
                dai_ui_mouse(ui, &mx2, &my2, &mdown, &mpress);

                // The frame, and a corner grip - only for a Text with a box
                // and for an Image, because those are the two that HAVE a
                // size. A grip on a label that is as wide as its words would
                // resize nothing.
                dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 6);
                dai_ui_rect_outline(ui, rx - 2.0f, ry - 2.0f, rw + 4.0f, rh + 4.0f, 1.0f,
                                    0xFF3D84D8u);
                bool sizable = have && (sr.image_on || (sr.text_on && sr.text_w > 0.0f));
                float gx = rx + rw - 5.0f, gy = ry + rh - 5.0f;
                if (sizable) {
                    dai_ui_rect(ui, gx, gy, 10.0f, 10.0f, 0xFF3D84D8u);
                    dai_ui_rect_outline(ui, gx, gy, 10.0f, 10.0f, 1.0f, 0xFFFFFFFFu);
                }
                dai_ui_layer_pop(ui);

                bool over_grip = sizable && mx2 >= gx - 2.0f && mx2 < gx + 12.0f &&
                                 my2 >= gy - 2.0f && my2 < gy + 12.0f;
                bool over_body = mx2 >= rx - 2.0f && mx2 < rx + rw + 2.0f &&
                                 my2 >= ry - 2.0f && my2 < ry + rh + 2.0f;
                if (mpress && have && (over_grip || over_body)) {
                    ui_drag_node = sel_n;
                    ui_drag_kind = over_grip ? 2 : 1;
                    ui_drag_x0 = mx2; ui_drag_y0 = my2;
                    if (ui_drag_kind == 1) {
                        ui_drag_ox = sr.image_on && !sr.text_on ? sr.image_x : sr.text_x;
                        ui_drag_oy = sr.image_on && !sr.text_on ? sr.image_y : sr.text_y;
                    } else {
                        ui_drag_ox = sr.image_on && !sr.text_on ? sr.image_w : sr.text_w;
                        ui_drag_oy = sr.image_on && !sr.text_on ? sr.image_h : sr.text_h;
                    }
                }
                if (over_grip) dai_ui_cursor_set(ui, DAI_CURSOR_RESIZE_NWSE);
                else if (over_body && ui_drag_node == DAI_INVALID_NODE)
                    dai_ui_cursor_set(ui, DAI_CURSOR_HAND);
            }
        }
        if (ui_drag_node != DAI_INVALID_NODE) {
            float mx2 = 0, my2 = 0;
            int mdown = 0;
            dai_ui_mouse(ui, &mx2, &my2, &mdown, nullptr);
            dai_node_desc sr{};
            if (dai_doc_get(doc, ui_drag_node, &sr) == DAI_OK) {
                float dx2 = mx2 - ui_drag_x0, dy2 = my2 - ui_drag_y0;
                bool img = sr.image_on && !sr.text_on;
                if (ui_drag_kind == 1) {
                    if (img) { sr.image_x = ui_drag_ox + dx2; sr.image_y = ui_drag_oy + dy2; }
                    else     { sr.text_x  = ui_drag_ox + dx2; sr.text_y  = ui_drag_oy + dy2; }
                } else {
                    float nw = ui_drag_ox + dx2, nh = ui_drag_oy + dy2;
                    if (nw < 8.0f) nw = 8.0f;
                    if (nh < 8.0f) nh = 8.0f;
                    if (img) { sr.image_w = nw; sr.image_h = nh; }
                    else     { sr.text_w  = nw; sr.text_h  = nh; }
                }
                // No transaction while the button is down: a drag is ONE undo
                // step, not one per frame. It is committed on release.
                dai_doc_set(doc, ui_drag_node, &sr);
            }
            if (!mdown) {
                ui_drag_node = DAI_INVALID_NODE;
                ui_drag_kind = 0;
            }
        }

        // ---- the game's own UI ------------------------------------------"""
assert s.count(old) == 1, 'hud draw anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('viewport: move and resize a UI element')
