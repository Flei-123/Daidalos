#!/usr/bin/env python3
# patch41 - the Project window becomes a file explorer: files dropped from the
# desktop are imported, and a row dragged onto a folder moves there.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p41'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

# ========================================================= 1. the window API
s = rd('include/dai_render.h')
s = sub1(s,
"""DAI_API int dai_window_double_click(dai_window *w);""",
"""DAI_API int dai_window_double_click(dai_window *w);

/* Files dropped onto the window from the desktop's file manager.
 *
 * Reads and clears, exactly like the wheel and the double click: a drop is one
 * event, and the frame that misses it is the frame the user is looking at.
 * `out` receives the absolute paths, one per line, UTF-8, NUL terminated; the
 * pointer position the drop landed on comes back in *x / *y in CLIENT pixels,
 * because the only thing that can decide which panel was dropped on is the
 * caller. Returns how many paths were written.
 *
 * Win32 (WM_DROPFILES) and X11 (XDND) implement it. Wayland does not yet, and
 * says so by returning 0 - a host that gets 0 simply never sees a drop, which
 * is the same contract the clipboard bridge has. */
DAI_API uint32_t dai_window_dropped_files(dai_window *w, char *out, uint32_t max,
                                          int *x, int *y);""",
    'render.h drop api')
wr('include/dai_render.h', s)

# ================================================================= 2. Win32
s = rd('src/rhi_vulkan_window_win32.cpp')
s = sub1(s,
"""#include <windows.h>
#include <vulkan/vulkan_win32.h>""",
"""#include <windows.h>
#include <shellapi.h>          /* DragAcceptFiles / DragQueryFile: WM_DROPFILES */
#include <vulkan/vulkan_win32.h>""",
    'win32 shellapi')

s = sub1(s,
"""    HCURSOR cursor = nullptr;
    int     cursor_id = -1;
    int     dbl_click = 0;
};""",
"""    HCURSOR cursor = nullptr;
    int     cursor_id = -1;
    int     dbl_click = 0;

    // What Explorer dropped, held until the host asks for it. A fixed buffer
    // rather than a vector of strings: a drop is a handful of paths, this file
    // allocates nothing anywhere else, and a queue that cannot grow cannot be
    // a memory bug in a message handler.
    char     dropped[4096] = { 0 };
    uint32_t dropped_len = 0;
    uint32_t dropped_count = 0;
    int      drop_x = 0, drop_y = 0;
};""",
    'win32 drop state')

s = sub1(s,
"""    case WM_MOUSEWHEEL:  w->wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; return 0;""",
"""    case WM_MOUSEWHEEL:  w->wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA; return 0;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        POINT pt{ 0, 0 };
        DragQueryPoint(hd, &pt);            // already client coordinates
        w->drop_x = pt.x; w->drop_y = pt.y;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFFu, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t wpath[MAX_PATH * 2];
            UINT len = DragQueryFileW(hd, i, wpath, (UINT)(sizeof(wpath) / sizeof(wpath[0])));
            if (!len) continue;
            char u8[MAX_PATH * 4];
            int got = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)len, u8,
                                          (int)sizeof(u8) - 1, nullptr, nullptr);
            if (got <= 0) continue;
            u8[got] = 0;
            // Windows hands out backslashes; everything above this line speaks
            // '/'. Normalising here means the editor never has to care which
            // window system a path came from.
            for (int c = 0; c < got; ++c) if (u8[c] == '\\\\') u8[c] = '/';
            uint32_t need = (uint32_t)got + 1;
            if (w->dropped_len + need >= sizeof(w->dropped)) break;
            std::memcpy(w->dropped + w->dropped_len, u8, (size_t)got);
            w->dropped_len += (uint32_t)got;
            w->dropped[w->dropped_len++] = '\\n';
            w->dropped[w->dropped_len] = 0;
            ++w->dropped_count;
        }
        DragFinish(hd);
        return 0;
    }""",
    'win32 WM_DROPFILES')

s = sub1(s,
"""    wc.style |= CS_DBLCLKS;                                 // WM_LBUTTONDBLCLK at all""",
"""    wc.style |= CS_DBLCLKS;                                 // WM_LBUTTONDBLCLK at all""",
    'win32 class marker (unchanged)')

