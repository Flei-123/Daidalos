// Daidalos - native (C++) behaviours: compile a .cpp in the project to a
// shared library and load it while the editor is running. See dai_native.h for
// why it is a shared library and why the API is a C struct.

#include "dai_native.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <sys/stat.h>
#  define DAI_LIB_EXT ".dll"
#else
#  include <dlfcn.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  define DAI_LIB_EXT ".so"
#endif

namespace {

typedef void (*InitFn)(const dai_native_api *, dai_nentity);
typedef void (*FrameFn)(const dai_native_api *, dai_nentity, float);

struct Slot {
    bool        alive = false;
    std::string source;
    std::string lib;
    void       *handle = nullptr;
    InitFn      init = nullptr;
    FrameFn     frame = nullptr;
    long long   built_mtime = 0;
};

long long file_mtime(const std::string &path) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0) return 0;
    return (long long)st.st_mtime;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (long long)st.st_mtime;
#endif
}

bool file_exists(const std::string &p) { return file_mtime(p) != 0; }

std::string base_no_ext(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = b.find_last_of('.');
    if (dot != std::string::npos) b = b.substr(0, dot);
    // Anything that is not a plain identifier becomes '_': the name ends up on
    // a command line, and a space or a quote in it is a command injection.
    for (char &c : b) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) c = '_';
    }
    if (b.empty()) b = "behaviour";
    return b;
}

// A path is quoted for the shell exactly once, here.
std::string q(const std::string &s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
#endif
}

bool have_program(const char *name) {
#ifdef _WIN32
    std::string cmd = std::string("where ") + name + " >nul 2>nul";
#else
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

const char *find_compiler() {
    // Order matters: the one most likely to already be installed AND to
    // produce a library this process can load without an ABI argument.
#ifdef _WIN32
    static const char *CANDIDATES[] = { "g++", "clang++", "c++", nullptr };
#else
    static const char *CANDIDATES[] = { "g++", "clang++", "c++", nullptr };
#endif
    for (int i = 0; CANDIDATES[i]; ++i)
        if (have_program(CANDIDATES[i])) return CANDIDATES[i];
    return nullptr;
}

} // namespace

// Generated from include/dai_native.h by tools/embed_native.py. The editor
// writes it next to the cache before every compile, so the header a behaviour
// is built against is always the one this exact binary defines.
extern "C" const char *dai_native_header_text(void);

struct dai_native {
    std::string       cache;
    std::string       inc;
    const char       *compiler = nullptr;
    bool              compiler_checked = false;
    std::vector<Slot> slots;
};

