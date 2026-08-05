#!/usr/bin/env python3
# patch77 - "Create: Material" wrote the file and the browser never showed it.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p77'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# The asset layer lists only extensions it knows what to DO with - and the
# comment three lines above this list says exactly why that is right, and
# exactly what goes wrong when the list is short: "a browser that hides a file
# the editor just wrote is a browser that lies."
#
# Which is what happened. Create: Material wrote the .daimat, asked for a
# refresh, and the refresh came back without it - so nothing appeared, the
# inline rename had no row to attach to, and the material picker had nothing to
# offer either. The file was on disk the whole time.
s = rd('src/dai_assets.cpp')
s = sub1(s,
"""            return e == "glb" || e == "gltf" || e == "js" || e == "cpp" ||
                   e == "daidalos" || e == "hpp" || e == "h" || e == "json" ||
                   e == "txt" || e == "md" || e == "png" || e == "jpg" ||
                   e == "jpeg" || e == "wav" || e == "ogg" || e == "glsl";""",
"""            return e == "glb" || e == "gltf" || e == "js" || e == "ts" ||
                   e == "cpp" || e == "cc" || e == "cxx" ||
                   e == "daidalos" || e == "daimat" ||
                   e == "hpp" || e == "h" || e == "json" ||
                   e == "txt" || e == "md" || e == "png" || e == "jpg" ||
                   e == "jpeg" || e == "wav" || e == "ogg" ||
                   e == "glsl" || e == "vert" || e == "frag";""",
    'daimat is listable')
wr('src/dai_assets.cpp', s)

# While we are here: the material file the editor writes is ALL DEFAULTS, and
# to_text writes only what differs from the default - so the file was one line
# long. Correct, and useless as a starting point: there is nothing in it to
# edit, and opening it in the script tab shows a header and a blank page.
# A freshly created material writes its fields out in full.
s = rd('src/dai_material.cpp')
s = sub1(s,
"""dai_result dai_matfile_save(const dai_matfile *m, const char *path) {
    if (!m || !path || !*path) return DAI_ERR_INVALID_ARG;
    char buf[1024];
    size_t need = dai_matfile_to_text(m, buf, sizeof(buf));""",
"""// Every field, whether it differs from the default or not. Used when CREATING
// a material: a new file that is one header line long is technically correct
// and completely unhelpful - you open it to change the colour and there is no
// colour line to change. Saving an EDITED material still writes the short form.
size_t dai_matfile_to_text_full(const dai_matfile *m, char *buf, size_t buf_size) {
    if (!m) return 0;
    std::string t;
    char line[160];
    std::snprintf(line, sizeof(line), "%s %d\\n", MAGIC, FORMAT_VERSION);
    t += line;
    std::snprintf(line, sizeof(line), "color %s %s %s\\n",
                  fstr(m->color.x).c_str(), fstr(m->color.y).c_str(),
                  fstr(m->color.z).c_str());
    t += line;
    std::snprintf(line, sizeof(line), "roughness %s\\n", fstr(m->roughness).c_str());
    t += line;
    std::snprintf(line, sizeof(line), "metallic %s\\n", fstr(m->metallic).c_str());
    t += line;
    std::snprintf(line, sizeof(line), "emissive %s\\n", fstr(m->emissive).c_str());
    t += line;
    if (buf && buf_size) {
        size_t n = t.size() < buf_size - 1 ? t.size() : buf_size - 1;
        std::memcpy(buf, t.data(), n);
        buf[n] = 0;
    }
    return t.size();
}

dai_result dai_matfile_save(const dai_matfile *m, const char *path) {
    if (!m || !path || !*path) return DAI_ERR_INVALID_ARG;
    char buf[1024];
    size_t need = dai_matfile_to_text(m, buf, sizeof(buf));""",
    'to_text_full')
wr('src/dai_material.cpp', s)

s = rd('include/dai_material.h')
s = sub1(s,
"""DAI_API size_t     dai_matfile_to_text(const dai_matfile *m, char *buf, size_t buf_size);""",
"""DAI_API size_t     dai_matfile_to_text(const dai_matfile *m, char *buf, size_t buf_size);
/* The same, with every field written out even when it matches the default.
 * What a NEW material file should contain: something to edit. */
DAI_API size_t     dai_matfile_to_text_full(const dai_matfile *m, char *buf, size_t buf_size);""",
    'to_text_full decl')
wr('include/dai_material.h', s)

s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""    make_parent_dirs(path);
    dai_matfile m = dai_matfile_default();
    return dai_matfile_save(&m, path) == DAI_OK ? 1 : 0;""",
"""    make_parent_dirs(path);
    dai_matfile m = dai_matfile_default();
    // Written in full: a new material is something you open and change, and
    // the short form has nothing in it to change.
    char text[1024];
    size_t need = dai_matfile_to_text_full(&m, text, sizeof(text));
    FILE *f = std::fopen(path, "wb");
    if (!f) return 0;
    size_t wrote = std::fwrite(text, 1, need, f);
    return (std::fclose(f) == 0 && wrote == need) ? 1 : 0;""",
    'create writes full text')
wr('examples/editor_demo.cpp', s)
print('patch77 ok')
