// THE FEEDING HALL - das ganze Spiel in einem Behaviour.
//
// Haengt auf dem Knoten "GameLogic". Ein Skript statt fuenf, weil Behaviours
// getrennte JS-Kontexte haben: der Manager greift Pferde, Wings, Kisten, den
// Pickup und die Kamera ueber scene.find() und schreibt ihre Transforms.
//
//   WASD   laufen / fahren      Maus  umsehen
//   Klick  Wing werfen          Shift rennen (zu Fuss) bzw. Handbremse
//   Enter  weiter               R     Neustart des Levels
//
// DREI LEVEL
//   1 THE FEEDING HALL  12 Pferde, leere Halle
//   2 THE WET WING      20 Pferde, Kisten im Weg, weniger Zeit
//   3 THE DRIVE-THRU    28 Pferde, im Pickup, Wings aus dem Fenster
//
// @header Spiel
// @param float speed       = 5.5
// @param float sprintMul   = 1.75
// @param float mouseSens   = 0.18
// @param float throwSpeed  = 16
// @param float gravity     = 14
// @tooltip Selbsttest: das Spiel spielt sich allein (fuer den Headless-Lauf).
// @param bool  autoplay    = false
// @tooltip Level, mit dem gestartet wird (1-3).
// @param int   startLevel  = 1

var P = (typeof params === "object" && params) ? params : {};
function num(k, d) { var v = P[k]; return (typeof v === "number" && !isNaN(v)) ? v : d; }

var SPEED, SPRINT, SENS, THROW, GRAV;
var DEG = Math.PI / 180;

/* ---- die Level ---------------------------------------------------------- */
var LEVELS = [
  { name: "THE FEEDING HALL", horses: 12, time: 180, ammo: 26, mode: "foot",  crates: 0,
    hint: "WIRF HOT WINGS AUF JEDES PFERD" },
  { name: "THE WET WING",     horses: 20, time: 200, ammo: 34, mode: "foot",  crates: 14,
    hint: "MEHR PFERDE, WENIGER PLATZ" },
  { name: "THE DRIVE-THRU",   horses: 28, time: 240, ammo: 70, mode: "drive", crates: 8,
    hint: "DU FAEHRST. WINGS AUS DEM FENSTER" }
];

var POOL_HORSES = 28, POOL_CRATES = 14, POOL_WINGS = 10;
var HW = 12.4, HL = 25.4;                 /* Innenmasse der Halle */

/* ---- Zustand ------------------------------------------------------------ */
var yaw = 0, pitch = -4;
var px = 0, py = 1.75, pz = -20;
var vel = 0, steer = 0;                    /* Fahrmodus */
var cam = -1, bucket = -1, truck = -1, wheelRing = -1, wheelHub = -1;
var horses = [], wings = [], crates = [];
var level = 0, fed = 0, ammo = 0, score = 0, total = 0;
var combo = 0, comboT = 0, timeLeft = 0;
var phase = "title";                       /* title | play | clear | over | won */
var stateT = 0, msg = "";
var clickHeld = false, enterHeld = false;
var stepT = 0, neighT = 3, ambStarted = false;

var HAS_SND = (typeof audio === "object" && audio && typeof audio.play === "function");
var AUTO = (P.autoplay === true);
var autoT = 0, logT = 0;
var wingBounce = 0;

/* Ein Treffer loest vier Sounds gleichzeitig aus, und bei einer Combo liegen
 * mehrere Treffer in derselben Zehntelsekunde. Ohne Bremse stapeln sich die
 * Stimmen und der Mix uebersteuert (gemessen: Peak 2.47). Jedes Event darf
 * deshalb nur alle SND_GAP Sekunden neu anfangen. */
var SND_GAP = 0.08, sndLast = {}, sndClock = 0;
function sndOk(n) {
  var t = sndLast[n];
  if (typeof t === "number" && sndClock - t < SND_GAP) return false;
  sndLast[n] = sndClock; return true;
}
function snd(n, v, p2) { if (HAS_SND && sndOk(n)) audio.play(n, v || 1, p2 || 1); }
function snd3(n, x, y, z, v, p2) {
  if (!HAS_SND || !sndOk(n)) return;
  if (audio.play3d) audio.play3d(n, x, y, z, v || 1, p2 || 1);
  else audio.play(n, v || 1, p2 || 1);
}

