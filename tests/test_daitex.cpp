// The procedural texture baker: a .daitex node graph in, three maps out.
//
//   ./build/test_daitex [outdir]        # outdir defaults to /tmp
//
// No renderer, no window, no GPU: a bake is arithmetic on pixels, the same way
// a thumbnail is arithmetic on triangles. What the checks below are about, in
// the order they run:
//
//   [1] the file. A .daitex that is not a graph must be REFUSED with a reason,
//       not baked into something grey - a generator that fails quietly is a
//       generator nobody trusts.
//   [2] determinism, the contract of the whole module. Two bakes of one graph
//       are compared BYTE FOR BYTE, and so are the two PNG files they write:
//       a float that comes out one ulp different is a failure here, not a
//       rounding detail. A changed seed must move the pixels, or the seed is
//       decoration.
//   [3] the nodes, each one against a value worked out by hand rather than
//       against what the code happened to produce: a flat height is exactly
//       (128,128,255), a levels node with its output range inverted is exactly
//       1-x, a blurred constant is still the constant.
//   [4] the four graphs that ship in projects/Untitled - they parse, they
//       bake, and they are DIFFERENT from each other. Two textures that share
//       a digest are one texture with two names.
//   [5] tiling. The maps are projected in world space by the triplanar
//       material, so the left edge has to match the right edge - a seam in the
//       corner of a room is what this check exists to stop.

#include "dai_daitex.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static std::string g_out = "/tmp";

// One graph, spelled out, so a check can say which literal it is about.
static std::string wrap(const char *nodes, const char *out, int res = 32,
                        int seed = 7) {
    char buf[4096];
    std::snprintf(buf, sizeof(buf),
                  "{ \"daitex\": 1, \"resolution\": %d, \"seed\": %d,"
                  " \"nodes\": [ %s ], \"out\": { %s } }", res, seed, nodes, out);
    return buf;
}

static bool bake_text(const std::string &text, daitex::Bake *b, std::string *err) {
    daitex::Graph g;
    if (!daitex::parse(text.c_str(), &g, err)) return false;
    return daitex::bake(g, b);
}