s = sub1(s,
"""int dai_window_double_click(dai_window *w) {""",
"""uint32_t dai_window_dropped_files(dai_window *w, char *out, uint32_t max, int *x, int *y) {
    if (!w || !out || !max) return 0;
    out[0] = 0;
    if (!w->dropped_count) return 0;
    uint32_t n = w->dropped_len;
    if (n >= max) n = max - 1;
    std::memcpy(out, w->dropped, n);
    out[n] = 0;
    if (x) *x = w->drop_x;
    if (y) *y = w->drop_y;
    uint32_t c = w->dropped_count;
    w->dropped_len = 0; w->dropped_count = 0; w->dropped[0] = 0;
    return c;
}

int dai_window_double_click(dai_window *w) {""",
    'win32 dropped_files')
wr('src/rhi_vulkan_window_win32.cpp', s)

# ================================================================== 3. X11
s = rd('src/rhi_vulkan_window.cpp')
s = sub1(s,
"""#include <X11/Xutil.h>""",
"""#include <X11/Xatom.h>       /* XA_ATOM: the XdndAware property is a list of them */
#include <X11/Xutil.h>""",
    'x11 Xatom include')
s = sub1(s,
"""    // Pointer shapes, created on first use. X11 has them built in (the "cursor
    // font"), so this costs nothing until something asks for one.
    Cursor cursors[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int    cursor_now = -1;""",
"""    // Pointer shapes, created on first use. X11 has them built in (the "cursor
    // font"), so this costs nothing until something asks for one.
    Cursor cursors[7] = { 0, 0, 0, 0, 0, 0, 0 };
    int    cursor_now = -1;

    // XDND, receiver side. X11 has no "drop" event: a drop is a conversation
    // of ClientMessages with the dragging program, ending in a selection
    // transfer - which is why this is six atoms and a state machine instead
    // of one case label.
    Atom xdnd_aware = 0, xdnd_enter = 0, xdnd_position = 0, xdnd_status = 0;
    Atom xdnd_drop = 0, xdnd_finished = 0, xdnd_selection = 0, xdnd_action_copy = 0;
    Atom xdnd_type_list = 0, xdnd_uri_list = 0;
    Window xdnd_source = 0;
    Atom   xdnd_type = 0;        // what the source offers that we can read
    int    xdnd_version = 0;
    char     dropped[4096] = { 0 };
    uint32_t dropped_len = 0;
    uint32_t dropped_count = 0;
    int      drop_x = 0, drop_y = 0;""",
    'x11 drop state')

s = sub1(s,
"""    w->wm_delete = XInternAtom(w->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(w->dpy, w->win, &w->wm_delete, 1);""",
"""    w->wm_delete = XInternAtom(w->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(w->dpy, w->win, &w->wm_delete, 1);
    // XDND: announcing version 5 on the window is the whole of "this window
    // accepts drops". Everything else happens in the message pump.
    w->xdnd_aware       = XInternAtom(w->dpy, "XdndAware", False);
    w->xdnd_enter       = XInternAtom(w->dpy, "XdndEnter", False);
    w->xdnd_position    = XInternAtom(w->dpy, "XdndPosition", False);
    w->xdnd_status      = XInternAtom(w->dpy, "XdndStatus", False);
    w->xdnd_drop        = XInternAtom(w->dpy, "XdndDrop", False);
    w->xdnd_finished    = XInternAtom(w->dpy, "XdndFinished", False);
    w->xdnd_selection   = XInternAtom(w->dpy, "XdndSelection", False);
    w->xdnd_action_copy = XInternAtom(w->dpy, "XdndActionCopy", False);
    w->xdnd_type_list   = XInternAtom(w->dpy, "XdndTypeList", False);
    w->xdnd_uri_list    = XInternAtom(w->dpy, "text/uri-list", False);
    {
        long version = 5;
        XChangeProperty(w->dpy, w->win, w->xdnd_aware, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&version, 1);
    }""",
    'x11 xdnd atoms')