/* ---- Transform-Helfer (IDs und Quaternionen, siehe player_controller.js) - */
function yawQuat(d) { var h = d * DEG * 0.5; return [0, Math.sin(h), 0, Math.cos(h)]; }
function yawPitchQuat(yd, pd) {
  var cy = Math.cos(yd * DEG * 0.5), sy = Math.sin(yd * DEG * 0.5);
  var cp = Math.cos(pd * DEG * 0.5), sp = Math.sin(pd * DEG * 0.5);
  return [cy * sp, sy * cp, -sy * sp, cy * cp];
}
function place(n, x, y, z, yd) {
  if (n < 0) return;
  node.setPos(n, x, y, z);
  if (typeof yd === "number") { var q = yawQuat(yd); node.setRot(n, q[0], q[1], q[2], q[3]); }
}
function park(n) { if (n >= 0) node.setPos(n, 0, -60, 0); }

/* ---- Aufbau ------------------------------------------------------------- */
function init() {
  /* Der Neon-Look. Die Halle ist gelb-pink gekachelt und voller Leuchtschrift -
   * ohne Bloom bleibt dieses Licht auf der Oberflaeche kleben. Einmal armen,
   * danach fasst das Spiel nur noch den Trefferblitz an. */
  if (typeof fx === "object" && fx && fx.set) {
    fx.set({ bloom: 0.85, threshold: 0.62, knee: 0.3,
             vignette: 0.32, grain: 0.035, aberration: 0.0022, scanlines: 0.07 });
  }
  SPEED = num("speed", 5.5); SPRINT = num("sprintMul", 1.75);
  SENS = num("mouseSens", 0.18); THROW = num("throwSpeed", 16);
  GRAV = num("gravity", 14);

  cam = scene.find("Main Camera");
  bucket = scene.find("Bucket");
  truck = scene.find("Truck");
  wheelRing = scene.find("WheelRing");
  wheelHub = scene.find("WheelHub");

  horses = [];
  for (var i = 0; i < POOL_HORSES; i++) {
    var n = scene.find("Horse" + i);
    if (n < 0) continue;
    horses.push({ node: n, crown: scene.find("Horse" + i + ".Crown"),
                  x: 0, z: 0, yaw: 0, fed: false, live: false, wx: 0, wz: 0 });
  }
  wings = [];
  for (var w = 0; w < POOL_WINGS; w++) {
    var wn = scene.find("Wing" + w);
    if (wn >= 0) wings.push({ node: wn, live: false, x: 0, y: 0, z: 0, vx: 0, vy: 0, vz: 0 });
  }
  crates = [];
  for (var c = 0; c < POOL_CRATES; c++) {
    var cn = scene.find("Crate" + c);
    if (cn >= 0) crates.push({ node: cn, top: scene.find("CrateTop" + c),
                               x: 0, z: 0, live: false });
  }
  collectPillars();
  var s0 = num("startLevel", 1) - 1;
  if (s0 < 0) s0 = 0;
  if (s0 > LEVELS.length - 1) s0 = LEVELS.length - 1;
  startLevel(s0);
  if (HAS_SND && !ambStarted) { audio.play("hall_ambience", 1, 1); ambStarted = true; }
}

function startLevel(idx) {
  level = idx;
  var L = LEVELS[level];
  total = L.horses;
  fed = 0; ammo = L.ammo; combo = 0; comboT = 0; timeLeft = L.time;
  vel = 0; steer = 0;
  yaw = 180; pitch = L.mode === "drive" ? -6 : -4;   /* Blick in die Halle, nicht in die Wand */
  px = 0; py = L.mode === "drive" ? 1.95 : 1.75; pz = -HL + 4;

  for (var i = 0; i < horses.length; i++) {
    var h = horses[i];
    h.fed = false;
    h.live = i < total;
    if (!h.live) { park(h.node); park(h.crown); continue; }
    park(h.crown);
    /* verteilt, aber nie im Ruecken des Spielers und nie in der Wand */
    var a = i * 2.399963, r = 4 + (i % 6) * 1.7;
    h.x = Math.cos(a) * r * (HW - 3) / 14;
    h.z = -HL + 9 + (i / total) * (HL * 2 - 14) + Math.sin(a) * 2.5;
    if (h.x > HW - 2) h.x = HW - 2;
    if (h.x < -HW + 2) h.x = -HW + 2;
    h.wx = h.x; h.wz = h.z;
    h.yaw = (i * 47) % 360;
    place(h.node, h.x, 0, h.z, h.yaw);
  }
  for (var c = 0; c < crates.length; c++) {
    var cr = crates[c];
    cr.live = c < L.crates;
    if (!cr.live) { park(cr.node); park(cr.top); continue; }
    var ang = c * 1.7;
    cr.x = Math.cos(ang) * (HW - 4);
    cr.z = -HL + 8 + c * (HL * 2 - 12) / L.crates;
    place(cr.node, cr.x, 0.9, cr.z, (c * 33) % 360);
    place(cr.top, cr.x, 1.85, cr.z, (c * 33) % 360);
  }
  for (var w = 0; w < wings.length; w++) { wings[w].live = false; park(wings[w].node); }

  if (L.mode === "drive") { place(truck, px, 0, pz, 180); }
  else { park(truck); park(wheelRing); park(wheelHub); }

  phase = "title"; stateT = 2.6; msg = "";
}

