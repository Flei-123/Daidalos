# INNEN — M0: the first three rooms

The first playable piece of the game in `docs/GDD_INNEN.md`, and the first
level built the way every later level will be built: **as JavaScript, sent
down the Jarvis bridge**. There is no room in any `.cpp` file.

```
examples/scripts/innen_m0.js     the level, as data
tools/innen_check.py             what the level has to be true about itself
tools/innen_shots.sh             four pictures from eye height + a plan view
projects/Untitled/scenes/innen_m0.daiscene   the result, openable in the editor
```

## Running it

```
./build.sh                                   # once
tools/build_modeling_shot.sh .gauntlet-shots # compiles the headless host
tools/innen_shots.sh                         # builds the rooms and photographs them
tools/innen_check.py --binary build/modeling_shot   # and checks them
```

Or in the editor, with a window and a mouse:

```
DAI_BRIDGE_PORT=8181 ./build/editor_demo &
tools/daibridge.py js -f examples/scripts/innen_m0.js
```

Both go through the same socket. `tools/run_tests.sh` runs the check and the
pictures on every green run.

## The three rooms

| room | inner size (m) | surfaces | what it is |
|---|---|---|---|
| `Zelle` | 1.30 × 1.30 × 2.30 | rusted sheet | the phone box you step into; the phone is a kitbash of blockout primitives with a bevel modifier |
| `Flur` | 1.70 × 9.00 × 2.55 | woodchip + vinyl | your parents' hallway, remembered too narrow; three ceiling lights, the far one nearly out |
| `Halle` | 9.00 × 7.00 × 4.20 | same woodchip, concrete ceiling | the first room that cannot fit in the house it is wallpapered like; a staircase that ends in the ceiling |

They stand in a line along −Z: box at z = 0, hallway centred at z = −5.285,
hall at z = −13.46. Those numbers are not typed anywhere — the script derives
them from the room depths and wall thicknesses, so changing a room moves its
neighbours.

## The one rule everything rests on: sockets

A room exposes `DoorSocket` nodes — `door.width`, `door.height`,
`door.normal`, named `<Room>.Door.<side><n>`. **A door is where two sockets sit
at the same point in the world with opposite normals and the same opening.**
That is how the generator of the finished game will place rooms: pick a room,
match a socket, done. It never invents geometry.

`tools/innen_check.py` asserts exactly that, plus:

* the rooms do not stand inside each other (three boxes, checked as boxes),
* a doorway is a **CSG subtraction** — a slab with a cut under it — and not a
  solid wall with a socket floating in front of it,
* one script is **one Ctrl-Z**: undo removes three rooms, redo brings back the
  same number of nodes.

Move a room by 30 cm and the first check goes red with both world positions
printed. That was tried before this file was written.

## The pictures

`tools/innen_shots.sh` starts `modeling_shot --serve`, sends the level, and
asks the bridge for four cameras at 1.55–1.70 m — eye height, because a room
that only reads from a drone shot is a room nobody has stood in.

```
20-innen-zelle.png   inside the box, looking out through its door
21-innen-flur.png    the hallway down its length, into the dark end
22-innen-halle.png   the hall, with the stairs to nowhere
23-innen-plan.png    all three from above, ceilings switched off for the shot
```

The ceilings go back on before the scene is saved, so the file on disk is the
level and not the photograph.

## What M0 is not

No player, no doors that open, no room generator, no sound. It is the geometry
contract: rooms that dock, openings that are holes, a level that is data. The
generator (GDD §5, "Graph statt Grid") plugs into the sockets described here;
the anti-pingpong rules of §5.2 are about which room is placed on a socket, not
about how a socket works.
