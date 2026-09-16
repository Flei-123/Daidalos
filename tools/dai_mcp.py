#!/usr/bin/env python3
"""Daidalos as an MCP server: one stdio pipe, and the pictures come back.

WHAT THIS IS
============

`tools/daibridge.py` already speaks the editor's socket - one JSON object per
line, 127.0.0.1 only, off unless a port was given. This file wraps that socket
in the Model Context Protocol, so an agent (JARVIS, Codex, Claude Desktop)
gets Daidalos as *tools* instead of as a shell command whose output it has to
parse out of prose.

The one tool that matters is `shot`. MCP lets a tool answer with an image
(`{"type":"image","data":<base64>,"mimeType":"image/png"}`), and that image
arrives in the model's context as a picture, not as a path. Measured on
16.09.2026 through the Claude Agent SDK with a 512x256 probe: a model asked
for a description named the red square, the blue circle and the text on it.
That closes the loop that was open before - build, render, *look*, fix -
without a web server and a browser detour to see one's own render.

    ./tools/dai_mcp.py                  # stdio, speaks MCP, keeps a runtime
    DAI_MCP_PORT=8393 ./tools/dai_mcp.py   # attach to that bridge instead

NO SDK DEPENDENCY
=================

The protocol here is written out by hand: JSON-RPC 2.0 over stdin/stdout, one
object per line, methods `initialize`, `tools/list`, `tools/call`. That is
about eighty lines and means this repository does not grow a Python package
dependency for a debugging aid. The same reason `daibridge.py` has no client
library.

THE RUNTIME'S LIFE
==================

An agent that has to remember to start a process will forget. So:

  * `ensure()` starts `build/modeling_shot --serve ... --bridge <port>`
    headless the first time a tool needs it, waits for the socket rather than
    sleeping a guess, and reuses it afterwards.
  * If something is ALREADY listening on the port, it is used as it stands and
    never killed - that is the editor the human has open, and killing it would
    throw away unsaved work.
  * `--serve` has a lifetime (seconds). It is renewed by `restart`, and the
    process is killed when this server exits.

WHAT IS DELIBERATELY NOT HERE
=============================

No authentication: the socket is loopback and the MCP server talks over a pipe
to its parent. Whoever can reach either can already run programs as this user.
No writing to arbitrary paths either - `save` and `shot` are confined to the
repository and /tmp, because an agent that can write anywhere via a "render"
tool is a foot-gun, and the confinement costs four lines.
"""

import base64
import json
import os
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from daibridge import Bridge, read_sources  # noqa: E402  (same directory)

PORT = int(os.environ.get("DAI_MCP_PORT", "8399"))
SERVE_SECONDS = int(os.environ.get("DAI_MCP_SERVE", "3600"))
SHOT_W = int(os.environ.get("DAI_MCP_W", "1280"))
SHOT_H = int(os.environ.get("DAI_MCP_H", "720"))
SHOT_DIR = os.path.join(ROOT, ".dai-mcp-shots")

_proc = None          # the runtime we started (None if we attached to one)
_log = os.path.join("/tmp", "dai_mcp_serve.log")


# ---------------------------------------------------------------------------
# the runtime
# ---------------------------------------------------------------------------

def _listening(port):
    s = socket.socket()
    s.settimeout(0.4)
    try:
        return s.connect_ex(("127.0.0.1", port)) == 0
    finally:
        s.close()


