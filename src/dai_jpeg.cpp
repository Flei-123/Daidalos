// Baseline JPEG, decoded from scratch.
//
// The engine reads its own PNGs (src/dai_inflate.cpp, inflate included) for
// the same reason it reads its own glTF: no libjpeg, no stb, nothing to ship
// alongside the exe. This is the other half of that promise - a photograph
// off a camera or a texture out of a marketplace is a .jpg far more often
// than it is a .png, and "only PNG is decoded" was the editor's most boring
// dead end.
//
// What is here: baseline sequential DCT (SOF0) and extended sequential
// (SOF1), Huffman coded, one to four components, any sampling factors up to
// 4x4 - so 4:4:4, 4:2:2, 4:4:0 and 4:2:0 all work - restart markers, and
// both interleaved and single component scans. Greyscale comes out grey,
// three components come out YCbCr unless an Adobe APP14 marker says the file
// is really RGB.
//
// What is NOT here, and says so instead of guessing: progressive JPEG (SOF2),
// arithmetic coding, lossless and hierarchical modes, 12 bit samples, and
// CMYK/YCCK separations. Those are different formats wearing the same suffix.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#ifdef _WIN32
#  include <windows.h>
#endif

namespace daiimg {

namespace {

const uint8_t ZIGZAG[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// One Huffman table in the form the standard's DECODE procedure wants: for
// every code length, the smallest code of that length, the largest, and where
// its values start. Decoding is then "shift a bit in until the code fits the
// range for this length", which needs no tree and no allocation.
struct Huff {
    uint8_t vals[256] = { 0 };
    int mincode[17] = { 0 };
    int maxcode[18] = { 0 };
    int valptr[17] = { 0 };
    bool ok = false;

    void build(const uint8_t counts[16], const uint8_t *values, int nvals) {
        std::memcpy(vals, values, (size_t)(nvals > 256 ? 256 : nvals));
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            valptr[l] = k;
            mincode[l] = code;
            code += counts[l - 1];
            k += counts[l - 1];
            maxcode[l] = counts[l - 1] ? code - 1 : -1;   // -1 never matches
            code <<= 1;
        }
        maxcode[17] = 0x7FFFFFFF;
        ok = true;
    }
};

struct Comp {
    int id = 0, h = 1, v = 1, tq = 0;
    int td = 0, ta = 0;               // huffman tables chosen by the scan
    int pred = 0;                     // running DC predictor
    uint32_t bw = 0, bh = 0;          // size in blocks, padded to whole MCUs
    std::vector<uint8_t> plane;       // bw*8 by bh*8 samples
};

// The entropy coded bit stream. Two things make it not a plain bit reader:
// a 0xFF byte inside the data is written as FF 00, and a real marker ends the
// stream - past which the decoder must see zeroes rather than read the next
// segment's header as picture data.
struct Bits {
    const uint8_t *p = nullptr;
    const uint8_t *end = nullptr;
    uint32_t buf = 0;
    int cnt = 0;
    bool hit = false;                 // a marker was reached

