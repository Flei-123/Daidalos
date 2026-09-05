// DEFLATE decoder (RFC 1951) and PNG reader (RFC 2083), written from scratch.
//
// The engine has no third party dependency, and that is not
// vanity: every dependency is a build system, a licence and a supply chain
// problem on every platform you ever port to. Inflate is ~200 lines, and it
// is verified against zlib-produced files in tests/test_image.cpp.
//
// Supported PNG subset: 8 and 16 bit, greyscale / GA / RGB / RGBA / palette,
// non interlaced. That covers everything Blender, Krita, GIMP and any
// texture pipeline will hand you. 16 bit is downsampled to 8.

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace daiimg {

namespace {

struct BitReader {
    const uint8_t *p, *end;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool bad = false;

    BitReader(const uint8_t *d, size_t n) : p(d), end(d + n) {}

    int bit() {
        if (bitcnt == 0) {
            if (p >= end) { bad = true; return 0; }
            bitbuf = *p++; bitcnt = 8;
        }
        int b = bitbuf & 1; bitbuf >>= 1; --bitcnt;
        return b;
    }
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= (uint32_t)bit() << i;
        return v;
    }
    void align() { bitcnt = 0; }
};

// Canonical Huffman table: decode by walking code lengths, which is small and
// fast enough for texture loading (this is not a streaming video decoder).
struct Huffman {
    std::vector<uint16_t> counts;   // number of codes per length
    std::vector<uint16_t> symbols;  // symbols ordered by code

    void build(const uint8_t *lengths, int n) {
        counts.assign(16, 0);
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        std::vector<uint16_t> offs(16, 0);
        for (int i = 1; i < 16; ++i) offs[i] = offs[i - 1] + counts[i - 1];
        symbols.assign(n, 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[offs[lengths[i]]++] = (uint16_t)i;
    }

    int decode(BitReader &br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.bit();
            int count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
const uint8_t  LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const uint16_t DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const uint8_t  DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

bool inflate_block(BitReader &br, const Huffman &lit, const Huffman &dist, std::vector<uint8_t> &out) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0 || br.bad) return false;
        if (sym < 256) { out.push_back((uint8_t)sym); continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int length = LEN_BASE[sym] + (int)br.bits(LEN_EXTRA[sym]);
        int dsym = dist.decode(br);
        if (dsym < 0 || dsym >= 30) return false;
        int distance = DIST_BASE[dsym] + (int)br.bits(DIST_EXTRA[dsym]);
        if ((size_t)distance > out.size()) return false;
        size_t start = out.size() - distance;
        for (int i = 0; i < length; ++i) out.push_back(out[start + i]);
    }
}

void fixed_tables(Huffman &lit, Huffman &dist) {
    uint8_t l[288];
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    lit.build(l, 288);
    uint8_t d[30];
    for (int i = 0; i < 30; ++i) d[i] = 5;
    dist.build(d, 30);
}

bool dynamic_tables(BitReader &br, Huffman &lit, Huffman &dist) {
    static const uint8_t ORDER[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int hlit = (int)br.bits(5) + 257;
    int hdist = (int)br.bits(5) + 1;
    int hclen = (int)br.bits(4) + 4;
    uint8_t clen[19] = {0};
    for (int i = 0; i < hclen; ++i) clen[ORDER[i]] = (uint8_t)br.bits(3);
    Huffman code_huff;
    code_huff.build(clen, 19);

    std::vector<uint8_t> lengths(hlit + hdist, 0);
    int i = 0;
    while (i < hlit + hdist) {
        int sym = code_huff.decode(br);
        if (sym < 0 || br.bad) return false;
        if (sym < 16) { lengths[i++] = (uint8_t)sym; }
        else if (sym == 16) {
            if (i == 0) return false;
            uint8_t prev = lengths[i - 1];
            int rep = 3 + (int)br.bits(2);
            while (rep-- && i < hlit + hdist) lengths[i++] = prev;
        } else if (sym == 17) {
            int rep = 3 + (int)br.bits(3);
            while (rep-- && i < hlit + hdist) lengths[i++] = 0;
        } else {
            int rep = 11 + (int)br.bits(7);
            while (rep-- && i < hlit + hdist) lengths[i++] = 0;
        }
    }
    lit.build(lengths.data(), hlit);
    dist.build(lengths.data() + hlit, hdist);
    return true;
}

} // namespace

// Raw DEFLATE stream -> bytes.
bool inflate_raw(const uint8_t *data, size_t size, std::vector<uint8_t> &out) {
    BitReader br(data, size);
    for (;;) {
        int final_block = br.bit();
        int type = (int)br.bits(2);
        if (br.bad) return false;
        if (type == 0) {
            br.align();
            if (br.p + 4 > br.end) return false;
            uint16_t len = (uint16_t)(br.p[0] | (br.p[1] << 8));
            br.p += 4;
            if (br.p + len > br.end) return false;
            out.insert(out.end(), br.p, br.p + len);
            br.p += len;
        } else if (type == 1 || type == 2) {
            Huffman lit, dist;
            if (type == 1) fixed_tables(lit, dist);
            else if (!dynamic_tables(br, lit, dist)) return false;
            if (!inflate_block(br, lit, dist, out)) return false;
        } else {
            return false;
        }
        if (final_block) return true;
    }
}

// zlib wrapper (RFC 1950): 2 byte header, deflate stream, adler32.
bool inflate_zlib(const uint8_t *data, size_t size, std::vector<uint8_t> &out) {
    if (size < 6) return false;
    if ((data[0] & 0x0f) != 8) return false;          // must be deflate
    if (((data[0] << 8) | data[1]) % 31 != 0) return false;
    if (data[1] & 0x20) return false;                 // preset dictionary: no
    return inflate_raw(data + 2, size - 6, out);
}

// ---------------------------------------------------------------- PNG

namespace {

uint32_t be32_at(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

} // namespace

// Decodes a PNG file into tightly packed RGBA8. Returns false on anything it
// does not support, and says why in `err` when given.
bool read_png(const uint8_t *file, size_t size, std::vector<uint8_t> &rgba,
              uint32_t *out_w, uint32_t *out_h, char *err, size_t err_len) {
    auto fail = [&](const char *m) { if (err && err_len) std::snprintf(err, err_len, "%s", m); return false; };
    static const uint8_t SIG[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    if (size < 8 || std::memcmp(file, SIG, 8) != 0) return fail("not a PNG");

    uint32_t w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t len = be32_at(file + pos);
        const char *type = (const char *)file + pos + 4;
        const uint8_t *data = file + pos + 8;
        if (pos + 12 + len > size) return fail("truncated chunk");
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len < 13) return fail("bad IHDR");
            w = be32_at(data); h = be32_at(data + 4);
            depth = data[8]; color = data[9]; interlace = data[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(data, data + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(data, data + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), data, data + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (!w || !h) return fail("no IHDR");
    if (interlace) return fail("interlaced PNG not supported");
    if (depth != 8 && depth != 16 && !(color == 3 && (depth == 1 || depth == 2 || depth == 4)))
        return fail("unsupported bit depth");

    int channels;
    switch (color) {
    case 0: channels = 1; break;   // grey
    case 2: channels = 3; break;   // rgb
    case 3: channels = 1; break;   // palette index
    case 4: channels = 2; break;   // grey + alpha
    case 6: channels = 4; break;   // rgba
    default: return fail("unsupported colour type");
    }

    std::vector<uint8_t> raw;
    raw.reserve((size_t)w * h * channels + h);
    if (!inflate_zlib(idat.data(), idat.size(), raw)) return fail("inflate failed");

    int bits_per_pixel = channels * depth;
    size_t stride = ((size_t)w * bits_per_pixel + 7) / 8;
    int filter_bpp = (bits_per_pixel + 7) / 8;
    if (raw.size() < (stride + 1) * h) return fail("not enough image data");

    // undo the per scanline filters in place
    std::vector<uint8_t> img((size_t)stride * h);
    const uint8_t *src = raw.data();
    for (uint32_t y = 0; y < h; ++y) {
        int f = *src++;
        uint8_t *cur = &img[(size_t)y * stride];
        const uint8_t *prev = y ? &img[(size_t)(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            int a = (x >= (size_t)filter_bpp) ? cur[x - filter_bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (size_t)filter_bpp) ? prev[x - filter_bpp] : 0;
            int v = src[x];
            switch (f) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: v += paeth(a, b, c); break;
            default: return fail("bad filter type");
            }
            cur[x] = (uint8_t)v;
        }
        src += stride;
    }

    rgba.assign((size_t)w * h * 4, 255);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row = &img[(size_t)y * stride];
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *o = &rgba[((size_t)y * w + x) * 4];
            if (color == 3) {
                uint32_t idx;
                if (depth == 8) idx = row[x];
                else {
                    int per = 8 / depth;
                    int shift = 8 - depth * (int)(x % per) - depth;
                    idx = (row[x / per] >> shift) & ((1 << depth) - 1);
                }
                if (idx * 3 + 2 >= palette.size()) return fail("palette index out of range");
                o[0] = palette[idx*3]; o[1] = palette[idx*3+1]; o[2] = palette[idx*3+2];
                o[3] = idx < trns.size() ? trns[idx] : 255;
            } else {
                int step = depth / 8;                    // 1 or 2 bytes per sample
                const uint8_t *p = row + (size_t)x * channels * step;
                auto sample = [&](int c) -> uint8_t { return p[c * step]; };   // 16 bit -> take the high byte
                if (color == 0) { o[0] = o[1] = o[2] = sample(0); }
                else if (color == 4) { o[0] = o[1] = o[2] = sample(0); o[3] = sample(1); }
                else if (color == 2) { o[0] = sample(0); o[1] = sample(1); o[2] = sample(2); }
                else { o[0] = sample(0); o[1] = sample(1); o[2] = sample(2); o[3] = sample(3); }
            }
        }
    }
    *out_w = w; *out_h = h;
    return true;
}

bool read_png_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
                   char *err, size_t err_len) {
    FILE *f = std::fopen(path, "rb");
    if (!f) { if (err && err_len) std::snprintf(err, err_len, "cannot open %s", path); return false; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)n);
    bool ok = std::fread(buf.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    if (!ok) { if (err && err_len) std::snprintf(err, err_len, "short read on %s", path); return false; }
    return read_png(buf.data(), buf.size(), rgba, w, h, err, err_len);
}

} // namespace daiimg
