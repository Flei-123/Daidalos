# Exporting a game

A Daidalos project becomes **one file** that someone else can double click. Nothing
has to be installed where it lands: no runtime, no redistributable, no engine, no
compiler.

```
./build.sh                                   # engine + editor (as usual)
tools/build_runtime.sh linux                 # the runtime template + the packer
build/dai_pack export projects/MyGame build/daidalos_runtime /tmp/MyGame
/tmp/MyGame                                  # <- one file, runs anywhere
```

For Windows, the same three lines with `./build_win.sh`,
`tools/build_runtime.sh windows` and `build-win/daidalos_runtime.exe`.

## Why it works this way

This is **Godot's model, not Unity's**.

Unity builds the player from source at export time: it needs a compiler, a linker
and several minutes. Godot ships a prebuilt *export template* — the same engine
without the editor — and export is a **file copy plus an append**. For an engine
this size that difference is everything: exporting takes a second, works on a
machine with no toolchain, and the thing that runs at the player's has already
been compiled and tested here.

So there are exactly three moving parts:

1. **`examples/runtime_main.cpp`** — the game. Window, scene, physics, scripts,
   renderer. No docking, no panels, no gizmos, no undo, no asset browser, no
   self-updater. `tools/build_runtime.sh` compiles it once into a *template*.
2. **The archive** (`src/dai_vfs.cpp`) — scenes, assets, settings and a generated
   `boot.cfg` in one blob.
3. **The append** (`src/dai_export.cpp`) — the template is copied, the archive is
   glued onto the end of the copy, and the copy is the game.

At start-up the game reads the **last 16 bytes of its own file**, finds the
archive and mounts it. Same bytes, two shapes: a bare `.dpk` and a tail on an
`.exe` are read by the same code.

## The archive format

All integers are little endian. The full comment lives in `include/dai_vfs.h`;
this is the layout.

An archive starts at absolute file offset `base` — `0` for a bare `.dpk`, the
4-byte-padded size of the runtime for an export — and `base` is always a multiple
of 4.

### Header, 32 bytes, at `base`

| offset | type      | meaning |
|-------:|-----------|---------|
| `+0`   | `char[8]` | `"DAIPACK1"` |
| `+8`   | `uint32`  | version, currently `1` |
| `+12`  | `uint32`  | number of entries |
| `+16`  | `uint32`  | directory offset, **relative to `base`** |
| `+20`  | `uint32`  | directory size in bytes |
| `+24`  | `uint32`  | first payload offset, relative to `base` |
| `+28`  | `uint32`  | total archive size (header + payloads + directory + trailer) |

### Payloads

One blob per entry, stored **raw** — no compression. Each starts at a 4-byte
aligned offset relative to `base`; the padding between them is zero.

Not compressing is a decision, not an omission: a `.glb` is already compressed
geometry and a scene file is a few kilobytes, so deflating either buys less than
the decoder costs. `src/dai_inflate.cpp` is in the tree if that ever changes —
the header has a version field for exactly that day.

### Directory, at `base + dir_offset`

One record per entry, each 4-byte aligned, **sorted by path** so a lookup is a
binary search:

| offset | type             | meaning |
|-------:|------------------|---------|
| `+0`   | `uint32`         | path length in bytes, **no terminator** |
| `+4`   | `uint32`         | payload offset, relative to `base` |
| `+8`   | `uint32`         | payload length |
| `+12`  | `uint32`         | FNV-1a 32 of the payload (`0` for an empty entry) |
| `+16`  | `char[path_len]` | the path, then zero padding to a multiple of 4 |

### Trailer, the last 16 bytes of the file

| offset | type      | meaning |
|-------:|-----------|---------|
| `+0`   | `uint64`  | `base` — where the header is |
| `+8`   | `char[8]` | `"DAIPACKE"` |

Reading is therefore: seek to `end-16`, check the magic, take `base`, seek there,
check `"DAIPACK1"`, read the directory.

**Why both a header and a trailer.** The header alone cannot be found from the end
of a file, and a trailer alone cannot be validated when the archive is handed over
on its own. Together they cost 48 bytes and every truncated download fails loudly
instead of loading half a scene.

