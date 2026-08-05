#!/usr/bin/env python3
# patch44 - the import helpers, portably. mingw declares `struct stat` in a way
# that shadows stat() in C++ and does not define S_ISDIR, so "is this a folder"
# has to be asked the way each platform asks it.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p44'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('examples/editor_demo.cpp')
s = sub1(s,
"""static int is_dir_path(const char *p) {
    if (!p || !*p) return 0;
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR ? 1 : 0;
}""",
"""// mingw's <sys/stat.h> declares `struct stat` without the C++ courtesy of
// keeping the FUNCTION stat() visible under the same name, and leaves S_ISDIR
// out entirely - so this cannot be written once for both. Asking each platform
// in its own words is shorter than fighting it, and the Win32 answer is the
// cheaper call anyway.
static int is_dir_path(const char *p) {
    if (!p || !*p) return 0;
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    if (::stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
#endif
}

static int path_exists(const char *p) {
    if (!p || !*p) return 0;
#ifdef _WIN32
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
#else
    struct stat st;
    return ::stat(p, &st) == 0 ? 1 : 0;
#endif
}""",
    'portable is_dir_path')

s = sub1(s,
"""        int tries = 0;
        while (tries < 200) {
            struct stat st;
            if (stat(dst.c_str(), &st) != 0) break;
            char suffix[16];""",
"""        int tries = 0;
        while (tries < 200) {
            if (!path_exists(dst.c_str())) break;
            char suffix[16];""",
    'portable exists check')
wr('examples/editor_demo.cpp', s)
print('patch44 ok')
