// A UI written in JavaScript, hot reloaded while the game runs.
//
// Type checking comes free: build/web/daidalos.d.ts describes the API, so
// writing this in TypeScript and running `tsc` gives completions and errors
// before the file is ever loaded.

function draw() {
    ui.panel(24, 24, 300, 260, "Daidalos");

    ui.label("fps: " + state.fps.toFixed(1));
    ui.label("bodies: " + state.bodies);
    ui.label("tick: " + state.tick);
    ui.separator();

    // The slider returns its new value - scripts have no pointers, so state
    // lives on the host side and comes back through the return value.
    state.volume = ui.slider("Volume", state.volume, 0, 1);
    ui.progress(state.loading, "loading " + Math.round(state.loading * 100) + "%");

    if (ui.button("Reset")) {
        state.resetRequested = 1;
    }

    ui.panelEnd();
}
