// Scene document: plain records, stable ids, generic undo. See include/dai_doc.h.
//
// No physics, no renderer, no Vulkan - this file must stay linkable on its own,
// which is what keeps the editor frontend agnostic. The reconciliation against
// a live world lives in dai_doc_sync.cpp.

#include "dai_doc.h"
#include "dai_doc_internal.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace daidoc {

dai_quat qmul(dai_quat a, dai_quat b) {
    return { a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
dai_quat qconj(dai_quat q) { return { -q.x, -q.y, -q.z, q.w }; }

dai_vec3 qrot(dai_quat q, dai_vec3 v) {
    dai_vec3 u{ q.x, q.y, q.z };
    dai_vec3 uv{ u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
    dai_vec3 uuv{ u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x };
    return { v.x + 2.0f*(q.w*uv.x + uuv.x),
             v.y + 2.0f*(q.w*uv.y + uuv.y),
             v.z + 2.0f*(q.w*uv.z + uuv.z) };
}

Node *find(dai_doc *d, dai_node n) {
    if (!d || n == DAI_INVALID_NODE) return nullptr;
    auto it = d->nodes.find(n);
    return (it != d->nodes.end() && it->second.alive) ? &it->second : nullptr;
}
const Node *find(const dai_doc *d, dai_node n) {
    if (!d || n == DAI_INVALID_NODE) return nullptr;
    auto it = d->nodes.find(n);
    return (it != d->nodes.end() && it->second.alive) ? &it->second : nullptr;
}

// Records the "before" state once per node per transaction. Calling it twice
// for the same node in one step must not overwrite the original state, or undo
// would only walk back the last mutation instead of the whole step.
void touch(dai_doc *d, dai_node n) {
    if (d->tx_seen.find(n) != d->tx_seen.end()) return;
    Change c{};
    c.id = n;
    auto it = d->nodes.find(n);
    c.had_before = (it != d->nodes.end() && it->second.alive);
    if (c.had_before) c.before = it->second.d;
    d->tx_seen.insert(n);
    d->tx_changes.push_back(c);
}

// A node's world transform depends on its ancestors, so moving a parent
// invalidates every descendant even though their records did not change.
// The sync layer keys off `rev`, so it has to be bumped down the whole subtree.
void bump_subtree(dai_doc *d, dai_node n) {
    Node *node = find(d, n);
    if (!node) return;
    node->rev = ++d->rev_counter;
    for (auto &kv : d->nodes)
        if (kv.second.alive && kv.second.d.parent == n) bump_subtree(d, kv.first);
}

bool is_descendant(const dai_doc *d, dai_node candidate, dai_node of) {
    // Bounded by the node count so a corrupted parent cycle cannot hang us.
    uint32_t guard = (uint32_t)d->nodes.size() + 1;
    dai_node p = candidate;
    while (p != DAI_INVALID_NODE && guard--) {
        if (p == of) return true;
        const Node *n = find(d, p);
        if (!n) break;
        p = n->d.parent;
    }
    return false;
}

void apply_record(dai_doc *d, dai_node id, bool exists, const dai_node_desc &rec) {
    if (exists) {
        Node &n = d->nodes[id];
        n.d = rec;
        n.alive = true;
        n.rev = ++d->rev_counter;
    } else {
        auto it = d->nodes.find(id);
        if (it != d->nodes.end()) {
            it->second.alive = false;
            it->second.rev = ++d->rev_counter;
        }
    }
}

} // namespace daidoc

using namespace daidoc;

namespace {

struct AutoTx {
    dai_doc *d;
    bool owned;
    AutoTx(dai_doc *doc, const char *name) : d(doc), owned(doc->tx_depth == 0) {
        dai_doc_begin(d, name);
    }
    ~AutoTx() { dai_doc_commit(d); }
};

bool same_record(const dai_node_desc &a, const dai_node_desc &b) {
    return std::memcmp(&a, &b, sizeof(dai_node_desc)) == 0;
}

} // namespace

extern "C" {

dai_node_desc dai_node_desc_default(void) {
    dai_node_desc d{};
    d.rotation = { 0, 0, 0, 1 };
    d.scale = { 1, 1, 1 };
    d.half_extent = { 0.5f, 0.5f, 0.5f };
    d.mesh = 0xFFFFFFFFu;
    d.roughness = 1.0f;
    d.shape = DAI_SHAPE_BOX;
    d.motion = DAI_STATIC;
    return d;
}

dai_doc *dai_doc_create(void) { return new dai_doc(); }
void dai_doc_destroy(dai_doc *d) { delete d; }

void dai_doc_clear(dai_doc *d) {
    if (!d) return;
    d->nodes.clear();
    d->undo.clear();
    d->redo.clear();
    d->tx_changes.clear();
    d->tx_seen.clear();
    d->tx_depth = 0;
    d->next_id = 1;
    d->rev_counter++;
    d->revision++;
}

// ------------------------------------------------------------ transactions

void dai_doc_begin(dai_doc *d, const char *name) {
    if (!d) return;
    if (d->tx_depth == 0) {
        d->tx_changes.clear();
        d->tx_seen.clear();
        d->tx_name = name ? name : "Edit";
    }
    ++d->tx_depth;
}

void dai_doc_commit(dai_doc *d) {
    if (!d || d->tx_depth == 0) return;
    if (--d->tx_depth > 0) return;              // inner bracket, keep collecting

    std::vector<Change> kept;
    kept.reserve(d->tx_changes.size());
    for (Change &c : d->tx_changes) {
        auto it = d->nodes.find(c.id);
        c.has_after = (it != d->nodes.end() && it->second.alive);
        if (c.has_after) c.after = it->second.d;
        if (c.had_before == c.has_after &&
            (!c.had_before || same_record(c.before, c.after))) continue;   // no-op
        kept.push_back(c);
    }
    d->tx_changes.clear();
    d->tx_seen.clear();
    if (kept.empty()) return;                   // a click that changed nothing is not a step

    Step s;
    s.name = d->tx_name;
    s.changes = std::move(kept);
    d->undo.push_back(std::move(s));
    d->redo.clear();                            // the classic rule: editing forks the future
    d->revision++;
}

void dai_doc_abort(dai_doc *d) {
    if (!d || d->tx_depth == 0) return;
    if (--d->tx_depth > 0) return;
    for (size_t i = d->tx_changes.size(); i-- > 0; ) {
        const Change &c = d->tx_changes[i];
        apply_record(d, c.id, c.had_before, c.before);
    }
    d->tx_changes.clear();
    d->tx_seen.clear();
}

int dai_doc_undo(dai_doc *d) {
    if (!d || d->undo.empty() || d->tx_depth != 0) return 0;
    Step s = d->undo.back();
    d->undo.pop_back();
    for (size_t i = s.changes.size(); i-- > 0; ) {
        const Change &c = s.changes[i];
        apply_record(d, c.id, c.had_before, c.before);
    }
    for (const Change &c : s.changes) bump_subtree(d, c.id);
    d->redo.push_back(std::move(s));
    d->revision++;
    return 1;
}

int dai_doc_redo(dai_doc *d) {
    if (!d || d->redo.empty() || d->tx_depth != 0) return 0;
    Step s = d->redo.back();
    d->redo.pop_back();
    for (const Change &c : s.changes) apply_record(d, c.id, c.has_after, c.after);
    for (const Change &c : s.changes) bump_subtree(d, c.id);
    d->undo.push_back(std::move(s));
    d->revision++;
    return 1;
}

uint32_t dai_doc_undo_depth(const dai_doc *d) { return d ? (uint32_t)d->undo.size() : 0; }
uint32_t dai_doc_redo_depth(const dai_doc *d) { return d ? (uint32_t)d->redo.size() : 0; }
const char *dai_doc_undo_name(const dai_doc *d) {
    return (d && !d->undo.empty()) ? d->undo.back().name.c_str() : "";
}
const char *dai_doc_redo_name(const dai_doc *d) {
    return (d && !d->redo.empty()) ? d->redo.back().name.c_str() : "";
}
uint64_t dai_doc_revision(const dai_doc *d) { return d ? d->revision : 0; }

// ------------------------------------------------------------------ nodes

dai_node dai_doc_add(dai_doc *d, const dai_node_desc *desc) {
    if (!d || !desc) return DAI_INVALID_NODE;
    if (desc->parent != DAI_INVALID_NODE && !find(d, desc->parent)) return DAI_INVALID_NODE;

    AutoTx tx(d, "Add");
    dai_node id = d->next_id++;
    touch(d, id);
    Node n;
    n.d = *desc;
    n.d.name[DAI_NODE_NAME_MAX - 1] = 0;
    n.alive = true;
    n.rev = ++d->rev_counter;
    d->nodes[id] = n;
    return id;
}

dai_result dai_doc_remove(dai_doc *d, dai_node n) {
    if (!find(d, n)) return DAI_ERR_NOT_FOUND;
    AutoTx tx(d, "Delete");

    // Collect the subtree first; deleting while walking children invalidates
    // the walk, and a child left behind would be an orphan pointing at a dead
    // parent - which is exactly the kind of thing undo cannot repair later.
    std::vector<dai_node> doomed;
    doomed.push_back(n);
    for (size_t i = 0; i < doomed.size(); ++i)
        for (auto &kv : d->nodes)
            if (kv.second.alive && kv.second.d.parent == doomed[i]) doomed.push_back(kv.first);

    for (dai_node id : doomed) {
        touch(d, id);
        Node &node = d->nodes[id];
        node.alive = false;
        node.rev = ++d->rev_counter;
    }
    return DAI_OK;
}

dai_result dai_doc_get(const dai_doc *d, dai_node n, dai_node_desc *out) {
    const Node *node = find(d, n);
    if (!node || !out) return DAI_ERR_NOT_FOUND;
    *out = node->d;
    return DAI_OK;
}

dai_result dai_doc_set(dai_doc *d, dai_node n, const dai_node_desc *desc) {
    Node *node = find(d, n);
    if (!node || !desc) return DAI_ERR_NOT_FOUND;
    if (desc->parent != DAI_INVALID_NODE) {
        if (!find(d, desc->parent)) return DAI_ERR_INVALID_ARG;
        if (is_descendant(d, desc->parent, n)) return DAI_ERR_INVALID_ARG;   // no cycles
    }
    if (same_record(node->d, *desc)) return DAI_OK;

    AutoTx tx(d, "Edit");
    touch(d, n);
    node->d = *desc;
    node->d.name[DAI_NODE_NAME_MAX - 1] = 0;
    bump_subtree(d, n);
    return DAI_OK;
}

dai_result dai_doc_set_parent(dai_doc *d, dai_node n, dai_node parent) {
    Node *node = find(d, n);
    if (!node) return DAI_ERR_NOT_FOUND;
    dai_node_desc rec = node->d;
    rec.parent = parent;
    AutoTx tx(d, "Reparent");
    return dai_doc_set(d, n, &rec);
}

int dai_doc_valid(const dai_doc *d, dai_node n) { return find(d, n) != nullptr; }

uint32_t dai_doc_count(const dai_doc *d) {
    if (!d) return 0;
    uint32_t c = 0;
    for (const auto &kv : d->nodes) if (kv.second.alive) ++c;
    return c;
}

uint32_t dai_doc_children(const dai_doc *d, dai_node parent, dai_node *out, uint32_t max) {
    if (!d) return 0;
    std::vector<dai_node> ids;
    for (const auto &kv : d->nodes)
        if (kv.second.alive && kv.second.d.parent == parent) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    uint32_t w = 0;
    for (dai_node id : ids) { if (out && w < max) out[w] = id; ++w; }
    return out ? std::min(w, max) : w;
}

uint32_t dai_doc_nodes(const dai_doc *d, dai_node *out, uint32_t max) {
    if (!d) return 0;
    // Parents before children, ids ascending: a stable order means saved files
    // and test expectations do not shuffle between runs of an unordered_map.
    std::vector<dai_node> roots;
    for (const auto &kv : d->nodes) {
        if (!kv.second.alive) continue;
        if (!find(d, kv.second.d.parent)) roots.push_back(kv.first);   // parentless = root
    }
    std::sort(roots.begin(), roots.end());

    std::vector<dai_node> order;
    order.reserve(d->nodes.size());
    for (size_t i = 0; i < roots.size(); ++i) order.push_back(roots[i]);
    for (size_t i = 0; i < order.size(); ++i) {
        std::vector<dai_node> kids;
        for (const auto &kv : d->nodes)
            if (kv.second.alive && kv.second.d.parent == order[i]) kids.push_back(kv.first);
        std::sort(kids.begin(), kids.end());
        for (dai_node k : kids) order.push_back(k);
    }
    uint32_t w = 0;
    for (dai_node id : order) { if (out && w < max) out[w] = id; ++w; }
    return out ? std::min(w, max) : w;
}

dai_node dai_doc_find(const dai_doc *d, const char *name) {
    if (!d || !name) return DAI_INVALID_NODE;
    std::vector<dai_node> ids((size_t)dai_doc_count(d));
    if (ids.empty()) return DAI_INVALID_NODE;
    dai_doc_nodes(d, ids.data(), (uint32_t)ids.size());
    for (dai_node id : ids) {
        const Node *n = find(d, id);
        if (n && std::strcmp(n->d.name, name) == 0) return id;
    }
    return DAI_INVALID_NODE;
}

// -------------------------------------------------------------- transforms

dai_result dai_doc_world_transform(const dai_doc *d, dai_node n,
                                   dai_vec3 *pos, dai_quat *rot, dai_vec3 *scale) {
    const Node *node = find(d, n);
    if (!node) return DAI_ERR_NOT_FOUND;

    // Walk up to the root, then compose downwards.
    std::vector<const Node *> chain;
    const Node *cur = node;
    uint32_t guard = (uint32_t)d->nodes.size() + 1;
    while (cur && guard--) {
        chain.push_back(cur);
        cur = find(d, cur->d.parent);
    }
    dai_vec3 p{ 0, 0, 0 }, s{ 1, 1, 1 };
    dai_quat r{ 0, 0, 0, 1 };
    for (size_t i = chain.size(); i-- > 0; ) {
        const dai_node_desc &l = chain[i]->d;
        dai_vec3 scaled{ l.position.x * s.x, l.position.y * s.y, l.position.z * s.z };
        dai_vec3 rotated = qrot(r, scaled);
        p = { p.x + rotated.x, p.y + rotated.y, p.z + rotated.z };
        r = qmul(r, l.rotation);
        s = { s.x * l.scale.x, s.y * l.scale.y, s.z * l.scale.z };
    }
    if (pos) *pos = p;
    if (rot) *rot = r;
    if (scale) *scale = s;
    return DAI_OK;
}

dai_result dai_doc_set_world_position(dai_doc *d, dai_node n, dai_vec3 world_pos) {
    Node *node = find(d, n);
    if (!node) return DAI_ERR_NOT_FOUND;
    dai_vec3 pp{ 0, 0, 0 }, ps{ 1, 1, 1 };
    dai_quat pr{ 0, 0, 0, 1 };
    if (node->d.parent != DAI_INVALID_NODE)
        dai_doc_world_transform(d, node->d.parent, &pp, &pr, &ps);

    dai_vec3 rel{ world_pos.x - pp.x, world_pos.y - pp.y, world_pos.z - pp.z };
    dai_vec3 local = qrot(qconj(pr), rel);
    dai_node_desc rec = node->d;
    rec.position = { local.x / (ps.x != 0 ? ps.x : 1.0f),
                     local.y / (ps.y != 0 ? ps.y : 1.0f),
                     local.z / (ps.z != 0 ? ps.z : 1.0f) };
    return dai_doc_set(d, n, &rec);
}

dai_result dai_doc_set_world_rotation(dai_doc *d, dai_node n, dai_quat world_rot) {
    Node *node = find(d, n);
    if (!node) return DAI_ERR_NOT_FOUND;
    dai_quat pr{ 0, 0, 0, 1 };
    if (node->d.parent != DAI_INVALID_NODE)
        dai_doc_world_transform(d, node->d.parent, nullptr, &pr, nullptr);
    dai_node_desc rec = node->d;
    rec.rotation = qmul(qconj(pr), world_rot);
    return dai_doc_set(d, n, &rec);
}

} // extern "C"
