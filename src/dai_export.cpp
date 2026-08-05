// dai_export.cpp - a project becomes one file somebody else can double click.
//
// This is the Godot arrangement, and the reason it is worth copying for an
// engine this size: EXPORTING COMPILES NOTHING.
//
//   1. a runtime binary was built once, from this same engine, without the
//      editor (examples/runtime_main.cpp -> tools/build_runtime.sh)
//   2. the project - scenes, assets, settings - is packed into one archive
//   3. the archive is APPENDED to a copy of that runtime
//
// The result starts, reads the last 16 bytes of its own file, finds the
// archive and mounts it. No compiler on the user's machine, no linking, no
// code generation, nothing to install on the machine it is handed to.
//
// Two entry points:
//   dai_pack_build_from_dir   the directory walk and the packing rules
//   dai_project_export        the same thing, driven by an open dai_project
//
// WHY THIS IS NOT IN dai_project.cpp, where its header declares it: build.sh
// links tests/test_project.cpp against src/dai_project.cpp AND NOTHING ELSE.
// That isolation is a tested property - the project layer must not drag a
// renderer, an archive writer or anything else in behind it - so the export
// lives in its own translation unit and the project layer stays a folder with
// opinions. Anything that wants dai_project_export links this file too.

#include "dai_vfs.h"
#include "dai_project.h"

#include <algorithm>
#include <string>
#include <vector>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

void set_err(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}
bool is_file(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

std::string trim_sep(const char *s) {
    std::string t = s ? s : "";
    for (size_t i = 0; i < t.size(); ++i) if (t[i] == '\\') t[i] = '/';
    while (t.size() > 1 && t.back() == '/') t.pop_back();
    return t;
}

// Every regular file under <root>/<rel>, as paths relative to <root>. Sorted,
// so two exports of the same project produce byte identical archives - which
// is what makes "did anything actually change" answerable with a checksum.
void collect(const std::string &root, const std::string &rel, std::vector<std::string> &out) {
    std::string here = rel.empty() ? root : root + "/" + rel;
    DIR *d = opendir(here.c_str());
    if (!d) return;
    std::vector<std::string> names;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        // '.' covers ".", "..", ".git", ".DS_Store" and every editor dropping
        // in one rule. Nothing a game needs at runtime starts with a dot.
        if (e->d_name[0] == '.') continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i < names.size(); ++i) {
        std::string child = rel.empty() ? names[i] : rel + "/" + names[i];
        std::string full = root + "/" + child;
        if (is_dir(full)) collect(root, child, out);
        else if (is_file(full) && child.size() < DAI_VFS_PATH_MAX) out.push_back(child);
    }
}

} // namespace

extern "C" {

uint32_t dai_pack_build_from_dir(const char *project_dir, const char *out_path,
                                 const char *prefix_file, const dai_boot_config *boot,
                                 char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!project_dir || !*project_dir || !out_path || !*out_path) {
        set_err(err, err_len, "dai_pack_build_from_dir: missing project or output path");
        return 0;
    }
    std::string root = trim_sep(project_dir);
    if (!is_dir(root)) {
        set_err(err, err_len, "'%s' is not a directory", root.c_str());
        return 0;
    }
    if (prefix_file && *prefix_file && !is_file(prefix_file)) {
        set_err(err, err_len, "no runtime template at '%s' - run tools/build_runtime.sh",
                prefix_file);
        return 0;
    }

    dai_boot_config cfg = boot ? *boot : dai_boot_config_default();

    // What goes in. cache/ is DERIVED data by definition (that is the promise
    // dai_project makes about it), so shipping it would only make the download
    // bigger and the first start no faster.
    std::vector<std::string> files;
    collect(root + "/scenes", "", files);
    for (size_t i = 0; i < files.size(); ++i) files[i] = "scenes/" + files[i];
    std::vector<std::string> assets;
    collect(root + "/assets", "", assets);
    for (size_t i = 0; i < assets.size(); ++i) files.push_back("assets/" + assets[i]);
    if (is_file(root + "/settings/project.txt")) files.push_back("settings/project.txt");
    if (is_file(root + "/" + DAI_PROJECT_MARKER)) files.push_back(DAI_PROJECT_MARKER);

    // The startup scene has to be IN there, or the export is a program that
    // opens a window and says nothing. Better to refuse now than at the user's.
    bool have_scene = false;
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i] == cfg.scene) { have_scene = true; break; }
    if (!have_scene) {
        set_err(err, err_len, "the startup scene '%s' is not in '%s'", cfg.scene, root.c_str());
        return 0;
    }

    dai_pack *p = dai_pack_begin(out_path, prefix_file, err, err_len);
    if (!p) return 0;

    char boot_text[1024];
    dai_boot_config_write(&cfg, boot_text, sizeof(boot_text));
    if (!dai_pack_add_mem(p, DAI_PACK_BOOT_PATH, boot_text, std::strlen(boot_text))) {
        set_err(err, err_len, "cannot write the boot config into the archive");
        dai_pack_abort(p);
        return 0;
    }

    for (size_t i = 0; i < files.size(); ++i) {
        std::string full = root + "/" + files[i];
        if (!dai_pack_add_file(p, files[i].c_str(), full.c_str())) {
            set_err(err, err_len, "cannot pack '%s'", files[i].c_str());
            dai_pack_abort(p);
            return 0;
        }
    }

    uint32_t n = dai_pack_count(p);
    if (dai_pack_end(p, err, err_len) != DAI_OK) return 0;
    return n;
}

dai_result dai_project_export(dai_project *project, const char *out_path,
                              const char *runtime_template, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!project || !out_path || !*out_path) {
        set_err(err, err_len, "dai_project_export: no project or no output path");
        return DAI_ERR_INVALID_ARG;
    }

    dai_project_settings s = dai_project_settings_default();
    dai_project_settings_load(project, &s);

    dai_boot_config cfg = dai_boot_config_default();
    // The startup scene, as the project itself defines it - the same rule
    // dai_project_scene_path uses, so the export opens what the editor opens.
    std::snprintf(cfg.scene, sizeof(cfg.scene), "%s",
                  s.default_scene[0] ? s.default_scene : "scenes/main.daidalos");
    for (size_t i = 0; cfg.scene[i]; ++i) if (cfg.scene[i] == '\\') cfg.scene[i] = '/';

    const char *title = s.app_name[0] ? s.app_name : dai_project_name(project);
    std::snprintf(cfg.title, sizeof(cfg.title), "%s", title ? title : "Daidalos");

    // The simulation settings belong to the PROJECT, so the shipped game runs
    // the tick rate and the gravity the scene was built against. Two people
    // who disagree about the tick rate are not playing the same game, and that
    // has to survive the export or the whole setting was decoration.
    cfg.tick_hz = s.tick_hz > 0 ? s.tick_hz : 60;
    cfg.max_bodies = s.max_bodies > 0 ? s.max_bodies : 4096;
    cfg.physics_backend = s.physics_backend;
    cfg.gravity[0] = s.gravity[0];
    cfg.gravity[1] = s.gravity[1];
    cfg.gravity[2] = s.gravity[2];

    const char *tmpl = runtime_template;
    if (!tmpl || !*tmpl) {
        set_err(err, err_len, "dai_project_export: no runtime template given "
                              "(build one with tools/build_runtime.sh)");
        return DAI_ERR_INVALID_ARG;
    }

    uint32_t n = dai_pack_build_from_dir(dai_project_path(project), out_path, tmpl, &cfg,
                                         err, err_len);
    if (n == 0) return DAI_ERR_FILE;
    return DAI_OK;
}

} // extern "C"
