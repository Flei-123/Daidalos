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


/* ---- DSX: the open, reviewable container ---------------------------------
 *
 * .skyc above is the format the FIRMWARE eats. This one is the format a human,
 * an authority or another vendor's tool reads: DSX v0.1, profile L1. The two
 * are not redundant - a compiled-only format cannot be diffed or audited, and
 * a sampled-only format loses intent, so DSX carries both and makes the
 * conversion between them normative.
 *
 * Three implementation points are worth a comment each:
 *
 * 1) ROUNDING. DSX positions are rounded to the millimetre with round-half-
 *    away-from-zero, on the EXACT binary64 value. floor(x+0.5) is not that
 *    rule - the addition itself can round up (0.4999... + 0.5 == 1.0 in
 *    binary64) - and neither is a second pass through a %f formatter, which
 *    rounds half-to-even. dsx_round below converts the value to its exact
 *    decimal string and rounds that, the one way that is genuinely away from
 *    zero on the value as stored.
 *
 * 2) THE CURVE. A Daidalos key segment knows its easing profile; a DSX bezier
 *    knows two control points. The exporter fits c1/c2 numerically (golden
 *    section on the squared distance over the segment) so the DSX curve tracks
 *    the Daidalos curve to within a fraction of a millimetre. That is a fit,
 *    not an identity - the normative reduction samples it and rounds to the
 *    millimetre, so the error budget is generous. A straight key needs none of
 *    this and is written as `linear`.
 *
 * 3) WHAT IS ABSENT. The RTH-feasibility map (spec section 7.3) is written as
 *    an empty window list, because the Daidalos planner computes no return
 *    branches. The honest value is "not declared", not a fabricated map.
 */

namespace {

// Round half away from zero on the exact binary64 value, per DSX 4.4.4. The
// Decimal route of the Python reference is not available here, so the exact
// decimal of the value is produced with %.17g (always enough to distinguish a
// binary64 from its neighbours) and the digit walk does the rounding.
long long dsx_round(double x) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", x);
    double back = strtod(buf, nullptr);          // exact decimal of x
    if (back == x) {
        // Round on the decimal string.
        const char *dot = std::strchr(buf, '.');
        int up = 0;
        if (dot) up = (dot[1] - '0') >= 5;
        long long r = (long long)std::fabs(x);
        if (up) ++r;
        return x < 0 ? -r : r;
    }
    // The 17-digit decimal is on the other side of the half-integer than x
    // only at the last ulp; nudge once and take the nearer integer.
    long long r = (long long)std::llround(std::fabs(x));   // llround is half away
    return x < 0 ? -r : r;
}

// Metres with exactly three decimals, formatted FROM the rounded millimetre,
// never from the float a second time (4.4.5).
std::string dsx_metres(double v) {
    long long mm = dsx_round(v * 1000.0);
    long long a = mm < 0 ? -mm : mm;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s%lld.%03lld", mm < 0 ? "-" : "",
                  (long long)(a / 1000), (long long)(a % 1000));
    return buf;
}

// One colour channel, 8 bit, same rule.
int dsx_byte(float v) {
    long long q = dsx_round((double)v);
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    return (int)q;
}

// The easing the keys carry, evaluated the same way dai_show_plan does. A
// local copy rather than a link: this file is arithmetic on the plan's public
// surface and pulls in nothing from the solver's internals.
// The profile a segment was flown with is stored on the DESTINATION key of
// the leg - which, in the exporter's loop below, is `ka`, the key the fade
// lands on. Reading it off `kb` (the PREVIOUS key) reads the hold key that
// stands still before the move, and that one is always LINEAR, so the colour
// ramp was eased with a straight line where the motion was a smoothstep. The
// parity test caught it as a 25-level residual exactly where the easing bends.