/* ---- Werfen ------------------------------------------------------------- */
function throwWing() {
  if (ammo <= 0) return;
  for (var i = 0; i < wings.length; i++) {
    var w = wings[i];
    if (w.live) continue;
    var cy = Math.cos(pitch * DEG), sy = Math.sin(pitch * DEG);
    var fx = -Math.sin(yaw * DEG) * cy, fz = -Math.cos(yaw * DEG) * cy;
    var speedBoost = LEVELS[level].mode === "drive" ? vel * 0.6 : 0;
    w.x = px + fx * 0.8; w.y = py - 0.15; w.z = pz + fz * 0.8;
    w.vx = fx * (THROW + speedBoost); w.vy = sy * THROW + 3.0; w.vz = fz * (THROW + speedBoost);
    w.live = true;
    ammo--;
    snd("wing_throw", 0.7, 0.9 + Math.random() * 0.3);
    return;
  }
}

function newTarget(h) {
  h.wx = (Math.random() * 2 - 1) * (HW - 2.5);
  h.wz = (Math.random() * 2 - 1) * (HL - 3);
}

/* ---- Statische Hindernisse ---------------------------------------------
 * Die Halle hat 14 Saeulen (Pillar<r>_<s>, bosize 2.3 x 6.5 x 2.3, also
 * Radius 1.15). Die standen bisher in KEINER Kollisionsliste: man lief
 * durch sie durch, und jeder Wing flog mitten durch den Beton. Die
 * Positionen holt der Code aus der Szene selbst, damit er nicht luegt,
 * wenn jemand eine Saeule verschiebt. */
var pillars = [];
var PILLAR_R = 1.15;
/* Der Spieler ist ein Mensch, keine Litfasssaeule. Mit rad=1.6 stand man
 * 1,6 m VOR dem Beton in der Luft - das war die "komische Hitbox". */
var PLAYER_R = 0.40;
var TRUCK_R  = 1.25;

function collectPillars() {
  pillars = [];
  for (var r = 0; r < 12; r++) {
    for (var sd = 0; sd < 2; sd++) {
      var n = scene.find("Pillar" + r + "_" + sd);
      if (n < 0) continue;
      var p = node.getPos(n);
      if (!p) continue;
      pillars.push({ node: n, x: p[0], z: p[2], r: PILLAR_R });
    }
  }
}

function blockedBy(x, z, rad) {
  for (var c = 0; c < crates.length; c++) {
    var cr = crates[c];
    if (!cr.live) continue;
    var dx = x - cr.x, dz = z - cr.z;
    if (dx * dx + dz * dz < rad * rad) return cr;
  }
  for (var q = 0; q < pillars.length; q++) {
    var pl = pillars[q];
    var ex = x - pl.x, ez = z - pl.z;
    var rr = rad + pl.r;
    if (ex * ex + ez * ez < rr * rr) return pl;
  }
  return null;
}


/* Alles-oder-nichts-Kollision fuehlt sich an wie Reibung 1: man klebt an der
 * Saeule fest und kann nicht an ihr entlang. Deshalb erst die volle Bewegung,
 * dann jede Achse einzeln - was frei ist, wird gegangen. Das IST das Gleiten. */
function moveSlide(mx, mz, rad) {
  var nx = px + mx, nz = pz + mz;
  if (!blockedBy(nx, nz, rad)) { px = nx; pz = nz; return 0; }
  if (mx !== 0 && !blockedBy(nx, pz, rad)) { px = nx; return 1; }
  if (mz !== 0 && !blockedBy(px, nz, rad)) { pz = nz; return 1; }
  return 2;
}

