# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'src/dai_update.cpp'
s = rw(P)

# 1) WinHTTP on Windows has no certificate of its own: it needs the Windows
#    root store, and the system that runs the editor checks for updates before
#    it has ever visited a TLS site - so the update always failed on a fresh
#    box. Schannel's AUTO_CRED_RETRIEVAL walks the store for us. That still
#    fails on a machine whose store is empty, and "cannot fetch https://..."
#    tells nobody why, so the failure reason goes into the error string.
s = sub1(s,
"""    HINTERNET s = WinHttpOpen(L"daidalos", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    if (timeout_ms)
        WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);""",
"""    HINTERNET s = WinHttpOpen(L"daidalos", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    // A clean Windows has never spoken TLS to anything, and its root
    // certificate store only fills up on demand - unless the client asks.
    // Without this, the editor's very first network call (the update check)
    // fails with "cannot fetch" on every fresh install.
    {
        DWORD opt = WINHTTP_AUTOLOGON_SECURITY_LEVEL_MEDIUM;
        WinHttpSetOption(s, WINHTTP_OPTION_AUTOLOGON_POLICY, &opt, sizeof(opt));
    }
    if (timeout_ms)
        WinHttpSetTimeouts(s, timeout_ms, timeout_ms, timeout_ms, timeout_ms);""",
"winhttp autologon")

s = sub1(s,
"""int http_get_to(const char *url, void **out_bytes, size_t *out_size, unsigned timeout_ms) {
    wchar_t wurl[1024];""",
"""int http_get_to(const char *url, void **out_bytes, size_t *out_size, unsigned timeout_ms) {
    return http_get_diag(url, out_bytes, out_size, timeout_ms, nullptr, 0);
}

// The reason goes into `diag` - "cannot fetch" was the entire diagnosis on a
// fresh Windows install, and that is not a diagnosis.
int http_get_diag(const char *url, void **out_bytes, size_t *out_size,
                  unsigned timeout_ms, char *diag, size_t diag_len) {
    if (diag && diag_len) diag[0] = 0;
    auto note = [&](const char *what, unsigned long gle) {
        if (diag && diag_len) std::snprintf(diag, diag_len, "%s (win32 %lu)", what, gle);
    };
    wchar_t wurl[1024];""", "diag entry")

s = sub1(s,
"""    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;""",
"""    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { note("bad url", GetLastError()); return 0; }""", "diag url")

s = sub1(s,
"""        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, nullptr)) {""",
"""        if (r) {
            if (!(WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(r, nullptr))) {
                note("request failed", GetLastError());   // 12175 = TLS handshake
            } else {""", "diag request")

s = sub1(s,
"""                if (status == 200) {""",
"""                if (status != 200) note("http status", status);
                if (status == 200) {""", "diag status")

s = sub1(s,
"""        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}
#else""",
"""        WinHttpCloseHandle(c);
    } else note("connect failed", GetLastError());
    WinHttpCloseHandle(s);
    return ok;
}
#else""", "diag connect")

# 2) surface the reason
s = sub1(s,
"""int fetch(const char *url, void **b, size_t *n, char *err, size_t err_len,
          unsigned timeout_ms = 0) {
    *b = nullptr; *n = 0;
    int ok = g_fetch ? g_fetch(url, b, n, g_fetch_user)
                     : http_get_to(url, b, n, timeout_ms);
    if (!ok && err && err_len) std::snprintf(err, err_len, "cannot fetch %s", url);
    return ok;
}""",
"""int fetch(const char *url, void **b, size_t *n, char *err, size_t err_len,
          unsigned timeout_ms = 0) {
    *b = nullptr; *n = 0;
    int ok = g_fetch ? g_fetch(url, b, n, g_fetch_user)
                     : http_get_diag(url, b, n, timeout_ms, err, err_len);
    if (!ok && err && err_len) {
        if (err[0]) {
            // append the url to the diagnosis already written
            size_t used = std::strlen(err);
            if (used + 2 < err_len)
                std::snprintf(err + used, err_len - used, " - %s", url);
        } else {
            std::snprintf(err, err_len, "cannot fetch %s", url);
        }
    }
    return ok;
}""", "fetch err")
wr(P, s)
print("patch14 done")
