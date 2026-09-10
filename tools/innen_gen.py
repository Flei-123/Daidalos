#!/usr/bin/env python3
"""The INNEN room generator, checked against the floor it actually built.

    tools/innen_gen.py [--binary build/modeling_shot] [--port 8398] [--seed 7]

`examples/scripts/innen_gen.js` grows a floor from the phone box: pick an open
door socket, draw a room type for that zone, dock it, repeat. This starts a
host with the bridge open, sends the library and the generator, and then asks
the DOCUMENT what stands there - never the generator's own report on its own
work, except as the claim being tested.

What it checks, and why each one is the check that catches a real failure:

  * **The same seed is the same floor.** A generator nobody can reproduce
    cannot be debugged: "it put a room inside another one" is not a bug report
    unless the room comes back. Two runs of one seed must agree down to the
    centre of every room, and two different seeds must not.
  * **It does not stall.** Rooms grow off open sockets, and a run of dead ends
    ends the house at three rooms. Asking for fourteen has to produce nearly
    fourteen.
  * **No room stands inside another.** Checked as rectangles, from the
    document's own node positions, not from the plan - the plan believing
    itself is what a bug looks like from the inside.
  * **Every door docks.** For each edge, the two sockets have to sit at the
    same point in the world with opposite normals and the same opening: the
    one contract of docs/INNEN_M0.md, now generated instead of typed.
  * **Everything is reachable.** A floor with an island in it is a floor the
    player cannot walk; the graph is walked breadth first from the phone box.
  * **Zones grow.** Depth rises by exactly one across every edge that is not a
    loop, because the zone tables of GDD 5.1 are indexed by it.
  * **One script is one undo.**

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
from daibridge import Bridge, read_sources          # noqa: E402

PASS = 0
FAIL = 0

LIB = "examples/scripts/innen_lib.js"
GEN = "examples/scripts/innen_gen.js"


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
    env = dict(os.environ)
    env.setdefault("DAI_SHADER_DIR", "shaders")
    env["DAI_BRIDGE_PORT"] = str(port)
    argv = [binary]
    if os.path.basename(binary) == "modeling_shot":
        argv += ["/tmp", "320", "200", "--serve", "300", "--bridge", str(port)]
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


def generate(b, seed, rooms, player=True, plan_only=False):
    """Run the generator once and give back its plan."""
    head = "var GEN = { seed: %d, rooms: %d, player: %s, planOnly: %s };\n" % (
        seed, rooms, "true" if player else "false", "true" if plan_only else "false")
    answer = b.js(head + read_sources([LIB, GEN]))
    if not answer.get("ok"):
        return None, answer.get("error")
    try:
        return json.loads(answer.get("result") or ""), None
    except ValueError as e:
        return None, "%s (%d characters came back)" % (e, len(answer.get("result") or ""))


# What the DOCUMENT says about the rooms and sockets the plan claims. Room
# groups carry no rotation and no scale, so a sum is the whole transform.
POSE_JS = """
(function (names) {
  var out = { rooms: {}, sockets: {}, doors: 0, player: false };
  for (var i = 0; i < names.length; i++) {
    var r = scene.find(names[i]);
    if (r < 0) continue;
    var p = node.getPos(r);
    out.rooms[names[i]] = [p[0], p[1], p[2]];
  }
  var n = editor.count();
  for (var k = 0; k < n; k++) {
    var id = editor.at(k);
    var nm = node.getStr(id, 'node.name');
    if (nm.indexOf('.Door.') > 0) {
      var q = node.getPos(id);
      var nv = node.getVec(id, 'door.normal');
      var room = nm.split('.')[0];
      var base = out.rooms[room] || [0, 0, 0];
      out.sockets[nm] = {
        world: [base[0] + q[0], base[1] + q[1], base[2] + q[2]],
        normal: [nv[0], nv[1], nv[2]],
        width: node.getNum(id, 'door.width'),
        height: node.getNum(id, 'door.height')
      };
    } else if (nm.indexOf('Tuer') === 0 && nm.indexOf('.Klinke') < 0) {
      out.doors++;
    } else if (nm === 'Spieler') {
      out.player = true;
    }
  }
  return JSON.stringify(out);
})(%s);
"""


def rect(room):
    """The inside of a room, as (x0, x1, z0, z1)."""
    cx, cz = room["centre"]
    w, d = room["size"][0], room["size"][1]
    return (cx - w / 2.0, cx + w / 2.0, cz - d / 2.0, cz + d / 2.0)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/modeling_shot")
    ap.add_argument("--port", type=int, default=8398)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rooms", type=int, default=14)
    args = ap.parse_args(argv[1:])

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    if not os.path.exists(args.binary):
        print("innen_gen: %s is not built" % args.binary)
        return 2

    proc = start(args.binary, args.port)
    if not wait_for_port(args.port, proc):
        out = proc.stdout.read().decode("utf-8", "replace") if proc.stdout else ""
        print("innen_gen: the host never opened the bridge\n%s" % out[-800:])
        stop(proc)
        return 2

    try:
        b = Bridge(args.port, timeout=120.0)
        before = b.ping().get("nodes", 0)

        plan, err = generate(b, args.seed, args.rooms)
        check(plan is not None, "the generator failed: %s" % err)
        if plan is None:
            return 1

        rooms = plan["rooms"]
        edges = plan["edges"]
        check(len(rooms) >= args.rooms - 3,
              "asked for %d rooms and got %d - the growth stalled (refused: %r)"
              % (args.rooms, len(rooms), plan.get("refused")))
        check(rooms[0]["type"] == "Vorraum" and rooms[0]["centre"] == [0, 0],
              "the floor does not start at the phone box: %r" % rooms[0])

        # ---- the same seed is the same floor -----------------------------
        b.undo()
        again, err2 = generate(b, args.seed, args.rooms)
        check(again is not None, "the second run failed: %s" % err2)
        if again:
            same = [(r["name"], r["centre"], r["size"]) for r in again["rooms"]]
            first = [(r["name"], r["centre"], r["size"]) for r in rooms]
            check(same == first,
                  "seed %d built a different floor the second time: %d vs %d "
                  "rooms, first difference %r"
                  % (args.seed, len(same), len(first),
                     next((a for a, c in zip(same, first) if a != c), None)))
        b.undo()

        other, err3 = generate(b, args.seed + 1, args.rooms)
        check(other is not None, "the other seed failed: %s" % err3)
        if other:
            check([r["type"] for r in other["rooms"]] != [r["type"] for r in rooms] or
                  [r["centre"] for r in other["rooms"]] != [r["centre"] for r in rooms],
                  "seed %d and seed %d built the same floor - the seed does "
                  "nothing" % (args.seed, args.seed + 1))
        b.undo()

        # Back to the floor under test, and this time the document is asked.
        plan, err = generate(b, args.seed, args.rooms)
        check(plan is not None, "the third run failed: %s" % err)
        if plan is None:
            return 1
        rooms, edges = plan["rooms"], plan["edges"]

        names = json.dumps([r["name"] for r in rooms])
        answer = b.js(POSE_JS % names)
        check(answer.get("ok") is True,
              "the document could not be asked about the floor: %r" % answer.get("error"))
        doc = json.loads(answer["result"]) if answer.get("ok") else {"rooms": {}, "sockets": {}}

        # ---- the document agrees with the plan ---------------------------
        for r in rooms:
            p = doc["rooms"].get(r["name"])
            check(p is not None, "room %s is not in the scene" % r["name"])
            if p:
                check(abs(p[0] - r["centre"][0]) < 0.002 and
                      abs(p[2] - r["centre"][1]) < 0.002,
                      "%s is planned at %r and stands at %r"
                      % (r["name"], r["centre"], [p[0], p[2]]))
        check(doc.get("player") is True, "the generated floor has no player in it")
        check(doc.get("doors", 0) >= len(edges),
              "%d door leaves for %d doorways" % (doc.get("doors", 0), len(edges)))

        # ---- no room stands inside another -------------------------------
        worst = None
        for i in range(len(rooms)):
            for j in range(i + 1, len(rooms)):
                a, c = rect(rooms[i]), rect(rooms[j])
                ox = min(a[1], c[1]) - max(a[0], c[0])
                oz = min(a[3], c[3]) - max(a[2], c[2])
                if ox > 0.02 and oz > 0.02:
                    worst = (rooms[i]["name"], rooms[j]["name"], round(ox, 2), round(oz, 2))
        check(worst is None,
              "two rooms occupy the same floor space: %r" % (worst,))

        # ---- every door docks --------------------------------------------
        by_room = dict((i, r) for i, r in enumerate(rooms))
        docked = 0
        for e in edges:
            if e["b"] < 0:
                continue          # a doorway into a wall: nothing to dock onto
            a_name = by_room[e["a"]]["name"]
            b_name = by_room[e["b"]]["name"]
            pair = [s for n, s in doc["sockets"].items()
                    if (n.startswith(a_name + ".Door.") or n.startswith(b_name + ".Door."))
                    and abs(s["world"][0] - e["world"][0]) < 0.02
                    and abs(s["world"][2] - e["world"][1]) < 0.02]
            check(len(pair) == 2,
                  "the doorway between %s and %s has %d sockets at %r, not 2"
                  % (a_name, b_name, len(pair), e["world"]))
            if len(pair) == 2:
                n1, n2 = pair[0]["normal"], pair[1]["normal"]
                check(abs(n1[0] + n2[0]) < 0.01 and abs(n1[2] + n2[2]) < 0.01,
                      "%s and %s meet but face the same way: %r / %r"
                      % (a_name, b_name, n1, n2))
                check(abs(pair[0]["width"] - pair[1]["width"]) < 0.01 and
                      abs(pair[0]["height"] - pair[1]["height"]) < 0.01,
                      "the two sides of the door between %s and %s are "
                      "different sizes" % (a_name, b_name))
                docked += 1
        check(docked >= len(rooms) - 1,
              "only %d doorways connect %d rooms" % (docked, len(rooms)))

        # ---- everything is reachable -------------------------------------
        adj = dict((i, []) for i in range(len(rooms)))
        for e in edges:
            if e["b"] >= 0:
                adj[e["a"]].append(e["b"])
                adj[e["b"]].append(e["a"])
        seen, queue = set([0]), [0]
        while queue:
            cur = queue.pop(0)
            for nxt in adj[cur]:
                if nxt not in seen:
                    seen.add(nxt)
                    queue.append(nxt)
        missing = [rooms[i]["name"] for i in range(len(rooms)) if i not in seen]
        check(not missing,
              "these rooms cannot be walked to from the phone box: %r" % missing)

        # ---- zones grow ---------------------------------------------------
        for e in edges:
            if e["b"] < 0 or e.get("loop"):
                continue
            da, db = rooms[e["a"]]["depth"], rooms[e["b"]]["depth"]
            check(abs(da - db) == 1,
                  "%s (zone %d) and %s (zone %d) are neighbours - a door has "
                  "to change the zone by exactly one"
                  % (rooms[e["a"]]["name"], da, rooms[e["b"]]["name"], db))
        deepest = max(r["depth"] for r in rooms)
        check(deepest >= 3,
              "the deepest room is zone %d - the floor never leaves the flat"
              % deepest)

        # ---- loops exist at all ------------------------------------------
        # A tree of rooms is a house you can only walk one way through. Loops
        # are meant to be rare - GDD 5.1 says about a tenth of the doors - so
        # one seed proving nothing is expected; four seeds proving nothing
        # means the stitching is dead code.
        loop_seeds = 0
        for extra in range(4):
            b.undo()
            other_plan, _ = generate(b, args.seed + 100 + extra, args.rooms,
                                     player=False, plan_only=True)
            if other_plan and other_plan.get("loops", 0) > 0:
                loop_seeds += 1
        check(loop_seeds > 0,
              "not one of four seeds produced a single loop - a generated "
              "floor is a tree, and a tree can only be walked one way")
        b.undo()
        plan, err = generate(b, args.seed, args.rooms)
        check(plan is not None, "the final run failed: %s" % err)

        # ---- one script, one undo ----------------------------------------
        after = b.ping().get("nodes", 0)
        check(after > before + 100, "only %d nodes arrived" % (after - before))
        u = b.undo()
        check(u.get("moved") is True, "undo did nothing after a whole floor")
        check(b.ping().get("nodes", -1) == before,
              "one Ctrl-Z did not remove the floor: %d nodes left"
              % (b.ping().get("nodes", -1) - before))

        print("seed %d: %d rooms, %d doorways (%d blocked, %d loops), "
              "deepest zone %d, %d nodes"
              % (args.seed, len(rooms), len(edges),
                 sum(1 for e in edges if e["blocked"]), plan.get("loops", 0),
                 deepest, plan.get("nodes", 0)))
        b.close()
    finally:
        stop(proc)

    print("ok: %d checks, %d failures" % (PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
