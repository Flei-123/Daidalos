// DEFLATE decoder (RFC 1951) and PNG reader (RFC 2083), written from scratch.
//
// The engine has no third party dependency, and that is not
// vanity: every dependency is a build system, a licence and a supply chain
// problem on every platform you ever port to. Inflate is ~200 lines, and it
// is verified against zlib-produced files in tests/test_image.cpp.
//
// Supported PNG subset: 8 and 16 bit, greyscale / GA / RGB / RGBA / palette,
// plain or Adam7 interlaced. That covers everything Blender, Krita, GIMP and
// any texture pipeline will hand you. 16 bit is downsampled to 8.
//
// read_png() also answers for JPEG. Every caller in the engine asks it "turn
// these bytes into pixels", and having each of them sniff the first three
// bytes for themselves is how one of them ends up not doing it. The JPEG
// decoder itself lives in src/dai_jpeg.cpp.

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>
#ifdef _WIN32
#  include <windows.h>
#endif

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
    if (inflate_raw(data + 2, size - 6, out)) return true;

    // The stream did not finish. Two things can be true at once here, and
    // treating them as one is what threw a readable picture away:
    //
    //   1. the last four bytes are the adler32 checksum - UNLESS the file was
    //      truncated, in which case they are the last four bytes of the data
    //      and skipping them removes real bits from the end;
    //   2. whatever WAS unpacked before it stopped is still correct. Deflate
    //      is sequential: byte 4000 does not become wrong because byte 90000
    //      never arrived.
    //
    // So: try again over the whole tail, keep whichever attempt recovered
    // more, and hand it back with a false. The caller decides whether what
    // arrived is enough to be a picture - see read_png, which draws the rows
    // it got instead of showing nothing at all.
    std::vector<uint8_t> tail;
    inflate_raw(data + 2, size - 2, tail);
    if (tail.size() > out.size()) out.swap(tail);
    return false;
}

// ---------------------------------------------------------------- PNG

