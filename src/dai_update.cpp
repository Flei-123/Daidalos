// Self updating. See include/dai_update.h.
//
// Three things that all have to be right at once, or the feature does more harm
// than good:
//
//   Verification before installation. A download is checked against the hash in
//   the manifest and only then allowed near the install directory. Replacing a
//   working editor with a truncated one is worse than being out of date.
//
//   Staging, then committing. Files arrive as "<name>.new" and nothing existing
//   moves until every one of them is present and correct. Pulling the plug
//   halfway leaves the old build running.
//
//   The rename trick. Windows will not let a running .exe be overwritten, but
//   it will let it be renamed. So the running file steps aside to "<name>.old",
//   the new one takes its place, and the leftover is swept on the next start.
//
// The transport is deliberately replaceable. Tests drive the entire flow with
// bytes from memory - no server, no network, no flakiness.

#include "dai_update.h"
#include "dai_json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <winhttp.h>
#else
#  include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// SHA-256. FIPS 180-4, written out rather than pulled in: the engine has no
// crypto dependency and this is the only hash it needs.
// ---------------------------------------------------------------------------

namespace {

struct Sha256 {
    uint32_t h[8];
    uint64_t len = 0;
    uint8_t  buf[64];
    size_t   have = 0;

    Sha256() {
        static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        std::memcpy(h, iv, sizeof(h));
    }

    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t *p) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                   ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    void update(const void *data, size_t size) {
        const uint8_t *p = (const uint8_t *)data;
        len += size;
        while (size) {
            size_t n = 64 - have;
            if (n > size) n = size;
            std::memcpy(buf + have, p, n);
            have += n; p += n; size -= n;
            if (have == 64) { block(buf); have = 0; }
        }
    }

    void final_hex(char *out) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (have != 56) update(&z, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = (uint8_t)(bits >> (56 - i * 8));
        // Bypass update() here: it would count these into len, which is already
        // baked into `bits`.
        std::memcpy(buf + have, tail, 8);
        block(buf);
        static const char *hex = "0123456789abcdef";
        for (int i = 0; i < 8; ++i)
            for (int b = 0; b < 4; ++b) {
                uint8_t v = (uint8_t)(h[i] >> (24 - b * 8));
                *out++ = hex[v >> 4];
                *out++ = hex[v & 15];
            }
        *out = 0;
    }
};

std::string join_path(const char *dir, const char *name) {
    std::string s = dir && dir[0] ? dir : ".";
    if (s.back() != '/' && s.back() != '\\') s += '/';
    return s + name;
}

// A name from a manifest becomes a path on this machine, so it does not get to
// contain a directory separator or "..". A compromised or careless manifest
// must not be able to write outside the install directory.
bool name_is_safe(const char *n) {
    if (!n || !n[0]) return false;
    if (std::strstr(n, "..")) return false;
    for (const char *p = n; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':') return false;
    return true;
}

dai_update_fetch_fn g_fetch = nullptr;
void *g_fetch_user = nullptr;

// ---- the built in transport ----------------------------------------------

#ifdef _WIN32
// WinHTTP ships with Windows, so a shipped editor needs no extra DLL.
int http_get(const char *url, void **out_bytes, size_t *out_size, void *) {
    wchar_t wurl[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)) return 0;

    URL_COMPONENTS uc{};
    wchar_t host[256], path[1024];
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;

    HINTERNET s = WinHttpOpen(L"daidalos", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    int ok = 0;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, nullptr)) {
                DWORD status = 0, slen = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    std::vector<uint8_t> body;
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                        size_t at = body.size();
                        body.resize(at + avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(r, body.data() + at, avail, &got)) break;
                        body.resize(at + got);
                    }
                    void *mem = std::malloc(body.size() ? body.size() : 1);
                    if (mem) {
                        std::memcpy(mem, body.data(), body.size());
                        *out_bytes = mem;
                        *out_size = body.size();
                        ok = 1;
                    }
                }
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}
#else
// On POSIX the shipped path is Windows, and dragging a TLS stack into the
// engine to update a Linux build nobody ships would be the wrong trade. curl is
// on every machine that would be running this, and being honest about the
// dependency beats implementing TLS badly.
int http_get(const char *url, void **out_bytes, size_t *out_size, void *) {
    for (const char *p = url; *p; ++p)
        if (*p == '\'' || *p == '\n' || *p == '`') return 0;      // no shell games
    std::string cmd = "curl -fsSL --max-time 60 '";
    cmd += url;
    cmd += "'";
    FILE *f = popen(cmd.c_str(), "r");
    if (!f) return 0;
    std::vector<uint8_t> body;
    char chunk[16384];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) body.insert(body.end(), chunk, chunk + n);
    if (pclose(f) != 0) return 0;
    void *mem = std::malloc(body.size() ? body.size() : 1);
    if (!mem) return 0;
    std::memcpy(mem, body.data(), body.size());
    *out_bytes = mem;
    *out_size = body.size();
    return 1;
}
#endif

