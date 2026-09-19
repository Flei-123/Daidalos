# Daidalos agententauglich machen — der Plan

Geschrieben 19.09.2026 von JARVIS für Justin. Grundlage: Analyse dieses
Checkouts (233k Zeilen, 66k in src/include) plus fünf parallele Recherchen
(Engine-MCPs, C++-Build-Beschleunigung, deterministischer Replay + visuelles
Testen, Skriptsprachen-Vergleich, LT2-Machbarkeit). Quellen stehen in den
jeweiligen Abschnitten.

Alle Zahlen unten, die mit **[gemessen]** markiert sind, stammen aus echten
Läufen auf diesem Server am 19.09.2026. Alles andere ist Schätzung und als
solche gekennzeichnet.

---

## 0. Der Befund: Daidalos ist schon weiter als jeder Community-MCP

Die Recherche über Unity-, Unreal- und Godot-MCP-Server hat ein überraschendes
Ergebnis: **Daidalos hat die Architektur bereits, die alle anderen anstreben,
und bei einem entscheidenden Punkt liegt es vorn.**

Der meistgenutzte Unity-MCP (CoplayDev/unity-mcp, ~14,3k Sterne, 47 Tools) hat
in seiner offiziellen Tool-Reference **kein Screenshot-Tool**. Der Agent baut
dort blind. Blender MCP (~29k Sterne) ist deshalb das erfolgreichste Projekt
der Kategorie, weil es `get_viewport_screenshot` hat und den Loop
"Zustand lesen → klein handeln → **hinschauen** → korrigieren" ausdrücklich
als Arbeitsweise dokumentiert.

`tools/dai_mcp.py` macht genau das seit dem 16.09.2026: 17 Tools, und `shot`
gibt das PNG als Bild zurück. Dazu kommt, was keine der drei großen Engines
hat: **Determinismus als Gesetz** (`state(n+1) = step(state(n), input(n))`),
ein textbasiertes, git-diffbares Szenenformat (`.daiscene`), ein einziges
`build.sh` ohne CMake-Generator-Schicht, und ein Runtime-Export nach
Godot-Muster (`dai_pack` hängt das Projekt an ein vorkompiliertes Binary).

Es gibt also nichts nachzubauen. Es gibt **vier konkrete Löcher zu stopfen**,
und drei davon sind der Grund, warum der Gauntlet-Lauf am 17./18.09. mit
56/100 scheiterte.

---

## 1. Die Obduktion des gescheiterten Gauntlet-Laufs

Aus `GAUNTLET.md`, Runde 1 und 2:

* Runde 1: 2 von 3 Buildern fehlgeschlagen — einer abgebrochen, einer
  **"Reached maximum number of turns (120)"**.
* Runde 2: **alle drei** Builder mit Turn-Limit gescheitert. Lauf abgebrochen.
* Kritik an Runde 1: vier geforderte Screenshots waren **byte-identisch**
  (md5 `b598614a…`) unter vier verschiedenen Namen. Drei der vier Dateinamen
  waren schlicht gelogen. Dazu leere Panels, abgeschnittene Labels
  ("Block.Beve", "Pos…", "Rot…", "Siz…"), aus dem Rahmen laufende Timeline.

Das sind nicht drei unabhängige Pannen, sondern **eine Ursache und zwei
fehlende Wächter**:

**Ursache — der Build ist zu langsam.** `build.sh` ruft den Compiler
**98-mal rein sequenziell** auf. **[gemessen]** eine einzelne mittelgroße TU
(`src/dai_engine.cpp`) braucht **2,38 s** mit `-O3`. 98 × ~2,4 s ≈ 235 s reine
Compile-Zeit auf **einem** Kern, plus Shader, Tests und Links — passt exakt zu
den in `RUN.md` dokumentierten ~6 Minuten. Ein Builder-Agent mit 120 Turns, der
pro Iteration einmal baut, verbrennt sein halbes Turn-Budget im Warten. **Das
Turn-Limit ist kein Agenten-Problem, es ist ein Build-Problem.**

