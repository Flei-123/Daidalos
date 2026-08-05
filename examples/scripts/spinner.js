// Spinner - the shortest possible behaviour, written the Unity way.
//
// There is not one "@param" line in this file. Every top level declaration
// below IS a serialized field: the inspector draws it, stores what you set on
// the NODE, and writes it back INTO this variable before init() runs. So the
// number you type in the inspector is the number `speed` has at Play, and the
// value written here is only the default.
//
// The comment directly above a field is its description - it shows up when
// the pointer rests on that row.

// @header Rotation

// Degrees per second around the axis below.
let speed = 90;
// 0 = X, 1 = Y, 2 = Z.
let axis = 1;
// Turn the other way.
let reverse = false;

// @header Debug

// Printed once when the object wakes up.
let label = "spinner";

let _t = 0;

function init() {
    print(label + ": " + speed + " deg/s on axis " + axis);
}

function frame() {
    _t += state.dt * speed * (reverse ? -1 : 1) * Math.PI / 180;
    const h = _t * 0.5;
    const s = Math.sin(h), c = Math.cos(h);
    if (axis === 0)      node.setRot(self, s, 0, 0, c);
    else if (axis === 2) node.setRot(self, 0, 0, s, c);
    else                 node.setRot(self, 0, s, 0, c);
}
