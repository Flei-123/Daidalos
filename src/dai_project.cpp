// Projects, project settings and per user preferences. See include/dai_project.h.
//
// This is all files and directories. No renderer, no interface, no world: the
// project picker is the first thing on screen and none of those exist yet when
// it runs. build.sh links the test against this object file ALONE, which keeps
// the claim honest instead of aspirational.
//
// Two text formats, both line based "key value", both written the way
// src/dai_doc_text.cpp writes a scene and for the same reasons: the settings
// file lives in version control, so it has to diff per property rather than per
// brace, and a float has to read back bit identical or a round trip through the
// editor would quietly change the physics of the game.
//
// One deliberate difference from the scene format: an unknown key here is
// SKIPPED, not an error. A scene is authored data, so swallowing a typo there
// is data loss and strictness protects the user. Settings are passed between
// people on different builds, where refusing to open the project because a
// colleague's newer editor wrote one more line would make the strictness the
// bug. The lines we did not understand are kept and written back out, so the
// older editor does not erode the file just by opening it.
//
// Comments in the file do NOT survive a save. Preserving them would mean either
// re-emitting our own explanatory header every time (it grows) or tracking
// which comment belonged to which key (it does not, once a key is deleted).

#include "dai_project.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
// The A suffixed calls are explicit on purpose: build_win.sh compiles with
// -DUNICODE, which would otherwise redirect CreateDirectory and friends to
// their wide versions and stop them taking the char* paths this API is made of.
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Format stamps
// ---------------------------------------------------------------------------

const char MARKER_MAGIC[]   = "daidalos-project";
const char SETTINGS_MAGIC[] = "daidalos-project-settings";
const char PREFS_MAGIC[]    = "daidalos-prefs";
const int  FORMAT_VERSION   = 1;

// Written into project.daidalos so a project can say what made it. A literal
// rather than dai_version(): this translation unit links on its own, and the
// project picker runs before there is an engine to ask. It is a stamp for
// humans reading a diff - nothing branches on it, so it cannot rot into a bug.
const char ENGINE_STAMP[] = "0.2.0";

// The one line a scene file needs to be a scene. An empty file is refused by
// dai_doc_from_text as "empty or truncated", and a new project whose scene
// will not open is a poor first impression. The magic is duplicated instead of
// included for the same reason as ENGINE_STAMP, and it is safe to pin at 1: the
// scene loader accepts anything at or below its own version.
const char EMPTY_SCENE[] = "daidalos-scene 1\n";

const char *DEFAULT_TAGS[]   = { "Untagged", "Player", "MainCamera", "EditorOnly" };
const char *DEFAULT_LAYERS[] = { "Default", "IgnoreRaycast", "Water", "UI" };

// ---------------------------------------------------------------------------
// Paths and the filesystem. Two implementations, no POSIX in the Windows half.
// ---------------------------------------------------------------------------

