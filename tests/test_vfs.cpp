// The virtual file system and the archive format.
//
//   ./build/test_vfs
//
// This test links against src/dai_vfs.cpp, src/dai_export.cpp and the project
// layer - no engine, no renderer, no interface. The archive layer has to stand
// on its own: the packer is a BUILD TOOL and must not need a GPU, and the
// loader runs before anything else in a shipped game exists.
//
// What is actually being checked, in the order the bugs would appear:
//
//   the format      magic, version, 4 byte alignment, the trailer offset - the
//                   things a reader gets wrong once and then reads garbage
//   appending       the runtime in front of the archive must come back BYTE
//                   IDENTICAL, or the exported game is a corrupted program
//   path safety     "..", absolute paths, backslashes. A scene file is data,
//                   and data must not be able to name /etc/shadow
//   priority        a patch folder over a shipped pack, and the other way
//   round trips     boot config, and every payload against its hash
//   the project     what dai_pack_build_from_dir packs and what it skips

#include "dai_vfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

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
static bool spit(const std::string &p, const std::string &text) {
    FILE *f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    if (!text.empty()) std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return true;
}
static std::string vfs_str(const char *path) {
    size_t n = 0;
    void *b = dai_vfs_read(path, &n);
    if (!b) return std::string("<missing>");
    std::string s((const char *)b, n);
    dai_vfs_free(b);
    return s;
}

