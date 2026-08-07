// LampFlicker.cpp - the component classes, in the smallest useful example.
//
// Drop this on a node that has a Light. It flickers the lamp, drifts its
// colour towards warm, and writes the current brightness into a Text node so
// there is something to look at that is not the lamp.
//
// WHAT THIS FILE IS REALLY FOR
// Every property below - light().intensity, text().value, transform().scale -
// is the SAME name the JavaScript side uses (self.light.intensity). One
// bridge, two languages, one spelling. If this file stops compiling, the C++
// half of the object model is broken, which is why the build compiles it.
//
// @header Flicker
// @tooltip Times per second.
// @param float rate      = 7
// @tooltip How far the brightness swings, 0 to 1.
// @param float depth     = 0.35
// @header Readout
// @tooltip Optional: a node with a Text component to print the value into.
// @param node readout

#include "dai_native.h"

static float g_t = 0.0f;

DAI_BEHAVIOUR_INIT(api, self) {
    Node me(api, self);
    // Turning a light ON does not decide WHICH light it is: a node that was
    // already a sun stays a sun. That rule lives in the editor, not here.
    me.light().enabled = true;
    if ((float)me.light().range <= 0.0f) me.light().range = 10.0f;
    me.log("LampFlicker ready");
}

DAI_BEHAVIOUR_FRAME(api, self, dt) {
    Node me(api, self);
    g_t += dt;

    float rate  = me.param("rate", 7.0f);
    float depth = me.param("depth", 0.35f);

    // One read, one write, straight into the document the inspector shows.
    float base = 1.0f;
    float wob  = __builtin_sinf(g_t * rate) * 0.5f + 0.5f;
    me.light().intensity = base * (1.0f - depth + depth * wob);

    // A colour is a Vec3 like any other, and it reads back through the same
    // property it was written to.
    Vec3 warm(1.0f, 0.85f, 0.65f);
    Vec3 c = me.light().color;
    me.light().color = c + (warm - c) * (1.0f - __builtin_powf(0.001f, dt));

    // The lamp breathes a little, so the transform component gets exercised
    // by something other than position.
    float s = 1.0f + 0.02f * wob;
    me.transform().scale = Vec3(s, s, s);

    // And the readout, if one was dragged into the field.
    Node out = me.param_node("readout");
    if (out) {
        char line[64];
        int pct = (int)((float)me.light().intensity * 100.0f + 0.5f);
        line[0] = 0;
        // No <cstdio> here on purpose: a behaviour compiles against
        // dai_native.h alone, and this is three digits and a sign.
        int n = 0;
        line[n++] = 'l'; line[n++] = 'a'; line[n++] = 'm'; line[n++] = 'p';
        line[n++] = ' ';
        if (pct >= 100) { line[n++] = (char)('0' + pct / 100); pct %= 100; line[n++] = (char)('0' + pct / 10); }
        else if (pct >= 10) line[n++] = (char)('0' + pct / 10);
        line[n++] = (char)('0' + pct % 10);
        line[n++] = '%'; line[n] = 0;
        out.text().value = line;
    }
}
