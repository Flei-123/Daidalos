// Verifies the from scratch DEFLATE/PNG/JPEG decoders against files produced
// by a real encoder (Python's zlib + PIL, see tools/make_png_fixtures.py). The
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
bool read_jpeg_file(const char *path, std::vector<uint8_t> &rgba, uint32_t *w, uint32_t *h,
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

// A JPEG is lossy and every decoder rounds differently: libjpeg uses an
// integer IDCT and a triangle filter for chroma, this one uses floats and
// repeats the sample. So the question is not "identical" - it is "did it
// decode the same PICTURE", which is what an average error under a level and
// no wild outlier says.
static void check_jpeg(const std::string &dir, const char *name, uint32_t ew, uint32_t eh,
                       double avg_allow, int max_allow) {
    std::string jpg = dir + "/" + name + ".jpg";
    std::string raw = dir + "/" + name + ".rgba";
    std::vector<uint8_t> got, want;
    uint32_t w = 0, h = 0;
    char err[256] = {0};
    if (!daiimg::read_jpeg_file(jpg.c_str(), got, &w, &h, err, sizeof(err))) {
        std::printf("  FAIL %-22s decoder said: %s\n", name, err); ++g_fail; return;
    }
    if (!load(raw, want)) { std::printf("  SKIP %-22s (no reference)\n", name); return; }
    if (w != ew || h != eh) {
        std::printf("  FAIL %-22s size %ux%u, expected %ux%u\n", name, w, h, ew, eh); ++g_fail; return;
    }
    if (got.size() != want.size()) {
        std::printf("  FAIL %-22s %zu bytes, expected %zu\n", name, got.size(), want.size()); ++g_fail; return;
    }
    double sum = 0; int maxdiff = 0; size_t n = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        if ((i & 3) == 3) continue;                 // alpha is 255 either way
        int d = std::abs((int)got[i] - (int)want[i]);
        sum += d; ++n;
        if (d > maxdiff) maxdiff = d;
    }
    double avg = n ? sum / (double)n : 0.0;
    if (avg > avg_allow || maxdiff > max_allow) {
        std::printf("  FAIL %-22s average error %.2f (allowed %.2f), worst %d (allowed %d)\n",
                    name, avg, avg_allow, maxdiff, max_allow);
        ++g_fail;
    } else {
        std::printf("  ok   %-22s %ux%u, average error %.2f, worst %d\n", name, w, h, avg, maxdiff);
        ++g_pass;
    }
}