def ensure():
    """A bridge on PORT, started if needed. Returns (ok, message)."""
    global _proc
    if _listening(PORT):
        return True, "already listening"
    exe = os.path.join(ROOT, "build", "modeling_shot")
    if not os.access(exe, os.X_OK):
        return False, ("build/modeling_shot is missing - run "
                       "tools/build_modeling_shot.sh first")
    env = dict(os.environ)
    env.setdefault("DAI_SHADER_DIR", os.path.join(ROOT, "shaders"))
    with open(_log, "ab") as log:
        log.write(b"\n---- dai_mcp start %s ----\n" % time.strftime("%F %T").encode())
        _proc = subprocess.Popen(
            [exe, "/tmp", str(SHOT_W), str(SHOT_H),
             "--serve", str(SERVE_SECONDS), "--bridge", str(PORT)],
            cwd=ROOT, env=env, stdout=log, stderr=log,
            stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(120):
        if _listening(PORT):
            return True, "started modeling_shot (pid %d)" % _proc.pid
        if _proc.poll() is not None:
            return False, "modeling_shot exited at once - see %s" % _log
        time.sleep(0.5)
    return False, "modeling_shot did not open the socket in 60 s - see %s" % _log


def stop():
    global _proc
    if _proc and _proc.poll() is None:
        try:
            os.killpg(os.getpgid(_proc.pid), signal.SIGTERM)
        except Exception:
            _proc.terminate()
        try:
            _proc.wait(timeout=5)
        except Exception:
            pass
    was, _proc = _proc, None
    return was is not None


def ask(message, timeout=120.0):
    ok, note = ensure()
    if not ok:
        raise RuntimeError(note)
    b = Bridge(PORT, timeout=timeout)
    try:
        return b.ask(message)
    finally:
        b.close()


def confine(path, default_dir):
    """A path inside the repository or /tmp. Anything else is refused."""
    p = path or ""
    if not p:
        raise ValueError("path is empty")
    if not os.path.isabs(p):
        p = os.path.join(default_dir, p)
    p = os.path.realpath(p)
    for ok_root in (os.path.realpath(ROOT), "/tmp"):
        if p == ok_root or p.startswith(ok_root + os.sep):
            return p
    raise ValueError("path must be inside %s or /tmp (got %s)" % (ROOT, p))


def run(cmd, timeout=1800):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                       timeout=timeout)
    out = (r.stdout or "") + (("\n[stderr]\n" + r.stderr) if r.stderr else "")
    return r.returncode, out


# ---------------------------------------------------------------------------
# the tools
# ---------------------------------------------------------------------------

