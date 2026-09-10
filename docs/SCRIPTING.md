# Behaviours and their inspector fields

A behaviour is a `.js` file in the project. Attach it to an object (drag it
from Project onto the object, or Add Component) and the editor calls `init()`
once and `frame()` every frame while the game plays.

## Fields are declarations

A top level declaration **is** a serialized field. There is no attribute to
write and no comment to keep in sync:

```js
let walkSpeed = 5;      // a float field, default 5
let maxJumps  = 2;      // an int field
let sprinting = false;  // a tick box
let playerName = "Ada"; // a text field
let _timer = 0;         // NOT a field: a leading _ is private
```

The inspector stores what you type **on the node**, not in the file - two
objects can carry the same script with different numbers - and writes the
value back **into the variable** before `init()` runs. So `walkSpeed` inside
`frame()` is the number the inspector shows, and the value in the file is only
the default. Reading `params.walkSpeed` still works and is the same value.

Only a literal counts: a string, a number, `true`/`false`. An expression has
no type the editor could guess and no value it could show, so it is skipped.

`const` fields are drawn but cannot be written back - JavaScript refuses the
assignment, quietly. Use `let` for anything you want to tune.

## Headers and descriptions

Unity's `[Header]` and `[Tooltip]`, spelled the way a comment can be:

```js
// @header Movement

// Metres per second on the ground.
let speed = 6;

// @tooltip Multiplied onto speed while Shift is held.
let sprintMul = 1.7;
```