int fetch(const char *url, void **b, size_t *n, char *err, size_t err_len) {
    *b = nullptr; *n = 0;
    int ok = g_fetch ? g_fetch(url, b, n, g_fetch_user) : http_get(url, b, n, nullptr);
    if (!ok && err && err_len) std::snprintf(err, err_len, "cannot fetch %s", url);
    return ok;
}

} // namespace

extern "C" {

// ---------------------------------------------------------------------------

void dai_sha256_hex(const void *data, size_t size, char *out) {
    if (!out) return;
    Sha256 s;
    if (data && size) s.update(data, size);
    s.final_hex(out);
}

dai_result dai_sha256_file(const char *path, char *out) {
    if (!path || !out) return DAI_ERR_INVALID_ARG;
    FILE *f = std::fopen(path, "rb");
    if (!f) return DAI_ERR_NOT_FOUND;
    Sha256 s;
    char chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) s.update(chunk, n);
    std::fclose(f);
    s.final_hex(out);
    return DAI_OK;
}

void dai_update_set_fetch(dai_update_fetch_fn fn, void *user) {
    g_fetch = fn;
    g_fetch_user = user;
}

int dai_version_compare(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a || *b) {
        long na = 0, nb = 0;
        while (*a >= '0' && *a <= '9') na = na * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9') nb = nb * 10 + (*b++ - '0');
        if (na != nb) return na < nb ? -1 : 1;
        // Skip one separator on each side; anything non numeric is a separator,
        // which makes "1.2-beta" and "1.2.0" compare as the same release.
        if (*a) ++a;
        if (*b) ++b;
        if (!*a && !*b) break;
    }
    return 0;
}

dai_result dai_update_check(const char *manifest_url, const char *current_version,
                            const char *install_dir, dai_update_info *out,
                            char *err, size_t err_len) {
    if (!manifest_url || !out) return DAI_ERR_INVALID_ARG;
    *out = dai_update_info{};

    void *bytes = nullptr;
    size_t size = 0;
    if (!fetch(manifest_url, &bytes, &size, err, err_len)) return DAI_ERR_NOT_FOUND;

    daijson::Document doc;
    std::string jerr;
    bool parsed = doc.parse((const char *)bytes, size, &jerr);
    std::free(bytes);
    if (!parsed) {
        if (err && err_len) std::snprintf(err, err_len, "manifest is not JSON: %s", jerr.c_str());
        return DAI_ERR_INVALID_ARG;
    }
    const daijson::Value *root = doc.root();
    if (!root || root->type != daijson::Value::OBJECT) {
        if (err && err_len) std::snprintf(err, err_len, "manifest root is not an object");
        return DAI_ERR_INVALID_ARG;
    }

    std::snprintf(out->version, sizeof(out->version), "%s", root->str_at("version", ""));
    std::snprintf(out->base_url, sizeof(out->base_url), "%s", root->str_at("base_url", ""));
    if (!out->version[0]) {
        if (err && err_len) std::snprintf(err, err_len, "manifest has no version");
        return DAI_ERR_INVALID_ARG;
    }
    out->available = dai_version_compare(out->version, current_version ? current_version : "") > 0;

    const daijson::Value *files = root->get("files");
    if (files && files->type == daijson::Value::OBJECT) {
        for (const daijson::Member &m : files->members) {
            if (out->file_count >= DAI_UPDATE_MAX_FILES) break;
            const char *key = m.key.c_str();
            const daijson::Value *v = m.value;
            if (!v || !name_is_safe(key)) continue;

            dai_update_file &f = out->files[out->file_count];
            f = dai_update_file{};
            std::snprintf(f.name, sizeof(f.name), "%s", key);
            if (v->type == daijson::Value::STRING) {
                std::snprintf(f.sha256, sizeof(f.sha256), "%s", v->string());
            } else if (v->type == daijson::Value::OBJECT) {
                std::snprintf(f.sha256, sizeof(f.sha256), "%s", v->str_at("sha256", ""));
                f.size = (uint64_t)v->num_at("size", 0);
            } else {
                continue;
            }
            if (std::strlen(f.sha256) != 64) continue;   // no hash, no install

            // The version number says a release happened; the hash says whether
            // this particular file actually changed. Only the hash gets to
            // decide what is downloaded.
            char local[65];
            std::string path = join_path(install_dir, f.name);
            f.needed = dai_sha256_file(path.c_str(), local) != DAI_OK ||
                       std::strcmp(local, f.sha256) != 0;
            if (f.needed) ++out->needed_count;
            ++out->file_count;
        }
    }
    return DAI_OK;
}

