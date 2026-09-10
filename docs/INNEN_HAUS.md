# INNEN — das Gehäuse

The rules of `docs/GDD_INNEN.md` §5.2, running in the shipped game:
`projects/Untitled/assets/innen_haus.js`, one behaviour on one node called
`Haus` that the generator puts into every floor it builds.

```
projects/Untitled/assets/innen_haus.js   the rules
tools/innen_haus.py                      the numbers they have to come out at
```

## What it does

| rule | what it means |
|---|---|
| **warme Räume** | The last three rooms the player stood in are frozen, whatever else is true. The way back is always the way back. |
| **kalt** | A room he has not been within three doors of since he left it may change when he next walks in. |
| **Umbau** | 50 % nothing, 30 % a detail, 15 % the same kind of room again, 5 % something wrong. |
| **Anker** | Listed rooms never change. The phone box is one from the start. |
| **Pingpong** | Three times back and forth through the same door is not answered with a reroll but with a **reaction**. |

The reaction is weighted the way §5.2 weights it: 40 % the door stops opening,
35 % the lights go out for a while, 25 % the room's light drops to a quarter
and flickers. The two reactions that need a *Kopie* — a double of the player —
are folded into darkness for now, because there is no Kopie yet.

## What it can and cannot rebuild

Everything under "a detail" is something the engine can do to a room that is
already standing: the light is the wrong colour, a lamp has moved, one of the
room's doors does not open any more ("eine Tür weniger"). That is the 30 %.

The 15 % "same kind of room again" and the 5 % "something wrong" need a room to
be **built while the game runs**, and this engine has no spawn API for a
behaviour: `editor` is deliberately not bound in a game, and `scene` can only
find nodes. So those two are drawn, counted and logged as `want=replace` /
`want=wrong` and nothing happens. They are in the distribution the test checks,
so the day a spawn API exists the numbers do not have to move — only the
effect.

That is the honest state: **80 % of the rebuild rule runs, 20 % is measured and
not yet performed.**

## How it sees the house

A behaviour has no `editor`, so it cannot enumerate the document. It can only
look things up by name — so the generator names things so that they can be
found:

```
Raum03            a room group, tag = "Zimmer" (its kind)
Raum03.Floor      its floor slab: scale is (W + 2t, t, D + 2t)
Raum03.Licht.1    its ceiling lights
Tuer07            a door leaf, tag = "3>7" (the two rooms it joins)
```

The room's rectangle comes out of the floor slab, because the slab is the room
plus its walls and the walls divide back out. The graph comes out of the door
tags.

The tag is also the **channel back**: when the house locks a door it writes
`!3>7`, and `innen_door.js` reads its own tag four times a second. A tag is a
real field on the node — it survives a save and it is visible in the inspector
— which is why it and not a hidden side channel.

## Measuring it

Rules like these look right in a play session and are wrong by a third in the
numbers. `tools/innen_haus.py` therefore does not look:

1. builds a generated floor over the bridge,
2. switches the `Haus` behaviour into `selfTest`, which walks the room graph a
   few thousand times at startup — with a bias for turning round, so the
   pingpong rule is provoked — through the **same** `enterRoom()` the game
   runs,
3. runs `build/daidalos_runtime --headless`, the shipped binary,
4. reads the `HAUS ...` lines.

It checks, 22 of them:

* **A warm room is never rebuilt.** The self test knows before every move what
  the answer has to be, and counts every time it is not. Exactly zero.
* **The dice are the dice**: 50/30/15/5 within five points over several hundred
  draws.
* **The house reacts** to pingpong, and uses all three reactions; the logged
  lock lines and the counted ones agree.
* **A walk that goes forwards rebuilds far more than one that doubles back** —
  otherwise the rule would not be "warm rooms are frozen", it would be "nothing
  ever changes". Measured: 88 % of a bouncing walk's moves are frozen against
  far fewer of a forward one's.
* **The same seed behaves the same way**, twice, down to the last count. The
  dice are the generator's own LCG, never `Math.random()`.

Typical run, seed 7: 4000 moves, 469 rebuild draws, 3531 frozen, 1009
reactions (400 lock / 373 dark / 236 flicker), 0 violations.

## What is missing

Runtime room building (the 20 % above, and the growth of the house behind a
door the player opens), the Kopie, anchors as items you can carry and drop,
the light pulse of §5.4, and portals.
