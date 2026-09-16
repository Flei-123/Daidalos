# INNEN — Anker

The rules of `docs/GDD_INNEN.md` §5.3, with the inventory of §5.9, running in
the shipped game: `projects/Untitled/assets/innen_anker.js`, one behaviour on
one node called `Anker` that the generator puts into every floor it builds.

```
projects/Untitled/assets/innen_anker.js   the objects and the jacket
projects/Untitled/assets/innen_haus.js    the freeze itself (the `anchors` map)
examples/scripts/innen_lib.js             INNEN.anker() - what the five things are
tools/innen_anker.py                      the numbers they have to come out at
```

## The five things out of your car

| object | size (m) | slots |
|---|---|---|
| `Anker.Warndreieck` | 0.43 × 0.40 × 0.04 | 2 |
| `Anker.ErsteHilfe` | 0.26 × 0.18 × 0.11 | 2 |
| `Anker.Eiskratzer` | 0.12 × 0.02 × 0.24 | 2 |
| `Anker.Tankquittung` | 0.08 × 0.001 × 0.14 | 2 |
| `Anker.Foto` | 0.09 × 0.002 × 0.13 | 2 |

They are built at the size the thing actually is, and the difference is the
point: you can tell across a room which one you left there. They start on the
floor of the phone box — room 0 — because that is where you arrive with a
car's worth of belongings.

They are **graphics only**, no rigidbody. An anchor is picked up by standing
near it and pressing `E`; a dynamic body would spend the game rolling down the
stair ramp in the hall.

## What an anchor does

> *Anker funktionieren, weil sie **echt** sind — von draußen, nicht aus
> Erinnerung. Das Gehäuse kann Echtes nicht überschreiben.* (§5.3)

Put one down in a room and the room is never rebuilt again. Pick it up and the
room is the house's once more. That is the whole mechanic, and the interesting
part is that it is **not a new rule**.

`innen_haus.js` has always had an `anchors` map that the rebuild dice consult
before every roll — the phone box has been in it since the generator was
written. So an anchor does not introduce a freeze; it **writes into the freeze
that was already there**, first in `enterRoom()`, before warm rooms, before
the cold-distance check, before the dice:

```js
if (anchors[room]) { stat.frozen++; stat.anchorFrozen++; return "anchor"; }
```

The channel is the one the rest of the game uses — `node.tag`, a real field
that survives a save and shows up in the inspector:

```
Anker    tag = "0,3,7"    the rooms that hold something real
```

`innen_haus.js` reads that tag four times a second and rebuilds its map from
it. Room 0 is always in the list: the phone box is anchored by the level, not
by an object, and picking up all five things must not un-anchor it. That is
why the house keeps the level's anchors separately (`fixedAnchors`) and
re-applies them on every update.

**Why the objects are not spawned by the behaviour:** a behaviour has no
`editor` and may not invent nodes. `INNEN.anker()` in the level script places
them; this behaviour only ever moves, hides and shows them. Picking one up
sets `renderer.enabled = 0` rather than destroying it — a behaviour that
destroyed what an author placed could not give it back.

## The jacket, §5.9

Four pockets. The torch is in your hand and uses none of them. An anchor is
big and takes **two**. So two anchors is the most anybody can carry, and
carrying two means carrying nothing else.

There is no menu and no crafting. The refusal is a message, not a dialog:

```
ANKER refused=full item=Anker.Eiskratzer needs=2 free=0
```

## Measuring it

"A room with an anchor in it is never rebuilt" is the kind of promise that is
easy to write and easy to have quietly stop being true: the anchor list is one
behaviour's idea and the rebuild dice are another's, and the day the two stop
agreeing, nothing looks wrong. So it is measured in the binary that ships.

`tools/innen_anker.py`:

1. builds a generated floor over the bridge,
2. gives the `Anker` behaviour a written programme — take, take, take one too
   many, drop, take back — and tells the `Haus` behaviour to anchor **room 5
   for the first 2000 of 4000 simulated moves and release it for the second
   2000**,
3. runs `build/daidalos_runtime --headless`, the shipped binary,
4. reads the `ANKER ...` and `HAUS ...` lines.

**27 checks.** Measured, seed 7, 4000 moves:

```
room 5, anchored (moves 0-2000):     0 rebuilds
room 5, released (moves 2000-4000):  26 rebuilds in 96 visits
anchorFrozen = 271     the rule fired 271 times
anchorViolations = 0
violations = 0         the warm-room rule was not disturbed
```

Both halves matter. The first is the promise. The second is what stops the
first from being true for a boring reason — a test that only checked "0
rebuilds while anchored" would also pass if anchors froze the house forever,
or if room 5 were simply never visited. 26 rebuilds in 96 visits after the
object is picked up is the room going back to being the house's.

The rest:

* **The five objects of §5.3 exist**, by name, in the level the generator
  built — asked of the document, not of the script that was supposed to place
  them.
* **The jacket holds.** Two anchors fill it; the third is refused, and it is
  refused *for want of a pocket* (`refused=full`) rather than by something
  going wrong. At seed 7: 2 refusals in the programme.
* **Taking and dropping are symmetric** — what is in a room is out of the
  jacket, with nothing lost or duplicated.
* **The house and the anchors agree**: the list the anchor behaviour publishes
  is the list the house's rules are actually using, compared through the
  runtime's own log rather than by reading both files and hoping.

## Two things that cost a run each

**The script field format splits on commas.** The document stores a
behaviour's inspector values as `file.js{a=1,b=2}` and the runtime splits that
string on `,`. So the item list and the test programme are separated with
`|` — written with commas, the five item names arrived as five broken fields
and the six-step programme ran exactly its first step. It is not a style
choice; it is the only separator that survives the format.

**The §5.4 sight rule does not apply to the self test.** The house's dice test
walks the *graph* — the player's capsule stands where the level left it for
all 4000 moves. Asking "can he see this room from where he is standing" would
have answered about the spawn point four thousand times and silently frozen
whichever rooms happen to lie in front of it, which would have moved the
measured 50/30/15/5 distribution and made it depend on where the phone box
points. `enterRoom()` therefore takes a `simulated` flag that gates that one
rule and nothing else. Everything the dice test measures is the same code the
real walk runs.

## What is missing

Batteries and the other small items that would make four pockets a real
decision — right now the only thing worth carrying is an anchor, so the
inventory is "two anchors or one". The −10 % Kopie-Fortschritt per anchor set
(§5.5), because there is no Kopie yet. And the hidden trigger of §5.3: all five
anchors stacked in the Vorraum giving the front door its *Echtheit* back,
which is the alternative route into Ende C.
