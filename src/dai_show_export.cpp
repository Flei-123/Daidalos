// Stage 6: the show leaves the building.
//
// Implements, from include/dai_show.h:
//     dai_show_export_skyc / dai_show_import_skyc
//     dai_show_export_csv
//     dai_show_export_json / dai_show_import_json
//
// .skyc is Skybrush's container and it is the reason this exporter exists at
// all: the shows have to fly on firmware that has been through a thousand
// displays, not on something written here last month. The format is a ZIP with
// a show.json and one trajectory and light program per drone. It is
// implemented from the format description, NOT ported - Daidalos is MIT, the
// reference implementation is GPL, and the two do not mix. The ZIP writer and
// the DEFLATE store path live here rather than in the engine because nothing
// else in Daidalos writes an archive.
//
// CSV and JSON exist because a container only one program can open makes a
// show unauditable. The JSON one round trips exactly - keyframes in, the same
// keyframes out - and the test proves it by comparing the reimported plan
// against the original key for key.

#include "dai_show.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void fail(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

// Shortest text that reads back bit identical. The round trip promise in the
// header is exactly this function: a show exported and reimported has to be
// the same show, not a show that is the same to six digits.
std::string fstr(float v) {
    char buf[40];
    for (int prec = 6; prec < 10; ++prec) {
        snprintf(buf, sizeof(buf), "%.*g", prec, (double)v);
        if ((float)strtod(buf, nullptr) == v) return buf;
    }
    snprintf(buf, sizeof(buf), "%.9g", (double)v);
    return buf;
}
std::string dstr(double v) {
    char buf[48];
    for (int prec = 15; prec < 18; ++prec) {
        snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (strtod(buf, nullptr) == v) return buf;
    }
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

// Every writer in this engine writes beside the target and renames. An export
// that is interrupted has to leave the previous file, or nothing - never half
// a show that a ground station will load the first two minutes of.
dai_result write_atomic(const char *path, const std::string &data,
                        char *err, size_t err_len) {
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { fail(err, err_len, "cannot open %s for writing", tmp.c_str()); return DAI_ERR_FILE; }
    size_t wrote = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
    int flushed = fflush(f);
    fclose(f);
    if (wrote != data.size() || flushed != 0) {
        remove(tmp.c_str());
        fail(err, err_len, "short write to %s (%zu of %zu bytes)", tmp.c_str(), wrote, data.size());
        return DAI_ERR_FILE;
    }
    if (rename(tmp.c_str(), path) != 0) {
        remove(tmp.c_str());
        fail(err, err_len, "cannot rename %s to %s", tmp.c_str(), path);
        return DAI_ERR_FILE;
    }
    return DAI_OK;
}

int read_whole(const char *path, std::string &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    fclose(f);
    return 1;
}

// ---- the smallest ZIP that is a ZIP ---------------------------------------
//
// Stored entries only. DEFLATE would save maybe 60% on a text member, and cost
// a compressor that nothing else in Daidalos needs; method 0 is in the
// specification, every unzip reads it, and a show that opens everywhere beats
// a show that is small. The reader below is the mirror image: end of central
// directory, central directory, member.

uint32_t crc32_of(const uint8_t *data, size_t n) {
    static uint32_t table[256];
    static int built = 0;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::string &s, uint32_t v) { s += (char)(v & 0xFF); s += (char)((v >> 8) & 0xFF); }
void put32(std::string &s, uint32_t v) { put16(s, v & 0xFFFF); put16(s, (v >> 16) & 0xFFFF); }
uint32_t get16(const std::string &s, size_t o) {
    return (uint8_t)s[o] | ((uint32_t)(uint8_t)s[o + 1] << 8);
}
uint32_t get32(const std::string &s, size_t o) { return get16(s, o) | (get16(s, o + 2) << 16); }

struct ZipMember { std::string name, data; };

// The timestamp is a constant, not the clock: two exports of the same show
// must be the same bytes, and a date stamp is the classic way to lose that.
std::string zip_build(const std::vector<ZipMember> &members) {
    std::string out, central;
    for (const ZipMember &m : members) {
        uint32_t crc = crc32_of((const uint8_t *)m.data.data(), m.data.size());
        uint32_t off = (uint32_t)out.size();
        out += "PK\3\4";
        put16(out, 20); put16(out, 0); put16(out, 0);        // version, flags, stored
        put16(out, 0);  put16(out, 0x21);                    // 1980-01-01, fixed
        put32(out, crc); put32(out, (uint32_t)m.data.size());
        put32(out, (uint32_t)m.data.size());
        put16(out, (uint32_t)m.name.size()); put16(out, 0);
        out += m.name;
        out += m.data;

        central += "PK\1\2";
        put16(central, 20); put16(central, 20); put16(central, 0); put16(central, 0);
        put16(central, 0);  put16(central, 0x21);
        put32(central, crc); put32(central, (uint32_t)m.data.size());
        put32(central, (uint32_t)m.data.size());
        put16(central, (uint32_t)m.name.size());
        put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0);
        put32(central, 0); put32(central, off);
        central += m.name;
    }
    uint32_t cd_off = (uint32_t)out.size();
    out += central;
    out += "PK\5\6";
    put16(out, 0); put16(out, 0);
    put16(out, (uint32_t)members.size()); put16(out, (uint32_t)members.size());
    put32(out, (uint32_t)central.size()); put32(out, cd_off);
    put16(out, 0);
    return out;
}

