// dai_pack - build, inspect and verify a Daidalos archive.
//
//   dai_pack build  <project-dir> <out.dpk>            just the archive
//   dai_pack export <project-dir> <runtime> <out.exe>  runtime + archive
//   dai_pack list   <file>                             the directory table
//   dai_pack verify <file>                             re-hash every payload
//   dai_pack cat    <file> <path>                      one entry to stdout
//
// Options for build/export:
//   --scene <path>     startup scene, relative to the project
//                      (default: settings/default_scene, else scenes/main.daidalos)
//   --title <text>     window title       --width / --height <n>
//   --fullscreen       start full screen  --msaa <n>
//   --tick <hz>        simulation rate    --gravity <x> <y> <z>
//
// `export` is exactly what dai_project_export does, from a shell - the same
// two functions, so a bug here is a bug there.
//
// The archive format is documented in include/dai_vfs.h.

#include "dai_vfs.h"
#include "dai_project.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int usage(void) {
    std::printf(
        "dai_pack - Daidalos archives\n"
        "\n"
        "  dai_pack build  <project-dir> <out.dpk>             archive only\n"
        "  dai_pack export <project-dir> <runtime> <out.exe>   runtime + archive\n"
        "  dai_pack list   <file>                              directory table\n"
        "  dai_pack verify <file>                              check every hash\n"
        "  dai_pack cat    <file> <path>                       one entry to stdout\n"
        "\n"
        "build/export options:\n"
        "  --scene <path>   --title <text>   --width <n>   --height <n>\n"
        "  --fullscreen     --msaa <n>       --tick <hz>   --max-bodies <n>\n"
        "  --physics <0|1|2>  (0 Talos, 1 none, 2 Jolt)\n"
        "  --gravity <x> <y> <z>\n");
    return 2;
}

// Reads <project>/settings/project.txt through the project layer, so the
// command line default matches what the editor would open. A directory that
// is not a project is not an error here - it is packed as it stands.
void defaults_from_project(const char *dir, dai_boot_config *cfg) {
    if (!dai_project_is_valid(dir)) return;
    char err[256];
    dai_project *p = dai_project_open(dir, err, sizeof(err));
    if (!p) return;
    dai_project_settings s = dai_project_settings_default();
    dai_project_settings_load(p, &s);
    if (s.default_scene[0]) std::snprintf(cfg->scene, sizeof(cfg->scene), "%s", s.default_scene);
    const char *title = s.app_name[0] ? s.app_name : dai_project_name(p);
    if (title && *title) std::snprintf(cfg->title, sizeof(cfg->title), "%s", title);
    if (s.tick_hz > 0) cfg->tick_hz = s.tick_hz;
    if (s.max_bodies > 0) cfg->max_bodies = s.max_bodies;
    cfg->physics_backend = s.physics_backend;
    cfg->gravity[0] = s.gravity[0];
    cfg->gravity[1] = s.gravity[1];
    cfg->gravity[2] = s.gravity[2];
    // The language the project previews in is the one the game starts in.
    // Anything else and the editor is showing a build nobody will play.
    std::snprintf(cfg->language, sizeof(cfg->language), "%s", s.language);
    dai_project_close(p);
    for (size_t i = 0; cfg->scene[i]; ++i) if (cfg->scene[i] == '\\') cfg->scene[i] = '/';
}

