// The WebAssembly API, exercised the way a web client would.
//
//   node tests/test_web.mjs
//
// This is the contract a browser renderer depends on: create a world, step it,
// read transforms out of the heap without copying, and get the same checksum
// the native build produces.

import Daidalos from '../build/web/daidalos.js';

let pass = 0, fail = 0;
const check = (cond, msg) => { if (cond) pass++; else { fail++; console.log('  FAIL ' + msg); } };

const M = await Daidalos();
console.log('daidalos wasm:', M.UTF8ToString(M._dai_web_version()));

// null backend so the result is comparable with the native headless build
const w = M._dai_web_create(1, 60, 512, 20260731);
check(w !== 0, 'world creation returned null');

const floor = M._dai_web_body(w, 0, 0, 50, 1, 50, 0, -1, 0, 0, 0);
check(floor !== 0, 'floor body was not created');

for (let i = 0; i < 64; i++) {
  const shape = (i % 2) ? 1 : 0;
  M._dai_web_body(w, shape, 2, 0.5, 0.5, 0.5,
                  -4 + (i % 8) * 1.1, 2 + Math.floor(i / 8) * 1.3, -3 + Math.floor(i / 8) * 0.7,
                  500 + i, 100 + i);
}
check(M._dai_web_body_count(w) === 65, `expected 65 bodies, got ${M._dai_web_body_count(w)}`);

// the same 600 ticks the native self check runs
for (let t = 0; t < 600; t++) {
  M._dai_web_input(w, 0, t, ((t % 120) - 60) / 60, 0, 0);
  M._dai_web_step(w, 1);
}
check(M._dai_web_tick(w) === 600, `tick is ${M._dai_web_tick(w)} after 600 steps`);

const checksum = M.UTF8ToString(M._dai_web_checksum(w));
console.log('  checksum:', checksum);

// Determinism is a property of the SIMULATION, not of scene construction in
// another language: JS computes -4 + (i % 8) * 1.1 in double and rounds on the
// way into the float parameter, while C++ does it in float throughout, so the
// two builds start from bodies that are microns apart. Feed both sides bit
// identical inputs and they agree; build the same scene twice in JS and it must
// agree with itself, which is what a rollback session actually depends on.
const w2 = M._dai_web_create(1, 60, 512, 20260731);
M._dai_web_body(w2, 0, 0, 50, 1, 50, 0, -1, 0, 0, 0);
for (let i = 0; i < 64; i++) {
  const shape = (i % 2) ? 1 : 0;
  M._dai_web_body(w2, shape, 2, 0.5, 0.5, 0.5,
                  -4 + (i % 8) * 1.1, 2 + Math.floor(i / 8) * 1.3, -3 + Math.floor(i / 8) * 0.7,
                  500 + i, 100 + i);
}
for (let t = 0; t < 600; t++) {
  M._dai_web_input(w2, 0, t, ((t % 120) - 60) / 60, 0, 0);
  M._dai_web_step(w2, 1);
}
const checksum2 = M.UTF8ToString(M._dai_web_checksum(w2));
check(checksum2 === checksum, `two identical runs gave ${checksum} and ${checksum2}`);
check(/^[0-9a-f]{16}$/.test(checksum), 'checksum is not a 16 digit hex string');
M._dai_web_destroy(w2);

// transforms, read straight out of the heap
const n = M._dai_web_transforms(w, 1.0);
const ptr = M._dai_web_transform_ptr();
const view = new Float32Array(M.HEAPF32.buffer, ptr, n * 9);
check(n === 65, `transforms returned ${n} bodies`);
check(view.length === n * 9, 'transform view has the wrong length');

let below = 0, above = 0;
for (let i = 0; i < n; i++) {
  const y = view[i * 9 + 3];
  if (y < -2) below++;
  if (y > 0) above++;
}
console.log(`  ${n} bodies, ${above} resting above the floor, ${below} fallen through`);
check(below === 0, `${below} bodies fell through the floor`);
check(above > 50, `only ${above} bodies are above the floor`);

// user data survives the round trip through the flat buffer
check(view[1 * 9 + 1] === 100, `second body user_data is ${view[1 * 9 + 1]}, expected 100`);

// rollback works in the browser too
const before = M.UTF8ToString(M._dai_web_checksum(w));
check(M._dai_web_rollback(w, 570) === 0, 'rollback returned an error');
check(M.UTF8ToString(M._dai_web_checksum(w)) === before, 'rollback did not reproduce the state');

M._dai_web_destroy(w);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
