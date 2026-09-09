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
24-innen-spieler.png from the player's own eyes, torch on
25-innen-tuer.png    the shut door at the end of the hallway
26-innen-treppe.png  the staircase, from the foot of it
```

The ceilings go back on before the scene is saved, so the file on disk is the
level and not the photograph.

## The player

`projects/Untitled/assets/innen_player.js` - first person, no jump, one torch.
WASD walks relative to where you look, Shift is the fastest this game gets,
Ctrl crouches, F switches the torch, and the torch's battery drains and starts
to stutter under a quarter, without ever showing a number. The level script
places the capsule, the camera in its head and the spot light in its hand, and
writes the behaviour and its inspector values onto the node - so a player
exists the moment the level does, and no one has to drag three nodes together.

Two things about the capsule are worth knowing because they cost an evening:

* `transform.scale` on a capsule is *radius* and *half height* factors of the
  0.5 half extent, so `0.55 x 1.30` is 0.55 m across and 1.85 m tall. At 1.75
  it came out exactly 2.30 m - the inner height of the phone box - and the
  player stood wedged between floor and ceiling with the walk running.
* A blockout or CSG node has the DEFAULT 1 m collider, not the shape it draws.
  Every one of them is switched to graphics-only, and the walls with doorways
  get invisible collider boxes around the hole (`*.Hit*`). Mesh and collider are
  two different objects, on purpose.

## Walking it, in the shipped game

`tools/innen_walk.py` builds the level over the bridge, switches the player to
`autoWalk`, saves the scene into a throwaway project and runs
`build/daidalos_runtime --headless` - the SHIPPED binary, no window, no
keyboard - while `walk_probe.js` prints position, velocity and grounded. The
checks: the player stays on the floor, walks out of the phone box, through the
hallway without touching its walls, through the second doorway into the hall,
and stops at the far wall.

That test found four real bugs in the runtime, all of the same family: an
exported game did not run behaviours the way the editor does. No `self`
(the object model was never installed), no `input`/`body` (the play host was
never bound), `node.setNum()` a no-op (the component half of the node host was
null), and every inspector field arriving as a *string*, so numbers fell back
to their defaults. See the end of `docs/SCRIPTING.md`.

## Doors

`projects/Untitled/assets/innen_door.js` sits on the LEAF - one box the size of
the opening, with a collider, standing in the doorway. The behaviour works out
where the hinge edge is from the leaf's own pose (half a width to the hinge
side, turned by the closed yaw), so a door is placed like any other box and
never needs a second node dragged into place. `E` opens and closes it while you
are within `range`; a locked one says no.

Five leaves stand in the level: the phone box's door, which starts open because
you just walked through it and **slams behind you and locks**; the door at the
end of the hallway, shut but not locked; and the three doors off the corridor,
all locked, so that trying them is answered.

Three things this cost:

* The leaf is **kinematic**. The runtime moves a node that has a body by
  setting the BODY's transform - so a static one would be teleporting behind
  the solver's back, and a dynamic one falls over.
* Children of a moved body **do not follow**, because nothing writes the
  document. The handle is therefore not a child: the door script carries it, by
  name, and that is why `handle` is a field.
* `slamBehind` fires only once the player is `slamAfter` metres **past** the
  plane. The first version fired on the crossing itself, closed onto the player
  still standing in the frame, and pushed him back into the phone box - the
  walk test stopped dead at z = -0.41 and said "never left the box".

## The staircase, and the thing that made it climbable

The flight in the hall is 23 steps, 0.189 m of rise and 0.261 m of going, and
it goes up **into** the ceiling. What the player walks on is not the treads: it
is one invisible ramp under them, at the pitch of the flight
(`Halle.Treppe.Rampe`).

One collider box per step was the first version, and it does not work: a
capsule in this engine has no step-up, so it walks into the 19 cm riser of step
one and stops there with the motor running - `tools/innen_walk.py` measured
0.25 m climbed in twelve seconds. With the ramp it measures 2.33 m. Every
engine that ships stairs does this; the steps are what you see, the ramp is
what you climb.

The staircase is also turned 180 degrees, so it climbs the way the player
walks. Which is where `transform.rotation` came in.

## The property that was accepted and dropped

`transform.rotation` had **no entry in the component table**. Every host took
`node.setVec(n, "transform.rotation", 0, 180, 0)` without complaint and stored
nothing: the document keeps a quaternion, the table only knew plain `dai_vec3`
fields, and the conversion to degrees lived privately inside
`src/dai_editor_ui.cpp`. The handset in the phone box and the whole staircase
were written turned and came out straight, and nothing anywhere said so.

There is now one conversion - `include/dai_euler.h`, ZYX degrees - used by the
inspector and by `comp_get_vec`/`comp_set_vec`, plus `transform.position` for
symmetry. `tests/test_euler.cpp` pins its meaning down (15 checks, including
the sign that decides whether the stair ramp lifts or sinks), and
`tools/innen_check.py` reads the ramp's pitch back out of the document, which
is the same property end to end.

A rotation has more than one spelling: `(0, 180, 0)` and `(180, 0, 180)` are
the same half turn, and the document hands back whichever the conversion
produced. The check therefore turns the staircase's own forward vector and asks
where it points, instead of comparing three numbers.

## What M0 is not

No room generator, no sound, no keys to pick up (a locked door stays locked).
It is the geometry contract plus a body to walk it: rooms that dock, openings
that are holes, doors that open, stairs that carry, a level that is data. The
generator (GDD §5, "Graph statt Grid") plugs into the sockets described here;
the anti-pingpong rules of §5.2 are about which room is placed on a socket, not
about how a socket works.