float dsx_ease(int profile, float u) {
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    switch (profile) {
    case DAI_SHOW_PROFILE_SMOOTH: {
        // cubic hermite, zero slope at both ends
        return u * u * (3.0f - 2.0f * u);
    }
    case DAI_SHOW_PROFILE_SMOOTH_LEFT:
        return u * (2.0f - u);
    case DAI_SHOW_PROFILE_SMOOTH_RIGHT:
        return u * u;
    default:
        return u;
    }
}

void jstr(std::string &o, const char *s) {
    o += '"';
    for (const char *c = s; *c; ++c) {
        if (*c == '"' || *c == '\\') o += '\\';
        if ((unsigned char)*c >= 0x20) o += *c; else if (*c == '\n') o += "\\n";
    }
    o += '"';
}
void jnum(std::string &o, double v) { o += dstr(v); }
void jint(std::string &o, long long v) { char b[24]; std::snprintf(b, sizeof(b), "%lld", v); o += b; }

// Fit bezier control points (c1, c2) to a Daidalos eased segment so the DSX
// curve approximates the Daidalos curve. The eased path is a straight line in
// space with a nonlinear time law; c1/c2 sit on that line and only their
// parameter matters, which collapses the fit to two scalars. Golden-section
// search on the squared position error over the segment, in binary64.
void fit_bezier(const dai_show_point *a, const dai_show_point *b, int profile,
                float t0, float t1, float *c1x, float *c1y, float *c1z,
                float *c2x, float *c2y, float *c2z) {
    double ax = a->x, ay = a->y, az = a->z;
    double bx = b->x, by = b->y, bz = b->z;
    double dx = bx - ax, dy = by - ay, dz = bz - az;
    auto target_s = [&](double u) {
        return (double)dsx_ease(profile, (float)u);
    };
    // squared error between the DSX bezier at parameter c1t,c2t and the
    // Daidalos eased point, summed over a few interior samples
    auto err2 = [&](double e1, double e2) {
        double sum = 0.0;
        for (int k = 1; k <= 6; ++k) {
            double u = k / 7.0;
            double s = target_s(u);
            double tx = ax + dx * s, ty = ay + dy * s, tz = az + dz * s;
            double omu = 1.0 - u;
            double bezx = omu*omu*omu*ax + 3*omu*omu*u*(ax+dx*e1) + 3*omu*u*u*(ax+dx*e2) + u*u*u*bx;
            double bezy = omu*omu*omu*ay + 3*omu*omu*u*(ay+dy*e1) + 3*omu*u*u*(ay+dy*e2) + u*u*u*by;
            double bezz = omu*omu*omu*az + 3*omu*omu*u*(az+dz*e1) + 3*omu*u*u*(az+dz*e2) + u*u*u*bz;
            double ex = bezx-tx, ey = bezy-ty, ez = bezz-tz;
            sum += ex*ex + ey*ey + ez*ez;
        }
        return sum;
    };
    // Two independent 1-D minimisations over e1 and e2, a few rounds.
    double e1 = 0.0, e2 = 1.0;
    if (std::fabs(dx) + std::fabs(dy) + std::fabs(dz) < 1e-9) {
        e1 = 0.0; e2 = 1.0;
    } else {
        for (int round = 0; round < 3; ++round) {
            for (int which = 0; which < 2; ++which) {
                double lo = (which == 0) ? -0.5 : 0.0, hi = (which == 0) ? 0.5 : 1.5;
                double gr = 0.6180339887498948482;
                double c = hi - gr * (hi - lo), d = lo + gr * (hi - lo);
                for (int it = 0; it < 48; ++it) {
                    double ec = (which == 0) ? err2(c, e2) : err2(e1, c);
                    double ed = (which == 0) ? err2(d, e2) : err2(e1, d);
                    if (ec < ed) hi = d; else lo = c;
                    c = hi - gr * (hi - lo);
                    d = lo + gr * (hi - lo);
                }
                if (which == 0) e1 = 0.5 * (lo + hi); else e2 = 0.5 * (lo + hi);
            }
        }
    }
    *c1x = (float)(ax + dx * e1); *c1y = (float)(ay + dy * e1); *c1z = (float)(az + dz * e1);
    *c2x = (float)(ax + dx * e2); *c2y = (float)(ay + dy * e2); *c2z = (float)(az + dz * e2);
}

} // namespace