**Fehlender Wächter 1 — nichts prüft, ob ein Screenshot lügt.** Vier identische
Dateien unter vier Namen sind mit einem Dreizeiler erkennbar und wurden erst
von einem menschlich lesenden Kritiker gefunden.

**Fehlender Wächter 2 — Layout-Fehler werden im Bild gesucht.** Abgeschnittene
Labels sind ein Geometrieproblem, kein Bildproblem. Das UI-System kennt die
Wahrheit exakt; das Bild kennt sie nur verrauscht.

---

## 2. Die vier Maßnahmen, nach Nutzen/Aufwand

### M1 — Build von 6 min auf unter 30 s. **Höchste Priorität.**

Das ist die Maßnahme, die alle anderen erst ermöglicht: Sie verwandelt die
Iterationsschleife von "Kaffee holen" in "sofort", für mich *und* für dich.

**Rechnung:** ~235 s Compile-Zeit auf einem Kern. Dieser Server hat **20 Kerne,
aber nur 19 GB RAM** — `-O3` mit Templates frisst 0,5–2,5 GB pro g++-Prozess,
also ist `-j` auf `min(nproc, RAM_GB/2)` ≈ **9** zu deckeln, sonst swappt der
Build und wird *langsamer* als sequenziell. 235 s / 9 ≈ **26 s**.

**Schritt 1 — ccache. [gemessen, bereits installiert und konfiguriert]**

| Lauf | Zeit |
|---|---|
| `g++` pur, `src/dai_engine.cpp` | **2,38 s** |
| `ccache g++`, kalt (Miss) | 2,57 s |
| `ccache g++`, warm (Hit) | **0,00 s** |

Konfiguriert wurde: `max_size=20G`, `compression=true`, `hash_dir=false`,
`inode_cache=true`, `depend_mode=true`,
`sloppiness=time_macros,include_file_mtime,include_file_ctime,locale`.
Einbau in `build.sh`: eine Zeile am Kopf, dann überall `"$CXX"` statt `g++`:
```bash
CXX="${CXX:-g++}"
command -v ccache >/dev/null && CXX="ccache $CXX"
```
Wirkung: Branch-Wechsel und `git checkout` hin und zurück kosten nichts mehr —
genau der Fall, in dem ein Gauntlet-Builder heute 6 Minuten verliert.

**Schritt 2 — Parallelisierung. Das ist der eigentliche Gewinn.**

Empfehlung: **generiertes `build.ninja`** statt `make`. Gründe: Ninja parst
die `.d`-Dateien einmal in ein Binärlog (`deps = gcc`), hat saubere
nicht-interleavte Ausgabe (bei 9 parallelen g++-Prozessen mit Template-Fehlern
entscheidend), erkennt **Änderungen an der Kommandozeile** (Flag geändert →
Rebuild, was Make nicht kann), und braucht für "nichts zu tun" <50 ms.
Ein ~60-Zeilen-`tools/gen_ninja.sh` erzeugt es aus der Dateiliste.

**Fallstricke, die eingeplant gehören:**
* Objektnamen müssen den **Pfad** kodieren — `src/a/util.cpp` und
  `src/b/util.cpp` dürfen nicht beide `obj/util.o` werden (stilles
  Überschreiben, Heisenbugs).
* `-MMD -MP` + `depfile`/`deps = gcc` ist Pflicht, sonst kein Inkrementbau.
* `-fdiagnostics-color=always` beibehalten.
* **Die Leak-Tests von `build.sh` müssen erhalten bleiben** (dai_engine.cpp
  ohne Jolt-Include-Pfad, kein Vulkan-Symbol außerhalb `rhi_vulkan*`). Das ist
  eine bewusste Eigenschaft dieser Codebasis, keine Altlast.
* Die Regel "compile und run sind zwei Statements, nie `g++ … && ./binary`"
  aus dem Kommentarkopf von `build.sh` muss in die Ninja-Fassung übertragen
  werden — sie hat schon einmal einen grünen Build mit gestrigem Binary
  verhindert.