extern "C" {

dai_native *dai_native_create(const char *cache_dir) {
    dai_native *n = new dai_native();
    n->cache = cache_dir ? cache_dir : ".";
#ifdef _WIN32
    CreateDirectoryA(n->cache.c_str(), nullptr);
#else
    mkdir(n->cache.c_str(), 0755);
#endif
    n->inc = n->cache + "/include";
#ifdef _WIN32
    CreateDirectoryA(n->inc.c_str(), nullptr);
#else
    mkdir(n->inc.c_str(), 0755);
#endif
    std::string hdr = n->inc + "/dai_native.h";
    if (const char *text = dai_native_header_text()) {
        // Rewritten every start rather than only when missing: an editor
        // update changes the ABI, and a stale header is a crash inside a
        // shared library instead of a compile error.
        if (FILE *f = std::fopen(hdr.c_str(), "wb")) {
            std::fwrite(text, 1, std::strlen(text), f);
            std::fclose(f);
        }
    }
    return n;
}

void dai_native_destroy(dai_native *n) {
    if (!n) return;
    for (size_t i = 0; i < n->slots.size(); ++i) dai_native_unload(n, (int)i);
    delete n;
}

const char *dai_native_compiler(dai_native *n) {
    if (!n) return nullptr;
    if (!n->compiler_checked) { n->compiler_checked = true; n->compiler = find_compiler(); }
    return n->compiler;
}

int dai_native_load(dai_native *n, const char *source_path, const char *include_dir,
                    char *err, size_t err_len) {
    auto fail = [&](const std::string &m) -> int {
        if (err && err_len) std::snprintf(err, err_len, "%s", m.c_str());
        return -1;
    };
    if (!n || !source_path || !*source_path) return fail("no source");
    std::string src = source_path;
    if (!file_exists(src)) return fail("file not found: " + src);

    const char *cc = dai_native_compiler(n);
    if (!cc)
        return fail("no C++ compiler found. Install one and restart:\n"
                    "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT "
                    "(or MSYS2 mingw-w64-x86_64-gcc)\n"
                    "  Linux:   apt install g++");

    std::string name = base_no_ext(src);
    // The library name carries the source's timestamp. Windows will not let
    // you overwrite a DLL that is currently loaded, and a fixed name means the
    // second build of the same file fails - which is exactly the case hot
    // reload is for.
    long long mt = file_mtime(src);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%lld", mt);
    std::string lib = n->cache + "/" + name + "_" + stamp + DAI_LIB_EXT;

    if (!file_exists(lib)) {
        std::string log = n->cache + "/" + name + ".buildlog";
        std::string cmd = std::string(cc) + " -std=c++17 -O2 -shared -fPIC ";
#ifdef _WIN32
        // The library must not need libstdc++/libgcc next to it, or loading it
        // on a machine without the toolchain on PATH fails with a message
        // Windows renders as "the specified module could not be found".
        cmd += "-static-libgcc -static-libstdc++ ";
#endif
        // The generated header always wins; an extra include dir from the host
        // (the project's own headers) comes after it.
        cmd += "-I" + q(n->inc) + " ";
        if (include_dir && *include_dir) cmd += "-I" + q(include_dir) + " ";
        cmd += q(src) + " -o " + q(lib) + " > " + q(log) + " 2>&1";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            // Hand back what the compiler said, not "compilation failed".
            std::string out;
            if (FILE *f = std::fopen(log.c_str(), "rb")) {
                char buf[512];
                size_t got;
                while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
                std::fclose(f);
            }
            if (out.size() > 900) out = out.substr(0, 900) + " ...";
            return fail(out.empty() ? "compile failed" : out);
        }
    }

    void *h = nullptr;
#ifdef _WIN32
    h = (void *)LoadLibraryA(lib.c_str());
    if (!h) {
        char m[128];
        std::snprintf(m, sizeof(m), "LoadLibrary failed (%lu)", (unsigned long)GetLastError());
        return fail(m);
    }
    InitFn  ifn = (InitFn)(void *)GetProcAddress((HMODULE)h, "dai_behaviour_init");
    FrameFn ffn = (FrameFn)(void *)GetProcAddress((HMODULE)h, "dai_behaviour_frame");
#else
    h = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return fail(dlerror() ? dlerror() : "dlopen failed");
    InitFn  ifn = (InitFn)dlsym(h, "dai_behaviour_init");
    FrameFn ffn = (FrameFn)dlsym(h, "dai_behaviour_frame");
#endif
    if (!ifn && !ffn) {
#ifdef _WIN32
        FreeLibrary((HMODULE)h);
#else
        dlclose(h);
#endif
        return fail("no dai_behaviour_init or dai_behaviour_frame in the file - "
                    "use the DAI_BEHAVIOUR_INIT / DAI_BEHAVIOUR_FRAME macros");
    }

    int id = -1;
    for (size_t i = 0; i < n->slots.size(); ++i)
        if (!n->slots[i].alive) { id = (int)i; break; }
    if (id < 0) { n->slots.push_back(Slot()); id = (int)n->slots.size() - 1; }
    Slot &s = n->slots[(size_t)id];
    s = Slot();
    s.alive = true;
    s.source = src;
    s.lib = lib;
    s.handle = h;
    s.init = ifn;
    s.frame = ffn;
    s.built_mtime = mt;
    if (err && err_len) err[0] = 0;
    return id;
}

void dai_native_unload(dai_native *n, int id) {
    if (!n || id < 0 || (size_t)id >= n->slots.size()) return;
    Slot &s = n->slots[(size_t)id];
    if (!s.alive) return;
    if (s.handle) {
#ifdef _WIN32
        FreeLibrary((HMODULE)s.handle);
#else
        dlclose(s.handle);
#endif
    }
    s = Slot();
}

int dai_native_stale(dai_native *n, int id) {
    if (!n || id < 0 || (size_t)id >= n->slots.size()) return 0;
    const Slot &s = n->slots[(size_t)id];
    if (!s.alive) return 0;
    long long now = file_mtime(s.source);
    return (now != 0 && now != s.built_mtime) ? 1 : 0;
}

void dai_native_init(dai_native *n, int id, const dai_native_api *api, dai_nentity self) {
    if (!n || id < 0 || (size_t)id >= n->slots.size()) return;
    const Slot &s = n->slots[(size_t)id];
    if (s.alive && s.init) s.init(api, self);
}

void dai_native_frame(dai_native *n, int id, const dai_native_api *api,
                      dai_nentity self, float dt) {
    if (!n || id < 0 || (size_t)id >= n->slots.size()) return;
    const Slot &s = n->slots[(size_t)id];
    if (s.alive && s.frame) s.frame(api, self, dt);
}

} // extern "C"