/* Steckt man doch einmal in einem Hindernis (Spawn, verschobene Saeule),
 * schiebt das hier radial heraus statt den Spieler einzusperren. */
function unstick(rad) {
  var o = blockedBy(px, pz, rad);
  if (!o) return;
  var r = (o.r || 0.9) + rad + 0.02;
  var dx = px - o.x, dz = pz - o.z;
  var d = Math.sqrt(dx * dx + dz * dz);
  if (d < 0.0001) { dx = 1; dz = 0; d = 1; }
  px = o.x + dx / d * r;
  pz = o.z + dz / d * r;
}


/* Ein Pferd ist ein KOERPER, kein Punkt. Der alte Test fragte den Abstand zur
 * Pferdemitte an der Endposition des Frames ab UND verlangte w.y < 2.9 - zwei
 * Bedingungen, die im selben Einzelbild zutreffen mussten. Wer etwas steiler
 * warf, dessen Wing war beim Passieren auf 3,9 m: er flog sichtbar durch das
 * Pferd und es passierte nichts. Dazu kam, dass nur die Endposition geprueft
 * wurde - zwischen zwei Bildern legt ein Wing bei 16 m/s eine ganze Strecke
 * zurueck, und was dazwischen liegt, sah niemand.
 * Jetzt: die gesamte Flugstrecke dieses Frames gegen einen stehenden Zylinder.
 * Damit ist der Treffer unabhaengig von der Bildrate und trifft den ganzen
 * Koerper, vom Huf bis ueber den Kopf (die Krone sitzt auf 3,1 m). */
var HORSE_R = 1.35;      /* Radius des Pferdekoerpers in der Draufsicht */
var HORSE_TOP = 3.15;    /* Oberkante inkl. Kopf/Krone                  */
var HORSE_BOT = 0.15;

function wingHitsHorse(ho, ax, ay, az, bx, by, bz) {
  /* naechster Punkt des Segments A->B zur Pferdeachse, in der Draufsicht */
  var dx = bx - ax, dz = bz - az;
  var len2 = dx * dx + dz * dz;
  var t = 0;
  if (len2 > 1e-9) {
    t = ((ho.x - ax) * dx + (ho.z - az) * dz) / len2;
    if (t < 0) t = 0; else if (t > 1) t = 1;
  }
  var cx = ax + dx * t, cz = az + dz * t;
  var ex = ho.x - cx, ez = ho.z - cz;
  if (ex * ex + ez * ez > HORSE_R * HORSE_R) return false;
  var cy = ay + (by - ay) * t;
  return cy > HORSE_BOT && cy < HORSE_TOP;
}

/* Wings prallen an Saeulen und Waenden ab statt durchzufliegen. */
function wingHitsSolid(w) {
  for (var q = 0; q < pillars.length; q++) {
    var pl = pillars[q];
    var dx = w.x - pl.x, dz = w.z - pl.z;
    var rr = pl.r + 0.18;
    if (dx * dx + dz * dz < rr * rr && w.y < 6.5) return true;
  }
  for (var c = 0; c < crates.length; c++) {
    var cr = crates[c];
    if (!cr.live) continue;
    var cx = w.x - cr.x, cz = w.z - cr.z;
    if (cx * cx + cz * cz < 0.95 * 0.95 && w.y < 2.0) return true;
  }
  if (w.x > HW - 0.2 || w.x < -HW + 0.2) return true;
  if (w.z > HL - 0.2 || w.z < -HL + 0.2) return true;
  return false;
}