**Schritt 3 — mold.** `apt install mold` ist erledigt (1.10.1). Ein
belastbarer Vorher/Nachher-Wert liegt **nicht** vor: der Testlink scheiterte
an fehlenden Symbolen, weil das Kommando aus `build.sh` nicht nachgebaut
wurde. Literaturwert: 3–8× kürzere Linkzeit. Vorgehen: `mold -run ./build.sh`
(klinkt sich per LD_PRELOAD ein, null Skriptänderung) und die Zeit
vergleichen, **bevor** `-fuse-ld=mold` fest eingetragen wird. Relevant wird das
erst, wenn die Compile-Phase auf ~26 s gefallen ist — dann sind ~40 Links
plötzlich ein spürbarer Anteil.

**Nicht machen: Unity/Jumbo Builds.** Bei 20 Kernen fressen sie die
Parallelität, ruinieren den Inkrementbau und die ccache-Hit-Rate. Chromium hat
sie wieder komplett entfernt.

**Erwartung:** Vollbuild kalt 360 s → **25–40 s**, mit warmem ccache ~15 s,
inkrementell (1–5 Dateien) **3–10 s**. Aufwand: 3–4 Stunden.

---

### M2 — Der Bild-Test darf nicht lügen. **Billigste echte Verbesserung.**

Direkt aus dem Gauntlet-Versagen abgeleitet. Ein Skript
`tools/shot_guard.py`, das nach jedem Screenshot-Lauf über `.gauntlet-shots/`
läuft und **hart fehlschlägt** bei:

1. **Duplikaten.** SHA256 über alle PNGs; zwei verschiedene Testnamen dürfen
   nicht dasselbe Bild ergeben, außer sie stehen in einer kommentierten
   `allowed_duplicates.txt`. Zusätzlich pHash mit kleiner Hamming-Distanz für
   *fast*-identische Bilder — die entstehen, wenn ein Setup-Schritt still
   nicht gewirkt hat. (Der Dreizeiler, der Runde 1 hätte retten können:
   `find .gauntlet-shots -name '*.png' -exec sha256sum {} + | sort | uniq -w64 -D`)
2. **Blank/Degenerate.** Mindest-Entropie: `unique_colors > N`,
   Standardabweichung über Schwelle. Ein schwarzes Bild (Kamera im Nichts,
   Renderer nicht initialisiert) darf **nie** bestehen. Genau dieser Fehler
   ist im Unity-MCP-Praxistest aufgetreten: Camera-Captures kamen beim Agenten
   "fast völlig schwarz" an, während Menschen die Szene normal sahen.
3. **Canary.** Ein fest eingebauter Negativtest mit absichtlich verändertem
   Bild, der **fehlschlagen muss**. Tut er es nicht, ist die Diff-Pipeline
   kaputt. Läuft bei jedem Lauf mit.
4. **Name ↔ Inhalt.** Jeder geforderte Shot-Name muss auf genau eine erzeugte
   Datei zeigen; verwaiste Dateien und fehlende Referenzen sind beides Fehler.

Aufwand: **1 Tag.** Wirkung: Der teuerste Einzelfehler des letzten
Gauntlet-Laufs wird strukturell unmöglich.

---

### M3 — Layout-Assertions statt Bildanalyse. **Der eigentliche UI-Bug-Detektor.**

Kernaussage der Recherche, und sie ist eindeutig: **Abgeschnittene und
überlappende Texte sucht man nicht im Bild.** Das Layout-System kennt die
Wahrheit exakt.

Die gute Nachricht: `include/dai_ui.h` hat die Bausteine schon —
`dai_ui_last_rect`, `dai_ui_last_field_rect`, `clip[4]` pro Draw-Kommando,
`dai_ui_clip_begin/end`. Es fehlt nur der **Export**.

Neu: `dai_ui_layout_dump(ui, path)` schreibt pro gezeichnetem Widget eine
Zeile:
```json
{"id":"inspector.name", "rect":[x,y,w,h], "clip":[x,y,w,h],
 "text":"Block.Bevelled", "ink":[w,h], "z":12, "visible":true, "opacity":1.0}
```
Darauf drei Assertions in `tools/run_tests.sh`:

* **CLIPPED**: `ink.w > rect.w - padding` → abgeschnitten. Das hätte
  "Block.Beve", "Pos…", "Rot…", "Siz…" *sofort und mit Namen* gemeldet.
