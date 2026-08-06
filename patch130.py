import io

# ===========================================================================
# "Ich sehe keine examples scripts": sie lagen im REPO, nicht im Projekt.
# Ein Beispiel, das man nicht anlegen kann, ist keins - also legt der Editor
# es an, aus derselben Quelle, die im Repo steht.
# ===========================================================================
src = io.open('examples/scripts/PlayerController.cpp', encoding='utf-8').read()

def cstr(text):
    out = []
    for line in text.split('\n'):
        e = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append('                   "%s\\n"' % e)
    return '\n'.join(out)

p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

old = """        // The template is the documentation. A behaviour that starts as an
        // empty file means reading a header to find out what to type."""
new = """        // "PlayerController.cpp" is the WORKED example, not the stub. It is
        // the file examples/scripts/PlayerController.cpp in the engine repo,
        // embedded here - because an example you have to go and find in a
        // source tree you did not clone is not an example, it is a rumour.
        //
        // Recognised by name on purpose: the Create menu asks for it by
        // asking for that file, and no second callback has to exist for one
        // template.
        {
            const char *base_name = std::strrchr(name, '/');
            base_name = base_name ? base_name + 1 : name;
            if (std::strcmp(base_name, "PlayerController.cpp") == 0) {
                std::fputs(
%s, cf);
                std::fclose(cf);
                return 1;
            }
        }
        // The template is the documentation. A behaviour that starts as an
        // empty file means reading a header to find out what to type.""" % cstr(src)
assert s.count(old) == 1, 'template comment not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('PlayerController.cpp is creatable from the editor')

# --- der Menueeintrag ------------------------------------------------------
p = 'src/dai_editor_ui.cpp'
s = io.open(p, encoding='utf-8').read()
old = """        { DAI_ICON_MATERIAL, "Create: Material", nullptr },
    };"""
new = """        { DAI_ICON_MATERIAL, "Create: Material", nullptr },
        { DAI_ICON_C_SCRIPT, "Create: Player Controller (C++)", nullptr },
    };"""
assert s.count(old) == 1, 'proj items not found'
s = s.replace(old, new)
old = """    static const int WITH_ROW[8]    = { 0, 1, 7, 2, 3, 4, 5, 6 };
    static const int WITHOUT_ROW[6] = { 0, 1, 7, 3, 4, 5 };"""
new = """    static const int WITH_ROW[9]    = { 0, 1, 7, 8, 2, 3, 4, 5, 6 };
    static const int WITHOUT_ROW[7] = { 0, 1, 7, 8, 3, 4, 5 };"""
assert s.count(old) == 1, 'row maps not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('menu entry added')
