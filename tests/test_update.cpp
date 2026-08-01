// Self update test - the failure modes matter more than the happy path.
//
// An updater that works when everything goes right is easy. This one is judged
// on what it does when things go wrong: a corrupt download, a manifest naming
// "../../windows/system32/x", a server that is unreachable, a download that
// stops halfway. In every one of those the correct behaviour is that the
// working install stays working. Being out of date is a nuisance; being
// replaced by a broken binary is a support call.
//
// The transport is injected, so the whole flow runs against bytes in memory -
// no server, no network, and the same result every time.
//
// The SHA-256 vectors are checked against lengths 55/56/57/63/64/65/119/120,
// which is where message padding either works or quietly does not: 56 is the
// first length that forces a second block for the length field alone.
//
//   ./build/test_update

#include "dai_update.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

// ---- a server made of a std::vector ---------------------------------------

struct FakeServer {
    std::vector<std::pair<std::string, std::string>> routes;
    int requests = 0;
    int fail_after = -1;          // -1 = always answer

    void put(const std::string &url, const std::string &body) {
        for (auto &r : routes) if (r.first == url) { r.second = body; return; }
        routes.push_back({ url, body });
    }
};

static int fake_fetch(const char *url, void **out_bytes, size_t *out_size, void *user) {
    FakeServer *s = (FakeServer *)user;
    ++s->requests;
    if (s->fail_after >= 0 && s->requests > s->fail_after) return 0;
    for (auto &r : s->routes) {
        if (r.first != url) continue;
        void *m = std::malloc(r.second.size() ? r.second.size() : 1);
        std::memcpy(m, r.second.data(), r.second.size());
        *out_bytes = m;
        *out_size = r.second.size();
        return 1;
    }
    return 0;                     // 404 is "cannot fetch", not a crash
}

static std::string hash_of(const std::string &s) {
    char h[65];
    dai_sha256_hex(s.data(), s.size(), h);
    return h;
}

static bool file_is(const std::string &path, const std::string &want) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string got;
    char c[4096];
    size_t n;
    while ((n = std::fread(c, 1, sizeof(c), f)) > 0) got.append(c, n);
    std::fclose(f);
    return got == want;
}

static bool exists(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}

static void write_file(const std::string &path, const std::string &body) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
}