    uint8_t nextbyte() {
        if (hit || p >= end) { hit = true; return 0; }
        uint8_t b = *p++;
        if (b == 0xFF) {
            uint8_t b2 = (p < end) ? *p : 0xD9;
            if (b2 == 0x00) { ++p; return 0xFF; }   // stuffed: a literal FF
            --p;                                    // leave the marker in place
            hit = true;
            return 0;
        }
        return b;
    }
    int bit() {
        if (cnt == 0) { buf = nextbyte(); cnt = 8; }
        --cnt;
        return (int)((buf >> cnt) & 1u);
    }
    int receive(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    void align() { cnt = 0; }
};

// A Huffman coded value is stored as "how many bits follow", and those bits
// are the number itself with the top of the negative range folded away.
int extend(int v, int n) {
    return (n && v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

int decode_huff(Bits &b, const Huff &h) {
    int code = b.bit();
    int l = 1;
    while (code > h.maxcode[l]) {
        code = (code << 1) | b.bit();
        if (++l > 16) return 0;       // corrupt: give up on this value, not the file
    }
    int idx = h.valptr[l] + code - h.mincode[l];
    return (idx >= 0 && idx < 256) ? h.vals[idx] : 0;
}

// Separable inverse DCT. Not the fastest arrangement in the world, but the
// one that is obviously the formula in the standard - and a texture is
// decoded once, not once a frame.
struct IdctTable {
    float t[8][8];
    IdctTable() {
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                t[u][x] = (u == 0 ? 0.3535533906f : 0.5f) *
                          std::cos((float)((2 * x + 1) * u) * 3.14159265358979f / 16.0f);
    }
};
const IdctTable IDCT;

void idct8x8(const int *in, uint8_t *out, size_t stride) {
    float tmp[64];
    for (int y = 0; y < 8; ++y) {                 // rows
        const int *r = in + y * 8;
        // An all zero row is the common case in flat areas; skipping it is
        // most of the speed this decoder has.
        if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {
            float v = IDCT.t[0][0] * (float)r[0];
            for (int x = 0; x < 8; ++x) tmp[y * 8 + x] = v;
            continue;
        }
        for (int x = 0; x < 8; ++x) {
            float s = 0.0f;
            for (int u = 0; u < 8; ++u) s += IDCT.t[u][x] * (float)r[u];
            tmp[y * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; ++x) {                 // columns
        for (int y = 0; y < 8; ++y) {
            float s = 0.0f;
            for (int u = 0; u < 8; ++u) s += IDCT.t[u][y] * tmp[u * 8 + x];
            int v = (int)std::lround(s) + 128;
            out[(size_t)y * stride + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

} // namespace

// Decodes a baseline JPEG into tightly packed RGBA8. Returns false and says
// why in `err` for anything it does not support.
bool read_jpeg(const uint8_t *file, size_t size, std::vector<uint8_t> &rgba,
               uint32_t *out_w, uint32_t *out_h, char *err, size_t err_len) {
    auto fail = [&](const char *m) {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return false;
    };
    if (size < 4 || file[0] != 0xFF || file[1] != 0xD8) return fail("not a JPEG file");

    uint16_t qt[4][64] = { { 0 } };
    Huff hdc[4], hac[4];
    Comp comp[4];
    int ncomp = 0;
    uint32_t w = 0, h = 0;
    int restart_interval = 0;
    int adobe_transform = -1;         // -1 = no APP14 marker seen
    bool progressive = false;

    size_t pos = 2;
    bool decoded = false;
    int hmax = 1, vmax = 1;
    uint32_t mcux = 0, mcuy = 0;

    while (pos + 1 < size) {
        if (file[pos] != 0xFF) { ++pos; continue; }     // resync over padding
        uint8_t m = file[pos + 1];
        pos += 2;
        if (m == 0xFF) { --pos; continue; }             // fill byte
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;                           // EOI
        if (pos + 2 > size) break;
        uint32_t len = ((uint32_t)file[pos] << 8) | file[pos + 1];
        if (len < 2 || pos + len > size) return fail("a JPEG segment runs past the end of the file");
        const uint8_t *seg = file + pos + 2;
        uint32_t seglen = len - 2;

        if (m == 0xDB) {                                // DQT
            uint32_t o = 0;
            while (o < seglen) {
                int pq = seg[o] >> 4, tq = seg[o] & 15;
                ++o;
                if (tq > 3) return fail("quantisation table id out of range");
                if (o + (pq ? 128u : 64u) > seglen) return fail("truncated quantisation table");
                for (int i = 0; i < 64; ++i) {
                    qt[tq][i] = pq ? (uint16_t)(((uint16_t)seg[o] << 8) | seg[o + 1])
                                   : (uint16_t)seg[o];
                    o += pq ? 2 : 1;
                }
            }
        } else if (m == 0xC4) {                         // DHT
            uint32_t o = 0;
            while (o + 17 <= seglen) {
                int tc = seg[o] >> 4, th = seg[o] & 15;
                ++o;
                if (th > 3 || tc > 1) return fail("huffman table id out of range");
                int n = 0;
                for (int i = 0; i < 16; ++i) n += seg[o + i];
                if (o + 16 + (uint32_t)n > seglen) return fail("truncated huffman table");
                (tc ? hac[th] : hdc[th]).build(seg + o, seg + o + 16, n);
                o += 16 + (uint32_t)n;
            }
        } else if (m == 0xDD) {                         // DRI
            if (seglen >= 2) restart_interval = (seg[0] << 8) | seg[1];
        } else if (m == 0xEE) {                         // APP14, Adobe
            if (seglen >= 12 && !std::memcmp(seg, "Adobe", 5)) adobe_transform = seg[11];
        } else if (m == 0xC2) {
            progressive = true;
            return fail("this is a progressive JPEG - only baseline is decoded, re-save it as baseline or PNG");
        } else if (m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            return fail("this JPEG uses a mode that is not baseline (lossless, hierarchical or arithmetic)");
        } else if (m == 0xC0 || m == 0xC1) {            // SOF0 / SOF1
            if (seglen < 6) return fail("truncated frame header");
            if (seg[0] != 8) return fail("only 8 bit JPEG samples are decoded");
            h = ((uint32_t)seg[1] << 8) | seg[2];
            w = ((uint32_t)seg[3] << 8) | seg[4];
            ncomp = seg[5];
            if (!w || !h) return fail("the JPEG says it is zero pixels wide or tall");
            if (ncomp < 1 || ncomp > 4) return fail("unsupported JPEG component count");
            if (ncomp == 4) return fail("this is a CMYK JPEG - convert it to RGB or PNG");
            if (seglen < 6 + (uint32_t)ncomp * 3) return fail("truncated frame header");
            for (int i = 0; i < ncomp; ++i) {
                const uint8_t *c = seg + 6 + i * 3;
                comp[i].id = c[0];
                comp[i].h = c[1] >> 4;
                comp[i].v = c[1] & 15;
                comp[i].tq = c[2];
                if (comp[i].h < 1 || comp[i].h > 4 || comp[i].v < 1 || comp[i].v > 4)
                    return fail("unsupported JPEG sampling factors");
                if (comp[i].tq > 3) return fail("quantisation table id out of range");
                if (comp[i].h > hmax) hmax = comp[i].h;
                if (comp[i].v > vmax) vmax = comp[i].v;
            }
            mcux = (w + (uint32_t)(hmax * 8) - 1) / (uint32_t)(hmax * 8);
            mcuy = (h + (uint32_t)(vmax * 8) - 1) / (uint32_t)(vmax * 8);
            for (int i = 0; i < ncomp; ++i) {
                comp[i].bw = mcux * (uint32_t)comp[i].h;
                comp[i].bh = mcuy * (uint32_t)comp[i].v;
                // 8 bits per sample, padded out to whole blocks. A 17 pixel
                // wide image is decoded 24 wide and cropped at the end.
                comp[i].plane.assign((size_t)comp[i].bw * 8 * comp[i].bh * 8, 128);
            }
        } else if (m == 0xDA) {                         // SOS - the picture itself
            if (!w || !h) return fail("the JPEG has no frame header before its scan");
            if (seglen < 1) return fail("truncated scan header");
            int ns = seg[0];
            if (ns < 1 || ns > ncomp) return fail("the scan names components the frame does not have");
            if (seglen < 1 + (uint32_t)ns * 2 + 3) return fail("truncated scan header");
            int order[4] = { 0, 0, 0, 0 };
            for (int s = 0; s < ns; ++s) {
                int cid = seg[1 + s * 2], tt = seg[2 + s * 2];
                int found = -1;
                for (int i = 0; i < ncomp; ++i) if (comp[i].id == cid) found = i;
                if (found < 0) return fail("the scan names a component the frame does not have");
                comp[found].td = tt >> 4;
                comp[found].ta = tt & 15;
                if (comp[found].td > 3 || comp[found].ta > 3)
                    return fail("huffman table id out of range");
                order[s] = found;
            }

            Bits bits;
            bits.p = file + pos + len;
            bits.end = file + size;
            for (int i = 0; i < ncomp; ++i) comp[i].pred = 0;

            int blk[64];
            auto one_block = [&](Comp &c, uint32_t bx, uint32_t by) -> bool {
                if (!hdc[c.td].ok || !hac[c.ta].ok)
                    return fail("the scan uses a huffman table the file never defined");
                std::memset(blk, 0, sizeof(blk));
                const uint16_t *q = qt[c.tq];
                int t = decode_huff(bits, hdc[c.td]);
                if (t > 16) t = 16;
                int diff = t ? extend(bits.receive(t), t) : 0;
                c.pred += diff;
                blk[0] = c.pred * (int)q[0];
                for (int k = 1; k < 64;) {
                    int rs = decode_huff(bits, hac[c.ta]);
                    int s = rs & 15, r = rs >> 4;
                    if (s == 0) {
                        if (r != 15) break;              // end of block
                        k += 16;                         // sixteen zeroes
                    } else {
                        k += r;
                        if (k > 63) break;
                        blk[ZIGZAG[k]] = extend(bits.receive(s), s) * (int)q[k];
                        ++k;
                    }
                }
                size_t stride = (size_t)c.bw * 8;
                idct8x8(blk, &c.plane[(size_t)by * 8 * stride + (size_t)bx * 8], stride);
                return true;
            };

            // A restart marker is a hard resynchronisation point: byte align,
            // swallow the RSTn, and forget the DC predictors. Without it one
            // corrupt bit ruins the rest of the picture instead of one strip.
            int since_restart = 0;
            auto maybe_restart = [&]() {
                if (!restart_interval || ++since_restart < restart_interval) return;
                since_restart = 0;
                bits.align();
                const uint8_t *q = bits.p;
                while (q + 1 < bits.end && !(q[0] == 0xFF && q[1] >= 0xD0 && q[1] <= 0xD7)) ++q;
                if (q + 1 < bits.end) { bits.p = q + 2; bits.hit = false; }
                for (int i = 0; i < ncomp; ++i) comp[i].pred = 0;
            };

            bool bad = false;
            if (ns == 1) {
                // A scan of one component walks that component's own blocks,
                // and only the ones that carry picture - the MCU padding is
                // not coded here the way it is in an interleaved scan.
                Comp &c = comp[order[0]];
                uint32_t cw = (w * (uint32_t)c.h + (uint32_t)hmax - 1) / (uint32_t)hmax;
                uint32_t ch = (h * (uint32_t)c.v + (uint32_t)vmax - 1) / (uint32_t)vmax;
                uint32_t nbx = (cw + 7) / 8, nby = (ch + 7) / 8;
                for (uint32_t by = 0; by < nby && !bad; ++by)
                    for (uint32_t bx = 0; bx < nbx && !bad; ++bx) {
                        if (!one_block(c, bx, by)) { bad = true; break; }
                        maybe_restart();
                    }
            } else {
                for (uint32_t my = 0; my < mcuy && !bad; ++my)
                    for (uint32_t mx = 0; mx < mcux && !bad; ++mx) {
                        for (int s = 0; s < ns && !bad; ++s) {
                            Comp &c = comp[order[s]];
                            for (int v = 0; v < c.v && !bad; ++v)
                                for (int hh = 0; hh < c.h && !bad; ++hh)
                                    if (!one_block(c, mx * (uint32_t)c.h + (uint32_t)hh,
                                                      my * (uint32_t)c.v + (uint32_t)v)) bad = true;
                        }
                        maybe_restart();
                    }
            }
            if (bad) return false;                       // one_block already said why
            decoded = true;

            // Past the scan: step to the next marker that is not a restart.
            const uint8_t *q = bits.p;
            while (q + 1 < file + size) {
                if (q[0] == 0xFF && q[1] != 0x00 && !(q[1] >= 0xD0 && q[1] <= 0xD7)) break;
                ++q;
            }
            pos = (size_t)(q - file);
            if (pos + 1 >= size) break;
            continue;                                    // pos is AT the marker
        }
        pos += len;
    }

    if (progressive) return fail("this is a progressive JPEG - only baseline is decoded");
    if (!decoded) return fail("the JPEG has no baseline scan in it");

    // ---- planes to pixels --------------------------------------------------
    // Chroma is upsampled by repeating a sample, which is what the sampling
    // factors mean: nothing smarter can invent detail that was thrown away,
    // and a smoother filter would blur edges the luma plane still has sharp.
    rgba.assign((size_t)w * h * 4, 255);
    bool rgb_direct = (ncomp == 3 && adobe_transform == 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t s[3] = { 128, 128, 128 };
            for (int i = 0; i < ncomp && i < 3; ++i) {
                const Comp &c = comp[i];
                size_t stride = (size_t)c.bw * 8;
                uint32_t sx = x * (uint32_t)c.h / (uint32_t)hmax;
                uint32_t sy = y * (uint32_t)c.v / (uint32_t)vmax;
                if (sx >= stride) sx = (uint32_t)stride - 1;
                if (sy >= c.bh * 8) sy = c.bh * 8 - 1;
                s[i] = c.plane[(size_t)sy * stride + sx];
            }
            uint8_t *o = &rgba[((size_t)y * w + x) * 4];
            if (ncomp == 1) {
                o[0] = o[1] = o[2] = s[0];
            } else if (rgb_direct) {
                o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
            } else {
                float Y = (float)s[0], cb = (float)s[1] - 128.0f, cr = (float)s[2] - 128.0f;
                o[0] = clamp8((int)std::lround(Y + 1.402f * cr));
                o[1] = clamp8((int)std::lround(Y - 0.344136f * cb - 0.714136f * cr));
                o[2] = clamp8((int)std::lround(Y + 1.772f * cb));
            }
            o[3] = 255;                                  // JPEG has no alpha
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}


// Opening a file whose PATH is UTF-8.
//
// On Windows fopen() interprets its argument in the machine's ANSI code page,
// not UTF-8 - so a path containing an umlaut, a Cyrillic letter or an emoji
// (which a picture folder may well have) simply does not open, and the caller
// reports "cannot open" about a file that is plainly there. The file dialog
// hands back UTF-16, this program carries UTF-8 everywhere, and the only place
// the two have to meet is here.
static FILE *open_utf8_rb(const char *path) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wn > 0) {
        std::vector<wchar_t> wp((size_t)wn);
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp.data(), wn) == wn) {
            FILE *wf = _wfopen(wp.data(), L"rb");
            if (wf) return wf;
        }
    }
#endif
    return std::fopen(path, "rb");
}

bool read_jpeg_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
                    char *err, size_t err_len) {
    FILE *f = open_utf8_rb(path);
    if (!f) { if (err && err_len) std::snprintf(err, err_len, "cannot open %s", path); return false; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)n);
    bool ok = std::fread(buf.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    if (!ok) { if (err && err_len) std::snprintf(err, err_len, "short read on %s", path); return false; }
    return read_jpeg(buf.data(), buf.size(), rgba, w, h, err, err_len);
}

} // namespace daiimg
