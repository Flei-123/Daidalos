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
| **Anker** | Listed rooms never change. The phone box is one from the start, and since [`INNEN_ANKER.md`](INNEN_ANKER.md) the five objects the player carries add to the same list, through the `Anker` node's tag. |
| **Puls** | While [`innen_puls.js`](INNEN_PULS.md) says `dunkel`, a room the player can **see** may be rebuilt too — but only outside his torch cone. In the bright phase, seeing a room protects it. |
| **Pingpong** | Three times back and forth through the same door is not answered with a reroll but with a **reaction**. |

The reaction is weighted the way §5.2 weights it: 40 % the door stops opening,
35 % the lights go out for a while, 25 % the room's light drops to a quarter
and flickers. The two reactions that need a *Kopie* — a double of the player —
are folded into darkness for now, because there is no Kopie yet.

## What it rebuilds, and with what

Everything under "a detail" is something the engine can do to a room that is
already standing: the light is the wrong colour, a lamp has moved, one of the
room's doors does not open any more ("eine Tür weniger"). That is the 30 %.

The 15 % "same kind of room again" and the 5 % "something wrong" need a room to
be **built while the game runs**. Since `include/dai_spawn_host.inl` a
behaviour can do that — not with `editor` (a game that can invent nodes and
save over the scene is a save game corrupter waiting for a typo) but by
**copying** something an author placed:

```js
var made = scene.spawn(src, 0, "Raum03.neu");   // the whole subtree
scene.destroy(old);                             // the node and its descendants
```

| roll | what actually happens |
|---|---|
| **15 % same kind** | The room is copied **from itself** — the only source whose door holes are guaranteed to line up with the doors already hanging in the neighbours' walls — the original is torn down, and the new one comes back wearing **another room's materials**. Same plan, different building. That is what déjà-vu is supposed to feel like. |
| **5 % wrong** | Another room of roughly the same footprint is put up in the hole. Its door holes are in the wrong walls, its surfaces belong to a different part of the house, and the door you came through opens into plaster. This is the roll that is *allowed* to be broken. |

Two details that are not decoration:

* The copy is spawned **before** the original is destroyed. A spawn that is
  refused (a bad id, the budget) has to leave the player standing in a room and
  not in the sky.
* The copy carries the SOURCE's child names — `Raum07.Floor` under a room now
  called `Raum03`. The subtree is renamed, because `readRooms()` finds a room's
  floor slab by name and a house whose rooms answer under the wrong name
  measures the wrong rectangle.

The room's **lights are not part of the subtree** (the generator parents them
to the root), so they survive the rebuild. The room changes, the lamp that was
above you stays where it was.

**All of the rule runs now.** `builtReplace` and `builtWrong` in the self test
line have to equal `wantReplace` and `wantWrong`, and
`tools/innen_haus.py` fails if they do not.

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

## The two rules that were added later

Both of them live **inside** `enterRoom()` rather than beside it, because a
second place that decides whether a room may change is a second place to keep
in step:

* **§5.3, the anchors.** First question asked, before warm rooms and before
  the dice: `if (anchors[room])`. The map is the one that was always there;
  `innen_anker.js` writes into it through the `Anker` node's tag. See
  [`INNEN_ANKER.md`](INNEN_ANKER.md).
* **§5.4, the sight rule.** After the warm and cold checks, before the dice:
  a room the player can see is frozen — unless the pulse says `dunkel`, in
  which case only the torch cone still protects it. See
  [`INNEN_PULS.md`](INNEN_PULS.md).

`enterRoom()` takes a `simulated` flag that gates the sight rule and nothing
else. The self test walks the *graph* while the player's capsule stands still,
so a geometric "can he see it" test would have answered about the spawn point
four thousand times and quietly moved the measured distribution. Everything
else — warm rooms, anchors, the dice, pingpong — is the same code on both
paths.

## What is missing

The growth of the house behind a door the player opens, the Kopie, portals,
and the §5.2 pingpong reaction that is supposed to start a real dark phase
(the house still makes its own local darkness instead of asking the pulse).

## The leak test

A spawn without its destroy leaks a room per rebuild — about seventy nodes,
hundreds of times — and nothing in the rules would notice. The runtime prints
how many instances it drew on the first and on the last frame, and
`tools/innen_haus.py` compares them: at seed 7 the house builds **64 same-kind
rooms and 20 wrong ones** over 4000 moves and draws the same number of
instances at the end as at the start.
