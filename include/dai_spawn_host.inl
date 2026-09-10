// Spawning, in ONE place: what scene.spawn() / scene.destroy() mean, for every
// host that runs behaviours.
//
// The companion of include/dai_props_host.inl and included right after it, for
// the same reason: examples/editor_demo.cpp, tools/modeling_shot.cpp and
// examples/runtime_main.cpp all answer the same JS, and three copies of "what
// does it mean to copy a room" is three chances for the editor and the shipped
// game to disagree about the one thing the player would notice.
//
// WHY A COPY AND NOT editor.add()
// A behaviour has no `editor` on purpose - see the comment on
// dai_script_editor_host in include/dai_script.h. But INNEN's Gehaeuse rule
// (docs/GDD_INNEN.md §5.2) needs a room that did not exist a second ago, and
// "the engine cannot do that" was 20 % of that rule sitting in a log line
// instead of in the game. The middle ground is this: a behaviour may DUPLICATE
// something an author placed, and nothing else. Whatever is correct about the
// source - a wall's CSG door hole, a collider, a script with its tuned
// parameters, a material stack - is correct about the copy, because the copy
// is the same record.
//
// The including file says which document it means before it includes:
//
//     #define DAI_PROPS_DOC g_doc
//     #include "dai_props_host.inl"
//     #include "dai_spawn_host.inl"
//
// Expected in scope: <vector>, <cstring>, dai_doc.h, and a `dai_doc *` named
// by DAI_PROPS_DOC.

#ifndef DAI_PROPS_DOC
#error "define DAI_PROPS_DOC to the dai_doc* this host owns before including"
#endif

namespace {

// A subtree bigger than this is a runaway, not a room: a behaviour that
// spawns inside its own frame() would otherwise eat the document in a second
// and the crash would land far away from the line that caused it. The largest
// thing INNEN copies is one room - about eighty nodes.
const uint32_t DAI_SPAWN_MAX_NODES = 4096;

// Every node of the subtree rooted at n, parents always before their children,
// so a copy can be made in one pass with the parent already mapped. Stops and
// returns false past the budget.
bool spawn_collect(dai_node n, std::vector<dai_node> &out) {
    out.clear();
    if (!DAI_PROPS_DOC || !n || !dai_doc_valid(DAI_PROPS_DOC, n)) return false;
    out.push_back(n);
    for (size_t i = 0; i < out.size(); ++i) {
        uint32_t c = dai_doc_children(DAI_PROPS_DOC, out[i], nullptr, 0);
        if (!c) continue;
        std::vector<dai_node> kids(c);
        c = dai_doc_children(DAI_PROPS_DOC, out[i], kids.data(), c);
        for (uint32_t k = 0; k < c; ++k) {
            if (out.size() >= DAI_SPAWN_MAX_NODES) return false;
            out.push_back(kids[k]);
        }
    }
    return true;
}

// Is `maybe` inside the subtree of `root`? Asked before a spawn, because a
// copy parented into its own source would be its own child - the document
// rejects the cycle, but the half built subtree would already be standing.
bool spawn_is_inside(dai_node root, dai_node maybe) {
    if (!maybe) return false;
    for (dai_node cur = maybe; cur; ) {
        if (cur == root) return true;
        dai_node_desc r{};
        if (dai_doc_get(DAI_PROPS_DOC, cur, &r) != DAI_OK) return false;
        cur = r.parent;
    }
    return false;
}

// The whole subtree, copied under `parent` (0 = document root). Returns the id
// of the new root, or -1 when it was refused. `name` renames the new root only
// - the children keep the source's names, because a caller that wants
// "Raum05.Floor" to become "Raum07.Floor" knows the rule and this does not.
double comp_spawn(dai_node src, dai_node parent, const char *name) {
    if (!DAI_PROPS_DOC) return -1.0;
    if (!src || !dai_doc_valid(DAI_PROPS_DOC, src)) return -1.0;
    if (parent && !dai_doc_valid(DAI_PROPS_DOC, parent)) return -1.0;
    if (spawn_is_inside(src, parent)) return -1.0;

    std::vector<dai_node> src_nodes;
    if (!spawn_collect(src, src_nodes)) return -1.0;

    std::vector<dai_node> made;                 // parallel to src_nodes
    made.reserve(src_nodes.size());
    dai_node root = 0;
    for (size_t i = 0; i < src_nodes.size(); ++i) {
        dai_node_desc r{};
        if (dai_doc_get(DAI_PROPS_DOC, src_nodes[i], &r) != DAI_OK) { made.push_back(0); continue; }
        if (i == 0) {
            r.parent = parent;
            if (name && *name) std::snprintf(r.name, sizeof(r.name), "%s", name);
        } else {
            // Map the source parent to the copy that stands for it. A child
            // whose parent failed to copy is dropped rather than reparented to
            // the root: half a room in the right place is a bug report, half a
            // room scattered around the origin is a mystery.
            dai_node mapped = 0;
            for (size_t j = 0; j < i; ++j)
                if (src_nodes[j] == r.parent) { mapped = made[j]; break; }
            if (!mapped) { made.push_back(0); continue; }
            r.parent = mapped;
        }
        dai_node n = dai_doc_add(DAI_PROPS_DOC, &r);
        made.push_back(n);
        if (i == 0) {
            if (!n) return -1.0;
            root = n;
        }
    }
    return (double)(uint32_t)root;
}

// The node and everything under it. dai_doc_remove already takes the
// descendants with it; this exists to refuse the two ids that would take the
// world with them.
int comp_destroy(dai_node n) {
    if (!DAI_PROPS_DOC || !n) return 0;
    if (!dai_doc_valid(DAI_PROPS_DOC, n)) return 0;
    return dai_doc_remove(DAI_PROPS_DOC, n) == DAI_OK ? 1 : 0;
}

double comp_child_count(dai_node n) {
    if (!DAI_PROPS_DOC || !dai_doc_valid(DAI_PROPS_DOC, n)) return 0.0;
    return (double)dai_doc_children(DAI_PROPS_DOC, n, nullptr, 0);
}

double comp_child_at(dai_node n, double index) {
    if (!DAI_PROPS_DOC || !dai_doc_valid(DAI_PROPS_DOC, n)) return -1.0;
    if (index < 0) return -1.0;
    uint32_t c = dai_doc_children(DAI_PROPS_DOC, n, nullptr, 0);
    uint32_t i = (uint32_t)index;
    if (i >= c) return -1.0;
    std::vector<dai_node> kids(c);
    dai_doc_children(DAI_PROPS_DOC, n, kids.data(), c);
    return (double)(uint32_t)kids[i];
}

double comp_parent_of(dai_node n) {
    dai_node_desc r{};
    if (!DAI_PROPS_DOC || dai_doc_get(DAI_PROPS_DOC, n, &r) != DAI_OK) return -1.0;
    return (double)(uint32_t)r.parent;
}

} // namespace
