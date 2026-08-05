# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'src/dai_editor.cpp'
s = rw(P)

# #43: a click into empty space must CLEAR the selection, and the camera's
# "handle" was big enough to catch clicks that were plainly on the sky.
s = sub1(s,
"""        // What the user sees: the render box when the collider was detached
        // from it, the collider otherwise, and a small handle for things that
        // have neither (cameras, lights, empties) so they are clickable at all.
        dai_vec3 half = r.half_extent;
        if (r.render_extent.x || r.render_extent.y || r.render_extent.z)
            half = r.render_extent;
        if (half.x <= 0.0f && half.y <= 0.0f && half.z <= 0.0f)
            half = dai_vec3{ 0.25f, 0.25f, 0.25f };""",
"""        // What the user sees: the render box when the collider was detached
        // from it, the collider otherwise, and a small handle for things that
        // have neither (cameras, lights, empties) so they are clickable at all.
        //
        // Small on purpose: a 0.25 m handle around a camera sitting in the
        // middle of the skybox eats every click near the horizon - which is
        // exactly what "clicking nothing selects the camera" was. The handle
        // is for clicking the icon, not the quadrant it hangs in.
        dai_vec3 half = r.half_extent;
        if (r.render_extent.x || r.render_extent.y || r.render_extent.z)
            half = r.render_extent;
        if (half.x <= 0.0f && half.y <= 0.0f && half.z <= 0.0f)
            half = dai_vec3{ 0.12f, 0.12f, 0.12f };""", "pick handle smaller")

wr(P, s)
print("patch25 done")