dai_result dai_show_export_dsx(const dai_show_plan *p, const dai_show_settings *s,
                               const char *path, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!p || !s || !path) return DAI_ERR_INVALID_ARG;
    uint32_t n = dai_show_plan_drone_count(p);
    if (!n) { fail(err, err_len, "the plan has no drones"); return DAI_ERR_STATE; }

    std::vector<ZipMember> members;

    // ---- devices/example dsxp: a fixed block, Daidalos' own profile -------
    std::string dsxp;
    dsxp += "{\n";
    dsxp += "  \"dsxp\": \"0.1\",\n";
    dsxp += "  \"device_type_id\": \"a3f9c1e2-4b7d-4f5a-9c2e-1d8b6a4e7f30\",\n";
    dsxp += "  \"device_class\": \"aircraft\",\n";
    dsxp += "  \"manufacturer\": \"FleiTec\",\n";
    dsxp += "  \"model\": \"DAIDALOS-SHOW-1\",\n";
    dsxp += "  \"revision\": { \"id\": \"rev1\", \"date\": \"2026-08-15\", \"modified_by\": \"daidalos\" },\n";
    dsxp += "  \"light\": { \"channels\": [\"R\",\"G\",\"B\",\"W\"],\n";
    dsxp += "    \"color_space\": { \"mode\": \"sRGB\", \"gamma\": 2.2, \"dimming_curve\": \"linear\" },\n";
    dsxp += "    \"pwm_hz\": null, \"beam_angle_deg\": null },\n";
    dsxp += "  \"flight\": {\n";
    dsxp += "    \"max_speed_xy_ms\": "; jnum(dsxp, s->v_max_ms); dsxp += ",\n";
    dsxp += "    \"max_speed_z_up_ms\": "; jnum(dsxp, s->v_max_ms); dsxp += ",\n";
    dsxp += "    \"max_speed_z_down_ms\": "; jnum(dsxp, s->v_max_ms); dsxp += ",\n";
    dsxp += "    \"max_accel_xy_ms2\": "; jnum(dsxp, s->a_max_ms2); dsxp += ",\n";
    dsxp += "    \"max_accel_z_ms2\": "; jnum(dsxp, s->a_max_ms2); dsxp += ",\n";
    dsxp += "    \"max_yaw_rate_dps\": null,\n";
    dsxp += "    \"min_nav_altitude_m\": "; jnum(dsxp, s->min_ground_m); dsxp += ",\n";
    dsxp += "    \"endurance_s\": null, \"mass_kg\": null,\n";
    dsxp += "    \"position_accuracy_m\": { \"horizontal\": 0.1, \"vertical\": 0.2, \"requires\": \"rtk_fixed\" } },\n";
    dsxp += "  \"payload_slots\": [],\n";
    dsxp += "  \"modes\": [ { \"name\": \"rgbw-key\",\n";
    dsxp += "    \"trajectory_rate_hz\": { \"required\": null, \"min\": null, \"max\": null },\n";
    dsxp += "    \"light_rate_hz\": { \"required\": null, \"min\": null, \"max\": null },\n";
    dsxp += "    \"channels\": [\"R\",\"G\",\"B\",\"W\"],\n";
    dsxp += "    \"capabilities\": [\"dsx.core\",\"dsx.light.rgbw\"] } ],\n";
    dsxp += "  \"firmware\": []\n}\n";
    members.push_back({ "devices/daidalos-show-1.dsxp", dsxp });

    // ---- geo/fence.json: the show box as a GeoJSON polygon around origin --
    if (s->fence_half_x > 0.0f && s->fence_half_z > 0.0f) {
        std::string gj = "{\n  \"type\": \"FeatureCollection\",\n  \"features\": [\n    {\n";
        gj += "      \"type\": \"Feature\",\n";
        gj += "      \"properties\": { \"name\": \"show geofence\", \"alt_band_m\": [";
        jnum(gj, s->min_ground_m); gj += ", "; jnum(gj, s->fence_top_m); gj += "] },\n";
        gj += "      \"geometry\": { \"type\": \"Polygon\", \"coordinates\": [ [ [";
        jnum(gj, -(double)s->fence_half_x); gj += ", "; jnum(gj, -(double)s->fence_half_z); gj += "], [";
        jnum(gj,  (double)s->fence_half_x); gj += ", "; jnum(gj, -(double)s->fence_half_z); gj += "], [";
        jnum(gj,  (double)s->fence_half_x); gj += ", "; jnum(gj,  (double)s->fence_half_z); gj += "], [";
        jnum(gj, -(double)s->fence_half_x); gj += ", "; jnum(gj,  (double)s->fence_half_z); gj += "], [";
        jnum(gj, -(double)s->fence_half_x); gj += ", "; jnum(gj, -(double)s->fence_half_z); gj += "] ] ] }\n";
        gj += "    }\n  ]\n}\n";
        members.push_back({ "geo/fence.json", gj });
    }

    // ---- one segment trajectory + one light program per drone --------------
    const double dur = (double)dai_show_plan_duration(p);
    for (uint32_t d = 0; d < n; ++d) {
        char name[64];
        std::snprintf(name, sizeof(name), "traj/%04u.json", (unsigned)(d + 1));
        std::string tj;
        tj += "{\n  \"kind\": \"segments\",\n  \"interp\": \"bezier\",\n  \"start_ms\": 0,\n";
        uint32_t kc = dai_show_plan_keyframe_count(p, d);
        dai_show_key k0;
        dai_show_plan_key_at(p, d, 0, &k0);
        tj += "  \"start_point\": [";
        tj += dsx_metres(k0.p.x); tj += ", "; tj += dsx_metres(k0.p.y); tj += ", "; tj += dsx_metres(k0.p.z);
        tj += "],\n  \"segments\": [\n";
        for (uint32_t k = 1; k < kc; ++k) {
            dai_show_key ka, kb;
            dai_show_plan_key_at(p, d, k - 1, &ka);
            dai_show_plan_key_at(p, d, k,     &kb);
            double dt = (double)kb.t - (double)ka.t;
            if (dt <= 0.0) continue;
            long long dt_ms = dsx_round(dt * 1000.0);
            tj += "    { \"dt_ms\": "; jint(tj, dt_ms);
            // A straight key is linear; an eased one is a fitted bezier.
            int linear = (kb.profile == DAI_SHOW_PROFILE_LINEAR);
            float dx = kb.p.x - ka.p.x, dy = kb.p.y - ka.p.y, dz = kb.p.z - ka.p.z;
            if (linear || (std::fabs(dx) < 1e-6f && std::fabs(dy) < 1e-6f && std::fabs(dz) < 1e-6f)) {
                tj += ", \"type\": \"linear\", \"p\": [";
            } else {
                float c1x, c1y, c1z, c2x, c2y, c2z;
                fit_bezier(&ka.p, &kb.p, kb.profile, ka.t, kb.t,
                           &c1x, &c1y, &c1z, &c2x, &c2y, &c2z);
                tj += ", \"type\": \"bezier\", \"p\": [";
                tj += dsx_metres(kb.p.x); tj += ", "; tj += dsx_metres(kb.p.y); tj += ", "; tj += dsx_metres(kb.p.z);
                tj += "], \"c1\": [";
                tj += dsx_metres(c1x); tj += ", "; tj += dsx_metres(c1y); tj += ", "; tj += dsx_metres(c1z);
                tj += "], \"c2\": [";
                tj += dsx_metres(c2x); tj += ", "; tj += dsx_metres(c2y); tj += ", "; tj += dsx_metres(c2z);
                tj += "] }";
                if (k + 1 < kc) tj += ",";
                tj += "\n";
                continue;
            }
            tj += dsx_metres(kb.p.x); tj += ", "; tj += dsx_metres(kb.p.y); tj += ", "; tj += dsx_metres(kb.p.z);
            tj += "] }";
            if (k + 1 < kc) tj += ",";
            tj += "\n";
        }
        tj += "  ]\n}\n";
        members.push_back({ name, tj });

        // light: set/fade ops from the keyframes' colours
        char lname[64];
        std::snprintf(lname, sizeof(lname), "light/%04u.json", (unsigned)(d + 1));
        std::string lj;
        lj += "{\n  \"kind\": \"light_program\",\n  \"channels\": [\"R\",\"G\",\"B\",\"W\"],\n";
        lj += "  \"color_space\": \"sRGB\",\n  \"ops\": [\n";
        // One op per keyframe whose colour CHANGED. The Daidalos plan eases the
        // colour with the SEGMENT's motion profile; a DSX fade is linear, so
        // the ramp is subdivided into 50 ms pieces - each a linear fade - which
        // tracks the eased ramp to within a handful of quantisation levels. A
        // single linear fade would hold the old colour too long and then catch
        // up; that is a different show, not the same one rounded.
        int first_op = 1;
        for (uint32_t k = 0; k < kc; ++k) {
            dai_show_key ka, kb;
            dai_show_plan_key_at(p, d, k, &ka);
            if (k > 0) {
                dai_show_plan_key_at(p, d, k - 1, &kb);
                if (ka.p.r == kb.p.r && ka.p.g == kb.p.g && ka.p.b == kb.p.b && ka.p.w == kb.p.w)
                    continue;
            }
            if (k > 0) {
                dai_show_key pk; dai_show_plan_key_at(p, d, k - 1, &pk);
                double span = (double)ka.t - (double)pk.t;
                if (span > 0.0) {
                    // The Daidalos plan eases the colour with the segment's
                    // motion profile; a DSX fade is linear, so the ramp is
                    // written as a chain of short PIECEWISE-CONSTANT steps.
                    // Each is a zero-length set AT the sample instant it
                    // stands for, holding the eased value there. A chain of
                    // fades reads half a piece out of date at every sample -
                    // the evaluator interpolates between the value in effect
                    // and the target over the op's own window - and that lag
                    // is exactly the residual the parity test measured. The
                    // steps are the value AT the instant, which is what the
                    // reduction asks for; the smoothness of the light is the
                    // hardware's dimming curve, not the file's.
                    int steps = (int)dsx_round(span * 1000.0 / 50.0);
                    if (steps < 1) steps = 1;
                    for (int st = 1; st <= steps; ++st) {
                        double u = (double)st / (double)steps;
                        double sc = dsx_ease(ka.profile, (float)u);
                        int rr = dsx_byte(pk.p.r + (ka.p.r - pk.p.r) * (float)sc);
                        int gg = dsx_byte(pk.p.g + (ka.p.g - pk.p.g) * (float)sc);
                        int bb = dsx_byte(pk.p.b + (ka.p.b - pk.p.b) * (float)sc);
                        int ww = dsx_byte(pk.p.w + (ka.p.w - pk.p.w) * (float)sc);
                        long long t_at = dsx_round(((double)pk.t + span * u) * 1000.0);
                        if (!first_op) lj += ",\n";
                        first_op = 0;
                        lj += "    { \"t_ms\": "; jint(lj, t_at);
                        lj += ", \"op\": \"set\"";
                        lj += ", \"channels\": { \"R\": "; jint(lj, rr);
                        lj += ", \"G\": "; jint(lj, gg);
                        lj += ", \"B\": "; jint(lj, bb);
                        lj += ", \"W\": "; jint(lj, ww); lj += " } }";
                    }
                } else {
                    if (!first_op) lj += ",\n";
                    first_op = 0;
                    long long t_ms = dsx_round((double)ka.t * 1000.0);
                    lj += "    { \"t_ms\": "; jint(lj, t_ms);
                    lj += ", \"op\": \"set\", \"channels\": { \"R\": "; jint(lj, ka.p.r);
                    lj += ", \"G\": "; jint(lj, ka.p.g);
                    lj += ", \"B\": "; jint(lj, ka.p.b);
                    lj += ", \"W\": "; jint(lj, ka.p.w); lj += " } }";
                }
            } else {
                long long t_ms = dsx_round((double)ka.t * 1000.0);
                if (!first_op) lj += ",\n";
                first_op = 0;
                lj += "    { \"t_ms\": "; jint(lj, t_ms);
                lj += ", \"op\": \"set\", \"channels\": { \"R\": "; jint(lj, ka.p.r);
                lj += ", \"G\": "; jint(lj, ka.p.g);
                lj += ", \"B\": "; jint(lj, ka.p.b);
                lj += ", \"W\": "; jint(lj, ka.p.w); lj += " } }";
            }
        }
        lj += "\n  ]\n}\n";
        members.push_back({ lname, lj });
    }

    // ---- the manifest ------------------------------------------------------
    std::string mj;
    mj += "{\n";
    mj += "  \"dsx\": \"0.1\",\n  \"profile\": \"L1\",\n";
    mj += "  \"show\": { \"title\": \"Daidalos show\", \"duration_ms\": ";
    jint(mj, dsx_round(dur * 1000.0));
    mj += " },\n";
    mj += "  \"frame\": { \"type\": \"ENU\", \"handedness\": \"right\", \"units\": \"m\",\n";
    mj += "    \"bearing_deg\": "; jnum(mj, s->show_orientation_deg); mj += ",\n";
    mj += "    \"origin\": { \"lat\": "; mj += dstr(s->show_origin_lat);
    mj += ", \"lon\": "; mj += dstr(s->show_origin_lon);
    mj += ", \"alt_m\": "; jnum(mj, s->show_origin_amsl);
    mj += ", \"alt_ref\": \"AMSL\", \"geoid\": \"EGM2008\", \"datum\": \"WGS84\" } },\n";
    mj += "  \"time\": { \"base\": \"ms\", \"start\": { \"mode\": \"countdown\" },\n";
    mj += "    \"time_source\": { \"primary\": \"gnss\", \"holdover\": { \"source\": \"rtc\", \"max_drift_ms_per_min\": null } } },\n";

    // fleet with declared envelope measured from the plan
    float peak_v = 0.0f, min_sep = 1e30f;
    {
        // envelope over the whole timeline at the export fps
        double fps = s->fps > 0 ? s->fps : 25;
        std::vector<dai_show_point> a(n), b2(n);
        double t = 0.0, dt = 1.0 / fps;
        for (; t <= dur; t += dt) {
            dai_show_plan_sample_all(p, (float)t, a.data());
            if (t > 0.0) {
                for (uint32_t i = 0; i < n; ++i) {
                    float dx = a[i].x - b2[i].x, dy = a[i].y - b2[i].y, dz = a[i].z - b2[i].z;
                    float v = std::sqrt(dx*dx + dy*dy + dz*dz) / (float)dt;
                    if (v > peak_v) peak_v = v;
                }
            }
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t j2 = i + 1; j2 < n; ++j2) {
                    float dx = a[i].x - a[j2].x, dy = a[i].y - a[j2].y, dz = a[i].z - a[j2].z;
                    float dd = std::sqrt(dx*dx + dy*dy + dz*dz);
                    if (dd < min_sep) min_sep = dd;
                }
            b2 = a;
        }
    }
    if (min_sep > 1e29f) min_sep = 0.0f;
    mj += "  \"fleet\": [ { \"id\": \"main\", \"count\": "; jint(mj, n); mj += ",\n";
    mj += "    \"device_profile\": \"devices/daidalos-show-1.dsxp\",\n";
    mj += "    \"device_type_id\": \"a3f9c1e2-4b7d-4f5a-9c2e-1d8b6a4e7f30\",\n";
    mj += "    \"device_mode\": \"rgbw-key\",\n";
    mj += "    \"declared_envelope\": { \"peak_speed_xy_ms\": "; jnum(mj, peak_v);
    mj += ", \"peak_speed_z_up_ms\": "; jnum(mj, peak_v);
    mj += ", \"peak_speed_z_down_ms\": "; jnum(mj, peak_v);
    mj += ", \"peak_accel_xy_ms2\": "; jnum(mj, s->a_max_ms2);
    mj += ", \"peak_accel_z_ms2\": "; jnum(mj, s->a_max_ms2);
    mj += ", \"min_separation_m\": "; jnum(mj, min_sep);
    mj += ", \"uses_channels\": [\"R\",\"G\",\"B\",\"W\"] } } ],\n";

    // drones: homes from the first keyframe
    mj += "  \"drones\": [\n";
    for (uint32_t d = 0; d < n; ++d) {
        dai_show_key k0;
        dai_show_plan_key_at(p, d, 0, &k0);
        mj += "    { \"id\": "; jint(mj, d + 1); mj += ", \"fleet\": \"main\",\n";
        mj += "      \"slot\": null, \"home\": { \"x\": "; jnum(mj, k0.p.x);
        mj += ", \"y\": "; jnum(mj, k0.p.y); mj += ", \"z\": "; jnum(mj, k0.p.z);
        mj += ", \"heading_deg\": 0.0 },\n";
        char tn[24], ln[24];
        std::snprintf(tn, sizeof(tn), "traj/%04u.json", (unsigned)(d + 1));
        std::snprintf(ln, sizeof(ln), "light/%04u.json", (unsigned)(d + 1));
        mj += "      \"trajectory\": "; jstr(mj, tn);
        mj += ", \"light\": "; jstr(mj, ln); mj += " }";
        if (d + 1 < n) mj += ",";
        mj += "\n";
    }
    mj += "  ],\n";

    mj += "  \"safety\": { \"min_separation_m\": "; jnum(mj, s->min_distance_m);
    if (s->fence_half_x > 0.0f) mj += ", \"geofence\": \"geo/fence.json\"";
    mj += " },\n";
    mj += "  \"termination\": { \"channel\": \"independent\",\n";
    mj += "    \"escalation\": [\"hold\",\"coordinated_rth\",\"land_in_place\",\"disarm\"],\n";
    mj += "    \"coordinated_rth\": { \"precomputed\": false },\n";
    // The RTH-feasibility windows stay empty on purpose: the planner computes
    // no return branches, and an empty map is the honest value.
    mj += "    \"rth_availability\": { \"windows\": [] },\n";
    mj += "    \"geofence\": {\n";
    mj += "      \"soft\": { \"type\": \"bubble\", \"radius_m\": 4.0, \"action\": \"auto_land\", \"timeout_ms\": 1500 },\n";
    mj += "      \"hard\": { \"type\": \"polygon\", \"ref\": \"geo/fence.json\", \"action\": \"disarm\" } },\n";
    mj += "    \"link_loss\": { \"heartbeat_timeout_ms\": 5000, \"action_on_loss\": \"land_in_place\" } },\n";
    mj += "  \"provenance\": { \"created_by\": { \"tool\": \"daidalos\", \"version\": \"2026.08.15\", \"date\": \"2026-08-15\" } }\n";
    mj += "}\n";
    members.push_back({ "show.json", mj });

    std::string zip = zip_build(members);
    return write_atomic(path, zip, err, err_len);
}


} // extern "C"
