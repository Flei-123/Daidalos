// Does the Windows keyboard actually arrive as the engine's key codes?
//
// The editor used to ask for keys as X11 keysyms, which is fine on X11 and
// meaningless on Windows, where the window procedure is handed virtual key
// codes. Making dai_key the engine's own vocabulary and translating inside each
// backend is the fix - but "it compiles" says nothing about whether pressing W
// reaches the host as DAI_KEY_W.
//
// So this presses the keys. Messages are posted straight to the window, which
// runs them through the real WM_KEYDOWN path and the real translation table,
// without stealing focus from whatever the user is doing - SendInput would have
// grabbed the desktop to test a lookup table.
//
//   win_keytest.exe        -> prints one line per key, exits non zero on a miss

#include "dai_render.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;

static void pump(dai_window *w) {
    // Two rounds: one to let the posted message reach the procedure, one to be
    // sure a message posted during handling is drained too.
    for (int i = 0; i < 2; ++i) {
        dai_window_poll(w);
        Sleep(1);
    }
}

static void check(dai_window *w, HWND hwnd, const char *name, WPARAM vk, uint32_t expect,
                  uint32_t also_expect) {
    PostMessageW(hwnd, WM_KEYDOWN, vk, 0);
    pump(w);
    const int down = dai_window_key_down(w, expect);
    const int down2 = also_expect ? dai_window_key_down(w, also_expect) : 1;

    PostMessageW(hwnd, WM_KEYUP, vk, 0);
    pump(w);
    const int up = dai_window_key_down(w, expect);

    const bool ok = down && down2 && !up;
    if (ok) ++g_pass; else ++g_fail;
    std::printf("  %-12s vk 0x%02X -> 0x%04X  down=%d up=%d  %s\n",
                name, (unsigned)vk, expect, down, up, ok ? "ok" : "MISS");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("win_keytest\n");

    char err[256] = { 0 };
    dai_render_desc rd{};
    rd.width = 320;
    rd.height = 200;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("FAIL renderer: %s\n", err); return 1; }

    const char *title = "Daidalos key test";
    dai_window *w = dai_window_open(r, title, 320, 200, err, sizeof(err));
    if (!w) { std::printf("FAIL window: %s\n", err); dai_render_destroy(r); return 1; }

    HWND hwnd = FindWindowW(nullptr, L"Daidalos key test");
    if (!hwnd) {
        std::printf("FAIL: cannot find the window that was just opened\n");
        dai_window_close(w); dai_render_destroy(r);
        return 1;
    }

    // The keys the editor actually binds.
    check(w, hwnd, "W",        'W',         DAI_KEY_W, 0);
    check(w, hwnd, "A",        'A',         DAI_KEY_A, 0);
    check(w, hwnd, "S",        'S',         DAI_KEY_S, 0);
    check(w, hwnd, "D",        'D',         DAI_KEY_D, 0);
    check(w, hwnd, "Q",        'Q',         DAI_KEY_Q, 0);
    check(w, hwnd, "E",        'E',         DAI_KEY_E, 0);
    check(w, hwnd, "R",        'R',         DAI_KEY_R, 0);
    check(w, hwnd, "F",        'F',         DAI_KEY_F, 0);
    check(w, hwnd, "Y",        'Y',         DAI_KEY_Y, 0);
    check(w, hwnd, "Z",        'Z',         DAI_KEY_Z, 0);
    check(w, hwnd, "space",    VK_SPACE,    DAI_KEY_SPACE, 0);
    check(w, hwnd, "delete",   VK_DELETE,   DAI_KEY_DELETE, 0);

    // The side-less modifiers must answer for both sides, because a host asking
    // "is shift held" should not have to know which one the user hit.
    check(w, hwnd, "shift",    VK_SHIFT,    DAI_KEY_SHIFT_L, DAI_KEY_SHIFT_R);
    check(w, hwnd, "ctrl",     VK_CONTROL,  DAI_KEY_CTRL_L,  DAI_KEY_CTRL_R);
    check(w, hwnd, "alt",      VK_MENU,     DAI_KEY_ALT_L,   DAI_KEY_ALT_R);
    check(w, hwnd, "shift(L)", VK_LSHIFT,   DAI_KEY_SHIFT_L, 0);
    check(w, hwnd, "ctrl(R)",  VK_RCONTROL, DAI_KEY_CTRL_R,  0);

    // A key nobody mapped must not land on top of one that is mapped - the slot
    // is a hash, and a collision would make an unrelated key press W.
    PostMessageW(hwnd, WM_KEYDOWN, VK_F12, 0);
    pump(w);
    const bool clean = !dai_window_key_down(w, DAI_KEY_W) && !dai_window_key_down(w, DAI_KEY_SPACE);
    if (clean) ++g_pass; else ++g_fail;
    std::printf("  %-12s          -> unmapped, touches nothing  %s\n", "F12", clean ? "ok" : "MISS");
    PostMessageW(hwnd, WM_KEYUP, VK_F12, 0);
    pump(w);

    dai_window_close(w);
    dai_render_destroy(r);

    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "ok", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
