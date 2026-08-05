#!/usr/bin/env python3
# patch69 - rotating one axis stops moving the other two.
import sys, os
ROOT = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(os.path.join(ROOT, p), 'r', encoding='utf-8') as f: return f.read()
def wr(p, s):
    full = os.path.join(ROOT, p); bak = full + '.bak_p69'
    if not os.path.exists(bak):
        with open(bak, 'w', encoding='utf-8') as f: f.write(rd(p))
    with open(full, 'w', encoding='utf-8') as f: f.write(s)
def sub1(t, old, new, what):
    if new in t: print('skip (already applied): ' + what); return t
    if old not in t: print('MISS: ' + what); sys.exit(1)
    if t.count(old) != 1: print('AMBIG %d: %s' % (t.count(old), what)); sys.exit(1)
    return t.replace(old, new)

s = rd('src/dai_editor_ui.cpp')

# A quaternion does not have "an" Euler triple - it has infinitely many, in two
# families, each shifted by whole turns:
#
#     (roll, pitch, yaw)  ==  (roll+180, 180-pitch, yaw+180)  ==  (+360k on any)
#
# atan2/asin return one particular member of one family, and which one flips as
# the object turns. So dragging ONE gizmo axis rewrote all three numbers, even
# though the orientation had only changed about one of them. Nothing was wrong
# with the rotation; the readout was jumping between equally correct spellings
# of it.
#
# The fix is to pick the spelling nearest the one already on screen: both
# families, each angle unwrapped to the closest equivalent, and whichever total
# is smallest wins. It cannot make Euler angles unambiguous - nothing can, that
# is what gimbal lock IS - but it makes the numbers move only as far as the
# object did.
s = sub1(s,
"""static dai_quat euler_to_quat(const float *deg) {""",
"""// The nearest spelling to `near_deg` of the same orientation. See above.
static void quat_to_euler_near(dai_quat q, const float *near_deg, float *deg) {
    float a[3];
    quat_to_euler(q, a);
    // The other family: flip roll and yaw by half a turn, mirror pitch.
    float b[3] = { a[0] + 180.0f, 180.0f - a[1], a[2] + 180.0f };

    auto unwrap = [](float v, float target) {
        // v + 360k, k chosen so the result is within half a turn of target.
        float d = v - target;
        float k = d / 360.0f;
        k = k >= 0.0f ? std::floor(k + 0.5f) : std::ceil(k - 0.5f);
        return v - k * 360.0f;
    };
    float cost_a = 0.0f, cost_b = 0.0f;
    for (int i = 0; i < 3; ++i) {
        a[i] = unwrap(a[i], near_deg[i]);
        b[i] = unwrap(b[i], near_deg[i]);
        cost_a += std::fabs(a[i] - near_deg[i]);
        cost_b += std::fabs(b[i] - near_deg[i]);
    }
    const float *win = cost_b + 0.001f < cost_a ? b : a;
    for (int i = 0; i < 3; ++i) deg[i] = win[i];
}

static dai_quat euler_to_quat(const float *deg) {""",
    'quat_to_euler_near')

s = sub1(s,
"""        if (p->euler_node != n || !quat_eq(p->euler_cached_q, rot)) {
            p->euler_node = n;
            p->euler_cached_q = rot;
            quat_to_euler(rot, p->euler_deg);
        }""",
"""        if (p->euler_node != n) {
            // A different object: no previous reading to stay near.
            p->euler_node = n;
            p->euler_cached_q = rot;
            quat_to_euler(rot, p->euler_deg);
        } else if (!quat_eq(p->euler_cached_q, rot)) {
            // The same object, turned from somewhere else - the gizmo, an
            // undo, the simulation. Re-read it in the spelling closest to what
            // is already on screen, so one axis of gizmo drag moves one field.
            p->euler_cached_q = rot;
            quat_to_euler_near(rot, p->euler_deg, p->euler_deg);
        }""",
    'nearest euler on refresh')
wr('src/dai_editor_ui.cpp', s)
print('patch69 ok')