int main() {
    std::printf("update\n");

    // --- SHA-256 against the published vectors ------------------------------
    {
        struct { const char *in; const char *want; } V[] = {
            { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
            { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
            { "The quick brown fox jumps over the lazy dog",
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592" },
        };
        for (auto &v : V) {
            char h[65];
            dai_sha256_hex(v.in, std::strlen(v.in), h);
            CHECK(!std::strcmp(h, v.want), "sha256(\"%s\") = %s", v.in, h);
        }
        // The padding boundaries, where a wrong implementation still passes
        // "abc" and then produces garbage on a real file.
        struct { size_t n; const char *want; } L[] = {
            { 55,  "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
            { 56,  "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
            { 57,  "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6" },
            { 63,  "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
            { 64,  "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
            { 65,  "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
            { 119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb" },
            { 120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c" },
        };
        for (auto &l : L) {
            std::string s(l.n, 'a');
            char h[65];
            dai_sha256_hex(s.data(), s.size(), h);
            CHECK(!std::strcmp(h, l.want), "sha256(%zu x 'a') = %s", l.n, h);
        }
        std::printf("  sha256: 3 vectors + 8 padding boundaries\n");
    }

    // --- hashing a file, including one that is not there ---------------------
    {
        write_file("/tmp/dai_up_hash.bin", "abc");
        char h[65];
        CHECK(dai_sha256_file("/tmp/dai_up_hash.bin", h) == DAI_OK, "hashing a real file failed");
        CHECK(!std::strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
              "file hash wrong: %s", h);
        CHECK(dai_sha256_file("/tmp/dai_up_nothing_here.bin", h) == DAI_ERR_NOT_FOUND,
              "a missing file should report NOT_FOUND");
        std::remove("/tmp/dai_up_hash.bin");
    }

    // --- version comparison --------------------------------------------------
    {
        CHECK(dai_version_compare("0.10.0", "0.9.9") > 0, "0.10.0 should beat 0.9.9");
        CHECK(dai_version_compare("1.0.0", "1.0.0") == 0, "equal versions should compare equal");
        CHECK(dai_version_compare("1.2", "1.2.0") == 0, "1.2 and 1.2.0 are the same release");
        CHECK(dai_version_compare("2.0.0", "10.0.0") < 0, "10 should beat 2");
        CHECK(dai_version_compare("0.1.0", "") > 0, "anything should beat no version");
        CHECK(dai_version_compare("1.2.3", "1.2.4") < 0, "patch levels should be compared");
        std::printf("  version compare: 0.10.0 > 0.9.9, 10 > 2, 1.2 == 1.2.0\n");
    }

    // --- the whole flow, end to end -----------------------------------------
    const std::string dir = "/tmp/dai_update_test";
    std::string mkdir_cmd = "rm -rf " + dir + " && mkdir -p " + dir;
    if (std::system(mkdir_cmd.c_str()) != 0) { std::printf("  cannot make the test dir\n"); return 1; }

    const std::string exe_old = "MZ...the old editor...";
    const std::string exe_new = "MZ...the new editor, now with fewer bugs...";
    const std::string data_new = "shader bytes";

    FakeServer srv;
    dai_update_set_fetch(fake_fetch, &srv);

    write_file(dir + "/editor.exe", exe_old);

    std::string manifest =
        "{\"version\":\"0.2.0\",\"base_url\":\"http://x/dl/\",\"files\":{"
        "\"editor.exe\":{\"sha256\":\"" + hash_of(exe_new) + "\",\"size\":" +
            std::to_string(exe_new.size()) + "},"
        "\"shaders.pack\":\"" + hash_of(data_new) + "\"}}";
    srv.put("http://x/manifest.json", manifest);
    srv.put("http://x/dl/editor.exe", exe_new);
    srv.put("http://x/dl/shaders.pack", data_new);

    {
        dai_update_info info;
        char err[256] = { 0 };
        CHECK(dai_update_check("http://x/manifest.json", "0.1.0", dir.c_str(), &info,
                               err, sizeof(err)) == DAI_OK, "check failed: %s", err);
        CHECK(info.available == 1, "0.2.0 over 0.1.0 should be available");
        CHECK(info.file_count == 2, "expected 2 files, got %u", info.file_count);
        CHECK(info.needed_count == 2, "both files differ locally, got %u", info.needed_count);
        CHECK(!std::strcmp(info.version, "0.2.0"), "version read as %s", info.version);

        char e2[256] = { 0 };
        CHECK(dai_update_download(&info, dir.c_str(), e2, sizeof(e2)) == 2,
              "download staged the wrong count: %s", e2);
        // Nothing may have moved yet.
        CHECK(file_is(dir + "/editor.exe", exe_old), "the live editor changed before commit");
        CHECK(exists(dir + "/editor.exe.new"), "the new editor was not staged");

        char e3[256] = { 0 };
        CHECK(dai_update_commit(&info, dir.c_str(), "editor.exe", e3, sizeof(e3)) == DAI_OK,
              "commit failed: %s", e3);
        CHECK(file_is(dir + "/editor.exe", exe_new), "the new editor is not in place");
        CHECK(file_is(dir + "/shaders.pack", data_new), "the pack is not in place");
        CHECK(file_is(dir + "/editor.exe.old", exe_old),
              "the running editor should have been renamed aside, not deleted");
        CHECK(!exists(dir + "/editor.exe.new"), "the staged copy should be gone");
        std::printf("  flow: 2 files staged, committed, running exe renamed aside\n");
    }

    // --- running it again finds nothing to do --------------------------------
    {
        dai_update_info info;
        char err[256] = { 0 };
        CHECK(dai_update_check("http://x/manifest.json", "0.2.0", dir.c_str(), &info,
                               err, sizeof(err)) == DAI_OK, "second check failed: %s", err);
        CHECK(info.available == 0, "0.2.0 should not be newer than itself");
        CHECK(info.needed_count == 0, "nothing should be needed, %u were", info.needed_count);
        std::printf("  second run: nothing to do\n");
    }

    // --- a corrupt download is refused ---------------------------------------
    {
        std::string manifest2 =
            "{\"version\":\"0.3.0\",\"base_url\":\"http://x/dl/\",\"files\":{"
            "\"editor.exe\":\"" + hash_of("what the manifest promises") + "\"}}";
        srv.put("http://x/manifest.json", manifest2);
        srv.put("http://x/dl/editor.exe", "what the server actually sends");

        dai_update_info info;
        char err[256] = { 0 };
        dai_update_check("http://x/manifest.json", "0.2.0", dir.c_str(), &info, err, sizeof(err));
        CHECK(info.needed_count == 1, "the changed hash should mark the file needed");

        char e2[256] = { 0 };
        CHECK(dai_update_download(&info, dir.c_str(), e2, sizeof(e2)) == 0,
              "a hash mismatch must not be staged");
        CHECK(std::strstr(e2, "hash") != nullptr, "the refusal should mention the hash: %s", e2);
        CHECK(file_is(dir + "/editor.exe", exe_new), "a bad download changed the live editor");
        CHECK(!exists(dir + "/editor.exe.new"), "a bad download left a staged file behind");
        std::printf("  corrupt download: refused, install untouched\n");
    }

    // --- an unreachable server is not an error, just no update ---------------
    {
        dai_update_info info;
        char err[256] = { 0 };
        CHECK(dai_update_check("http://x/does_not_exist.json", "0.2.0", dir.c_str(), &info,
                               err, sizeof(err)) != DAI_OK, "a 404 manifest should not succeed");
        CHECK(file_is(dir + "/editor.exe", exe_new), "a failed check changed the install");
        std::printf("  unreachable server: reported, install untouched\n");
    }

    // --- a download that stops halfway leaves the install alone --------------
    {
        const std::string a = "file a body", b = "file b body";
        std::string m =
            "{\"version\":\"0.4.0\",\"base_url\":\"http://x/dl/\",\"files\":{"
            "\"one.dat\":\"" + hash_of(a) + "\",\"two.dat\":\"" + hash_of(b) + "\"}}";
        srv.put("http://x/manifest.json", m);
        srv.put("http://x/dl/one.dat", a);
        srv.put("http://x/dl/two.dat", b);

        dai_update_info info;
        char err[256] = { 0 };
        dai_update_check("http://x/manifest.json", "0.2.0", dir.c_str(), &info, err, sizeof(err));
        CHECK(info.needed_count == 2, "two new files should be needed");

        srv.requests = 0;
        srv.fail_after = 1;               // the second download dies
        char e2[256] = { 0 };
        CHECK(dai_update_download(&info, dir.c_str(), e2, sizeof(e2)) == 0,
              "a partial download should report failure");
        srv.fail_after = -1;

        // Commit must refuse rather than install half of it.
        char e3[256] = { 0 };
        CHECK(dai_update_commit(&info, dir.c_str(), nullptr, e3, sizeof(e3)) != DAI_OK,
              "commit should refuse when a file was never staged");
        CHECK(!exists(dir + "/two.dat"), "a file was installed despite the failure");
        std::printf("  interrupted download: commit refuses, install stays consistent\n");
        std::remove((dir + "/one.dat.new").c_str());
    }

    // --- a manifest that tries to escape the install directory ---------------
    {
        std::string evil =
            "{\"version\":\"9.9.9\",\"base_url\":\"http://x/dl/\",\"files\":{"
            "\"../escaped.txt\":\"" + hash_of("x") + "\","
            "\"/etc/passwd\":\"" + hash_of("x") + "\","
            "\"sub/dir.txt\":\"" + hash_of("x") + "\","
            "\"fine.txt\":\"" + hash_of("fine") + "\"}}";
        srv.put("http://x/manifest.json", evil);
        srv.put("http://x/dl/fine.txt", "fine");

        dai_update_info info;
        char err[256] = { 0 };
        dai_update_check("http://x/manifest.json", "0.2.0", dir.c_str(), &info, err, sizeof(err));
        CHECK(info.file_count == 1, "only the safe name should have survived, %u did",
              info.file_count);
        if (info.file_count == 1)
            CHECK(!std::strcmp(info.files[0].name, "fine.txt"), "kept the wrong name: %s",
                  info.files[0].name);
        std::printf("  path traversal in the manifest: dropped\n");
    }

    // --- a manifest with no hash, and one that is not JSON at all ------------
    {
        srv.put("http://x/manifest.json",
                "{\"version\":\"9.9.9\",\"base_url\":\"http://x/dl/\","
                "\"files\":{\"x.dat\":\"tooshort\"}}");
        dai_update_info info;
        char err[256] = { 0 };
        dai_update_check("http://x/manifest.json", "0.2.0", dir.c_str(), &info, err, sizeof(err));
        CHECK(info.file_count == 0, "a file without a real hash should be dropped");

        srv.put("http://x/manifest.json", "this is not json {{{");
        char e2[256] = { 0 };
        CHECK(dai_update_check("http://x/manifest.json", "0.2.0", dir.c_str(), &info,
                               e2, sizeof(e2)) != DAI_OK, "broken JSON should not succeed");
        CHECK(e2[0] != 0, "a refusal should say why");

        srv.put("http://x/manifest.json", "{\"base_url\":\"http://x/dl/\"}");
        char e3[256] = { 0 };
        CHECK(dai_update_check("http://x/manifest.json", "0.2.0", dir.c_str(), &info,
                               e3, sizeof(e3)) != DAI_OK, "a manifest without a version is invalid");
        std::printf("  malformed manifests: rejected with a reason\n");
    }

    dai_update_set_fetch(nullptr, nullptr);
    std::string rm = "rm -rf " + dir;
    if (std::system(rm.c_str()) != 0) std::printf("  (could not clean %s)\n", dir.c_str());

    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "ok", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
