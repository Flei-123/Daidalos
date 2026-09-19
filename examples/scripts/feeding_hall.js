/* THE FEEDING HALL - Szene fuer Daidalos, ueber MCP gebaut.
 * Version 2: Kachelmuster kommen aus Texturen (triplanar), keine ARRAY-Modifier,
 * weil deren Instanzen im Blockout-Renderer nicht zuverlaessig auftauchen.
 */
var M = {
  floor: "materials/fh_floor_tiles.daimat",
  wall:  "materials/fh_wall_tiles.daimat",
  yel:   "materials/fh_tile_yellow.daimat",
  pink:  "materials/fh_tile_pink.daimat",
  mag:   "materials/fh_tile_magenta.daimat",
  dark:  "materials/fh_dark.daimat",
  nPink: "materials/fh_neon_pink.daimat",
  nYel:  "materials/fh_neon_yellow.daimat",
  nCyan: "materials/fh_neon_cyan.daimat",
  horse: "materials/fh_horse.daimat",
  bucket:"materials/fh_bucket_tex.daimat",
  red:   "materials/fh_bucket_red.daimat",
  wing:  "materials/fh_wing.daimat",
  metal: "materials/fh_metal.daimat"
};

function clearScene() {
  var guard = 0;
  while (editor.count() > 0 && guard++ < 6000) {
    var id = editor.at(0);
    if (id < 0) break;
    editor.remove(id);
  }
}
function grp(name, parent) {
  var n = parent === undefined ? editor.add(name) : editor.add(name, parent);
  node.setNum(n, "renderer.enabled", 0);   /* leere Knoten sonst als 1-m-Wuerfel sichtbar */
  node.setNum(n, "blockout.kind", 0);
  return n;
}
function box(name, parent, px, py, pz, sx, sy, sz, mat) {
  var n = editor.add(name, parent);
  node.setNum(n, "blockout.kind", 1);
  node.setVec(n, "blockout.size", sx, sy, sz);
  node.setVec(n, "transform.position", px, py, pz);
  if (mat) editor.setMaterial(n, mat);
  return n;
}
function cyl(name, parent, px, py, pz, r, h, mat) {
  var n = editor.add(name, parent);
  node.setNum(n, "blockout.kind", 2);
  node.setVec(n, "blockout.size", r * 2, h, r * 2);
  node.setNum(n, "blockout.segments", 18);
  node.setVec(n, "transform.position", px, py, pz);
  if (mat) editor.setMaterial(n, mat);
  return n;
}
function light(name, parent, px, py, pz, r, g, b, intensity, range) {
  var n = editor.add(name, parent);
  node.setNum(n, "renderer.enabled", 0);
  node.setNum(n, "light.enabled", 1);
  node.setNum(n, "light.mode", 1);
  node.setVec(n, "light.color", r, g, b);
  node.setNum(n, "light.intensity", intensity);
  node.setNum(n, "light.range", range);
  node.setVec(n, "transform.position", px, py, pz);
  return n;
}

var HW = 13, HL = 26, H = 6.5;

clearScene();
editor.begin("THE FEEDING HALL");
var root = grp("FeedingHall");

/* ---- Boden ---- */
var fl = grp("Floor", root);
box("Floor.Slab", fl, 0, -0.2, 0, HW * 2, 0.4, HL * 2, M.floor);
var pud = [[-7, -16, 5, 4], [4, -5, 6, 4.5], [-3, 8, 5, 3.5], [8, 16, 6, 4]];
for (var p = 0; p < pud.length; p++)
  box("Puddle" + p, fl, pud[p][0], 0.04, pud[p][1], pud[p][2], 0.03, pud[p][3], M.nCyan);

/* ---- Waende + Decke ---- */
var wl = grp("Walls", root);
box("Wall.Left",  wl, -HW - 0.4, H / 2, 0, 0.8, H, HL * 2, M.wall);
box("Wall.Right", wl,  HW + 0.4, H / 2, 0, 0.8, H, HL * 2, M.wall);
box("Wall.Back",  wl, 0, H / 2, -HL - 0.4, HW * 2 + 1.6, H, 0.8, M.wall);
box("Wall.Front", wl, 0, H / 2,  HL + 0.4, HW * 2 + 1.6, H, 0.8, M.wall);
/* pinke Sockelleiste und Zierband, damit die Wand Gliederung hat */
for (var sd = 0; sd < 2; sd++) {
  var wx = sd === 0 ? -HW + 0.05 : HW - 0.05;
  box("Wall.Base" + sd, wl, wx, 0.35, 0, 0.35, 0.7, HL * 2, M.pink);
  box("Wall.Band" + sd, wl, wx, 3.1, 0, 0.3, 0.5, HL * 2, M.pink);
}
box("Ceiling", root, 0, H + 0.35, 0, HW * 2 + 2, 0.7, HL * 2 + 2, M.dark);
var bm = grp("Beams", root);
for (var b = 0; b < 11; b++)
  box("Beam" + b, bm, 0, H - 0.2, -HL + 3 + b * 5, HW * 2, 0.35, 0.4, M.metal);

