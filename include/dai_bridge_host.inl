// MODULE 4 (Jarvis bridge) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that can be driven from outside:
// examples/editor_demo.cpp and tools/modeling_shot.cpp. It is the local TCP
// socket an outside tool (tools/daibridge.py, or Jarvis) talks to.
//
// The contract, and none of it is negotiable:
//
//   * OFF by default. It opens only when the host was told to - a flag on the
//     command line or a project setting - and it says in the console that it
//     is open and on which port.
//   * 127.0.0.1 ONLY. Bound to the loopback address, never to 0.0.0.0. A
//     modelling socket that answers the network is a remote code execution
//     hole, because "run this JS" is exactly what it is for.
//   * One JSON object per line, in and out. No length prefixes, no framing to
//     get wrong, and a human can talk to it with netcat.
//   * Everything it changes goes through the SAME path the panels use:
//     dai_doc_begin/commit around each command, the existing JS API for the
//     work itself. Undo after a bridge command undoes exactly that command.
//   * Non blocking. A bridge with nobody connected costs one poll per frame;
//     a client that stops reading must not freeze the editor.
//
// Called once per frame, near the end, after the document has settled.
//
// ---- how it is switched on ------------------------------------------------
//
// The seam has no command line of its own - it is handed a dai_ext_host and
// nothing else - so the switch is the environment, which every host on every
// platform already has and which no host file has to grow an argument for:
//
//     DAI_BRIDGE_PORT=8181 ./build/editor_demo        # the running editor
//     ./build/modeling_shot --bridge 8181 out/        # the headless one
//
// Unset or 0 is off, and off means the socket is never created. There is no
// "listen but refuse" state: a port that is open is a port that answers, and a
// port that is closed cannot be probed.
//
// ---- the protocol ---------------------------------------------------------
//
// Request and answer are one JSON object per line. Every answer carries "ok".
//
//   {"cmd":"ping"}                  -> {"ok":true,"version":1,"nodes":7,...}
//   {"cmd":"eval","code":"..."}     -> {"ok":true,"result":"<last value>"}
//   {"cmd":"scene"}                 -> {"ok":true,"nodes":[{...},...]}
//   {"cmd":"save","path":"a.daiscene"}
//   {"cmd":"shot","path":"a.png","eye":[4,2,6],"target":[0,1,0],"fov":55}
//   {"cmd":"undo"} / {"cmd":"redo"} -> {"ok":true,"undo":N,"redo":M}
//   {"cmd":"quit"}                  -> {"ok":true,"bye":true}
//
// `eval` is the whole modelling API: it runs in a script context bound to the
// SAME node table the behaviours and the inspector use (dai_script_bind_nodes
// with the host's `g_node_host`) plus the tool half of it (`editor`, see
// dai_script.h). So "make a wall" from Jarvis and "make a wall" typed into the
// script editor are the same call, and the undo stack cannot tell them apart -
// which is the point.
//
// The value of the last expression comes back as `result`, the way a REPL
// answers, so a caller can ASK the scene something instead of only telling it
// things. That is done by evaluating the code through JavaScript's own eval
// inside one wrapper statement rather than by adding a second entry point to
// dai_script.h - the runtime already has the feature.

#include "dai_scene.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>

#ifdef DAI_WITH_SCRIPT
#include "dai_script.h"
#endif

