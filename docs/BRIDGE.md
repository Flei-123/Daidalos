# The Jarvis bridge — driving the editor from outside

A local TCP socket that runs JavaScript in the editor's own script context.
It is how an AI (or a script, or a person with `netcat`) builds rooms, sets
fields, assigns materials, points the camera and takes screenshots **inside
Daidalos**, without Blender and without a second code path.

Files: `include/dai_bridge_host.inl` (the socket and the protocol),
`tools/daibridge.py` (the command line), `tools/modeling_shot.cpp` (a headless
host that drives itself through the bridge), `tools/bridge_check.py` (the end
to end test), `examples/scripts/innen_room.js` (an example room, as data).

## Switching it on

**It is off.** Not "off in the default config" — the socket is not created,
so nothing can be probed. It opens only when the host is told to:

```
DAI_BRIDGE_PORT=8181 ./build/editor_demo               # the editor you use
DAI_SHADER_DIR=shaders ./build/modeling_shot --serve 60 --bridge 8181 /tmp
```

The editor prints one line when it does:

```
bridge: listening on 127.0.0.1:8181 (JSON lines; DAI_BRIDGE_PORT)
```

The listener is bound to `127.0.0.1` and to nothing else, so nothing on the
network can reach it — `tools/bridge_check.py` asserts exactly that by trying
the machine's own address and requiring the connection to be refused.

There is no password, and there will not be one. Anything that can open a
loopback socket on this machine can already start programs as this user; a
password would be theatre. What matters is that it is off by default, that it
is loopback only, and that it says out loud when it is open.

## The protocol

One JSON object per line in, one per line out. Every answer has `"ok"`.

| line | answer |
|---|---|
| `{"cmd":"ping"}` | `{"ok":true,"version":1,"nodes":11,"undo":1,"redo":0,"script":true}` |
| `{"cmd":"eval","code":"..."}` | `{"ok":true,"result":"<last expression>","undo":N}` |
| `{"cmd":"scene"}` | `{"ok":true,"nodes":[{"id":..,"name":..,"position":[..],"scale":[..],"materials":".."},...]}` |
| `{"cmd":"save","path":"scenes/room.daiscene"}` | `{"ok":true,"path":".."}` |
| `{"cmd":"shot","path":"a.png","eye":[5,3,-7],"target":[0,1,0],"fov":52}` | `{"ok":true,"path":".."}` |
| `{"cmd":"undo"}` / `{"cmd":"redo"}` | `{"ok":true,"moved":true,"undo":N,"redo":M}` |
| `{"cmd":"quit"}` | `{"ok":true,"bye":true}` — closes the socket; a windowed editor keeps running |

Anything else answers `{"ok":false,"error":"..."}` and the connection stays
open. A line of junk, an unknown command and an empty line are all answers,
never a crash — the editor is somebody's open document.

`result` is the value of the **last expression**, the way a REPL answers, so a
caller can ask the scene a question instead of only telling it things:

```
$ tools/daibridge.py js "scene.find('Floor').transform.scale.x"
6.4
```

## What the JavaScript can do

Everything the script editor's JS can do — `scene.find`, `node.getNum` /
`setNum` / `getVec` / `setVec` / `getStr` / `setStr`, `node.getPos` / `setPos`,
the whole component table of `prop_ref()` — plus the tool half, bound as
`editor` (see `include/dai_script.h`):

```js
editor.begin("build a room");            // one undo step for everything after
var wall = editor.add("Wall");           // -> node id
node.setVec(wall, "transform.scale", 2, 3, 1);      // a box 2 x 3 x 1 metres
node.setNum(wall, "blockout.kind", 1);              // module 1's components
editor.setMaterial(wall, "materials/raufaser.daimat");
editor.commit();
editor.camera([5,3,-7], [0,1,0], 52);
editor.shot("/tmp/room.png");
editor.save("scenes/room.daiscene");
editor.undo(); editor.redo();
editor.count(); editor.at(0);            // walk the document
```

A behaviour running in the game does **not** get `editor` — it is a separate
binding that only a tool host installs, because a script that can delete nodes
and overwrite the scene file is not something a game should be able to do by
accident.

### The undo stack stays intact

Every command is wrapped in `dai_doc_begin` / `dai_doc_commit`, so **one
bridge command is one Ctrl-Z**, however many nodes it touched. `editor.begin`
/ `editor.commit` nest inside that (the document counts the brackets), so a
script that builds a whole room is still one step. That is what
`tools/bridge_check.py` proves: it makes a box, undoes it, and the node is
gone; it redoes, and the node is back with the same id.

## The command line

```
tools/daibridge.py ping
tools/daibridge.py js "var b = editor.add('Box'); node.setVec(b,'transform.scale',2,3,1); b"
tools/daibridge.py js -f examples/scripts/innen_room.js
tools/daibridge.py scene --name Box
tools/daibridge.py shot /tmp/room.png --eye 5,3,-7 --target 0,1,0 --fov 52
tools/daibridge.py save projects/INNEN/scenes/room.daiscene
tools/daibridge.py undo
```

It exits non-zero when the editor answered `"ok": false`, so a shell script
can use it without reading prose. `daibridge.Bridge` is also importable, which
is what `tools/bridge_check.py` does.

## The screenshot tool

`tools/modeling_shot.cpp` is a second host beside `examples/editor_demo.cpp`:
the same four seams of `include/dai_ext.h`, the same panels, no window. It
opens the bridge, connects to it over a real loopback socket — not by calling
the handler, which would prove nothing about the socket — sends
`examples/scripts/innen_room.js`, and photographs the result:

```
tools/build_modeling_shot.sh .gauntlet-shots 1600 900
   .gauntlet-shots/10-modeling-room.png         the room, CSG wall selected
   .gauntlet-shots/11-modeling-doorsocket.png   the DoorSocket gizmo, inspector open
   .gauntlet-shots/12-modeling-materials.png    floor and wall in one picture
   .gauntlet-shots/13-modeling-bridge-shot.png  taken THROUGH the bridge
   .gauntlet-shots/modeling-room.daiscene       saved through the bridge
```

`--serve SECONDS` instead of photographing keeps it answering, which is how a
headless machine gets a bridge without a window.

The room is **not** in the C++. It is `examples/scripts/innen_room.js`, it is
sent over the socket, and it asks the build what it can do before it uses it:
where the blockout and CSG components exist the doorway is a subtraction,
where they do not it is three boxes around the same opening. Change the room
by editing the JavaScript; nothing has to be recompiled.

## Property names

The names the JS uses are the ones `prop_ref()` answers — one table, shared by
the inspector, the behaviours and the bridge, so all three mean the same thing
by `light.intensity`. The blockout components add theirs through
`include/dai_blockout_props.inl` (`blockout.kind`, `blockout.size`, `csg.op`,
`door.width`, `door.height`, `door.normal`). An unknown name is not an error —
it answers the fallback — which is what lets a script written for a newer
editor degrade instead of exploding, and what lets `innen_room.js` ask
`supports()` before it commits to a shape.

## Windows

The socket works there too, and `build_win.sh` is frozen, which means
`-lws2_32` cannot be added to the link line. So `ws2_32.dll` is loaded at run
time with `LoadLibrary` and the ten functions a loopback listener needs are
resolved by name. If the DLL is not there the editor says so and the bridge
stays closed — the rest of the editor never notices.