s = sub1(s,
"""        switch (e.type) {
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == w->wm_delete) w->open = false;
            break;""",
"""        switch (e.type) {
        case SelectionNotify:
            // The answer to the XConvertSelection sent from XdndDrop: a
            // text/uri-list, which is "file:///path" lines with the awkward
            // characters percent encoded.
            if (e.xselection.property == None) break;
            if (e.xselection.selection != w->xdnd_selection) break;
            {
                Atom type = 0; int fmt = 0;
                unsigned long items = 0, after = 0;
                unsigned char *data = nullptr;
                if (XGetWindowProperty(w->dpy, w->win, w->xdnd_selection, 0, 65536, True,
                                       AnyPropertyType, &type, &fmt, &items, &after,
                                       &data) == Success && data) {
                    xdnd_take_uri_list(w, (const char *)data, (size_t)items);
                    XFree(data);
                }
                if (w->xdnd_source) {
                    XEvent r{};
                    r.xclient.type = ClientMessage;
                    r.xclient.display = w->dpy;
                    r.xclient.window = w->xdnd_source;
                    r.xclient.message_type = w->xdnd_finished;
                    r.xclient.format = 32;
                    r.xclient.data.l[0] = (long)w->win;
                    r.xclient.data.l[1] = 1;
                    r.xclient.data.l[2] = (long)w->xdnd_action_copy;
                    XSendEvent(w->dpy, w->xdnd_source, False, NoEventMask, &r);
                    XFlush(w->dpy);
                }
                w->xdnd_source = 0;
            }
            break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == w->wm_delete) { w->open = false; break; }
            if (e.xclient.message_type == w->xdnd_enter) {
                w->xdnd_source = (Window)e.xclient.data.l[0];
                w->xdnd_version = (int)(((unsigned long)e.xclient.data.l[1]) >> 24);
                w->xdnd_type = 0;
                if (e.xclient.data.l[1] & 1) {
                    // More than three types: they are in a property instead.
                    Atom type = 0; int fmt = 0;
                    unsigned long items = 0, after = 0;
                    unsigned char *data = nullptr;
                    if (XGetWindowProperty(w->dpy, w->xdnd_source, w->xdnd_type_list, 0, 64,
                                           False, XA_ATOM, &type, &fmt, &items, &after,
                                           &data) == Success && data) {
                        Atom *ats = (Atom *)data;
                        for (unsigned long i = 0; i < items; ++i)
                            if (ats[i] == w->xdnd_uri_list) { w->xdnd_type = ats[i]; break; }
                        XFree(data);
                    }
                } else {
                    for (int i = 2; i <= 4; ++i)
                        if ((Atom)e.xclient.data.l[i] == w->xdnd_uri_list)
                            { w->xdnd_type = w->xdnd_uri_list; break; }
                }
                break;
            }
            if (e.xclient.message_type == w->xdnd_position) {
                // Root coordinates on the wire; the editor thinks in client
                // ones, so translate before storing.
                int rx = (int)(e.xclient.data.l[2] >> 16);
                int ry = (int)(e.xclient.data.l[2] & 0xFFFF);
                int cx = 0, cy = 0; Window child = 0;
                XTranslateCoordinates(w->dpy, DefaultRootWindow(w->dpy), w->win,
                                      rx, ry, &cx, &cy, &child);
                w->drop_x = cx; w->drop_y = cy;
                XEvent r{};
                r.xclient.type = ClientMessage;
                r.xclient.display = w->dpy;
                r.xclient.window = (Window)e.xclient.data.l[0];
                r.xclient.message_type = w->xdnd_status;
                r.xclient.format = 32;
                r.xclient.data.l[0] = (long)w->win;
                r.xclient.data.l[1] = w->xdnd_type ? 1 : 0;   // bit 0: we accept
                r.xclient.data.l[2] = 0;                      // no "silent" rect
                r.xclient.data.l[3] = 0;
                r.xclient.data.l[4] = (long)w->xdnd_action_copy;
                XSendEvent(w->dpy, (Window)e.xclient.data.l[0], False, NoEventMask, &r);
                XFlush(w->dpy);
                break;
            }
            if (e.xclient.message_type == w->xdnd_drop) {
                w->xdnd_source = (Window)e.xclient.data.l[0];
                if (!w->xdnd_type) {
                    XEvent r{};
                    r.xclient.type = ClientMessage;
                    r.xclient.display = w->dpy;
                    r.xclient.window = w->xdnd_source;
                    r.xclient.message_type = w->xdnd_finished;
                    r.xclient.format = 32;
                    r.xclient.data.l[0] = (long)w->win;
                    XSendEvent(w->dpy, w->xdnd_source, False, NoEventMask, &r);
                    w->xdnd_source = 0;
                } else {
                    Time t = (Time)e.xclient.data.l[2];
                    XConvertSelection(w->dpy, w->xdnd_selection, w->xdnd_type,
                                      w->xdnd_selection, w->win, t);
                }
                break;
            }
            break;""",
    'x11 xdnd events')