bool is_dir(const std::string &p) {
    if (p.empty()) return false;
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool file_exists(const std::string &p) {
    if (p.empty()) return false;
#ifdef _WIN32
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0;
#endif
}

// True when the directory is there afterwards, whoever made it. "Already
// exists" is success: two editors opening the same project must not race each
// other into a spurious error.
bool make_dir(const std::string &p) {
    if (is_dir(p)) return true;
#ifdef _WIN32
    return CreateDirectoryA(p.c_str(), nullptr) != 0 || is_dir(p);
#else
    return mkdir(p.c_str(), 0777) == 0 || is_dir(p);
#endif
}

void rm_dir(const std::string &p) {
    // rmdir only removes an EMPTY directory. That is the safety property the
    // rollback in dai_project_create relies on: it cannot delete anything that
    // was already in there.
#ifdef _WIN32
    RemoveDirectoryA(p.c_str());
#else
    rmdir(p.c_str());
#endif
}

// mkdir -p. Walks the components so a root_dir the user typed but never made
// still works, and so ~/.config/daidalos comes into being on a fresh account.
bool make_dirs(const std::string &path) {
    if (path.empty()) return false;
    std::string acc;
    size_t i = 0;
    if (path[0] == '/' || path[0] == '\\') {
        acc = "/";                       // absolute: keep the root
        i = 1;
    }
#ifdef _WIN32
    else if (path.size() >= 2 && path[1] == ':') {
        acc = path.substr(0, 2);         // "C:" is not a directory anyone creates
        i = 2;
        if (i < path.size() && (path[i] == '/' || path[i] == '\\')) { acc += '/'; ++i; }
    }
#endif
    while (i <= path.size()) {
        size_t j = i;
        while (j < path.size() && path[j] != '/' && path[j] != '\\') ++j;
        std::string part = path.substr(i, j - i);
        if (!part.empty()) {
            if (!acc.empty() && acc[acc.size() - 1] != '/') acc += '/';
            acc += part;
            if (!make_dir(acc)) return false;
        }
        if (j >= path.size()) break;
        i = j + 1;
    }
    return is_dir(acc);
}

std::string join(const std::string &base, const std::string &leaf) {
    if (base.empty()) return leaf;
    if (leaf.empty()) return base;
    char last = base[base.size() - 1];
    if (last == '/' || last == '\\') return base + leaf;
    return base + "/" + leaf;
}

// A trailing separator makes "<path>/" + "/assets" and confuses stat on some
// systems, so every path that enters this module is normalised once.
std::string trim_sep(const char *p) {
    std::string s = p ? p : "";
    while (s.size() > 1) {
        char c = s[s.size() - 1];
        if (c != '/' && c != '\\') break;
#ifdef _WIN32
        if (s.size() == 3 && s[1] == ':') break;   // keep "C:/"
#endif
        s.resize(s.size() - 1);
    }
    return s;
}

std::string base_name(const std::string &p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool is_absolute(const std::string &p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() > 1 && p[1] == ':';
}

bool read_file(const std::string &path, std::string &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) out.append(chunk, n);
    std::fclose(f);
    return true;
}

// Write to a temporary file, then move it over the target. A crash halfway
// through must not leave a truncated settings file where a working one was -
// same reasoning as dai_doc_save, and the same reason the editor survives being
// killed while saving.
bool write_file_atomic(const std::string &path, const std::string &text) {
    std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t wrote = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), f);
    int flushed = std::fflush(f);
    std::fclose(f);
    if (wrote != text.size() || flushed != 0) { std::remove(tmp.c_str()); return false; }
#ifdef _WIN32
    // POSIX rename() replaces the target. The Windows CRT's does NOT - it fails
    // when the destination exists, so saving a second time would silently stop
    // working on the one platform the editor actually ships to.
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::remove(tmp.c_str());
        return false;
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
#endif
    return true;
}

// Subdirectories of `dir`, one level, unsorted.
std::vector<std::string> list_subdirs(const std::string &dir) {
    std::vector<std::string> out;
    if (dir.empty()) return out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(join(dir, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent *e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        // d_type is unreliable on some filesystems (it can be DT_UNKNOWN), so
        // the answer comes from stat, not from the directory entry.
        if (!is_dir(join(dir, e->d_name))) continue;
        out.push_back(e->d_name);
    }
    closedir(d);
#endif
    return out;
}

const char *env_or_null(const char *name) {
    const char *v = std::getenv(name);
    return (v && v[0]) ? v : nullptr;
}

std::string iso_utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm *g = std::gmtime(&t);
    char buf[32] = "1970-01-01T00:00:00Z";
    if (g) std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", g);
    return buf;
}

// ---------------------------------------------------------------------------
// Text: writing
// ---------------------------------------------------------------------------

void put(std::string &s, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

// Shortest text that still reads back bit identical, lifted from
// dai_doc_text.cpp for the same reason it exists there: %.9g is always exact but
// prints a gravity of -9.81 as -9.81000042, which makes a hand edited settings
// file look broken and a diff unreadable.
std::string fstr(float v) {
    char buf[40];
    for (int prec = 6; prec < 9; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, (double)v);
        if ((float)std::strtod(buf, nullptr) == v) return buf;
    }
    std::snprintf(buf, sizeof(buf), "%.9g", (double)v);
    return buf;
}

