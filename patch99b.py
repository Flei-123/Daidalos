import io

# dai_ui_typing() = "ein Feld ODER der Code-Editor hat die Tastatur" - die
# Frage vor einem Shortcut. Ctrl+S braucht die engere: NUR der Code-Editor.
# Sonst speichert Ctrl+S das Script, waehrend der Cursor in einem Zahlenfeld
# des Inspectors steht.
p = 'include/dai_ui.h'
s = io.open(p, encoding='utf-8').read()
old = "DAI_API int  dai_ui_typing(const dai_ui *ui);"
new = """DAI_API int  dai_ui_typing(const dai_ui *ui);
/* Only the code editor, not text fields. Ctrl+S asks this: it means the
 * SCRIPT when the caret is in a script, and the scene in every other case. */
DAI_API int  dai_ui_code_focused(const dai_ui *ui);"""
assert s.count(old) == 1, 'dai_ui_typing decl not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)

p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()
old = """int  dai_ui_typing(const dai_ui *ui) {"""
new = """int  dai_ui_code_focused(const dai_ui *ui) { return ui && ui->code_focus ? 1 : 0; }
int  dai_ui_typing(const dai_ui *ui) {"""
assert s.count(old) == 1, 'dai_ui_typing def not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_ui: code_focused() - the narrow question Ctrl+S needs')