# The uri-list parser, next to the key helpers at the top of the file.
s = sub1(s,
"""uint32_t key_slot(uint32_t keysym) { return (keysym ^ (keysym >> 8)) & 0xFF; }""",
"""uint32_t key_slot(uint32_t keysym) { return (keysym ^ (keysym >> 8)) & 0xFF; }

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "file:///home/me/a%20file.png\\r\\n..." -> one absolute path per line. Only
// file:// URIs: a drag from a browser hands over an http one, and downloading
// it is not something a window backend should decide to do.
void xdnd_take_uri_list(dai_window *w, const char *text, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\\n' && text[e] != '\\r') ++e;
        const char *line = text + i;
        size_t len = e - i;
        i = e;
        while (i < n && (text[i] == '\\n' || text[i] == '\\r')) ++i;
        if (!len || line[0] == '#') continue;
        const char *pfx = "file://";
        size_t pl = 7;
        if (len <= pl || std::strncmp(line, pfx, pl) != 0) continue;
        const char *p = line + pl;
        size_t rem = len - pl;
        // file://host/path is legal; we only ever see an empty host in
        // practice, and a remote one is not a path we could open anyway.
        while (rem && *p != '/') { ++p; --rem; }
        char out[1024];
        size_t o = 0;
        for (size_t k = 0; k < rem && o + 1 < sizeof(out); ++k) {
            if (p[k] == '%' && k + 2 < rem) {
                int hi = hexv(p[k + 1]), lo = hexv(p[k + 2]);
                if (hi >= 0 && lo >= 0) { out[o++] = (char)(hi * 16 + lo); k += 2; continue; }
            }
            out[o++] = p[k];
        }
        out[o] = 0;
        if (!o) continue;
        if (w->dropped_len + o + 2 >= sizeof(w->dropped)) break;
        std::memcpy(w->dropped + w->dropped_len, out, o);
        w->dropped_len += (uint32_t)o;
        w->dropped[w->dropped_len++] = '\\n';
        w->dropped[w->dropped_len] = 0;
        ++w->dropped_count;
    }
}""",
    'x11 uri parser')

s = sub1(s,
"""int dai_window_double_click(dai_window *w) {""",
"""uint32_t dai_window_dropped_files(dai_window *w, char *out, uint32_t max, int *x, int *y) {
    if (!w || !out || !max) return 0;
    out[0] = 0;
    if (!w->dropped_count) return 0;
    uint32_t n = w->dropped_len;
    if (n >= max) n = max - 1;
    std::memcpy(out, w->dropped, n);
    out[n] = 0;
    if (x) *x = w->drop_x;
    if (y) *y = w->drop_y;
    uint32_t c = w->dropped_count;
    w->dropped_len = 0; w->dropped_count = 0; w->dropped[0] = 0;
    return c;
}

int dai_window_double_click(dai_window *w) {""",
    'x11 dropped_files')
wr('src/rhi_vulkan_window.cpp', s)

# ============================================================== 4. Wayland
s = rd('src/rhi_vulkan_window_wayland.cpp')
s = sub1(s,
"""int dai_window_double_click(dai_window *w) { (void)w; return 0; }""",
"""int dai_window_double_click(dai_window *w) { (void)w; return 0; }

// Wayland's drag and drop rides on wl_data_device, which needs a data device
// manager, an offer listener and a pipe read per drop. Not wired yet; saying
// so with a 0 is the same contract the clipboard bridge has here, and a host
// that gets 0 simply never sees a drop.
uint32_t dai_window_dropped_files(dai_window *w, char *out, uint32_t max, int *x, int *y) {
    (void)w; (void)max; (void)x; (void)y;
    if (out && max) out[0] = 0;
    return 0;
}""",
    'wayland stub')
wr('src/rhi_vulkan_window_wayland.cpp', s)
print('patch41 part A ok (window layer)')