// A value is the rest of its line, so anything that could end a line early or
// grow whitespace has to go before it is written. Doing it here rather than at
// the API boundary keeps the guarantee simple: what is saved is what loads back.
std::string sanitise(const char *raw) {
    std::string s = raw ? raw : "";
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) s[i] = ' ';
    }
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
// Text: reading
// ---------------------------------------------------------------------------

const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

const char *token(const char *p, std::string &out) {
    p = skip_ws(p);
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    out.assign(start, (size_t)(p - start));
    return p;
}

// Trailing whitespace trimmed, so a name may contain spaces but cannot smuggle
// them in at the edges - which is what makes the round trip exact.
std::string rest_of_line(const char *p) {
    p = skip_ws(p);
    std::string s(p);
    while (!s.empty()) {
        char c = s[s.size() - 1];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t') break;
        s.resize(s.size() - 1);
    }
    return s;
}

bool parse_floats(const char *p, float *out, int count) {
    for (int i = 0; i < count; ++i) {
        p = skip_ws(p);
        if (!*p || *p == '\r' || *p == '\n') return false;
        char *endp = nullptr;
        double v = std::strtod(p, &endp);
        if (endp == p) return false;
        // NaN or infinity in a settings file is corruption, and a NaN gravity
        // would take the whole simulation with it. Keep the default instead.
        if (v != v || v > 3.5e38 || v < -3.5e38) return false;
        out[i] = (float)v;
        p = endp;
    }
    return true;
}

bool parse_int(const char *p, int *out) {
    p = skip_ws(p);
    char *endp = nullptr;
    long v = std::strtol(p, &endp, 10);
    if (endp == p) return false;
    *out = (int)v;
    return true;
}

// Splits text into lines without copying the whole thing twice.
std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            if (pos < text.size()) out.push_back(text.substr(pos));
            break;
        }
        out.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

// Zero fills the tail instead of only terminating. Two settings structs that
// mean the same thing then hold the same bytes, which is what lets the editor
// answer "is there anything unsaved?" with one memcmp rather than a field by
// field comparison that will forget a field the day somebody adds one.
void copy_str(char *dst, size_t cap, const std::string &src) {
    if (!dst || !cap) return;
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memset(dst, 0, cap);
    std::memcpy(dst, src.data(), n);
}

void set_err(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

} // namespace

// ---------------------------------------------------------------------------

struct dai_project {
    std::string path;            // no trailing separator
    std::string name;
    std::string scene;           // resolved startup scene
    std::string assets;
    std::string cache;
    std::string settings_dir;
    std::string settings_file;
    // Settings lines this build did not recognise. Kept so saving from an older
    // editor hands a newer one its own keys back untouched.
    std::vector<std::string> unknown;
};

namespace {

// The three directories that make a folder a project, in one place so
// is_valid, create and open cannot drift apart.
const char *REQUIRED_DIRS[] = { "assets", "scenes", "settings" };

void fill_paths(dai_project *p) {
    p->assets        = join(p->path, "assets");
    p->cache         = join(p->path, "cache");
    p->settings_dir  = join(p->path, "settings");
    p->settings_file = join(p->settings_dir, "project.txt");
}

// default_scene is stored relative to the project so the file survives being
// cloned to another machine; the editor wants something it can fopen.
void resolve_scene(dai_project *p, const dai_project_settings &s) {
    std::string rel = s.default_scene[0] ? s.default_scene : "scenes/main.daidalos";
    p->scene = is_absolute(rel) ? rel : join(p->path, rel);
}

} // namespace

