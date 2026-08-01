// Key codes are the engine's own vocabulary now (dai_key in dai_render.h),
// and they are DEFINED to be the X11 keysym values - which is what lets the X11
// backend do no translation at all while Win32 maps onto them.
//
// That claim is load bearing and invisible: if a value drifts, nothing fails to
// compile, the X11 backend just quietly stops matching and the editor's keys die
// on Linux. So it gets asserted against the real X11 headers.
//
//   ./build/test_keys

#include "dai_render.h"

#include <X11/keysym.h>
#include <cstdio>
#define EQ(a,b) do { if ((unsigned)(a) != (unsigned)(b)) { \
    std::printf("  MISMATCH %s=0x%X vs %s=0x%X\n", #a,(unsigned)(a),#b,(unsigned)(b)); ++bad; } \
    else ++good; } while(0)
int main(){ int bad=0, good=0;
  EQ(DAI_KEY_W, XK_w); EQ(DAI_KEY_A, XK_a); EQ(DAI_KEY_S, XK_s); EQ(DAI_KEY_D, XK_d);
  EQ(DAI_KEY_Q, XK_q); EQ(DAI_KEY_E, XK_e); EQ(DAI_KEY_R, XK_r); EQ(DAI_KEY_F, XK_f);
  EQ(DAI_KEY_G, XK_g); EQ(DAI_KEY_X, XK_x); EQ(DAI_KEY_Y, XK_y); EQ(DAI_KEY_Z, XK_z);
  EQ(DAI_KEY_0, XK_0); EQ(DAI_KEY_9, XK_9);
  EQ(DAI_KEY_SPACE, XK_space); EQ(DAI_KEY_ESCAPE, XK_Escape); EQ(DAI_KEY_TAB, XK_Tab);
  EQ(DAI_KEY_RETURN, XK_Return); EQ(DAI_KEY_BACKSPACE, XK_BackSpace);
  EQ(DAI_KEY_DELETE, XK_Delete); EQ(DAI_KEY_LEFT, XK_Left); EQ(DAI_KEY_UP, XK_Up);
  EQ(DAI_KEY_RIGHT, XK_Right); EQ(DAI_KEY_DOWN, XK_Down);
  EQ(DAI_KEY_F1, XK_F1); EQ(DAI_KEY_F5, XK_F5);
  EQ(DAI_KEY_SHIFT_L, XK_Shift_L); EQ(DAI_KEY_SHIFT_R, XK_Shift_R);
  EQ(DAI_KEY_CTRL_L, XK_Control_L); EQ(DAI_KEY_CTRL_R, XK_Control_R);
  EQ(DAI_KEY_ALT_L, XK_Alt_L); EQ(DAI_KEY_ALT_R, XK_Alt_R);
  std::printf("%s: %d key codes match X11 exactly, %d wrong\n", bad?"FAILED":"ok", good, bad);
  return bad?1:0; }
