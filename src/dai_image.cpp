// Image output for the renderer: PPM (trivial) and PNG.
//
// The PNG writer stores the data in uncompressed deflate blocks, so it needs
// no zlib and no third party header. Files are bigger than a real encoder
// would produce, which is a fine trade for "the engine has zero image
// dependencies and screenshots just work".

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

} // namespace

bool write_png_rgb(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    // raw scanlines: filter byte 0 + RGB triples
    std::vector<uint8_t> raw;
    raw.reserve(((size_t)w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t *row = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) { raw.push_back(row[x*4]); raw.push_back(row[x*4+1]); raw.push_back(row[x*4+2]); }
    }

    // zlib stream with stored deflate blocks
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    const size_t MAX = 65535;
    for (size_t off = 0; off < raw.size(); off += MAX) {
        size_t n = raw.size() - off < MAX ? raw.size() - off : MAX;
        bool last = (off + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF)); z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
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