uint32_t dai_update_download(const dai_update_info *info, const char *install_dir,
                             char *err, size_t err_len) {
    if (!info || !info->base_url[0]) {
        if (err && err_len) std::snprintf(err, err_len, "no base_url to download from");
        return 0;
    }
    uint32_t staged = 0;
    for (uint32_t i = 0; i < info->file_count; ++i) {
        const dai_update_file &f = info->files[i];
        if (!f.needed) continue;

        std::string url = info->base_url;
        if (url.back() != '/') url += '/';
        url += f.name;

        void *bytes = nullptr;
        size_t size = 0;
        if (!fetch(url.c_str(), &bytes, &size, err, err_len)) return 0;

        // Verified before it is allowed to exist as a file. A corrupt download
        // is discarded here, not discovered after it replaced the editor.
        char got[65];
        dai_sha256_hex(bytes, size, got);
        if (std::strcmp(got, f.sha256) != 0) {
            std::free(bytes);
            if (err && err_len)
                std::snprintf(err, err_len, "%s: hash mismatch, refusing it", f.name);
            return 0;
        }
        if (f.size && f.size != size) {
            std::free(bytes);
            if (err && err_len) std::snprintf(err, err_len, "%s: size mismatch", f.name);
            return 0;
        }

        std::string dst = join_path(install_dir, f.name) + ".new";
        FILE *o = std::fopen(dst.c_str(), "wb");
        if (!o) {
            std::free(bytes);
            if (err && err_len) std::snprintf(err, err_len, "cannot write %s", dst.c_str());
            return 0;
        }
        bool ok = std::fwrite(bytes, 1, size, o) == size;
        ok = (std::fflush(o) == 0) && ok;
        std::fclose(o);
        std::free(bytes);
        if (!ok) {
            std::remove(dst.c_str());
            if (err && err_len) std::snprintf(err, err_len, "writing %s failed", dst.c_str());
            return 0;
        }
        ++staged;
    }
    return staged;
}

dai_result dai_update_commit(const dai_update_info *info, const char *install_dir,
                             const char *running_exe, char *err, size_t err_len) {
    if (!info) return DAI_ERR_INVALID_ARG;

    // Everything staged must be present before anything is moved, so a partial
    // download cannot produce a half updated install.
    for (uint32_t i = 0; i < info->file_count; ++i) {
        if (!info->files[i].needed) continue;
        std::string staged = join_path(install_dir, info->files[i].name) + ".new";
        FILE *f = std::fopen(staged.c_str(), "rb");
        if (!f) {
            if (err && err_len)
                std::snprintf(err, err_len, "%s was never staged", info->files[i].name);
            return DAI_ERR_NOT_FOUND;
        }
        std::fclose(f);
    }

    for (uint32_t i = 0; i < info->file_count; ++i) {
        const dai_update_file &f = info->files[i];
        if (!f.needed) continue;
        std::string live = join_path(install_dir, f.name);
        std::string staged = live + ".new";

        // The running executable cannot be overwritten on Windows, but it can
        // be renamed out of the way - which is the whole trick.
        if (running_exe && !std::strcmp(running_exe, f.name)) {
            std::string old = live + ".old";
            std::remove(old.c_str());
            if (std::rename(live.c_str(), old.c_str()) != 0) {
                if (err && err_len)
                    std::snprintf(err, err_len, "cannot move the running %s aside", f.name);
                return DAI_ERR_FILE;
            }
        } else {
            std::remove(live.c_str());
        }
        if (std::rename(staged.c_str(), live.c_str()) != 0) {
            if (err && err_len) std::snprintf(err, err_len, "cannot install %s", f.name);
            return DAI_ERR_FILE;
        }
    }
    return DAI_OK;
}

void dai_update_sweep(const char *install_dir) {
    // Only the names the manifest could have produced are swept, so this never
    // deletes something a user happened to call ".old".
#ifdef _WIN32
    std::string pattern = join_path(install_dir, "*.old");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string p = join_path(install_dir, fd.cFileName);
        DeleteFileA(p.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)install_dir;   // the shipped path is Windows; nothing to sweep here
#endif
}

} // extern "C"