extern "C" {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

int dai_project_name_valid(const char *name) {
    if (!name || !name[0]) return 0;
    size_t len = std::strlen(name);
    if (len >= DAI_PROJECT_NAME_MAX) return 0;
    if (name[0] == ' ' || name[len - 1] == ' ') return 0;   // Windows drops both

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
        // Everything else is refused rather than escaped, and that includes
        // '.', which is what makes ".." and "../../etc/passwd" impossible here
        // instead of impossible-if-the-filter-is-right. A project name is shown
        // to humans and joined into a path; it does not need to be expressive.
        if (!ok) return 0;
    }

    // Windows reserves these whatever the extension, so "CON" would create a
    // project that cannot be opened, deleted or explained.
    static const char *reserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        const char *r = reserved[i];
        size_t rl = std::strlen(r);
        if (len != rl) continue;
        size_t k = 0;
        for (; k < rl; ++k) {
            char a = name[k];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (a != r[k]) break;
        }
        if (k == rl) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Creating, opening, validating
// ---------------------------------------------------------------------------

int dai_project_is_valid(const char *path) {
    if (!path || !path[0]) return 0;
    std::string root = trim_sep(path);
    if (!is_dir(root)) return 0;
    for (size_t i = 0; i < sizeof(REQUIRED_DIRS) / sizeof(REQUIRED_DIRS[0]); ++i)
        if (!is_dir(join(root, REQUIRED_DIRS[i]))) return 0;
    return 1;
}

dai_project *dai_project_create(const char *root_dir, const char *name,
                                char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!root_dir || !root_dir[0]) {
        set_err(err, err_len, "no directory to create the project in");
        return nullptr;
    }
    if (!dai_project_name_valid(name)) {
        set_err(err, err_len,
                "'%s' is not a usable project name - letters, digits, spaces, - and _ only",
                name ? name : "");
        return nullptr;
    }

    std::string root = trim_sep(root_dir);
    std::string path = join(root, name);
    std::string marker = join(path, DAI_PROJECT_MARKER);

    if (file_exists(marker)) {
        set_err(err, err_len, "there is already a project at '%s'", path.c_str());
        return nullptr;
    }

    if (!make_dirs(root)) {
        set_err(err, err_len, "cannot create '%s'", root.c_str());
        return nullptr;
    }

    // Track what we made so a failure halfway can undo exactly that much. rmdir
    // refuses to touch a non empty directory, so the worst case of this rollback
    // is that it does nothing.
    std::vector<std::string> made;
    bool root_existed = is_dir(path);
    if (!root_existed) {
        if (!make_dir(path)) {
            set_err(err, err_len, "cannot create '%s'", path.c_str());
            return nullptr;
        }
        made.push_back(path);
    }

    struct Rollback {
        std::vector<std::string> *dirs;
        std::vector<std::string> *files;
        bool armed = true;
        ~Rollback() {
            if (!armed) return;
            for (size_t i = files->size(); i-- > 0;) std::remove((*files)[i].c_str());
            for (size_t i = dirs->size(); i-- > 0;) rm_dir((*dirs)[i]);
        }
    };
    std::vector<std::string> files;
    Rollback rb{ &made, &files, true };

    const char *subs[] = { "assets", "scenes", "settings", "cache" };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); ++i) {
        std::string sub = join(path, subs[i]);
        bool existed = is_dir(sub);
        if (!make_dir(sub)) {
            set_err(err, err_len, "cannot create '%s'", sub.c_str());
            return nullptr;
        }
        if (!existed) made.push_back(sub);
    }

    std::string scene_file = join(join(path, "scenes"), "main.daidalos");
    if (!file_exists(scene_file)) {
        if (!write_file_atomic(scene_file, EMPTY_SCENE)) {
            set_err(err, err_len, "cannot write '%s'", scene_file.c_str());
            return nullptr;
        }
        files.push_back(scene_file);
    }

    // cache/ is derived data. Committing it means merge conflicts in files
    // nobody wrote, so the ignore rule ships with the project rather than being
    // something every new team member has to be told.
    std::string ignore = join(path, ".gitignore");
    if (!file_exists(ignore)) {
        if (write_file_atomic(ignore, "# derived data, safe to delete at any time\ncache/\n"))
            files.push_back(ignore);
    }

    dai_project *p = new dai_project();
    p->path = path;
    p->name = name;
    fill_paths(p);

    // The product name starts out as the project name, the way Unity's does:
    // a sensible answer beats an empty field the user has to discover.
    dai_project_settings s = dai_project_settings_default();
    copy_str(s.app_name, sizeof(s.app_name), name);
    if (dai_project_settings_save(p, &s) != DAI_OK) {
        set_err(err, err_len, "cannot write '%s'", p->settings_file.c_str());
        delete p;
        return nullptr;
    }
    files.push_back(p->settings_file);

    // The marker goes last, so an interrupted creation never leaves something
    // that claims to be a finished project.
    std::string text;
    put(text, "%s %d\n", MARKER_MAGIC, FORMAT_VERSION);
    put(text, "name %s\n", sanitise(name).c_str());
    put(text, "engine %s\n", ENGINE_STAMP);
    put(text, "created %s\n", iso_utc_now().c_str());
    if (!write_file_atomic(marker, text)) {
        set_err(err, err_len, "cannot write '%s'", marker.c_str());
        delete p;
        return nullptr;
    }

    rb.armed = false;
    return p;
}

