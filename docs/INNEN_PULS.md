# INNEN — Licht & Puls

The rules of `docs/GDD_INNEN.md` §5.4, running in the shipped game:
`projects/Untitled/assets/innen_puls.js`, one behaviour on one node called
`Puls` that the generator puts into every floor it builds — and the half of
§5.4 that lives in the house itself, because a dark phase is only interesting
if it changes what the house is allowed to do.

```
projects/Untitled/assets/innen_puls.js   the clock
projects/Untitled/assets/innen_haus.js   what the clock changes (the sight rule)
tools/innen_puls.py                      the numbers they have to come out at
```

## The cycle

| phase | how long | what it is |
|---|---|---|
| **hell** | 4–7 minutes, drawn per cycle | Ceiling lights on, rooms stable. |
| **flacker** | 5 s, every time | The last five seconds of the bright phase. The lights stutter together. This is the only warning the player gets. |
| **dunkel** | 40–90 s, drawn per cycle | Every ceiling light out. The torch is all there is — and the house may rebuild rooms the player is **looking at**. |

The flicker is the **end of** the bright phase, not an extra five seconds
after it: a cycle the GDD calls "4–7 Minuten" must not quietly become 4–7
minutes plus a flicker, and the measurement below would have caught it if it
had.

The first bright phase is deliberately short (`ersteHell`, 95 s in a generated
floor). A mechanic the player meets after seven minutes is a mechanic he meets
once a session; one he meets after ninety seconds is a rule he learns.

## The two behaviours, and the one channel between them

A behaviour has no `editor` and no globals it shares with another behaviour.
The pulse therefore publishes its phase the way the house already talks to its
doors — through **`node.tag`**, a real field that survives a save and is
visible in the inspector:

```
Puls          tag = "hell" | "flacker" | "dunkel"
```

`innen_haus.js` reads that tag four times a second, exactly as
`innen_door.js` reads the tag the house writes to *it*. One mechanism for
every cross-behaviour message in the game.

**Why the pulse does not rebuild anything itself:** the rebuild dice, the warm
rooms and the anchors live in `innen_haus.js` and there is one of them. A
second copy of "may this room change", driven by the light, would be a second
set of rules to keep in step — and the day they disagreed, the house would
rebuild the room the player was standing in. The pulse only says what time of
day it is.

## What the dark phase changes

§5.4: *"Umbau-Regel greift auch auf Räume, die du siehst — aber nur außerhalb
deines Lichtkegels."* So seeing a room is what protects it, and the dark phase
shrinks that protection from **wherever you look** to **wherever the beam is**:

| phase | a room he can see | a room in the torch cone |
|---|---|---|
| hell | frozen (`seen`) | frozen |
| dunkel | **may be rebuilt** | frozen (`cone`) |

That is the whole trade the player is asked to make: in the dark he can hold
one corridor still with the torch, and everything else he can see is the
house's. The cone is the player's real one — 26°, `light.cone` on
`Spieler.Lampe`, the spot light `innen_lib.js` builds — and the beam direction
is read off the torch node, which `innen_player.js` aims with the eye's own
rotation every frame. There is one look direction in the game, not two.

A torch that is switched off or out of battery protects nothing:
`inTorchCone()` asks the document for `light.enabled` rather than assuming.

The sight test is deliberately crude — room centre against player position and
facing, no occlusion. A frustum-and-occlusion test is what §5.2's "kein
Sichtkontakt" will eventually mean (§10) and it needs a renderer. What matters
for the *rule* is the asymmetry above, and that is geometry the headless
runtime can answer.

## Measuring it

Three numbers nobody can check by playing: you would have to sit through forty
seven-minute cycles with a stopwatch. `tools/innen_puls.py` therefore does not
play:

1. builds a generated floor over the bridge,
2. switches the `Puls` behaviour into `selfTest`, which runs the **same**
   `step()` the game runs at a fixed dt until it has completed 40 full cycles,
3. runs `build/daidalos_runtime --headless`, the shipped binary,
4. reads the `PULS ...` lines.

**29 checks.** Measured, seed 7, 40 cycles:

```
bright  248.9 – 415.1 s   (avg 321.5)      GDD: 240 – 420
dark     40.1 –  89.8 s   (avg  69.6)      GDD:  40 –  90
flicker   5.03 s before every dark phase   GDD:   5
```

It is not enough that every phase falls inside the range — a clock that always
picked 300 s would pass that and would not be "zufällig". So the **spread** is
checked too (over 100 s of it across 40 bright phases, over 20 s across 40 dark
ones) and the average is held near the middle of a uniform draw.

The flicker is the one number with no spread: 5.03 s every time, and there is
one flicker for every dark phase. (One flicker *more* than dark phases is
allowed — the run is cut off the moment the last cycle completes, which can
leave the clock stopped mid-breath.)

Then the parts that are not arithmetic:

* **The phases run in order**, `hell → flacker → dunkel → hell`, every time.
* **The channel works end to end**: a second project with a deliberately fast
  clock reaches a real dark phase inside 240 frames, and `HAUS phase=dunkel`
  appears in the same log — the house *saw* it. Without that line the pulse
  would be a light show that changes no rules.
* **The cone is really the cone.** The house reports, room by room, what the
  sight rule answers with the torch aimed one way. At seed 7 the player can
  see 12 of 14 rooms from where he stands; the 26° beam holds some of them and
  leaves more of them exposed than it protects — which is what makes the dark
  phase a choice instead of a formality.

An earlier version of that last test aimed the torch **at each room in turn**
and then asked whether that room was in the cone. Every room within range was,
of course. It measured the torch's *range*, called it the cone, and would have
passed just as happily with the cone set to 180°. The beam now points one way
and every room is judged against that one direction.

* **The same seed breathes the same way**, twice, down to the last count. The
  dice are the generator's LCG, never `Math.random()` — a house that breathes
  differently in two runs of one seed cannot be measured and cannot be a bug
  report either.

## What is missing

The Wärter, who are supposed to wander in the dark (§5.6) — there are none
yet. Batteries to find (§5.4) — the torch drains and stutters but nothing
refills it. The relay-clack and neon hiss of §9. And the dark phase as a
*pingpong reaction*: `innen_haus.js` still answers pingpong with its own local
darkness rather than asking the pulse to start a real dark phase early, which
is what §5.2's 10 % case describes.