/* ---- Frame -------------------------------------------------------------- */
function frame() {
  var dt = (typeof state === "object" && state && state.dt > 0) ? state.dt : 1 / 60;
  if (dt > 0.1) dt = 0.1;
  sndClock += dt;
  if (input.key("r")) { startLevel(level); return; }

  if (phase === "title") {
    stateT -= dt;
    if (stateT <= 0 || pressedEnter()) phase = "play";
    drawHud();
    return;
  }
  if (phase === "clear") {
    stateT -= dt;
    if (stateT <= 0 || pressedEnter()) {
      if (level + 1 < LEVELS.length) startLevel(level + 1);
      else { phase = "won"; snd("win", 1, 1); }
    }
    drawHud();
    return;
  }
  if (phase === "over" || phase === "won") {
    if (pressedEnter()) startLevel(phase === "won" ? 0 : level);
    drawHud();
    return;
  }

  var L = LEVELS[level];
  var drive = L.mode === "drive";

  /* ---- umsehen ---- */
  yaw -= input.mouseDX() * SENS;
  pitch -= input.mouseDY() * SENS;
  if (pitch > 75) pitch = 75;
  if (pitch < -75) pitch = -75;

  var fx = -Math.sin(yaw * DEG), fz = -Math.cos(yaw * DEG);
  var rx = -fz, rz = fx;

  if (!drive) {
    /* ---- laufen ---- */
    var mx = 0, mz = 0;
    if (input.key("w")) { mx += fx; mz += fz; }
    if (input.key("s")) { mx -= fx; mz -= fz; }
    if (input.key("d")) { mx += rx; mz += rz; }
    if (input.key("a")) { mx -= rx; mz -= rz; }
    var len = Math.sqrt(mx * mx + mz * mz);
    if (len > 0.001) {
      var v = SPEED * (input.key("shift") ? SPRINT : 1) * dt / len;
      moveSlide(mx * v, mz * v, PLAYER_R);
      stepT -= dt * (input.key("shift") ? 1.6 : 1.0);
      if (stepT <= 0) { snd("step", 0.5, 0.92 + Math.random() * 0.2); stepT = 0.46; }
    } else stepT = 0.12;
    py = 1.75;
  } else {
    /* ---- fahren: Gas, Bremse, Lenkung, und der Blick folgt dem Wagen ---- */
    var acc = 0;
    if (input.key("w")) acc += 14;
    if (input.key("s")) acc -= 11;
    if (input.key("shift")) acc -= vel * 6;          /* Handbremse */
    vel += acc * dt;
    vel -= vel * 0.6 * dt;                            /* Rollwiderstand */
    if (vel > 22) vel = 22;
    if (vel < -7) vel = -7;
    var want = 0;
    if (input.key("a")) want -= 1;
    if (input.key("d")) want += 1;
    steer += (want - steer) * Math.min(1, dt * 6);
    yaw -= steer * (55 * dt) * Math.min(1, Math.abs(vel) / 6) * (vel < 0 ? -1 : 1);
    fx = -Math.sin(yaw * DEG); fz = -Math.cos(yaw * DEG);
    var tx = px + fx * vel * dt, tz = pz + fz * vel * dt;
    var hit = blockedBy(tx, tz, TRUCK_R);
    if (hit) {
      /* Schraegt man eine Saeule an, soll der Wagen an ihr entlangschrammen -
       * nur ein Frontaltreffer (beide Achsen blockiert) bremst wirklich. */
      var how = moveSlide(fx * vel * dt, fz * vel * dt, TRUCK_R);
      if (how === 2) vel *= -0.25; else vel *= 0.86;
      snd3("wing_hit", hit.x, 1, hit.z, 0.55, 0.7);
    } else { px = tx; pz = tz; }
    py = 1.95;
    place(truck, px, 0, pz, yaw + 180);
    /* Lenkrad und Bucket sitzen im Cockpit, vor der Kamera */
    /* Lenkrad: liegt flach geneigt vor dem Fahrer und dreht mit dem Einschlag */
    var wx = px + fx * 0.95, wz = pz + fz * 0.95;
    var wq = yawPitchQuat(yaw + steer * 25, 62);
    if (wheelRing >= 0) {
      node.setPos(wheelRing, wx, py - 0.46, wz);
      node.setRot(wheelRing, wq[0], wq[1], wq[2], wq[3]);
    }
    if (wheelHub >= 0) {
      node.setPos(wheelHub, wx, py - 0.46, wz);
      node.setRot(wheelHub, wq[0], wq[1], wq[2], wq[3]);
    }
    /* Das Lenkrad steht schraeg im Cockpit: eine Drehung, nicht zwei
     * Schreibvorgaenge - place() hat die Pose schon gesetzt. */
  }
  if (px > HW) px = HW; if (px < -HW) px = -HW;
  if (pz > HL) pz = HL; if (pz < -HL) pz = -HL;
  unstick(drive ? TRUCK_R : PLAYER_R);

  /* ---- Ohren, Kamera, Bucket ---- */
  if (HAS_SND && audio.listener) audio.listener(px, py, pz, fx, 0, fz);
  if (cam >= 0) {
    node.setPos(cam, px, py, pz);
    var cq = yawPitchQuat(yaw, pitch);
    node.setRot(cam, cq[0], cq[1], cq[2], cq[3]);
  }
  if (bucket >= 0) {
    var cyp = Math.cos(pitch * DEG);
    var bx = px + fx * cyp * 0.75 + rx * 0.42;
    var bz = pz + fz * cyp * 0.75 + rz * 0.42;
    place(bucket, bx, py - 0.48 + Math.sin(pitch * DEG) * 0.6, bz, yaw + 14);
  }

  /* ---- Selbsttest ---- */
  if (AUTO) autoplay(dt, drive);

  /* ---- werfen ---- */
  var down = input.mouseButton(0);
  if (down && !clickHeld) throwWing();
  clickHeld = down;

  /* ---- Wings ---- */
  for (var i = 0; i < wings.length; i++) {
    var w = wings[i];
    if (!w.live) continue;
    w.vy -= GRAV * dt;
    var oldx = w.x, oldy = w.y, oldz = w.z;
    w.x += w.vx * dt; w.y += w.vy * dt; w.z += w.vz * dt;
    place(w.node, w.x, w.y, w.z, 0);
    if (w.y < 0.05) { snd3("wing_splash", w.x, 0.1, w.z, 0.9, 1); w.live = false; park(w.node); continue; }
    if (wingHitsSolid(w)) {
      snd3("wing_hit", w.x, w.y, w.z, 0.5, 1.15); wingBounce++;
      w.live = false; park(w.node); continue;
    }
    for (var h = 0; h < horses.length; h++) {
      var ho = horses[h];
      if (!ho.live || ho.fed) continue;
      if (wingHitsHorse(ho, oldx, oldy, oldz, w.x, w.y, w.z)) {
        feedHorse(ho);
        w.live = false; park(w.node);
        break;
      }
    }
  }

  /* ---- Pferde ---- */
  for (var k = 0; k < horses.length; k++) {
    var ho2 = horses[k];
    if (!ho2.live) continue;
    var txx, tzz, spd;
    if (ho2.fed) { txx = px; tzz = pz; spd = drive ? 5.5 : 3.4; }
    else {
      txx = ho2.wx; tzz = ho2.wz; spd = 1.5;
      var ddx = txx - ho2.x, ddz = tzz - ho2.z;
      if (ddx * ddx + ddz * ddz < 1.5) newTarget(ho2);
    }
    if (ho2.fed && ho2.crown >= 0) node.setPos(ho2.crown, ho2.x, 3.1, ho2.z);
    var ax = txx - ho2.x, az = tzz - ho2.z;
    var d = Math.sqrt(ax * ax + az * az);
    var keep = ho2.fed ? (drive ? 4.5 : 2.6) : 0.2;
    if (d > keep) {
      var stepx = ax / d * spd * dt, stepz = az / d * spd * dt;
      if (!blockedBy(ho2.x + stepx, ho2.z + stepz, HORSE_R)) { ho2.x += stepx; ho2.z += stepz; }
      else newTarget(ho2);
      ho2.yaw = Math.atan2(ax, az) / DEG;
    }
    place(ho2.node, ho2.x, 0, ho2.z, ho2.yaw);
  }

  /* ---- Wiehern ---- */
  neighT -= dt;
  if (neighT <= 0) {
    neighT = 6 + Math.random() * 9;
    var hungry = [];
    for (var q2 = 0; q2 < horses.length; q2++)
      if (horses[q2].live && !horses[q2].fed) hungry.push(horses[q2]);
    if (hungry.length) {
      var hh = hungry[Math.floor(Math.random() * hungry.length)];
      snd3("horse_neigh", hh.x, 2.4, hh.z, 0.8, 0.95 + Math.random() * 0.15);
    }
  }

  /* ---- Uhr und Ende ---- */
  comboT -= dt;
  if (comboT <= 0) { combo = 0; comboT = 0; }
  timeLeft -= dt;
  if (fed >= total) {
    score += Math.floor(timeLeft) * 10;
    phase = "clear"; stateT = 3.2;
    if (typeof fx === "object" && fx && fx.flash) fx.flash(0.35, 1.0, 0.94, 0.55);
    msg = "LEVEL " + (level + 1) + " GESCHAFFT";
    snd("level_clear", 1, 1);
  } else if (timeLeft <= 0) {
    timeLeft = 0; phase = "over"; msg = "DIE PFERDE HUNGERN WEITER";
    snd3("horse_neigh", px + 2, 2.4, pz + 2, 1, 0.8);
  } else if (ammo <= 0 && noWingLive()) {
    phase = "over"; msg = "KEINE WINGS MEHR";
  }

  drawHud();
}