namespace {

uint32_t be32_at(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// PNG's own CRC, so a chunk can be asked whether it is telling the truth about
// its own length. It is not paranoia: the file that started all this declares
// an IDAT 66 bytes shorter than the one it actually contains, and believing
// the number threw away the end of the picture.
uint32_t png_crc(const uint8_t *d, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Does a chunk header start here, and does it look like one? A type is four
// letters; a length past the end of the file is not a length.
bool looks_like_chunk(const uint8_t *file, size_t size, size_t at) {
    if (at + 8 > size) return false;
    for (int i = 0; i < 4; ++i) {
        uint8_t c = file[at + 4 + i];
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!letter) return false;
    }
    uint32_t len = ((uint32_t)file[at] << 24) | ((uint32_t)file[at + 1] << 16) |
                   ((uint32_t)file[at + 2] << 8) | file[at + 3];
    return (size_t)len + 12 + at <= size + 12;
}

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

} // namespace

bool read_jpeg(const uint8_t *file, size_t size, std::vector<uint8_t> &rgba,
               uint32_t *out_w, uint32_t *out_h, char *err, size_t err_len);

// Decodes a PNG file into tightly packed RGBA8. Returns false on anything it
// does not support, and says why in `err` when given.
bool read_png(const uint8_t *file, size_t size, std::vector<uint8_t> &rgba,
              uint32_t *out_w, uint32_t *out_h, char *err, size_t err_len) {
    auto fail = [&](const char *m) { if (err && err_len) std::snprintf(err, err_len, "%s", m); return false; };
    static const uint8_t SIG[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    // A JPEG is not a PNG, but it IS a picture, and the caller wanted pixels.
    if (size >= 3 && file[0] == 0xFF && file[1] == 0xD8 && file[2] == 0xFF)
        return read_jpeg(file, size, rgba, out_w, out_h, err, err_len);
    if (size < 8 || std::memcmp(file, SIG, 8) != 0) return fail("not a PNG file");

    uint32_t w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t len = be32_at(file + pos);
        const char *type = (const char *)file + pos + 4;
        const uint8_t *data = file + pos + 8;
        if (pos + 12 + len > size) {
            // The file ends inside a chunk. Hard-failing here threw away a
            // picture that was perfectly readable: writers append things, a
            // copy can lose its last bytes, and the ONLY chunks that matter
            // have usually arrived long before. If the header and some pixel
            // data are already in hand, decode what there is; complain only
            // when there is nothing to decode.
            if (w && h && !idat.empty()) break;
            char m[128];
            std::snprintf(m, sizeof(m),
                          "file ends inside a '%c%c%c%c' chunk: it wants %u more bytes",
                          type[0], type[1], type[2], type[3],
                          (unsigned)(pos + 12 + len - size));
            return fail(m);
        }
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len < 13) return fail("bad IHDR");
            w = be32_at(data); h = be32_at(data + 4);
            depth = data[8]; color = data[9]; interlace = data[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(data, data + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(data, data + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            uint32_t stored = be32_at(data + len);
            uint32_t want = png_crc(file + pos + 4, len + 4);
            uint32_t use = len;
            if (stored != want) {
                // The chunk is lying about itself. Rather than trust it or
                // give up, find where the NEXT chunk really starts and take
                // everything up to it: a writer that got the length wrong
                // still wrote the bytes, and those bytes are the picture.
                //
                // Windows opens this file. So should this decoder - being
                // stricter than every other reader on the machine is not
                // correctness, it is a bug with a rulebook.
                for (size_t q = pos + 8; q + 12 <= size; ++q) {
                    if (!looks_like_chunk(file, size, q)) continue;
                    const char *t2 = (const char *)file + q + 4;
                    if (std::memcmp(t2, "IEND", 4) && std::memcmp(t2, "IDAT", 4)) continue;
                    uint32_t l2 = be32_at(file + q);
                    if (png_crc(file + q + 4, l2 + 4) != be32_at(file + q + 8 + l2)) continue;
                    size_t end = q - 4;                     // its own CRC sits here
                    if (end > pos + 8 && end - (pos + 8) <= size) use = (uint32_t)(end - (pos + 8));
                    break;
                }
                if (use < len) use = len;                   // never shrink below the claim
            }
            if (pos + 8 + use > size) use = (uint32_t)(size - pos - 8);
            idat.insert(idat.end(), data, data + use);
            if (use != len) { pos += 12 + use; continue; }  // walk on from the REAL end
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    if (!w || !h) return fail("no IHDR");
    if (interlace != 0 && interlace != 1) return fail("unknown PNG interlace method");
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
    if (idat.empty()) return fail("no IDAT data");
    bool whole = inflate_zlib(idat.data(), idat.size(), raw);
    if (!whole && raw.empty())
        return fail("inflate failed (compressed image data could not be unpacked)");

    int bits_per_pixel = channels * depth;
    size_t stride = ((size_t)w * bits_per_pixel + 7) / 8;
    int filter_bpp = (bits_per_pixel + 7) / 8;

    // The picture, one row after another, filters already undone. Zeroed
    // because an interlaced file fills it in seven visits and a pass that is
    // empty on a narrow image must leave black rather than rubbish.
    std::vector<uint8_t> img((size_t)stride * h, 0);
    const uint8_t *src = raw.data();
    size_t left = raw.size();
    const char *why = nullptr;
    uint32_t short_by = 0;             // rows the file did not contain
    uint32_t bad_rows = 0;             // rows whose filter byte was impossible

    // Undoes the per scanline filters of ONE rectangle of pixels. A plain PNG
    // is one of these that happens to be the whole image; an interlaced one is
    // seven of them, each a smaller picture in its own right. Writing it once
    // is the only reason Adam7 costs a dozen lines instead of a second decoder.
    auto unfilter = [&](uint32_t pw, uint32_t ph, std::vector<uint8_t> &out) -> bool {
        size_t ps = ((size_t)pw * bits_per_pixel + 7) / 8;
        out.assign(ps * ph, 0);
        if (!pw || !ph) return true;
        // A truncated file gets the rows it has. Refusing the whole picture
        // over a missing tail is how a logo that was 99% there showed up as a
        // red cross - and the cross says nothing about which bytes were lost.
        uint32_t have = (ps + 1) ? (uint32_t)(left / (ps + 1)) : 0;
        if (have < ph) {
            short_by += ph - have;
            ph = have;
            if (!ph) return true;      // nothing at all: the rows stay blank
        }
        for (uint32_t y = 0; y < ph; ++y) {
            int f = *src++; --left;
            if (f > 4) { ++bad_rows; f = 0; }
            uint8_t *cur = &out[(size_t)y * ps];
            const uint8_t *prev = y ? &out[(size_t)(y - 1) * ps] : nullptr;
            for (size_t x = 0; x < ps; ++x) {
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
                default:
                    // An impossible filter byte. Every other reader on the
                    // machine - Windows Photos included - treats it as "no
                    // filter" and keeps going, and what comes out is a picture
                    // with a scar rather than no picture at all. Counted, so
                    // the Console can say how much of it is suspect.
                    break;
                }
                cur[x] = (uint8_t)v;
            }
            src += ps; left -= ps;
        }
        return true;
    };

    if (!interlace) {
        std::vector<uint8_t> pass;
        if (!unfilter(w, h, pass)) return fail(why);
        img.swap(pass);
    } else {
        // Adam7. Seven passes, each picking a coarser or finer lattice of the
        // same picture, so a half loaded file already shows something. Nothing
        // about DECODING changes - the pixels simply land somewhere other than
        // next to each other, which is the whole of the work below.
        static const uint32_t XO[7] = { 0, 4, 0, 2, 0, 1, 0 };
        static const uint32_t YO[7] = { 0, 0, 4, 0, 2, 0, 1 };
        static const uint32_t XS[7] = { 8, 8, 4, 4, 2, 2, 1 };
        static const uint32_t YS[7] = { 8, 8, 8, 4, 4, 2, 2 };
        for (int pi = 0; pi < 7; ++pi) {
            uint32_t pw = w > XO[pi] ? (w - XO[pi] + XS[pi] - 1) / XS[pi] : 0;
            uint32_t ph = h > YO[pi] ? (h - YO[pi] + YS[pi] - 1) / YS[pi] : 0;
            if (!pw || !ph) continue;          // a pass can be empty on a small image
            std::vector<uint8_t> pass;
            if (!unfilter(pw, ph, pass)) return fail(why);
            size_t ps = ((size_t)pw * bits_per_pixel + 7) / 8;
            for (uint32_t py = 0; py < ph; ++py) {
                const uint8_t *prow = &pass[(size_t)py * ps];
                uint8_t *drow = &img[(size_t)(YO[pi] + py * YS[pi]) * stride];
                for (uint32_t px = 0; px < pw; ++px) {
                    uint32_t dx = XO[pi] + px * XS[pi];
                    if (bits_per_pixel >= 8) {
                        size_t n = (size_t)bits_per_pixel / 8;
                        std::memcpy(drow + (size_t)dx * n, prow + (size_t)px * n, n);
                    } else {
                        // 1, 2 and 4 bit palette entries: the destination byte
                        // holds several pixels, so this is a read and a merge,
                        // not a copy.
                        int per = 8 / bits_per_pixel;
                        unsigned mask = (1u << bits_per_pixel) - 1u;
                        int ss = 8 - bits_per_pixel * (int)(px % (uint32_t)per) - bits_per_pixel;
                        unsigned v = ((unsigned)prow[px / (uint32_t)per] >> ss) & mask;
                        int ds = 8 - bits_per_pixel * (int)(dx % (uint32_t)per) - bits_per_pixel;
                        uint8_t &dst = drow[dx / (uint32_t)per];
                        dst = (uint8_t)((dst & ~(mask << ds)) | (v << ds));
                    }
                }
            }
        }
    }

    rgba.assign((size_t)w * h * 4, 255);
    size_t bad_rows_dummy = 0;         // pixels whose palette index does not exist
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
                if (palette.empty()) return fail("paletted PNG with no PLTE chunk");
                // An index past the end of the palette can only come from
                // bytes that are not picture data any more. Refusing the whole
                // file over one is how a damaged logo became a red cross;
                // clamping it paints that pixel the first colour and lets the
                // rest of the image through, which is what every other reader
                // does.
                if (idx * 3 + 2 >= palette.size()) { idx = 0; ++bad_rows_dummy; }
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
    // It IS a picture, and the caller gets it. But a file that ends in the
    // middle is a file somebody should know about - a download that stopped,
    // a disk that filled up - so the reason travels back in `err` even on the
    // way out of a successful call.
    if (bad_rows_dummy && !bad_rows) bad_rows = 1;   // damaged, even if the rows lined up
    if ((!whole || short_by || bad_rows) && err && err_len) {
        if (bad_rows)
            std::snprintf(err, err_len,
                          "the file is damaged: %u of %u rows had to be guessed at, so the "
                          "picture is shown but parts of it will look wrong",
                          bad_rows, h);
        else if (short_by)
            std::snprintf(err, err_len,
                          "the file ends early: %u of %u rows arrived, the rest is blank",
                          h - short_by, h);
        else
            std::snprintf(err, err_len,
                          "the file ends without its checksum - every row arrived, so it "
                          "was decoded anyway");
    }
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

bool read_png_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
                   char *err, size_t err_len) {
    FILE *f = open_utf8_rb(path);
    if (!f) { if (err && err_len) std::snprintf(err, err_len, "cannot open %s", path); return false; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)n);
    bool ok = std::fread(buf.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    if (!ok) { if (err && err_len) std::snprintf(err, err_len, "short read on %s", path); return false; }
    return read_png(buf.data(), buf.size(), rgba, w, h, err, err_len);
}

} // namespace daiimg