int main(void) {
    std::string root = "/tmp/dai_vfs_test";
    std::string rm = "rm -rf " + root;
    if (std::system(rm.c_str()) != 0) { /* first run: nothing to remove */ }
    if (std::system(("mkdir -p " + root + "/proj/scenes " + root + "/proj/assets/models " +
                     root + "/proj/settings " + root + "/proj/cache").c_str()) != 0) {
        std::printf("cannot make the test tree\n");
        return 1;
    }

    // ---- a project on disk -------------------------------------------------
    spit(root + "/proj/scenes/main.daidalos", "daidalos-scene 1\n\nnode 1\n  name Ground\nend\n");
    spit(root + "/proj/scenes/level2.daidalos", "daidalos-scene 1\n");
    spit(root + "/proj/assets/spin.js", "function init(){}\nfunction frame(){}\n");
    spit(root + "/proj/assets/models/crate.txt", std::string(5000, 'x'));   // odd size on purpose
    spit(root + "/proj/settings/project.txt", "daidalos-project-settings 1\n");
    spit(root + "/proj/project.daidalos", "daidalos-project 1\nname Test\n");
    spit(root + "/proj/cache/derived.bin", "SHOULD NOT BE PACKED");
    spit(root + "/proj/.hidden", "SHOULD NOT BE PACKED");
    spit(root + "/proj/assets/.dotfile", "SHOULD NOT BE PACKED");

    // ---- boot config round trip -------------------------------------------
    {
        dai_boot_config c = dai_boot_config_default();
        std::snprintf(c.scene, sizeof(c.scene), "scenes/level2.daidalos");
        std::snprintf(c.title, sizeof(c.title), "My Game");
        c.width = 1600; c.height = 900; c.fullscreen = 1; c.msaa = 8;
        c.tick_hz = 120; c.max_bodies = 999; c.physics_backend = 2;
        c.gravity[0] = 1.5f; c.gravity[1] = -3.25f; c.gravity[2] = 0.5f;
        char text[1024];
        size_t need = dai_boot_config_write(&c, text, sizeof(text));
        CHECK(need > 0 && need < sizeof(text), "boot config wants %zu bytes", need);

        dai_boot_config back = dai_boot_config_default();
        CHECK(dai_boot_config_parse(text, std::strlen(text), &back) == DAI_OK,
              "the boot config it just wrote did not parse");
        CHECK(std::strcmp(back.scene, c.scene) == 0, "scene came back '%s'", back.scene);
        CHECK(std::strcmp(back.title, c.title) == 0, "title came back '%s'", back.title);
        CHECK(back.width == 1600 && back.height == 900, "size came back %dx%d", back.width, back.height);
        CHECK(back.fullscreen == 1 && back.msaa == 8, "flags came back %d/%d", back.fullscreen, back.msaa);
        CHECK(back.tick_hz == 120 && back.max_bodies == 999, "sim came back %d/%d",
              back.tick_hz, back.max_bodies);
        CHECK(back.physics_backend == 2, "backend came back %d", back.physics_backend);
        CHECK(back.gravity[1] == -3.25f, "gravity came back %g", (double)back.gravity[1]);

        // A newer exporter writing one more line must not stop an older
        // runtime: unknown keys are skipped, the opposite of a scene file.
        std::string plus = std::string(text) + "hdr_pipeline 3\nvsync 1\n";
        dai_boot_config fwd = dai_boot_config_default();
        CHECK(dai_boot_config_parse(plus.c_str(), plus.size(), &fwd) == DAI_OK,
              "an unknown boot key was rejected");
        CHECK(fwd.width == 1600, "an unknown key ate a known one");

        // Something that is not a boot config at all must say so.
        const char *junk = "hello\nworld\n";
        dai_boot_config bad = dai_boot_config_default();
        CHECK(dai_boot_config_parse(junk, std::strlen(junk), &bad) != DAI_OK,
              "a file with no header was accepted as a boot config");
        std::printf("  boot config: round trips, tolerates new keys, rejects junk\n");
    }

    // ---- build a bare archive ---------------------------------------------
    std::string dpk = root + "/game.dpk";
    {
        char err[512] = { 0 };
        dai_boot_config c = dai_boot_config_default();
        std::snprintf(c.title, sizeof(c.title), "Packed");
        uint32_t n = dai_pack_build_from_dir((root + "/proj").c_str(), dpk.c_str(), nullptr,
                                             &c, err, sizeof(err));
        CHECK(n > 0, "build_from_dir failed: %s", err);
        CHECK(file_exists(dpk), "no archive at %s", dpk.c_str());

        dai_pack_info info;
        std::memset(&info, 0, sizeof(info));
        dai_pack_entry e[32];
        uint32_t got = dai_pack_read(dpk.c_str(), &info, e, 32, err, sizeof(err));
        CHECK(got == n, "read back %u entries, packed %u", got, n);
        CHECK(info.base == 0, "a bare archive should start at 0, starts at %llu",
              (unsigned long long)info.base);
        CHECK(info.version == DAI_PACK_VERSION, "version %u", info.version);
        CHECK(info.archive_bytes == info.file_bytes,
              "a bare archive should BE the file: %u vs %llu",
              info.archive_bytes, (unsigned long long)info.file_bytes);

        // Alignment is a promise the format makes, and the only way anyone
        // finds out it was broken is a crash on a platform that cares.
        int misaligned = 0, sorted = 1;
        for (uint32_t i = 0; i < got; ++i) {
            if (e[i].offset % DAI_PACK_ALIGN) ++misaligned;
            if (i && std::strcmp(e[i - 1].path, e[i].path) >= 0) sorted = 0;
        }
        CHECK(misaligned == 0, "%d payloads are not 4 byte aligned", misaligned);
        CHECK(sorted == 1, "the directory is not sorted - a binary search would miss");

        // What went in, and what did NOT.
        int have_boot = 0, have_scene = 0, have_js = 0, have_deep = 0, have_cache = 0, have_dot = 0;
        for (uint32_t i = 0; i < got; ++i) {
            if (!std::strcmp(e[i].path, DAI_PACK_BOOT_PATH)) have_boot = 1;
            if (!std::strcmp(e[i].path, "scenes/main.daidalos")) have_scene = 1;
            if (!std::strcmp(e[i].path, "assets/spin.js")) have_js = 1;
            if (!std::strcmp(e[i].path, "assets/models/crate.txt")) have_deep = 1;
            if (std::strstr(e[i].path, "cache/")) have_cache = 1;
            if (std::strstr(e[i].path, ".hidden") || std::strstr(e[i].path, ".dotfile")) have_dot = 1;
        }
        CHECK(have_boot, "no %s in the archive", DAI_PACK_BOOT_PATH);
        CHECK(have_scene, "the startup scene is not in the archive");
        CHECK(have_js, "assets/spin.js is not in the archive");
        CHECK(have_deep, "assets/models/crate.txt (a subfolder) is not in the archive");
        CHECK(!have_cache, "cache/ was packed - it is derived data by definition");
        CHECK(!have_dot, "a dotfile was packed");

        CHECK(dai_pack_verify(dpk.c_str(), err, sizeof(err)) == 0, "verify: %s", err);
        std::printf("  archive: %u entries, %u bytes, aligned, sorted, hashes match\n",
                    got, info.archive_bytes);
    }

    // ---- refusing to build something broken --------------------------------
    {
        char err[512] = { 0 };
        dai_boot_config c = dai_boot_config_default();
        std::snprintf(c.scene, sizeof(c.scene), "scenes/does_not_exist.daidalos");
        uint32_t n = dai_pack_build_from_dir((root + "/proj").c_str(), (root + "/bad.dpk").c_str(),
                                             nullptr, &c, err, sizeof(err));
        CHECK(n == 0, "an export with no startup scene was allowed");
        CHECK(err[0] != 0, "it failed without saying why");
        CHECK(!file_exists(root + "/bad.dpk"), "a failed export left a file behind");

        // A template that is not there is the most common export failure, and
        // it has to name the thing that is missing.
        std::snprintf(c.scene, sizeof(c.scene), "scenes/main.daidalos");
        n = dai_pack_build_from_dir((root + "/proj").c_str(), (root + "/bad2.dpk").c_str(),
                                    "/nope/not/a/runtime", &c, err, sizeof(err));
        CHECK(n == 0 && err[0], "a missing runtime template was accepted");
        std::printf("  refuses: missing startup scene, missing template - and leaves nothing\n");
    }

    // ---- appending to a "runtime" -----------------------------------------
    // The bytes in front MUST come back identical. This is the check that
    // matters most: everything else being right and this being wrong means
    // shipping a program that no longer runs.
    std::string fake_rt = root + "/runtime.bin";
    std::string game = root + "/MyGame";
    std::string rt_bytes;
    {
        rt_bytes.reserve(9999);
        for (int i = 0; i < 9999; ++i) rt_bytes += (char)(i * 7 + 3);   // not 4 byte aligned
        spit(fake_rt, rt_bytes);

        char err[512] = { 0 };
        dai_boot_config c = dai_boot_config_default();
        std::snprintf(c.title, sizeof(c.title), "Appended");
        uint32_t n = dai_pack_build_from_dir((root + "/proj").c_str(), game.c_str(),
                                             fake_rt.c_str(), &c, err, sizeof(err));
        CHECK(n > 0, "export failed: %s", err);

        std::string out = slurp(game);
        CHECK(out.size() > rt_bytes.size(), "the export is not bigger than the runtime");
        CHECK(out.compare(0, rt_bytes.size(), rt_bytes) == 0,
              "the runtime bytes were changed by the append");

        dai_pack_info info;
        std::memset(&info, 0, sizeof(info));
        uint32_t got = dai_pack_read(game.c_str(), &info, nullptr, 0, err, sizeof(err));
        CHECK(got == n, "the appended archive lists %u of %u entries", got, n);
        CHECK(info.base >= rt_bytes.size(), "the archive starts inside the runtime (%llu < %zu)",
              (unsigned long long)info.base, rt_bytes.size());
        CHECK(info.base - rt_bytes.size() < DAI_PACK_ALIGN, "too much padding: %llu bytes",
              (unsigned long long)(info.base - rt_bytes.size()));
        CHECK(info.base % DAI_PACK_ALIGN == 0, "the archive does not start on a 4 byte boundary");
        CHECK(info.base + info.archive_bytes == info.file_bytes,
              "the archive does not reach the end of the file");
        CHECK(dai_pack_verify(game.c_str(), err, sizeof(err)) == 0, "verify: %s", err);

        // The trailer, read the way the loader reads it: backwards.
        std::string tail = out.substr(out.size() - DAI_PACK_TRAILER_BYTES);
        CHECK(std::memcmp(tail.data() + 8, DAI_PACK_TRAILER_MAGIC, 8) == 0,
              "the last 8 bytes are not %s", DAI_PACK_TRAILER_MAGIC);
        uint64_t base_from_tail = 0;
        for (int i = 7; i >= 0; --i) base_from_tail = (base_from_tail << 8) | (uint8_t)tail[i];
        CHECK(base_from_tail == info.base, "the trailer offset %llu is not the header offset %llu",
              (unsigned long long)base_from_tail, (unsigned long long)info.base);
        CHECK(std::memcmp(out.data() + info.base, DAI_PACK_MAGIC, 8) == 0,
              "no %s where the trailer points", DAI_PACK_MAGIC);
        std::printf("  append: runtime intact, archive at %llu, trailer points at it\n",
                    (unsigned long long)info.base);
    }

    // ---- reading through the VFS ------------------------------------------
    {
        char err[512] = { 0 };
        dai_vfs_unmount_all();
        CHECK(dai_vfs_mount_archive(game.c_str(), 0, err, sizeof(err)) == DAI_OK,
              "cannot mount the exported game: %s", err);
        CHECK(dai_vfs_mount_count() == 1, "%u mounts", dai_vfs_mount_count());
        CHECK(vfs_str("scenes/main.daidalos").find("name Ground") != std::string::npos,
              "the scene did not come back out of the archive");
        CHECK(vfs_str("assets/models/crate.txt").size() == 5000,
              "the odd sized payload came back wrong");
        CHECK(dai_vfs_exists("assets/spin.js"), "spin.js is not there");
        CHECK(!dai_vfs_exists("assets/nope.js"), "a file that does not exist reported as there");
        CHECK(dai_vfs_size("assets/models/crate.txt") == 5000, "size is %lld",
              (long long)dai_vfs_size("assets/models/crate.txt"));

        // The buffer is NUL terminated one past the end, so a scene file can be
        // handed straight to a parser that expects a C string.
        size_t n = 0;
        char *b = (char *)dai_vfs_read("assets/spin.js", &n);
        CHECK(b && b[n] == 0, "the read buffer is not NUL terminated");
        dai_vfs_free(b);

        // Paths a scene file must never be able to name.
        CHECK(!dai_vfs_read("../etc/passwd", nullptr), "'..' escaped the mount");
        CHECK(!dai_vfs_read("/etc/passwd", nullptr), "an absolute path was accepted");
        CHECK(!dai_vfs_read("assets/../../x", nullptr), "an inner '..' escaped the mount");
        CHECK(!dai_vfs_read("", nullptr), "an empty path was accepted");
        CHECK(!dai_vfs_read(nullptr, nullptr), "a null path was accepted");
        // Windows separators mean the same file - a project authored there has
        // to load here.
        CHECK(vfs_str("assets\\spin.js") == vfs_str("assets/spin.js"),
              "a backslash path did not resolve to the same file");
        CHECK(vfs_str("./assets/spin.js") == vfs_str("assets/spin.js"),
              "a leading './' broke the lookup");

        // An archive is not a directory: nothing can hand out a real path for
        // something that only exists inside a file.
        char real[512];
        CHECK(!dai_vfs_real_path("assets/spin.js", real, sizeof(real)),
              "an archived file claimed a path on disk: %s", real);
        std::printf("  vfs: reads from the archive, refuses '..', '/', and lies about paths\n");
    }

    // ---- mount priority ----------------------------------------------------
    {
        char err[512] = { 0 };
        std::system(("mkdir -p " + root + "/patch/assets").c_str());
        spit(root + "/patch/assets/spin.js", "PATCHED\n");

        dai_vfs_unmount_all();
        CHECK(dai_vfs_mount_archive(game.c_str(), 0, err, sizeof(err)) == DAI_OK, "mount pack: %s", err);
        CHECK(dai_vfs_mount_dir((root + "/patch").c_str(), 10) == DAI_OK, "mount patch dir");
        CHECK(vfs_str("assets/spin.js") == "PATCHED\n",
              "the higher priority folder did not win: '%s'", vfs_str("assets/spin.js").c_str());
        CHECK(vfs_str("scenes/main.daidalos").find("name Ground") != std::string::npos,
              "a file only the pack has stopped resolving once a folder was mounted");
        // And the real path IS available now, because that copy is on disk.
        char real[512] = { 0 };
        CHECK(dai_vfs_real_path("assets/spin.js", real, sizeof(real)) &&
              std::string(real).find("/patch/") != std::string::npos,
              "real_path did not point at the patch copy: '%s'", real);

        // The other way round: the shipped pack wins when it is mounted higher.
        dai_vfs_unmount_all();
        CHECK(dai_vfs_mount_dir((root + "/patch").c_str(), 0) == DAI_OK, "mount patch low");
        CHECK(dai_vfs_mount_archive(game.c_str(), 10, err, sizeof(err)) == DAI_OK, "mount pack high");
        CHECK(vfs_str("assets/spin.js").find("function init") != std::string::npos,
              "priority is not being honoured in both directions");

        // Listing is the union, sorted and de-duplicated: the same file in a
        // folder and in a pack is ONE entry, because that is what it is.
        uint32_t total = dai_vfs_list(nullptr, 0, 0);
        std::vector<char> buf((size_t)total * DAI_VFS_PATH_MAX);
        uint32_t got = dai_vfs_list(buf.data(), total, DAI_VFS_PATH_MAX);
        CHECK(got == total, "list said %u then wrote %u", total, got);
        int spin_seen = 0, sorted = 1;
        for (uint32_t i = 0; i < got; ++i) {
            const char *p = buf.data() + (size_t)i * DAI_VFS_PATH_MAX;
            if (!std::strcmp(p, "assets/spin.js")) ++spin_seen;
            if (i && std::strcmp(buf.data() + (size_t)(i - 1) * DAI_VFS_PATH_MAX, p) >= 0) sorted = 0;
        }
        CHECK(spin_seen == 1, "assets/spin.js is listed %d times across two mounts", spin_seen);
        CHECK(sorted, "the listing is not sorted");
        dai_vfs_unmount_all();
        std::printf("  mounts: priority wins in both directions, listing is a sorted union\n");
    }

    // ---- an empty and a corrupt archive ------------------------------------
    {
        char err[512] = { 0 };
        // A file with no archive is the NORMAL state of an unexported runtime,
        // and it has to fail cleanly rather than read garbage.
        CHECK(dai_vfs_mount_archive(fake_rt.c_str(), 0, err, sizeof(err)) != DAI_OK,
              "a file with no archive mounted anyway");
        CHECK(err[0] != 0, "it failed silently");

        // A truncated export: everything but the last byte.
        std::string good = slurp(game);
        spit(root + "/cut", good.substr(0, good.size() - 1));
        CHECK(dai_vfs_mount_archive((root + "/cut").c_str(), 0, err, sizeof(err)) != DAI_OK,
              "a truncated archive mounted");

        // A flipped byte inside a payload: mounts fine (the directory is
        // intact) and verify is what catches it. That split is deliberate -
        // starting a game must not cost a full re-hash of every asset.
        std::string bent = good;
        dai_pack_info info;
        std::memset(&info, 0, sizeof(info));
        dai_pack_entry e[32];
        uint32_t got = dai_pack_read(game.c_str(), &info, e, 32, err, sizeof(err));
        CHECK(got > 0, "could not read the archive back to corrupt it");
        size_t victim = 0;
        for (uint32_t i = 0; i < got; ++i)
            if (e[i].length > 16) { victim = (size_t)e[i].offset + 4; break; }
        CHECK(victim != 0, "no payload big enough to corrupt");
        bent[victim] = (char)(bent[victim] ^ 0xFF);
        spit(root + "/bent", bent);
        CHECK(dai_vfs_mount_archive((root + "/bent").c_str(), 0, err, sizeof(err)) == DAI_OK,
              "a bent payload stopped the archive mounting - it should only fail verify");
        dai_vfs_unmount_all();
        CHECK(dai_pack_verify((root + "/bent").c_str(), err, sizeof(err)) > 0,
              "verify did not notice a flipped byte");
        std::printf("  damage: no archive / truncated -> refused, flipped byte -> caught by verify\n");
    }

    // ---- the writer's own rules --------------------------------------------
    {
        char err[512] = { 0 };
        dai_pack *p = dai_pack_begin((root + "/manual.dpk").c_str(), nullptr, err, sizeof(err));
        CHECK(p != nullptr, "pack_begin: %s", err);
        CHECK(dai_pack_add_mem(p, "a.txt", "hello", 5) == 1, "add_mem failed");
        CHECK(dai_pack_add_mem(p, "a.txt", "again", 5) == 0, "a duplicate path was accepted");
        // Once it has failed it stays failed: a half written archive that
        // reports success is worse than one that reports nothing.
        CHECK(dai_pack_end(p, err, sizeof(err)) != DAI_OK, "a failed pack still finished");
        CHECK(!file_exists(root + "/manual.dpk"), "a failed pack left a file");

        p = dai_pack_begin((root + "/manual.dpk").c_str(), nullptr, err, sizeof(err));
        CHECK(p != nullptr, "pack_begin (2): %s", err);
        CHECK(dai_pack_add_mem(p, "../escape", "x", 1) == 0, "'..' was packed");
        dai_pack_abort(p);

        p = dai_pack_begin((root + "/manual.dpk").c_str(), nullptr, err, sizeof(err));
        dai_pack_add_mem(p, "empty.bin", "", 0);
        dai_pack_add_mem(p, "one.bin", "\x01", 1);
        dai_pack_add_mem(p, "big.bin", std::string(1234, 'z').c_str(), 1234);
        CHECK(dai_pack_end(p, err, sizeof(err)) == DAI_OK, "pack_end: %s", err);
        CHECK(dai_pack_verify((root + "/manual.dpk").c_str(), err, sizeof(err)) == 0,
              "verify on hand built archive: %s", err);
        dai_vfs_unmount_all();
        dai_vfs_mount_archive((root + "/manual.dpk").c_str(), 0, err, sizeof(err));
        CHECK(dai_vfs_exists("empty.bin"), "a zero length entry vanished");
        CHECK(dai_vfs_size("empty.bin") == 0, "a zero length entry has a size");
        CHECK(vfs_str("big.bin").size() == 1234, "the 1234 byte entry came back wrong");
        dai_vfs_unmount_all();
        std::printf("  writer: rejects duplicates and '..', keeps zero length entries\n");
    }

    if (std::system(rm.c_str()) != 0) std::printf("  (could not clean %s)\n", root.c_str());
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "ok", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
