import io

# ===========================================================================
# self.transform.position.x - Unitys Schreibweise, in JavaScript.
#
# Der Vorspann laeuft vor jeder Datei. Er baut KEINE neue Welt: jede Zeile
# darin ruft am Ende dieselben node.*-Bindungen auf wie vorher. Was er tut,
# ist die Punkt-Schreibweise moeglich machen, mit Gettern und Settern.
#
# Der Trick, der alles Alte am Leben laesst: valueOf(). `self` ist jetzt ein
# Objekt, aber body.setVel(self, ...) erwartet eine Zahl - und QuickJS ruft
# valueOf(), wenn es eine Zahl braucht. Also funktioniert beides, und kein
# vorhandenes Script bricht.
# ===========================================================================
PRELUDE = r'''
// ---- Daidalos object model (installed before every behaviour) -------------
// self.transform.position.x, self.name, scene.find("X").transform - the
// Unity spelling. Everything here ends in the same node.*/body.* calls the
// engine has always had; this is sugar, and it is the good kind: the kind
// that costs one indirection and removes an argument you had to remember.
(function () {
    function V3(node, which) { this.__n = node; this.__w = which; }
    function get3(n, w) {
        return w === 0 ? node.getPos(n) : (w === 1 ? node.getScale(n) : node.getPos(n));
    }
    function set3(n, w, x, y, z) {
        if (w === 0) node.setPos(n, x, y, z);
        else if (w === 1 && node.setScale) node.setScale(n, x, y, z);
    }
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

    function Node(n) { this.__n = n; }
    // THE line that keeps every older script working: where a number is
    // wanted - body.setVel(self, ...) - JavaScript asks for one, and gets it.
    Node.prototype.valueOf = function () { return this.__n; };
    Node.prototype.toString = function () { return "Node(" + this.__n + ")"; };
    Object.defineProperty(Node.prototype, "id", { get: function () { return this.__n; } });
    Object.defineProperty(Node.prototype, "transform", {
        get: function () { return new Transform(this.__n); }
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
        set: function (t) { node.setText(this.__n, "" + t); }
    });
    Node.prototype.impulse = function (x, y, z) { body.impulse(this.__n, x, y, z); return this; };
    Node.prototype.setVelocity = function (x, y, z) { body.setVel(this.__n, x, y, z); return this; };
    Node.prototype.isValid = function () { return this.__n >= 0; };

    globalThis.Node = Node;
    globalThis.Vec3 = V3;
    // scene.find gives back a Node - and a Node is still a number where one
    // is wanted, so scene.find("X") keeps working in old code too.
    var rawFind = scene.find;
    scene.find = function (name) { return new Node(rawFind(name)); };
    globalThis.__wrapSelf = function (id) { return new Node(id); };
})();
'''

p = 'examples/editor_demo.cpp'
s = io.open(p, encoding='utf-8').read()

def cstr(text):
    out = []
    for line in text.split('\n'):
        e = line.replace('\\', '\\\\').replace('"', '\\"')
        out.append('                "%s\\n"' % e)
    return '\n'.join(out)

old = """            {
                char selfjs[64];
                std::snprintf(selfjs, sizeof(selfjs), "var self = %u;", (unsigned)id);
                dai_script_eval(s, selfjs, "self", err, sizeof(err));
            }"""
new = """            // The object model, then `self` as one of its Nodes. In this
            // order: the wrapper has to exist before anything is wrapped.
            {
                static const char *PRELUDE =
%s;
                if (dai_script_eval(s, PRELUDE, "prelude", err, sizeof(err)) != DAI_OK && err[0])
                    std::printf("prelude: %%s\\n", err);
                char selfjs[96];
                std::snprintf(selfjs, sizeof(selfjs),
                              "var self = __wrapSelf(%%u);", (unsigned)id);
                dai_script_eval(s, selfjs, "self", err, sizeof(err));
            }""" % cstr(PRELUDE)
assert s.count(old) == 1, 'self eval not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('JS object model installed')
