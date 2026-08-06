import io

p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()

# ---------------------------------------------------------------------------
# Die Liste: Tasten zuerst (sie muss Pfeile und Tab VOR dem Editor sehen),
# dann zeichnen.
# ---------------------------------------------------------------------------
old = """        for (int i = 0; i < 8 && in.text[i]; ++i) {
            uint32_t cp = in.text[i];
            if (cp < 0x20 || cp == 0x7F) continue;"""
new = """        // ---- autocomplete: the keys it owns --------------------------------
        // Taken BEFORE the editor's own handling, because up, down, tab and
        // escape mean something else while a list is open - and a completion
        // list that you cannot dismiss with escape is a trap.
        if (st->ac_open > 0) {
            if (in.key_escape) { st->ac_open = 0; caret_input = true; }
            else if (in.key_up)   { if (--st->ac_sel < 0) st->ac_sel = st->ac_open - 1; }
            else if (in.key_down) { if (++st->ac_sel >= st->ac_open) st->ac_sel = 0; }
            else if (in.key_tab || in.key_enter) {
                ac_take = 1;                      // applied below, where the list is built
            }
            if (in.key_up || in.key_down || in.key_tab || in.key_enter || in.key_escape) {
                // Swallow them: the editor must not also move the caret.
                in.key_up = in.key_down = in.key_tab = in.key_enter = 0;
                in.key_escape = 0;
            }
        }

        for (int i = 0; i < 8 && in.text[i]; ++i) {
            uint32_t cp = in.text[i];
            if (cp < 0x20 || cp == 0x7F) continue;"""
assert s.count(old) == 1, 'text input loop not found'
s = s.replace(old, new)

# `in` ist eine Kopie? Pruefen: der Code nutzt "in.key_enter" - wir brauchen
# eine lokale, veraenderbare Kopie.
# Das vorhandene "const dai_ui_input &in" wird zu einer KOPIE: Autocomplete
# isst die Tasten, die es benutzt, und der Editor darunter darf sie nicht
# mehr sehen. ui->input direkt zu aendern wuerde sie jedem anderen Widget im
# Frame wegnehmen.
old = """    if (st->focused) {
        const dai_ui_input &in = ui->input;
        bool shift = in.key_shift != 0;"""
new = """    int ac_take = 0;
    if (st->focused) {
        dai_ui_input in = ui->input;      // a COPY: see the note above ac_take
        bool shift = in.key_shift != 0;"""
assert s.count(old) == 1, 'focused input not found'
s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('key handling staged')
