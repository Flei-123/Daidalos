#!/usr/bin/env python3
# patch60 - the crash: p->assets[p->asset_sel] read a pointer that was not
# there. One checked accessor, everywhere, and it says so when it catches one.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p60'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# The crash report pointed here exactly: project_body, a std::string built from
# p->assets[p->asset_sel], strlen on a pointer that is not a string.
#
#   movslq 0x438(%rax),%rdx      ; asset_sel
#   mov    0x3f8(%rax),%rax      ; assets.data()
#   mov    (%rax,%rdx,8),%rdi    ; assets[asset_sel]   <- no bound, only a null test
#   test   %rdi,%rdi ; je        ; the null check WAS there and passed
#   call   strlen                ; 0xc0000005
#
# Non-null and invalid means the index was outside the vector: the selection is
# an INDEX into a list the host replaces whenever the disk changes, and every
# place that reads it re-derived its own idea of whether it was still in range.
# Deleting a folder changes the list, so that is where it showed up.
#
# An index into somebody else's array is a weak handle by nature. It stays -
# rewriting the browser around paths is a bigger change than this bug deserves
# - but every read goes through one function now, and that function checks.
s = sub1(s,
"""void dai_editor_ui_asset_list(dai_editor_ui *p, const char *const *paths, uint32_t count) {""",
"""// The selected asset's path, or "" when the selection no longer names one.
// Never returns a pointer the caller has to test: the whole point is that
// there is exactly one place left where this can be got wrong.
static const char *asset_at(const dai_editor_ui *p, int index) {
    if (!p || index < 0 || index >= (int)p->assets.size()) return "";
    const char *a = p->assets[(size_t)index];
    return a ? a : "";
}

void dai_editor_ui_asset_list(dai_editor_ui *p, const char *const *paths, uint32_t count) {""",
    'asset_at accessor')

# The line the crash report named.
s = sub1(s,
"""        } else if (sel_valid) {
            const char *pick = p->assets[(size_t)p->asset_sel];
            std::string base = base_of(pick ? pick : "");""",
"""        } else if (sel_valid) {
            const char *pick = asset_at(p, p->asset_sel);
            std::string base = base_of(pick);""",
    'strip uses accessor')

s = sub1(s,
"""            if (pick && is_behaviour_file(pick)) {""",
"""            if (*pick && is_behaviour_file(pick)) {""",
    'strip behaviour test')

# Everywhere else that indexed the list by hand.
s = sub1(s,
"""                std::string full = p->assets[(size_t)fi] ? p->assets[(size_t)fi] : "";
                std::string label = searching ? full : base_of(full);""",
"""                std::string full = asset_at(p, fi);
                std::string label = searching ? full : base_of(full);""",
    'file row uses accessor')

s = sub1(s,
"""                                p->pending_asset = p->assets[(size_t)fi];
                                p->pending_as_tree = 0;""",
"""                                // The PENDING pick is handed to the host after
                                // the frame, so it must be the host's own
                                // pointer and not a temporary - asset_at
                                // returns "" for a bad index, which the host
                                // then ignores.
                                p->pending_asset = fi >= 0 && fi < (int)p->assets.size()
                                                 ? p->assets[(size_t)fi] : nullptr;
                                p->pending_as_tree = 0;""",
    'pending asset guarded')

s = sub1(s,
"""    int sel_valid = p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size();
    int folder_sel = !p->proj_sel_folder.empty();""",
"""    // Recomputed here and used here. It used to be decided at the top of the
    // function and trusted a hundred lines later, across every row the browser
    // draws - and a row can change the selection.
    if (p->asset_sel >= (int)p->assets.size()) p->asset_sel = -1;
    int sel_valid = p->asset_sel >= 0 && p->assets[(size_t)p->asset_sel] != nullptr;
    int folder_sel = !p->proj_sel_folder.empty();""",
    'sel_valid rechecked')

s = sub1(s,
"""        } else if (sel_valid) {
            const char *pick = asset_at(p, p->asset_sel);""",
"""        } else if (p->asset_sel >= 0 && p->asset_sel < (int)p->assets.size()) {
            const char *pick = asset_at(p, p->asset_sel);""",
    'strip guard is its own')

# The old flat browser panel indexes the same list.
s = sub1(s,
"""        const char *full = p->assets[i] ? p->assets[i] : "";
        const char *slash = std::strrchr(full, '/');""",
"""        const char *full = asset_at(p, (int)i);
        const char *slash = std::strrchr(full, '/');""",
    'flat browser uses accessor')

s = sub1(s,
"""int dai_editor_ui_asset_selected(const dai_editor_ui *p) {""",
"""int dai_editor_ui_asset_selected(const dai_editor_ui *p) {
    // Same rule as everywhere else: an index the list no longer has is no
    // selection at all.
    if (p && (p->asset_sel < 0 || p->asset_sel >= (int)p->assets.size())) return -1;""",
    'asset_selected guarded')
wr('src/dai_editor_ui.cpp', s)
print('patch60 ok')