dai_project *dai_project_open(const char *path, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!path || !path[0]) {
        set_err(err, err_len, "no project path given");
        return nullptr;
    }
    std::string root = trim_sep(path);
    if (!is_dir(root)) {
        set_err(err, err_len, "'%s' is not a directory", root.c_str());
        return nullptr;
    }
    if (!dai_project_is_valid(root.c_str())) {
        set_err(err, err_len,
                "'%s' is not a daidalos project - it needs assets/, scenes/ and settings/",
                root.c_str());
        return nullptr;
    }

    dai_project *p = new dai_project();
    p->path = root;
    p->name = base_name(root);
    fill_paths(p);

    // cache/ is allowed to be missing - that is what makes it a cache. Putting
    // it back here is what turns "delete cache/ and reopen" into a supported
    // repair instead of a project that no longer builds its derived data.
    make_dir(p->cache);

    std::string text;
    if (read_file(join(root, DAI_PROJECT_MARKER), text)) {
        std::vector<std::string> lines = split_lines(text);
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string key;
            const char *after = token(lines[i].c_str(), key);
            if (key == "name") {
                std::string v = rest_of_line(after);
                if (!v.empty()) p->name = v;
                break;
            }
        }
    }
    // A missing or nameless marker is not fatal: the directory name is a fine
    // display name, and a project that will not open because a merge ate one
    // line would be a bad trade.

    dai_project_settings s{};
    dai_project_settings_load(p, &s);   // also collects the unknown keys
    resolve_scene(p, s);
    return p;
}

void dai_project_close(dai_project *p) { delete p; }

const char *dai_project_path(const dai_project *p)       { return p ? p->path.c_str() : nullptr; }
const char *dai_project_name(const dai_project *p)       { return p ? p->name.c_str() : nullptr; }
const char *dai_project_scene_path(const dai_project *p) { return p ? p->scene.c_str() : nullptr; }
const char *dai_project_asset_dir(const dai_project *p)  { return p ? p->assets.c_str() : nullptr; }
const char *dai_project_cache_dir(const dai_project *p)  { return p ? p->cache.c_str() : nullptr; }

uint32_t dai_project_list(const char *root_dir, char *out, uint32_t max, uint32_t stride) {
    if (!root_dir || !root_dir[0]) return 0;
    std::string root = trim_sep(root_dir);

    std::vector<std::string> names = list_subdirs(root);
    std::vector<std::string> found;
    for (size_t i = 0; i < names.size(); ++i) {
        std::string full = join(root, names[i]);
        if (dai_project_is_valid(full.c_str())) found.push_back(full);
    }
    // Directory order is whatever the filesystem feels like. A picker that
    // reorders itself between runs looks broken, so sort.
    std::sort(found.begin(), found.end());

    if (!out || !max || !stride) return (uint32_t)found.size();

    uint32_t n = (uint32_t)found.size() < max ? (uint32_t)found.size() : max;
    for (uint32_t i = 0; i < n; ++i) copy_str(out + (size_t)i * stride, stride, found[i]);
    return n;
}

// ---------------------------------------------------------------------------
// Project settings
// ---------------------------------------------------------------------------

