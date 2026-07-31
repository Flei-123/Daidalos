// Shared internals of the scene document. Not a public header.
#ifndef DAI_DOC_INTERNAL_HPP
#define DAI_DOC_INTERNAL_HPP

#include "dai_doc.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace daidoc {

struct Node {
    dai_node_desc d{};
    bool     alive = false;
    uint64_t rev = 0;      // bumped on every change, including ancestor moves
};

// One node before and after a change. `had_before == false` is a creation,
// `has_after == false` is a deletion - one struct covers all three cases, which
// is why undo needs no per-property command classes.
struct Change {
    dai_node      id = 0;
    bool          had_before = false;
    bool          has_after = false;
    dai_node_desc before{};
    dai_node_desc after{};
};

struct Step {
    std::string         name;
    std::vector<Change> changes;
};

dai_quat qmul(dai_quat a, dai_quat b);
dai_quat qconj(dai_quat q);
dai_vec3 qrot(dai_quat q, dai_vec3 v);

} // namespace daidoc

struct dai_doc {
    std::unordered_map<dai_node, daidoc::Node> nodes;
    dai_node next_id = 1;              // never reused, even after delete + undo
    uint64_t rev_counter = 1;
    uint64_t revision = 0;

    int                              tx_depth = 0;
    std::string                      tx_name;
    std::vector<daidoc::Change>      tx_changes;
    std::unordered_set<dai_node>     tx_seen;

    std::vector<daidoc::Step> undo, redo;
};

namespace daidoc {
daidoc::Node       *find(dai_doc *d, dai_node n);
const daidoc::Node *find(const dai_doc *d, dai_node n);
void                touch(dai_doc *d, dai_node n);
void                bump_subtree(dai_doc *d, dai_node n);
bool                is_descendant(const dai_doc *d, dai_node candidate, dai_node of);
void                apply_record(dai_doc *d, dai_node id, bool exists, const dai_node_desc &rec);
} // namespace daidoc

#endif
