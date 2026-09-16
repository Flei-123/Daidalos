# Daidalos as an MCP server

`tools/dai_mcp.py`, written 16.09.2026. It is the editor's bridge socket with
the Model Context Protocol in front of it, so an agent gets Daidalos as tools
instead of as a shell command whose output has to be read out of prose.

## Why this is the right shape

The socket already existed. `include/dai_bridge_host.inl` speaks one JSON
object per line on 127.0.0.1 and is off unless a port was passed, and
`tools/daibridge.py` already wraps it (`ping`, `eval`, `scene`, `shot`,
`save`, `undo`, `redo`, `quit`). `tools/innen_shots.sh` has been driving the
whole game through it for a week. So there was nothing to invent: this file
speaks JSON-RPC over stdin/stdout, calls the same bridge, and re-uses
`daibridge.Bridge` and `read_sources` directly instead of copying them.

**The picture is the point.** MCP lets a tool answer with an image
(`{"type":"image","data":<base64>,"mimeType":...}`), and that image arrives in
the model's context as a picture. Measured through the Claude Agent SDK on
16.09.2026: asked to load `innen_lib.js` + `innen_m0.js` and shoot the hallway
from eye height, the model came back with "narrow straight corridor, one-point
perspective, coarse plaster on the walls, dark square floor tiles, skirting
board" — it was looking at the render, not at a path. Before this, a render
could only be seen by serving the file over a local web server and taking a
browser screenshot of it.

## The tools

| tool | what it does |
|---|---|
| `state` | Is a runtime up, which binaries exist, and are they **older than a source**? Also states the trap that `build.sh` does not build `build/daidalos_runtime` — `tools/build_runtime.sh` does. |
| `js` | Run JavaScript in the runtime's script context; returns the value of the last expression. `files:[...]` concatenates repo files into ONE eval, which is how a level script gets its library in front of it. |
| `shot` | Render and **return the PNG as a picture**. `eye`, `target`, `fov`. |
| `scene` | The scene graph as JSON — instead of guessing what the last script built or reading `.daiscene` by hand. |
| `save` | Write the scene to a `.daiscene`, so the editor can open exactly what was shot. |
| `build` | `all` → `./build.sh`, `runtime` → `tools/build_runtime.sh`, `shot` → `tools/build_modeling_shot.sh`, `win` → `./build_win.sh`. Reminds the caller to `restart` afterwards. |
| `tests` | `tools/run_tests.sh`, filtered down to the `TOTAL` line and the failures. |
| `restart` | Kill the headless runtime **this server started** and start a fresh one. |

## Decisions worth knowing

**The runtime's life is the server's problem, not the agent's.** An agent that
has to remember to start a process will forget. `ensure()` starts
`build/modeling_shot --serve … --bridge <port>` headless on first use, waits
for the socket instead of sleeping a guess, and reuses it. If something is
*already* listening on the port it is used as it stands and **never killed** —
that is the editor a human has open, and killing it would throw away unsaved
work. Port via `DAI_MCP_PORT` (default 8399).

**No MCP SDK.** The protocol is eighty lines of JSON-RPC by hand. This
repository does not grow a Python package dependency for a debugging aid, for
the same reason `daibridge.py` has no client library.

**The picture that travels is a JPEG.** A 1280×720 render of this engine is
about 2.7 MB of PNG, 3.6 MB once base64'd — down a pipe, per shot, for
something a model reads as roughly a thousand image tokens either way. So the
image is resized to a 1568 px long edge (the documented useful maximum) and
sent as JPEG q82: 229 KB in the measured run, a factor of 16. The PNG stays on
disk untouched in `.dai-mcp-shots/` for the human and for reftests. Without
PIL the PNG goes as it is — a missing library must not cost the loop its eyes.

**Tool errors are results, not protocol errors.** A failed tool answers with
`isError: true` and the exception text, so the model reads it and tries
something else instead of the whole session falling over.

**Paths are confined** to the repository and `/tmp`. An agent that can write
anywhere through a "render" tool is a foot-gun, and the confinement costs four
lines.

## Using it from JARVIS

JARVIS got a generic MCP client on 16.09.2026 (`/root/jarvis/lib/mcpext.js`,
`docs/MCP.md` there). Entry in its `config.json`:

    "mcp_servers": {
      "daidalos": {
        "transport": "stdio",
        "command": "python3",
        "args": ["/root/projects/daidalos/tools/dai_mcp.py"],
        "cwd": "/root/projects/daidalos",
        "note": "Daidalos: js/shot/scene/build/tests über die Bridge",
        "auto_allow": ["state", "scene", "shot", "tests"]
      }
    }

`auto_allow` holds the four that only look: `state`, `scene`, `shot`, `tests`.
`js`, `save`, `build` and `restart` change something and are confirmed each
time.

## Using it from Codex or Claude Desktop

Same command, their own config format, e.g.

    { "mcpServers": { "daidalos": {
        "command": "python3",
        "args": ["/root/projects/daidalos/tools/dai_mcp.py"],
        "cwd": "/root/projects/daidalos" } } }

## Known limits (honest)

* **The shot shows the editor, not the game.** `tools/modeling_shot.cpp`
  photographs the real editor with its real panels on purpose (that is what it
  was written for), so hierarchy and inspector eat the left and right of every
  picture and the 3D view is the middle. For judging a room that is wrong.
  The fix is a `--clean` path in that host — no panels, camera viewport = the
  whole frame — or a `"ui": false` flag on the bridge's `shot` command. Not
  done yet; `.gauntlet-shots/` has the same property today.
* **No animation.** One frame per call. A pulse cycle or a door swinging needs
  frames, which means a `frames` command on the bridge, not on this side.
* **`js` runs in one context that persists** across calls, like a REPL. Two
  agents on the same port would step on each other's scene. One port per
  worker (`DAI_MCP_PORT`) if that ever happens.
* **`--serve` expires** (`DAI_MCP_SERVE`, default 3600 s). After that the first
  tool call reports a dead socket and `restart` fixes it. It does not renew
  itself silently, because a server that restarts the world under a running
  measurement is worse than one that says the measurement is over.
