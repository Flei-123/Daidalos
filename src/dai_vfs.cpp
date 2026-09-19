// dai_vfs.cpp - the one read path, and the archive it reads out of.
//
// See include/dai_vfs.h for the byte layout. The two halves here are:
//
//   MOUNTS + READ   a list of sources, searched by priority. A directory
//                   source is an fopen; an archive source is a directory
//                   table in memory and an fseek.
//
//   PACK WRITER     streams an archive into a file, optionally after copying
//                   a runtime binary in front of it. Streaming rather than
//                   building it in RAM because an assets folder is allowed to
//                   be bigger than this machine's memory.
//
// No exceptions, no RTTI: the same flags as the rest of the engine.

#include "dai_vfs.h"

#include <algorithm>
#include <string>
#include <vector>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>   // mingw has it too - one directory API for both
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#define DAI_FSEEK(f, off, whence) _fseeki64((f), (long long)(off), (whence))
#define DAI_FTELL(f)              ((int64_t)_ftelli64(f))
#else
#include <unistd.h>
#define DAI_FSEEK(f, off, whence) fseeko((f), (off_t)(off), (whence))
#define DAI_FTELL(f)              ((int64_t)ftello(f))
#endif

namespace {

// ---------------------------------------------------------------- small bits

char g_err[512] = { 0 };

void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

void out_err(char *err, size_t err_len, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::snprintf(g_err, sizeof(g_err), "%s", buf);
    if (err && err_len) std::snprintf(err, err_len, "%s", buf);
}

uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr32(p + 4, (uint32_t)(v >> 32));
}

uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }
uint64_t align4_64(uint64_t v) { return (v + 3u) & ~(uint64_t)3u; }

bool is_dir(const std::string &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

// A virtual path is '/' separated, relative, and cannot leave the mount.
// ".." is REJECTED rather than resolved: the only way one gets into a scene
// file is a bug or an attack, and quietly normalising it hides both.
bool norm_path(const char *in, std::string &out) {
    out.clear();
    if (!in || !*in) return false;
    std::string s(in);
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '\\') s[i] = '/';
    // Strip leading "./" and '/'; collapse repeated separators.
    size_t i = 0;
    while (i < s.size() && (s[i] == '/' || (s[i] == '.' && i + 1 < s.size() && s[i + 1] == '/')))
        i += (s[i] == '/') ? 1 : 2;
    std::string t;
    t.reserve(s.size());
    bool prev_slash = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '/') {
            if (prev_slash) continue;
            prev_slash = true;
        } else prev_slash = false;
        t += c;
    }
    if (t.empty() || t.size() >= DAI_VFS_PATH_MAX) return false;
    if (t.back() == '/') return false;
    // No drive letters, no ".." anywhere as a component.
    if (t.size() > 1 && t[1] == ':') return false;
    size_t start = 0;
    while (start <= t.size()) {
        size_t sl = t.find('/', start);
        std::string comp = t.substr(start, sl == std::string::npos ? std::string::npos : sl - start);
        if (comp == "..") return false;
        if (sl == std::string::npos) break;
        start = sl + 1;
    }
    out.swap(t);
    return true;
}

int64_t file_size_of(FILE *f) {
    int64_t cur = DAI_FTELL(f);
    if (DAI_FSEEK(f, 0, SEEK_END) != 0) return -1;
    int64_t n = DAI_FTELL(f);
    DAI_FSEEK(f, cur, SEEK_SET);
    return n;
}

// ---------------------------------------------------------------- the mounts

struct PackEntry {
    std::string path;
    uint64_t    offset;   // absolute inside the file
    uint32_t    length;
    uint32_t    hash;
};

struct Mount {
    int         priority = 0;
    int         seq = 0;          // mount order, for a stable tie break
    bool        archive = false;
    std::string root;             // directory, or archive file path
    std::vector<PackEntry> dir;   // archive only, sorted by path
};

std::vector<Mount> g_mounts;
int g_seq = 0;

// Highest priority first; among equals the LAST mounted wins, so a patch
// folder mounted after the shipped archive at the same priority still wins.
bool mount_less(const Mount &a, const Mount &b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.seq > b.seq;
}

