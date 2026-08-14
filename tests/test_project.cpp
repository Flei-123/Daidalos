// Projects, project settings and preferences.
//
//   ./build/test_project
//
// This test links against src/dai_project.cpp and NOTHING else - no engine, no
// renderer, no interface. That is not thrift, it is the claim being tested: the
// project picker is on screen before any of those exist, so the project layer
// has to stand on its own or the editor cannot start.
//
// The rest is what actually breaks in a settings system: a name that escapes
// its directory, a float that comes back slightly different, a key from a
// newer build being silently deleted by an older one, and a missing file being
// treated as an error instead of "nothing changed yet".

#include "dai_project.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static bool dir_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static std::string slurp(const std::string &p) {
    std::string out;
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}
static bool append(const std::string &p, const char *text) {
    FILE *f = std::fopen(p.c_str(), "ab");
    if (!f) return false;
    std::fputs(text, f);
    std::fclose(f);
    return true;
}
static bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
// Bit exact, not "close enough": a settings round trip that drifts by one ulp
// per save is a game whose physics changes because someone opened a dialog.
static bool same_bits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
// The same for the show origin, which is a double because the seventh decimal
// of a degree is a centimetre and a show is flown over a real field.
static bool same_double_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
// Writes a string the way a caller should: zero the field first. snprintf alone
// leaves the tail of a shorter replacement behind ("Player" overwritten with
// "Enemy" keeps the 'r'), and the whole point below is comparing structs byte
// for byte.
static void set_str(char *dst, size_t cap, const char *text) {
    std::memset(dst, 0, cap);
    std::snprintf(dst, cap, "%s", text);
}