const char *human(uint64_t bytes, char *buf, size_t n) {
    const char *unit[] = { "B", "kB", "MB", "GB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    std::snprintf(buf, n, u == 0 ? "%.0f %s" : "%.1f %s", v, unit[u]);
    return buf;
}

int cmd_list(const char *file) {
    char err[512] = { 0 };
    dai_pack_info info;
    std::memset(&info, 0, sizeof(info));
    uint32_t n = dai_pack_read(file, &info, nullptr, 0, err, sizeof(err));
    if (!n && err[0]) { std::printf("dai_pack: %s\n", err); return 1; }

    std::vector<dai_pack_entry> e((size_t)n);
    if (n) dai_pack_read(file, &info, e.data(), n, err, sizeof(err));

    char b1[32], b2[32], b3[32];
    std::printf("%s\n", file);
    std::printf("  file        %s (%llu bytes)\n", human(info.file_bytes, b1, sizeof(b1)),
                (unsigned long long)info.file_bytes);
    std::printf("  archive at  %llu%s\n", (unsigned long long)info.base,
                info.base ? "  (appended - there is a program in front of it)" : "  (bare archive)");
    std::printf("  archive     %s, version %u, %u entries\n",
                human(info.archive_bytes, b2, sizeof(b2)), info.version, info.entry_count);
    std::printf("  payload     %s\n", human(info.payload_bytes, b3, sizeof(b3)));
    std::printf("\n  %-52s %10s %12s %10s\n", "path", "bytes", "offset", "hash");
    for (uint32_t i = 0; i < n; ++i)
        std::printf("  %-52s %10u %12llu   %08x\n", e[i].path, e[i].length,
                    (unsigned long long)e[i].offset, e[i].hash);
    return 0;
}

int cmd_verify(const char *file) {
    char err[512] = { 0 };
    uint32_t bad = dai_pack_verify(file, err, sizeof(err));
    if (bad == 0xFFFFFFFFu) { std::printf("dai_pack: %s\n", err); return 1; }
    if (bad) { std::printf("dai_pack: %u bad entries (%s)\n", bad, err); return 1; }
    dai_pack_info info;
    std::memset(&info, 0, sizeof(info));
    dai_pack_read(file, &info, nullptr, 0, err, sizeof(err));
    std::printf("ok: %u entries, every payload matches its hash\n", info.entry_count);
    return 0;
}

int cmd_cat(const char *file, const char *path) {
    char err[512] = { 0 };
    if (dai_vfs_mount_archive(file, 0, err, sizeof(err)) != DAI_OK) {
        std::printf("dai_pack: %s\n", err);
        return 1;
    }
    size_t n = 0;
    void *bytes = dai_vfs_read(path, &n);
    if (!bytes) { std::printf("dai_pack: %s\n", dai_vfs_last_error()); return 1; }
    std::fwrite(bytes, 1, n, stdout);
    dai_vfs_free(bytes);
    dai_vfs_unmount_all();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];

    if (cmd == "list")   return cmd_list(argv[2]);
    if (cmd == "verify") return cmd_verify(argv[2]);
    if (cmd == "cat")    return argc < 4 ? usage() : cmd_cat(argv[2], argv[3]);
    if (cmd != "build" && cmd != "export") return usage();

    // build  <project> <out>
    // export <project> <runtime> <out>
    int need = cmd == "export" ? 5 : 4;
    if (argc < need) return usage();
    const char *project = argv[2];
    const char *runtime = cmd == "export" ? argv[3] : nullptr;
    const char *out = cmd == "export" ? argv[4] : argv[3];

    dai_boot_config cfg = dai_boot_config_default();
    defaults_from_project(project, &cfg);

    for (int i = need; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](void) -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--scene")           std::snprintf(cfg.scene, sizeof(cfg.scene), "%s", next());
        else if (a == "--title")      std::snprintf(cfg.title, sizeof(cfg.title), "%s", next());
        else if (a == "--width")      cfg.width = std::atoi(next());
        else if (a == "--height")     cfg.height = std::atoi(next());
        else if (a == "--fullscreen") cfg.fullscreen = 1;
        else if (a == "--msaa")       cfg.msaa = std::atoi(next());
        else if (a == "--tick")       cfg.tick_hz = std::atoi(next());
        else if (a == "--max-bodies") cfg.max_bodies = std::atoi(next());
        else if (a == "--physics")    cfg.physics_backend = std::atoi(next());
        else if (a == "--language")   std::snprintf(cfg.language, sizeof(cfg.language), "%s", next());
        else if (a == "--gravity") {
            cfg.gravity[0] = (float)std::atof(next());
            cfg.gravity[1] = (float)std::atof(next());
            cfg.gravity[2] = (float)std::atof(next());
        } else { std::printf("dai_pack: unknown option '%s'\n", a.c_str()); return usage(); }
    }

    char err[512] = { 0 };
    uint32_t n = dai_pack_build_from_dir(project, out, runtime, &cfg, err, sizeof(err));
    if (!n) { std::printf("dai_pack: %s\n", err); return 1; }

    dai_pack_info info;
    std::memset(&info, 0, sizeof(info));
    dai_pack_read(out, &info, nullptr, 0, err, sizeof(err));
    char b1[32], b2[32];
    std::printf("%s: %u entries, archive %s at offset %llu, file %s\n",
                out, n, human(info.archive_bytes, b1, sizeof(b1)),
                (unsigned long long)info.base, human(info.file_bytes, b2, sizeof(b2)));
    if (cfg.language[0]) std::printf("  language: %s\n", cfg.language);
    std::printf("  startup scene: %s\n  title: %s\n  %dx%d%s, tick %d Hz\n",
                cfg.scene, cfg.title, cfg.width, cfg.height,
                cfg.fullscreen ? ", fullscreen" : "", cfg.tick_hz);
    return 0;
}