- `// @header X` (or the C# form `// [Header("X")]`) groups everything under
  it until the next header.
- `// @tooltip X`, `// [Tooltip("X")]`, or simply **a comment line directly
  above a field** becomes the description shown when the pointer rests on that
  row.

## Object references

A reference to another object has no literal to declare it with, so it keeps
the explicit form:

```js
// @param node   followCam    // any object
// @param camera mainCam      // only objects that HAVE a Camera
```

The type is the contract, exactly as in Unity, where a field declared
`Rigidbody target` offers only rigidbodies in its picker. The types are
`node`/`object` (anything), `transform`, `camera`, `light`, `rigidbody`,
`collider`, `sprite`, `audio` and `mesh`. The field shows what it points at
with its type in brackets - `Main Camera (Camera)` - and says so when an
assignment no longer fits.

It draws as Unity's object field: the name of what it points at, and a target
button that opens a searchable list of every object in the scene. Dragging a
node from the Hierarchy onto the field works too, and clicking the field
selects what it points at.

At play time the value is the object's **name**; `scene.find(name)` turns it
into a node id.

## The explicit form, for everything else

`// @param [type] name [= default]` still declares a field and still wins over
a declaration of the same name. Types: `float`, `int`, `bool`, `string`,
`node`. It is the only way to declare a field in a `.cpp` behaviour, whose
members the editor cannot see.

## Input

```js
input.key("w")        // held? letters, digits, space, enter, escape, tab,
                      // arrows, shift, ctrl, alt
input.mouseDX()       // pixels the pointer moved THIS frame
input.mouseDY()
input.mouseButton(0)  // 0 left, 1 right, 2 middle - held?
```

The mouse is a **delta**, not a position, because that is what a look control
wants and because a position would make every script do the same subtraction.
Every script in a frame sees the same movement: the host takes it once.

`examples/scripts/player_controller.js` is the worked example - camera
relative WASD, right mouse to orbit, and a camera that sets its own rotation
as well as its position. A camera that only takes a position keeps pointing
wherever the scene left it, which looks exactly like a camera that is not
following at all.

## Playing

**Ctrl+P** starts and pauses the game, Unity's binding. Space does it too,
but **only while editing** - once the game runs, the keyboard belongs to the
game, and the first jump must not pause the editor.

## The game's own UI

**Add Component > Text (UI)** puts a label on the screen. Not in the world: it
is anchored to a corner of the picture, because a score pinned to x=1720 is
off screen the moment the window is 1280 wide.

- **Anchor** is a 3x3 grid, **Offset** nudges it in pixels from there.
- **Size** is in pixels, **Color** is the usual vector.
- A node with a Text component is normally `hidden` as well - every node here
  draws a box unless it is, and a label with a grey box behind it is not a
  label. `hidden` is the MESH renderer's checkbox and does not touch the
  label; `disabled` turns the whole object off, label included.

From a script:

```js
node.setText(self, "Score: " + score);
```

## Localisation

The Text field holds either the words or a **key**:

```
Score          <- the text, used as written
@hud.score     <- looked up in the project's string table
```

Tables live in `Assets/Strings/<code>.daistr`, one per language:

```
daidalos-strings 1
lang de
name Deutsch

hud.score   Punkte
hud.hint    Leertaste zum Springen\nZweite Zeile
```

Key, whitespace, then **the rest of the line** - so a value may contain
spaces, colons and quotes. `\n` is a line break.

**A missing key shows the key.** Never blank: a half translated build has to
stay usable and has to make its gaps obvious. And text that does not start
with `@` is not a key at all, so a label works before any table exists -
localisation is opt-in per string, which is the only way it gets adopted.

The language is a **project setting** (Settings > Project > Language), so
everyone on a team sees the same labels, and the exported game starts in it.

## UI from code

The Text and Image components are for what is **always there** - a score, a
crosshair, a logo. They are placed with the mouse and they work without a
script running.

For anything **conditional** - a menu, a death screen, a debug readout - there
is `gui`, drawn per frame:

```js
function frame() {
    gui.text(20, 20, "Score: " + score, 28, 0xFFFFFFFF);
    if (dead) {
        var s = gui.size();                       // [width, height] of the view
        gui.rect(0, 0, s[0], s[1], 0xA0000000);   // dim everything
        gui.text(s[0] / 2 - 60, s[1] / 2 - 40, "You died", 40, 0xFFFF5555);
        if (gui.button(s[0] / 2 - 70, s[1] / 2 + 10, 140, 36, "Restart")) restart();
    }
}
```

- `gui.text(x, y, text, size, colour)`
- `gui.rect(x, y, w, h, colour)`
- `gui.image(x, y, w, h, "textures/heart.png", tint)`
- `gui.button(x, y, w, h, label)` - true on the frame it is released over
- `gui.size()` - the view, so you can lay out against the middle or the right

Colours are `0xAARRGGBB`. Coordinates are pixels from the top left of the view.

**Immediate mode**: a frame says what should be on the screen, and nothing has
to be deleted afterwards. A retained tree needs creation, destruction and an
owner for every label - and its first bug is always a menu from the last game
still hanging there after a restart.

## Autocomplete

The script editor suggests after two characters: the engine API, and every
identifier already in the file. **Up/Down** picks, **Tab** or **Enter** takes
it, **Escape** dismisses.

It is a list of names, not a parser. A half parser is wrong on exactly the
lines you are in the middle of writing, and being confidently wrong there is
worse than offering nothing.

## The object model

`self` is an object, spelled the way Unity spells it:

```js
self.transform.position.x += 3 * state.dt;
self.transform.yaw = 90;                 // degrees
self.velocity = [0, 0, -6];
self.text = "Score: " + score;           // the Text component
if (self.grounded) self.impulse(0, 5, 0);

var cam = scene.find("Main Camera");
cam.transform.position = self.transform.position;
```

Setting **one component** writes only that one: `position.x = 10` leaves y and
z alone. That sounds obvious and is the thing a naive wrapper gets wrong.

**Every older script still works.** `self` returns its id wherever a number is
expected, so `body.setVel(self, 4, 0, 0)` and `node.setPos(self, ...)` are
unchanged. That is what `valueOf()` is for, and it is the reason the model
could be added at all rather than replacing what was there.

## Spawning

A behaviour has no `editor` — a game that can add arbitrary nodes and save over
the scene is a save game corrupter waiting for a typo. What it *can* do is
**copy** something an author placed:

```js
var made = scene.spawn(src, parent, "Name");  // the whole subtree, -1 refused
scene.destroy(made);                          // the node and its descendants
scene.childCount(n); scene.childAt(n, i); scene.children(n); scene.parent(n);
made.destroy();                               // the same thing on a Node
```

The copy is deep and it is the same record: the source's components, its
material stack, its collider, its behaviours **and the parameters tuned into
them** come along, because whatever is correct about the source is correct
about the copy. It keeps the source's local transform until you move it, and it
keeps the source's child *names* — rename them yourself if anything looks
things up by name.

Refused, rather than half done: an id that is not a node, a `parent` inside the
source's own subtree (the copy would be its own child), and a subtree over
`DAI_SPAWN_MAX_NODES` — that last one is what stops a spawn loop from eating the
document in a second.

This is what makes INNEN's house able to rebuild a room behind the player's
back while the game runs — see `docs/INNEN_HAUS.md`. The implementation is one
seam, `include/dai_spawn_host.inl`, included by the editor, the modelling host
and the shipped runtime, so all three mean the same thing by it.

## C++ behaviours

Same idea, in a header-only wrapper over the C function table:

```cpp
DAI_BEHAVIOUR_FRAME(api, self, dt) {
    Node me(api, self);
    float speed = me.param("speed", 6.0f);     // the inspector's value
    if (me.key('w')) me.velocity(Vec3(0, 0, -speed));
    if (me.grounded() && me.key(DAI_KEY_SPACE)) me.impulse(Vec3(0, 5, 0));

    Node cam = me.param_node("followCam");     // a @param camera field
    if (cam) cam.position(me.position() + Vec3(0, 3, 8));
}
```

`me.param()`, `me.param_text()` and `me.param_node()` are the inspector's
fields. They used to be drawn, stored and then **never handed to the code** -
which looks exactly like a setting that does nothing.

The wrapper is sugar over `dai_native_api` and nothing else: no base class, no
state, no second path into the engine. The contract stays a C struct so that a
behaviour built with one compiler cannot crash an editor built with another.

**A C++ behaviour needs a C++ compiler on PATH** (g++ or clang++). The Console
says so if there is none. The `.js` behaviours need nothing.

## UI buttons

A Button is not a fourth way to draw a rectangle. It is the **Image** (its
background) and the **Text** (its label) on the same node, plus the one thing
neither has: a reaction. Add *Button (UI)* to a node that has either.

It brightens under the pointer, darkens while held, and fires when the button
is let go **inside** it - let go somewhere else and nothing happens, which is
the only way out of a click you did not mean.

```js
// on the button's own script
function onClick() {
    print("start pressed");
}
```

`On click` in the inspector names a different function if `onClick` is taken.
The call goes to the script **on that node** - a button press is a message to
an object, not an announcement to the whole game.

Buttons only answer the pointer in the **Game** view and only while playing.
The copy drawn over the Scene view is for placing them; one that fired there
would go off every time you dragged it.

## Rich text

The Text component takes HTML-style tags, Unity's spelling:

```
<b>bold</b>  <u>underlined</u>  <s>struck out</s>
<color=#ff3355>red</color>  <color=orange>named</color>
two<br>lines
```

Anything that is not a known tag stays exactly as typed: `<3` is a heart and
`a < b` is a comparison.

There is no `<i>`. Slanting a glyph needs a slanted glyph and the atlas holds
one shape per character - put an italic `.ttf` in the Text component's **Font**
field instead. Bold is drawn out of the one face by striking it twice, a hair
apart; it is not a real bold cut and does not pretend to be one.


## The editor and the exported game run the same script

A behaviour is loaded the same way in `examples/editor_demo.cpp` (Play) and in
`examples/runtime_main.cpp` (the shipped game):

1. the file is evaluated,
2. `include/dai_prelude.h` installs the object model, and `self` becomes a
   `Node` for the object the behaviour is on,
3. the inspector's fields arrive as `params`, **typed** - `float`/`int` as a
   number, `bool` as a boolean, `string` and every node reference as a string,
4. `init()` runs once, `frame()` every frame with `state.dt` set.

That list is written down because for a while it was only true in the editor.
The runtime bound five of the twelve node functions, no play host and no
prelude, so an exported game had no `self`, `input.key()` and `body.setVel()`
did not exist, `node.setNum()` silently did nothing, and every field arrived as
a string - which made `typeof v === "number"` false and every tuned number fall
back to its default. A game that is subtly different from what the editor
showed is worse than one that does not start; `tools/innen_walk.py` walks a
player through three rooms in the real runtime to keep it honest.