function feedHorse(ho) {
  ho.fed = true; fed++;
  comboT = 2.5; combo++;
  score += 100 * (combo > 5 ? 5 : combo);
  ammo += 2;
  /* Die Krone ist das EINZIGE sichtbare Zeichen, dass ein Treffer gezaehlt
   * hat. Sie stand hier auf der festen Position (0, 3.1, 1.35) - das ist
   * keine Position am Pferd, sondern ein fester Punkt mitten in der Halle,
   * derselbe fuer jedes Pferd. Der Treffer wurde also gewertet, aber am
   * getroffenen Tier passierte sichtbar nichts. Zusammen mit stummem Ton
   * sieht das exakt aus wie "der Wing fliegt durch und nichts geschieht".
   * Die Krone gehoert ueber DIESES Pferd - und muss ihm folgen, denn ein
   * gefuettertes Pferd laeuft dem Spieler hinterher. */
  if (ho.crown >= 0) node.setPos(ho.crown, ho.x, 3.1, ho.z);
  /* Sofortiges Feedback im ganzen Bild, nicht nur am Tier: die Web-Version
   * hat dafuer ein Hitmarker-Kreuz, und genau dieses "es hat gezaehlt" hat
   * hier gefehlt. Warm und kurz, mit der Combo etwas kraeftiger. */
  if (typeof fx === "object" && fx && fx.flash)
    fx.flash(1.0, 0.82, 0.25, 0.18 + Math.min(combo, 5) * 0.03);
  snd3("wing_hit", ho.x, 1.6, ho.z, 0.8, 1);
  snd3("horse_feed", ho.x, 2.2, ho.z, 0.7, 1);
  snd3("horse_neigh", ho.x, 2.4, ho.z, 0.55, 1);
  snd("ammo_pickup", 0.35, 1);
}