dai_project_settings dai_project_settings_default(void) {
    dai_project_settings s{};
    // These mirror dai_config and dai_body_desc: a project that never touches
    // this file must behave exactly like a world created without one.
    s.gravity[0] = 0.0f;
    s.gravity[1] = -9.81f;
    s.gravity[2] = 0.0f;
    s.tick_hz             = 60;
    s.max_bodies          = 8192;
    s.physics_backend     = DAI_PHYSICS_TALOS;
    s.default_friction    = 0.6f;
    s.default_restitution = 0.0f;
    copy_str(s.app_name, sizeof(s.app_name), "Untitled");
    copy_str(s.default_scene, sizeof(s.default_scene), "scenes/main.daidalos");
    // Slot 0 of each table is load bearing and matches Unity's: a node with no
    // tag reads back as "Untagged", and everything starts on layer "Default".
    for (size_t i = 0; i < sizeof(DEFAULT_TAGS) / sizeof(DEFAULT_TAGS[0]); ++i)
        copy_str(s.tags[i], DAI_PROJECT_TAG_MAX, DEFAULT_TAGS[i]);
    for (size_t i = 0; i < sizeof(DEFAULT_LAYERS) / sizeof(DEFAULT_LAYERS[0]); ++i)
        copy_str(s.layers[i], DAI_PROJECT_TAG_MAX, DEFAULT_LAYERS[i]);
    return s;
}

dai_result dai_project_settings_load(dai_project *p, dai_project_settings *out) {
    if (!p || !out) return DAI_ERR_INVALID_ARG;
    *out = dai_project_settings_default();
    p->unknown.clear();   // or every load would stack another copy onto the file

    std::string text;
    if (!read_file(p->settings_file, text)) {
        // Nothing saved yet. That is the state of every freshly created project
        // whose settings nobody has touched, so it is DAI_OK with the defaults.
        return DAI_OK;
    }

    std::vector<std::string> lines = split_lines(text);
    bool seen_header = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const char *p0 = skip_ws(lines[i].c_str());
        if (!*p0 || *p0 == '\r' || *p0 == '#') continue;

        std::string key;
        const char *after = token(p0, key);

        if (!seen_header) {
            if (key != SETTINGS_MAGIC) {
                // Pointed at the wrong file entirely. The defaults are already
                // in *out, so the caller can carry on with a sane world.
                return DAI_ERR_FILE;
            }
            // A newer format version is read, not refused: every key is
            // independent here, so understanding four of five is still useful.
            seen_header = true;
            continue;
        }

        bool ok = true;
        if      (key == "gravity")     ok = parse_floats(after, out->gravity, 3);
        else if (key == "tick-hz")     ok = parse_int(after, &out->tick_hz);
        else if (key == "max-bodies")  ok = parse_int(after, &out->max_bodies);
        else if (key == "physics-backend") ok = parse_int(after, &out->physics_backend);
        else if (key == "friction")    ok = parse_floats(after, &out->default_friction, 1);
        else if (key == "restitution") ok = parse_floats(after, &out->default_restitution, 1);
        else if (key == "app-name")    copy_str(out->app_name, sizeof(out->app_name), rest_of_line(after));
        else if (key == "default-scene") copy_str(out->default_scene, sizeof(out->default_scene), rest_of_line(after));
        else if (key == "tag" || key == "layer") {
            int idx = 0;
            const char *rest = skip_ws(after);
            char *endp = nullptr;
            long v = std::strtol(rest, &endp, 10);
            if (endp == rest) { ok = false; }
            else {
                idx = (int)v;
                if (idx < 0 || idx >= DAI_PROJECT_TAGS) {
                    // A slot this build has no room for. Keeping the line means
                    // an editor with a bigger table still finds its tag.
                    p->unknown.push_back(lines[i]);
                    continue;
                }
                char *dst = (key == "tag") ? out->tags[idx] : out->layers[idx];
                copy_str(dst, DAI_PROJECT_TAG_MAX, rest_of_line(endp));
            }
        }
        else {
            p->unknown.push_back(lines[i]);
            continue;
        }
        // A key we know with a value we cannot read keeps its default rather
        // than failing the load. Half a settings file still beats no project.
        (void)ok;
    }

    if (!seen_header) return DAI_ERR_FILE;
    return DAI_OK;
}