* **CUT_OFF**: `rect ⊄ clip` bzw. `rect ⊄ viewport` → ragt raus. Hätte die
  Timeline gefangen, die rechts mit angeschnittenem "13" statt "130" endet.
* **OVERLAP**: Rechteck-Schnitt zwischen **opaken, nicht verschachtelten
  Geschwistern**. Filter sind Pflicht, sonst ertrinkst du in False Positives:
  Container enthalten ihre Kinder (kein Bug), Modals/Tooltips/Dropdowns per
  `layer: overlay` ausschließen, bei Text das **Ink-Rect** statt der Box
  nehmen (sonst schlägt line-height-Puffer an). Sweep-Line über x statt O(n²).

Fehlermeldung wird damit von "sieht komisch aus" zu
`inspector.name CLIPPED: ink 118px > box 96px bei 1100x700`.

Ergänzend der **Layout-Stresstest**, der die Bugs überhaupt erst produziert:
alle Assertions bei jeder getesteten Auflösung laufen lassen, plus
Pseudo-Lokalisierung (+40 % Textlänge). Genau das fehlt heute — die
1100×700-Fehler fielen erst einem menschlichen Kritiker auf.

Aufwand: **3–5 Tage.** Wirkung: der ganze Fehlerbereich, den ein Agent im
Quelltext prinzipiell nicht sehen kann, wird maschinell prüfbar.

---

### M4 — Deterministischer Replay. **Das Alleinstellungsmerkmal.**

Daidalos' README verspricht `state(n+1) = step(state(n), input(n))`. Der
Replay ist die Einlösung dieses Versprechens — und das Werkzeug, mit dem ich
einen Bug **selbst reproduzieren** kann, statt dich zu fragen "geht's jetzt?".

Format:
```
replay: version, build_hash, seed, initial_state_hash
        ticks:       [{tick, input_mask_p0, …}, …]
        checkpoints: [{tick, state_hash}, alle 60 Ticks]
```
`build_hash` ist **Pflicht** — ein Replay gegen einen anderen Build
abzuspielen ist undefiniert und muss hart fehlschlagen, nicht still abweichen.
Die State-Hash-Checkpoints sind der Grund, warum das Werkzeug taugt: eine
Divergenz wird **auf den Tick genau lokalisiert**, statt "am Ende sieht's
anders aus".

Dazu der **SyncTest nach GGPO-Muster**: jeden Tick künstlich zurückrollen,
neu simulieren, Checksumme vergleichen. Läuft ohne Netzwerk und findet
Nichtdeterminismus lokal und sofort. Das ist der Test, der aus "deterministisch"
einen Zustand statt einer Behauptung macht.

Die Determinismus-Killer, die Factorio in Jahren gesammelt hat und die auch
hier gelten — als Verbotsliste für den Simulationspfad:
`std::unordered_map`-Iteration (Bucket-Reihenfolge implementierungsabhängig),
Pointer-Adressen als ID oder Sortierschlüssel (ASLR!), `std::sin/cos/exp`
(libm ist **nicht** IEEE-spezifiziert, glibc/musl/Apple liefern
unterschiedliche letzte Bits), Wall-Clock, Thread-Scheduling-Reihenfolge,
uninitialisierter Speicher im gehashten State. Anmerkung: `-ffp-contract=fast`
steht in den aktuellen `FLAGS` von `build.sh` — für die Simulation gehört dort
`-ffp-contract=off` hin, sonst entscheidet der Compiler über FMA-Kontraktion
und damit über die letzten Bits.

Neue MCP-Tools: `replay_record`, `replay_play`, `replay_verify`.

Aufwand: **1–2 Wochen.** Wirkung: Ich kann Bugs selbst reproduzieren und
belegen, dass ein Fix wirkt.

---

## 3. Skripting: bei QuickJS bleiben, API ausbauen

Die Recherche kommt zu einem klaren Ergebnis, und es widerspricht der Intuition
"für Spiele nimmt man Lua":

