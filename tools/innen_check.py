#!/usr/bin/env python3
"""INNEN M0 — the first three rooms, checked through the bridge.

    tools/innen_check.py [--binary build/editor_demo] [--port 8397]

It starts the real editor with `DAI_BRIDGE_PORT`, sends
`examples/scripts/innen_m0.js` down the socket — the same way Jarvis builds a
room — and then asks the DOCUMENT what was built. Nothing is read out of the
reply the script wrote itself: every number comes back out of the scene.

Why these checks and not others:

  * **The rooms dock.** The generator of the game will place a room by putting
    its door socket on somebody else's door socket. If two neighbours' sockets
    are not at the same point in the world, in metres, with the same opening
    and opposite normals, then walking through a door lands in a wall. This is
    the one invariant the whole room graph rests on, so it is the first test.
  * **The rooms do not overlap.** Three boxes in a line, checked as boxes:
    a generator that places rooms by socket has to be told, in a test, that
    "next to" means "not inside".
  * **The doorway is a hole.** A wall with an opening is a CSG subtraction with
    a slab and a cut under it — not a solid wall that merely has a socket
    floating in front of it, which is what a broken build degrades into.
  * **One script is one undo.** Ctrl-Z after "build me the first three rooms"
    has to remove three rooms and not the last skirting board.

It prints "ok: N checks, 0 failures", the shape tools/run_tests.sh counts.
"""

import argparse
import json
import os
import subprocess
import sys
import socket as pysocket
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from daibridge import Bridge          # noqa: E402

PASS = 0
FAIL = 0


def check(cond, message):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print("  FAIL %s" % message)