dai_result dai_project_settings_save(dai_project *p, const dai_project_settings *s) {
    if (!p || !s) return DAI_ERR_INVALID_ARG;
    if (!make_dirs(p->settings_dir)) return DAI_ERR_FILE;

    const dai_project_settings d = dai_project_settings_default();
    std::string t;
    put(t, "%s %d\n", SETTINGS_MAGIC, FORMAT_VERSION);
    put(t, "# project settings - shared, belongs in version control\n");

    // Only what differs from the default is written. That is what makes the
    // file diff per property, keeps a three way merge of two people changing
    // different settings resolvable, and lets a later default actually reach
    // projects that never overrode it.
    if (s->gravity[0] != d.gravity[0] || s->gravity[1] != d.gravity[1] || s->gravity[2] != d.gravity[2])
        put(t, "gravity %s %s %s\n", fstr(s->gravity[0]).c_str(), fstr(s->gravity[1]).c_str(),
            fstr(s->gravity[2]).c_str());
    if (s->tick_hz != d.tick_hz)                   put(t, "tick-hz %d\n", s->tick_hz);
    if (s->max_bodies != d.max_bodies)             put(t, "max-bodies %d\n", s->max_bodies);
    if (s->physics_backend != d.physics_backend)   put(t, "physics-backend %d\n", s->physics_backend);
    if (s->default_friction != d.default_friction)
        put(t, "friction %s\n", fstr(s->default_friction).c_str());
    if (s->default_restitution != d.default_restitution)
        put(t, "restitution %s\n", fstr(s->default_restitution).c_str());
    if (std::strncmp(s->app_name, d.app_name, sizeof(d.app_name)) != 0)
        put(t, "app-name %s\n", sanitise(s->app_name).c_str());
    if (std::strncmp(s->default_scene, d.default_scene, sizeof(d.default_scene)) != 0)
        put(t, "default-scene %s\n", sanitise(s->default_scene).c_str());

    for (int i = 0; i < DAI_PROJECT_TAGS; ++i)
        if (std::strncmp(s->tags[i], d.tags[i], DAI_PROJECT_TAG_MAX) != 0)
            put(t, "tag %d %s\n", i, sanitise(s->tags[i]).c_str());
    for (int i = 0; i < DAI_PROJECT_TAGS; ++i)
        if (std::strncmp(s->layers[i], d.layers[i], DAI_PROJECT_TAG_MAX) != 0)
            put(t, "layer %d %s\n", i, sanitise(s->layers[i]).c_str());

    // Everything a newer build wrote and this one did not understand, handed
    // back exactly as it arrived. Without this, opening a project in an older
    // editor and pressing save would quietly delete a colleague's settings.
    if (!p->unknown.empty()) {
        put(t, "# written by a newer build, kept as found\n");
        for (size_t i = 0; i < p->unknown.size(); ++i) { t += p->unknown[i]; t += "\n"; }
    }

    if (!write_file_atomic(p->settings_file, t)) return DAI_ERR_FILE;

    // default_scene may just have changed; the editor asks scene_path, not the
    // settings struct, so it has to be right immediately.
    resolve_scene(p, *s);
    return DAI_OK;
}

// ---------------------------------------------------------------------------
// Preferences
// ---------------------------------------------------------------------------

const char *dai_prefs_path(void) {
    static char buf[512];
    std::string dir;

    if (const char *o = env_or_null("DAI_PREFS_DIR")) {
        // Portable installs, and tests: a test that wrote to the developer's
        // real ~/.config would be a test nobody dares run twice.
        dir = trim_sep(o);
    }
#ifdef _WIN32
    else if (const char *appdata = env_or_null("APPDATA")) {
        dir = join(trim_sep(appdata), "daidalos");
    } else if (const char *up = env_or_null("USERPROFILE")) {
        dir = join(join(trim_sep(up), "AppData\\Roaming"), "daidalos");
    }
#else
    else if (const char *xdg = env_or_null("XDG_CONFIG_HOME")) {
        dir = join(trim_sep(xdg), "daidalos");
    } else if (const char *home = env_or_null("HOME")) {
        dir = join(join(trim_sep(home), ".config"), "daidalos");
    }
#endif
    // No environment at all (a service, a stripped container). The current
    // directory is a poor home for preferences, but losing them is better than
    // writing to a path built out of nothing.
    if (dir.empty()) dir = ".";

    copy_str(buf, sizeof(buf), join(dir, "prefs.txt"));
    return buf;
}

