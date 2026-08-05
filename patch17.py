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

# The component header menu, Unity's gear list: reset to the defaults, copy
# the values, paste them onto the same kind of component, remove.
s = sub1(s,
"""        int cpick = dai_ui_popup_menu(p->ui, &p->menu_comp, COMP_MENU, 3);""",
"""        static const dai_ui_menu_item COMP_MENU_X[] = {
            { DAI_ICON_RESET, "Reset", nullptr },
            { DAI_ICON_COPY, "Copy Component", nullptr },
            { DAI_ICON_SAVE, "Paste Component Values", nullptr },
            { DAI_ICON_CLOSE, "Remove Component", nullptr },
        };
        int cpick = dai_ui_popup_menu(p->ui, &p->menu_comp, COMP_MENU_X, 4);""", "comp menu entries")

s = sub1(s,
"""                if (cpick == 0) {
                    dai_editor_ui_clipboard_set(p, 2, comp_to_text(target, ar).c_str());
                    dai_editor_ui_toast(p, "component copied", 1.5f);
                } else if (cpick == 1) {""",
"""                if (cpick == 0) {
                    // Reset is what a grown-up editor answers when you clicked
                    // three values too far: the component goes back to the
                    // defaults, the rest of the object stays put.
                    dai_doc_begin(d, "Reset Component");
                    switch (target) {
                    case 1: ar.density = 0.0f; ar.friction = p->def_friction;
                            ar.restitution = p->def_restitution; ar.motion = DAI_DYNAMIC; break;
                    case 2: ar.trigger = 0; ar.collider_center = dai_vec3{ 0, 0, 0 }; break;
                    case 3: ar.camera_fov = 0.0f; ar.camera_size = 0.0f; break;
                    case 4: ar.light_color = dai_vec3{ 0, 0, 0 }; ar.light_range = 0.0f;
                            ar.light_intensity = 0.0f; ar.light_cone = 0.0f; break;
                    default: break;
                    }
                    dai_doc_set(d, an, &ar);
                    dai_doc_commit(d);
                    dai_editor_resync(p->ed);
                    dai_editor_ui_toast(p, "component reset", 1.5f);
                } else if (cpick == 1) {
                    dai_editor_ui_clipboard_set(p, 2, comp_to_text(target, ar).c_str());
                    dai_editor_ui_toast(p, "component copied", 1.5f);
                } else if (cpick == 2) {""", "comp menu dispatch")

s = sub1(s,
"""                } else if (cpick == 2) {
                    dai_doc_begin(d, "Remove Component");""",
"""                } else if (cpick == 3) {
                    dai_doc_begin(d, "Remove Component");""", "comp remove index")

wr(P, s)
print("patch17 done")