## What gets packed

From `<project>/`:

- `scenes/**` — every scene, not just the startup one
- `assets/**` — recursively
- `settings/project.txt`
- `project.daidalos`
- `boot.cfg` — **generated**, see below

Skipped: `cache/` (derived data by definition — that is the promise
`dai_project` makes about it) and anything starting with `.` (which covers
`.git`, `.DS_Store` and every editor dropping in one rule).

## boot.cfg

The startup description, packed as an ordinary entry. Text, one `key value` per
line, because a binary struct in an archive is a version number waiting to be
forgotten. Unknown keys are **skipped**, so a newer exporter cannot stop an older
runtime from starting.

```
daidalos-boot 1
scene scenes/main.daidalos
title My Game
width 1280
height 720
fullscreen 0
msaa 4
tick_hz 60
max_bodies 4096
physics 0
gravity 0 -9.81 0
```

`scene`, `title`, `tick_hz`, `max_bodies`, `physics` and `gravity` come from the
project's own settings, so the shipped game simulates what the editor simulated.
Two people who disagree about the tick rate are not playing the same game, and
that has to survive the export or the setting was decoration.

## The virtual file system

`include/dai_vfs.h`. One read path with two backings:

```c
dai_vfs_mount_dir("MyGame", 0);                  /* the editor: a folder     */
dai_vfs_mount_archive(NULL, 0, err, sizeof err); /* the game: its own tail   */

size_t n;
void *bytes = dai_vfs_read("scenes/main.daidalos", &n);
...
dai_vfs_free(bytes);
```

- Paths are always `/` separated and relative. `\` is accepted (a project
  authored on Windows has to load on Linux) and normalised.
- `..`, absolute paths and drive letters are **rejected**, not normalised. The
  only thing a `..` in a scene file can be is a bug or an attack, and quietly
  fixing it hides both.
- Sources are searched by priority, highest first; among equal priorities the one
  mounted last wins. Mount the shipped archive at 0 and a patch folder at 10 and
  the patch wins, with nothing else changing.
- `dai_vfs_read` returns a buffer with one extra zero byte past the end, so a
  scene or a script can go straight into a parser that wants a C string.

## Running the runtime

```
MyGame                             the export: the archive is inside the file
daidalos_runtime <project-dir>     development: read the project off disk
daidalos_runtime <game.dpk>        a standalone archive
daidalos_runtime --headless 120    no window: mount, load, simulate, report
daidalos_runtime ... --shot f.ppm  render one frame off screen and write it
```

`--headless` needs no GPU and no display; `--shot` needs a Vulkan device but still
no display, which is what makes an export testable on a build server.

The game writes a log **next to the executable** (`MyGame.log`), always. On
Windows it is linked with `-mwindows`, so there is no console for `printf` to go
to — and "what did it say when it did not start" is the first question anyone
asks.

## Differences from the editor

| | editor | runtime |
|---|---|---|
| scripts | run between Play and Stop | always; there is no play button |
| assets | Mnemosyne, mounts folders, watches mtimes | VFS bytes → `dai_gltf_load_memory`, loaded once |
| camera | editor camera, or the game camera in the Game tab | the node tagged `MainCamera` |
| files | `fopen` on project paths | `dai_vfs_read` only |
| no camera | draws "No cameras rendering" | frames the scene and says so in the log |

The last row is deliberate: telling an author about a mistake is useful, and
showing a player a black window is not.

## Calling the export from code

```c
char err[256];
if (dai_project_export(project, "MyGame.exe",
                       "build-win/daidalos_runtime.exe", err, sizeof err) != DAI_OK)
    printf("export failed: %s\n", err);
```

On failure nothing is left at the target: the archive is written to a temporary
file and renamed only once it is complete.

`dai_project_export` is declared in `include/dai_project.h` but implemented in
`src/dai_export.cpp`, because `build.sh` links `tests/test_project.cpp` against
`src/dai_project.cpp` **and nothing else** — the project layer must not drag an
archive writer in behind it.
