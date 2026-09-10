/*
 * The JavaScript object model, in ONE place.
 *
 * This string is evaluated in every behaviour's context before the behaviour
 * itself runs. It is what turns the engine's flat bindings - node.setPos(id,
 * x, y, z) - into the spelling anybody coming from Unity or Godot already
 * knows: self.transform.position.x, self.light.intensity, self.text.value.
 *
 * WHY IT IS A HEADER AND NOT A STRING IN THE EDITOR
 * It used to be a C literal inside examples/editor_demo.cpp, with a SECOND
 * copy in tests/test_objmodel.cpp whose own comment admitted "a copy in a
 * test is a copy that goes stale". One file, included by both, cannot drift.
 *
 * THE TWO RULES THIS FILE LIVES BY
 *   1. Everything here is sugar. Every property below ends in a binding the
 *      engine already had; there is no second way into the engine.
 *   2. Nothing that used to work may stop working. Node.valueOf() returns the
 *      raw id, so body.setVel(self, ...) written a year ago still runs, and
 *      self.position / self.velocity / self.text = "..." all still mean what
 *      they meant before components existed.
 */
#ifndef DAI_PRELUDE_H
#define DAI_PRELUDE_H

static const char *const DAI_JS_PRELUDE = R"JS(
// ---- Daidalos object model (installed before every behaviour) -------------
// self.transform.position.x, self.light.intensity, scene.find("X").transform -
// the Unity spelling. Everything here ends in the same node.*/body.* calls the
// engine has always had; this is sugar, and it is the good kind: the kind that
// costs one indirection and removes an argument you had to remember.
(function () {
    function V3(node, which) { this.__n = node; this.__w = which; }
    // Only position for now: the engine binds getPos/setPos and
    // nothing for scale, and a getter that calls a binding which
    // does not exist throws on the first read.
    function get3(n, w) { return node.getPos(n); }
    function set3(n, w, x, y, z) { node.setPos(n, x, y, z); }
    ["x", "y", "z"].forEach(function (name, i) {
        Object.defineProperty(V3.prototype, name, {
            get: function () { return get3(this.__n, this.__w)[i]; },
            set: function (v) {
                var c = get3(this.__n, this.__w);
                c[i] = v;
                set3(this.__n, this.__w, c[0], c[1], c[2]);
            }
        });
    });
    // A vector prints and compares like the array it is.
    V3.prototype.toString = function () {
        var c = get3(this.__n, this.__w);
        return "(" + c[0].toFixed(3) + ", " + c[1].toFixed(3) + ", " + c[2].toFixed(3) + ")";
    };
    V3.prototype.set = function (x, y, z) { set3(this.__n, this.__w, x, y, z); return this; };
    V3.prototype.add = function (x, y, z) {
        var c = get3(this.__n, this.__w);
        set3(this.__n, this.__w, c[0] + x, c[1] + (y || 0), c[2] + (z || 0));
        return this;
    };

    // ---- the generic property bridge -------------------------------------
    // Everything a component is - a light's range, a camera's field of view,
    // the words in a Text - is a field of the node in the document, and the
    // host reaches all of them by NAME. One binding instead of forty, and a
    // component added to the editor tomorrow needs no new binding at all.
    //
    // A host that predates the bridge (an old editor, a test harness) simply
    // does not have these functions. Reading a property then answers 0 or ""
    // instead of throwing, because a behaviour that dies on line one because
    // the engine is a version behind is worse than one that reads a zero.
    var hasProps = (typeof node.getNum === "function");
    function pget(n, p, d) { return hasProps ? node.getNum(n, p) : d; }
    function pset(n, p, v) { if (hasProps) node.setNum(n, p, +v); }
    function vget(n, p) { return hasProps ? node.getVec(n, p) : [0, 0, 0]; }
    function vset(n, p, v) {
        if (!hasProps) return;
        if (typeof v === "number") { node.setVec(n, p, v, v, v); return; }
        var x = (v[0] !== undefined) ? v[0] : (v.x || 0);
        var y = (v[1] !== undefined) ? v[1] : (v.y || 0);
        var z = (v[2] !== undefined) ? v[2] : (v.z || 0);
        node.setVec(n, p, x, y, z);
    }
    function sget(n, p) { return hasProps ? node.getStr(n, p) : ""; }
    function sset(n, p, v) { if (hasProps) node.setStr(n, p, "" + v); }

    function defNum(cls, name, prop) {
        Object.defineProperty(cls.prototype, name, {
            get: function () { return pget(this.__n, prop, 0); },
            set: function (v) { pset(this.__n, prop, v); }
        });
    }
    function defBool(cls, name, prop) {
        Object.defineProperty(cls.prototype, name, {
            get: function () { return pget(this.__n, prop, 0) !== 0; },
            set: function (v) { pset(this.__n, prop, v ? 1 : 0); }
        });
    }
    function defVec(cls, name, prop) {
        Object.defineProperty(cls.prototype, name, {
            get: function () { return vget(this.__n, prop); },
            set: function (v) { vset(this.__n, prop, v); }
        });
    }
    function defStr(cls, name, prop) {
        Object.defineProperty(cls.prototype, name, {
            get: function () { return sget(this.__n, prop); },
            set: function (v) { sset(this.__n, prop, v); }
        });
    }

    function Transform(n) { this.__n = n; }
    Object.defineProperty(Transform.prototype, "position", {
        get: function () { return new V3(this.__n, 0); },
        set: function (v) {
            if (v instanceof V3) { var c = get3(v.__n, v.__w); node.setPos(this.__n, c[0], c[1], c[2]); }
            else node.setPos(this.__n, v[0] || v.x || 0, v[1] || v.y || 0, v[2] || v.z || 0);
        }
    });
    Object.defineProperty(Transform.prototype, "rotation", {
        get: function () { return node.getRot(this.__n); },
        set: function (q) { node.setRot(this.__n, q[0], q[1], q[2], q[3]); }
    });
    // Yaw in DEGREES, because that is the number anyone actually has in mind.
    Object.defineProperty(Transform.prototype, "yaw", {
        get: function () {
            var q = node.getRot(this.__n);
            return Math.atan2(2 * (q[3] * q[1] + q[0] * q[2]),
                              1 - 2 * (q[1] * q[1] + q[0] * q[0])) * 180 / Math.PI;
        },
        set: function (deg) {
            var h = deg * Math.PI / 360;
            node.setRot(this.__n, 0, Math.sin(h), 0, Math.cos(h));
        }
    });
    defVec(Transform, "scale", "transform.scale");
    Transform.prototype.translate = function (x, y, z) {
        var c = node.getPos(this.__n);
        node.setPos(this.__n, c[0] + x, c[1] + (y || 0), c[2] + (z || 0));
        return this;
    };
    Transform.prototype.toString = function () { return "Transform(" + this.__n + ")"; };

    // ---- Rigidbody -------------------------------------------------------
    // Velocity and impulses come from the PLAY host (the live simulation);
    // mass, friction and the constraints come from the document. Both are the
    // same component to anyone using it, which is the point.
    function Rigidbody(n) { this.__n = n; }
    Object.defineProperty(Rigidbody.prototype, "velocity", {
        get: function () { return body.getVel(this.__n); },
        set: function (v) {
            var x = (v[0] !== undefined) ? v[0] : (v.x || 0);
            var y = (v[1] !== undefined) ? v[1] : (v.y || 0);
            var z = (v[2] !== undefined) ? v[2] : (v.z || 0);
            body.setVel(this.__n, x, y, z);
        }
    });
    Object.defineProperty(Rigidbody.prototype, "grounded", {
        get: function () { return body.grounded(this.__n); }
    });
    Rigidbody.prototype.impulse = function (x, y, z) { body.impulse(this.__n, x, y, z); return this; };
    Rigidbody.prototype.setVelocity = function (x, y, z) { body.setVel(this.__n, x, y, z); return this; };
    defNum(Rigidbody, "density", "rigidbody.density");
    defNum(Rigidbody, "friction", "rigidbody.friction");
    defNum(Rigidbody, "restitution", "rigidbody.restitution");
    defNum(Rigidbody, "motion", "rigidbody.motion");
    defBool(Rigidbody, "trigger", "rigidbody.trigger");
    defBool(Rigidbody, "enabled", "rigidbody.enabled");
    Rigidbody.prototype.toString = function () { return "Rigidbody(" + this.__n + ")"; };

    // ---- Camera ----------------------------------------------------------
    function Camera(n) { this.__n = n; }
    defNum(Camera, "mode", "camera.mode");          // 0 none, 1 perspective, 2 ortho
    defNum(Camera, "fov", "camera.fov");
    defNum(Camera, "size", "camera.size");
    defBool(Camera, "enabled", "camera.enabled");
    Object.defineProperty(Camera.prototype, "orthographic", {
        get: function () { return pget(this.__n, "camera.mode", 0) === 2; },
        set: function (v) { pset(this.__n, "camera.mode", v ? 2 : 1); }
    });
    Camera.prototype.toString = function () { return "Camera(" + this.__n + ")"; };

    // ---- Light -----------------------------------------------------------
    function Light(n) { this.__n = n; }
    defNum(Light, "mode", "light.mode");            // 0 none, 1 point, 2 spot, 3 sun
    defNum(Light, "range", "light.range");
    defNum(Light, "intensity", "light.intensity");
    defNum(Light, "cone", "light.cone");
    defVec(Light, "color", "light.color");
    defBool(Light, "enabled", "light.enabled");
    Light.prototype.toString = function () { return "Light(" + this.__n + ")"; };

    // ---- Text ------------------------------------------------------------
    // `self.text = "score: 3"` still works, so every HUD script written before
    // components existed keeps running. It is the SAME component either way -
    // the setter is a shortcut for self.text.value.
    function Text(n) { this.__n = n; }
    defStr(Text, "value", "text.value");
    defNum(Text, "size", "text.size");
    defNum(Text, "anchor", "text.anchor");
    defVec(Text, "color", "text.color");
    defBool(Text, "enabled", "text.enabled");
    Text.prototype.set = function (t) { node.setText(this.__n, "" + t); return this; };
    Text.prototype.toString = function () { return sget(this.__n, "text.value"); };

    // ---- Image -----------------------------------------------------------
    function Image(n) { this.__n = n; }
    defStr(Image, "asset", "image.asset");
    defVec(Image, "size", "image.size");
    defBool(Image, "enabled", "image.enabled");
    Image.prototype.toString = function () { return "Image(" + this.__n + ")"; };

    function Node(n) { this.__n = n; }
    // THE line that keeps every older script working: where a number is
    // wanted - body.setVel(self, ...) - JavaScript asks for one, and gets it.
    Node.prototype.valueOf = function () { return this.__n; };
    Node.prototype.toString = function () { return "Node(" + this.__n + ")"; };
    Object.defineProperty(Node.prototype, "id", { get: function () { return this.__n; } });
    Object.defineProperty(Node.prototype, "transform", {
        get: function () { return new Transform(this.__n); }
    });
    Object.defineProperty(Node.prototype, "rigidbody", {
        get: function () { return new Rigidbody(this.__n); }
    });
    Object.defineProperty(Node.prototype, "camera", {
        get: function () { return new Camera(this.__n); }
    });
    Object.defineProperty(Node.prototype, "light", {
        get: function () { return new Light(this.__n); }
    });
    Object.defineProperty(Node.prototype, "image", {
        get: function () { return new Image(this.__n); }
    });
    Object.defineProperty(Node.prototype, "position", {
        get: function () { return new V3(this.__n, 0); },
        set: function (v) { this.transform.position = v; }
    });
    Object.defineProperty(Node.prototype, "velocity", {
        get: function () { return body.getVel(this.__n); },
        set: function (v) { body.setVel(this.__n, v[0], v[1], v[2]); }
    });
    Object.defineProperty(Node.prototype, "grounded", {
        get: function () { return body.grounded(this.__n); }
    });
    Object.defineProperty(Node.prototype, "text", {
        get: function () { return new Text(this.__n); },
        set: function (t) { node.setText(this.__n, "" + t); }
    });
    Object.defineProperty(Node.prototype, "name", {
        get: function () { return sget(this.__n, "node.name"); },
        set: function (v) { sset(this.__n, "node.name", v); }
    });
    Node.prototype.impulse = function (x, y, z) { body.impulse(this.__n, x, y, z); return this; };
    Node.prototype.setVelocity = function (x, y, z) { body.setVel(this.__n, x, y, z); return this; };
    Node.prototype.isValid = function () { return this.__n >= 0; };

    globalThis.Node = Node;
    globalThis.Vec3 = V3;
    globalThis.Transform = Transform;
    globalThis.Rigidbody = Rigidbody;
    globalThis.Camera = Camera;
    globalThis.Light = Light;
    globalThis.Text = Text;
    globalThis.Image = Image;
    // scene.find gives back a Node - and a Node is still a number where one
    // is wanted, so scene.find("X") keeps working in old code too.
    var rawFind = scene.find;
    scene.find = function (name) { return new Node(rawFind(name)); };

    // ---- spawn -----------------------------------------------------------
    // scene.spawn(src, parent, name) copies a subtree an author placed and
    // gives back a Node; scene.destroy() takes one away. Wrapped here for the
    // same reason find() is: everything a behaviour holds should be a Node.
    // A host older than the binding simply does not have it - spawning then
    // answers an invalid Node instead of throwing, exactly as reading an
    // unknown property answers 0.
    var rawSpawn = scene.spawn, rawChildAt = scene.childAt, rawParent = scene.parent;
    if (typeof rawSpawn === "function") {
        scene.spawn = function (src, parent, name) {
            return new Node(rawSpawn(+src, parent === undefined ? 0 : +parent,
                                     name === undefined ? null : "" + name));
        };
        scene.childAt = function (n, i) { return new Node(rawChildAt(+n, i)); };
        scene.parent = function (n) { return new Node(rawParent(+n)); };
        // The children as an array, because every caller was about to write
        // this loop and one of them was going to write it wrong.
        scene.children = function (n) {
            var out = [], c = scene.childCount(+n);
            for (var i = 0; i < c; i++) out.push(new Node(rawChildAt(+n, i)));
            return out;
        };
        Node.prototype.spawn = function (parent, name) {
            return scene.spawn(this.__n, parent === undefined ? 0 : parent, name);
        };
        Node.prototype.destroy = function () { return scene.destroy(this.__n); };
        Node.prototype.children = function () { return scene.children(this.__n); };
    }
    globalThis.__wrapSelf = function (id) { return new Node(id); };
})();
)JS";

#endif /* DAI_PRELUDE_H */
