// A Daidalos behaviour, running in the SHIPPED game - there is no play button.
//   init()  once, when the scene is up
//   frame() every rendered frame
// @param pivot

var t = 0;
var me = -1;

function init() {
    me = scene.find("Spinner");
    t = 0;
}

function frame() {
    if (me < 0) return;
    t += 1 / 60;
    var a = t * 1.2;              // radians about Y
    var s = Math.sin(a * 0.5), c = Math.cos(a * 0.5);
    node.setRot(me, 0, s, 0, c);
    var p = node.getPos(me);
    if (p) node.setPos(me, p[0], 2.5 + Math.sin(t * 2.0) * 0.6, p[2]);
}