// Something the decoder does NOT do has to be REFUSED, not half decoded into
// a picture of noise. The message is part of the behaviour: it is what the
// editor shows the person who dragged the file in.
static void check_refused(const std::string &dir, const char *file, const char *needle) {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    char err[256] = {0};
    std::string path = dir + "/" + file;
    bool ok = daiimg::read_jpeg_file(path.c_str(), px, &w, &h, err, sizeof(err));
    if (ok) {
        std::printf("  FAIL %-22s was decoded, and it should have been refused\n", file);
        ++g_fail;
    } else if (!std::strstr(err, needle)) {
        std::printf("  FAIL %-22s refused with \"%s\", which does not mention '%s'\n",
                    file, err, needle);
        ++g_fail;
    } else {
        std::printf("  ok   %-22s refused: %s\n", file, err);
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

    // Adam7. The same pictures, delivered in seven passes instead of one.
    check_png(dir, "i_rgba8",    64, 48);
    check_png(dir, "i_rgb8",     64, 48);
    check_png(dir, "i_small",    17, 13);     // passes of every shape, two empty
    check_png(dir, "i_pal4",     64, 48);     // 4 bit indices, unpacked and repacked

    // The PNG entry point is what the engine calls for any picture, so it has
    // to hand a JPEG to the JPEG decoder rather than complain about it.
    {
        std::vector<uint8_t> px; uint32_t w = 0, h = 0; char err[256] = {0};
        std::string j = dir + "/j_444.jpg";
        if (daiimg::read_png_file(j.c_str(), px, &w, &h, err, sizeof(err)) && w == 96 && h == 64) {
            std::printf("  ok   read_png_file on a .jpg  decoded it anyway (%ux%u)\n", w, h); ++g_pass;
        } else {
            std::printf("  FAIL read_png_file on a .jpg: %s\n", err); ++g_fail;
        }
    }

    check_jpeg(dir, "j_444",  96, 64, 1.0, 8);
    check_jpeg(dir, "j_422",  96, 64, 1.2, 8);    // chroma halved across
    check_jpeg(dir, "j_420",  96, 64, 1.2, 8);    // chroma halved both ways
    check_jpeg(dir, "j_grey", 96, 64, 1.0, 8);
    check_jpeg(dir, "j_odd",  51, 37, 1.2, 8);    // not a whole number of MCUs
    check_jpeg(dir, "j_restart", 96, 64, 1.2, 8);

    // ---- a chunk that lies about its own length --------------------------
    // Every byte of the picture is in the file; the number in front of it is
    // 66 too small. Believing that number is what made a logo Windows opens
    // fine unreadable here. The decoded image has to come out EXACT.
    check_png(dir, "lie_rgb8", 64, 48);

    // ---- a file that stops in the middle ---------------------------------
    // The decoder must hand back what IS a picture and say how much that was.
    // Refusing outright is what turned a damaged logo into a red cross with no
    // explanation, and a red cross is not a bug report.
    {
        std::vector<uint8_t> got, want;
        uint32_t w = 0, h = 0;
        char err[256] = { 0 };
        std::string cut = dir + "/cut_rgb8.png";
        bool ok = daiimg::read_png_file(cut.c_str(), got, &w, &h, err, sizeof(err));
        if (!ok) {
            std::printf("  FAIL cut_rgb8               refused a partly readable file: %s\n", err);
            ++g_fail;
        } else if (w != 64 || h != 48) {
            std::printf("  FAIL cut_rgb8               size %ux%u, expected 64x48\n", w, h);
            ++g_fail;
        } else if (!err[0]) {
            std::printf("  FAIL cut_rgb8               decoded silently - nobody learns the file is damaged\n");
            ++g_fail;
        } else if (!load(dir + "/rgb8.rgba", want)) {
            std::printf("  SKIP cut_rgb8               (no reference)\n");
        } else {
            // The rows that arrived have to be RIGHT, not merely present.
            size_t rows_ok = 0;
            for (uint32_t y = 0; y < h; ++y) {
                bool same = std::memcmp(&got[(size_t)y * w * 4], &want[(size_t)y * w * 4],
                                        (size_t)w * 4) == 0;
                if (!same) break;
                ++rows_ok;
            }
            if (rows_ok < 8) {
                std::printf("  FAIL cut_rgb8               only %zu rows came back correct\n", rows_ok);
                ++g_fail;
            } else if (rows_ok >= h) {
                std::printf("  FAIL cut_rgb8               all %u rows survived a cut file - "
                            "is the fixture actually truncated?\n", h);
                ++g_fail;
            } else {
                std::printf("  ok   cut_rgb8               %zu of %u rows recovered, and it says so: %s\n",
                            rows_ok, h, err);
                ++g_pass;
            }
        }
    }
    // A file that is whole says NOTHING - a warning on every good picture is a
    // warning nobody reads.
    {
        std::vector<uint8_t> px;
        uint32_t w = 0, h = 0;
        char err[256] = { 0 };
        std::string good = dir + "/rgb8.png";
        bool ok = daiimg::read_png_file(good.c_str(), px, &w, &h, err, sizeof(err));
        if (ok && !err[0]) { std::printf("  ok   rgb8 (intact)          decoded without a complaint\n"); ++g_pass; }
        else { std::printf("  FAIL rgb8 (intact)          ok=%d, said '%s'\n", (int)ok, err); ++g_fail; }
    }

    check_refused(dir, "j_progressive.jpg", "progressive");
    check_refused(dir, "rgb8.png", "not a JPEG");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