/* ---- Saeulen ---- */
var pil = grp("Pillars", root);
for (var i = 0; i < 7; i++) {
  var pz = -HL + 5 + i * 7.5;
  for (var s2 = 0; s2 < 2; s2++) {
    var px = s2 === 0 ? -7.5 : 7.5;
    cyl("Pillar" + i + "_" + s2, pil, px, H / 2, pz, 1.15, H, M.yel);
    cyl("PillarRing" + i + "_" + s2, pil, px, 1.5, pz, 1.3, 0.7, M.pink);
    cyl("PillarTop" + i + "_" + s2, pil, px, H - 0.5, pz, 1.3, 0.5, M.pink);
  }
}

/* ---- Neon ---- */
var ne = grp("Neon", root);
for (var t = 0; t < 8; t++) {
  var nz = -HL + 4 + t * 6.5;
  box("Neon.L" + t, ne, -HW + 0.55, 3.9, nz, 0.22, 0.22, 4.2, M.nPink);
  box("Neon.R" + t, ne,  HW - 0.55, 3.9, nz, 0.22, 0.22, 4.2, M.nPink);
  box("Neon.C" + t, ne, 0, H - 0.65, nz, 1.0, 0.16, 4.6, M.nYel);
  light("L.Warm" + t, ne, 0, H - 1.3, nz, 1.0, 0.86, 0.45, 9, 20);
  if (t % 2 === 0) light("L.Pink" + t, ne, t % 4 === 0 ? -9 : 9, 3.4, nz, 1.0, 0.25, 0.7, 5, 13);
}
/* Cyan-Akzent tief am Boden, spiegelt sich in den Pfuetzen */
for (var cq = 0; cq < 4; cq++)
  box("Neon.Low" + cq, ne, cq % 2 ? -HW + 0.6 : HW - 0.6, 0.35, -HL + 8 + cq * 12, 0.16, 0.16, 7, M.nCyan);

/* ---- Leuchtschild ---- */
var sg = grp("Sign", root);
box("Sign.Frame", sg, 0, 4.5, -HL + 0.95, 12.4, 3.1, 0.18, M.pink);
box("Sign.Panel", sg, 0, 4.6, -HL + 1.45, 11.6, 3.4, 0.3, M.dark);

/* --- Schriftzug aus echter Geometrie -------------------------------------
 * Texturen scheiden aus: triplanar projiziert in WELTkoordinaten, damit sitzt
 * auf einer Flaeche irgendein Ausschnitt der Kachel, nicht das Wort. Also
 * Buchstaben als Balken in einem 5x7-Raster.                              */
var GLYPH = {
  "F": [[0,0,1,7],[1,6,3,1],[1,3,2,1]],
  "E": [[0,0,1,7],[1,6,3,1],[1,3,2,1],[1,0,3,1]],
  "D": [[0,0,1,7],[1,6,3,1],[1,0,3,1],[4,1,1,5]],
  "I": [[2,0,1,7],[0,6,5,1],[0,0,5,1]],
  "N": [[0,0,1,7],[4,0,1,7],[1,4,1,2],[2,2,1,2],[3,1,1,2]],
  "G": [[1,6,3,1],[0,1,1,5],[1,0,3,1],[4,0,1,3],[3,3,2,1]],
  "H": [[0,0,1,7],[4,0,1,7],[1,3,3,1]],
  "A": [[1,6,3,1],[0,0,1,6],[4,0,1,6],[1,3,3,1]],
  "L": [[0,0,1,7],[1,0,3,1]],
  " ": []
};
function writeWord(word, parent, cx, cy, cz, cell, mat) {
  var w = word.length * 6 * cell - cell;       /* 5 Zellen + 1 Lueckenzelle */
  var x0 = cx - w / 2;
  for (var i = 0; i < word.length; i++) {
    var gl = GLYPH[word.charAt(i)] || [];
    for (var b = 0; b < gl.length; b++) {
      var r = gl[b];
      box("Sign." + word + "." + i + "_" + b, parent,
          x0 + (i * 6 + r[0]) * cell + r[2] * cell / 2,
          cy + r[1] * cell + r[3] * cell / 2,
          cz, r[2] * cell, r[3] * cell, 0.14, mat);
    }
  }
}
writeWord("FEEDING", sg, 0, 4.85, -HL + 1.62, 0.145, "materials/fh_letter_yellow.daimat");
writeWord("HALL",    sg, 0, 3.60, -HL + 1.62, 0.145, "materials/fh_letter_pink.daimat");
light("Sign.Glow", sg, 0, 4.4, -HL + 3.2, 1, 0.9, 0.95, 7, 12);
light("Sign.Glow2", sg, -4.5, 4.4, -HL + 3.2, 1, 0.9, 0.95, 5, 10);
light("Sign.Glow3", sg, 4.5, 4.4, -HL + 3.2, 1, 0.9, 0.95, 5, 10);