int zip_read_member(const std::string &zip, const char *want, std::string &out) {
    if (zip.size() < 22) return 0;
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22 + 1; i-- > 0; ) {
        if (zip.compare(i, 4, "PK\5\6") == 0) { eocd = i; break; }
        if (zip.size() - i > 65557) break;
    }
    if (eocd == std::string::npos) return 0;
    uint32_t count = get16(zip, eocd + 10);
    size_t   p     = get32(zip, eocd + 16);
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > zip.size() || zip.compare(p, 4, "PK\1\2") != 0) return 0;
        uint32_t method = get16(zip, p + 10);
        uint32_t csize  = get32(zip, p + 20);
        uint32_t nlen   = get16(zip, p + 28);
        uint32_t elen   = get16(zip, p + 30);
        uint32_t clen   = get16(zip, p + 32);
        uint32_t lho    = get32(zip, p + 42);
        std::string name = zip.substr(p + 46, nlen);
        if (name == want) {
            if (method != 0) return 0;            // this writer only stores
            if (lho + 30 > zip.size()) return 0;
            uint32_t lnlen = get16(zip, lho + 26), lelen = get16(zip, lho + 28);
            size_t data = lho + 30 + lnlen + lelen;
            if (data + csize > zip.size()) return 0;
            out.assign(zip, data, csize);
            return 1;
        }
        p += 46 + nlen + elen + clen;
    }
    return 0;
}

// ---- just enough JSON to read back what is written above -------------------
//
// The engine's own parser lives with the glTF reader and is linked into the
// asset archive; the show pipeline is deliberately in the plain one, so it
// carries the eighty lines it needs rather than dragging the asset layer into
// a library that is arithmetic on points and time.

struct JVal {
    enum Kind { NUM, STR, ARR, OBJ } kind = NUM;
    double                        num = 0.0;
    std::string                   str;
    std::vector<JVal>             items;                 // ARR
    std::vector<std::pair<std::string, JVal>> members;   // OBJ

    const JVal *get(const char *key) const {
        if (kind != OBJ) return nullptr;
        for (const auto &m : members) if (m.first == key) return &m.second;
        return nullptr;
    }
    double num_at(const char *key, double def) const {
        const JVal *v = get(key); return (v && v->kind == NUM) ? v->num : def;
    }
    const JVal *at(size_t i) const {
        return (kind == ARR && i < items.size()) ? &items[i] : nullptr;
    }
    size_t size() const { return kind == ARR ? items.size() : 0; }
};