function pressedEnter() {
  var e = input.key("enter") || input.key("space");
  var hit = e && !enterHeld;
  enterHeld = e;
  return hit;
}
function noWingLive() {
  for (var i = 0; i < wings.length; i++) if (wings[i].live) return false;
  return true;
}
function two(n) { n = Math.floor(n); return (n < 10 ? "0" : "") + n; }

/* ---- Selbsttest --------------------------------------------------------- */
function autoplay(dt, drive) {
  autoT -= dt;
  var best = -1, bd = 1e9;
  for (var i = 0; i < horses.length; i++) {
    if (!horses[i].live || horses[i].fed) continue;
    var dx = horses[i].x - px, dz = horses[i].z - pz;
    var d2 = dx * dx + dz * dz;
    if (d2 < bd) { bd = d2; best = i; }
  }
  if (best < 0) return;
  var t = horses[best], dist = Math.sqrt(bd);
  yaw = Math.atan2(-(t.x - px), -(t.z - pz)) / DEG;
  pitch = -6 + dist * 0.55;
  if (drive) {
    /* Der Selbsttest faehrt mit fester Zielgeschwindigkeit statt zu regeln -
     * ein Regler, der um seinen Sollwert pendelt, bleibt bei 0 stehen. */
    var wantV = dist > 5 ? 9 : 3;
    vel += (wantV - vel) * Math.min(1, dt * 3);
    px += -Math.sin(yaw * DEG) * vel * dt;
    pz += -Math.cos(yaw * DEG) * vel * dt;
  } else if (dist > 4.5) {
    /* Der Selbsttest muss durch DIESELBE Kollision laufen wie der Spieler -
     * sonst geht er durch Saeulen und beweist gar nichts. Steht eine Saeule
     * im Weg, weicht er seitlich aus. */
    var sx = (t.x - px) / dist * SPEED * dt, sz = (t.z - pz) / dist * SPEED * dt;
    if (!blockedBy(px + sx, pz + sz, PLAYER_R)) { px += sx; pz += sz; }
    else if (!blockedBy(px - sz * 1.4, pz + sx * 1.4, PLAYER_R)) { px -= sz * 1.4; pz += sx * 1.4; }
    else { px += sz * 1.4; pz -= sx * 1.4; }
  }
  if (dist < 6 && autoT <= 0) { throwWing(); autoT = 0.3; ammo += 1; }
  logT -= dt;
  if (logT <= 0) {
    logT = 1;
    print("autoplay: L" + (level + 1) + " fed " + fed + "/" + total +
          "  ziel@" + t.x.toFixed(1) + "," + t.z.toFixed(1) +
          "  ich@" + px.toFixed(1) + "," + pz.toFixed(1) +
          "  d=" + dist.toFixed(1) + "  v=" + vel.toFixed(1) + "  " + phase +
          "  COLLPROBE pillars=" + pillars.length + " inside=" + (blockedBy(px, pz, 0.1) ? 1 : 0) +
          " wingsBounced=" + wingBounce);
  }
}

