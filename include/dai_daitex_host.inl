// MODULE 3 (texture baker) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that draws a scene: examples/editor_demo.cpp and
// tools/modeling_shot.cpp. It is where a `.daitex` node graph in the project
// becomes three PNGs on disk - base colour, ORM, normal - which module 2's
// material seam then loads like any other map.
//
// The rule from docs/MATERIALS.md is NOT bent by this: the engine renders
// MAPS. The graph is a bake tool that happens to live in the editor instead of
// in Blender, and what reaches the renderer is a texture, exactly as before.
//
// The contract:
//
//   * Bake on load, and re-bake when the .daitex is newer than its output.
//     Never every frame: a 1024x1024 Worley graph is not a per frame cost.
//   * DETERMINISTIC. Same graph plus same seed = bit identical PNG bytes, on
//     every machine, every run. No clock, no unseeded random, no threading
//     order that can reorder a reduction. The test for this compares two bakes
//     byte for byte, so a float that comes out one ULP different is a failure,
//     not a rounding detail.
//   * Never block the editor for longer than a bake takes; a graph that fails
//     to parse writes nothing and says so in the console, leaving the previous
//     PNGs in place.
//
// Called once per frame. `dai_daitex_host_thumb` answers the Project panel's
// thumbnail callback: it returns 0 for anything that is not a .daitex, which
// leaves the host's own thumbnail path in charge of everything else.
//
// How the three rules above are kept, concretely:
//
//   * the folder is walked, and every known graph re-stat'ed, twice a second
//     rather than every frame - `POLL_EVERY` below - and immediately when the
//     host bumps `asset_revision`, which is what a save or an import does;
//   * ONE graph is baked per poll. A folder of twelve graphs comes up over
//     twelve polls instead of freezing the first frame after a project opens;
//   * a graph is baked when its mtime moved or its `_basecolor.png` is
//     missing, and then never again. The digest is printed with it, so two
//     machines can be compared by reading one line rather than by diffing a
//     PNG.

#include "dai_daitex.h"

#include <map>
#include <string>
#include <vector>

#include <dirent.h>     // mingw has it too - one directory API for both
#include <sys/stat.h>

struct dai_daitex_state {
    long long mtime = 0;         /* of the .daitex when it was last baked   */
    uint64_t  digest = 0;        /* what it baked to - printed, and compared */
    uint32_t  size = 0;
    dai_texture preview = 0;     /* 64px of the base colour, for the panels  */
    int       failed = 0;        /* said so once already                     */
};

static std::map<std::string, dai_daitex_state> g_daitex;
static uint32_t g_daitex_revision = 0xFFFFFFFFu;
static int      g_daitex_countdown = 0;
static const int DAI_DAITEX_POLL_EVERY = 30;   /* frames: twice a second at 60 */

static long long dai_daitex_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime;
}

static int dai_daitex_is_graph(const std::string &p) {
    return p.size() > 7 && p.compare(p.size() - 7, 7, ".daitex") == 0;
}

// Every .daitex under `dir`, depth first, in the order readdir hands them over
// - which is why the list is SORTED before anything is baked: two machines
// with different file systems must bake the same graph first, or "one bake per
// poll" is a different picture on each of them for the first few frames.
static void dai_daitex_scan(const std::string &dir, std::vector<std::string> *out) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string full = dir + "/" + e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) dai_daitex_scan(full, out);
        else if (dai_daitex_is_graph(full)) out->push_back(full);
    }
    closedir(d);
    for (size_t i = 1; i < out->size(); ++i)      /* insertion sort: a handful */
        for (size_t j = i; j > 0 && (*out)[j] < (*out)[j - 1]; --j)
            std::swap((*out)[j], (*out)[j - 1]);
}

