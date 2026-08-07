# Game UI: flex containers and style tokens

**Status: proposal. Nothing here is built yet — task #11 was explicitly "agree
the concept first".** This document is the thing to argue with; the code comes
after a yes.

## What is wrong today

The game's UI is drawn imperatively, in pixels:

```js
gui.rect(20, 20, 200, 40, 0x202020FF);
gui.text(28, 30, "Score: " + state.score, 16, 0xFFFFFFFF);
gui.button(28, 70, 120, 32, "Pause");
```

Three problems, and they are the same three every UI has before it grows a
layout engine:

1. **Every number is a magic number.** Move the panel and every child moves by
   hand. Add one row and everything under it shifts by 40 — in three files.
2. **Nothing adapts.** The picture is 1920x1080 in the editor and 1280x720 on
   someone's laptop; a hard coded 20 is 20 in both, so the HUD is a different
   size relative to the screen in each.
3. **Colours and spacings are copied.** `0x202020FF` appears eleven times. A
   theme change is a search and replace, and the twelfth place is always
   missed.

## The proposal, in one sentence

Two additions, neither of which replaces what exists: **containers that place
their children** and **named tokens instead of literals**.

## Part 1 — flex containers

One layout model, the one every developer already knows from CSS and from
Flutter: a row or a column, children laid along the main axis.

```js
gui.column({ gap: 8, padding: 12, align: "stretch" }, function () {
    gui.label("Score: " + state.score, { style: "h2" });
    gui.row({ gap: 8, justify: "end" }, function () {
        gui.button("Pause",  { grow: 1 });
        gui.button("Resume", { grow: 1 });
    });
});
```

### The container's own properties

| name | meaning |
|---|---|
| `gap` | space BETWEEN children, not around them |
| `padding` | space inside the container's edge; one number or `[t, r, b, l]` |
| `justify` | along the main axis: `start`, `center`, `end`, `between`, `around` |
| `align` | across it: `start`, `center`, `end`, `stretch` |
| `wrap` | `false` (default) or `true` |

### The child's

| name | meaning |
|---|---|
| `basis` | the size it asks for before any stretching, in px or `"auto"` |
| `grow` | share of the LEFTOVER space it takes (0 = none) |
| `shrink` | share of the OVERFLOW it gives back (1 = default) |
| `align` | overrides the parent's `align` for this one child |

That is a deliberately small subset of CSS flexbox: one line of children per
container unless `wrap`, no `order`, no `flex-flow` shorthand. Every one of
those was left out because it can be added later without breaking a scene, and
none of them is what a health bar needs.

### How it resolves — the three pass rule

The whole engine is one function that runs three times over the tree, which is
what keeps it small enough to be worth having:

1. **Measure.** Ask every child its natural size, bottom up. Text measures
   through the font atlas the HUD already uses; an image measures its texture;
   a container measures the sum of its children plus gaps and padding.
2. **Distribute.** Along the main axis: hand out `basis`, then share the
   leftover by `grow`, or claw back the overflow by `shrink`. Across it, apply
   `align`.
3. **Place.** Walk down assigning absolute rectangles, then emit exactly the
   `gui.rect` / `gui.text` calls the immediate mode API already takes.

Nothing about the renderer changes. **The layout engine is a producer of the
same draw calls that exist today**, which means the old imperative spelling
keeps working, side by side, in the same frame — the same compatibility rule
the scripting object model was held to.

### Where it lives

`src/dai_flex.cpp` + `include/dai_flex.h`, dependent on nothing but the font
metrics. That makes it unit testable with no GPU and no window: feed it a
tree, read the rectangles back, compare to numbers a human worked out. That is
how `dai_thumb` and `dai_hud` are tested and it is why both are trustworthy.

## Part 2 — style tokens

A token is a **named value stored in the project**, not in a script:

```
# project/ui/tokens.daitokens
color.bg          #1E1E1EFF
color.surface     #2A2A2AFF
color.accent      #4C8BF5FF
color.text        #EAEAEAFF
color.text.dim    #9A9A9AFF

space.xs 4
space.sm 8
space.md 12
space.lg 20

radius.sm 3
radius.md 6

text.h1  { size: 28, weight: bold,   color: color.text }
text.h2  { size: 20, weight: medium, color: color.text }
text.body{ size: 14, weight: regular,color: color.text }
text.dim { size: 14, weight: regular,color: color.text.dim }
```

Used by name, never by value:

```js
gui.column({ gap: "space.md", padding: "space.lg", bg: "color.surface" }, ...)
gui.label("Game Over", { style: "text.h1" });
```

Three properties this buys:

- **One place to change a theme.** Dark and light are two token files.
- **The editor can edit them.** A Tokens window with colour pickers writes the
  same file; no script is touched.
- **A token that does not exist is a visible error**, not a black rectangle.
  Unknown name → the value falls back to a loud magenta and one Console line
  naming the token and the file. Silence is what makes theming bugs expensive.

Text styles reference colour tokens, so the indirection is one level deep and
resolvable at load. No cycles, no cascade, no inheritance — those are the
parts of CSS that make CSS hard.

## What this is NOT

- Not a retained scene graph. The tree is rebuilt every frame, exactly as now.
  Layout is arithmetic, not state.
- Not a replacement for the editor's own UI (`dai_ui.cpp`). That is a different
  problem with a different owner, and merging them is how both get worse.
- Not CSS. No selectors, no cascade, no inheritance, no units but pixels and
  the container's own fraction.

## Suggested order of work

1. `dai_flex` measure/distribute/place + tests, no bindings. **Half a day.**
   Verifiable at the server with no GPU.
2. Bind `gui.row` / `gui.column` / `gui.label` to it in `dai_script.cpp`, and
   the same three in the C++ GUI API. **Half a day.**
3. Token file format, loader, and the `"name"`-instead-of-number resolution.
   **Half a day.**
4. A Tokens window in the editor that edits the file. **A day.**

Steps 1–3 are worth doing on their own; step 4 only pays off once someone has
lived with 1–3 for a week.

## The open questions — these are what I need an answer to

1. **Spelling.** `gui.column({...}, function () {...})` (a callback, shown
   above) or `gui.columnBegin({...}) ... gui.columnEnd()` (a pair, matching the
   existing `gui.panel` / `gui.panelEnd`)? The callback nests better and cannot
   be unbalanced; the pair matches what is already there and works in C++
   without a lambda.
2. **Are percentages needed**, or is `grow` enough? `basis: "50%"` is easy to
   add and easy to regret.
3. **Does the token file belong in the project root** (`ui/tokens.daitokens`)
   or in the scene? Per project is my recommendation — a HUD that changes
   colour between two scenes of one game is a bug, not a feature.
4. **How much of the editor's own theme should the tokens default to?** Starting
   the game UI with the editor's dark palette makes a new project look designed
   rather than beige; it also makes the game look like the editor.