/* ---- Pferde ---- */
function horse(name, parent, px, pz, ry, fed) {
  var g = grp(name, parent);
  node.setVec(g, "transform.position", px, 0, pz);
  node.setVec(g, "transform.rotation", 0, ry, 0);
  box(name + ".Body", g, 0, 1.45, 0, 0.95, 0.95, 2.3, M.horse);
  box(name + ".Neck", g, 0, 2.0, 1.1, 0.6, 1.2, 0.6, M.horse);
  box(name + ".Head", g, 0, 2.55, 1.5, 0.55, 0.5, 1.05, M.horse);
  box(name + ".EarL", g, -0.18, 2.9, 1.2, 0.12, 0.3, 0.12, M.horse);
  box(name + ".EarR", g,  0.18, 2.9, 1.2, 0.12, 0.3, 0.12, M.horse);
  box(name + ".Tail", g, 0, 1.6, -1.3, 0.22, 0.8, 0.22, M.horse);
  var legs = [[-0.34, 0.85], [0.34, 0.85], [-0.34, -0.85], [0.34, -0.85]];
  for (var i2 = 0; i2 < 4; i2++)
    box(name + ".Leg" + i2, g, legs[i2][0], 0.5, legs[i2][1], 0.24, 1.0, 0.24, M.horse);
  /* Der Wing-Kranz haengt an JEDEM Pferd, geparkt unter dem Boden -
     das Spielskript hebt ihn hoch, sobald das Pferd gefuettert ist. */
  box(name + ".Crown", g, 0, fed ? 3.1 : -60, 1.35, 0.7, 0.14, 0.7, M.wing);
  if (fed) light(name + ".Glow", g, 0, 2.9, 1.3, 1.0, 0.65, 0.2, 3.5, 6);
  return g;
}
var herd = grp("Herd", root);
/* Der Pool fasst alle drei Level: Level 1 nimmt 12, Level 2 zwanzig,
 * Level 3 achtundzwanzig. Das Spielskript parkt, was es nicht braucht. */
var HORSES = 28;
for (var h2 = 0; h2 < HORSES; h2++) {
  var ang = h2 * 2.399963;                      /* goldener Winkel, keine Reihen */
  var rad = 3 + (h2 % 7) * 1.6;
  horse("Horse" + h2, herd,
        Math.cos(ang) * rad, -18 + h2 * 1.7,
        (h2 * 47) % 360, 0);
}

/* ---- Hindernisse (Level 2) - geparkt, das Spiel hebt sie hoch ---------- */
var obs = grp("Obstacles", root);
for (var ob = 0; ob < 14; ob++) {
  var c1 = box("Crate" + ob, obs, 0, -60, 0, 1.8, 1.8, 1.8, M.metal);
  box("CrateTop" + ob, obs, 0, -60, 0, 1.9, 0.18, 1.9, M.pink);
}