**Determinismus spricht für JavaScript, nicht dagegen.** Lua und LuaJIT hashen
Tabellen-, Closure- und Userdata-Keys **nach Speicheradresse**. Mit ASLR heißt
das: gleicher Build, gleiche Inputs, gleicher Rechner → andere
`pairs()`-Reihenfolge bei jedem Prozessstart. Offenes LuaJIT-Issue #719, seit
2013 ungelöst. JavaScript garantiert seit ES2015 per Spezifikation:
Integer-Indices aufsteigend, dann String-Keys in **Einfügereihenfolge**,
`Map`/`Set` ebenso. Kostenlos, für immer.

Für eine Engine, deren Kernversprechen Determinismus ist, ist das ein
struktureller Vorteil, den man nicht aufgibt. Der einzige Kandidat mit besserem
Determinismus wäre WASM (mitgebündelte libm → bitidentische Transzendente über
alle OS), kostet aber die gesamte Ergonomie: linearer Speicher, keine Pointer,
Serialisierung an jeder Host-Grenze.

**Zu tun:**

1. **Determinismus-Härtung des Runtimes** (klar abgrenzbar, ~2 Tage):
   `Math.random`, `Date`, `performance`, `WeakRef`, `FinalizationRegistry`,
   `Intl` entfernen. `Math.sin/cos/tan/exp/pow/log/atan2` durch eine
   mitgelieferte fdlibm-Portierung ersetzen — dann ist die libm-Frage
   erledigt und WASMs einziger echter Vorteil verschwindet.
   Memory-Limit + Interrupt-Handler gegen Endlosschleifen.
2. **Entity-API auf 7 Lebenszyklus-Hooks bringen.** Heute gibt es
   `init()`/`frame()` (siehe `docs/SCRIPTING.md`) und 19 UI-Funktionen. Ziel:
   ```
   onAttach · onReady · onFixedUpdate(dt) · onUpdate(dt) · onEvent · onDetach
   serialize()/deserialize()
   ```
   Die zwei Regeln, ohne die Determinismus strukturell unmöglich ist:
   **nur `onFixedUpdate` darf Simulationszustand ändern**, mit konstantem dt
   (nicht gemessener Wandzeit); `onUpdate` bekommt einen Read-Only-View. Und
   die **Aufrufreihenfolge über Entities legt die Engine fest** (sortierte
   Entity-IDs), nicht die Script-Registry — dort bricht Determinismus in der
   Praxis am häufigsten.
   Godots Modell ist das bessere Vorbild: `onReady` erst, wenn alle Kinder
   fertig sind — das eliminiert die Unity-Klasse von
   "Awake/Start-Reihenfolge"-Bugs.
3. **Zustand gehört in C++, nicht in JS-Objektfelder.** Das ist die eine
   Architekturentscheidung, die Hot-Reload, Save/Load, Replay und Netcode
   **gleichzeitig** löst. Skripte werden damit möglichst zustandslose
   Verhaltensfunktionen über Engine-Daten, und Hot-Reload wird trivial:
   Modul neu evaluieren, Prototyp-Methoden tauschen, Instanzen stehen lassen
   (das Lua-Muster). Bei Compile-Fehler im neuen Modul: **altes behalten und
   loggen**, nie halb geladen weiterlaufen.
4. **`engine.d.ts` aus der Bindungsdefinition generieren**, nicht von Hand
   pflegen. Zahlt sich ab der dritten API-Erweiterung aus und gibt
   Skriptautoren TypeScript-Autovervollständigung auf die Engine-API.

Zur Performance-Sorge: QuickJS ist ein Bytecode-Interpreter und in heißen
numerischen Schleifen 10–50× langsamer als LuaJIT. Die richtige Konsequenz ist
nicht "andere Sprache", sondern **andere Arbeitsteilung**: Bewegung, Physik,
Pathfinding, Massen-Updates in C++; Skript macht Ereignislogik,
Zustandsmaschinen, Quests, Fähigkeiten, Tuning — einige tausend Aufrufe pro
Frame, nicht Millionen. In diesem Budget ist QuickJS unauffällig.

---

## 4. Der MCP-Server: was noch fehlt

`tools/dai_mcp.py` hat 17 Tools und ist gut geschnitten. Aus dem Vergleich mit
Blender MCP und IvanMurzak/Unity-MCP fehlen vier Dinge:

