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
    s.physics_backend = 2;
    s.default_friction = 0.123456789f;
    s.default_restitution = 3.4028235e38f;   // FLT_MAX
    set_str(s.app_name, sizeof(s.app_name), "My Game Deluxe Edition");
    set_str(s.default_scene, sizeof(s.default_scene), "scenes/level 2.daidalos");
    set_str(s.tags[1], sizeof(s.tags[1]), "Enemy");
    set_str(s.tags[7], sizeof(s.tags[7]), "Pickup Item");
    set_str(s.tags[2], sizeof(s.tags[2]), "");   // cleared on purpose: must stay cleared
    set_str(s.layers[9], sizeof(s.layers[9]), "Water Surface");

    CHECK(dai_project_settings_save(p, &s) == DAI_OK, "settings save failed");
    CHECK(file_exists(path + "/settings/project.txt"), "settings file vanished");

    dai_project_settings back{};
    CHECK(dai_project_settings_load(p, &back) == DAI_OK, "settings load failed");
    for (int i = 0; i < 3; ++i)
        CHECK(same_bits(back.gravity[i], s.gravity[i]),
              "gravity[%d] %.9g != %.9g", i, back.gravity[i], s.gravity[i]);
    CHECK(back.tick_hz == s.tick_hz, "tick_hz %d", back.tick_hz);
    CHECK(back.max_bodies == s.max_bodies, "max_bodies %d", back.max_bodies);
    CHECK(back.physics_backend == s.physics_backend, "physics_backend %d", back.physics_backend);
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
    CHECK(std::memcmp(&back, &s, sizeof(s)) == 0, "the struct did not survive the round trip byte for byte");
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

        // And the other direction: pure defaults produce a file with no values,
        // which is what keeps a diff about the one thing that actually changed.
        CHECK(dai_project_settings_save(p, &d) == DAI_OK, "saving the defaults failed");
        std::string bare = slurp(path + "/settings/project.txt");
        CHECK(!has(bare, "tick-hz"), "a default value was written: %s", bare.c_str());
        CHECK(!has(bare, "gravity"), "a default gravity was written");
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

    if (std::system(rm.c_str()) != 0) std::printf("  (could not clean %s)\n", root.c_str());
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "ok", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