#ifdef _WIN32
// Winsock is loaded at RUN time, not linked. build_win.sh is frozen and does
// not name -lws2_32, and a feature that is off by default has no business
// forcing a library onto every build of the editor. Six functions and two
// constants is the whole of what a loopback listener needs.
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace daibridge {

// ---------------------------------------------------------------- sockets
#ifdef _WIN32
typedef uintptr_t sock_t;
static const sock_t SOCK_NONE = (sock_t)~(uintptr_t)0;

struct WinSock {
    int  loaded = 0;
    int  ok = 0;
    int  (__stdcall *startup)(WORD, void *) = nullptr;
    sock_t (__stdcall *socket_)(int, int, int) = nullptr;
    int  (__stdcall *bind_)(sock_t, const void *, int) = nullptr;
    int  (__stdcall *listen_)(sock_t, int) = nullptr;
    sock_t (__stdcall *accept_)(sock_t, void *, int *) = nullptr;
    int  (__stdcall *recv_)(sock_t, char *, int, int) = nullptr;
    int  (__stdcall *send_)(sock_t, const char *, int, int) = nullptr;
    int  (__stdcall *closesocket_)(sock_t) = nullptr;
    int  (__stdcall *ioctlsocket_)(sock_t, long, unsigned long *) = nullptr;
    int  (__stdcall *setsockopt_)(sock_t, int, int, const char *, int) = nullptr;
    int  (__stdcall *lasterror)() = nullptr;
};

static WinSock &winsock() {
    static WinSock w;
    if (w.loaded) return w;
    w.loaded = 1;
    HMODULE m = LoadLibraryA("ws2_32.dll");
    if (!m) return w;
    // memcpy rather than a cast through FARPROC*: a function pointer written
    // through a pointer of another type is exactly what -Wstrict-aliasing is
    // for, and the Windows build compiles with -Wall.
    #define DAI_BR_GET(field, name)                            \
        do {                                                   \
            FARPROC fn_ = GetProcAddress(m, name);             \
            if (!fn_) return w;                                \
            std::memcpy((void *)&w.field, &fn_, sizeof(fn_));  \
        } while (0)
    DAI_BR_GET(startup, "WSAStartup");
    DAI_BR_GET(socket_, "socket");
    DAI_BR_GET(bind_, "bind");
    DAI_BR_GET(listen_, "listen");
    DAI_BR_GET(accept_, "accept");
    DAI_BR_GET(recv_, "recv");
    DAI_BR_GET(send_, "send");
    DAI_BR_GET(closesocket_, "closesocket");
    DAI_BR_GET(ioctlsocket_, "ioctlsocket");
    DAI_BR_GET(setsockopt_, "setsockopt");
    DAI_BR_GET(lasterror, "WSAGetLastError");
    #undef DAI_BR_GET
    char wsadata[512];
    if (w.startup(0x0202, wsadata) != 0) return w;
    w.ok = 1;
    return w;
}

static void sock_close(sock_t s) { if (s != SOCK_NONE) winsock().closesocket_(s); }
static int  sock_nonblock(sock_t s) {
    unsigned long on = 1;
    return winsock().ioctlsocket_(s, (long)0x8004667E /* FIONBIO */, &on) == 0;
}
static int sock_would_block() { return winsock().lasterror() == 10035 /* WSAEWOULDBLOCK */; }
static int sock_recv(sock_t s, char *buf, int n) { return winsock().recv_(s, buf, n, 0); }
static int sock_send(sock_t s, const char *buf, int n) { return winsock().send_(s, buf, n, 0); }
#else
typedef int sock_t;
static const sock_t SOCK_NONE = -1;

static void sock_close(sock_t s) { if (s != SOCK_NONE) ::close(s); }
static int  sock_nonblock(sock_t s) {
    int fl = ::fcntl(s, F_GETFL, 0);
    return fl >= 0 && ::fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
}
static int sock_would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
static int sock_recv(sock_t s, char *buf, int n) { return (int)::recv(s, buf, (size_t)n, 0); }
static int sock_send(sock_t s, const char *buf, int n) {
#ifdef MSG_NOSIGNAL
    return (int)::send(s, buf, (size_t)n, MSG_NOSIGNAL);
#else
    return (int)::send(s, buf, (size_t)n, 0);
#endif
}
#endif

// ---------------------------------------------------------------- tiny JSON
//
// The protocol is our own and one line wide, so it gets a reader of its own
// rather than a dependency: examples/ is compiled with -Iinclude only, and
// src/dai_json.hpp is not on that path. Objects, arrays, strings, numbers,
// true/false/null - the whole grammar, no shortcuts, because a parser that
// only handles the messages we happen to send is a parser that lies about the
// first message somebody else sends.
struct Value;
typedef std::vector<std::pair<std::string, Value> > Members;

struct Value {
    enum Kind { NONE, NUL, BOOL, NUM, STR, ARR, OBJ };
    Kind kind = NONE;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    Members obj;

    const Value *find(const char *name) const {
        for (size_t i = 0; i < obj.size(); ++i)
            if (obj[i].first == name) return &obj[i].second;
        return nullptr;
    }
    std::string text(const char *name, const char *fallback = "") const {
        const Value *v = find(name);
        return (v && v->kind == STR) ? v->str : std::string(fallback);
    }
    double number(const char *name, double fallback) const {
        const Value *v = find(name);
        return (v && v->kind == NUM) ? v->num : fallback;
    }
    int vec3(const char *name, double *out) const {
        const Value *v = find(name);
        if (!v || v->kind != ARR || v->arr.size() < 3) return 0;
        for (int i = 0; i < 3; ++i) out[i] = v->arr[(size_t)i].kind == NUM ? v->arr[(size_t)i].num : 0.0;
        return 1;
    }
};

struct Reader {
    const char *p = nullptr;
    const char *end = nullptr;
    int failed = 0;

    void space() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    int lit(const char *word) {
        size_t n = std::strlen(word);
        if ((size_t)(end - p) < n || std::strncmp(p, word, n) != 0) return 0;
        p += n;
        return 1;
    }
    std::string string_() {
        std::string out;
        if (p >= end || *p != '"') { failed = 1; return out; }
        ++p;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { out.push_back(c); continue; }
            if (p >= end) { failed = 1; return out; }
            char e = *p++;
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case '/': out.push_back('/'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'u': {
                if (end - p < 4) { failed = 1; return out; }
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = p[i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else { failed = 1; return out; }
                }
                p += 4;
                // UTF-8, the three cases a BMP code point can take. Surrogate
                // pairs arrive as two escapes and are written through as they
                // came: JS reads them back as the same string.
                if (cp < 0x80) out.push_back((char)cp);
                else if (cp < 0x800) {
                    out.push_back((char)(0xC0 | (cp >> 6)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: failed = 1; return out;
            }
        }
        if (p >= end) { failed = 1; return out; }
        ++p;
        return out;
    }
    Value value() {
        Value v;
        space();
        if (p >= end) { failed = 1; return v; }
        if (*p == '{') {
            ++p;
            v.kind = Value::OBJ;
            space();
            if (p < end && *p == '}') { ++p; return v; }
            for (;;) {
                space();
                std::string key = string_();
                if (failed) return v;
                space();
                if (p >= end || *p != ':') { failed = 1; return v; }
                ++p;
                Value member = value();
                if (failed) return v;
                v.obj.push_back(std::make_pair(key, member));
                space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return v; }
                failed = 1;
                return v;
            }
        }
        if (*p == '[') {
            ++p;
            v.kind = Value::ARR;
            space();
            if (p < end && *p == ']') { ++p; return v; }
            for (;;) {
                Value item = value();
                if (failed) return v;
                v.arr.push_back(item);
                space();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return v; }
                failed = 1;
                return v;
            }
        }
        if (*p == '"') { v.kind = Value::STR; v.str = string_(); return v; }
        if (lit("true")) { v.kind = Value::BOOL; v.num = 1.0; return v; }
        if (lit("false")) { v.kind = Value::BOOL; v.num = 0.0; return v; }
        if (lit("null")) { v.kind = Value::NUL; return v; }
        {
            char *stop = nullptr;
            double d = std::strtod(p, &stop);
            if (!stop || stop == p) { failed = 1; return v; }
            p = stop;
            v.kind = Value::NUM;
            v.num = d;
            return v;
        }
    }
};

static int parse(const std::string &text, Value *out) {
    Reader r;
    r.p = text.c_str();
    r.end = r.p + text.size();
    *out = r.value();
    return !r.failed && out->kind != Value::NONE;
}

// ---- writing. One string, built by hand: the answers have five shapes.
static std::string quote(const std::string &s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out += esc;
            } else {
                out.push_back((char)c);
            }
        }
    }
    out.push_back('"');
    return out;
}