int main() {
    std::printf("project + settings\n");

    const std::string root = "/tmp/dai_project_test";
    std::string rm = "rm -rf " + root;
    if (std::system(rm.c_str()) != 0) std::printf("  (could not clean %s first)\n", root.c_str());

    // Preferences must never touch the real ~/.config while a test runs.
    const std::string prefs_dir = root + "/prefs_home";
    setenv("DAI_PREFS_DIR", prefs_dir.c_str(), 1);

    char err[256] = { 0 };

    // ---- 1. creating a project makes the whole layout ---------------------
    dai_project *p = dai_project_create(root.c_str(), "My Game", err, sizeof(err));
    CHECK(p != nullptr, "create failed: %s", err);
    if (!p) { std::printf("cannot continue\n"); return 1; }

    const std::string path = dai_project_path(p);
    CHECK(path == root + "/My Game", "path is '%s'", path.c_str());
    CHECK(std::strcmp(dai_project_name(p), "My Game") == 0, "name is '%s'", dai_project_name(p));

    CHECK(dir_exists(path + "/assets"),   "assets/ missing");
    CHECK(dir_exists(path + "/scenes"),   "scenes/ missing");
    CHECK(dir_exists(path + "/settings"), "settings/ missing");
    CHECK(dir_exists(path + "/cache"),    "cache/ missing");
    CHECK(file_exists(path + "/project.daidalos"), "marker file missing");
    CHECK(file_exists(path + "/scenes/main.daidalos"), "startup scene missing");
    CHECK(file_exists(path + "/settings/project.txt"), "settings file missing");
    CHECK(std::strcmp(dai_project_scene_path(p), (path + "/scenes/main.daidalos").c_str()) == 0,
          "scene path is '%s'", dai_project_scene_path(p));
    CHECK(std::strcmp(dai_project_asset_dir(p), (path + "/assets").c_str()) == 0,
          "asset dir is '%s'", dai_project_asset_dir(p));

    // The marker says who made it and when, so a folder found in five years
    // can still answer the question.
    std::string marker = slurp(path + "/project.daidalos");
    CHECK(has(marker, "daidalos-project 1"), "marker has no header: %s", marker.c_str());
    CHECK(has(marker, "name My Game"), "marker lost the name");
    CHECK(has(marker, "engine "), "marker has no engine version");
    CHECK(has(marker, "created "), "marker has no creation date");
    std::printf("  layout, marker and startup scene: all present\n");

    // A new scene must be loadable, not merely present - an empty file is
    // refused by the scene loader.
    CHECK(has(slurp(path + "/scenes/main.daidalos"), "daidalos-scene"),
          "the startup scene is not a scene file");

    // ---- 2. is_valid ------------------------------------------------------
    CHECK(dai_project_is_valid(path.c_str()) == 1, "a project it made is not valid");
    CHECK(dai_project_is_valid((path + "/").c_str()) == 1, "trailing slash broke validation");
    CHECK(dai_project_is_valid(root.c_str()) == 0, "the containing folder is not a project");
    CHECK(dai_project_is_valid("/tmp/dai_project_test/nope") == 0, "a missing path is not a project");
    CHECK(dai_project_is_valid(nullptr) == 0, "NULL is not a project");
    CHECK(dai_project_is_valid("") == 0, "an empty path is not a project");
    // A folder with only some of the three is NOT a project: half an answer to
    // "where do settings live" is no answer.
    CHECK(std::system(("mkdir -p '" + root + "/half/assets' '" + root + "/half/scenes'").c_str()) == 0,
          "could not build the half project fixture");
    CHECK(dai_project_is_valid((root + "/half").c_str()) == 0, "assets+scenes without settings passed");
    std::printf("  is_valid: three directories, no more, no less\n");

    // ---- 3. names that must be refused ------------------------------------
    // Every one of these is a path component in the making.
    const char *bad[] = { "", "..", ".", "/", "a/b", "a\\b", "..\\..\\windows",
                          "../../etc", " leading", "trailing ", "con", "NUL", "COM1",
                          "star*", "quote\"", "colon:name", "new\nline", "tab\there",
                          "Ümlaut",
                          "way_too_long_0123456789012345678901234567890123456789012345678901234567890" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(dai_project_name_valid(bad[i]) == 0, "name '%s' should be refused", bad[i]);
        char e2[256] = { 0 };
        dai_project *r = dai_project_create(root.c_str(), bad[i], e2, sizeof(e2));
        CHECK(r == nullptr, "create accepted the name '%s'", bad[i]);
        CHECK(e2[0] != 0, "a refusal of '%s' should say why", bad[i]);
        if (r) dai_project_close(r);
    }
    CHECK(dai_project_name_valid(nullptr) == 0, "NULL is not a name");
    CHECK(!dir_exists(root + "/a"), "'a/b' created a directory anyway");
    CHECK(!dir_exists(root + "/con"), "a reserved device name created a directory");
    const char *good[] = { "a", "My Game", "level-2_final", "Untitled 42" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i)
        CHECK(dai_project_name_valid(good[i]) == 1, "name '%s' should be accepted", good[i]);
    std::printf("  %d bad names refused, none of them created anything\n",
                (int)(sizeof(bad) / sizeof(bad[0])));

    // ---- 4. creating over an existing project is refused ------------------
    {
        char e2[256] = { 0 };
        dai_project *again = dai_project_create(root.c_str(), "My Game", e2, sizeof(e2));
        CHECK(again == nullptr, "create overwrote an existing project");
        CHECK(e2[0] != 0, "refusing to overwrite should say so");
        if (again) dai_project_close(again);
        CHECK(file_exists(path + "/project.daidalos"), "the failed attempt damaged the project");
    }

    // ---- 5. settings round trip, bit exact --------------------------------
    dai_project_settings d = dai_project_settings_default();
    CHECK(same_bits(d.gravity[1], -9.81f), "default gravity is %f", d.gravity[1]);
    CHECK(d.tick_hz == 60 && d.max_bodies == 8192, "defaults do not match the engine's");
    CHECK(std::strcmp(d.tags[0], "Untagged") == 0, "tag 0 must be Untagged, is '%s'", d.tags[0]);
    CHECK(std::strcmp(d.layers[0], "Default") == 0, "layer 0 must be Default, is '%s'", d.layers[0]);

    dai_project_settings s = d;
    s.gravity[0] = 0.1f;
    s.gravity[1] = -3.711f;              // Mars, and a number that needs 7 digits
    s.gravity[2] = 1.17549435e-38f;      // smallest normal float
    s.tick_hz = 128;
    s.max_bodies = 65535;
    // 2 was Jolt. The backend is gone, and loading a project written back
    // then must not select a solver nothing answers to - it comes back as
    // Talos on purpose. Every other field still round trips byte for byte.
    s.physics_backend = 2;
    s.default_friction = 0.123456789f;
    s.default_restitution = 3.4028235e38f;   // FLT_MAX
    set_str(s.app_name, sizeof(s.app_name), "My Game Deluxe Edition");
    set_str(s.default_scene, sizeof(s.default_scene), "scenes/level 2.daidalos");
    set_str(s.tags[1], sizeof(s.tags[1]), "Enemy");
    set_str(s.tags[7], sizeof(s.tags[7]), "Pickup Item");
    set_str(s.tags[2], sizeof(s.tags[2]), "");   // cleared on purpose: must stay cleared
    set_str(s.layers[9], sizeof(s.layers[9]), "Water Surface");
    // The show half, with the values that actually break: a minimum distance
    // that needs seven digits, a latitude no float can hold, a seed that fills
    // 64 bits and a drone count past the point where a signed int stops being
    // enough to think with.
    s.min_distance_m       = 2.7182817f;
    s.v_max_ms             = 11.5f;
    s.a_max_ms2            = 3.25f;
    s.drone_count          = 10000u;
    s.show_origin_lat      = 47.4979123456789;   // Budapest, to the centimetre
    s.show_origin_lon      = -19.0402198765432;  // and west of Greenwich
    s.show_origin_amsl     = 102.75f;
    s.show_orientation_deg = 137.5f;
    s.takeoff_alt_m        = 12.5f;
    s.fps                  = 25;
    s.fence_half_x         = 333.5f;
    s.fence_half_z         = 444.25f;
    s.fence_top_m          = 119.5f;
    s.min_ground_m         = 1.5f;
    s.show_seed            = 0xFEEDFACECAFEBEEFull;

    CHECK(dai_project_settings_save(p, &s) == DAI_OK, "settings save failed");
    CHECK(file_exists(path + "/settings/project.txt"), "settings file vanished");

    dai_project_settings back{};
    CHECK(dai_project_settings_load(p, &back) == DAI_OK, "settings load failed");
    for (int i = 0; i < 3; ++i)
        CHECK(same_bits(back.gravity[i], s.gravity[i]),
              "gravity[%d] %.9g != %.9g", i, back.gravity[i], s.gravity[i]);
    CHECK(back.tick_hz == s.tick_hz, "tick_hz %d", back.tick_hz);
    CHECK(back.max_bodies == s.max_bodies, "max_bodies %d", back.max_bodies);
    CHECK(back.physics_backend == DAI_PHYSICS_TALOS, "legacy Jolt did not fall back to Talos: %d", back.physics_backend);
    CHECK(same_bits(back.default_friction, s.default_friction),
          "friction %.9g != %.9g", back.default_friction, s.default_friction);
    CHECK(same_bits(back.default_restitution, s.default_restitution),
          "restitution %.9g != %.9g", back.default_restitution, s.default_restitution);
    CHECK(std::strcmp(back.app_name, s.app_name) == 0, "app_name '%s'", back.app_name);
    CHECK(std::strcmp(back.default_scene, s.default_scene) == 0, "default_scene '%s'", back.default_scene);
    CHECK(std::strcmp(back.tags[1], "Enemy") == 0, "tag 1 '%s'", back.tags[1]);
    CHECK(std::strcmp(back.tags[7], "Pickup Item") == 0, "tag 7 with a space: '%s'", back.tags[7]);
    CHECK(back.tags[2][0] == 0, "a cleared tag came back as '%s'", back.tags[2]);
    CHECK(std::strcmp(back.layers[9], "Water Surface") == 0, "layer 9 '%s'", back.layers[9]);
    // The show settings are the ones a safety case rests on, so "close enough"
    // is not a result: a minimum distance that drifts by an ulp per save is a
    // number nobody can be held to.
    CHECK(same_bits(back.min_distance_m, s.min_distance_m),
          "min_distance_m %.9g != %.9g", back.min_distance_m, s.min_distance_m);
    CHECK(same_bits(back.v_max_ms, s.v_max_ms), "v_max_ms %.9g", back.v_max_ms);
    CHECK(same_bits(back.a_max_ms2, s.a_max_ms2), "a_max_ms2 %.9g", back.a_max_ms2);
    CHECK(back.drone_count == s.drone_count, "drone_count %u", back.drone_count);
    CHECK(same_double_bits(back.show_origin_lat, s.show_origin_lat),
          "show_origin_lat %.17g != %.17g", back.show_origin_lat, s.show_origin_lat);
    CHECK(same_double_bits(back.show_origin_lon, s.show_origin_lon),
          "show_origin_lon %.17g != %.17g", back.show_origin_lon, s.show_origin_lon);
    CHECK(same_bits(back.show_origin_amsl, s.show_origin_amsl), "show_origin_amsl %.9g", back.show_origin_amsl);
    CHECK(same_bits(back.show_orientation_deg, s.show_orientation_deg),
          "show_orientation_deg %.9g", back.show_orientation_deg);
    CHECK(same_bits(back.takeoff_alt_m, s.takeoff_alt_m), "takeoff_alt_m %.9g", back.takeoff_alt_m);
    CHECK(back.fps == s.fps, "fps %d", back.fps);
    CHECK(same_bits(back.fence_half_x, s.fence_half_x), "fence_half_x %.9g", back.fence_half_x);
    CHECK(same_bits(back.fence_half_z, s.fence_half_z), "fence_half_z %.9g", back.fence_half_z);
    CHECK(same_bits(back.fence_top_m, s.fence_top_m), "fence_top_m %.9g", back.fence_top_m);
    CHECK(same_bits(back.min_ground_m, s.min_ground_m), "min_ground_m %.9g", back.min_ground_m);
    CHECK(back.show_seed == s.show_seed, "show_seed %llu != %llu",
          (unsigned long long)back.show_seed, (unsigned long long)s.show_seed);
    {   // ...and everything else byte for byte. The backend is compared above:
        // it is the one field that is deliberately not preserved.
        dai_project_settings expect = s;
        expect.physics_backend = DAI_PHYSICS_TALOS;
        CHECK(std::memcmp(&back, &expect, sizeof(expect)) == 0,
              "the struct did not survive the round trip byte for byte");
    }
    std::printf("  settings round trip: byte identical, floats included\n");

    // Changing default_scene must move what the editor opens at startup.
    CHECK(std::strcmp(dai_project_scene_path(p), (path + "/scenes/level 2.daidalos").c_str()) == 0,
          "scene path did not follow default_scene: '%s'", dai_project_scene_path(p));

    // ---- 6. only what differs from the default is written -----------------
    {
        std::string text = slurp(path + "/settings/project.txt");
        CHECK(has(text, "daidalos-project-settings 1"), "settings file has no header");
        CHECK(has(text, "tick-hz 128"), "the changed tick rate is not in the file");
        CHECK(has(text, "tag 1 Enemy"), "the changed tag is not in the file");
        CHECK(!has(text, "layer 0 "), "an unchanged layer was written anyway");
        CHECK(!has(text, "tag 3 "), "an unchanged tag was written anyway");
        CHECK(has(text, "show-drone-count 10000"), "the fleet size is not in the file");
        CHECK(has(text, "show-origin-lat 47.4979123456789"),
              "the show origin lost precision on the way to the file: %s", text.c_str());
        CHECK(has(text, "show-seed 18369614221190020847"), "the seed is not in the file");

        // And the other direction: pure defaults produce a file with no values,
        // which is what keeps a diff about the one thing that actually changed.
        CHECK(dai_project_settings_save(p, &d) == DAI_OK, "saving the defaults failed");
        std::string bare = slurp(path + "/settings/project.txt");
        CHECK(!has(bare, "tick-hz"), "a default value was written: %s", bare.c_str());
        CHECK(!has(bare, "gravity"), "a default gravity was written");
        // The point of the whole "only differences" rule, restated for the new
        // half: adding a second project type must not add a single line to a
        // game's settings file.
        CHECK(!has(bare, "show-"), "a game project grew a drone show setting: %s", bare.c_str());
        CHECK(dai_project_settings_save(p, &s) == DAI_OK, "restoring the settings failed");
        std::printf("  only non default values reach the file\n");
    }

    // ---- 7. a key from a newer build: skipped, and kept ---------------------
    {
        CHECK(append(path + "/settings/project.txt",
                     "shadow-cascades 4\nrender-scale 0.75\ntag 900 FromTheFuture\n"), "append failed");
        dai_project_settings fwd{};
        CHECK(dai_project_settings_load(p, &fwd) == DAI_OK,
              "an unknown key must not fail the load");
        s.physics_backend = DAI_PHYSICS_TALOS;   // see above: never round trips
        CHECK(std::memcmp(&fwd, &s, sizeof(s)) == 0, "an unknown key disturbed the known values");

        CHECK(dai_project_settings_save(p, &fwd) == DAI_OK, "save after an unknown key failed");
        std::string text = slurp(path + "/settings/project.txt");
        CHECK(has(text, "shadow-cascades 4"), "an older build deleted a newer build's key");
        CHECK(has(text, "render-scale 0.75"), "an older build deleted a newer build's key");
        CHECK(has(text, "tag 900 FromTheFuture"), "a tag slot beyond this table was dropped");

        // And saving twice must not stack another copy of them onto the file.
        dai_project_settings twice{};
        dai_project_settings_load(p, &twice);
        dai_project_settings_save(p, &twice);
        std::string again = slurp(path + "/settings/project.txt");
        size_t first = again.find("shadow-cascades");
        CHECK(first != std::string::npos &&
              again.find("shadow-cascades", first + 1) == std::string::npos,
              "preserved keys are multiplying on every save");
        std::printf("  unknown keys: read past, written back, never duplicated\n");
    }

    // ---- 8. a missing settings file is not an error -----------------------
    {
        CHECK(std::remove((path + "/settings/project.txt").c_str()) == 0, "could not remove the file");
        dai_project_settings none{};
        CHECK(dai_project_settings_load(p, &none) == DAI_OK,
              "a missing settings file must yield the defaults, not an error");
        CHECK(std::memcmp(&none, &d, sizeof(d)) == 0, "the defaults did not come back");

        // A file that is not ours at all IS an error - but the caller still
        // gets a usable struct rather than uninitialised memory.
        FILE *f = std::fopen((path + "/settings/project.txt").c_str(), "wb");
        CHECK(f != nullptr, "could not write the junk fixture");
        if (f) { std::fputs("<xml>nope</xml>\n", f); std::fclose(f); }
        dai_project_settings junk{};
        CHECK(dai_project_settings_load(p, &junk) != DAI_OK, "a foreign file should be refused");
        CHECK(std::memcmp(&junk, &d, sizeof(d)) == 0, "a refused load must still leave the defaults");
        CHECK(dai_project_settings_save(p, &s) == DAI_OK, "could not restore the settings");
        std::printf("  missing file -> defaults, foreign file -> refused with defaults\n");
    }

    // ---- 9. reopening, and cache/ being disposable ------------------------
    dai_project_close(p);
    p = nullptr;
    {
        CHECK(std::system(("rm -rf '" + path + "/cache'").c_str()) == 0, "could not delete cache/");
        CHECK(!dir_exists(path + "/cache"), "cache/ is still there");
        CHECK(dai_project_is_valid(path.c_str()) == 1, "deleting cache/ must not invalidate a project");

        char e2[256] = { 0 };
        dai_project *re = dai_project_open(path.c_str(), e2, sizeof(e2));
        CHECK(re != nullptr, "reopen failed: %s", e2);
        if (re) {
            CHECK(dir_exists(path + "/cache"), "open did not put cache/ back");
            CHECK(std::strcmp(dai_project_name(re), "My Game") == 0,
                  "the name did not come back from the marker: '%s'", dai_project_name(re));
            CHECK(std::strcmp(dai_project_scene_path(re), (path + "/scenes/level 2.daidalos").c_str()) == 0,
                  "reopen forgot the startup scene: '%s'", dai_project_scene_path(re));
            dai_project_close(re);
        }
        std::printf("  cache/ deleted and reopened: repaired, project intact\n");
    }
    {
        char e2[256] = { 0 };
        CHECK(dai_project_open((root + "/half").c_str(), e2, sizeof(e2)) == nullptr,
              "opened something that is not a project");
        CHECK(e2[0] != 0, "refusing to open should say why");
        CHECK(dai_project_open("/tmp/dai_project_test/does-not-exist", e2, sizeof(e2)) == nullptr,
              "opened a path that does not exist");
        CHECK(dai_project_open(nullptr, e2, sizeof(e2)) == nullptr, "opened NULL");
    }

    // ---- 10. listing what is in a folder ----------------------------------
    {
        char e2[256] = { 0 };
        dai_project *b = dai_project_create(root.c_str(), "Zebra", e2, sizeof(e2));
        dai_project *c = dai_project_create(root.c_str(), "Alpha", e2, sizeof(e2));
        CHECK(b && c, "could not create the extra projects: %s", e2);
        if (b) dai_project_close(b);
        if (c) dai_project_close(c);

        CHECK(dai_project_list(root.c_str(), nullptr, 0, 0) == 3,
              "counting found %u projects, expected 3", dai_project_list(root.c_str(), nullptr, 0, 0));

        char paths[8][256];
        uint32_t n = dai_project_list(root.c_str(), &paths[0][0], 8, 256);
        CHECK(n == 3, "list returned %u, expected 3 (half/ and prefs_home/ are not projects)", n);
        if (n == 3) {
            // Sorted, so the picker does not reshuffle itself between runs.
            CHECK(std::strcmp(paths[0], (root + "/Alpha").c_str()) == 0, "first is '%s'", paths[0]);
            CHECK(std::strcmp(paths[1], (root + "/My Game").c_str()) == 0, "second is '%s'", paths[1]);
            CHECK(std::strcmp(paths[2], (root + "/Zebra").c_str()) == 0, "third is '%s'", paths[2]);
            for (uint32_t i = 0; i < n; ++i)
                CHECK(dai_project_is_valid(paths[i]) == 1, "listed '%s' is not openable", paths[i]);
        }
        // A smaller buffer must fill what it can, not run off the end.
        char two[2][64];
        CHECK(dai_project_list(root.c_str(), &two[0][0], 2, 64) == 2, "max was not respected");
        CHECK(std::strcmp(two[0], (root + "/Alpha").c_str()) == 0, "truncated list lost its order");
        CHECK(dai_project_list("/tmp/dai_project_test/nothing-here", &paths[0][0], 8, 256) == 0,
              "listing a missing folder should find nothing");
        std::printf("  list: %u projects, sorted, junk folders ignored\n", n);
    }

    // ---- 11. preferences: outside the project, and per machine ------------
    {
        const char *pp = dai_prefs_path();
        CHECK(pp && pp[0], "prefs path is empty");
        CHECK(std::string(pp) == prefs_dir + "/prefs.txt", "prefs path is '%s'", pp);
        CHECK(!has(std::string(pp), path.c_str()), "preferences must NOT live inside the project");

        dai_prefs pd = dai_prefs_default();
        CHECK(same_bits(pd.ui_scale, 1.0f), "default ui_scale is %f", pd.ui_scale);
        CHECK(pd.last_project[0] == 0, "a fresh install has no last project");

        dai_prefs none{};
        CHECK(dai_prefs_load(&none) == DAI_OK, "a first start has no prefs file and that is fine");
        CHECK(std::memcmp(&none, &pd, sizeof(pd)) == 0, "the defaults did not come back");

        dai_prefs w = pd;
        w.ui_scale = 1.7999999f;
        w.theme = 1;
        w.cam_speed = 12.25f;
        w.gizmo_px = 96.5f;
        w.snap_translate = 0.25f;
        w.snap_rotate_deg = 22.5f;
        w.autosave_seconds = 0;
        set_str(w.last_project, sizeof(w.last_project), path.c_str());
        CHECK(dai_prefs_save(&w) == DAI_OK, "prefs save failed (%s)", dai_prefs_path());
        CHECK(file_exists(prefs_dir + "/prefs.txt"), "prefs file was not created");

        dai_prefs r{};
        CHECK(dai_prefs_load(&r) == DAI_OK, "prefs load failed");
        CHECK(std::memcmp(&r, &w, sizeof(w)) == 0, "prefs did not survive the round trip");
        CHECK(same_bits(r.ui_scale, w.ui_scale), "ui_scale %.9g != %.9g", r.ui_scale, w.ui_scale);
        CHECK(r.autosave_seconds == 0, "autosave 0 (off) was lost: %d", r.autosave_seconds);
        // This one line is why the editor can reopen what you were working on.
        CHECK(std::strcmp(r.last_project, path.c_str()) == 0, "last_project is '%s'", r.last_project);

        char e2[256] = { 0 };
        dai_project *last = dai_project_open(r.last_project, e2, sizeof(e2));
        CHECK(last != nullptr, "the remembered project does not open: %s", e2);
        if (last) dai_project_close(last);
        std::printf("  prefs round trip, and last_project reopens\n");
    }

    // ---- 12. where preferences land when nothing overrides ----------------
    {
        // The documented Linux rule, checked rather than assumed: the editor
        // ships to two platforms and only one of them is the one it is built on.
        unsetenv("DAI_PREFS_DIR");
        std::string xdg = root + "/xdg";
        setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
        CHECK(std::string(dai_prefs_path()) == xdg + "/daidalos/prefs.txt",
              "XDG_CONFIG_HOME ignored: '%s'", dai_prefs_path());

        unsetenv("XDG_CONFIG_HOME");
        std::string home = root + "/home";
        setenv("HOME", home.c_str(), 1);
        CHECK(std::string(dai_prefs_path()) == home + "/.config/daidalos/prefs.txt",
              "~/.config fallback is wrong: '%s'", dai_prefs_path());

        // And saving there must create the directory chain on a fresh account.
        dai_prefs w = dai_prefs_default();
        w.theme = 1;
        CHECK(dai_prefs_save(&w) == DAI_OK, "could not save into a config dir that did not exist");
        CHECK(file_exists(home + "/.config/daidalos/prefs.txt"), "the config directory was not created");
        std::printf("  prefs path: $DAI_PREFS_DIR > $XDG_CONFIG_HOME > ~/.config\n");
    }

    // ---- 13. the second project type --------------------------------------
    {
        char e2[256] = { 0 };
        dai_project *g = dai_project_open(path.c_str(), e2, sizeof(e2));
        CHECK(g != nullptr, "reopen for the kind check failed: %s", e2);
        if (g) {
            CHECK(dai_project_kind(g) == DAI_PROJECT_GAME,
                  "a project made by dai_project_create is not a game: %d", dai_project_kind(g));
            dai_project_close(g);
        }
        CHECK(dai_project_kind(nullptr) == DAI_PROJECT_GAME, "NULL must answer, not crash");

        const std::string show_path = root + "/Night Show";
        dai_project *ds = dai_project_create_kind(root.c_str(), "Night Show",
                                                  DAI_PROJECT_DRONESHOW, e2, sizeof(e2));
        CHECK(ds != nullptr, "creating a droneshow project failed: %s", e2);
        if (ds) {
            CHECK(dai_project_kind(ds) == DAI_PROJECT_DRONESHOW,
                  "the kind did not come back from create: %d", dai_project_kind(ds));
            CHECK(std::string(dai_project_path(ds)) == show_path, "path is '%s'", dai_project_path(ds));
            // Same layout, same engine, same everything else - this is a second
            // question, not a second program.
            CHECK(dir_exists(show_path + "/assets") && dir_exists(show_path + "/scenes") &&
                  dir_exists(show_path + "/settings"), "a droneshow project has a different layout");
            dai_project_close(ds);
        }
        // The word, not a number: project.daidalos is read in a diff far more
        // often than by the parser.
        CHECK(has(slurp(show_path + "/project.daidalos"), "kind droneshow"),
              "the marker does not say what kind of project this is");

        dai_project *re = dai_project_open(show_path.c_str(), e2, sizeof(e2));
        CHECK(re != nullptr, "reopening the droneshow project failed: %s", e2);
        if (re) {
            CHECK(dai_project_kind(re) == DAI_PROJECT_DRONESHOW,
                  "the kind did not survive being closed and reopened: %d", dai_project_kind(re));
            // A fresh show project starts on the documented defaults, and the
            // one number the safety case rests on is checked by name.
            dai_project_settings ss{};
            CHECK(dai_project_settings_load(re, &ss) == DAI_OK, "show settings load failed");
            CHECK(same_bits(ss.min_distance_m, dai_project_settings_default().min_distance_m),
                  "a new show project does not start on the default spacing: %.9g", ss.min_distance_m);
            dai_project_close(re);
        }

        // ---- and the whole point: a marker written before this field existed.
        // Every project on disk today looks like this, and every one of them
        // has to keep opening as exactly what it was.
        FILE *f = std::fopen((show_path + "/project.daidalos").c_str(), "wb");
        CHECK(f != nullptr, "could not write the legacy marker fixture");
        if (f) {
            std::fputs("daidalos-project 1\nname Night Show\nengine 0.1.0\n"
                       "created 2024-01-01T00:00:00Z\n", f);
            std::fclose(f);
        }
        dai_project *old = dai_project_open(show_path.c_str(), e2, sizeof(e2));
        CHECK(old != nullptr, "a marker without a kind line must still open: %s", e2);
        if (old) {
            CHECK(dai_project_kind(old) == DAI_PROJECT_GAME,
                  "a missing kind line must mean game, got %d", dai_project_kind(old));
            CHECK(std::strcmp(dai_project_name(old), "Night Show") == 0,
                  "the legacy marker lost the name: '%s'", dai_project_name(old));
            dai_project_close(old);
        }

        // A kind this build has never heard of is not an error either: the
        // project opens plainly rather than not at all.
        f = std::fopen((show_path + "/project.daidalos").c_str(), "wb");
        if (f) {
            std::fputs("daidalos-project 1\nname Night Show\nkind hologram\n", f);
            std::fclose(f);
        }
        dai_project *fut = dai_project_open(show_path.c_str(), e2, sizeof(e2));
        CHECK(fut != nullptr, "an unknown kind must not stop a project opening: %s", e2);
        if (fut) {
            CHECK(dai_project_kind(fut) == DAI_PROJECT_GAME,
                  "an unknown kind must fall back to game, got %d", dai_project_kind(fut));
            dai_project_close(fut);
        }
        std::printf("  kinds: droneshow round trips, no kind line means game\n");
    }

    // ---- 14. show settings from a newer build survive an older one ---------
    {
        // The forward compatibility rule again, aimed at the field that will
        // grow next: a show setting this build does not know must come back out
        // of the file untouched, or a colleague's newer editor loses the number
        // the moment somebody opens the project here.
        char e2[256] = { 0 };
        dai_project *sp = dai_project_open((root + "/Night Show").c_str(), e2, sizeof(e2));
        CHECK(sp != nullptr, "could not reopen the show project: %s", e2);
        if (sp) {
            dai_project_settings ss = dai_project_settings_default();
            ss.drone_count    = 2400u;
            ss.min_distance_m = 2.5f;
            ss.show_seed      = 7ull;
            CHECK(dai_project_settings_save(sp, &ss) == DAI_OK, "show settings save failed");
            CHECK(append(root + "/Night Show/settings/project.txt",
                         "show-wind-limit-ms 9.5\n"), "append failed");

            dai_project_settings rd{};
            CHECK(dai_project_settings_load(sp, &rd) == DAI_OK, "load past an unknown show key failed");
            CHECK(std::memcmp(&rd, &ss, sizeof(ss)) == 0,
                  "an unknown show key disturbed the known values");
            CHECK(dai_project_settings_save(sp, &rd) == DAI_OK, "save after the unknown key failed");

            std::string text = slurp(root + "/Night Show/settings/project.txt");
            CHECK(has(text, "show-wind-limit-ms 9.5"),
                  "this build deleted a newer build's show setting");
            CHECK(has(text, "show-drone-count 2400"), "the fleet size did not survive");
            CHECK(has(text, "show-seed 7"), "the seed did not survive");
            CHECK(!has(text, "show-fps"), "a default show value was written anyway");
            dai_project_close(sp);
        }
        std::printf("  show settings: only what changed, unknown keys handed back\n");
    }

    if (std::system(rm.c_str()) != 0) std::printf("  (could not clean %s)\n", root.c_str());
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "ok", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