// Bakes one graph and writes its three maps. Returns 0 and leaves the previous
// PNGs alone when the file does not parse - half a wall beats no wall, and the
// console line is what tells the author which line of which file to look at.
static int dai_daitex_bake_one(dai_renderer *r, const std::string &path,
                               dai_daitex_state *st) {
    daitex::Graph g;
    std::string err;
    if (!daitex::parse_file(path.c_str(), &g, &err)) {
        if (!st->failed) std::printf("daitex: %s: %s\n", path.c_str(), err.c_str());
        st->failed = 1;
        st->mtime = dai_daitex_mtime(path.c_str());
        return 0;
    }
    daitex::Bake b;
    if (!daitex::bake(g, &b)) {
        if (!st->failed) std::printf("daitex: %s: nothing to bake\n", path.c_str());
        st->failed = 1;
        st->mtime = dai_daitex_mtime(path.c_str());
        return 0;
    }
    if (!daitex::write_maps(b, path, &err)) {
        if (!st->failed) std::printf("daitex: %s\n", err.c_str());
        st->failed = 1;
        st->mtime = dai_daitex_mtime(path.c_str());
        return 0;
    }
    st->failed = 0;
    st->mtime = dai_daitex_mtime(path.c_str());
    st->digest = daitex::digest(b);
    st->size = b.size;
    if (st->preview && r) dai_render_texture_destroy(r, st->preview);
    st->preview = 0;
    if (r) {
        std::vector<uint8_t> px;
        daitex::preview(b, 64, &px);
        st->preview = dai_render_texture_create(r, px.data(), 64, 64, 0);
    }
    std::printf("daitex: baked %s  %ux%u  digest %016llx\n", path.c_str(),
                b.size, b.size, (unsigned long long)st->digest);
    return 1;
}

// The Project panel's picture for a .daitex, by FULL path. A separate entry
// point from the one below because a host's thumbnail callback runs wherever
// the host put it - in examples/editor_demo.cpp that is above the line where
// dai_ext.h is included - and a picture is not worth moving a host's includes
// around for.
static dai_texture dai_daitex_host_preview(dai_renderer *r, const char *full_path) {
    if (!full_path || !dai_daitex_is_graph(full_path)) return 0;
    dai_daitex_state &st = g_daitex[full_path];
    long long now = dai_daitex_mtime(full_path);
    if (!st.preview || st.mtime != now) dai_daitex_bake_one(r, full_path, &st);
    return st.preview;
}

static void dai_daitex_host_poll(const dai_ext_host *h) {
    if (!h || !h->assets_dir || !h->assets_dir[0]) return;
    int forced = (h->asset_revision != g_daitex_revision);
    if (forced) g_daitex_revision = h->asset_revision;
    if (!forced && --g_daitex_countdown > 0) return;
    g_daitex_countdown = DAI_DAITEX_POLL_EVERY;

    std::vector<std::string> graphs;
    dai_daitex_scan(h->assets_dir, &graphs);
    for (const std::string &p : graphs) {
        dai_daitex_state &st = g_daitex[p];
        long long now = dai_daitex_mtime(p.c_str());
        int stale = (st.mtime != now);
        if (!stale && !st.failed && st.digest == 0) stale = 1;      /* never baked */
        if (!stale && !st.failed) {
            /* The maps are what the material loads: if one of them is gone,
             * the graph is stale no matter what its own mtime says. */
            std::string bc = daitex::map_path(p, "_basecolor.png");
            if (dai_daitex_mtime(bc.c_str()) == 0) stale = 1;
        }
        if (!stale) continue;
        dai_daitex_bake_one(h->renderer, p, &st);
        return;                       /* one per poll - see the header above */
    }
}

// `inline` as well as `static`: a host that answers its thumbnail callback
// through dai_daitex_host_preview above (examples/editor_demo.cpp does) never
// calls this one, and an unused static function is a warning in a build that
// keeps -Wall on.
static inline dai_texture dai_daitex_host_thumb(const dai_ext_host *h, const char *path) {
    if (!h || !path) return 0;                 /* 0 = not a .daitex          */
    if (!dai_daitex_is_graph(path)) return 0;
    /* Relative to the project's Assets folder, the way every asset reference
     * in the document is - and absolute when it already was. */
    std::string full = path;
    if (full.find('/') != 0 && h->assets_dir && h->assets_dir[0] &&
        full.compare(0, std::strlen(h->assets_dir), h->assets_dir) != 0)
        full = std::string(h->assets_dir) + "/" + path;
    return dai_daitex_host_preview(h->renderer, full.c_str());
}