struct JParser {
    const char *p, *end;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool value(JVal &v) {
        ws();
        if (p >= end) return false;
        if (*p == '{') {
            ++p; v.kind = JVal::OBJ; ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                JVal key;
                if (p >= end || *p != '"' || !value(key)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                ++p;
                v.members.emplace_back(key.str, JVal());
                if (!value(v.members.back().second)) return false;
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (*p == '[') {
            ++p; v.kind = JVal::ARR; ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                v.items.emplace_back();
                if (!value(v.items.back())) return false;
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (*p == '"') {
            ++p; v.kind = JVal::STR;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) { ++p; v.str += *p++; }
                else v.str += *p++;
            }
            if (p >= end) return false;
            ++p;
            return true;
        }
        if (!strncmp(p, "true", 4))  { p += 4; v.kind = JVal::NUM; v.num = 1.0; return true; }
        if (!strncmp(p, "false", 5)) { p += 5; v.kind = JVal::NUM; v.num = 0.0; return true; }
        if (!strncmp(p, "null", 4))  { p += 4; v.kind = JVal::NUM; v.num = 0.0; return true; }
        char *stop = nullptr;
        double d = strtod(p, &stop);
        if (stop == p) return false;
        v.kind = JVal::NUM; v.num = d; p = stop;
        return true;
    }
};

int json_parse(const std::string &text, JVal &out) {
    JParser jp{ text.c_str(), text.c_str() + text.size() };
    return jp.value(out) ? 1 : 0;
}

// ---- the settings block, written the same way in both containers -----------

void settings_json(std::string &s, const dai_show_settings *st, const char *indent) {
    char line[256];
    snprintf(line, sizeof(line), "%s\"minDistance\": %s,\n", indent, fstr(st->min_distance_m).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"maxVelocity\": %s,\n", indent, fstr(st->v_max_ms).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"maxAcceleration\": %s,\n", indent, fstr(st->a_max_ms2).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"droneCount\": %u,\n", indent, st->drone_count); s += line;
    snprintf(line, sizeof(line), "%s\"originLat\": %s,\n", indent, dstr(st->show_origin_lat).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"originLon\": %s,\n", indent, dstr(st->show_origin_lon).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"originAmsl\": %s,\n", indent, fstr(st->show_origin_amsl).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"orientation\": %s,\n", indent, fstr(st->show_orientation_deg).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"takeoffAltitude\": %s,\n", indent, fstr(st->takeoff_alt_m).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"fps\": %d,\n", indent, st->fps); s += line;
    snprintf(line, sizeof(line), "%s\"fenceHalfX\": %s,\n", indent, fstr(st->fence_half_x).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"fenceHalfZ\": %s,\n", indent, fstr(st->fence_half_z).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"fenceTop\": %s,\n", indent, fstr(st->fence_top_m).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"minGround\": %s,\n", indent, fstr(st->min_ground_m).c_str()); s += line;
    snprintf(line, sizeof(line), "%s\"seed\": %llu\n", indent, (unsigned long long)st->seed); s += line;
}

void settings_read(const JVal *j, dai_show_settings *st) {
    if (!j || !st) return;
    *st = dai_show_settings_default();
    st->min_distance_m       = (float)j->num_at("minDistance", st->min_distance_m);
    st->v_max_ms             = (float)j->num_at("maxVelocity", st->v_max_ms);
    st->a_max_ms2            = (float)j->num_at("maxAcceleration", st->a_max_ms2);
    st->drone_count          = (uint32_t)j->num_at("droneCount", st->drone_count);
    st->show_origin_lat      = j->num_at("originLat", st->show_origin_lat);
    st->show_origin_lon      = j->num_at("originLon", st->show_origin_lon);
    st->show_origin_amsl     = (float)j->num_at("originAmsl", st->show_origin_amsl);
    st->show_orientation_deg = (float)j->num_at("orientation", st->show_orientation_deg);
    st->takeoff_alt_m        = (float)j->num_at("takeoffAltitude", st->takeoff_alt_m);
    st->fps                  = (int)j->num_at("fps", st->fps);
    st->fence_half_x         = (float)j->num_at("fenceHalfX", st->fence_half_x);
    st->fence_half_z         = (float)j->num_at("fenceHalfZ", st->fence_half_z);
    st->fence_top_m          = (float)j->num_at("fenceTop", st->fence_top_m);
    st->min_ground_m         = (float)j->num_at("minGround", st->min_ground_m);
    st->seed                 = (uint64_t)j->num_at("seed", (double)st->seed);
}

// The plan, rebuilt from a flat key list. Both importers end here, so both
// round trips are the same round trip.
dai_show_plan *plan_from(const std::vector<uint32_t> &counts,
                         const std::vector<dai_show_key> &keys) {
    if (counts.empty() || keys.empty()) return nullptr;
    return dai_show_plan_from_keys((uint32_t)counts.size(), counts.data(), keys.data());
}

} // namespace