static bool read_all(const std::string &path, std::vector<uint8_t> *out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

int main(int argc, char **argv) {
    if (argc > 1) g_out = argv[1];

    // ---- [1] the file ----------------------------------------------------
    std::printf("[1] what a .daitex is, and what it is not\n");
    {
        daitex::Graph g;
        std::string err;
        CHECK(!daitex::parse("{ \"nodes\": [] }", &g, &err),
              "a JSON object with no \"daitex\" version was accepted");
        CHECK(!err.empty(), "a refused graph came back without a reason");

        err.clear();
        CHECK(!daitex::parse("{ \"daitex\": 1, \"nodes\": [] , \"out\": {} }", &g, &err),
              "a graph with no nodes was accepted");

        err.clear();
        CHECK(!daitex::parse("{ \"daitex\": 1, nodes }", &g, &err),
              "broken JSON was accepted");

        err.clear();
        CHECK(!daitex::parse("{ \"daitex\": 1, \"nodes\": [ { \"type\": \"const\" } ],"
                             " \"out\": { \"base_color\": \"a\" } }", &g, &err),
              "a node with no id was accepted");

        err.clear();
        std::string t = wrap("{ \"id\": \"a\", \"type\": \"const\","
                             " \"params\": { \"value\": 0.25 } }",
                             "\"base_color\": \"a\"", 4000, 3);
        CHECK(daitex::parse(t.c_str(), &g, &err), "a valid graph was refused: %s", err.c_str());
        CHECK(g.resolution == 2048, "resolution 4000 was not clamped to 2048 (got %u)",
              g.resolution);
        CHECK(g.seed == 3, "seed did not survive the parse (%u)", g.seed);
        CHECK(g.nodes.size() == 1 && g.nodes[0].type == "const",
              "the node did not survive the parse");
        CHECK(g.base_color.is_node && g.base_color.node == "a",
              "the base colour output did not name its node");
        CHECK(!g.metallic.is_node && g.metallic.value == 0.0f,
              "an output slot nobody wrote is not the neutral default");

        // A slot may be a NUMBER instead of a node - metal is metal everywhere.
        err.clear();
        std::string t2 = wrap("{ \"id\": \"a\", \"type\": \"const\","
                              " \"params\": { \"value\": 0.25 } }",
                              "\"base_color\": \"a\", \"metallic\": 1.0");
        CHECK(daitex::parse(t2.c_str(), &g, &err) && !g.metallic.is_node &&
              g.metallic.value == 1.0f, "a constant output slot was not read");

        // The span: the inspector writes a slider back into the literal it
        // came from, so the literal has to be found.
        CHECK(g.nodes[0].params.size() == 1 && g.nodes[0].params[0].key == "value",
              "the parameter list is not the file's own");
        const daitex::Param &p = g.nodes[0].params[0];
        CHECK(p.span_len > 0 && g.text.compare(p.span_at, p.span_len, "0.25") == 0,
              "the parameter's span points at \"%s\", not at its literal",
              g.text.substr(p.span_at, p.span_len).c_str());
    }

    // ---- [2] determinism -------------------------------------------------
    std::printf("[2] the same graph twice\n");
    {
        std::string text = wrap(
            "{ \"id\": \"n\", \"type\": \"noise\", \"kind\": \"perlin\","
            "  \"params\": { \"cells\": 6, \"octaves\": 3, \"gain\": 0.5, \"lacunarity\": 2 } },"
            "{ \"id\": \"w\", \"type\": \"noise\", \"kind\": \"worley\","
            "  \"params\": { \"cells\": 9, \"octaves\": 1 } },"
            "{ \"id\": \"m\", \"type\": \"mix\", \"kind\": \"mul\", \"in\": \"n\", \"in_b\": \"w\","
            "  \"params\": { \"factor\": 0.8 } },"
            "{ \"id\": \"c\", \"type\": \"colorramp\", \"in\": \"m\", \"stops\": ["
            "  { \"t\": 0.0, \"rgb\": [ 0.1, 0.2, 0.3 ] },"
            "  { \"t\": 1.0, \"rgb\": [ 0.9, 0.8, 0.4 ] } ] },"
            "{ \"id\": \"nr\", \"type\": \"normal\", \"in\": \"m\", \"params\": { \"strength\": 2 } },"
            "{ \"id\": \"ao\", \"type\": \"ao\", \"in\": \"m\", \"params\": { \"radius\": 4, \"strength\": 1 } }",
            "\"base_color\": \"c\", \"occlusion\": \"ao\", \"roughness\": \"m\","
            " \"metallic\": 0.0, \"normal\": \"nr\"", 64, 12345);

        daitex::Bake a, b;
        std::string err;
        CHECK(bake_text(text, &a, &err), "the reference graph did not bake: %s", err.c_str());
        CHECK(bake_text(text, &b, &err), "the second bake failed: %s", err.c_str());
        CHECK(a.base_color.size() == (size_t)64 * 64 * 4,
              "the base colour is %zu bytes, not 64x64 RGBA8", a.base_color.size());
        CHECK(a.base_color == b.base_color, "two bakes disagree about the base colour");
        CHECK(a.orm == b.orm, "two bakes disagree about the ORM map");
        CHECK(a.normal == b.normal, "two bakes disagree about the normal map");
        CHECK(daitex::digest(a) == daitex::digest(b),
              "two bakes of one graph have different digests (%016llx vs %016llx)",
              (unsigned long long)daitex::digest(a), (unsigned long long)daitex::digest(b));

        // ...and the PNGs on disk, byte for byte. This is the claim the
        // module is sold on, so it is checked on the FILES, not on the buffer
        // the files were written from.
        std::string p1 = g_out + "/daitex_det_a.daitex";
        std::string p2 = g_out + "/daitex_det_b.daitex";
        CHECK(daitex::write_maps(a, p1, &err), "could not write the maps: %s", err.c_str());
        CHECK(daitex::write_maps(b, p2, &err), "could not write the maps: %s", err.c_str());
        const char *suffix[3] = { "_basecolor.png", "_orm.png", "_normal.png" };
        for (int i = 0; i < 3; ++i) {
            std::vector<uint8_t> f1, f2;
            CHECK(read_all(daitex::map_path(p1, suffix[i]), &f1) && !f1.empty(),
                  "%s was not written", suffix[i]);
            CHECK(read_all(daitex::map_path(p2, suffix[i]), &f2),
                  "the second %s was not written", suffix[i]);
            CHECK(f1 == f2, "two bakes wrote different %s bytes (%zu vs %zu)",
                  suffix[i], f1.size(), f2.size());
        }

        // A seed that changes nothing is not a seed.
        std::string other = text;
        size_t at = other.find("12345");
        other.replace(at, 5, "54321");
        daitex::Bake c;
        CHECK(bake_text(other, &c, &err), "the reseeded graph did not bake");
        CHECK(c.base_color != a.base_color, "changing the seed changed no pixel");
        CHECK(c.base_color.size() == a.base_color.size(),
              "changing the seed changed the size of the map");
    }

    // ---- [3] the nodes, against numbers worked out by hand ---------------
    std::printf("[3] one node at a time\n");
    {
        std::string err;
        daitex::Bake b;

        // const: 0.25 * 255 + 0.5 = 64.25 -> 64, in every channel.
        CHECK(bake_text(wrap("{ \"id\": \"a\", \"type\": \"const\","
                             " \"params\": { \"value\": 0.25 } }",
                             "\"base_color\": \"a\", \"roughness\": \"a\""), &b, &err),
              "the const graph did not bake");
        CHECK(b.base_color[0] == 64 && b.base_color[1] == 64 && b.base_color[2] == 64,
              "const 0.25 quantised to (%u,%u,%u), not 64", b.base_color[0],
              b.base_color[1], b.base_color[2]);
        CHECK(b.orm[1] == 64, "the roughness channel of ORM is %u, not 64", b.orm[1]);
        CHECK(b.orm[0] == 255, "an occlusion nobody wrote is %u, not white", b.orm[0]);
        // No normal node: the flat normal, exactly.
        CHECK(b.normal[0] == 128 && b.normal[1] == 128 && b.normal[2] == 255,
              "the default normal is (%u,%u,%u), not flat", b.normal[0], b.normal[1],
              b.normal[2]);
        // ...and an rgb const is a colour, not a grey.
        CHECK(bake_text(wrap("{ \"id\": \"a\", \"type\": \"const\", \"rgb\": [ 1.0, 0.0, 0.5 ] }",
                             "\"base_color\": \"a\""), &b, &err) &&
              b.base_color[0] == 255 && b.base_color[1] == 0 && b.base_color[2] == 128,
              "an rgb const came out (%u,%u,%u)", b.base_color[0], b.base_color[1],
              b.base_color[2]);

        // A flat height through the Sobel: exactly the flat normal. This is
        // the check that catches a sign error in one axis only.
        CHECK(bake_text(wrap("{ \"id\": \"h\", \"type\": \"const\", \"params\": { \"value\": 0.7 } },"
                             "{ \"id\": \"n\", \"type\": \"normal\", \"in\": \"h\","
                             "  \"params\": { \"strength\": 4 } }",
                             "\"normal\": \"n\""), &b, &err),
              "the flat-height normal graph did not bake");
        int flat = 1;
        for (size_t p = 0; p < b.normal.size(); p += 4)
            if (b.normal[p] != 128 || b.normal[p + 1] != 128 || b.normal[p + 2] != 255) flat = 0;
        CHECK(flat, "a flat height did not produce a flat normal map");

        // A ramp along x: the normal must lean along x and NOT along y.
        CHECK(bake_text(wrap("{ \"id\": \"g\", \"type\": \"gradient\", \"kind\": \"x\","
                             "  \"params\": { \"from\": 0, \"to\": 1 } },"
                             "{ \"id\": \"n\", \"type\": \"normal\", \"in\": \"g\","
                             "  \"params\": { \"strength\": 4 } }",
                             "\"base_color\": \"g\", \"normal\": \"n\""), &b, &err),
              "the gradient graph did not bake");
        {
            const uint32_t N = b.size;
            // base colour: (x+0.5)/N, quantised. Checked at two places.
            uint8_t want0 = (uint8_t)(int)(0.5f / (float)N * 255.0f + 0.5f);
            uint8_t wantm = (uint8_t)(int)(((float)(N / 2) + 0.5f) / (float)N * 255.0f + 0.5f);
            CHECK(b.base_color[0] == want0, "gradient x at the left is %u, not %u",
                  b.base_color[0], want0);
            CHECK(b.base_color[(size_t)(N / 2) * 4] == wantm,
                  "gradient x at the middle is %u, not %u", b.base_color[(size_t)(N / 2) * 4],
                  wantm);
            // inside the tile, away from the wrap, the slope is constant
            size_t mid = ((size_t)(N / 2) * N + N / 2) * 4;
            CHECK(b.normal[mid] < 128, "a height rising along +x tilts its normal to %u,"
                  " not below 128", b.normal[mid]);
            CHECK(b.normal[mid + 1] == 128, "a height flat along y tilted the normal's y to %u",
                  b.normal[mid + 1]);
        }

        // levels with the output range turned around is exactly 1-x.
        CHECK(bake_text(wrap("{ \"id\": \"g\", \"type\": \"gradient\", \"kind\": \"x\" },"
                             "{ \"id\": \"l\", \"type\": \"levels\", \"in\": \"g\","
                             "  \"params\": { \"in_min\": 0, \"in_max\": 1,"
                             "                \"out_min\": 1, \"out_max\": 0, \"gamma\": 1 } }",
                             "\"base_color\": \"g\", \"roughness\": \"l\""), &b, &err),
              "the levels graph did not bake");
        {
            int inverted = 1;
            for (size_t p = 0; p < (size_t)b.size; ++p) {
                int sum = (int)b.base_color[p * 4] + (int)b.orm[p * 4 + 1];
                if (sum < 254 || sum > 256) inverted = 0;
            }
            CHECK(inverted, "levels with out_min 1 and out_max 0 is not 1-x");
        }

        // checker 2x2: four quadrants, alternating, exactly 0 and 255.
        CHECK(bake_text(wrap("{ \"id\": \"c\", \"type\": \"checker\","
                             "  \"params\": { \"rows\": 2, \"cols\": 2 } }",
                             "\"base_color\": \"c\""), &b, &err),
              "the checker graph did not bake");
        {
            const uint32_t N = b.size;
            uint8_t tl = b.base_color[0];
            uint8_t tr = b.base_color[(size_t)(N - 1) * 4];
            uint8_t bl = b.base_color[((size_t)(N - 1) * N) * 4];
            CHECK(tl == 0 && tr == 255 && bl == 255,
                  "a 2x2 checker reads %u %u %u across its corners", tl, tr, bl);
        }

        // a blurred constant is the constant - the wrap has no edge to pull in
        CHECK(bake_text(wrap("{ \"id\": \"a\", \"type\": \"const\", \"params\": { \"value\": 0.6 } },"
                             "{ \"id\": \"b\", \"type\": \"blur\", \"in\": \"a\","
                             "  \"params\": { \"radius\": 5, \"passes\": 3 } }",
                             "\"base_color\": \"b\""), &b, &err),
              "the blur graph did not bake");
        {
            int same = 1;
            for (size_t p = 0; p < b.base_color.size(); p += 4)
                if (b.base_color[p] != b.base_color[0]) same = 0;
            CHECK(same && b.base_color[0] == 153,
                  "a blurred constant is not constant (first pixel %u)", b.base_color[0]);
        }

        // ambient occlusion of a flat height is white: nothing occludes.
        CHECK(bake_text(wrap("{ \"id\": \"a\", \"type\": \"const\", \"params\": { \"value\": 0.5 } },"
                             "{ \"id\": \"o\", \"type\": \"ao\", \"in\": \"a\","
                             "  \"params\": { \"radius\": 6, \"strength\": 2 } }",
                             "\"occlusion\": \"o\""), &b, &err),
              "the ao graph did not bake");
        {
            int white = 1;
            for (size_t p = 0; p < b.orm.size(); p += 4) if (b.orm[p] != 255) white = 0;
            CHECK(white, "a flat height occludes itself");
        }
        // ...and a bumpy one is not: the pit next to a ridge goes darker.
        CHECK(bake_text(wrap("{ \"id\": \"c\", \"type\": \"checker\","
                             "  \"params\": { \"rows\": 4, \"cols\": 4 } },"
                             "{ \"id\": \"o\", \"type\": \"ao\", \"in\": \"c\","
                             "  \"params\": { \"radius\": 6, \"strength\": 2 } }",
                             "\"occlusion\": \"o\""), &b, &err),
              "the bumpy ao graph did not bake");
        {
            uint8_t lo = 255, hi = 0;
            for (size_t p = 0; p < b.orm.size(); p += 4) {
                if (b.orm[p] < lo) lo = b.orm[p];
                if (b.orm[p] > hi) hi = b.orm[p];
            }
            CHECK(lo < 200 && hi == 255, "ao over a step is flat (%u..%u)", lo, hi);
        }

        // the mix modes, on two constants, against arithmetic done here
        struct { const char *mode; float want; } MODES[] = {
            { "mix",  0.5f  },      /* lerp(0.2,0.8,0.5)                */
            { "add",  0.6f  },      /* 0.2 + 0.8*0.5                    */
            { "mul",  0.18f },      /* lerp(0.2, 0.16, 0.5)             */
            { "min",  0.2f  },
            { "max",  0.5f  },      /* lerp(0.2, 0.8, 0.5)              */
        };
        for (const auto &m : MODES) {
            char nodes[512];
            std::snprintf(nodes, sizeof(nodes),
                          "{ \"id\": \"a\", \"type\": \"const\", \"params\": { \"value\": 0.2 } },"
                          "{ \"id\": \"b\", \"type\": \"const\", \"params\": { \"value\": 0.8 } },"
                          "{ \"id\": \"m\", \"type\": \"mix\", \"kind\": \"%s\","
                          "  \"in\": \"a\", \"in_b\": \"b\", \"params\": { \"factor\": 0.5 } }",
                          m.mode);
            CHECK(bake_text(wrap(nodes, "\"base_color\": \"m\""), &b, &err),
                  "the %s graph did not bake", m.mode);
            uint8_t want = (uint8_t)(int)(m.want * 255.0f + 0.5f);
            CHECK(b.base_color[0] == want, "mix \"%s\" gave %u, not %u", m.mode,
                  b.base_color[0], want);
        }

        // the colour ramp reads its stops, and at t=0 it IS the first stop
        CHECK(bake_text(wrap("{ \"id\": \"g\", \"type\": \"gradient\", \"kind\": \"x\" },"
                             "{ \"id\": \"c\", \"type\": \"colorramp\", \"in\": \"g\", \"stops\": ["
                             "  { \"t\": 0.0, \"rgb\": [ 0.0, 0.5, 1.0 ] },"
                             "  { \"t\": 1.0, \"rgb\": [ 1.0, 0.5, 0.0 ] } ] }",
                             "\"base_color\": \"c\""), &b, &err),
              "the colorramp graph did not bake");
        {
            const uint32_t N = b.size;
            CHECK(b.base_color[1] == 128, "the ramp's green is %u, not 128 everywhere",
                  b.base_color[1]);
            CHECK(b.base_color[0] < b.base_color[(size_t)(N - 1) * 4],
                  "the ramp does not run from its first stop to its last");
        }

        // brick: the mortar line at the bottom of a course is black
        CHECK(bake_text(wrap("{ \"id\": \"b\", \"type\": \"brick\","
                             "  \"params\": { \"rows\": 4, \"cols\": 2, \"gap\": 0.2,"
                             "                \"shift\": 0.5, \"bevel\": 0.05, \"variation\": 0 } }",
                             "\"base_color\": \"b\""), &b, &err),
              "the brick graph did not bake");
        {
            const uint32_t N = b.size;             /* 32 px, 4 rows -> 8 px per course */
            uint8_t seam = b.base_color[((size_t)(N / 4) * N + N / 4) * 4];      /* y=8: a course boundary */
            uint8_t body = b.base_color[((size_t)(N / 8) * N + N / 4) * 4];      /* y=4: the middle of one */
            CHECK(seam == 0, "the mortar between two courses is %u, not black", seam);
            CHECK(body == 255, "the middle of a brick is %u, not white", body);
        }
    }

    // ---- [4] the four graphs that ship ----------------------------------
    std::printf("[4] the graphs in projects/Untitled/assets/textures\n");
    {
        const char *NAMES[4] = { "raufaser_wand", "pvc_boden", "rostblech", "beton" };
        daitex::Bake bakes[4];
        uint64_t dig[4] = { 0, 0, 0, 0 };
        int ok = 0;
        for (int i = 0; i < 4; ++i) {
            std::string path = std::string("projects/Untitled/assets/textures/") +
                               NAMES[i] + ".daitex";
            daitex::Graph g;
            std::string err;
            if (!daitex::parse_file(path.c_str(), &g, &err)) {
                CHECK(false, "%s did not parse: %s", NAMES[i], err.c_str());
                continue;
            }
            CHECK(g.resolution >= 64, "%s bakes at %u px - too small to look at",
                  NAMES[i], g.resolution);
            CHECK(g.tiling_m > 0.0f, "%s has no tiling size in metres", NAMES[i]);
            // Every number in the file has to be reachable on the slider the
            // inspector gives it - a graph that ships a value its own panel
            // cannot produce is a graph that changes the moment it is touched.
            for (const daitex::Node &nd : g.nodes)
                for (const daitex::Param &pm : nd.params) {
                    float lo, hi, step;
                    daitex::param_range(nd.type.c_str(), pm.key.c_str(), &lo, &hi, &step);
                    CHECK(pm.value >= lo && pm.value <= hi,
                          "%s: %s.%s is %g, outside the slider's %g..%g", NAMES[i],
                          nd.id.c_str(), pm.key.c_str(), (double)pm.value,
                          (double)lo, (double)hi);
                    CHECK(pm.span_len > 0, "%s: %s.%s has no literal to write back to",
                          NAMES[i], nd.id.c_str(), pm.key.c_str());
                }
            CHECK(daitex::bake(g, &bakes[i]), "%s did not bake", NAMES[i]);
            dig[i] = daitex::digest(bakes[i]);
            CHECK(dig[i] != 0, "%s baked to nothing", NAMES[i]);
            ++ok;

            // written, re-read, and identical when baked again
            std::string out = g_out + "/daitex_" + NAMES[i] + ".daitex";
            CHECK(daitex::write_maps(bakes[i], out, &err), "%s: %s", NAMES[i], err.c_str());
            daitex::Bake again;
            CHECK(daitex::bake(g, &again) && daitex::digest(again) == dig[i],
                  "%s does not bake to the same picture twice", NAMES[i]);
        }
        CHECK(ok == 4, "only %d of the four shipped graphs baked", ok);

        // Four graphs, four textures. Two that share a digest are one texture
        // with two names, and the screenshots would show a room with one
        // surface on everything.
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
                CHECK(dig[i] != dig[j], "%s and %s baked to the same pixels",
                      NAMES[i], NAMES[j]);

        // The floor is darker than the wall: this is what "the room shows two
        // different surfaces" means when a machine has to check it.
        auto mean = [](const std::vector<uint8_t> &m) {
            double s = 0;
            for (size_t p = 0; p < m.size(); p += 4) s += m[p] + m[p + 1] + m[p + 2];
            return s / (double)(m.size() / 4 * 3);
        };
        double wall = mean(bakes[0].base_color), floor = mean(bakes[1].base_color);
        CHECK(wall > floor + 40.0, "the Raufaser wall (%.1f) is not clearly brighter than"
              " the PVC floor (%.1f)", wall, floor);

        // Rust: the metallic channel is a MAP, not a number - bare metal where
        // the rust has not taken, dielectric where it has.
        uint8_t mlo = 255, mhi = 0;
        for (size_t p = 0; p < bakes[2].orm.size(); p += 4) {
            uint8_t m = bakes[2].orm[p + 2];
            if (m < mlo) mlo = m;
            if (m > mhi) mhi = m;
        }
        CHECK(mlo < 40 && mhi > 215, "the rust's metallic channel runs %u..%u - it is not"
              " a map", mlo, mhi);

        // Every normal map is a normal map: z up, and x/y centred on 128.
        for (int i = 0; i < 4; ++i) {
            if (bakes[i].size == 0) continue;
            double sx = 0, sy = 0;
            int zlow = 0;
            for (size_t p = 0; p < bakes[i].normal.size(); p += 4) {
                sx += bakes[i].normal[p]; sy += bakes[i].normal[p + 1];
                if (bakes[i].normal[p + 2] < 128) ++zlow;
            }
            size_t n = bakes[i].normal.size() / 4;
            CHECK(std::fabs(sx / (double)n - 128.0) < 4.0 &&
                  std::fabs(sy / (double)n - 128.0) < 4.0,
                  "%s's normal map is not centred (%.1f, %.1f)", NAMES[i],
                  sx / (double)n, sy / (double)n);
            CHECK(zlow == 0, "%s's normal map has %d texels pointing into the surface",
                  NAMES[i], zlow);
        }

        // ---- [5] tiling --------------------------------------------------
        std::printf("[5] the tile matches itself\n");
        for (int i = 0; i < 4; ++i) {
            if (bakes[i].size == 0) continue;
            const uint32_t N = bakes[i].size;
            const std::vector<uint8_t> &m = bakes[i].base_color;
            double seam = 0, inside = 0;
            for (uint32_t y = 0; y < N; ++y) {
                const uint8_t *l = &m[((size_t)y * N + 0) * 4];
                const uint8_t *r = &m[((size_t)y * N + N - 1) * 4];
                const uint8_t *a = &m[((size_t)y * N + N / 3) * 4];
                const uint8_t *b = &m[((size_t)y * N + N / 3 + 1) * 4];
                for (int c = 0; c < 3; ++c) {
                    seam += std::fabs((double)l[c] - (double)r[c]);
                    inside += std::fabs((double)a[c] - (double)b[c]);
                }
            }
            // The wrap is not a special case in this baker - the left column
            // and the right column are neighbours, so their difference must be
            // the difference of any two neighbours and not more.
            CHECK(seam <= inside * 1.6 + 1.0,
                  "%s has a seam: %.0f across the wrap against %.0f between two"
                  " neighbouring columns", NAMES[i], seam, inside);
        }
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