| fehlt | warum es zählt |
|---|---|
| `layout` | Der Bounding-Box-Dump aus M3 als Tool. Ohne ihn suche ich Layout-Bugs weiter im Bild. |
| `replay_*` | M4. Bug reproduzieren, ohne dich zu fragen. |
| `shot_compare` | Zwei Renders mit zwei Schwellen diffen (**global und lokal** — sonst versteckt sich ein fehlendes Icon im globalen Mittelwert, genau dafür hat Unreal `Maximum Local Error`). ODiff ist für große Auflösungen die beste Wahl. |
| `asset_find` | Blender MCPs unterschätzter Trumpf: der Agent kann fehlende Assets **selbst besorgen** statt Platzhalter-Würfel zu bauen. Im Unity-Praxistest war "alles hat Default-Material" der Grund, warum KI-Zubauten sofort erkennbar waren. |

Dazu eine Warnung aus den Praxisberichten, die direkt auf den Gauntlet-Lauf
zutrifft: **"Kein räumliches Verständnis."** Ein Agent scheiterte an einer
Türverbindung, weil er nur Außenkamera-Aufnahmen machte und nie *in* den Raum
ging. Übersetzt: `shot` braucht feste, benannte Kamera-Presets
(Augenhöhe-innen, Übersicht, Detail), sonst steht die Kamera in der Geometrie —
exakt der Mangel, den die Jury an `16-modifier-bevel.png` bemängelt hat.

---

## 5. Kann Daidalos "besser als UE5/Unity" werden?

**In Features: nein, und das sollte auch nicht das Ziel sein.** Lumen, Nanite,
Animation-Tooling, Asset-Marktplatz — das sind 25 Jahre und hunderte
Entwickler.

**In der Nische "eine Engine, die ein Agent bedienen kann": ja, und der
Vorsprung ist real.** UE5 und Unity sind für Menschen mit Maus gebaut; ihre
MCP-Server sind nachträglich angeklebt, und der meistgenutzte gibt dem Agenten
nicht einmal ein Bild zurück. Daidalos hat Textformat, Headless-Rendering,
Determinismus und Bild-Rückgabe **im Kern**, nicht als Aufsatz. Nach M1–M4
kann ein Agent hier Dinge, die er in Unity nicht kann: einen Bug
reproduzierbar abspielen, Layout-Fehler mit Namen und Pixelzahl melden, und in
Sekunden statt Minuten iterieren.

Der ehrliche Vergleich ist nicht Daidalos gegen UE5, sondern
**Daidalos+Agent gegen Unity+Mensch bei derselben Aufgabe.**

---

## 6. Und das Spiel? (LT2-artiges Ressourcen-Tycoon)

Kurzfassung der Machbarkeitsanalyse — Details siehe Recherche.

LT2 ist **kein** Voxelspiel; die Minecraft-Architektur trägt nicht. Der
"Zauber" ist Runtime-Mesh-Slicing: die Säge schneidet entlang einer beliebigen
Ebene, beide Hälften werden eigenständige Rigidbodies. Das ist mit 20–40
Personentagen das teuerste Einzelfeature — **und für einen Prototypen
überflüssig.** Baum aus 3–4 vorsegmentierten Stammstücken sieht 85 % so gut aus
und kostet 5 %.

Die drei Sätze, auf die es ankommt:

1. **Server-Autorität ist eine Architekturgrenze ab Commit 1, kein Feature für
   später.** Nachrüsten kostet das Dreifache. LT2s eigene Dupe-Geschichte ist
   die Warnung.
2. **Persistiere nur das Nötigste.** LT2 speichert platzierte Objekte,
   Inventar und Geld — loses Holz verschwindet beim Restart. Das ist die
   größte Einzelersparnis im ganzen Projekt, und es ist eine
   Design-Entscheidung des Originals, keine Notlösung.
3. **Vom Loop her bauen, nicht von der Engine her.** "Baum → Stamm → Brett →
   Wand" mit vier Spielern, hässlich aber stabil.