static std::string number(double v) {
    char buf[40];
    // %.9g: a float survives the round trip and an integer still reads as one.
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

static std::string vec3(const dai_vec3 &v) {
    return "[" + number(v.x) + "," + number(v.y) + "," + number(v.z) + "]";
}

// ---------------------------------------------------------------- the state
struct Client {
    sock_t fd = SOCK_NONE;
    std::string in;
    std::string out;
};

struct Bridge {
    int    started = 0;        /* did we look at the environment yet          */
    int    port = 0;           /* 0 = off, and off is the default             */
    sock_t listener = SOCK_NONE;
    std::vector<Client> clients;
    int    quit = 0;           /* a client said so; a headless host may leave */
    uint32_t handled = 0;      /* commands answered, for the console line     */

    const dai_ext_host *host = nullptr;   /* valid only inside one poll       */
    /* The camera a `shot` uses. Set by editor.camera(), kept between calls so
     * a caller can point the camera once and take five pictures. */
    dai_vec3 eye{ 4.0f, 2.4f, 6.0f };
    dai_vec3 target{ 0.0f, 1.0f, 0.0f };
    float    fov = 55.0f;
    int      camera_set = 0;

#ifdef DAI_WITH_SCRIPT
    dai_script *script = nullptr;
#endif
};

static Bridge &state() {
    static Bridge b;
    return b;
}

static dai_doc *doc() {
    Bridge &b = state();
    return b.host ? b.host->doc : nullptr;
}

// ---------------------------------------------------------- the editor half
//
// The `editor` global of dai_script.h, implemented against the document the
// seam was handed. Every mutation is bracketed by dai_doc_begin/commit, and
// the brackets NEST (dai_doc.h counts them), so a script that opens its own
// transaction around a whole room still gets one undo step and a script that
// does not still gets one per call.
#ifdef DAI_WITH_SCRIPT

static double eh_add(const char *name, double parent, void *) {
    dai_doc *d = doc();
    if (!d) return -1.0;
    dai_node_desc r = dai_node_desc_default();
    std::snprintf(r.name, sizeof(r.name), "%s", (name && *name) ? name : "Node");
    dai_node p = (dai_node)(uint32_t)(parent > 0.0 ? parent : 0.0);
    if (p && dai_doc_valid(d, p)) r.parent = p;
    dai_doc_begin(d, "bridge: add node");
    dai_node n = dai_doc_add(d, &r);
    dai_doc_commit(d);
    return n ? (double)(uint32_t)n : -1.0;
}

static int eh_remove(double id, void *) {
    dai_doc *d = doc();
    dai_node n = (dai_node)(uint32_t)id;
    if (!d || !n || !dai_doc_valid(d, n)) return 0;
    dai_doc_begin(d, "bridge: remove node");
    int ok = dai_doc_remove(d, n) == DAI_OK;
    dai_doc_commit(d);
    return ok;
}

static void eh_begin(const char *label, void *) {
    dai_doc *d = doc();
    if (d) dai_doc_begin(d, (label && *label) ? label : "bridge");
}

static void eh_commit(void *) {
    dai_doc *d = doc();
    if (d) dai_doc_commit(d);
}

static int eh_undo(void *) {
    dai_doc *d = doc();
    return d ? dai_doc_undo(d) : 0;
}

static int eh_redo(void *) {
    dai_doc *d = doc();
    return d ? dai_doc_redo(d) : 0;
}

static double eh_count(void *) {
    dai_doc *d = doc();
    return d ? (double)dai_doc_count(d) : 0.0;
}

static double eh_at(double index, void *) {
    dai_doc *d = doc();
    if (!d || index < 0.0) return -1.0;
    uint32_t n = dai_doc_count(d);
    if ((uint32_t)index >= n) return -1.0;
    std::vector<dai_node> all(n ? n : 1);
    dai_doc_nodes(d, all.data(), n);
    return (double)(uint32_t)all[(size_t)index];
}

static int eh_save(const char *path, void *) {
    dai_doc *d = doc();
    if (!d || !path || !*path) return 0;
    return dai_doc_save(d, path) == DAI_OK;
}

// Slot 0 of the material stack - the object's own surface. The rest of the
// stack is left exactly as it was: a wall that already had a second slot does
// not lose it because somebody re-pointed its first one.
static int eh_set_material(double id, const char *path, void *) {
    dai_doc *d = doc();
    dai_node n = (dai_node)(uint32_t)id;
    dai_node_desc r{};
    if (!d || !n || dai_doc_get(d, n, &r) != DAI_OK) return 0;
    std::string rest = r.materials;
    size_t semi = rest.find(';');
    rest = semi == std::string::npos ? std::string() : rest.substr(semi);
    std::string all = std::string(path ? path : "") + rest;
    if (all.size() >= sizeof(r.materials)) return 0;
    std::snprintf(r.materials, sizeof(r.materials), "%s", all.c_str());
    dai_doc_begin(d, "bridge: assign material");
    int ok = dai_doc_set(d, n, &r) == DAI_OK;
    dai_doc_commit(d);
    return ok;
}

static const char *eh_get_material(double id, void *) {
    static char slot[256];
    slot[0] = 0;
    dai_doc *d = doc();
    dai_node_desc r{};
    if (!d || dai_doc_get(d, (dai_node)(uint32_t)id, &r) != DAI_OK) return slot;
    std::string first = r.materials;
    size_t semi = first.find(';');
    if (semi != std::string::npos) first = first.substr(0, semi);
    std::snprintf(slot, sizeof(slot), "%s", first.c_str());
    return slot;
}

static void eh_select(double id, void *);
static void eh_camera(const double *eye, const double *target, double fov, void *);
static int  eh_shot(const char *path, void *);

#endif /* DAI_WITH_SCRIPT */

// The selection lives in the editor, which a seam does not get - a host that
// wants `editor.select()` to move ITS selection sets this hook before it
// includes the file. Nothing else in here needs the editor, and a hook that
// nobody sets is a selection that is simply not moved.
#ifndef DAI_BRIDGE_SELECT
#define DAI_BRIDGE_SELECT(node) ((void)(node))
#endif

// ------------------------------------------------------------- a screenshot
//
// The scene as the bridge's camera sees it, into a PNG. Not the panels: the
// seam has no UI, and a tool that wanted the panels asks the host for them
// (tools/modeling_shot.cpp does exactly that through DAI_BRIDGE_SHOT).
#ifndef DAI_BRIDGE_SHOT
#define DAI_BRIDGE_SHOT(path) daibridge::shot_scene(path)
#endif

static inline int shot_scene(const char *path) {
    Bridge &b = state();
    if (!b.host || !b.host->renderer || !b.host->scene || !path || !*path) return 0;
    dai_render_camera(b.host->renderer, b.eye, b.target, dai_vec3{ 0, 1, 0 }, b.fov, 0.05f, 400.0f);
    std::vector<dai_render_instance> inst(4096);
    uint32_t n = dai_scene_instances(b.host->scene, inst.data(), (uint32_t)inst.size(), 1.0f);
    if (n > (uint32_t)inst.size()) n = (uint32_t)inst.size();
    if (dai_render_frame(b.host->renderer, inst.data(), n) != DAI_OK) return 0;
    return dai_render_write_png(b.host->renderer, path) == DAI_OK;
}

#ifdef DAI_WITH_SCRIPT
static void eh_select(double id, void *) {
    dai_node n = (dai_node)(uint32_t)(id > 0.0 ? id : 0.0);
    DAI_BRIDGE_SELECT(n);
}

static void eh_camera(const double *eye, const double *target, double fov, void *) {
    Bridge &b = state();
    b.eye = dai_vec3{ (float)eye[0], (float)eye[1], (float)eye[2] };
    b.target = dai_vec3{ (float)target[0], (float)target[1], (float)target[2] };
    if (fov > 1.0) b.fov = (float)fov;
    b.camera_set = 1;
}

static int eh_shot(const char *path, void *) {
    return DAI_BRIDGE_SHOT(path);
}

// The context is made on the FIRST command, not when the socket opens: a
// bridge nobody talks to costs nothing, and a QuickJS runtime that is never
// used is 200 kB of nothing.
static dai_script *script_context() {
    Bridge &b = state();
    if (b.script) return b.script;
    char err[256] = { 0 };
    b.script = dai_script_create(err, sizeof(err));
    if (!b.script) {
        std::printf("bridge: no script runtime (%s)\n", err);
        return nullptr;
    }
    // The same table the behaviours and the inspector use - see the file
    // comment. The host defines it; a host without one has no `scene`/`node`
    // globals and the bridge says so rather than pretending.
    dai_script_bind_nodes(b.script, &g_node_host);
    dai_script_editor_host eh{};
    eh.add = eh_add;
    eh.remove = eh_remove;
    eh.begin = eh_begin;
    eh.commit = eh_commit;
    eh.undo = eh_undo;
    eh.redo = eh_redo;
    eh.count = eh_count;
    eh.at = eh_at;
    eh.save = eh_save;
    eh.select = eh_select;
    eh.camera = eh_camera;
    eh.shot = eh_shot;
    eh.set_material = eh_set_material;
    eh.get_material = eh_get_material;
    eh.user = nullptr;
    dai_script_bind_editor(b.script, &eh);
    return b.script;
}
#endif /* DAI_WITH_SCRIPT */

// ------------------------------------------------------------- the commands
static std::string scene_json() {
    dai_doc *d = doc();
    std::string out = "{\"ok\":true,\"nodes\":[";
    if (!d) return out + "]}";
    uint32_t n = dai_doc_count(d);
    std::vector<dai_node> all(n ? n : 1);
    if (n) dai_doc_nodes(d, all.data(), n);
    for (uint32_t i = 0; i < n; ++i) {
        dai_node_desc r{};
        if (dai_doc_get(d, all[i], &r) != DAI_OK) continue;
        if (i) out += ",";
        out += "{\"id\":" + number((double)(uint32_t)all[i]);
        out += ",\"name\":" + quote(r.name);
        out += ",\"parent\":" + number((double)(uint32_t)r.parent);
        out += ",\"position\":" + vec3(r.position);
        out += ",\"rotation\":[" + number(r.rotation.x) + "," + number(r.rotation.y) + "," +
               number(r.rotation.z) + "," + number(r.rotation.w) + "]";
        out += ",\"scale\":" + vec3(r.scale);
        out += ",\"half_extent\":" + vec3(r.half_extent);
        out += ",\"render_extent\":" + vec3(r.render_extent);
        out += ",\"asset\":" + quote(r.asset);
        out += ",\"materials\":" + quote(r.materials);
        out += ",\"script\":" + quote(r.script);
        out += "}";
    }
    return out + "]}";
}

static std::string fail(const std::string &why) {
    return "{\"ok\":false,\"error\":" + quote(why) + "}";
}

static std::string handle(const std::string &line) {
    Value msg;
    if (!parse(line, &msg) || msg.kind != Value::OBJ) return fail("not a JSON object");
    std::string cmd = msg.text("cmd");
    dai_doc *d = doc();
    ++state().handled;

    if (cmd == "ping") {
        std::string out = "{\"ok\":true,\"version\":1";
        out += ",\"nodes\":" + number(d ? (double)dai_doc_count(d) : 0.0);
        out += ",\"undo\":" + number(d ? (double)dai_doc_undo_depth(d) : 0.0);
        out += ",\"redo\":" + number(d ? (double)dai_doc_redo_depth(d) : 0.0);
        out += ",\"script\":";
#ifdef DAI_WITH_SCRIPT
        out += "true";
#else
        out += "false";
#endif
        return out + "}";
    }

    if (cmd == "scene") return scene_json();

    if (cmd == "eval") {
        const Value *code = msg.find("code");
        if (!code || code->kind != Value::STR) return fail("eval needs a \"code\" string");
#ifdef DAI_WITH_SCRIPT
        dai_script *s = script_context();
        if (!s) return fail("no script runtime");
        if (!d) return fail("no document open");
        // One undo step for the whole line, whatever it touched. The step is
        // opened HERE rather than inside the script so that a caller who
        // forgot editor.begin() still gets one Ctrl-Z, and a caller who used
        // it gets exactly the same thing - the brackets nest.
        dai_doc_begin(d, "bridge: eval");
        // JavaScript's own eval gives the value of the last expression, which
        // is what a REPL answers with. `__dai_bridge_r` is assigned without
        // `var` on purpose: a direct eval in global code must not leave a
        // binding behind that the next command trips over.
        std::string wrapped =
            "state.result = \"\"; __dai_bridge_r = eval(" + quote(code->str) + ");"
            "if (__dai_bridge_r !== undefined && __dai_bridge_r !== null)"
            " state.result = (typeof __dai_bridge_r === \"object\")"
            " ? JSON.stringify(__dai_bridge_r) : String(__dai_bridge_r);";
        char err[512] = { 0 };
        dai_result rc = dai_script_eval(s, wrapped.c_str(), "bridge", err, sizeof(err));
        dai_doc_commit(d);
        if (rc != DAI_OK) return fail(err[0] ? err : "script error");
        // The answer is as long as it is. It used to be read into a fixed
        // 4096 byte buffer, which cut a generated floor plan off in the middle
        // of a number and handed the caller JSON that ends without its closing
        // brace - an error that looks like a bug in the caller's parser and is
        // in fact this line.
        size_t need = dai_script_get_string_size(s, "result");
        std::vector<char> result(need > 0 ? need : 1, 0);
        dai_script_get_string(s, "result", result.data(), result.size());
        return "{\"ok\":true,\"result\":" + quote(result.data()) +
               ",\"undo\":" + number((double)dai_doc_undo_depth(d)) + "}";
#else
        return fail("this build has no script runtime");
#endif
    }

    if (cmd == "save") {
        std::string path = msg.text("path");
        if (!d) return fail("no document open");
        if (path.empty()) return fail("save needs a \"path\"");
        if (dai_doc_save(d, path.c_str()) != DAI_OK) return fail("could not write " + path);
        return "{\"ok\":true,\"path\":" + quote(path) + "}";
    }

    if (cmd == "shot") {
        Bridge &b = state();
        std::string path = msg.text("path");
        if (path.empty()) return fail("shot needs a \"path\"");
        double e[3], t[3];
        if (msg.vec3("eye", e)) {
            b.eye = dai_vec3{ (float)e[0], (float)e[1], (float)e[2] };
            b.camera_set = 1;
        }
        if (msg.vec3("target", t)) {
            b.target = dai_vec3{ (float)t[0], (float)t[1], (float)t[2] };
            b.camera_set = 1;
        }
        double f = msg.number("fov", 0.0);
        if (f > 1.0) b.fov = (float)f;
        if (!DAI_BRIDGE_SHOT(path.c_str())) return fail("could not render " + path);
        return "{\"ok\":true,\"path\":" + quote(path) + "}";
    }

    if (cmd == "undo" || cmd == "redo") {
        if (!d) return fail("no document open");
        int moved = cmd == "undo" ? dai_doc_undo(d) : dai_doc_redo(d);
        std::string out = "{\"ok\":true,\"moved\":";
        out += moved ? "true" : "false";
        out += ",\"undo\":" + number((double)dai_doc_undo_depth(d));
        out += ",\"redo\":" + number((double)dai_doc_redo_depth(d));
        return out + "}";
    }

    if (cmd == "quit") {
        state().quit = 1;
        return "{\"ok\":true,\"bye\":true}";
    }

    return fail(cmd.empty() ? "no \"cmd\" in the message" : ("unknown command: " + cmd));
}

// -------------------------------------------------------------- the listener
static int open_listener(int port) {
    Bridge &b = state();
    b.started = 1;
    if (b.listener != SOCK_NONE) return 1;
    if (port <= 0 || port > 65535) return 0;    // OFF, and off is the default
#ifdef _WIN32
    if (!winsock().ok) {
        std::printf("bridge: ws2_32.dll unavailable - the socket stays closed\n");
        return 0;
    }
    sock_t fd = winsock().socket_(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 6 /*IPPROTO_TCP*/);
    if (fd == SOCK_NONE) { std::printf("bridge: socket() failed\n"); return 0; }
    int one = 1;
    winsock().setsockopt_(fd, 0xFFFF /*SOL_SOCKET*/, 0x0004 /*SO_REUSEADDR*/, (const char *)&one, (int)sizeof(one));
    struct { short family; unsigned short port; unsigned long addr; char pad[8]; } sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.family = 2;
    sa.port = (unsigned short)(((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
    sa.addr = 0x0100007F;                        // 127.0.0.1, network order
    if (winsock().bind_(fd, &sa, (int)sizeof(sa)) != 0 || winsock().listen_(fd, 4) != 0) {
        sock_close(fd);
        return 0;
    }
#else
    sock_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_NONE) { std::printf("bridge: socket() failed\n"); return 0; }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 and nothing else
    if (::bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || ::listen(fd, 4) != 0) {
        sock_close(fd);
        return 0;
    }
#endif
    if (!sock_nonblock(fd)) {
        std::printf("bridge: could not make the listener non blocking\n");
        sock_close(fd);
        return 0;
    }
    b.listener = fd;
    b.port = port;
    // Said out loud, every time: a socket that runs code and does not announce
    // itself is the definition of a back door.
    std::printf("bridge: listening on 127.0.0.1:%d (JSON lines; DAI_BRIDGE_PORT)\n", port);
    std::fflush(stdout);
    return 1;
}

static void close_all() {
    Bridge &b = state();
    for (size_t i = 0; i < b.clients.size(); ++i) sock_close(b.clients[i].fd);
    b.clients.clear();
    sock_close(b.listener);
    b.listener = SOCK_NONE;
}

static void flush_out(Client &c) {
    while (!c.out.empty()) {
        int wrote = sock_send(c.fd, c.out.c_str(), (int)c.out.size());
        if (wrote > 0) { c.out.erase(0, (size_t)wrote); continue; }
        if (wrote < 0 && sock_would_block()) return;   // try again next frame
        c.out.clear();                                  // gone; the read side notices
        return;
    }
}

static void poll() {
    Bridge &b = state();
    if (b.listener == SOCK_NONE) return;

    for (;;) {
#ifdef _WIN32
        sock_t fd = winsock().accept_(b.listener, nullptr, nullptr);
#else
        sock_t fd = ::accept(b.listener, nullptr, nullptr);
#endif
        if (fd == SOCK_NONE) break;
        if (!sock_nonblock(fd)) { sock_close(fd); continue; }
        Client c;
        c.fd = fd;
        b.clients.push_back(c);
        if (b.clients.size() > 8) {           // a tool, not a server
            sock_close(b.clients.front().fd);
            b.clients.erase(b.clients.begin());
        }
    }

    for (size_t i = 0; i < b.clients.size();) {
        Client &c = b.clients[i];
        int alive = 1;
        char buf[4096];
        for (;;) {
            int got = sock_recv(c.fd, buf, (int)sizeof(buf));
            if (got > 0) { c.in.append(buf, (size_t)got); continue; }
            if (got == 0) { alive = 0; break; }         // the peer hung up
            if (!sock_would_block()) alive = 0;
            break;
        }
        for (;;) {
            size_t nl = c.in.find('\n');
            if (nl == std::string::npos) break;
            std::string line = c.in.substr(0, nl);
            c.in.erase(0, nl + 1);
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            c.out += handle(line);
            c.out += "\n";
        }
        // A client that stopped reading must not grow our memory without end,
        // and must not stall the editor: past a megabyte it is not a client
        // any more, it is a leak with a socket.
        if (c.in.size() > (size_t)1 << 20) alive = 0;
        flush_out(c);
        if (!alive && c.out.empty()) {
            sock_close(c.fd);
            b.clients.erase(b.clients.begin() + (std::ptrdiff_t)i);
            continue;
        }
        ++i;
    }

    if (b.quit) close_all();
}

} // namespace daibridge

static void dai_bridge_host_poll(const dai_ext_host *h) {
    daibridge::Bridge &b = daibridge::state();
    b.host = h;
    if (!b.started) {
        const char *env = std::getenv("DAI_BRIDGE_PORT");
        int port = env ? std::atoi(env) : 0;
        if (!daibridge::open_listener(port) && port > 0)
            std::printf("bridge: port %d is not available - the bridge stays closed\n", port);
    }
    daibridge::poll();
    b.host = nullptr;
}

/* Opens the bridge on an explicit port, for a host that HAS a command line:
 * tools/modeling_shot.cpp takes `--bridge 8181`. Returns 0 when the port is
 * taken, so a caller can walk to the next one instead of dying on a port
 * somebody else's editor is already sitting on. Still off by default: a host
 * that never calls this and is started without DAI_BRIDGE_PORT opens nothing. */
static inline int dai_bridge_host_open(int port) { return daibridge::open_listener(port); }

/* The port the bridge is actually on, 0 when it is closed. */
static inline int dai_bridge_host_port(void) { return daibridge::state().port; }

/* Whether a client asked the host to stop. A windowed editor ignores it - a
 * socket must not be able to close somebody's editor - and the headless tools
 * use it to end their serve loop. */
static inline int dai_bridge_host_quit(void) { return daibridge::state().quit; }

/* How many commands have been answered since the process started. The console
 * line and tools/bridge_check.sh both read it; a bridge that answered nothing
 * and a bridge that was never opened look identical without it. */
static inline uint32_t dai_bridge_host_handled(void) { return daibridge::state().handled; }