dai_prefs dai_prefs_default(void) {
    dai_prefs p{};
    p.ui_scale         = 1.0f;
    p.theme            = 0;       // dark: the editor is looked at for hours
    p.cam_speed        = 6.0f;    // m/s, walking pace through a scene
    p.gizmo_px         = 80.0f;   // pixels, so it stays grabbable at any zoom
    p.snap_translate   = 0.0f;    // off, like Unity
    p.snap_rotate_deg  = 15.0f;
    p.autosave_seconds = 300;
    p.last_project[0]  = 0;
    return p;
}

dai_result dai_prefs_load(dai_prefs *out) {
    if (!out) return DAI_ERR_INVALID_ARG;
    *out = dai_prefs_default();

    std::string text;
    if (!read_file(dai_prefs_path(), text)) return DAI_OK;   // first ever start

    std::vector<std::string> lines = split_lines(text);
    bool seen_header = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const char *p0 = skip_ws(lines[i].c_str());
        if (!*p0 || *p0 == '\r' || *p0 == '#') continue;
        std::string key;
        const char *after = token(p0, key);

        if (!seen_header) {
            if (key != PREFS_MAGIC) return DAI_ERR_FILE;
            seen_header = true;
            continue;
        }
        if      (key == "ui-scale")      parse_floats(after, &out->ui_scale, 1);
        else if (key == "theme")         parse_int(after, &out->theme);
        else if (key == "cam-speed")     parse_floats(after, &out->cam_speed, 1);
        else if (key == "gizmo-px")      parse_floats(after, &out->gizmo_px, 1);
        else if (key == "snap-translate")parse_floats(after, &out->snap_translate, 1);
        else if (key == "snap-rotate")   parse_floats(after, &out->snap_rotate_deg, 1);
        else if (key == "autosave")      parse_int(after, &out->autosave_seconds);
        else if (key == "last-project")  copy_str(out->last_project, sizeof(out->last_project),
                                                  rest_of_line(after));
        // Unknown keys are skipped and NOT preserved: unlike the project file,
        // this one is not shared with anyone, so there is no colleague whose
        // settings could be lost - only this machine's own older self.
    }
    if (!seen_header) return DAI_ERR_FILE;
    return DAI_OK;
}

dai_result dai_prefs_save(const dai_prefs *pr) {
    if (!pr) return DAI_ERR_INVALID_ARG;
    std::string full = dai_prefs_path();
    size_t slash = full.find_last_of("/\\");
    if (slash != std::string::npos) {
        std::string dir = full.substr(0, slash);
        if (!dir.empty() && !make_dirs(dir)) return DAI_ERR_FILE;
    }

    const dai_prefs d = dai_prefs_default();
    std::string t;
    put(t, "%s %d\n", PREFS_MAGIC, FORMAT_VERSION);
    put(t, "# this machine only - never commit this file\n");
    if (pr->ui_scale != d.ui_scale)               put(t, "ui-scale %s\n", fstr(pr->ui_scale).c_str());
    if (pr->theme != d.theme)                     put(t, "theme %d\n", pr->theme);
    if (pr->cam_speed != d.cam_speed)             put(t, "cam-speed %s\n", fstr(pr->cam_speed).c_str());
    if (pr->gizmo_px != d.gizmo_px)               put(t, "gizmo-px %s\n", fstr(pr->gizmo_px).c_str());
    if (pr->snap_translate != d.snap_translate)   put(t, "snap-translate %s\n", fstr(pr->snap_translate).c_str());
    if (pr->snap_rotate_deg != d.snap_rotate_deg) put(t, "snap-rotate %s\n", fstr(pr->snap_rotate_deg).c_str());
    if (pr->autosave_seconds != d.autosave_seconds) put(t, "autosave %d\n", pr->autosave_seconds);
    if (pr->last_project[0])                      put(t, "last-project %s\n", sanitise(pr->last_project).c_str());

    return write_file_atomic(full, t) ? DAI_OK : DAI_ERR_FILE;
}

} // extern "C"