def wait_for_port(port, proc, seconds=40.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            s = pysocket.create_connection(("127.0.0.1", port), 0.5)
            s.close()
            return True
        except (pysocket.error, OSError):
            time.sleep(0.2)
    return False


def start(binary, port):
    """Start a host with the bridge open.

    Two hosts can answer: examples/editor_demo (a window, so it needs a
    screen) and tools/modeling_shot (headless, --serve). Both put the same
    seams together, so the check runs against either - which is what lets it
    run on a machine with no X at all.
    """
    env = dict(os.environ)
    env.setdefault("DAI_SHADER_DIR", "shaders")
    env["DAI_BRIDGE_PORT"] = str(port)
    argv = [binary]
    if os.path.basename(binary) == "modeling_shot":
        argv += ["/tmp", "320", "200", "--serve", "180", "--bridge", str(port)]
    return subprocess.Popen(argv, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)


def jsjson(b, code):
    """Run JS that returns a JSON string, and give back the object."""
    answer = b.js(code)
    if not answer.get("ok"):
        return None, answer.get("error")
    try:
        return json.loads(answer.get("result") or ""), None
    except ValueError as e:
        return None, "%s: %r" % (e, answer.get("result"))


# The world position of a node whose parent is a room group: rooms carry no
# rotation and no scale, so a sum is the whole transform. Written as JS because
# the DOCUMENT has to answer, not this file's idea of the script.
POSE_JS = """
(function () {
  // node.getPos and node.getVec answer ARRAYS, not objects with .x - the
  // check reads them the way the API returns them, never the way it might.
  function pos(n) { var p = node.getPos(n); return [p[0], p[1], p[2]]; }
  var out = { rooms: {}, sockets: [] };
  var names = ['Zelle', 'Flur', 'Halle'];
  for (var i = 0; i < names.length; i++) {
    var r = scene.find(names[i]);
    if (r) out.rooms[names[i]] = pos(r);
  }
  // No node.getName in the script API, so the sockets are asked for BY NAME -
  // which is fine, because those names are the contract the generator will
  // dock against, and a renamed socket has to fail this test.
  var want = ['Zelle.Door.front0', 'Flur.Door.back0', 'Flur.Door.front0',
              'Flur.Door.left0', 'Flur.Door.left1', 'Flur.Door.right0',
              'Halle.Door.back0', 'Halle.Door.left0', 'Halle.Door.front0'];
  for (var k = 0; k < want.length; k++) {
    var n = scene.find(want[k]);
    if (!n) continue;
    var room = want[k].split('.')[0];
    var base = out.rooms[room] || [0, 0, 0];
    var p = pos(n);
    var nv = node.getVec(n, 'door.normal');
    out.sockets.push({
      name: want[k],
      world: [base[0] + p[0], base[1] + p[1], base[2] + p[2]],
      width: node.getNum(n, 'door.width'),
      height: node.getNum(n, 'door.height'),
      normal: [nv[0], nv[1], nv[2]]
    });
  }
  return JSON.stringify(out);
})();
"""


def near(a, b, eps=0.002):
    return abs(a - b) <= eps


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/editor_demo")
    ap.add_argument("--port", type=int, default=8397)
    ap.add_argument("--script", default="examples/scripts/innen_m0.js")
    args = ap.parse_args(argv[1:])

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    if not os.path.exists(args.binary):
        print("innen_check: %s is not built" % args.binary)
        return 2

    proc = start(args.binary, args.port)
    if not wait_for_port(args.port, proc):
        out = proc.stdout.read().decode("utf-8", "replace") if proc.stdout else ""
        print("innen_check: the editor never opened the bridge\n%s" % out[-800:])
        stop(proc)
        return 2

    try:
        b = Bridge(args.port)
        before = b.ping().get("nodes", 0)

        with open(args.script) as f:
            src = f.read()
        answer = b.js(src)
        check(answer.get("ok") is True,
              "the M0 script failed: %r" % answer.get("error"))
        if not answer.get("ok"):
            return 1
        built = json.loads(answer.get("result"))
        check(len(built.get("rooms", [])) == 3,
              "the script did not build three rooms: %r" % built.get("rooms"))
        check(built.get("csg") and built.get("socket"),
              "this build has no CSG or no door socket, so the rooms are boxes: %r" % built)

        after = b.ping().get("nodes", 0)
        check(after > before + 40,
              "only %d nodes arrived, which is not three rooms" % (after - before))

        # ---- the sockets ------------------------------------------------
        pose, err = jsjson(b, POSE_JS)
        check(pose is not None, "the document could not be asked about its sockets: %s" % err)
        if pose is None:
            return 1
        sockets = pose["sockets"]
        check(len(sockets) >= 7, "only %d door sockets are in the scene" % len(sockets))

        by_name = dict((s["name"], s) for s in sockets)
        # The three pairs that make the walk: box -> hallway -> hall.
        pairs = [("Zelle.Door.front0", "Flur.Door.back0"),
                 ("Flur.Door.front0", "Halle.Door.back0")]
        for a, c in pairs:
            sa, sc = by_name.get(a), by_name.get(c)
            check(sa is not None and sc is not None,
                  "a socket of the pair %s / %s is missing" % (a, c))
            if not (sa and sc):
                continue
            same = all(near(x, y) for x, y in zip(sa["world"], sc["world"]))
            check(same, "%s at %s and %s at %s are not the same doorway"
                  % (a, sa["world"], c, sc["world"]))
            check(near(sa["width"], sc["width"]) and near(sa["height"], sc["height"]),
                  "%s is %.3fx%.3f and %s is %.3fx%.3f - one of them is a wall"
                  % (a, sa["width"], sa["height"], c, sc["width"], sc["height"]))
            opposite = all(near(x, -y) for x, y in zip(sa["normal"], sc["normal"]))
            check(opposite, "%s %s and %s %s do not face each other"
                  % (a, sa["normal"], c, sc["normal"]))
            check(sa["width"] > 0.6 and sa["height"] > 1.8,
                  "the opening %s is %.2f x %.2f - nobody fits through that"
                  % (a, sa["width"], sa["height"]))

        # ---- the rooms do not stand inside each other -------------------
        dims = {"Zelle": (1.30, 1.30), "Flur": (1.70, 9.00), "Halle": (9.00, 7.00)}
        boxes = {}
        for name, (w, d) in dims.items():
            p = pose["rooms"].get(name)
            check(p is not None, "room %s is not in the scene" % name)
            if p:
                boxes[name] = (p[0] - w / 2, p[0] + w / 2, p[2] - d / 2, p[2] + d / 2)
        names = sorted(boxes)
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                a, c = boxes[names[i]], boxes[names[j]]
                overlap = (a[0] < c[1] - 0.01 and c[0] < a[1] - 0.01 and
                           a[2] < c[3] - 0.01 and c[2] < a[3] - 0.01)
                check(not overlap, "%s and %s occupy the same floor space"
                      % (names[i], names[j]))

        # ---- a doorway is a hole, not a promise -------------------------
        hole, err = jsjson(b, """
        (function () {
          var w = scene.find('Flur.Wall.Back');
          if (!w) return JSON.stringify({found: false});
          return JSON.stringify({
            found: true,
            csg: node.getNum(w, 'csg.op'),
            slab: !!scene.find('Flur.Wall.Back.Slab'),
            cut: !!scene.find('Flur.Wall.Back.Opening0')
          });
        })();
        """)
        check(hole is not None and hole.get("found"),
              "the hallway has no back wall: %s" % err)
        if hole and hole.get("found"):
            check(hole.get("csg") == 2,
                  "the wall with the door is csg.op %r, not a subtraction" % hole.get("csg"))
            check(hole.get("slab") and hole.get("cut"),
                  "the subtraction has no slab or no opening under it: %r" % hole)

        # ---- one script, one undo ---------------------------------------
        u = b.undo()
        check(u.get("moved") is True, "undo did nothing after a whole level was built")
        check(b.ping().get("nodes", -1) == before,
              "one Ctrl-Z did not remove the three rooms: %d nodes left"
              % (b.ping().get("nodes", -1) - before))
        r = b.redo()
        check(r.get("moved") is True, "redo did not bring the rooms back")
        check(b.ping().get("nodes", -1) == after,
              "redo rebuilt a different number of nodes")

        b.close()
    finally:
        stop(proc)

    print("ok: %d checks, %d failures" % (PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
