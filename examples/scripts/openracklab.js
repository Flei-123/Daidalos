/* OPENRACKLAB - Diplomarbeits-Systemmodell fuer Justin.
 * 19-Zoll-Rack, 48-V-Busbar vertikal hinten, Chassis zu je 4 HE,
 * Blades hochkant in den Breiten 1S / 2S / 4S, kabellos gesteckt.
 * Massstab: 1 Einheit = 1 Meter (echte Rackmasse).
 * Bauen:  js files:["examples/scripts/openracklab.js"]
 */

var M = {
  rack:   "materials/orl_rack.daimat",
  frame:  "materials/orl_frame.daimat",
  chas:   "materials/orl_chassis.daimat",
  floor:  "materials/orl_floor.daimat",
  busP:   "materials/orl_bus_pos.daimat",
  busN:   "materials/orl_bus_neg.daimat",
  cu:     "materials/orl_copper.daimat",
  au:     "materials/orl_gold.daimat",
  pcb:    "materials/orl_pcb.daimat",
  pcbD:   "materials/orl_pcb_dark.daimat",
  b1:     "materials/orl_blade1s.daimat",
  b2:     "materials/orl_blade2s.daimat",
  b4:     "materials/orl_blade4s.daimat",
  psu:    "materials/orl_psu.daimat",
  psuM:   "materials/orl_psu_mod.daimat",
  sw:     "materials/orl_switch.daimat",
  gpu:    "materials/orl_gpu.daimat",
  soc:    "materials/orl_soc.daimat",
  ssd:    "materials/orl_ssd.daimat",
  dcdc:   "materials/orl_dcdc.daimat",
  hs:     "materials/orl_heatsink.daimat",
  lG:     "materials/orl_led_grn.daimat",
  lA:     "materials/orl_led_amb.daimat",
  lR:     "materials/orl_led_red.daimat",
  fan:    "materials/orl_fan.daimat",
  cab:    "materials/orl_cable.daimat"
};

/* ---------- Helfer ---------- */
function clearScene() {
  var g = 0;
  while (editor.count() > 0 && g++ < 9000) {
    var id = editor.at(0);
    if (id < 0) break;
    editor.remove(id);
  }
}
function deco(n) {
  node.setNum(n, "collider.enabled", 0);
  node.setNum(n, "rigidbody.enabled", 0);
  return n;
}
function grp(name, parent) {
  var n = parent === undefined ? editor.add(name) : editor.add(name, parent);
  node.setNum(n, "renderer.enabled", 0);
  node.setNum(n, "blockout.kind", 0);
  return deco(n);
}
/* px,py,pz = MITTELPUNKT ; sx,sy,sz = Groesse */
function box(name, parent, px, py, pz, sx, sy, sz, mat) {
  var n = editor.add(name, parent);
  node.setNum(n, "blockout.kind", 1);
  node.setVec(n, "blockout.size", sx, sy, sz);
  node.setVec(n, "transform.position", px, py, pz);
  if (mat) editor.setMaterial(n, mat);
  return deco(n);
}
function cyl(name, parent, px, py, pz, r, h, mat) {
  var n = editor.add(name, parent);
  node.setNum(n, "blockout.kind", 2);
  node.setVec(n, "blockout.size", r * 2, h, r * 2);
  node.setNum(n, "blockout.segments", 16);
  node.setVec(n, "transform.position", px, py, pz);
  if (mat) editor.setMaterial(n, mat);
  return deco(n);
}
function light(name, parent, px, py, pz, r, g, b, inten, range) {
  var n = editor.add(name, parent);
  node.setNum(n, "renderer.enabled", 0);
  node.setNum(n, "light.enabled", 1);
  node.setNum(n, "light.mode", 1);
  node.setVec(n, "light.color", r, g, b);
  node.setNum(n, "light.intensity", inten);
  node.setNum(n, "light.range", range);
  node.setVec(n, "transform.position", px, py, pz);
  return deco(n);
}
function sun(name, parent, rx, ry, inten) {
  var n = editor.add(name, parent);
  node.setNum(n, "renderer.enabled", 0);
  node.setNum(n, "light.enabled", 1);
  node.setNum(n, "light.mode", 3);
  node.setVec(n, "light.color", 1.0, 0.98, 0.94);
  node.setNum(n, "light.intensity", inten);
  node.setVec(n, "transform.rotation", rx, ry, 0);
  return deco(n);
}

