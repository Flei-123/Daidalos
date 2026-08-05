#!/usr/bin/env python3
# patch85 - the host fills the About block and answers "check for updates".
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p85'
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
"""        // Unsaved? The asterisk in the hierarchy comes from here.""",
"""        // The About block in Settings: three strings, pushed every frame
        // because they cost nothing and can never then be stale.
        {
            static char upd_status[160] = { 0 };
            if (dai_editor_ui_take_update_check(panels)) {
                // The editor updates itself against the download page's
                // manifest on start-up - that machinery lives in dai_update
                // and needs a URL and an install dir this build does not
                // carry. So this button is honest about what it can do: it
                // opens the page, where the version on offer is written down.
                std::snprintf(upd_status, sizeof(upd_status),
                              "opened the download page - it updates itself on restart");
                dai_editor_ui_log(panels, 0, "opening https://daidalos.fleitec.com");
#ifdef _WIN32
                ShellExecuteA(nullptr, "open", "https://daidalos.fleitec.com",
                              nullptr, nullptr, SW_SHOWNORMAL);
#else
                (void)std::system("xdg-open https://daidalos.fleitec.com >/dev/null 2>&1 &");
#endif
            }
            dai_editor_ui_about(panels, g_projects_root,
                                g_assets_dir[0] ? g_assets_dir : "", upd_status);
        }

        // Unsaved? The asterisk in the hierarchy comes from here.""",
    'about wiring')
wr('examples/editor_demo.cpp', s)
print('patch85 ok')