extern "C" {

// ---- JSON: the auditable one ----------------------------------------------

dai_result dai_show_export_json(const dai_show_plan *p, const dai_show_settings *s,
                                const char *path, char *err, size_t err_len) {
    if (!p || !s || !path) { fail(err, err_len, "no plan, settings or path"); return DAI_ERR_INVALID_ARG; }

    std::string out;
    out.reserve((size_t)dai_show_plan_drone_count(p) * 256u + 1024u);
    out += "{\n  \"format\": \"daidalos-show\",\n  \"version\": 1,\n";
    out += "  \"settings\": {\n";
    settings_json(out, s, "    ");
    out += "  },\n  \"drones\": [\n";

    uint32_t n = dai_show_plan_drone_count(p);
    char line[256];
    for (uint32_t i = 0; i < n; ++i) {
        out += "    { \"keys\": [";
        uint32_t kc = dai_show_plan_keyframe_count(p, i);
        for (uint32_t k = 0; k < kc; ++k) {
            dai_show_key key;
            if (!dai_show_plan_key_at(p, i, k, &key)) continue;
            snprintf(line, sizeof(line), "%s[%s,%s,%s,%s,%u,%u,%u,%u,%d]",
                     k ? "," : "", fstr(key.t).c_str(), fstr(key.p.x).c_str(),
                     fstr(key.p.y).c_str(), fstr(key.p.z).c_str(),
                     key.p.r, key.p.g, key.p.b, key.p.w, key.profile);
            out += line;
        }
        out += (i + 1 < n) ? "] },\n" : "] }\n";
    }
    out += "  ]\n}\n";
    return write_atomic(path, out, err, err_len);
}

dai_show_plan *dai_show_import_json(const char *path, dai_show_settings *out_settings,
                                    char *err, size_t err_len) {
    std::string text;
    if (!path || !read_whole(path, text)) { fail(err, err_len, "cannot read %s", path ? path : "(null)"); return nullptr; }
    JVal root;
    if (!json_parse(text, root)) { fail(err, err_len, "%s is not valid JSON", path); return nullptr; }
    const JVal *drones = root.get("drones");
    if (!drones || drones->kind != JVal::ARR) { fail(err, err_len, "%s has no drone list", path); return nullptr; }
    if (out_settings) settings_read(root.get("settings"), out_settings);

    std::vector<uint32_t>       counts;
    std::vector<dai_show_key>   keys;
    counts.reserve(drones->size());
    for (size_t i = 0; i < drones->size(); ++i) {
        const JVal *kl = drones->at(i)->get("keys");
        uint32_t c = 0;
        for (size_t k = 0; kl && k < kl->size(); ++k) {
            const JVal *e = kl->at(k);
            if (!e || e->size() < 9) continue;
            dai_show_key key;
            key.t       = (float)e->at(0)->num;
            key.p.x     = (float)e->at(1)->num;
            key.p.y     = (float)e->at(2)->num;
            key.p.z     = (float)e->at(3)->num;
            key.p.r     = (uint8_t)e->at(4)->num;
            key.p.g     = (uint8_t)e->at(5)->num;
            key.p.b     = (uint8_t)e->at(6)->num;
            key.p.w     = (uint8_t)e->at(7)->num;
            key.profile = (int)e->at(8)->num;
            keys.push_back(key);
            ++c;
        }
        counts.push_back(c);
    }
    dai_show_plan *plan = plan_from(counts, keys);
    if (!plan) fail(err, err_len, "%s describes no keyframes", path);
    return plan;
}

// ---- CSV: the one a spreadsheet opens --------------------------------------

dai_result dai_show_export_csv(const dai_show_plan *p, const dai_show_settings *s,
                               const char *path, char *err, size_t err_len) {
    if (!p || !s || !path) { fail(err, err_len, "no plan, settings or path"); return DAI_ERR_INVALID_ARG; }
    const int fps = (s->fps > 0) ? s->fps : 25;
    const uint32_t n = dai_show_plan_drone_count(p);
    const uint32_t frames = (uint32_t)std::floor((double)dai_show_plan_duration(p) * (double)fps) + 1u;

    std::string out;
    out.reserve((size_t)n * frames * 48u + 64u);
    out += "time_s,drone,x,y,z,r,g,b,w\n";
    std::vector<dai_show_point> fleet(n);
    char line[160];
    for (uint32_t f = 0; f < frames; ++f) {
        float t = (float)((double)f / (double)fps);
        dai_show_plan_sample_all(p, t, fleet.data());
        for (uint32_t i = 0; i < n; ++i) {
            const dai_show_point &q = fleet[i];
            snprintf(line, sizeof(line), "%s,%u,%s,%s,%s,%u,%u,%u,%u\n",
                     fstr(t).c_str(), i, fstr(q.x).c_str(), fstr(q.y).c_str(),
                     fstr(q.z).c_str(), q.r, q.g, q.b, q.w);
            out += line;
        }
    }
    return write_atomic(path, out, err, err_len);
}

// ---- .skyc: the one that flies ---------------------------------------------
//
// A ZIP with a show.json in it, laid out the way the Skybrush format
// description does it: an environment, and one entry per drone carrying a
// trajectory of [t, [x, y, z], []] points and a light program. Coordinates go
// out in the format's local NEU frame - north, east, up - which is the
// engine's (z, x, y), a permutation and therefore exact in both directions.
//
// The profile of each segment rides in a "daidalos" extension object next to
// the trajectory. Generic firmware ignores it and flies the straight line
// between two keys, which is what it would do with any other exporter; this
// library reads it back and gets the identical plan, which is what makes the
// round trip test possible at all.

dai_result dai_show_export_skyc(const dai_show_plan *p, const dai_show_settings *s,
                                const char *path, char *err, size_t err_len) {
    if (!p || !s || !path) { fail(err, err_len, "no plan, settings or path"); return DAI_ERR_INVALID_ARG; }
    const uint32_t n = dai_show_plan_drone_count(p);

    std::string j;
    j.reserve((size_t)n * 320u + 1024u);
    j += "{\n  \"version\": 1,\n  \"meta\": { \"generator\": \"Daidalos\" },\n";
    j += "  \"environment\": {\n    \"type\": \"outdoor\",\n    \"location\": {\n";
    j += "      \"origin\": [" + dstr(s->show_origin_lon) + ", " + dstr(s->show_origin_lat) +
         ", " + fstr(s->show_origin_amsl) + "],\n";
    j += "      \"orientation\": " + fstr(s->show_orientation_deg) + ",\n";
    j += "      \"type\": \"neu\"\n    }\n  },\n";
    j += "  \"settings\": {\n";
    settings_json(j, s, "    ");
    j += "  },\n  \"swarm\": { \"drones\": [\n";

    char line[256];
    for (uint32_t i = 0; i < n; ++i) {
        snprintf(line, sizeof(line),
                 "    { \"type\": \"generic\", \"settings\": {\n      \"name\": \"drone %u\",\n", i);
        j += line;
        j += "      \"trajectory\": { \"version\": 1, \"takeoffTime\": 0, \"points\": [";
        uint32_t kc = dai_show_plan_keyframe_count(p, i);
        std::string lights, profiles;
        for (uint32_t k = 0; k < kc; ++k) {
            dai_show_key key;
            if (!dai_show_plan_key_at(p, i, k, &key)) continue;
            snprintf(line, sizeof(line), "%s[%s,[%s,%s,%s],[]]", k ? "," : "",
                     fstr(key.t).c_str(), fstr(key.p.z).c_str(),
                     fstr(key.p.x).c_str(), fstr(key.p.y).c_str());
            j += line;
            snprintf(line, sizeof(line), "%s[%s,%u,%u,%u,%u]", k ? "," : "",
                     fstr(key.t).c_str(), key.p.r, key.p.g, key.p.b, key.p.w);
            lights += line;
            snprintf(line, sizeof(line), "%s%d", k ? "," : "", key.profile);
            profiles += line;
        }
        j += "] },\n";
        j += "      \"lights\": { \"version\": 1, \"colorKeyframes\": [" + lights + "] },\n";
        j += "      \"daidalos\": { \"profiles\": [" + profiles + "] }\n";
        j += (i + 1 < n) ? "    } },\n" : "    } }\n";
    }
    j += "  ] }\n}\n";

    std::vector<ZipMember> members;
    members.push_back(ZipMember{ "show.json", j });
    return write_atomic(path, zip_build(members), err, err_len);
}

dai_show_plan *dai_show_import_skyc(const char *path, dai_show_settings *out_settings,
                                    char *err, size_t err_len) {
    std::string zip;
    if (!path || !read_whole(path, zip)) { fail(err, err_len, "cannot read %s", path ? path : "(null)"); return nullptr; }
    std::string text;
    if (!zip_read_member(zip, "show.json", text)) {
        fail(err, err_len, "%s holds no readable show.json", path);
        return nullptr;
    }
    JVal root;
    if (!json_parse(text, root)) { fail(err, err_len, "the show.json in %s is not valid JSON", path); return nullptr; }
    if (out_settings) settings_read(root.get("settings"), out_settings);

    const JVal *swarm = root.get("swarm");
    const JVal *drones = swarm ? swarm->get("drones") : nullptr;
    if (!drones || drones->kind != JVal::ARR) { fail(err, err_len, "%s has no swarm", path); return nullptr; }

    std::vector<uint32_t>     counts;
    std::vector<dai_show_key> keys;
    for (size_t i = 0; i < drones->size(); ++i) {
        const JVal *set = drones->at(i)->get("settings");
        const JVal *traj = set ? set->get("trajectory") : nullptr;
        const JVal *pts  = traj ? traj->get("points") : nullptr;
        const JVal *lit  = set ? set->get("lights") : nullptr;
        const JVal *cols = lit ? lit->get("colorKeyframes") : nullptr;
        const JVal *ext  = set ? set->get("daidalos") : nullptr;
        const JVal *prof = ext ? ext->get("profiles") : nullptr;
        uint32_t c = 0;
        for (size_t k = 0; pts && k < pts->size(); ++k) {
            const JVal *e = pts->at(k);
            const JVal *xyz = e ? e->at(1) : nullptr;
            if (!e || !xyz || xyz->size() < 3) continue;
            dai_show_key key;
            std::memset(&key, 0, sizeof(key));
            key.t   = (float)e->at(0)->num;
            key.p.z = (float)xyz->at(0)->num;     // north
            key.p.x = (float)xyz->at(1)->num;     // east
            key.p.y = (float)xyz->at(2)->num;     // up
            const JVal *cl = cols ? cols->at(k) : nullptr;
            if (cl && cl->size() >= 5) {
                key.p.r = (uint8_t)cl->at(1)->num;
                key.p.g = (uint8_t)cl->at(2)->num;
                key.p.b = (uint8_t)cl->at(3)->num;
                key.p.w = (uint8_t)cl->at(4)->num;
            }
            const JVal *pf = prof ? prof->at(k) : nullptr;
            key.profile = pf ? (int)pf->num : DAI_SHOW_PROFILE_LINEAR;
            keys.push_back(key);
            ++c;
        }
        counts.push_back(c);
    }
    dai_show_plan *plan = plan_from(counts, keys);
    if (!plan) fail(err, err_len, "%s describes no keyframes", path);
    return plan;
}

} // extern "C"
