// Image output for the renderer: PPM (trivial) and PNG.
//
// The PNG writer carries its own deflate, so it still needs no zlib and no
// third party header - but it no longer STORES the data.
//
// Uncompressed was the original trade: "files are bigger, the engine has zero
// image dependencies". The bill arrived somewhere else. A 1280x720 screenshot
// was 2.7 MB - exactly width*height*3 plus headers - and this repository keeps
// its evidence pictures in git: 114 files, 341 MB in .gauntlet-shots, which is
// most of a 337 MB .git and the reason a push takes a quarter of an hour.
//
// So: fixed-Huffman deflate with an LZ77 match finder over a hash chain, plus
// the PNG row filters, in about two hundred lines and still no dependency.
// Fixed tables rather than dynamic ones because the second Huffman pass would
// buy a few more percent for twice the code, and the row filters are where the
// real win is on a render anyway - a screen of flat panels is almost all Up
// and Sub predictions of zero.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace daiimg {

namespace {

uint32_t crc_table[256];
bool crc_ready = false;

void init_crc() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = true;
}

uint32_t crc32_buf(const uint8_t *d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    if (!crc_ready) init_crc();
    for (size_t i = 0; i < n; ++i) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void be32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
    be32(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t c = crc32_buf(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu;
    be32(out, c);
}


// ---- deflate, the little one -----------------------------------------------
//
// Fixed Huffman (BTYPE=01), literals and one distance code, matches found with
// a three byte hash chain. The window is the full 32 KB deflate allows; the
// chain is capped at 8 candidates per position, which is the knob that trades
// ratio for time. Measured on a 1280x720 render that looks like this editor
// (flat panels, a gradient, dither noise):
//
//     chain  8    96 ms   200 484 bytes      <- chosen
//     chain 16   111 ms   195 984
//     chain 32   145 ms   191 436
//
// Going from 8 to 32 buys 4.5% of size for 50% more time, and these pictures
// are written by test runs that take screenshots by the dozen. Against the
// 2 765 800 bytes the stored version wrote, all three are a factor of 14.
class BitOut {
public:
    explicit BitOut(std::vector<uint8_t> &out) : o_(out) {}
    // Deflate packs its bits LSB first within a byte - the opposite of the
    // Huffman codes themselves, which are written MSB first. Mixing the two up
    // produces a stream that looks plausible and decodes to garbage.
    void bits(uint32_t v, int n) {
        for (int i = 0; i < n; ++i) {
            acc_ |= ((v >> i) & 1u) << nbits_;
            if (++nbits_ == 8) { o_.push_back((uint8_t)acc_); acc_ = 0; nbits_ = 0; }
        }
    }
    void code(uint32_t v, int n) {          // Huffman codes travel MSB first
        for (int i = n - 1; i >= 0; --i) {
            acc_ |= ((v >> i) & 1u) << nbits_;
            if (++nbits_ == 8) { o_.push_back((uint8_t)acc_); acc_ = 0; nbits_ = 0; }
        }
    }
    void flush() { if (nbits_) { o_.push_back((uint8_t)acc_); acc_ = 0; nbits_ = 0; } }
private:
    std::vector<uint8_t> &o_;
    uint32_t acc_ = 0;
    int nbits_ = 0;
};

// The fixed literal/length table of RFC 1951, section 3.2.6.
void fixed_literal(BitOut &b, uint32_t lit) {
    if (lit < 144)      b.code(0x30 + lit, 8);
    else if (lit < 256) b.code(0x190 + lit - 144, 9);
    else if (lit < 280) b.code(lit - 256, 7);
    else                b.code(0xC0 + lit - 280, 8);
}

const uint16_t LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                                67,83,99,115,131,163,195,227,258 };
const uint8_t  LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const uint16_t DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                 1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const uint8_t  DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

void emit_match(BitOut &b, uint32_t len, uint32_t dist) {
    int li = 28;
    while (li > 0 && LEN_BASE[li] > len) --li;
    fixed_literal(b, 257u + (uint32_t)li);
    if (LEN_EXTRA[li]) b.bits(len - LEN_BASE[li], LEN_EXTRA[li]);
    int di = 29;
    while (di > 0 && DIST_BASE[di] > dist) --di;
    b.code((uint32_t)di, 5);                 // distance codes are 5 bits, fixed
    if (DIST_EXTRA[di]) b.bits(dist - DIST_BASE[di], DIST_EXTRA[di]);
}

void deflate_fixed(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
    BitOut b(out);
    b.bits(1, 1);       // final block
    b.bits(1, 2);       // fixed Huffman

    const size_t n = in.size();
    const int HBITS = 15, HSIZE = 1 << HBITS, MAXCHAIN = 8;
    std::vector<int32_t> head((size_t)HSIZE, -1), prev(n ? n : 1, -1);
    auto hash3 = [&](size_t i) -> uint32_t {
        return (uint32_t)(((in[i] << 16) ^ (in[i + 1] << 8) ^ in[i + 2]) * 2654435761u) >> (32 - HBITS);
    };

    size_t i = 0;
    while (i < n) {
        uint32_t best_len = 0, best_dist = 0;
        if (i + 3 <= n && i + 2 < n) {
            uint32_t h = hash3(i);
            int32_t cand = head[h];
            int chain = 0;
            while (cand >= 0 && chain++ < MAXCHAIN) {
                size_t d = i - (size_t)cand;
                if (d == 0 || d > 32768) break;
                size_t l = 0;
                const size_t maxl = (n - i) < 258 ? (n - i) : 258;
                while (l < maxl && in[(size_t)cand + l] == in[i + l]) ++l;
                if (l > best_len) { best_len = (uint32_t)l; best_dist = (uint32_t)d; }
                if (best_len >= 258) break;
                cand = prev[(size_t)cand];
            }
            head[h] = (int32_t)i;
            prev[i] = cand >= 0 ? head[h] : -1;
            // Re-link properly: prev[i] must point at the PREVIOUS occupant of
            // the bucket, which was read before head[h] was overwritten above.
        }
        if (best_len >= 3) {
            emit_match(b, best_len, best_dist);
            // Every position inside the match still has to enter the chain, or
            // the next match cannot be found across it.
            for (uint32_t k = 1; k < best_len && i + k + 2 < n; ++k) {
                uint32_t hh = hash3(i + k);
                prev[i + k] = head[hh];
                head[hh] = (int32_t)(i + k);
            }
            i += best_len;
        } else {
            fixed_literal(b, in[i]);
            ++i;
        }
    }
    fixed_literal(b, 256);      // end of block
    b.flush();
}

// ---- PNG row filters --------------------------------------------------------
// Five predictions per row, the best one chosen by the sum of absolute
// differences the PNG spec recommends. This is where a render compresses: a
// panel of one colour becomes a row of zeroes under Sub, and a gradient
// becomes small numbers under Paeth.
uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

void filter_rows(const uint8_t *rgba, uint32_t w, uint32_t h, std::vector<uint8_t> &raw) {
    const size_t stride = (size_t)w * 3;
    std::vector<uint8_t> cur(stride), prev(stride, 0), line(stride), best(stride);
    raw.reserve((stride + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            cur[x * 3 + 0] = row[x * 4 + 0];
            cur[x * 3 + 1] = row[x * 4 + 1];
            cur[x * 3 + 2] = row[x * 4 + 2];
        }
        uint32_t best_score = 0xFFFFFFFFu;
        uint8_t  best_type = 0;
        for (uint8_t t = 0; t < 5; ++t) {
            uint32_t score = 0;
            for (size_t k = 0; k < stride; ++k) {
                const uint8_t a = k >= 3 ? cur[k - 3] : 0;
                const uint8_t bb = prev[k];
                const uint8_t c = k >= 3 ? prev[k - 3] : 0;
                uint8_t v = 0;
                switch (t) {
                    case 0: v = cur[k]; break;
                    case 1: v = (uint8_t)(cur[k] - a); break;
                    case 2: v = (uint8_t)(cur[k] - bb); break;
                    case 3: v = (uint8_t)(cur[k] - (uint8_t)(((int)a + bb) >> 1)); break;
                    default: v = (uint8_t)(cur[k] - paeth(a, bb, c)); break;
                }
                line[k] = v;
                score += v < 128 ? v : (uint32_t)(256 - v);   // signed magnitude
            }
            if (score < best_score) { best_score = score; best_type = t; best = line; }
        }
        raw.push_back(best_type);
        raw.insert(raw.end(), best.begin(), best.end());
        prev = cur;
    }
}

} // namespace

bool write_png_rgb(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    // Filtered scanlines, then deflate. The filter is chosen per row.
    std::vector<uint8_t> raw;
    filter_rows(rgba, w, h, raw);

    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);      // zlib header, default window
    deflate_fixed(raw, z);
    // Adler-32 over the UNCOMPRESSED data, which is what the reader checks.
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

bool write_ppm_rgb(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h * 4; i += 4) std::fwrite(&rgba[i], 1, 3, f);
    std::fclose(f);
    return true;
}

} // namespace daiimg