**Netcode-Empfehlung:** autoritativer Server + Snapshot-Interpolation (15 Hz)
+ Client-Prediction nur für die eigene Spielerfigur + Ownership-Delegation für
das eigene Fahrzeug. **Kein** deterministisches Lockstep (Float-Determinismus
über heterogene Hardware bekommst du mit Jolt nicht geschenkt — Factorios
Desync-Leidensgeschichte ist öffentlich dokumentiert), **kein** Rollback (bei
hunderten Rigidbodies nicht bezahlbar). Transport: ENet oder Valve
GameNetworkingSockets nehmen, kein eigenes Reliability-UDP — spart drei Wochen.

**Realistischer Aufwand:** MVP-Loop mit 4 Spielern ~8–12 Wochen Vollzeit, wenn
Renderer und Physik stehen (tun sie). Voll ausgebaut mit Slicing, Fahrzeugen,
Streaming: 18–30 Monate solo. Der Netzwerk-Block allein: 55–80 Tage, und die
Zeile "Debug-Tooling (Netzwerk-Graph, Packet-Logger, Lag-Simulator), 5–8 Tage"
wird immer gestrichen und ist immer die, deren Fehlen drei Wochen Debugging
kostet.

**Was Daidalos dafür heute fehlt** (nichts davon ist unmöglich, alles ist
Arbeit): Netzwerkschicht, Persistenz mit Schema-Migration (SQLite, wie Luanti),
Platzierungssystem mit serverseitiger Nachvalidierung, Inventar-Datenmodell.
Renderer, Physik, UI, Szenenformat und Export stehen bereits.

---

## 6a. Stand der Umsetzung (19.09.2026, JARVIS)

**M1 — erledigt, Ziel aber NICHT erreicht.** Commit `2269ca1`.
`build.sh` hat jetzt ccache davor und führt die Compiler-Aufrufe eines
Abschnitts parallel aus (`J` sammelt, `Jwait` ist die Barriere,
`tools/parallel.sh`). Alles, was ein Artefakt benutzt — `ar`, ein gelaufener
Test, ein Python-Schritt, der nächste Abschnitt — wartet vorher; die Ordnung,
für die `build.sh` argumentiert, ist unangetastet.

Gegen den Plan entschieden: **kein generiertes `build.ninja`.** Es wäre eine
zweite Beschreibung desselben Builds, und `build.sh` ist keine Befehlsliste,
sondern eine Argumentation mit einem Kommentar über jeder Zeile, die etwas
schützt (Leak-Test, "compile und run sind zwei Statements", `rm` vor
`ar rcs`). Ein Generator müsste das alles nachbilden und würde beim ersten
einseitigen Edit auseinanderlaufen.

Die erste Fassung von `tools/parallel.sh` drosselte über Hintergrund-Jobs und
machte den Build **langsamer** — 7:08 gegen 2:35 — bei 40–44 laufbereiten
Prozessen gegen ein Limit von 9. Grund: ein Aufruf ist nicht ein Prozess
(g++ forkt cc1plus, as, collect2; ein Aufruf mit sechs `.cpp` arbeitet sie
nacheinander ab). `xargs -P` begrenzt das, was wirklich begrenzt werden soll.

**[gemessen] 19.09.2026, alle drei Läufe unter derselben Fremdlast** (ein
anderer Chat hielt die Maschine bei Load 30–45, die Zahlen sind also
pessimistisch):

| Variante | Wall | CPU-Zeit |
|---|---|---|
| seriell, ohne ccache | **10:44** | 514 s |
| parallel + ccache (66 % Hits) | **7:37** | 392 s |
| parallel + ccache (heiß) | **5:58** | 357 s |

Das sind **−44 %**, nicht die im Plan erhofften <30 s. Der Grund ist
strukturell und war in der Rechnung des Plans nicht enthalten: **~50 der 98
Aufrufe kompilieren UND linken in einem Schritt** (`g++ tests/x.cpp $LIBS -o
build/x`). Die kann ccache grundsätzlich nicht cachen, und sie übersetzen ihre
Quellen bei **jedem** Lauf neu — deshalb bleibt die CPU-Zeit bei ~357 s
kleben, egal wie warm der Cache ist. Der nächste echte Schritt ist deshalb
nicht mold, sondern: **Tests gegen Objektdateien linken statt Quellen neu zu
übersetzen.** Das ist ein größerer Umbau als M1 selbst.