/* ---- Der Pickup (Level 3) - ebenfalls geparkt -------------------------- */
var truck = grp("Truck", root);
node.setVec(truck, "transform.position", 0, -60, 0);
box("Truck.Body",   truck, 0, 0.85, 0, 2.3, 0.75, 5.0, M.red);
box("Truck.Bed",    truck, 0, 1.25, -1.4, 2.1, 0.5, 2.2, M.metal);
box("Truck.Cabin",  truck, 0, 1.6, 0.9, 2.0, 0.85, 1.9, M.red);
box("Truck.Hood",   truck, 0, 1.15, 2.1, 2.1, 0.3, 1.2, M.red);
box("Truck.Dash",   truck, 0, 1.45, 0.05, 1.9, 0.25, 0.6, M.dark);
cyl("Truck.Wheel0", truck, -1.15, 0.45, 1.7, 0.45, 0.35, M.dark);
cyl("Truck.Wheel1", truck,  1.15, 0.45, 1.7, 0.45, 0.35, M.dark);
cyl("Truck.Wheel2", truck, -1.15, 0.45, -1.7, 0.45, 0.35, M.dark);
cyl("Truck.Wheel3", truck,  1.15, 0.45, -1.7, 0.45, 0.35, M.dark);
for (var wr = 0; wr < 4; wr++)
  node.setVec(scene.find("Truck.Wheel" + wr), "transform.rotation", 0, 0, 90);
/* Lenkrad im Sichtfeld: zwei eigenstaendige Knoten (keine Gruppe - eine
 * Hierarchie, die pro Frame verschoben wird, ist teurer als zwei Knoten). */
var wheelRing = cyl("WheelRing", root, 0, -60, 0, 0.25, 0.07, M.dark);
var wheelHub  = cyl("WheelHub",  root, 0, -60, 0, 0.08, 0.1, M.red);

/* ---- Der Bucket in der Hand (Kamera schaut aus 0,1.75,-21 nach vorn) ---- */
var bk = grp("Bucket", root);
node.setVec(bk, "transform.position", -0.38, 1.28, -20.28);
node.setVec(bk, "transform.rotation", 10, 16, -7);
cyl("Bucket.Body", bk, 0, 0, 0, 0.16, 0.24, M.bucket);
cyl("Bucket.Rim",  bk, 0, 0.13, 0, 0.17, 0.04, M.red);
var wp = [[0.05, 0.22, 0.03, 20], [-0.07, 0.24, -0.03, -35], [0.01, 0.26, -0.08, 55],
          [0.09, 0.22, -0.05, -15], [-0.09, 0.21, 0.07, 42]];
for (var w2 = 0; w2 < wp.length; w2++) {
  var wn = box("Bucket.Wing" + w2, bk, wp[w2][0], wp[w2][1], wp[w2][2], 0.10, 0.06, 0.13, M.wing);
  node.setVec(wn, "transform.rotation", wp[w2][3], wp[w2][3] * 0.5, 12);
}
/* Arm/Hand, damit es nach Ego-Perspektive aussieht */
box("Bucket.Arm", bk, 0.02, -0.30, -0.18, 0.17, 0.42, 0.2, M.wing);

/* ---- Fuetterstationen ---- */
var st = grp("Stations", root);
var stp = [[-11, -9], [11, 4], [-11, 19]];
for (var q = 0; q < stp.length; q++) {
  box("Station" + q + ".Crate", st, stp[q][0], 0.45, stp[q][1], 1.5, 0.9, 1.5, M.metal);
  cyl("Station" + q + ".Bucket", st, stp[q][0], 1.25, stp[q][1], 0.45, 0.7, M.bucket);
  light("Station" + q + ".Glow", st, stp[q][0], 1.9, stp[q][1], 1.0, 0.7, 0.25, 4, 8);
}

/* ---- schwache Sonne, damit nichts absaeuft ---- */
var sun = editor.add("Sun", root);
node.setNum(sun, "renderer.enabled", 0);
node.setNum(sun, "light.enabled", 1);
node.setNum(sun, "light.mode", 3);
node.setVec(sun, "light.color", 1, 0.82, 0.92);
node.setNum(sun, "light.intensity", 0.8);
node.setVec(sun, "transform.rotation", 55, 20, 0);

/* ---- Kamera, Wing-Vorrat und der Spiel-Manager ------------------------- */
var camN = editor.add("Main Camera", root);
node.setNum(camN, "renderer.enabled", 0);
node.setNum(camN, "camera.enabled", 1);
node.setNum(camN, "camera.fov", 66);
node.setStr(camN, "node.tag", "MainCamera");
node.setVec(camN, "transform.position", 0, 1.75, -20);

var pool = grp("Wings", root);
for (var wi = 0; wi < 10; wi++)
  box("Wing" + wi, pool, 0, -60, 0, 0.18, 0.11, 0.24, M.wing);

var gl = editor.add("GameLogic", root);
node.setNum(gl, "renderer.enabled", 0);
node.setStr(gl, "script", "fh_game.js");

editor.commit();
editor.camera([0, 1.75, -21], [0, 1.55, 6], 62);
"NODES=" + editor.count();