/* ---- HUD ---------------------------------------------------------------- */
function drawHud() {
  if (typeof gui !== "object" || !gui) return;       /* headless: kein UI */
  var s = gui.size(), W = s[0], H = s[1];
  var L = LEVELS[level];

  gui.text(24, 22, "LEVEL " + (level + 1) + " - " + L.name +
           (L.mode === "drive" ? "  [DRIVE]" : ""), 22, 0xFFFFE11A);
  gui.text(24, 50, "FEED ALL HORSES  " + fed + "/" + total, 30, 0xFFFF2EA6);
  gui.text(24, 88, "TIME " + two(timeLeft / 60) + ":" + two(timeLeft % 60), 20,
           timeLeft < 30 ? 0xFFFF2EA6 : 0xFF57FFF0);
  gui.text(W - 200, 22, "SCORE " + score, 22, 0xFF57FFF0);
  if (combo > 1) gui.text(W - 200, 50, "COMBO x" + combo, 24, 0xFFFF2EA6);
  gui.text(24, H - 44, "WINGS " + ammo, 24, 0xFFFFFFFF);
  if (LEVELS[level].mode === "drive")
    gui.text(W - 200, H - 44, Math.round(Math.abs(vel) * 3.6) + " KM/H", 24, 0xFFFFE11A);

  gui.rect(W / 2 - 9, H / 2 - 1, 18, 2, 0xC0FFE11A);
  gui.rect(W / 2 - 1, H / 2 - 9, 2, 18, 0xC0FFE11A);

  if (phase === "title") {
    gui.rect(0, H / 2 - 110, W, 220, 0xC0140212);
    gui.text(W / 2 - 220, H / 2 - 70, "LEVEL " + (level + 1), 40, 0xFFFFE11A);
    gui.text(W / 2 - 220, H / 2 - 18, L.name, 46, 0xFFFF2EA6);
    gui.text(W / 2 - 220, H / 2 + 40, L.hint, 22, 0xFFFFFFFF);
    gui.text(W / 2 - 220, H / 2 + 74, "ENTER", 20, 0xFF57FFF0);
  } else if (phase === "clear") {
    gui.rect(0, 0, W, H, 0xA0140212);
    gui.text(W / 2 - 200, H / 2 - 60, "LEVEL CLEAR", 46, 0xFFFFE11A);
    gui.text(W / 2 - 200, H / 2 - 4, msg, 24, 0xFFFFFFFF);
    gui.text(W / 2 - 200, H / 2 + 34, "SCORE " + score, 28, 0xFF57FFF0);
    gui.text(W / 2 - 200, H / 2 + 74, "ENTER FUER LEVEL " + (level + 2), 20, 0xFFFFFFFF);
  } else if (phase === "over") {
    gui.rect(0, 0, W, H, 0xB0140212);
    gui.text(W / 2 - 200, H / 2 - 60, "GAME OVER", 46, 0xFFFF2EA6);
    gui.text(W / 2 - 200, H / 2 - 4, msg, 24, 0xFFFFFFFF);
    gui.text(W / 2 - 200, H / 2 + 34, "SCORE " + score, 28, 0xFF57FFF0);
    gui.text(W / 2 - 200, H / 2 + 74, "ENTER ODER R", 20, 0xFFFFFFFF);
  } else if (phase === "won") {
    gui.rect(0, 0, W, H, 0xC0140212);
    gui.text(W / 2 - 240, H / 2 - 70, "ALLE PFERDE SATT", 46, 0xFFFFE11A);
    gui.text(W / 2 - 240, H / 2 - 10, "DREI LEVEL, EIN EIMER", 24, 0xFFFFFFFF);
    gui.text(W / 2 - 240, H / 2 + 30, "ENDSTAND " + score, 30, 0xFF57FFF0);
    gui.text(W / 2 - 240, H / 2 + 74, "ENTER FUER VON VORN", 20, 0xFFFFFFFF);
  }
}