/* ---------- Masse (Meter) ---------- */
var U      = 0.04445;      /* 1 HE */
var RW     = 0.600;        /* Rackbreite aussen */
var RD     = 1.000;        /* Racktiefe */
var IW     = 0.448;        /* nutzbare Innenbreite */
var CH_H   = 4 * U;        /* Chassis 4 HE = 0.1778 */
var BL_D   = 0.330;        /* Blade-Tiefe */
var BL_Z0  = 0.055;        /* Blade-Vorderkante */
var BP_Z   = 0.412;        /* Backplane-Mitte (hinter den Blades) */
var BP_T   = 0.016;
var BUS_Z  = 0.470;        /* Busbar-Ebene */
var PITCH  = 0.043;        /* 1S-Rastermass */
var W1S    = 0.038;
var X0     = -IW / 2 + 0.006;

clearScene();
editor.begin("OPENRACKLAB");

var root = grp("OpenRackLab");

/* ================= Boden + Umgebung ================= */
box("Floor", root, 0, -0.012, RD / 2 - 0.1, 4.0, 0.024, 4.0, M.floor);

/* ================= Rackrahmen ================= */
var rk = grp("Rack", root);
var RH = 0.95;                              /* gezeigter Rackausschnitt */
var hx = RW / 2 - 0.013;
for (var c1 = 0; c1 < 2; c1++) {
  for (var c2 = 0; c2 < 2; c2++) {
    var px = (c1 === 0 ? -hx : hx);
    var pz = (c2 === 0 ? 0.013 : RD - 0.013);
    box("Post_" + c1 + c2, rk, px, RH / 2, pz, 0.026, RH, 0.026, M.frame);
  }
}
/* 19-Zoll-Montageschienen mit Lochreihe */
for (var s1 = 0; s1 < 2; s1++) {
  var sx = (s1 === 0 ? -1 : 1) * (IW / 2 + 0.012);
  box("Rail" + s1, rk, sx, RH / 2, 0.042, 0.020, RH, 0.030, M.rack);
  for (var hcount = 0; hcount < 54; hcount++)
    box("RailHole" + s1 + "_" + hcount, rk, sx, 0.035 + hcount * U / 3, 0.030,
        0.022, 0.006, 0.006, M.floor);
}
/* Seitenwaende und Dach nur angedeutet */
box("Rack.Top", rk, 0, RH + 0.014, RD / 2, RW, 0.028, RD, M.rack);
box("Rack.Back", rk, 0, RH / 2, RD - 0.006, RW, RH, 0.012, M.rack);

/* ================= 48-V-Busbar vertikal ================= */
var bus = grp("Busbar48V", root);
box("Bus.Plus",  bus, -0.075, 0.48, BUS_Z, 0.040, 0.86, 0.008, M.busP);
box("Bus.Minus", bus,  0.075, 0.48, BUS_Z, 0.040, 0.86, 0.008, M.busN);
/* Isolator-Halter */
for (var iso = 0; iso < 5; iso++)
  box("Bus.Iso" + iso, bus, 0, 0.09 + iso * 0.20, BUS_Z + 0.014, 0.24, 0.022, 0.020, M.rack);

/* ================= Power Shelf (3 HE, unten) ================= */
var psZ0 = 0.020, psH = 3 * U;
var ps = grp("PowerShelf", root);
box("PS.Case", ps, 0, psZ0 + psH / 2, RD / 2 - 0.09, IW, psH, 0.62, M.psu);
for (var r6 = 0; r6 < 6; r6++) {
  var rx = X0 + 0.010 + r6 * 0.071;
  box("PS.Rect" + r6, ps, rx + 0.030, psZ0 + psH / 2, 0.128, 0.062, psH - 0.020, 0.26, M.psuM);
  cyl("PS.Fan" + r6, ps, rx + 0.030, psZ0 + psH / 2, 0.006, 0.024, 0.008, M.fan);
  box("PS.Led" + r6, ps, rx + 0.030, psZ0 + psH - 0.016, 0.004, 0.008, 0.004, 0.004, M.lG);
}
/* Anbindung Shelf -> Busbar */
box("PS.BusClamp", ps, 0, psZ0 + psH / 2, BUS_Z - 0.020, 0.22, 0.050, 0.046, M.cu);
/* der einzige Netzanschluss im ganzen Rack */
box("PS.AcInlet", ps, IW / 2 - 0.05, psZ0 + 0.030, RD - 0.030, 0.060, 0.040, 0.040, M.cab);
box("PS.AcCable", ps, IW / 2 - 0.05, psZ0 + 0.030, RD + 0.14, 0.022, 0.022, 0.30, M.cab);

