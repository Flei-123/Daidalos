#!/usr/bin/env python3
# Runde 27d - Host-Verdrahtung fuer UI-Buttons: waehrend Play antwortet der HUD
# der Game-Ansicht auf den Zeiger, und ein Klick ruft die Funktion im Skript
# DES ANGEKLICKTEN KNOTENS auf. Nicht in allen Skripten - "der Button hat
# gefeuert" ist eine Nachricht an ein Objekt, nicht an das Spiel.
import io, os, sys, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
p = os.path.join(ROOT, "examples", "editor_demo.cpp")

def sub(s, old, new, what):
    if old not in s:
        sys.exit("!! nicht gefunden: " + what)
    return s.replace(old, new, 1)

t = io.open(p, encoding="utf-8").read()

# ---- welcher Knoten zu welchem laufenden Skript gehoert -----------------
t = sub(t,
    "struct RunningScript { dai_script *s = nullptr; std::string path; };",
    "// The node is part of it now: a button click has to reach the script ON\n"
    "// that button, and \"which script belongs to which object\" was the one\n"
    "// thing this list did not know.\n"
    "struct RunningScript { dai_script *s = nullptr; std::string path; dai_node node = DAI_INVALID_NODE; };",
    "RunningScript")
t = sub(t, "g_running.push_back({ s, path });",
           "g_running.push_back({ s, path, id });", "g_running.push_back")

# ---- Buttons reagieren nur in der Game-Ansicht, nur waehrend Play -------
t = sub(t,
    """            if (has_game) {
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);
                gui_flush(ui, hx, hy, hw, hh);""",
    """            if (has_game) {
                // Buttons answer the pointer HERE and only here: this is the
                // picture the player is looking at. The copy over the Scene
                // view is for editing, and a button that fired there would go
                // off every time you tried to drag it.
                dai_hud_interactive(dai_editor_state_get(ed) == DAI_EDITOR_PLAY);
                dai_hud_draw(ui, doc, hx, hy, hw, hh, 1.0f, hud_resolve, nullptr);
                dai_hud_interactive(0);
                gui_flush(ui, hx, hy, hw, hh);""",
    "Game-HUD-Zeichnung")

# ---- Klicks zustellen ---------------------------------------------------
t = sub(t,
    """                    dai_script_set_number(rs.s, "dt", 1.0 / 60.0);
                    if (dai_script_call(rs.s, "frame", serr, sizeof(serr)) != DAI_OK && serr[0]) {
                        char line[400];
                        std::snprintf(line, sizeof(line), "%s: %s", rs.path.c_str(), serr);
                        dai_editor_ui_log(panels, 2, line);   // collapses on repeat
                    }
                }
        }
#endif""",
    """                    dai_script_set_number(rs.s, "dt", 1.0 / 60.0);
                    if (dai_script_call(rs.s, "frame", serr, sizeof(serr)) != DAI_OK && serr[0]) {
                        char line[400];
                        std::snprintf(line, sizeof(line), "%s: %s", rs.path.c_str(), serr);
                        dai_editor_ui_log(panels, 2, line);   // collapses on repeat
                    }
                }

            // ---- what the UI buttons did this frame --------------------
            // After frame(), so a click and the frame it happened in are in
            // the order they read: the world moved, THEN the button fired.
            // The call goes to the script on the clicked node only - a button
            // press is a message to an object, not an announcement.
            if (g_scripts_live) {
                dai_node clicked[16];
                uint32_t nc = dai_hud_take_clicks(clicked, 16);
                for (uint32_t ci = 0; ci < nc; ++ci) {
                    dai_node_desc bd{};
                    const char *fn = "onClick";
                    if (dai_doc_get(doc, clicked[ci], &bd) == DAI_OK && bd.button_action[0])
                        fn = bd.button_action;
                    bool any = false;
                    for (RunningScript &rs : g_running) {
                        if (rs.node != clicked[ci]) continue;
                        any = true;
                        char berr[256] = { 0 };
                        dai_result br = dai_script_call(rs.s, fn, berr, sizeof(berr));
                        if (br == DAI_ERR_NOT_FOUND) continue;   // no handler: fine
                        if (br != DAI_OK && berr[0]) {
                            char line[400];
                            std::snprintf(line, sizeof(line), "%s: %s", rs.path.c_str(), berr);
                            dai_editor_ui_log(panels, 2, line);
                        }
                    }
                    if (!any) {
                        // Worth saying out loud: a button that looks alive and
                        // does nothing is the hardest kind of nothing to debug.
                        char line[256];
                        std::snprintf(line, sizeof(line),
                                      "button '%s' clicked - no script on it defines %s()",
                                      bd.name[0] ? bd.name : "(unnamed)", fn);
                        dai_editor_ui_log(panels, 1, line);
                    }
                }
            }
        }
#endif""",
    "Skript-Frame-Block")

shutil.copyfile(p, p + ".bak_p138")
io.open(p, "w", encoding="utf-8").write(t)
print("-- editor_demo.cpp: Buttons interaktiv + onClick-Zustellung")
print("OK")