const PackEntry *find_in(const Mount &m, const std::string &path) {
    size_t lo = 0, hi = m.dir.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = m.dir[mid].path.compare(path);
        if (c == 0) return &m.dir[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

// Reads the trailer + header + directory of `file`. Returns false with a
// reason in g_err (and `err`) when there is no archive - which is the normal
// answer for a runtime binary that has not been exported yet.
bool read_directory(const std::string &file, std::vector<PackEntry> &out,
                    dai_pack_info *info, char *err, size_t err_len) {
    out.clear();
    FILE *f = std::fopen(file.c_str(), "rb");
    if (!f) { out_err(err, err_len, "cannot open '%s'", file.c_str()); return false; }
    int64_t fsz = file_size_of(f);
    if (fsz < (int64_t)(DAI_PACK_HEADER_BYTES + DAI_PACK_TRAILER_BYTES)) {
        out_err(err, err_len, "'%s' is too small to hold an archive", file.c_str());
        std::fclose(f); return false;
    }

    uint8_t trailer[DAI_PACK_TRAILER_BYTES];
    if (DAI_FSEEK(f, fsz - (int64_t)DAI_PACK_TRAILER_BYTES, SEEK_SET) != 0 ||
        std::fread(trailer, 1, sizeof(trailer), f) != sizeof(trailer)) {
        out_err(err, err_len, "'%s': cannot read the trailer", file.c_str());
        std::fclose(f); return false;
    }
    if (std::memcmp(trailer + 8, DAI_PACK_TRAILER_MAGIC, 8) != 0) {
        out_err(err, err_len, "'%s' has no archive (no %s at the end)",
                file.c_str(), DAI_PACK_TRAILER_MAGIC);
        std::fclose(f); return false;
    }
    uint64_t base = rd64(trailer);
    if (base + DAI_PACK_HEADER_BYTES + DAI_PACK_TRAILER_BYTES > (uint64_t)fsz) {
        out_err(err, err_len, "'%s': archive offset %llu is past the end",
                file.c_str(), (unsigned long long)base);
        std::fclose(f); return false;
    }

    uint8_t head[DAI_PACK_HEADER_BYTES];
    if (DAI_FSEEK(f, (int64_t)base, SEEK_SET) != 0 ||
        std::fread(head, 1, sizeof(head), f) != sizeof(head)) {
        out_err(err, err_len, "'%s': cannot read the header", file.c_str());
        std::fclose(f); return false;
    }
    if (std::memcmp(head, DAI_PACK_MAGIC, 8) != 0) {
        out_err(err, err_len, "'%s': bad header magic at %llu",
                file.c_str(), (unsigned long long)base);
        std::fclose(f); return false;
    }
    uint32_t version   = rd32(head + 8);
    uint32_t count     = rd32(head + 12);
    uint32_t dir_off   = rd32(head + 16);
    uint32_t dir_bytes = rd32(head + 20);
    uint32_t data_off  = rd32(head + 24);
    uint32_t arch_len  = rd32(head + 28);
    if (version != DAI_PACK_VERSION) {
        out_err(err, err_len, "'%s': archive version %u, this build reads %u",
                file.c_str(), version, DAI_PACK_VERSION);
        std::fclose(f); return false;
    }
    if (base + (uint64_t)arch_len > (uint64_t)fsz ||
        (uint64_t)dir_off + dir_bytes > arch_len) {
        out_err(err, err_len, "'%s': archive header describes %u bytes that are not there",
                file.c_str(), arch_len);
        std::fclose(f); return false;
    }

    std::vector<uint8_t> dir((size_t)dir_bytes);
    if (dir_bytes) {
        if (DAI_FSEEK(f, (int64_t)(base + dir_off), SEEK_SET) != 0 ||
            std::fread(dir.data(), 1, dir_bytes, f) != dir_bytes) {
            out_err(err, err_len, "'%s': cannot read the directory", file.c_str());
            std::fclose(f); return false;
        }
    }
    std::fclose(f);

    out.reserve(count);
    size_t p = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 16 > dir.size()) {
            out_err(err, err_len, "'%s': directory ends after %u of %u entries",
                    file.c_str(), i, count);
            return false;
        }
        uint32_t plen = rd32(&dir[p]);
        PackEntry e;
        e.offset = base + rd32(&dir[p + 4]);
        e.length = rd32(&dir[p + 8]);
        e.hash   = rd32(&dir[p + 12]);
        p += 16;
        if (plen == 0 || plen >= DAI_VFS_PATH_MAX || p + plen > dir.size()) {
            out_err(err, err_len, "'%s': entry %u has a bad path length %u",
                    file.c_str(), i, plen);
            return false;
        }
        e.path.assign((const char *)&dir[p], plen);
        p += align4(plen);
        if (e.offset + e.length > base + arch_len) {
            out_err(err, err_len, "'%s': entry '%s' points outside the archive",
                    file.c_str(), e.path.c_str());
            return false;
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(),
              [](const PackEntry &a, const PackEntry &b) { return a.path < b.path; });

    if (info) {
        info->base = base;
        info->file_bytes = (uint64_t)fsz;
        info->version = version;
        info->entry_count = count;
        info->archive_bytes = arch_len;
        uint64_t payload = 0;
        for (size_t i = 0; i < out.size(); ++i) payload += out[i].length;
        info->payload_bytes = (uint32_t)payload;
    }
    (void)data_off;
    return true;
}

void *read_whole(FILE *f, size_t n, size_t *out_size) {
    uint8_t *buf = (uint8_t *)std::malloc(n + 1);
    if (!buf) { set_err("out of memory (%zu bytes)", n + 1); return nullptr; }
    if (n && std::fread(buf, 1, n, f) != n) {
        std::free(buf);
        set_err("short read");
        return nullptr;
    }
    buf[n] = 0;
    if (out_size) *out_size = n;
    return buf;
}

} // namespace

