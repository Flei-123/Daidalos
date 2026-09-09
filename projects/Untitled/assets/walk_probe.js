// The probe that turns a headless run into a measurement.
//
// Put it next to a behaviour you want to watch (the script field stacks with
// ';'), and every `every` seconds it prints one line:
//
//     PROBE t=3.00 p=0.000,0.902,-1.874 v=0.00,-0.01,-2.20 g=1
//
// tools/innen_walk.py reads those lines. It exists because a headless runtime
// has no window to look at and no debugger attached: without a line of text,
// "the player walked down the hallway" is an opinion. It prints the position,
// the velocity and whether the feet are on something - the three numbers that
// tell a walk apart from a fall, a slide and standing in a wall.
//
// @tooltip Seconds between two lines. Small values make long logs.
// @param float every = 0.5
// @tooltip Also print the very first frame, before anything has moved.
// @param bool  atStart = true

var P = (typeof params === "object" && params) ? params : {};
var EVERY = (typeof P.every === "number" && P.every > 0) ? P.every : 0.5;
var AT_START = (typeof P.atStart === "boolean") ? P.atStart : true;

var t = 0, next = 0, printed = 0;

function f2(x) { return (Math.round(x * 1000) / 1000).toFixed(3); }

function init() {
    next = AT_START ? 0 : EVERY;
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    t += dt;
    if (t < next) return;
    next = t + EVERY;
    var p = node.getPos(self);
    var v = body.getVel(self);
    print("PROBE t=" + f2(t) +
          " p=" + f2(p[0]) + "," + f2(p[1]) + "," + f2(p[2]) +
          " v=" + f2(v[0]) + "," + f2(v[1]) + "," + f2(v[2]) +
          " g=" + (body.grounded(self) ? 1 : 0));
    printed += 1;
}
