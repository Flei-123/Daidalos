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
