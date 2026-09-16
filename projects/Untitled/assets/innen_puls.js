// INNEN - Licht & Puls. GDD §5.4, running in the shipped game.
//
// The house breathes. One behaviour on one node, "Puls", that owns the clock
// and nothing else:
//
//   HELL      the ceiling lights are on, rooms are stable, 4-7 minutes.
//   FLACKER   the last 5 s of the bright phase. The lights stutter. That is
//             the warning, and it is the only one the player gets.
//   DUNKEL    40-90 s. Every ceiling light in the house is out. The torch is
//             all there is - and the house may rebuild rooms the player is
//             LOOKING AT, as long as they are outside the torch's cone.
//
// WHY THE PHASE IS A TAG, NOT A GLOBAL: innen_haus.js has to know which phase
// it is in, and two behaviours in this engine talk the way the house already
// talks to its doors - through `node.tag`, a real field that survives a save
// and is visible in the inspector. This node's tag is one of "hell",
// "flacker" or "dunkel", written the moment it changes. innen_haus.js reads
// it four times a second, the same way innen_door.js reads its own.
//
// WHY THE PULSE DOES NOT REBUILD ANYTHING ITSELF: the rebuild dice, the warm
// rooms and the anchors live in innen_haus.js and there is exactly one of
// them. A second copy of "may this room change" that happened to be driven by
// the light would be a second set of rules to keep in step, and the day they
// disagreed the house would rebuild a room the player was standing in. The
// pulse only says what time of day it is. The house decides what that means.
//
// @header Zyklus
// @tooltip Shortest bright phase, in seconds. The GDD says 4 minutes.
// @param float hellMin     = 240
// @tooltip Longest bright phase, in seconds. The GDD says 7 minutes.
// @param float hellMax     = 420
// @tooltip Shortest dark phase, in seconds.
// @param float dunkelMin   = 40
// @tooltip Longest dark phase, in seconds.
// @param float dunkelMax   = 90
// @tooltip Seconds of flickering before the dark phase. The warning.
// @param float flackerZeit = 5
// @tooltip Seconds before the FIRST dark phase. The first bright phase is
// @tooltip normally shorter than the rest, so the player meets the mechanic
// @tooltip before he has stopped expecting anything. 0 = a full cycle.
// @param float ersteHell   = 0
//
// @header Haus
// @tooltip How many rooms the generator built: Raum00, Raum01...
// @param float rooms       = 14
// @tooltip The node that carries innen_haus.js. Its tag is not touched; this
// @tooltip node's own tag is the channel, and the house reads it by name.
// @param string haus       = Haus
// @tooltip The node the player is on.
// @param string player     = Spieler
// @tooltip The torch. During the dark phase it is the only light left on.
// @param string torch      = Spieler.Lampe
//
// @header Flackern
// @tooltip How many times a second the lights stutter while flickering.
// @param float flackerHz   = 7
// @tooltip What a stuttering light drops to, as a fraction of its own
// @tooltip brightness. 0 is a hard blink, 1 is no flicker at all.
// @param float flackerTief = 0.15
//
// @header Zufall
// @tooltip The dice. The same seed breathes the same way - which is the only
// @tooltip reason tools/innen_puls.py can measure a cycle length at all.
// @param float seed        = 1
//
// @header Test
// @tooltip Run the clock `testCycles` times as fast as the frames allow and
// @tooltip print every phase it went through, instead of waiting out a real
// @tooltip seven minutes. That is how tools/innen_puls.py measures the
// @tooltip distribution in the SHIPPED game.
// @param bool  selfTest    = false
// @tooltip How many full cycles to run in the self test.
// @param float testCycles  = 40

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }
function flag(k, d) { var v = P[k]; return (typeof v === "boolean") ? v : d; }
function text(k, d) { var v = P[k]; return (typeof v === "string" && v.length) ? v : d; }

