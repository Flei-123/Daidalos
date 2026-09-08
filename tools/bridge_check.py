#!/usr/bin/env python3
"""The Jarvis bridge, proved end to end against the REAL editor.

    tools/bridge_check.py [--binary build/editor_demo] [--port 8391]

It starts the editor with `DAI_BRIDGE_PORT` set - the only way the socket ever
opens - talks to it over TCP with tools/daibridge.py, and checks what came
back. Nothing here reaches into the process it is testing: everything goes
through the socket, which is the whole point of having one.

What it asserts, and why each one is here:

  * the socket is CLOSED without the environment variable. A modelling socket
    that is on by default is a hole, so "off" is a test and not a promise.
  * it answers on 127.0.0.1 and NOT on the machine's own address, so nothing
    on the network can reach it.
  * "make a box 2 x 3 x 1" through the JS API produces a node with exactly
    those measurements - read back out of the document, not out of the reply.
  * one bridge command is one undo step: undo removes the box, redo brings it
    back, and the undo depth counts what actually happened.
  * a script error is an error message, not a dead editor: the next command
    still works.
  * the protocol survives junk - a line that is not JSON, an unknown command,
    an empty line - because a tool that crashes an editor with a typo is not a
    tool anybody will leave running.

It prints "ok: N checks, 0 failures" in the shape tools/run_tests.sh counts.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
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
            s = socket.create_connection(("127.0.0.1", port), 0.5)
            s.close()
            return True
        except (socket.error, OSError):
            time.sleep(0.2)
    return False


def start(binary, env_extra, args):
    env = dict(os.environ)
    env.setdefault("DAI_SHADER_DIR", "shaders")
    env.update(env_extra)
    return subprocess.Popen([binary] + list(args), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=15)
    out = b""
    if proc.stdout:
        try:
            out = proc.stdout.read() or b""
        except (IOError, ValueError):
            out = b""
    return out.decode("utf-8", "replace")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/editor_demo")
    ap.add_argument("--port", type=int, default=8391)
    args = ap.parse_args(argv)

    if not os.path.exists(args.binary):
        print("bridge_check: %s is not built - nothing to check" % args.binary)
        return 1

    # ---- 1. off by default -----------------------------------------------
    proc = start(args.binary, {}, [])
    time.sleep(6.0)
    closed = False
    try:
        s = socket.create_connection(("127.0.0.1", args.port), 1.0)
        s.close()
    except (socket.error, OSError):
        closed = True
    log = stop(proc)
    check(closed, "the bridge answered on port %d although nothing asked for it" % args.port)
    check("bridge: listening" not in log,
          "the editor announced a bridge it was never told to open")

    # ---- 2. on, because it was told to -----------------------------------
    proc = start(args.binary, {"DAI_BRIDGE_PORT": str(args.port)}, [])
    if not wait_for_port(args.port, proc):
        log = stop(proc)
        print("  FAIL the bridge never opened on port %d" % args.port)
        print(log[-2000:])
        print("\nFAILED: %d checks, %d failures" % (PASS + 1, FAIL + 1))
        return 1

    b = Bridge(args.port)
    try:
        pong = b.ping()
        check(pong.get("ok") is True, "ping was not answered with ok")
        check(pong.get("version") == 1, "the protocol version is not 1: %r" % pong.get("version"))
        check(pong.get("script") is True, "this editor has no script runtime, so nothing can be built")
        nodes_before = pong.get("nodes", 0)

        # ---- 3. not reachable from the network ---------------------------
        # The same port on the machine's own address must NOT answer: the
        # listener is bound to 127.0.0.1 and to nothing else.
        outside = None
        try:
            outside = socket.gethostbyname(socket.gethostname())
        except (socket.error, OSError):
            outside = None
        if outside and not outside.startswith("127."):
            reachable = True
            try:
                s = socket.create_connection((outside, args.port), 1.0)
                s.close()
            except (socket.error, OSError):
                reachable = False
            check(not reachable,
                  "the bridge answered on %s - it must be loopback only" % outside)

        # ---- 4. "make me a box 2 x 3 x 1" --------------------------------
        answer = b.js("var b = editor.add('BridgeBox');"
                      " node.setVec(b, 'transform.scale', 2, 3, 1);"
                      " node.setPos(b, 1.5, 0.5, -2);"
                      " b")
        check(answer.get("ok") is True, "the box script failed: %r" % answer.get("error"))
        made = answer.get("result", "")
        check(made.isdigit() and int(made) > 0, "the script did not answer a node id: %r" % made)

        box = b.node("BridgeBox")
        check(box is not None, "no node called BridgeBox is in the document")
        if box:
            check(box["scale"] == [2, 3, 1],
                  "the box is %r metres, not [2, 3, 1]" % (box["scale"],))
            check(box["position"] == [1.5, 0.5, -2],
                  "the box is at %r, not where it was put" % (box["position"],))
            check(int(made) == box["id"],
                  "the id the script answered (%s) is not the node's (%s)" % (made, box["id"]))

        after = b.ping()
        check(after.get("nodes") == nodes_before + 1,
              "the document grew by %d nodes, not by one" % (after.get("nodes", 0) - nodes_before))
        check(after.get("undo") == 1,
              "one bridge command left %r undo steps behind" % after.get("undo"))

        # ---- 5. a material, assigned the way the panels assign one -------
        answer = b.js("editor.setMaterial(scene.find('BridgeBox'), 'materials/rost.daimat');"
                      " editor.getMaterial(scene.find('BridgeBox'))")
        check(answer.get("result") == "materials/rost.daimat",
              "the material did not stick: %r" % answer.get("result"))
        box = b.node("BridgeBox")
        check(box and box["materials"] == "materials/rost.daimat",
              "the document does not carry the material the bridge assigned")

        # ---- 6. undo, and redo -------------------------------------------
        u = b.undo()
        check(u.get("moved") is True, "undo did nothing")
        check(b.node("BridgeBox") is not None,
              "undo removed the box although the material change was the last step")
        u = b.undo()
        check(u.get("moved") is True, "the second undo did nothing")
        check(b.node("BridgeBox") is None,
              "undo did not remove the box the bridge made")
        r = b.redo()
        check(r.get("moved") is True, "redo did nothing")
        check(b.node("BridgeBox") is not None, "redo did not bring the box back")

        # Put the document back where it started, so a bridge test leaves no
        # trace in an editor somebody might still be looking at.
        b.undo()
        check(b.node("BridgeBox") is None, "the check did not clean up after itself")
        check(b.ping().get("nodes") == nodes_before,
              "the document did not end up the size it started at")

        # ---- 7. errors are answers, not deaths ---------------------------
        bad = b.js("this is not javascript")
        check(bad.get("ok") is False, "a syntax error was reported as success")
        check(bool(bad.get("error")), "a failed script produced no error message")
        bad = b.js("null.x = 1")
        check(bad.get("ok") is False, "a throwing script was reported as success")
        check(b.ping().get("ok") is True, "the editor stopped answering after a script error")

        # ---- 8. junk on the socket ---------------------------------------
        raw = socket.create_connection(("127.0.0.1", args.port), 10.0)
        f = raw.makefile("rwb")
        f.write(b"this is not json\n\n{\"cmd\":\"nonsense\"}\n{\"cmd\":\"ping\"}\n")
        f.flush()
        first = json.loads(f.readline().decode())
        check(first.get("ok") is False, "a line of junk was accepted")
        second = json.loads(f.readline().decode())
        check(second.get("ok") is False and "nonsense" in second.get("error", ""),
              "an unknown command was not named in the error: %r" % second)
        third = json.loads(f.readline().decode())
        check(third.get("ok") is True, "the connection did not survive the junk before it")
        f.close()
        raw.close()

        # ---- 9. a second client, at the same time ------------------------
        other = Bridge(args.port)
        try:
            check(other.ping().get("ok") is True, "a second client was not answered")
            check(b.ping().get("ok") is True, "the first client stopped being answered")
        finally:
            other.close()

        # ---- 10. a screenshot, from a camera given over the wire ---------
        shot_path = os.path.abspath("build/bridge_check_shot.png")
        if os.path.exists(shot_path):
            os.remove(shot_path)
        s = b.shot(shot_path, eye=[4, 3, 6], target=[0, 1, 0], fov=52)
        check(s.get("ok") is True, "the bridge could not take a picture: %r" % s.get("error"))
        check(os.path.exists(shot_path) and os.path.getsize(shot_path) > 1024,
              "the picture the bridge reported writing is not on disk")

        # ---- 11. saving the scene ----------------------------------------
        scene_path = os.path.abspath("build/bridge_check_scene.daiscene")
        if os.path.exists(scene_path):
            os.remove(scene_path)
        s = b.save(scene_path)
        check(s.get("ok") is True, "the bridge could not save the scene: %r" % s.get("error"))
        check(os.path.exists(scene_path), "the saved scene is not on disk")

        b.js("1")     # one last command, so the count below is not a fluke
    finally:
        b.close()
        log = stop(proc)

    check("bridge: listening on 127.0.0.1:%d" % args.port in log,
          "the editor did not say out loud that the bridge was open")

    print("%s: %d checks, %d failures" % ("FAILED" if FAIL else "ok", PASS + FAIL, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