TOOLS = [
    {
        "name": "state",
        "description": "Is a Daidalos runtime up, which binaries exist and how old are they "
                       "against the sources? Start here - it also answers the trap that "
                       "build.sh does NOT build build/daidalos_runtime (tools/build_runtime.sh does).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "api",
        "description": "READ THIS FIRST if you have not worked in Daidalos before. The whole cheat "
                       "sheet: the object model, every property name, the enums, the modifier fields, "
                       "the two rules that are not guessable (csg.op sits on the PARENT; supports() is "
                       "a library helper, not a builtin), which docs to read and the build-look-fix "
                       "loop. No other engine's knowledge transfers - nobody has seen this one.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "docs",
        "description": "Read a file from the repository - without a name it lists docs/. The documents "
                       "worth reading: BRIDGE.md (socket + property table), SCRIPTING.md (behaviours, "
                       "object model), BLOCKOUT.md (kinds + CSG), MATERIALS.md, GDD_INNEN.md (the game), "
                       "INNEN_*.md (what is already built and measured).",
        "inputSchema": {"type": "object", "properties": {
            "file": {"type": "string", "description": "e.g. BRIDGE.md, or examples/scripts/innen_lib.js"},
        }},
    },
    {
        "name": "add",
        "description": "Create a node and set its properties in one undo step - the structured form of "
                       "editor.add, for when you do not want to write JavaScript. props is a map of "
                       "property name to value: a list of three is written as a vector, a number as a "
                       "number, anything else as text. See `api` for the names and enums.",
        "inputSchema": {"type": "object", "properties": {
            "name": {"type": "string"},
            "parent": {"type": "integer", "description": "Parent node id. Remember: for CSG the PARENT carries csg.op."},
            "props": {"type": "object", "description": 'e.g. {"blockout.kind":1,"blockout.size":[4,3,0.2],"transform.position":[0,1.5,0]}'},
            "material": {"type": "string", "description": 'e.g. "materials/raufaser.daimat"'},
        }, "required": ["name"]},
    },
    {
        "name": "set",
        "description": "Change properties of an existing node and READ THEM BACK, so the answer is what "
                       "the document says rather than what was sent - an unknown property name answers a "
                       "harmless fallback, which would otherwise look like success.",
        "inputSchema": {"type": "object", "properties": {
            "id": {"type": "integer"},
            "props": {"type": "object"},
            "material": {"type": "string"},
        }, "required": ["id"]},
    },
    {
        "name": "get",
        "description": "Read properties of a node. Without a list of names it reads the usual ones "
                       "(name, tag, transform, blockout, csg, modifier count, script, collider, body). "
                       "Each value comes back read as number, vector AND text, because one table serves "
                       "all three and guessing the kind from the name would be a second table to keep in step.",
        "inputSchema": {"type": "object", "properties": {
            "id": {"type": "integer"},
            "props": {"type": "array", "items": {"type": "string"}},
        }, "required": ["id"]},
    },
    {
        "name": "remove",
        "description": "Delete a node and its subtree (one undo step).",
        "inputSchema": {"type": "object", "properties": {"id": {"type": "integer"}}, "required": ["id"]},
    },
    {
        "name": "assets",
        "description": "What is on disk to reference: materials (.daimat), textures, scenes (.daiscene) "
                       "and the behaviour scripts. Use this instead of guessing a material path.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "undo",
        "description": "Undo (or redo) whole steps. One bridge call was one step, however many nodes it touched.",
        "inputSchema": {"type": "object", "properties": {
            "steps": {"type": "integer"}, "redo": {"type": "boolean"},
        }},
    },
    {
        "name": "js",
        "description": "Run JavaScript in the runtime's script context and return the value of the "
                       "last expression. This is how scenes are built in this project - the rooms "
                       "are data, not C++. Either code, or files (concatenated into ONE eval, so a "
                       "level script gets its library in front of it: "
                       "['examples/scripts/innen_lib.js','examples/scripts/innen_m0.js']).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {"type": "string", "description": "JavaScript source."},
                "files": {"type": "array", "items": {"type": "string"},
                          "description": "Repo-relative files, evaluated as one program in this order."},
            },
        },
    },
    {
        "name": "shot",
        "description": "Render the current scene and RETURN THE PICTURE (PNG) so it can be looked at, "
                       "not just a file path. Camera: eye and target as [x,y,z], fov in degrees. "
                       "Eye height 1.55 is where the player's camera is - a room that only reads from "
                       "a drone shot is a room nobody has stood in.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "eye": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                "target": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                "fov": {"type": "number"},
                "path": {"type": "string", "description": "Where to keep the PNG (default .dai-mcp-shots/)."},
            },
        },
    },
    {
        "name": "scene",
        "description": "The scene graph as JSON: nodes, names, transforms. Use this instead of guessing "
                       "what the last script actually built, and instead of reading .daiscene files.",
        "inputSchema": {"type": "object", "properties": {
            "name": {"type": "string", "description": "Only the first node with this name."},
        }},
    },
    {
        "name": "save",
        "description": "Save the scene to a .daiscene file, so the editor can open exactly what was shot.",
        "inputSchema": {"type": "object", "properties": {
            "path": {"type": "string", "description": "Repo-relative or absolute (repo or /tmp only)."},
        }, "required": ["path"]},
    },
    {
        "name": "build",
        "description": "Build. target=all runs ./build.sh, target=runtime runs tools/build_runtime.sh "
                       "(the one build.sh does not do), target=shot runs tools/build_modeling_shot.sh, "
                       "target=win runs ./build_win.sh. Returns the tail of the log and the exit code.",
        "inputSchema": {"type": "object", "properties": {
            "target": {"type": "string", "enum": ["all", "runtime", "shot", "win"]},
        }, "required": ["target"]},
    },
    {
        "name": "tests",
        "description": "Run tools/run_tests.sh and return the TOTAL line plus any failures. The suite "
                       "has a freshness guard: a runtime older than a source in include/src/examples "
                       "fails on purpose, because a stale binary measures nothing.",
        "inputSchema": {"type": "object", "properties": {
            "pattern": {"type": "string", "description": "Only tests whose name contains this."},
        }},
    },
    {
        "name": "restart",
        "description": "Kill the headless runtime this server started and start a fresh one - after a "
                       "rebuild, or when --serve ran out. An editor started by a human is never killed.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def t_state(_a):
    lines = []
    up = _listening(PORT)
    lines.append("bridge on port %d: %s%s" % (
        PORT, "up" if up else "down",
        " (started by this server)" if (_proc and _proc.poll() is None) else
        (" (someone else's - will not be killed)" if up else "")))
    newest_src = 0.0
    for d in ("include", "src", "examples"):
        p = os.path.join(ROOT, d)
        for root, _dirs, files in os.walk(p):
            for f in files:
                if f.endswith((".h", ".inl", ".cpp", ".c", ".js")):
                    try:
                        newest_src = max(newest_src, os.path.getmtime(os.path.join(root, f)))
                    except OSError:
                        pass
    for name in ("modeling_shot", "daidalos_runtime", "editor_demo"):
        p = os.path.join(ROOT, "build", name)
        if not os.path.exists(p):
            lines.append("build/%-18s MISSING" % name)
            continue
        age = os.path.getmtime(p)
        stale = age < newest_src
        lines.append("build/%-18s %s%s" % (
            name, time.strftime("%F %T", time.localtime(age)),
            "   STALE - a source is newer, rebuild before measuring" if stale else ""))
    lines.append("newest source:            " + time.strftime("%F %T", time.localtime(newest_src)))
    return [{"type": "text", "text": "\n".join(lines)}]


def t_js(a):
    code = a.get("code")
    files = a.get("files") or []
    if files:
        paths = [confine(f, ROOT) for f in files]
        code = read_sources(paths)
    if not code:
        raise ValueError("give either code or files")
    r = ask({"cmd": "eval", "code": code}, timeout=300.0)
    if not r.get("ok", True):
        return [{"type": "text", "text": "the runtime refused it:\n" + json.dumps(r, indent=1)}]
    return [{"type": "text", "text": json.dumps(r.get("result"), indent=1, default=str)}]


def t_shot(a):
    os.makedirs(SHOT_DIR, exist_ok=True)
    path = confine(a.get("path") or ("shot-%d.png" % int(time.time())), SHOT_DIR)
    msg = {"cmd": "shot", "path": path}
    for k in ("eye", "target"):
        if a.get(k) is not None:
            msg[k] = [float(x) for x in a[k]]
    if a.get("fov") is not None:
        msg["fov"] = float(a["fov"])
    r = ask(msg, timeout=300.0)
    if not r.get("ok", True) or not os.path.exists(path):
        return [{"type": "text", "text": "no picture: " + json.dumps(r, default=str)}]
    data, mime, note = encode_for_model(path)
    return [
        {"type": "text", "text": "%s  %dx%d  eye=%s target=%s fov=%s%s" % (
            os.path.relpath(path, ROOT), SHOT_W, SHOT_H,
            msg.get("eye", "(scene camera)"), msg.get("target", "-"), msg.get("fov", "-"), note)},
        {"type": "image", "data": data, "mimeType": mime},
    ]


# A 1280x720 render of this engine comes out around 2.7 MB of PNG, which is
# 3.6 MB once base64'd - down a pipe, per picture, for something the model reads
# as roughly a thousand image tokens either way. So the picture that TRAVELS is
# a JPEG at the long edge Anthropic documents as the useful maximum (1568 px);
# the PNG stays on disk untouched for the human and for reftests. Without PIL
# the PNG goes as it is - a missing library must not cost the loop its eyes.
MODEL_MAX_EDGE = 1568


def encode_for_model(path):
    try:
        from PIL import Image
    except ImportError:
        with open(path, "rb") as f:
            return base64.b64encode(f.read()).decode("ascii"), "image/png", "  (PNG as-is, no PIL)"
    import io
    with Image.open(path) as im:
        im = im.convert("RGB")
        w, h = im.size
        if max(w, h) > MODEL_MAX_EDGE:
            k = MODEL_MAX_EDGE / float(max(w, h))
            im = im.resize((int(w * k), int(h * k)), Image.LANCZOS)
        buf = io.BytesIO()
        im.save(buf, "JPEG", quality=82, optimize=True)
    raw = buf.getvalue()
    on_disk = os.path.getsize(path)
    return (base64.b64encode(raw).decode("ascii"), "image/jpeg",
            "  (sent as JPEG %d KB, PNG on disk %d KB)" % (len(raw) // 1024, on_disk // 1024))


def t_scene(a):
    r = ask({"cmd": "scene"})
    nodes = r.get("nodes", [])
    want = a.get("name")
    if want:
        nodes = [n for n in nodes if n.get("name") == want]
        if not nodes:
            return [{"type": "text", "text": 'no node called "%s" (%d nodes in the scene)'
                                             % (want, len(r.get("nodes", [])))}]
    return [{"type": "text", "text": json.dumps(nodes, indent=1, default=str)}]


def t_save(a):
    path = confine(a.get("path"), ROOT)
    r = ask({"cmd": "save", "path": path})
    return [{"type": "text", "text": ("saved %s" % os.path.relpath(path, ROOT))
             if r.get("ok", True) else json.dumps(r, default=str)}]


def t_build(a):
    target = a.get("target")
    cmd = {"all": ["./build.sh"], "runtime": ["tools/build_runtime.sh"],
           "shot": ["tools/build_modeling_shot.sh"], "win": ["./build_win.sh"]}[target]
    code, out = run(cmd)
    tail = "\n".join(out.strip().splitlines()[-40:])
    note = ""
    if target in ("all", "shot", "runtime") and code == 0:
        note = "\n\nThe headless runtime is still the old process - call restart before measuring."
    return [{"type": "text", "text": "%s -> exit %d\n\n%s%s" % (" ".join(cmd), code, tail, note)}]


def t_tests(a):
    cmd = ["tools/run_tests.sh"]
    if a.get("pattern"):
        cmd.append(a["pattern"])
    code, out = run(cmd)
    lines = out.strip().splitlines()
    keep = [l for l in lines if ("TOTAL" in l or "FAIL" in l or "failed" in l
                                 or "all green" in l or "STALE" in l)]
    return [{"type": "text", "text": "exit %d\n%s" % (code, "\n".join(keep[-40:]) or "\n".join(lines[-20:]))}]


def t_restart(_a):
    killed = stop()
    ok, note = ensure()
    return [{"type": "text", "text": ("killed the old one; " if killed else "") + note}]



# ---------------------------------------------------------------------------
# the cheat sheet
# ---------------------------------------------------------------------------
# WHY THIS TOOL EXISTS AT ALL. A model that is handed Blender or Unity already
# knows bpy and UnityEditor from its training - the MCP server there only has
# to carry the calls. Nobody has ever seen Daidalos. So the server has to teach
# it: the property names, the enums, and above all the two rules that are not
# guessable and that cost a wrong render every time (csg.op sits on the PARENT;
# supports() is a helper in innen_lib.js, not a builtin). Without this tool a
# foreign agent writes plausible nonsense and the pictures look fine to it.

API_TEXT = """DAIDALOS THROUGH MCP - everything a model needs to build in this engine.

THE MODEL. A scene is a tree of nodes. A node is a record of components; you do
not "add a component", you set its properties, and an unknown property name
answers a harmless fallback instead of failing (that is what lets a script
written for a newer editor degrade instead of exploding).

ONE CALL IS ONE UNDO STEP, however many nodes it touched. editor.begin("label")
/ editor.commit() nest inside that.

BUILDING (tool: js, or the structured tools add/set/get/remove)
  var n = editor.add("Name");          // -> node id
  var c = editor.add("Child", n);      // with a parent
  editor.remove(n);                    // and its subtree
  node.setVec(n, "transform.position", 0, 1, 0);
  node.setNum(n, "light.intensity", 3);
  node.setStr(n, "script", "innen_door.js{width=0.9}");
  node.getVec(n, "blockout.size"); node.getNum(...); node.getStr(...)
  editor.setMaterial(n, "materials/raufaser.daimat");
  editor.count(); editor.at(i);        // walk the document
  editor.camera([5,3,-7], [0,1,0], 52);
  editor.save("scenes/room.daiscene"); editor.undo(); editor.redo();
  self / scene.find("Name") / node.transform.position.x also work (Unity-style),
  and setting one component writes only that one.

PROPERTY NAMES (one table, shared by inspector, behaviours and this bridge)
  transform.position|rotation|scale     vectors; rotation in DEGREES
  node.name|tag|asset                   tag is also the channel behaviours read
  renderer.enabled
  light.enabled|mode|color|intensity|range|cone
  camera.enabled|mode|fov|size
  text.enabled|value|size|color|anchor
  image.enabled|asset|size
  rigidbody.enabled|motion|density|friction|restitution|trigger|freeze
  collider.shape|center|enabled         shape: 0 box 1 sphere 2 capsule 4 cylinder
  script                                behaviours, ';' separated, each file.js{k=v,...}
  blockout.enabled|kind|size|pivot|segments|steps|thickness|width
  csg.enabled|op
  door.enabled|width|height|normal|offset      the sockets the generator docks to
  modifier.count and modifier.<0-7>.<field>

ENUMS
  blockout.kind : 0 none, 1 BOX, 2 CYLINDER, 3 STAIRS, 4 ARCH, 5 WEDGE
  csg.op        : 0 none, 1 UNION, 2 SUBTRACT, 3 INTERSECT
  modifier.type : 0 none, 1 BEVEL, 2 SUBDIVIDE, 3 SOLIDIFY, 4 ARRAY, 5 MIRROR
  collider.shape: 0 BOX, 1 SPHERE, 2 CAPSULE, 3 COMPOUND, 4 CYLINDER
  light.mode    : 0 = NO LIGHT AT ALL, 1 POINT, 2 SPOT, 3 DIRECTIONAL (sun)
  rigidbody.freeze is a mask like Unity's Constraints; 40 keeps a capsule upright

FIVE MORE THINGS THAT HAVE ALREADY COST SOMEBODY A SCENE:

 4. light.mode 0 IS NOT A DEFAULT, IT IS OFF. A node with light.enabled 1 and
    light.mode 0 emits nothing and looks like a broken renderer. Point light is
    1, sun is 3. Set the mode every single time you make a light.

 5. A NODE WITHOUT BLOCKOUT STILL RENDERS - as a 1 m cube. So every node you
    only use as a GROUP needs node.setNum(g, "renderer.enabled", 0), or your
    scene quietly fills with white boxes at every group's origin.

 6. COLLIDERS AND BODIES ARE ON BY DEFAULT, on every node you create. In a
    277-node level that means the physics has 277 bodies, the neon tubes are
    solid, and a bucket held in front of the camera collides with the player.
    Anything decorative wants collider.enabled 0 and rigidbody.enabled 0 -
    check with get before you hand the level to a behaviour.

 7. THE ARRAY MODIFIER IS NOT A LEVEL BUILDER. It works on a small scene, but
    in a big one the instances have been seen not to appear at all - tiles and
    pillars silently missing. For anything structural, place real nodes in a
    loop; keep ARRAY for small props you can verify in one shot.

 8. text.value DOES NOT RENDER in modeling_shot. The string sits in the
    document and photographs as nothing, and painting letters into a texture
    smears if the material is triplanar. A sign is a glowing panel until this
    is solved - do not spend a run on it.

MODIFIER FIELDS have a neutral name AND the name of the type that uses them -
they are the same slot, so width==amount==thickness==weld, and
count==segments==level==copies:
  modifier.0.type|enabled|off
  modifier.0.width   (= amount, thickness, weld)
  modifier.0.count   (= segments, level, copies)
  modifier.0.angle   (= rotation)
  modifier.0.offset  (= step, a vector)
  modifier.0.axis | .param (= shift) | .smooth | .relative
  modifier.count     how many slots are live (set it, or the stack is ignored)

TWO RULES THAT ARE NOT GUESSABLE - each one costs a wrong render:

 1. csg.op GOES ON THE PARENT. The parent is the operator, the children are the
    operands: FIRST child = the base, every child after it = a tool. Putting
    csg.op on the cylinder you want subtracted does nothing at all - it just
    stands there as its own block. Correct:
        var cut = editor.add("Wall");           node.setNum(cut,"csg.op",2);
        var slab = editor.add("Wall.Slab",cut); node.setNum(slab,"blockout.kind",1);
                                                node.setVec(slab,"blockout.size",4,3,0.2);
        var hole = editor.add("Wall.Hole",cut); node.setNum(hole,"blockout.kind",1);
                                                node.setVec(hole,"blockout.size",1,2.1,1);
                                                node.setVec(hole,"transform.position",1,0,0);

 2. supports() IS NOT A BUILTIN. It is a helper in examples/scripts/innen_lib.js
    that adds a __probe node, writes and reads a property back and removes it.
    Load the library if you want it: js files:["examples/scripts/innen_lib.js",
    "your_script.js"] - the files are concatenated into ONE eval, which is how a
    level script gets its library in front of it. innen_lib.js also gives you
    place/wall/group/collider and the INNEN constants.

 3. scene.spawn(src,parent,"Name") is for RUNTIME (a behaviour copying a room
    while the game runs). In the editor it refuses with -1; use editor.add.

WHAT TO READ BEFORE A BIG CHANGE
  docs/BRIDGE.md        the socket, the commands, the property table
  docs/SCRIPTING.md     behaviours, inspector fields, the object model, spawning
  docs/BLOCKOUT.md      blockout kinds and CSG
  docs/MATERIALS.md     material stack and .daimat
  docs/GDD_INNEN.md     the game this engine is being built for
  examples/scripts/     innen_lib.js (library), innen_m0.js (three real rooms)
  tool `docs` reads any of them; tool `state` says whether a binary is stale.

THE LOOP THAT WORKS
  state -> js (build) -> shot (LOOK at it) -> fix -> shot again -> save -> tests
  The shot comes back as a picture, so judge the picture, not the code. Note the
  render currently photographs the whole editor, so panels take the sides and
  the 3D view is the middle - that is a known limit, not your camera being wrong.
"""


def t_api(_a):
    return [{"type": "text", "text": API_TEXT}]


def t_docs(a):
    name = a.get("file") or ""
    if not name:
        names = sorted(f for f in os.listdir(os.path.join(ROOT, "docs")) if f.endswith(".md"))
        return [{"type": "text", "text": "docs/: " + ", ".join(names)}]
    path = confine(name if name.startswith(("docs/", "examples/", "projects/", "tools/"))
                   else os.path.join("docs", name), ROOT)
    if not os.path.exists(path):
        raise ValueError("no such file: %s" % os.path.relpath(path, ROOT))
    with open(path, "r", errors="replace") as f:
        text = f.read()
    cut = 60000
    return [{"type": "text", "text": text[:cut] + ("\n...(cut at %d chars)" % cut if len(text) > cut else "")}]


def t_add(a):
    name = str(a.get("name") or "Node")
    parent = a.get("parent")
    props = a.get("props") or {}
    material = a.get("material")
    lines = ['editor.begin("mcp add %s");' % name.replace('"', "'"),
             'var n = editor.add(%s%s);' % (json.dumps(name), (", %d" % int(parent)) if parent not in (None, "",) else "")]
    lines += _prop_lines("n", props)
    if material:
        lines.append("editor.setMaterial(n, %s);" % json.dumps(str(material)))
    lines.append("editor.commit();")
    lines.append('JSON.stringify({id:n, name:%s});' % json.dumps(name))
    r = ask({"cmd": "eval", "code": "\n".join(lines)}, timeout=300.0)
    if not r.get("ok", True):
        return [{"type": "text", "text": "refused:\n" + json.dumps(r, indent=1, default=str)}]
    return [{"type": "text", "text": json.dumps(r.get("result"), default=str)}]


def _prop_lines(var, props):
    """One setter per property, choosing setVec/setNum/setStr by the VALUE.
    A list of three -> vector, a number -> number, anything else -> string."""
    out = []
    for key, val in props.items():
        k = json.dumps(str(key))
        if isinstance(val, (list, tuple)):
            nums = ", ".join(repr(float(x)) for x in val)
            out.append("node.setVec(%s, %s, %s);" % (var, k, nums))
        elif isinstance(val, bool):
            out.append("node.setNum(%s, %s, %d);" % (var, k, 1 if val else 0))
        elif isinstance(val, (int, float)):
            out.append("node.setNum(%s, %s, %s);" % (var, k, repr(val)))
        else:
            out.append("node.setStr(%s, %s, %s);" % (var, k, json.dumps(str(val))))
    return out


def t_set(a):
    nid = a.get("id")
    if nid is None:
        raise ValueError("id is required")
    props = a.get("props") or {}
    if not props and not a.get("material"):
        raise ValueError("give props and/or material")
    lines = ['editor.begin("mcp set");', "var n = %d;" % int(nid)]
    lines += _prop_lines("n", props)
    if a.get("material"):
        lines.append("editor.setMaterial(n, %s);" % json.dumps(str(a["material"])))
    lines.append("editor.commit();")
    # read every property back, so the answer is what the DOCUMENT says, not
    # what was sent - a fallback property would otherwise look like a success.
    reads = []
    for key, val in props.items():
        k = json.dumps(str(key))
        if isinstance(val, (list, tuple)):
            reads.append("%s: node.getVec(n, %s)" % (json.dumps(str(key)), k))
        elif isinstance(val, (int, float, bool)):
            reads.append("%s: node.getNum(n, %s)" % (json.dumps(str(key)), k))
        else:
            reads.append("%s: node.getStr(n, %s)" % (json.dumps(str(key)), k))
    lines.append("JSON.stringify({id:n, now:{%s}});" % ", ".join(reads))
    r = ask({"cmd": "eval", "code": "\n".join(lines)}, timeout=300.0)
    if not r.get("ok", True):
        return [{"type": "text", "text": "refused:\n" + json.dumps(r, indent=1, default=str)}]
    return [{"type": "text", "text": json.dumps(r.get("result"), default=str)}]


def t_get(a):
    nid = a.get("id")
    names = a.get("props")
    if nid is None:
        raise ValueError("id is required")
    if not names:
        names = ["node.name", "node.tag", "transform.position", "transform.rotation",
                 "transform.scale", "blockout.kind", "blockout.size", "csg.op",
                 "modifier.count", "script", "collider.shape", "rigidbody.enabled"]
    parts = []
    for nm in names:
        k = json.dumps(str(nm))
        # Read all three ways and let the caller see which one carries a value:
        # one table serves numbers, vectors and strings, and guessing the kind
        # from the name would be a second table to keep in step.
        parts.append("%s: {num: node.getNum(n, %s), vec: node.getVec(n, %s), str: node.getStr(n, %s)}"
                     % (json.dumps(str(nm)), k, k, k))
    code = "var n = %d;\nJSON.stringify({id:n, props:{%s}});" % (int(nid), ", ".join(parts))
    r = ask({"cmd": "eval", "code": code})
    return [{"type": "text", "text": json.dumps(r.get("result"), indent=1, default=str)}]


def t_remove(a):
    nid = a.get("id")
    if nid is None:
        raise ValueError("id is required")
    code = ('editor.begin("mcp remove");\nvar ok = editor.remove(%d);\neditor.commit();\n'
            'JSON.stringify({removed:%d, ok:ok, count:editor.count()});' % (int(nid), int(nid)))
    r = ask({"cmd": "eval", "code": code})
    return [{"type": "text", "text": json.dumps(r.get("result"), default=str)}]


def t_assets(_a):
    """What is actually on disk to reference: materials, scenes, scripts."""
    out = []
    for sub, ext in (("projects/Untitled/assets/materials", ".daimat"),
                     ("projects/Untitled/assets/textures", None),
                     ("projects/Untitled/scenes", ".daiscene"),
                     ("projects/Untitled/assets", ".js"),
                     ("examples/scripts", ".js"),
                     ("materials", ".daimat")):
        d = os.path.join(ROOT, sub)
        if not os.path.isdir(d):
            continue
        names = sorted(f for f in os.listdir(d) if (ext is None or f.endswith(ext)))
        if names:
            out.append("%s/  (%d)\n  %s" % (sub, len(names), "\n  ".join(names[:40])))
    return [{"type": "text", "text": "\n\n".join(out) or "(nothing found)"}]


def t_undo(a):
    n = int(a.get("steps") or 1)
    cmd = "redo" if a.get("redo") else "undo"
    res = []
    for _ in range(max(1, min(n, 50))):
        res.append(ask({"cmd": cmd}).get("ok", True))
    return [{"type": "text", "text": "%s x%d -> %s" % (cmd, len(res), "ok" if all(res) else "some refused")}]


HANDLERS = {"state": t_state, "js": t_js, "shot": t_shot, "scene": t_scene,
            "save": t_save, "build": t_build, "tests": t_tests, "restart": t_restart,
            "api": t_api, "docs": t_docs, "add": t_add, "set": t_set, "get": t_get,
            "remove": t_remove, "assets": t_assets, "undo": t_undo}


# ---------------------------------------------------------------------------
# the protocol
# ---------------------------------------------------------------------------

def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def handle(msg):
    mid = msg.get("id")
    method = msg.get("method") or ""
    if method == "initialize":
        return send({"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "daidalos", "version": "1.0.0"},
        }})
    if method.startswith("notifications/"):
        return
    if method == "tools/list":
        return send({"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}})
    if method == "tools/call":
        name = (msg.get("params") or {}).get("name")
        args = (msg.get("params") or {}).get("arguments") or {}
        fn = HANDLERS.get(name)
        if not fn:
            return send({"jsonrpc": "2.0", "id": mid,
                         "error": {"code": -32601, "message": "no tool called %s" % name}})
        try:
            return send({"jsonrpc": "2.0", "id": mid, "result": {"content": fn(args)}})
        except Exception as e:
            # An error is a RESULT with isError, not a protocol error: the model
            # is supposed to read it and try something else.
            return send({"jsonrpc": "2.0", "id": mid, "result": {
                "isError": True,
                "content": [{"type": "text", "text": "%s: %s" % (type(e).__name__, e)}]}})
    if mid is not None:
        send({"jsonrpc": "2.0", "id": mid,
              "error": {"code": -32601, "message": "unknown method %s" % method}})


def main():
    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            handle(msg)
    finally:
        stop()


if __name__ == "__main__":
    main()