/* ================= Backplane + Chassis ================= */
function backplane(parent, y0) {
  var bp = grp("Backplane", parent);
  box("BP.Board", bp, 0, y0 + CH_H / 2, BP_Z, IW, CH_H - 0.014, BP_T, M.pcb);
  /* Busbar-Klemme der Backplane */
  box("BP.ClampP", bp, -0.075, y0 + CH_H / 2, BUS_Z - 0.016, 0.046, 0.055, 0.030, M.cu);
  box("BP.ClampN", bp,  0.075, y0 + CH_H / 2, BUS_Z - 0.016, 0.046, 0.055, 0.030, M.cu);
  /* je Slot: Hot-Swap-Stufe (FET + Shunt + Controller) auf der Backplane */
  for (var s = 0; s < 10; s++) {
    var sx = X0 + W1S / 2 + s * PITCH;
    box("BP.Fet" + s,   bp, sx, y0 + 0.030, BP_Z - 0.012, 0.014, 0.020, 0.008, M.pcbD);
    box("BP.Shunt" + s, bp, sx, y0 + 0.058, BP_Z - 0.012, 0.012, 0.008, 0.006, M.au);
    box("BP.Ctrl" + s,  bp, sx, y0 + 0.082, BP_Z - 0.012, 0.012, 0.012, 0.006, M.pcbD);
  }
  return bp;
}

/* Kontaktsatz am Blade: DREI STUFEN + Present-Pin.
   Laenge nach hinten gestaffelt -> Masse kontaktiert zuerst, Present zuletzt. */
function contacts(parent, cx, y0, w) {
  var ct = grp("Contacts", parent);
  var zb = BL_Z0 + BL_D;           /* Blade-Hinterkante */
  /* 1. Masse - am laengsten */
  box("C1.GND",       ct, cx - w * 0.30, y0 + 0.030, zb + 0.020, 0.012, 0.026, 0.040, M.cu);
  /* 2. Vorladepfad - mittel */
  box("C2.Precharge", ct, cx - w * 0.10, y0 + 0.030, zb + 0.014, 0.008, 0.018, 0.028, M.au);
  /* 3. Hauptleistung 48 V - kurz und breit */
  box("C3.Power48",   ct, cx + w * 0.12, y0 + 0.030, zb + 0.010, 0.016, 0.028, 0.020, M.cu);
  /* 4. Present / I2C - am kuerzesten, meldet das Ziehen zuerst */
  box("C4.Present",   ct, cx + w * 0.32, y0 + 0.030, zb + 0.006, 0.006, 0.012, 0.012, M.au);
  return ct;
}

function bladeShell(parent, name, cx, y0, w, mat, pull) {
  var z0 = BL_Z0 - pull;
  var b = grp(name, parent);
  box(name + ".Tray", b, cx, y0 + 0.012, z0 + BL_D / 2, w, 0.006, BL_D, M.rack);
  box(name + ".PCB",  b, cx, y0 + 0.020, z0 + BL_D / 2, w - 0.006, 0.004, BL_D - 0.020, M.pcb);
  /* Frontblende mit Griff und Status-LEDs */
  box(name + ".Front", b, cx, y0 + CH_H / 2 - 0.006, z0 - 0.006, w, CH_H - 0.016, 0.008, mat);
  box(name + ".Grip",  b, cx, y0 + 0.030, z0 - 0.016, w * 0.55, 0.012, 0.014, M.frame);
  box(name + ".LedPwr", b, cx - w * 0.22, y0 + CH_H - 0.030, z0 - 0.012, 0.006, 0.006, 0.004, M.lG);
  box(name + ".LedAct", b, cx + w * 0.10, y0 + CH_H - 0.030, z0 - 0.012, 0.006, 0.006, 0.004, M.lA);
  /* DC/DC-Wandler 48 V -> 12 V, hinten am Blade */
  box(name + ".DCDC", b, cx, y0 + 0.034, z0 + BL_D - 0.055, w - 0.012, 0.024, 0.060, M.dcdc);
  box(name + ".DCDCfin", b, cx, y0 + 0.050, z0 + BL_D - 0.055, w - 0.016, 0.010, 0.056, M.hs);
  contacts(b, cx, y0, w);
  return b;
}

