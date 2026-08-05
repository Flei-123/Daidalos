#!/usr/bin/env python3
# patch73 - "Cylinder (1) (1) (1)". The number goes up; it does not queue.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p73'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_doc.cpp')

s = sub1(s,
"""static void make_unique_name(dai_doc *d, dai_node_desc *desc, dai_node parent) {
    if (!desc->name[0]) return;
    char base[DAI_NODE_NAME_MAX];
    std::snprintf(base, sizeof(base), "%s", desc->name);""",
"""// Strips one trailing " (N)" - the suffix THIS function adds. "Cylinder (3)"
// gives "Cylinder"; "Mark (2019)" gives "Mark" too, and that is the right
// trade: the alternative is that duplicating a copy appends a second suffix,
// and then a third, until the hierarchy reads "Cylinder (1) (1) (1) (1)".
// Copying a copy means "another one of those", not "a copy of the copy".
static void strip_copy_suffix(char *name) {
    size_t n = std::strlen(name);
    if (n < 4 || name[n - 1] != ')') return;
    size_t i = n - 2;
    if (name[i] < '0' || name[i] > '9') return;          // "(  )" or "(a)" is a name
    while (i > 0 && name[i] >= '0' && name[i] <= '9') --i;
    if (name[i] != '(') return;
    if (i == 0 || name[i - 1] != ' ') return;            // "Box(2)" is somebody's name
    name[i - 1] = 0;
}

static void make_unique_name(dai_doc *d, dai_node_desc *desc, dai_node parent) {
    if (!desc->name[0]) return;
    char base[DAI_NODE_NAME_MAX];
    std::snprintf(base, sizeof(base), "%s", desc->name);
    strip_copy_suffix(base);
    if (!base[0]) std::snprintf(base, sizeof(base), "%s", desc->name);   // "(2)" alone""",
    'strip copy suffix')

# With the base stripped, the FIRST candidate must still be the bare name -
# otherwise renaming "Cylinder (3)" to itself would rename it to "Cylinder".
# It already is: suffix 0 is the base, and the base is only free when nothing
# else holds it.
s = sub1(s,
"""    for (int suffix = 0; suffix < 100; ++suffix) {""",
"""    for (int suffix = 0; suffix < 1000; ++suffix) {""",
    'more room before giving up')
wr('src/dai_doc.cpp', s)
print('patch73 ok')
