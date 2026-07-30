// Verifies the from scratch DEFLATE/PNG decoder against files produced by a
// real encoder (Python's zlib + PIL, see tools/make_png_fixtures.py). The
// fixtures cover every colour type, both bit depths and all five PNG filters,
// because a decoder that only handles the one case you tested is worse than
// no decoder at all: it fails silently on someone else's texture.
//
//   ./build/test_image /tmp/pngfix

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

namespace daiimg {
bool read_png_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
                   char *err, size_t err_len);
bool inflate_zlib(const uint8_t *data, size_t size, std::vector<uint8_t> &out);
}

static int g_fail = 0, g_pass = 0;

static bool load(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    bool ok = std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

static void check_png(const std::string &dir, const char *name, uint32_t ew, uint32_t eh) {
    std::string png = dir + "/" + name + ".png";
    std::string raw = dir + "/" + name + ".rgba";
    std::vector<uint8_t> got, want;
    uint32_t w = 0, h = 0;
    char err[256] = {0};
    if (!daiimg::read_png_file(png.c_str(), got, &w, &h, err, sizeof(err))) {
        std::printf("  FAIL %-22s decoder said: %s\n", name, err); ++g_fail; return;
    }
    if (!load(raw, want)) { std::printf("  SKIP %-22s (no reference)\n", name); return; }
    if (w != ew || h != eh) {
        std::printf("  FAIL %-22s size %ux%u, expected %ux%u\n", name, w, h, ew, eh); ++g_fail; return;
    }
    if (got.size() != want.size()) {
        std::printf("  FAIL %-22s %zu bytes, expected %zu\n", name, got.size(), want.size()); ++g_fail; return;
    }
    size_t diff = 0; int maxdiff = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        int d = std::abs((int)got[i] - (int)want[i]);
        if (d) { ++diff; if (d > maxdiff) maxdiff = d; }
    }
    if (diff) {
        std::printf("  FAIL %-22s %zu/%zu bytes differ (max %d)\n", name, diff, got.size(), maxdiff);
        ++g_fail;
    } else {
        std::printf("  ok   %-22s %ux%u exact\n", name, w, h);
        ++g_pass;
    }
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp/pngfix";
    std::printf("PNG/inflate fixtures from %s\n", dir.c_str());

    // raw inflate against a zlib stream produced by Python
    {
        std::vector<uint8_t> z, want, got;
        if (load(dir + "/blob.z", z) && load(dir + "/blob.bin", want)) {
            if (!daiimg::inflate_zlib(z.data(), z.size(), got)) {
                std::printf("  FAIL inflate_zlib returned false\n"); ++g_fail;
            } else if (got != want) {
                std::printf("  FAIL inflate_zlib: %zu bytes, expected %zu\n", got.size(), want.size()); ++g_fail;
            } else {
                std::printf("  ok   inflate_zlib          %zu bytes exact\n", got.size()); ++g_pass;
            }
        }
    }

    check_png(dir, "rgb8",       64, 48);
    check_png(dir, "rgba8",      64, 48);
    check_png(dir, "grey8",      64, 48);
    check_png(dir, "greyalpha8", 64, 48);
    check_png(dir, "palette8",   64, 48);
    check_png(dir, "rgb16",      64, 48);
    check_png(dir, "noise_rgba", 256, 256);   // large, exercises long matches
    check_png(dir, "gradient",   512, 8);     // wide rows, filter heavy

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