var HELL_MIN, HELL_MAX, DUNKEL_MIN, DUNKEL_MAX, FLACKER, ERSTE;
var FLACKER_HZ, FLACKER_TIEF, SEED;

var LIGHTS = [];         // every ceiling light in the house, with its own base
var TORCH = -1;
var phase = "hell";      // hell | flacker | dunkel
var left = 0;            // seconds left in this phase
var elapsed = 0;
var cycles = 0;
var lastWritten = null;

// The same LCG the generator and the house use. Not Math.random(): a house
// that breathes differently in two runs of one seed cannot be measured, and
// the whole point of tools/innen_puls.py is that these numbers are measured.
var rngState = 1;
function rnd() {
    rngState = (Math.imul(rngState, 1664525) + 1013904223) >>> 0;
    return rngState / 4294967296;
}

function pad2(n) { return (n < 10 ? "0" : "") + n; }
function say(line) { print("PULS " + line); }

// ---- the lights ---------------------------------------------------------
// Read once, by the generator's own naming - "Raum03.Licht.1". A behaviour
// has no `editor` to enumerate the document with, so the names are the API.
// Each one remembers the brightness it was BUILT with, because that is what
// the bright phase has to give back: a room whose far lamp is nearly out is
// meant to stay nearly out, and a pulse that set every light to the same
// number would quietly flatten the whole floor's lighting design.
function readLights(count) {
    for (var i = 0; i < count; i++) {
        for (var l = 1; l <= 3; l++) {
            var n = scene.find("Raum" + pad2(i) + ".Licht." + l);
            if (n < 0) continue;
            LIGHTS.push({ node: n, base: node.getNum(n, "light.intensity"),
                          room: i });
        }
    }
}

// Everything the house's own lights do, in one place. `tint` multiplies onto
// the brightness each lamp was built with.
function allLights(on, tint) {
    for (var i = 0; i < LIGHTS.length; i++) {
        var L = LIGHTS[i];
        node.setNum(L.node, "light.intensity", on ? L.base * tint : 0);
        node.setNum(L.node, "light.enabled", on ? 1 : 0);
    }
}

// ---- the phases ---------------------------------------------------------
function enterPhase(name) {
    phase = name;
    if (name === "hell") {
        left = HELL_MIN + rnd() * (HELL_MAX - HELL_MIN) - FLACKER;
        // The flicker is the END of the bright phase, not an extra stretch of
        // time: a cycle the GDD calls "4 to 7 minutes" must not quietly
        // become 4 to 7 minutes plus five seconds, measured.
        if (left < 0) left = 0;
        allLights(true, 1);
    } else if (name === "flacker") {
        left = FLACKER;
    } else {
        left = DUNKEL_MIN + rnd() * (DUNKEL_MAX - DUNKEL_MIN);
        allLights(false, 1);
        cycles++;
    }
    writeTag();
    say("phase=" + phase + " for=" + left.toFixed(2) + " t=" + elapsed.toFixed(2));
}

// The channel. Written only when it changes - a tag rewritten every frame is
// a document touched every frame, and innen_haus.js only ever reads it.
function writeTag() {
    if (lastWritten === phase) return;
    node.setStr(self, "node.tag", phase);
    lastWritten = phase;
}

// While flickering, the lights stutter together. Deterministic, off the
// clock: a flicker made of Math.random() differs between two runs of one
// seed, and then no screenshot and no measurement of it means anything.
function flickerNow(t) {
    var s = Math.sin(t * FLACKER_HZ * 6.28318) + Math.sin(t * FLACKER_HZ * 2.7);
    return (s > 0.35) ? FLACKER_TIEF : 1.0;
}