Ebenfalls offen geblieben: die mold-Messung (im Plan schon als offen
vermerkt). Sie lohnt erst, wenn die Compile-Phase wirklich klein ist.

**M2 — erledigt und wirksam.** `tools/shot_guard.py`, eingehängt in
`tools/run_tests.sh`. Prüft Byte-Duplikate (SHA-256), Fast-Duplikate über
einen 16×16-Average-Hash, leere/flache Bilder, Name↔Datei, und fährt bei
jedem Lauf einen **Canary**: eine absichtlich veränderte Kopie eines echten
Frames, die als verschieden erkannt werden MUSS. Keine Abhängigkeiten — der
PNG-Dekoder steht in der Datei.

Bewertet wird das **Viewport-Rechteck** (`--crop 184,150,840,463`), nicht das
ganze Bild: `modeling_shot` fotografiert den kompletten Editor, und ein Hash
über die Panels misst überwiegend den Inspector.

Am Tag der Einführung hat er **vier echte Lügen im Bestand** gefunden:

| Paar | Abstand |
|---|---|
| `wide-01-editor-start` ↔ `wide-02-editor-hover` | **0** von 256 Bits, 0,025 % der Bytes |
| `wide-01-editor-start` ↔ `wide-03-editor-gizmo-rotate` | 1 Bit |
| `02c-cube-formation` ↔ `03-viewport-conflict` | 2 Bits, 0,47 % der Bytes |
| `narrow-02c-cube-formation` ↔ `narrow-03-viewport-conflict` | 1 Bit |

Ein „hover"-Screenshot, der der „start"-Screenshot ist, heißt: das Hover hat
nie stattgefunden. Die Suite ist damit rot, wo sie vorher grün war
(`TOTAL 5983 passed, 2 failed` statt `1 failed`) — genau dafür ist die
Maßnahme da. Die drei alten Roten (`test_window_two` ohne X-Display,
`bridge_check`, `innen_gen_shot`) sind unverändert und nicht von M1/M2
verursacht.

**Als Nächstes:** die vier Bildpaare reparieren (die Shot-Skripte erzeugen
identische Frames), dann M3 (Layout-Assertions).

---

## 7. Reihenfolge

| Phase | Inhalt | Aufwand |
|---|---|---|
| **1** | M1 Build (ccache ✓ installiert, Ninja-Generator, mold messen) | 3–4 h |
| **2** | M2 shot_guard | 1 Tag |
| **3** | M3 Layout-Assertions + Stresstest | 3–5 Tage |
| **4** | M4 Replay + SyncTest, `-ffp-contract=off` für die Sim | 1–2 Wochen |
| **5** | Skript-Härtung + Entity-API auf 7 Hooks | 1 Woche |
| **6** | MCP: `layout`, `replay_*`, `shot_compare`, Kamera-Presets | 3 Tage |
| **7** | *Erst jetzt* das Spiel — und dann mit Gauntlet, das jetzt tatsächlich funktioniert | — |

Phase 1 und 2 zusammen sind zwei Tage und beheben die Ursache des
gescheiterten Gauntlet-Laufs. Alles danach baut darauf auf.

---

## Anhang: Gemessen am 19.09.2026 auf diesem Server

* Hardware: 20 Kerne, 19 GB RAM, g++ 12.2.0 (Debian)
* `g++ -O3 -c src/dai_engine.cpp`: **2,38 s**
* `ccache g++` kalt: 2,57 s · warm (Hit): **0,00 s**
* `build.sh`: **98** Compiler-Aufrufe, rein sequenziell, 501 Zeilen
* `src/` + `include/`: 66.843 Zeilen; Projekt gesamt ~233k Zeilen
* Installiert und konfiguriert: `mold 1.10.1`, `ccache 4.7.5` (20 G, depend_mode),
  `ninja`
* **Nicht** gemessen: mold-Linkzeit an diesem Projekt (Testlink scheiterte an
  fehlenden Symbolen — noch offen).