/* --- Blade-Typen --- */
function bladeSoC(parent, cx, y0, pull) {
  var b = bladeShell(parent, "Blade1S_SoC", cx, y0, W1S, M.b1, pull);
  var z0 = BL_Z0 - pull;
  box("SoC.Module", b, cx, y0 + 0.032, z0 + 0.130, W1S - 0.010, 0.020, 0.090, M.soc);
  box("SoC.Sink",   b, cx, y0 + 0.052, z0 + 0.130, W1S - 0.014, 0.022, 0.080, M.hs);
  box("SoC.Ram",    b, cx, y0 + 0.028, z0 + 0.210, W1S - 0.014, 0.010, 0.030, M.pcbD);
  box("SoC.Nvme",   b, cx, y0 + 0.026, z0 + 0.055, W1S - 0.016, 0.005, 0.070, M.ssd);
  return b;
}
function bladeStorage(parent, cx, y0) {
  var b = bladeShell(parent, "Blade1S_Storage", cx, y0, W1S, M.b1, 0);
  var z0 = BL_Z0;
  for (var d = 0; d < 6; d++)
    box("SSD" + d, b, cx, y0 + 0.028, z0 + 0.040 + d * 0.040, W1S - 0.012, 0.010, 0.034, M.ssd);
  box("Sto.Ctrl", b, cx, y0 + 0.030, z0 + 0.290, W1S - 0.014, 0.014, 0.028, M.pcbD);
  return b;
}
function bladeMiniITX(parent, cx, y0, pull) {
  var w = 2 * PITCH - 0.005;
  var b = bladeShell(parent, "Blade2S_MiniITX", cx, y0, w, M.b2, pull);
  var z0 = BL_Z0 - pull;
  box("ITX.Board", b, cx, y0 + 0.022, z0 + 0.150, w - 0.012, 0.004, 0.170, M.pcbD);
  cyl("ITX.Cooler", b, cx - 0.012, y0 + 0.062, z0 + 0.120, 0.038, 0.070, M.hs);
  box("ITX.Ram0", b, cx + 0.030, y0 + 0.050, z0 + 0.120, 0.008, 0.052, 0.028, M.pcbD);
  box("ITX.Ram1", b, cx + 0.042, y0 + 0.050, z0 + 0.120, 0.008, 0.052, 0.028, M.pcbD);
  /* ATX12VO: ein einziger 12-V-Stecker statt 24-Pin-Strang */
  box("ITX.P12VO", b, cx - 0.030, y0 + 0.030, z0 + 0.250, 0.030, 0.014, 0.012, M.au);
  box("ITX.Nvme", b, cx + 0.020, y0 + 0.026, z0 + 0.215, 0.022, 0.005, 0.070, M.ssd);
  return b;
}
function bladeGPU(parent, cx, y0) {
  var w = 4 * PITCH - 0.009;
  var b = bladeShell(parent, "Blade4S_GPU", cx, y0, w, M.b4, 0);
  var z0 = BL_Z0;
  box("GPU.Board", b, cx, y0 + 0.022, z0 + 0.160, w - 0.014, 0.005, 0.230, M.pcbD);
  box("GPU.Card",  b, cx - 0.030, y0 + 0.060, z0 + 0.150, 0.070, 0.070, 0.210, M.gpu);
  box("GPU.Sink",  b, cx - 0.030, y0 + 0.098, z0 + 0.150, 0.066, 0.014, 0.200, M.hs);
  for (var f = 0; f < 3; f++)
    cyl("GPU.Fan" + f, b, cx - 0.030, y0 + 0.102, z0 + 0.075 + f * 0.070, 0.028, 0.006, M.fan);
  box("GPU.Cpu",   b, cx + 0.045, y0 + 0.048, z0 + 0.110, 0.055, 0.046, 0.055, M.hs);
  box("GPU.Ram",   b, cx + 0.045, y0 + 0.044, z0 + 0.190, 0.048, 0.038, 0.026, M.pcbD);
  return b;
}