function init() {
    HELL_MIN   = num("hellMin", 240);
    HELL_MAX   = num("hellMax", 420);
    DUNKEL_MIN = num("dunkelMin", 40);
    DUNKEL_MAX = num("dunkelMax", 90);
    FLACKER    = num("flackerZeit", 5);
    ERSTE      = num("ersteHell", 0);
    FLACKER_HZ = num("flackerHz", 7);
    FLACKER_TIEF = num("flackerTief", 0.15);
    SEED       = num("seed", 1);
    rngState   = (SEED >>> 0) || 1;

    readLights(num("rooms", 14));
    var tn = text("torch", "Spieler.Lampe");
    TORCH = tn ? scene.find(tn) : -1;

    say("ready lights=" + LIGHTS.length +
        " hell=" + HELL_MIN + ".." + HELL_MAX +
        " dunkel=" + DUNKEL_MIN + ".." + DUNKEL_MAX +
        " flacker=" + FLACKER);

    enterPhase("hell");
    if (ERSTE > 0) {
        left = ERSTE - FLACKER;
        if (left < 0) left = 0;
        say("first bright phase shortened to " + ERSTE);
    }

    if (flag("selfTest", false)) selfTest(num("testCycles", 40));
}

// ---- the self test ------------------------------------------------------
// It runs the SAME step() the game runs, with a fixed dt, and prints one line
// per phase it completed. A test against a copy of the clock would prove
// nothing about the clock the player meets.
function selfTest(wantCycles) {
    var dt = 1 / 30;
    var guard = 0;
    var hells = [], darks = [], flacks = [];
    var curPhase = phase, spent = 0;
    // The first bright phase may be deliberately short (`ersteHell`), so it is
    // not a sample of the cycle length. It is walked through and dropped.
    var first = true;
    while (cycles < wantCycles && guard < 40000000) {
        guard++;
        var before = phase;
        spent += dt;
        step(dt);
        if (phase !== before) {
            if (before === "hell") {
                // A bright phase as the player experiences it is the stable
                // stretch PLUS the flicker that ends it: that whole span is
                // what the GDD calls 4-7 minutes.
                if (!first) hells.push(spent + FLACKER);
                first = false;
            } else if (before === "dunkel") darks.push(spent);
            else if (before === "flacker") flacks.push(spent);
            spent = 0;
        }
    }
    function stats(a) {
        if (!a.length) return { n: 0, min: 0, max: 0, avg: 0 };
        var mn = a[0], mx = a[0], sum = 0;
        for (var i = 0; i < a.length; i++) {
            if (a[i] < mn) mn = a[i];
            if (a[i] > mx) mx = a[i];
            sum += a[i];
        }
        return { n: a.length, min: mn, max: mx, avg: sum / a.length };
    }
    var H = stats(hells), D = stats(darks), F = stats(flacks);
    say("test cycles=" + cycles +
        " hellN=" + H.n + " hellMin=" + H.min.toFixed(2) +
        " hellMax=" + H.max.toFixed(2) + " hellAvg=" + H.avg.toFixed(2) +
        " dunkelN=" + D.n + " dunkelMin=" + D.min.toFixed(2) +
        " dunkelMax=" + D.max.toFixed(2) + " dunkelAvg=" + D.avg.toFixed(2) +
        " flackerN=" + F.n + " flackerMin=" + F.min.toFixed(2) +
        " flackerMax=" + F.max.toFixed(2));
}

// One tick of the clock, used by frame() and by the self test.
function step(dt) {
    elapsed += dt;
    left -= dt;
    if (phase === "flacker") {
        var f = flickerNow(elapsed);
        for (var i = 0; i < LIGHTS.length; i++) {
            var L = LIGHTS[i];
            node.setNum(L.node, "light.intensity", L.base * f);
            node.setNum(L.node, "light.enabled", f > 0.5 ? 1 : 0);
        }
    }
    if (left > 0) return;
    if (phase === "hell") enterPhase("flacker");
    else if (phase === "flacker") enterPhase("dunkel");
    else enterPhase("hell");
}

function frame() {
    var dt = (typeof state === "object" && state.dt) ? state.dt : 1 / 60;
    if (dt > 0.1) dt = 0.1;              // a hitch must not skip a phase
    step(dt);
}
