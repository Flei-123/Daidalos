// Game string tables. See include/dai_strings.h for what this is FOR.
//
// Deliberately a flat map and a linear parser: a table has hundreds of
// entries, not millions, and it is read when a language changes rather than
// per frame. The interesting decisions are all in the format, not the code.

#include "dai_strings.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct dai_strings {
    std::string lang;
    std::string name;
    std::unordered_map<std::string, std::string> map;
    // Sorted keys, rebuilt on load. The editor's picker wants a stable order,
    // and "whatever the hash table felt like" is not one.
    std::vector<std::string> keys;
};

namespace {

void set_err(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// "\n" in a value is a line break. Nothing else is escaped: a format that
// escapes everything needs a quoting rule, and a quoting rule in a file
// people hand-edit is a file people get wrong.
std::string unescape(const std::string &v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char c = v[i + 1];
            if (c == 'n')      { out += '\n'; ++i; continue; }
            if (c == 't')      { out += '\t'; ++i; continue; }
            if (c == '\\')     { out += '\\'; ++i; continue; }
        }
        out += v[i];
    }
    return out;
}

} // namespace

dai_strings *dai_strings_create(void) { return new dai_strings(); }
void dai_strings_destroy(dai_strings *s) { delete s; }

dai_result dai_strings_parse(dai_strings *s, const char *text, size_t len,
                             char *err, size_t err_len) {
    if (!s || !text) return DAI_ERR_INVALID_ARG;
    if (err && err_len) err[0] = 0;

    dai_strings tmp;                 // parse into a copy: half a table is worse
    std::string all(text, len);      // than none, and the caller keeps the old
    size_t pos = 0;
    int line_no = 0;
    bool header = false;

    while (pos <= all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? all.size() + 1 : nl + 1;
        ++line_no;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;    // '#' comments a whole line

        if (!header) {
            if (t.compare(0, 17, "daidalos-strings ") != 0) {
                set_err(err, err_len, "line %d: expected 'daidalos-strings 1', got '%s'",
                        line_no, t.substr(0, 40).c_str());
                return DAI_ERR_FILE;
            }
            int v = std::atoi(t.c_str() + 17);
            if (v != 1) {
                set_err(err, err_len, "string table version %d, this build reads 1", v);
                return DAI_ERR_FILE;
            }
            header = true;
            continue;
        }

        // key, then whitespace, then everything else on the line
        size_t sp = t.find_first_of(" \t");
        std::string key = sp == std::string::npos ? t : t.substr(0, sp);
        std::string val = sp == std::string::npos ? std::string() : trim(t.substr(sp + 1));

        if (key == "lang") { tmp.lang = val; continue; }
        if (key == "name") { tmp.name = val; continue; }

        if (val.empty()) {
            // A key with no text is almost always a line someone started and
            // did not finish; it would resolve to itself and look translated.
            set_err(err, err_len, "line %d: key '%s' has no text", line_no, key.c_str());
            return DAI_ERR_FILE;
        }
        if (tmp.map.count(key)) {
            set_err(err, err_len, "line %d: '%s' appears twice - which one wins is not"
                                  " something a file should leave open", line_no, key.c_str());
            return DAI_ERR_FILE;
        }
        tmp.map[key] = unescape(val);
    }

    if (!header) {
        set_err(err, err_len, "empty file - a string table starts with 'daidalos-strings 1'");
        return DAI_ERR_FILE;
    }

    tmp.keys.reserve(tmp.map.size());
    for (const auto &kv : tmp.map) tmp.keys.push_back(kv.first);
    std::sort(tmp.keys.begin(), tmp.keys.end());

    s->lang.swap(tmp.lang);
    s->name.swap(tmp.name);
    s->map.swap(tmp.map);
    s->keys.swap(tmp.keys);
    return DAI_OK;
}

dai_result dai_strings_load(dai_strings *s, const char *path, char *err, size_t err_len) {
    if (!s || !path) return DAI_ERR_INVALID_ARG;
    FILE *f = std::fopen(path, "rb");
    if (!f) { set_err(err, err_len, "cannot open %s", path); return DAI_ERR_FILE; }
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    return dai_strings_parse(s, all.data(), all.size(), err, err_len);
}

const char *dai_strings_lang(const dai_strings *s) { return s ? s->lang.c_str() : ""; }
const char *dai_strings_name(const dai_strings *s) { return s ? s->name.c_str() : ""; }
uint32_t    dai_strings_count(const dai_strings *s) { return s ? (uint32_t)s->map.size() : 0; }

const char *dai_strings_get(const dai_strings *s, const char *key) {
    if (!key) return "";
    if (!s) return key;
    auto it = s->map.find(key);
    // The key itself, not "". A gap you can see is a gap that gets filled.
    return it == s->map.end() ? key : it->second.c_str();
}

const char *dai_strings_resolve(const dai_strings *s, const char *text,
                                char *out, size_t out_len) {
    if (!out || !out_len) return "";
    out[0] = 0;
    if (!text || !text[0]) return out;
    if (text[0] != '@') {          // plain text, used as written
        std::snprintf(out, out_len, "%s", text);
        return out;
    }
    std::snprintf(out, out_len, "%s", dai_strings_get(s, text + 1));
    return out;
}

uint32_t dai_strings_keys(const dai_strings *s, const char **out, uint32_t max) {
    if (!s) return 0;
    uint32_t n = (uint32_t)s->keys.size();
    if (out) for (uint32_t i = 0; i < n && i < max; ++i) out[i] = s->keys[i].c_str();
    return n;
}