/* ================= Chassis stapeln ================= */
var chassisY = [];
var y = 0.020 + 3 * U + 0.010;
for (var ci = 0; ci < 3; ci++) { chassisY.push(y); y += CH_H + 0.008; }

for (var ci2 = 0; ci2 < 3; ci2++) {
  var y0 = chassisY[ci2];
  var ch = grp("Chassis" + (ci2 + 1), root);
  /* Boden, Decke, Seiten */
  box("Ch.Floor", ch, 0, y0 + 0.004, RD * 0.30, IW, 0.008, 0.62, M.chas);
  box("Ch.Top",   ch, 0, y0 + CH_H - 0.004, RD * 0.30, IW, 0.008, 0.62, M.chas);
  box("Ch.SideL", ch, -IW / 2 - 0.004, y0 + CH_H / 2, RD * 0.30, 0.008, CH_H, 0.62, M.chas);
  box("Ch.SideR", ch,  IW / 2 + 0.004, y0 + CH_H / 2, RD * 0.30, 0.008, CH_H, 0.62, M.chas);
  /* Fuehrungsschienen im 1S-Raster */
  for (var gi = 0; gi <= 10; gi++)
    box("Ch.Guide" + gi, ch, X0 - 0.0025 + gi * PITCH, y0 + 0.011, BL_Z0 + BL_D / 2,
        0.003, 0.010, BL_D + 0.020, M.frame);
  backplane(ch, y0);

  if (ci2 === 0) {
    /* Chassis 1: 10 x 1S */
    for (var s10 = 0; s10 < 10; s10++) {
      var cx = X0 + W1S / 2 + s10 * PITCH;
      if (s10 === 3 || s10 === 7) bladeStorage(ch, cx, y0);
      else                        bladeSoC(ch, cx, y0, 0);
    }
  } else if (ci2 === 1) {
    /* Chassis 2: 5 x 2S, das mittlere im Hot-Swap herausgezogen */
    for (var s5 = 0; s5 < 5; s5++) {
      var cx2 = X0 + PITCH - 0.0025 + s5 * 2 * PITCH;
      bladeMiniITX(ch, cx2, y0, s5 === 2 ? 0.215 : 0);
    }
  } else {
    /* Chassis 3: 2 x 4S (GPU) + 2 x 1S, ein Slot bleibt leer */
    bladeGPU(ch, X0 + 2 * PITCH - 0.0045, y0);
    bladeGPU(ch, X0 + 6 * PITCH - 0.0045, y0);
    bladeSoC(ch, X0 + W1S / 2 + 8 * PITCH, y0, 0);
    /* Slot 10 absichtlich leer -> man sieht die Kontakte der Backplane */
  }
}

/* ================= Top-of-Rack-Switch ================= */
var swY = chassisY[2] + CH_H + 0.012;
var sw = grp("TorSwitch", root);
box("SW.Case", sw, 0, swY + U / 2, RD * 0.25, IW, U, 0.42, M.sw);
for (var p24 = 0; p24 < 12; p24++)
  box("SW.Port" + p24, sw, X0 + 0.015 + p24 * 0.034, swY + U / 2, 0.036, 0.018, 0.012, 0.010, M.pcbD);
box("SW.BusClamp", sw, 0, swY + U / 2, BUS_Z - 0.020, 0.20, 0.030, 0.046, M.cu);

/* ================= Licht ================= */
var lg = grp("Lights", root);
sun("Sun", lg, -52, 38, 1.15);
light("Fill.Front", lg, 0.0, 0.75, -0.55, 0.85, 0.90, 1.00, 2.6, 2.2);
light("Fill.Low",   lg, -0.45, 0.22, -0.30, 0.80, 0.85, 1.00, 1.5, 1.6);
light("Bus.Glow",   lg, 0.0, 0.50, BUS_Z - 0.06, 1.00, 0.45, 0.30, 1.6, 0.9);
light("Slot.Glow",  lg, 0.0, chassisY[1] + 0.09, -0.10, 1.00, 0.95, 0.85, 2.0, 1.1);

editor.commit();
"OPENRACKLAB gebaut, Knoten: " + editor.count();