// ------------------------------------------------------------------ mounting

extern "C" {

dai_result dai_vfs_mount_dir(const char *dir, int priority) {
    if (!dir || !*dir) return DAI_ERR_INVALID_ARG;
    std::string d(dir);
    while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
    if (!is_dir(d)) { set_err("'%s' is not a directory", d.c_str()); return DAI_ERR_FILE; }
    Mount m;
    m.priority = priority;
    m.seq = ++g_seq;
    m.archive = false;
    m.root = d;
    g_mounts.push_back(m);
    std::stable_sort(g_mounts.begin(), g_mounts.end(), mount_less);
    return DAI_OK;
}

dai_result dai_vfs_mount_archive(const char *path, int priority, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    std::string file;
    if (!path || !*path || std::strcmp(path, "self") == 0) {
        char self[1024];
        if (!dai_vfs_self_path(self, sizeof(self))) {
            out_err(err, err_len, "cannot find my own executable");
            return DAI_ERR_FILE;
        }
        file = self;
    } else file = path;

    Mount m;
    if (!read_directory(file, m.dir, nullptr, err, err_len)) return DAI_ERR_FILE;
    m.priority = priority;
    m.seq = ++g_seq;
    m.archive = true;
    m.root = file;
    g_mounts.push_back(m);
    std::stable_sort(g_mounts.begin(), g_mounts.end(), mount_less);
    return DAI_OK;
}

void dai_vfs_unmount_all(void) { g_mounts.clear(); }

uint32_t dai_vfs_mount_count(void) { return (uint32_t)g_mounts.size(); }

const char *dai_vfs_mount_name(uint32_t index) {
    static char buf[1024];
    buf[0] = 0;
    if (index >= g_mounts.size()) return buf;
    std::snprintf(buf, sizeof(buf), "%s:%s", g_mounts[index].archive ? "pack" : "dir",
                  g_mounts[index].root.c_str());
    return buf;
}

// ------------------------------------------------------------------- reading

void *dai_vfs_read(const char *path, size_t *out_size) {
    if (out_size) *out_size = 0;
    std::string p;
    if (!norm_path(path, p)) {
        set_err("'%s' is not a usable path", path ? path : "(null)");
        return nullptr;
    }
    for (size_t i = 0; i < g_mounts.size(); ++i) {
        const Mount &m = g_mounts[i];
        if (m.archive) {
            const PackEntry *e = find_in(m, p);
            if (!e) continue;
            FILE *f = std::fopen(m.root.c_str(), "rb");
            if (!f) continue;
            if (DAI_FSEEK(f, (int64_t)e->offset, SEEK_SET) != 0) { std::fclose(f); continue; }
            void *b = read_whole(f, e->length, out_size);
            std::fclose(f);
            if (b) return b;
        } else {
            std::string full = m.root + "/" + p;
            FILE *f = std::fopen(full.c_str(), "rb");
            if (!f) continue;
            int64_t n = file_size_of(f);
            if (n < 0) { std::fclose(f); continue; }
            void *b = read_whole(f, (size_t)n, out_size);
            std::fclose(f);
            if (b) return b;
        }
    }
    set_err("'%s' is in none of the %u mounted sources", p.c_str(), (unsigned)g_mounts.size());
    return nullptr;
}

void dai_vfs_free(void *bytes) { std::free(bytes); }

int64_t dai_vfs_size(const char *path) {
    std::string p;
    if (!norm_path(path, p)) return -1;
    for (size_t i = 0; i < g_mounts.size(); ++i) {
        const Mount &m = g_mounts[i];
        if (m.archive) {
            const PackEntry *e = find_in(m, p);
            if (e) return (int64_t)e->length;
        } else {
            struct stat st;
            std::string full = m.root + "/" + p;
            if (stat(full.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG)
                return (int64_t)st.st_size;
        }
    }
    return -1;
}

int dai_vfs_exists(const char *path) { return dai_vfs_size(path) >= 0 ? 1 : 0; }

int dai_vfs_real_path(const char *path, char *out, size_t out_len) {
    if (out && out_len) out[0] = 0;
    std::string p;
    if (!norm_path(path, p)) return 0;
    for (size_t i = 0; i < g_mounts.size(); ++i) {
        const Mount &m = g_mounts[i];
        if (m.archive) {
            // An archive wins here too: if the shipped copy is what a read
            // would return, saying "it is at this path on disk" would be a lie.
            if (find_in(m, p)) return 0;
            continue;
        }
        struct stat st;
        std::string full = m.root + "/" + p;
        if (stat(full.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
            if (out && out_len) std::snprintf(out, out_len, "%s", full.c_str());
            return 1;
        }
    }
    return 0;
}

const char *dai_vfs_last_error(void) { return g_err; }

int dai_vfs_self_path(char *out, size_t out_len) {
    if (!out || !out_len) return 0;
    out[0] = 0;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(nullptr, out, (DWORD)out_len);
    if (n == 0 || n >= out_len) { out[0] = 0; return 0; }
    return 1;
#else
    ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
    if (n <= 0) { out[0] = 0; return 0; }
    out[n] = 0;
    return 1;
#endif
}

} // extern "C"

// ------------------------------------------------------------------- listing

namespace {
void list_dir_rec(const std::string &root, const std::string &rel, std::vector<std::string> &out);
}

extern "C" uint32_t dai_vfs_list(char *out, uint32_t max, uint32_t stride) {
    std::vector<std::string> all;
    for (size_t i = 0; i < g_mounts.size(); ++i) {
        const Mount &m = g_mounts[i];
        if (m.archive) {
            for (size_t k = 0; k < m.dir.size(); ++k) all.push_back(m.dir[k].path);
        } else {
            list_dir_rec(m.root, "", all);
        }
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    if (out && stride) {
        uint32_t n = (uint32_t)all.size() < max ? (uint32_t)all.size() : max;
        for (uint32_t i = 0; i < n; ++i) {
            char *dst = out + (size_t)i * stride;
            std::snprintf(dst, stride, "%s", all[i].c_str());
        }
    }
    return (uint32_t)all.size();
}

// ------------------------------------------------------------- the directory walk


namespace {

void list_dir_rec(const std::string &root, const std::string &rel, std::vector<std::string> &out) {
    std::string here = rel.empty() ? root : root + "/" + rel;
    DIR *d = opendir(here.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;          // . .. .git .DS_Store
        std::string child = rel.empty() ? std::string(e->d_name) : rel + "/" + e->d_name;
        std::string full = root + "/" + child;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if ((st.st_mode & S_IFMT) == S_IFDIR) list_dir_rec(root, child, out);
        else if ((st.st_mode & S_IFMT) == S_IFREG && child.size() < DAI_VFS_PATH_MAX)
            out.push_back(child);
    }
    closedir(d);
}

} // namespace

// --------------------------------------------------------------- boot config

extern "C" {

dai_boot_config dai_boot_config_default(void) {
    dai_boot_config c;
    std::memset(&c, 0, sizeof(c));
    std::snprintf(c.scene, sizeof(c.scene), "scenes/main.daidalos");
    std::snprintf(c.title, sizeof(c.title), "Daidalos");
    c.width = 1280; c.height = 720;
    c.fullscreen = 0;
    c.msaa = 4;
    c.tick_hz = 60;
    c.max_bodies = 4096;
    c.physics_backend = DAI_PHYSICS_TALOS;
    c.gravity[0] = 0.0f; c.gravity[1] = -9.81f; c.gravity[2] = 0.0f;
    return c;
}

size_t dai_boot_config_write(const dai_boot_config *c, char *buf, size_t buf_size) {
    if (!c) return 0;
    char t[1024];
    int n = std::snprintf(t, sizeof(t),
        "daidalos-boot 1\n"
        "# written by the exporter - one 'key value' per line\n"
        "scene %s\n"
        "title %s\n"
        "width %d\n"
        "height %d\n"
        "fullscreen %d\n"
        "msaa %d\n"
        "tick_hz %d\n"
        "max_bodies %d\n"
        "physics %d\n"
        "gravity %g %g %g\n"
        "language %s\n"
        "audio_bank %s\n",
        c->scene, c->title, c->width, c->height, c->fullscreen, c->msaa,
        c->tick_hz, c->max_bodies, c->physics_backend,
        (double)c->gravity[0], (double)c->gravity[1], (double)c->gravity[2],
        c->language, c->audio_bank);
    if (n < 0) return 0;
    if (buf && buf_size) std::snprintf(buf, buf_size, "%s", t);
    return (size_t)n;
}

dai_result dai_boot_config_parse(const char *text, size_t len, dai_boot_config *out) {
    if (!text || !out) return DAI_ERR_INVALID_ARG;
    *out = dai_boot_config_default();
    std::string src(text, len);
    size_t pos = 0;
    bool seen_header = false;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        std::string line = src.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? src.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        if (line[0] == '#') continue;
        size_t sp = line.find(' ');
        std::string key = line.substr(0, sp);
        std::string val = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        if (key == "daidalos-boot") { seen_header = true; continue; }
        // Unknown keys are skipped, not rejected: a newer exporter writing one
        // more line must not stop an older runtime from starting.
        if (key == "scene")           std::snprintf(out->scene, sizeof(out->scene), "%s", val.c_str());
        else if (key == "title")      std::snprintf(out->title, sizeof(out->title), "%s", val.c_str());
        else if (key == "width")      out->width = std::atoi(val.c_str());
        else if (key == "height")     out->height = std::atoi(val.c_str());
        else if (key == "fullscreen") out->fullscreen = std::atoi(val.c_str());
        else if (key == "msaa")       out->msaa = std::atoi(val.c_str());
        else if (key == "tick_hz")    out->tick_hz = std::atoi(val.c_str());
        else if (key == "max_bodies") out->max_bodies = std::atoi(val.c_str());
        else if (key == "language")   std::snprintf(out->language, sizeof(out->language), "%s", val.c_str());
        else if (key == "audio_bank") std::snprintf(out->audio_bank, sizeof(out->audio_bank), "%s", val.c_str());
        else if (key == "physics")    out->physics_backend = std::atoi(val.c_str());
        else if (key == "gravity") {
            float g[3] = { 0, -9.81f, 0 };
            if (std::sscanf(val.c_str(), "%f %f %f", &g[0], &g[1], &g[2]) == 3) {
                out->gravity[0] = g[0]; out->gravity[1] = g[1]; out->gravity[2] = g[2];
            }
        }
    }
    if (out->width  < 64)  out->width  = 64;
    if (out->height < 64)  out->height = 64;
    if (out->msaa   < 1)   out->msaa   = 1;
    if (out->tick_hz < 1)  out->tick_hz = 60;
    if (out->max_bodies < 16) out->max_bodies = 16;
    if (!seen_header) return DAI_ERR_FILE;   // not a boot config at all
    return DAI_OK;
}

uint32_t dai_pack_hash(const void *bytes, size_t len) {
    if (!bytes || !len) return 0;
    const uint8_t *p = (const uint8_t *)bytes;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

} // extern "C"

// -------------------------------------------------------------- the writer

struct dai_pack {
    FILE       *f = nullptr;
    std::string out_path;
    std::string tmp_path;
    uint64_t    base = 0;
    bool        executable = false;    // a runtime was copied in front
    bool        failed = false;
    char        err[512] = { 0 };
    std::vector<PackEntry> entries;
};

namespace {

bool pad_to_align(dai_pack *p) {
    int64_t at = DAI_FTELL(p->f);
    if (at < 0) return false;
    uint64_t want = align4_64((uint64_t)at);
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    size_t pad = (size_t)(want - (uint64_t)at);
    if (pad && std::fwrite(zero, 1, pad, p->f) != pad) return false;
    return true;
}

bool fail_pack(dai_pack *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    std::snprintf(g_err, sizeof(g_err), "%s", p->err);
    p->failed = true;
    return false;
}

bool add_common(dai_pack *p, const char *vpath, std::string &norm) {
    if (!p) return false;
    if (p->failed) return false;
    if (!norm_path(vpath, norm))
        return fail_pack(p, "'%s' is not a usable archive path", vpath ? vpath : "(null)");
    for (size_t i = 0; i < p->entries.size(); ++i)
        if (p->entries[i].path == norm)
            return fail_pack(p, "'%s' is already in the archive", norm.c_str());
    if (!pad_to_align(p)) return fail_pack(p, "cannot align '%s'", norm.c_str());
    return true;
}

} // namespace

extern "C" {

dai_pack *dai_pack_begin(const char *out_path, const char *prefix_file,
                         char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!out_path || !*out_path) { out_err(err, err_len, "no output path"); return nullptr; }

    dai_pack *p = new dai_pack();
    p->out_path = out_path;
    p->tmp_path = std::string(out_path) + ".packtmp";
    p->f = std::fopen(p->tmp_path.c_str(), "wb+");
    if (!p->f) {
        out_err(err, err_len, "cannot write '%s'", p->tmp_path.c_str());
        delete p;
        return nullptr;
    }

    if (prefix_file && *prefix_file) {
        FILE *in = std::fopen(prefix_file, "rb");
        if (!in) {
            out_err(err, err_len, "cannot read the runtime template '%s'", prefix_file);
            std::fclose(p->f); std::remove(p->tmp_path.c_str()); delete p;
            return nullptr;
        }
        char buf[64 * 1024];
        size_t got;
        bool ok = true;
        while ((got = std::fread(buf, 1, sizeof(buf), in)) > 0)
            if (std::fwrite(buf, 1, got, p->f) != got) { ok = false; break; }
        std::fclose(in);
        if (!ok) {
            out_err(err, err_len, "cannot copy '%s' (disk full?)", prefix_file);
            std::fclose(p->f); std::remove(p->tmp_path.c_str()); delete p;
            return nullptr;
        }
        p->executable = true;
    }

    if (!pad_to_align(p)) {
        out_err(err, err_len, "cannot align the archive start");
        std::fclose(p->f); std::remove(p->tmp_path.c_str()); delete p;
        return nullptr;
    }
    p->base = (uint64_t)DAI_FTELL(p->f);

    uint8_t head[DAI_PACK_HEADER_BYTES];
    std::memset(head, 0, sizeof(head));
    if (std::fwrite(head, 1, sizeof(head), p->f) != sizeof(head)) {
        out_err(err, err_len, "cannot write the header");
        std::fclose(p->f); std::remove(p->tmp_path.c_str()); delete p;
        return nullptr;
    }
    return p;
}

int dai_pack_add_mem(dai_pack *p, const char *vpath, const void *bytes, size_t len) {
    std::string norm;
    if (!add_common(p, vpath, norm)) return 0;
    if (len > 0xFFFFFFFFull) { fail_pack(p, "'%s' is larger than 4 GB", norm.c_str()); return 0; }
    PackEntry e;
    e.path = norm;
    e.offset = (uint64_t)DAI_FTELL(p->f) - p->base;   // relative while writing
    e.length = (uint32_t)len;
    e.hash = dai_pack_hash(bytes, len);
    if (len && std::fwrite(bytes, 1, len, p->f) != len) {
        fail_pack(p, "cannot write '%s' (disk full?)", norm.c_str());
        return 0;
    }
    p->entries.push_back(e);
    return 1;
}

int dai_pack_add_file(dai_pack *p, const char *vpath, const char *disk_path) {
    std::string norm;
    if (!add_common(p, vpath, norm)) return 0;
    FILE *in = std::fopen(disk_path ? disk_path : "", "rb");
    if (!in) { fail_pack(p, "cannot read '%s'", disk_path ? disk_path : "(null)"); return 0; }

    PackEntry e;
    e.path = norm;
    e.offset = (uint64_t)DAI_FTELL(p->f) - p->base;
    e.length = 0;
    uint32_t h = 2166136261u;
    char buf[64 * 1024];
    size_t got;
    uint64_t total = 0;
    while ((got = std::fread(buf, 1, sizeof(buf), in)) > 0) {
        if (std::fwrite(buf, 1, got, p->f) != got) {
            std::fclose(in);
            fail_pack(p, "cannot write '%s' (disk full?)", norm.c_str());
            return 0;
        }
        for (size_t i = 0; i < got; ++i) { h ^= (uint8_t)buf[i]; h *= 16777619u; }
        total += got;
    }
    std::fclose(in);
    if (total > 0xFFFFFFFFull) { fail_pack(p, "'%s' is larger than 4 GB", norm.c_str()); return 0; }
    e.length = (uint32_t)total;
    e.hash = total ? h : 0u;
    p->entries.push_back(e);
    return 1;
}

uint32_t dai_pack_count(const dai_pack *p) { return p ? (uint32_t)p->entries.size() : 0; }

void dai_pack_abort(dai_pack *p) {
    if (!p) return;
    if (p->f) std::fclose(p->f);
    std::remove(p->tmp_path.c_str());
    delete p;
}

dai_result dai_pack_end(dai_pack *p, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!p) return DAI_ERR_INVALID_ARG;
    if (p->failed) {
        out_err(err, err_len, "%s", p->err);
        dai_pack_abort(p);
        return DAI_ERR_FILE;
    }

    // The directory, sorted so the reader can binary search it.
    std::sort(p->entries.begin(), p->entries.end(),
              [](const PackEntry &a, const PackEntry &b) { return a.path < b.path; });

    if (!pad_to_align(p)) { out_err(err, err_len, "cannot align the directory"); dai_pack_abort(p); return DAI_ERR_FILE; }
    uint64_t dir_at = (uint64_t)DAI_FTELL(p->f);
    uint32_t dir_off = (uint32_t)(dir_at - p->base);

    std::vector<uint8_t> dir;
    for (size_t i = 0; i < p->entries.size(); ++i) {
        const PackEntry &e = p->entries[i];
        uint32_t plen = (uint32_t)e.path.size();
        size_t at = dir.size();
        dir.resize(at + 16 + align4(plen), 0);
        wr32(&dir[at + 0], plen);
        wr32(&dir[at + 4], (uint32_t)e.offset);
        wr32(&dir[at + 8], e.length);
        wr32(&dir[at + 12], e.hash);
        std::memcpy(&dir[at + 16], e.path.data(), plen);
    }
    if (!dir.empty() && std::fwrite(dir.data(), 1, dir.size(), p->f) != dir.size()) {
        out_err(err, err_len, "cannot write the directory (disk full?)");
        dai_pack_abort(p);
        return DAI_ERR_FILE;
    }

    uint64_t after_dir = (uint64_t)DAI_FTELL(p->f);
    uint64_t archive_bytes = (after_dir - p->base) + DAI_PACK_TRAILER_BYTES;
    if (archive_bytes > 0xFFFFFFFFull) {
        out_err(err, err_len, "the archive is larger than 4 GB");
        dai_pack_abort(p);
        return DAI_ERR_FILE;
    }

    uint8_t trailer[DAI_PACK_TRAILER_BYTES];
    wr64(trailer, p->base);
    std::memcpy(trailer + 8, DAI_PACK_TRAILER_MAGIC, 8);
    if (std::fwrite(trailer, 1, sizeof(trailer), p->f) != sizeof(trailer)) {
        out_err(err, err_len, "cannot write the trailer (disk full?)");
        dai_pack_abort(p);
        return DAI_ERR_FILE;
    }

    uint32_t data_off = p->entries.empty() ? dir_off
                                           : (uint32_t)p->entries.front().offset;
    // The first PAYLOAD, not the first directory entry: after the sort the
    // lowest offset is not necessarily entries[0].
    for (size_t i = 0; i < p->entries.size(); ++i)
        if ((uint32_t)p->entries[i].offset < data_off) data_off = (uint32_t)p->entries[i].offset;

    uint8_t head[DAI_PACK_HEADER_BYTES];
    std::memset(head, 0, sizeof(head));
    std::memcpy(head, DAI_PACK_MAGIC, 8);
    wr32(head + 8,  DAI_PACK_VERSION);
    wr32(head + 12, (uint32_t)p->entries.size());
    wr32(head + 16, dir_off);
    wr32(head + 20, (uint32_t)dir.size());
    wr32(head + 24, data_off);
    wr32(head + 28, (uint32_t)archive_bytes);
    if (DAI_FSEEK(p->f, (int64_t)p->base, SEEK_SET) != 0 ||
        std::fwrite(head, 1, sizeof(head), p->f) != sizeof(head)) {
        out_err(err, err_len, "cannot rewrite the header");
        dai_pack_abort(p);
        return DAI_ERR_FILE;
    }

    if (std::fflush(p->f) != 0) {
        out_err(err, err_len, "cannot flush '%s' (disk full?)", p->tmp_path.c_str());
        dai_pack_abort(p);
        return DAI_ERR_FILE;
    }
    std::fclose(p->f);
    p->f = nullptr;

    // Rename into place. On Windows rename() refuses an existing target, so
    // the old one goes first - the temp file is already complete, which is the
    // whole reason for writing through one.
    std::remove(p->out_path.c_str());
    if (std::rename(p->tmp_path.c_str(), p->out_path.c_str()) != 0) {
        out_err(err, err_len, "cannot move '%s' into place", p->out_path.c_str());
        std::remove(p->tmp_path.c_str());
        delete p;
        return DAI_ERR_FILE;
    }
#ifndef _WIN32
    if (p->executable) chmod(p->out_path.c_str(), 0755);
#endif
    delete p;
    return DAI_OK;
}

// ------------------------------------------------------------- inspecting

uint32_t dai_pack_read(const char *file, dai_pack_info *info, dai_pack_entry *out,
                       uint32_t max, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!file || !*file) { out_err(err, err_len, "no file given"); return 0; }
    std::vector<PackEntry> dir;
    dai_pack_info local;
    std::memset(&local, 0, sizeof(local));
    if (!read_directory(file, dir, &local, err, err_len)) return 0;
    if (info) *info = local;
    if (out && max) {
        uint32_t n = (uint32_t)dir.size() < max ? (uint32_t)dir.size() : max;
        for (uint32_t i = 0; i < n; ++i) {
            std::memset(&out[i], 0, sizeof(out[i]));
            std::snprintf(out[i].path, sizeof(out[i].path), "%s", dir[i].path.c_str());
            out[i].offset = dir[i].offset;
            out[i].length = dir[i].length;
            out[i].hash = dir[i].hash;
        }
    }
    return (uint32_t)dir.size();
}

uint32_t dai_pack_verify(const char *file, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    std::vector<PackEntry> dir;
    if (!read_directory(file ? file : "", dir, nullptr, err, err_len))
        return 0xFFFFFFFFu;   // could not even read it: not "0 bad entries"
    FILE *f = std::fopen(file, "rb");
    if (!f) { out_err(err, err_len, "cannot open '%s'", file); return 0xFFFFFFFFu; }
    uint32_t bad = 0;
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < dir.size(); ++i) {
        buf.resize(dir[i].length ? dir[i].length : 1);
        if (DAI_FSEEK(f, (int64_t)dir[i].offset, SEEK_SET) != 0 ||
            (dir[i].length && std::fread(buf.data(), 1, dir[i].length, f) != dir[i].length)) {
            ++bad;
            continue;
        }
        if (dai_pack_hash(buf.data(), dir[i].length) != dir[i].hash) {
            out_err(err, err_len, "'%s': hash mismatch", dir[i].path.c_str());
            ++bad;
        }
    }
    std::fclose(f);
    return bad;
}

} // extern "C"
