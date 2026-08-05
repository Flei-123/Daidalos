import io

def rd(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def rep(s, old, new, n=1, tag=''):
    c = s.count(old)
    assert c == n, 'count %d != %d for %s :: %r' % (c, n, tag, old[:70])
    return s.replace(old, new)

D = 'examples/editor_demo.cpp'
d = rd(D)

start = d.index('// "// @param [type] name [= default]" lines declare the serialized fields the')
end = d.index('// The JS side of what the C++ behaviours already had')
old = d[start:end]
assert 'script_params_of' in old

new = r'''// What the inspector draws for a behaviour, reported as "type:name=default"
// entries separated by commas, with an optional "|description" on the end:
//
//     let walkSpeed = 5           ->  float:walkSpeed=5
//     // @param float speed = 6   ->  float:speed=6
//     // @header Movement         ->  header:Movement=
//
// Two ways in, because there are two kinds of day. A top level declaration in
// a .js IS the field - that is Unity's rule for a public member, and it means
// a script with no comments in it still has an inspector. The "// @param"
// line stays for the cases a declaration cannot express: a node reference has
// no literal to infer a type from, and a .cpp behaviour's members are not
// where an editor can see them.
//
// Headers and descriptions follow Unity too: "// @header X" or the C# form
// "// [Header("X")]" groups what comes after it, and "// @tooltip X",
// "// [Tooltip("X")]" or simply a comment line directly above a field is the
// text shown when the pointer rests on that row.
static void params_emit(char *buf, size_t n, size_t *used, const char *one) {
    size_t kl = std::strlen(one);
    if (*used + kl + 2 >= n) return;
    if (*used) buf[(*used)++] = ',';
    std::memcpy(buf + *used, one, kl);
    *used += kl;
    buf[*used] = 0;
}

// To the end of the line, trimmed, with the four characters this report uses
// as separators turned into spaces - a description may not redraw the format.
static void params_text(const char *e, char *out, int cap) {
    while (*e == ' ' || *e == '\t') ++e;
    int i = 0;
    while (*e && *e != '\n' && *e != '\r' && i < cap - 1) {
        char c = *e++;
        if (c == ',' || c == '|' || c == ':' || c == '=') c = ' ';
        out[i++] = c;
    }
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) --i;
    out[i] = 0;
}

// The text inside the first pair of quotes, for the C# attribute forms.
static bool params_quoted(const char *line, char *out, int cap) {
    const char *a = std::strchr(line, '"');
    if (!a) return false;
    const char *b = std::strchr(a + 1, '"');
    if (!b) return false;
    int i = 0;
    for (const char *c = a + 1; c < b && i < cap - 1; ++c)
        *out = 0, out[i++] = (*c == ',' || *c == '|' || *c == ':' || *c == '=') ? ' ' : *c;
    out[i] = 0;
    return i > 0;
}

static bool params_ident_ok(const char *s) {
    if (!s || !*s) return false;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) return false;
    for (const char *c = s; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_')) return false;
    return true;
}

static void script_params_of(const char *path, char *buf, size_t n, void *) {
    if (!buf || !n) return;
    buf[0] = 0;
    if (!path || !g_assets_dir[0]) return;
    char full[640];
    std::snprintf(full, sizeof(full), "%s/%s", g_assets_dir, path);
    FILE *f = std::fopen(full, "rb");
    if (!f) return;
    bool js = !is_cpp_script(path);
    char line[512];
    size_t used = 0;
    char pend_tip[160] = { 0 };
    std::vector<std::string> seen;      // one field, however it was declared
    while (std::fgets(line, sizeof(line), f)) {
        // ---- headers ------------------------------------------------------
        const char *h = std::strstr(line, "@header");
        char txt[160];
        if (!h && std::strstr(line, "[Header(") && params_quoted(line, txt, sizeof(txt))) {
            char one[200];
            std::snprintf(one, sizeof(one), "header:%s=", txt);
            params_emit(buf, n, &used, one);
            pend_tip[0] = 0;
            continue;
        }
        if (h) {
            params_text(h + 7, txt, sizeof(txt));
            if (txt[0]) {
                char one[200];
                std::snprintf(one, sizeof(one), "header:%s=", txt);
                params_emit(buf, n, &used, one);
            }
            pend_tip[0] = 0;
            continue;
        }
        // ---- descriptions --------------------------------------------------
        if (std::strstr(line, "[Tooltip(") && params_quoted(line, txt, sizeof(txt))) {
            std::snprintf(pend_tip, sizeof(pend_tip), "%s", txt);
            continue;
        }
        const char *t = std::strstr(line, "@tooltip");
        int tskip = 8;
        if (!t) { t = std::strstr(line, "@desc"); tskip = 5; }
        if (t) {
            params_text(t + tskip, pend_tip, sizeof(pend_tip));
            continue;
        }
        // ---- "// @param [type] name [= default]" ----------------------------
        const char *p = std::strstr(line, "// @param");
        if (p) {
            p += 9;
            auto word = [&p](char *out, int cap) {
                while (*p == ' ' || *p == '\t') ++p;
                int i = 0;
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
                    if (i < cap - 1) out[i++] = *p;
                    ++p;
                }
                out[i] = 0;
                return i;
            };
            char first[64], second[64];
            if (!word(first, sizeof(first))) { pend_tip[0] = 0; continue; }
            const char *type = "node";
            char *key = first;
            // Two words means the first was a type. One word is a node
            // reference, which is what this line meant before types existed.
            if (word(second, sizeof(second))) {
                static const char *TYPES[] = { "float", "number", "int", "bool", "string", "text", "node" };
                for (const char *ty : TYPES)
                    if (std::strcmp(first, ty) == 0) { type = first; break; }
                key = second;
            }
            char def[96] = { 0 };
            {
                const char *ee = std::strchr(p, '=');
                if (ee) params_text(ee + 1, def, sizeof(def));
            }
            bool dup = false;
            for (const std::string &sname : seen) if (sname == key) dup = true;
            if (!dup) {
                seen.push_back(key);
                char one[400];
                if (pend_tip[0])
                    std::snprintf(one, sizeof(one), "%s:%s=%s|%s", type, key, def, pend_tip);
                else
                    std::snprintf(one, sizeof(one), "%s:%s=%s", type, key, def);
                params_emit(buf, n, &used, one);
            }
            pend_tip[0] = 0;
            continue;
        }
        // ---- a plain comment line is the next field's description ----------
        {
            const char *c = line;
            while (*c == ' ' || *c == '\t') ++c;
            if (c[0] == '/' && c[1] == '/') {
                params_text(c + 2, pend_tip, sizeof(pend_tip));
                continue;
            }
            if (!*c || *c == '\n' || *c == '\r') { pend_tip[0] = 0; continue; }
        }
        // ---- a top level declaration IS a field ----------------------------
        // Column zero only: an indented "let" is a local inside a function,
        // and a behaviour whose loop counters showed up in the inspector
        // would be worse than no inspector at all.
        if (!js) { pend_tip[0] = 0; continue; }
        const char *c = line;
        int kw = 0;
        if (!std::strncmp(c, "let ", 4))        kw = 4;
        else if (!std::strncmp(c, "var ", 4))   kw = 4;
        else if (!std::strncmp(c, "const ", 6)) kw = 6;
        if (!kw) { pend_tip[0] = 0; continue; }
        c += kw;
        while (*c == ' ' || *c == '\t') ++c;
        char name[64];
        int i = 0;
        while (((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                (*c >= '0' && *c <= '9') || *c == '_') && i < (int)sizeof(name) - 1)
            name[i++] = *c++;
        name[i] = 0;
        while (*c == ' ' || *c == '\t') ++c;
        if (!i || *c != '=') { pend_tip[0] = 0; continue; }
        ++c;
        while (*c == ' ' || *c == '\t') ++c;
        // The literal, and only a literal: an expression has no value the
        // editor could show and no type it could guess.
        char val[128] = { 0 };
        const char *type = nullptr;
        if (*c == '"' || *c == '\'') {
            char q = *c++;
            int j = 0;
            while (*c && *c != q && j < (int)sizeof(val) - 1) {
                char ch = *c++;
                val[j++] = (ch == ',' || ch == '|' || ch == ':' || ch == '=') ? ' ' : ch;
            }
            val[j] = 0;
            type = "string";
        } else if (!std::strncmp(c, "true", 4) || !std::strncmp(c, "false", 5)) {
            std::snprintf(val, sizeof(val), "%s", *c == 't' ? "true" : "false");
            type = "bool";
        } else if ((*c >= '0' && *c <= '9') || *c == '-' || *c == '+' || *c == '.') {
            int j = 0, dots = 0;
            bool ok = true;
            const char *q = c;
            if (*q == '-' || *q == '+') val[j++] = *q++;
            while (*q && j < (int)sizeof(val) - 1) {
                if (*q >= '0' && *q <= '9') val[j++] = *q++;
                else if (*q == '.') { if (++dots > 1) { ok = false; break; } val[j++] = *q++; }
                else break;
            }
            val[j] = 0;
            // Trailing junk on the number ("5e3", "5px") is not a number.
            while (*q == ' ' || *q == '\t') ++q;
            if (*q && *q != ';' && *q != '/' && *q != '\n' && *q != '\r') ok = false;
            if (ok && j) type = dots ? "float" : "int";
        }
        if (!type) { pend_tip[0] = 0; continue; }
        bool dup = false;
        for (const std::string &sname : seen) if (sname == name) dup = true;
        if (!dup) {
            seen.push_back(name);
            char one[400];
            if (pend_tip[0])
                std::snprintf(one, sizeof(one), "%s:%s=%s|%s", type, name, val, pend_tip);
            else
                std::snprintf(one, sizeof(one), "%s:%s=%s", type, name, val);
            params_emit(buf, n, &used, one);
        }
        pend_tip[0] = 0;
    }
    std::fclose(f);
}

'''
d = d[:start] + new + d[end:]

# ---- the values reach the declaration, not only the params object ----------
d = rep(d, """            std::string path = entry, params_js;""",
"""            std::string path = entry, params_js, assign_js;""", tag='assign decl')

d = rep(d, """                        if (v == "true" || v == "false" || numeric)
                            params_js += "\\"" + k + "\\":" + v + ",";
                        else
                            params_js += "\\"" + k + "\\":\\"" + v + "\\",";""",
"""                        std::string lit = (v == "true" || v == "false" || numeric)
                                        ? v : ("\\"" + v + "\\"");
                        params_js += "\\"" + k + "\\":" + lit + ",";
                        // ...and onto the declaration itself. A script that
                        // says `let walkSpeed = 5` at the top has just told
                        // the inspector its default; the value the user typed
                        // has to land in THAT variable, or the field is a
                        // display that changes nothing. A const cannot be
                        // assigned - that throws, and a throw here would take
                        // the rest of the fields with it.
                        if (params_ident_ok(k.c_str()))
                            assign_js += "try{" + k + "=" + lit + ";}catch(e){}";""",
       tag='assign build')

d = rep(d, """            if (!params_js.empty()) dai_script_eval(s, params_js.c_str(), "params", err, sizeof(err));""",
"""            if (!params_js.empty()) dai_script_eval(s, params_js.c_str(), "params", err, sizeof(err));
            // After the file has run, so it overwrites the defaults the
            // declarations just installed, and before init(), so the first
            // frame already sees the inspector's numbers.
            if (!assign_js.empty()) dai_script_eval(s, assign_js.c_str(), "fields", err, sizeof(err));""",
       tag='assign eval')

wr(D, d)
print('editor_demo ok')
