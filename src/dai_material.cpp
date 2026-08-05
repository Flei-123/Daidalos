// Materials as files. See include/dai_material.h for why.

#include "dai_material.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char *MAGIC = "daidalos-material";
const int   FORMAT_VERSION = 1;

const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// One word, and where the rest of the line starts.
const char *token(const char *p, std::string &out) {
    p = skip_ws(p);
    out.clear();
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') out += *p++;
    return p;
}

void parse_floats(const char *p, float *out, int n) {
    for (int i = 0; i < n; ++i) {
        p = skip_ws(p);
        if (!*p || *p == '\n' || *p == '\r') return;
        char *end = nullptr;
        float v = std::strtof(p, &end);
        if (end == p) return;
        out[i] = v;
        p = end;
    }
}

// A float, short: 0.35 not 0.349999994. The rest of this engine's text formats
// do the same, and for the same reason - these files are read by people.
std::string fstr(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", (double)v);
    return buf;
}

} // namespace

dai_matfile dai_matfile_default(void) {
    dai_matfile m{};
    m.color = dai_vec3{ 0.8f, 0.8f, 0.8f };
    m.roughness = 0.5f;
    m.metallic = 0.0f;
    m.emissive = 0.0f;
    return m;
}

dai_result dai_matfile_from_text(dai_matfile *out, const char *text, size_t len) {
    if (!out || !text) return DAI_ERR_INVALID_ARG;
    *out = dai_matfile_default();

    std::string all(text, len);
    size_t pos = 0;
    bool seen_header = false;
    while (pos <= all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? nl : nl - pos);
        pos = nl == std::string::npos ? all.size() + 1 : nl + 1;

        const char *p = skip_ws(line.c_str());
        if (!*p || *p == '\r' || *p == '#') continue;
        std::string key;
        const char *after = token(p, key);

        if (!seen_header) {
            // The header is not decoration: it is what stops a .txt that
            // happens to be in the folder from loading as a material and
            // silently painting something grey.
            if (key != MAGIC) return DAI_ERR_FILE;
            seen_header = true;
            continue;
        }
        if      (key == "color")     parse_floats(after, &out->color.x, 3);
        else if (key == "roughness") parse_floats(after, &out->roughness, 1);
        else if (key == "metallic")  parse_floats(after, &out->metallic, 1);
        else if (key == "emissive")  parse_floats(after, &out->emissive, 1);
        // Anything else is from a newer editor. Skipped, not refused: a file
        // that will not open is worse than a field that is not understood.
    }
    if (!seen_header) return DAI_ERR_FILE;

    // Clamped on the way in, not at every use. A negative roughness reaching
    // the renderer is a NaN three functions later.
    if (out->roughness < 0.02f) out->roughness = 0.02f;
    if (out->roughness > 1.0f)  out->roughness = 1.0f;
    if (out->metallic < 0.0f)   out->metallic = 0.0f;
    if (out->metallic > 1.0f)   out->metallic = 1.0f;
    if (out->emissive < 0.0f)   out->emissive = 0.0f;
    for (float *v : { &out->color.x, &out->color.y, &out->color.z })
        *v = *v < 0.0f ? 0.0f : (*v > 1.0f ? 1.0f : *v);
    return DAI_OK;
}

size_t dai_matfile_to_text(const dai_matfile *m, char *buf, size_t buf_size) {
    if (!m) return 0;
    const dai_matfile d = dai_matfile_default();
    std::string t;
    char line[160];
    std::snprintf(line, sizeof(line), "%s %d\n", MAGIC, FORMAT_VERSION);
    t += line;
    if (m->color.x != d.color.x || m->color.y != d.color.y || m->color.z != d.color.z) {
        std::snprintf(line, sizeof(line), "color %s %s %s\n",
                      fstr(m->color.x).c_str(), fstr(m->color.y).c_str(),
                      fstr(m->color.z).c_str());
        t += line;
    }
    if (m->roughness != d.roughness) {
        std::snprintf(line, sizeof(line), "roughness %s\n", fstr(m->roughness).c_str());
        t += line;
    }
    if (m->metallic != d.metallic) {
        std::snprintf(line, sizeof(line), "metallic %s\n", fstr(m->metallic).c_str());
        t += line;
    }
    if (m->emissive != d.emissive) {
        std::snprintf(line, sizeof(line), "emissive %s\n", fstr(m->emissive).c_str());
        t += line;
    }
    if (buf && buf_size) {
        size_t n = t.size() < buf_size - 1 ? t.size() : buf_size - 1;
        std::memcpy(buf, t.data(), n);
        buf[n] = 0;
    }
    return t.size();
}

dai_result dai_matfile_load(dai_matfile *out, const char *path, char *err, size_t err_size) {
    if (err && err_size) err[0] = 0;
    if (!out || !path || !*path) return DAI_ERR_INVALID_ARG;
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        if (err && err_size) std::snprintf(err, err_size, "cannot open '%s'", path);
        return DAI_ERR_FILE;
    }
    std::string text;
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
    std::fclose(f);
    dai_result r = dai_matfile_from_text(out, text.data(), text.size());
    if (r != DAI_OK && err && err_size)
        std::snprintf(err, err_size, "'%s' is not a daidalos material", path);
    return r;
}

dai_result dai_matfile_save(const dai_matfile *m, const char *path) {
    if (!m || !path || !*path) return DAI_ERR_INVALID_ARG;
    char buf[1024];
    size_t need = dai_matfile_to_text(m, buf, sizeof(buf));
    if (need >= sizeof(buf)) return DAI_ERR_FILE;      // cannot happen; not assumed
    FILE *f = std::fopen(path, "wb");
    if (!f) return DAI_ERR_FILE;
    size_t wrote = std::fwrite(buf, 1, need, f);
    int ok = std::fclose(f) == 0 && wrote == need;
    return ok ? DAI_OK : DAI_ERR_FILE;
}

int dai_matfile_is_file(const char *path) {
    if (!path) return 0;
    size_t n = std::strlen(path);
    const char *ext = ".daimat";
    size_t e = std::strlen(ext);
    if (n <= e) return 0;
    // Case insensitive: Windows hands back whatever the user typed.
    for (size_t i = 0; i < e; ++i) {
        char a = path[n - e + i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[i]) return 0;
    }
    return 1;
}
