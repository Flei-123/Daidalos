# INNEN — the room generator

"Wachstum statt Karte" (`docs/GDD_INNEN.md` §5.1): the house is not a level
somebody drew, it is a graph that grows off door sockets. This is the first
version of it, and like M0 it is **data, not C++** — one JavaScript file sent
down the Jarvis bridge.

```
examples/scripts/innen_lib.js    the parts a room is made of
examples/scripts/innen_gen.js    the generator
tools/innen_gen.py               what a generated floor has to be true about itself
tools/innen_gen_shot.py          a plan view and two from eye height
projects/Untitled/scenes/innen_gen.daiscene   the last floor shot, openable
```

## Running it

```
tools/build_modeling_shot.sh .gauntlet-shots     # once
tools/innen_gen.py --seed 7                      # build and check a floor
tools/innen_gen_shot.py --seed 7                 # ...and photograph it
```

Or into the editor, with a window:

```
DAI_BRIDGE_PORT=8181 ./build/editor_demo &
tools/daibridge.py js "var GEN={seed:7,rooms:14}" 
tools/daibridge.py js -f examples/scripts/innen_lib.js -f examples/scripts/innen_gen.js
```

`tools/daibridge.py js -f` now takes **several files** and concatenates them
into one eval, because a level script is written against the library and the
bridge evaluates one string per command.

## The two halves, and why they are separate

```
plan(seed, n)    pure arithmetic - rooms, sizes, doorways, the graph.
                 No editor call, no node, nothing that needs a document.
build(plan)      turns that value into rooms with INNEN.room()
```

A layout is a **value**. That is what makes "the same seed is the same floor" a
test instead of a hope: the plan can be computed twice and compared, and the
generator can be asked for a plan with `planOnly: true` without building
anything at all. It is also the shape the finished game needs, where a room is
planned when a door is opened and built a frame later.

## How it grows

1. The phone box stands at the origin with one door, facing −Z.
2. Take an open socket — the oldest few, so the house grows outwards instead of
   boring one corridor into the distance.
3. Draw a room type from the **zone table** for that depth (§5.1: zone 1 is the
   flat you remember, zone 5 is the building it never was), draw a size, and
   dock it: the new room's centre is placed so that its wall lands exactly on
   that socket. There is one function for that arithmetic and everything goes
   through it.
4. Reject it if its **inside** overlaps another room's inside. Five tries, then
   the doorway stays a doorway into a wall — the parent's wall already has the
   hole in it, so it is boarded up, not deleted.
5. Give the new room its own ways on, one per wall slot; a long wall takes two.
   A room that draws zero is a dead end, which the GDD asks for.

## Rooms sit on a 60 cm module

What is snapped is the **wall line span** — centre of wall to centre of wall —
not the inner size. So a room's half span is always a multiple of 30 cm and
every room in the house sits on one grid, whatever its walls are dimensioned.

That is not tidiness. With free sizes two rooms *never* share a wall line, and
then the loop of §5.1 — a door that comes out in a room that is already there —
can only happen by accident, which is to say never. On a module rooms end up
back to back constantly: in a fourteen room floor about ten pairs share a wall.

## Loops

Two ways, both of which move no geometry at all:

* When an opening is made, it is tried against every room that already stands.
  If it sits on that room's wall line, inside the wall and clear of its other
  doors, the matching opening is cut on the other side and the two are joined.
* Afterwards, every pair of rooms that shares at least 1.9 m of wall gets a
  door with probability 0.6, unless a door is already near that spot.

Most shared walls are between a room and its own parent, so most of them are
already a door — which is why loops stay rare: one or two per floor, about a
tenth of the doorways, which is the number the GDD asks for. `tools/innen_gen.py`
checks four seeds and fails if **none** of them produces a loop, because rare
and dead look the same from one sample.

## What the test asserts

`tools/innen_gen.py`, ~100 checks:

* **The same seed is the same floor**, down to every room's centre and size —
  and a different seed is not.
* **The growth does not stall**: asking for fourteen rooms gives at least
  eleven.
* **No room stands inside another**, measured as rectangles from the
  document's own node positions.
* **Every door docks**: two sockets at the same world point, opposite normals,
  same opening. The one contract of `docs/INNEN_M0.md`, now generated.
* **Everything is reachable** from the phone box, walked breadth first.
* **Zones rise by exactly one** across every edge that is not a loop, because
  the zone tables are indexed by depth.
* **One script is one Ctrl-Z.**

## The bug this found

The bridge read a command's answer into a fixed **4096 byte** buffer. A floor
plan is bigger than that, so it came back cut off in the middle of a number:
JSON with no closing brace, and a parse error in the caller that says nothing
about where it came from. There is now `dai_script_get_string_size()` and the
bridge sizes the buffer to the value. `tools/bridge_check.py` asks for a ten
kilobyte answer and checks its last characters.

## What it is not yet

No rebuild rule, no warm rooms, no anti-pingpong (§5.2), no anchors, no
portals, no runtime growth — the floor is built once, in full, before the game
starts. Those are all about a room **changing while the player is in the
house**, and there was no house to change until this existed.
