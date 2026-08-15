# Skybrush Studio for Blender — was es kann, und was uns davon fehlt

Stand 15.08.2026. Quelle ist die offizielle Doku unter
`docs.skybrush.io/public/skybrush-studio-for-blender/latest/` (die alte Adresse
`doc.collmot.com` leitet per 301 dorthin) und der Quelltext in
`github.com/skybrush-io/studio-blender`. Wo eine Zahl steht, steht sie so im
Code oder in der Doku; wo nichts zu belegen war, steht das ausdrücklich dabei.

Dieses Dokument ist kein Wunschzettel. Es ist eine Bestandsaufnahme mit einer
unbequemen Botschaft: **Skybrush' Panel-Liste ist länger als sein funktionaler
Kern.** Ein guter Teil davon existiert nur, weil das Addon in Blender lebt
(Collections, Vertex-Groups, Constraints, Outliner-Reihenfolge), und ein
weiterer Teil, weil dahinter eine Firma mit Kundenverträgen steht (elf
Fremdformate, Pyro, Lizenzstufen). Beides ist für uns kein Ziel.

Was wirklich zählt, steht in Stufe 1 und 2.

---

## 0. Die Abgrenzung zuerst, sonst wird die Liste unehrlich

Skybrush ist **vier Programme**, nicht eines. Wer „das Addon" sagt und die
Feature-Liste der Suite meint, vergleicht sich mit dem Falschen.

| Programm | Aufgabe | Für uns relevant? |
|---|---|---|
| **Studio for Blender** | das Design-Frontend: Panels, Storyboard, Licht | **ja, das ist der Vergleich** |
| **Studio Server** | rechnet Transitions, Export, Safety-Report | ja — *aber wir haben das lokal* |
| **Skybrush Live** | Bodenstation: Telemetrie, Preflight, RTK, Geofence im Feld | nein, anderes Produkt |
| **Skybrush Viewer** | .skyc anschauen, Validierungsdiagramme | teilweise (wir haben eine Vorschau) |

Der wichtigste Satz der ganzen Recherche: **das Blender-Addon allein ist keine
funktionsfähige Showsoftware.** Transition-Planung und Export laufen auf dem
Studio Server, nicht im Addon. Der Hersteller sagt selbst, warum: „transition
planning runs online in our servers so we can tweak and enhance the algorithms
without having to update the plugin".

Seit 01.03.2025 gilt für den kostenlosen Community-Server:

* **maximal 64 Drohnen** pro Design-Request
* reduzierte Rechenzeit pro Request
* abgeschaltet: SVG-Formationen, QR-Formationen, Yaw-Steuerung, Smart-RTH

Das ist der Punkt, an dem unsere Architektur kein Nachbau mehr ist, sondern
eine andere Antwort: **bei uns rechnet alles lokal, ohne Server, ohne Limit.**
Gemessen: 10.000 Drohnen komplett durchgerechnet in 8,16 s.

---

## 1. Der volle Funktionsumfang des Addons

Fünf Tabs, elf Panels.

| Tab | Panels |
|---|---|
| Skybrush | Setup, Show, Swarm, Drone Groups |
| Formations | Formations, Storyboard |
| LEDs | LED Control, Light Effects |
| Pyro | Pyro Control |
| Safety & Export | Safety Check, Export |

### 1.1 Show / Swarm
Show type `Outdoor`/`Indoor` (stellt Drohnengröße und verfügbare Exporter um),
Latitude/Longitude/Orientation des Show-Origins, Drone collection, Drone
template (Sphere/Cone/Selected Object), Drone radius, **Preferred
acceleration** (Eingang der automatischen Bahnplanung), Create Takeoff Grid.
Framerate ist die Blender-Szenenrate, kein eigenes Feld.

### 1.2 Drone Groups
Benannte Untermengen der Flotte: anlegen, löschen, aus der Auswahl befüllen,
im Viewport selektieren, leeren. Wird von Light Effects als Filter benutzt.

### 1.3 Formations
Flugphasen-Operatoren — **hier liegen Takeoff, Land und RTH**, nicht in
eigenen Panels:

* **Create Takeoff Grid** — Rows, Columns, Drohnenzahl, Spacing, optional
  getrenntes Column-Spacing und Slot-Grids mit mehreren Drohnen je Slot.
* **Takeoff** — Zielhöhe, mittlere Vertikalgeschwindigkeit, Startframe,
  Spacing, Layer height. Unterschreitet die Aufstellung das Spacing, schaltet
  der Operator **automatisch auf geschichteten Start** und optimiert die
  Layer-Reihenfolge gegen Downwash.
* **RTH** — mittlere Geschwindigkeit, Spacing, Layer height; „Smart RTH"
  (jede Drohne an ihren eigenen Startplatz, Pro-Lizenz) und „Return to aerial
  grid" (Schwebegrid statt Landung).
* **Land** — Zielhöhe, mittlere Sinkgeschwindigkeit, Spacing, Motor-Spindown-
  Delay zwischen den Layern.

Formationsverwaltung: anlegen aus `Empty` / `Current positions of drones` /
`Selected objects` / `Current positions of selected objects` / `Current
positions of selected vertices`; **Generate Markers** aus CSV, gezippter
CSV-Animation, PATH/PATH3, SVG, QR-Code; **Update**; **Reorder** (Sort by name,
Shuffle, Reverse, Sort by X/Y/Z, Every 2nd/3rd/4th, **Ensure safety
distance**); **Stats** (Markerzahl, Bounding Box, minimale Abstände);
**Append to Storyboard** mit automatisch berechneter Transitionsdauer.

### 1.4 Storyboard
Je Eintrag: Formation, Start Frame, Duration, End Frame, **Purpose**
(`Takeoff`/`Show`/`Land`/`Unspecified`), Custom Name, **Locked**.

Transition je Eintrag, getrennt für „from previous" und „to next":
Type `Auto`/`Manual`, Profile `Linear`/`Smooth`/`Smooth from left`/`Smooth from
right`, Schedule `Synchronized`/`Staggered` mit **Departure delay** und
**Arrival delay**, **Schedule overrides** (Timing einzelner Marker von Hand
verschieben, um Abstandskonflikte aufzulösen), **Recalculate** für genau diese
eine Transition.

Global: **Recalculate Transitions** für alle nicht gesperrten Einträge,
**Update Time Markers** (Timeline-Marker mit dem Storyboard synchronisieren).

Einträge dürfen sich **nicht überlappen**. Die Kollisionsfreiheit einer
Transition ist auf **ca. 0,7 × D** garantiert (D = Formationsabstand) — und
nur, „if the source and target formations are sparse enough".

### 1.5 LED Control
Visualization `None`/`Markers`/`Materials`, Marker size, Primary/Secondary
colour mit Swap, **Apply** (harte Farb-Keyframes am aktuellen Frame),
**Fade to Color** (lineare Interpolation vom vorherigen Keyframe), Color to
apply `Primary`/`Secondary`/`Gradient`, Order in gradient
`Default`/`Random`/`X`/`Y`/`Z`/`Distance from 3D cursor`, Bloom-Effekt
(entfällt ab Blender 4.3).

### 1.6 Light Effects — das mit Abstand mächtigste Panel
Ein **Effekt-Stack**, der pro Frame live über die Basisfarbe gerendert wird.
Priorität = Reihenfolge im Stack, sequentielles Alpha-Compositing.

* **3 Farbquellen:** Color Ramp (Interpolation in RGB/HSV/HSL), Color Image
  (2D, beide Achsen getrennt gemappt), Custom Function (Python, gibt RGBA).
* **Output-Mapping** (was eine Drohne auf 0…1 abbildet): First color, Last
  color, **Indexed by drones**, **Indexed by formation**, Gradient in sechs
  Achsenreihenfolgen (XYZ…ZYX), **Temporal**, **Distance from mesh**, Light
  preset, **Custom expression**, Custom expression (vektorisiert, NumPy).
  Dazu der Untermodus **Ordered** gegen **Proportional**.
* **Räumliche Filter:** `All drones` / `Inside the mesh` / `Front side of
  plane`, jeweils invertierbar, plus Beschränkung auf eine Drone Group.
* **Zeit:** Start frame, Duration, End frame, **Attach to** (Timing an einen
  Storyboard-Eintrag koppeln, mit Offset), Fade in, Fade out.
* **Mischen:** Influence 0…1 (keyframe-animierbar), 8 Blend-Modi (Normal,
  Multiply, Screen, Darken, Lighten, Overlay, Hard Light, Soft Light),
  Randomness (±0,5 pro Drohne, modulo 1).
* Import/Export einzelner Effekte als Bibliothek.

Signatur der Custom-Funktion:

```python
def color_function(frame, time_fraction, drone_index,
                   formation_index, position, drone_count):
    return (1.0, 1.0, 1.0, 1.0)   # RGBA
```

`time_fraction = (frame - frame_start) / max(duration - 1, 1)`.

**Skybrush rechnet durchgehend RGB, nicht RGBW.** Die Umsetzung auf einen
RGBW-Treiber passiert erst in Firmware/Backend. `libskybrush` kennt nur
`sb_rgb_color_t`.

### 1.7 Pyro
Kanäle ab 1, ein Event je Kanal und Drohne, Name, Duration (nur
Visualisierung), **Prefire time** (Zündverzug), Yaw, Pitch, Trigger.
Visualisierung `None`/`Markers`/`Particles`/`Info`.

### 1.8 Safety Check — Defaults aus dem Quelltext

| Größe | Property | Default |
|---|---|---|
| Mindestabstand | `proximity_warning_threshold` | **3,0 m** |
| Zielmenge | `proximity_warning_target` | `ABOVE_MIN_NAV_ALT` |
| Max. Höhe | `altitude_warning_threshold` | **150 m** |
| Min. Navigationshöhe | `min_navigation_altitude` | **2,5 m** |
| Max. Horizontalgeschwindigkeit | `velocity_xy_warning_threshold` | **10 m/s** |
| Max. Vertikalgeschwindigkeit | `velocity_z_warning_threshold` | **2 m/s** |
| ...getrennt aufwärts | `velocity_z_warning_threshold_up` | **2 m/s** |
| Max. Beschleunigung | `acceleration_warning_threshold` | **4 m/s²** |
| Max. Gierrate | `yaw_rate_warning_threshold` | **30 °/s** |

Anzeige: Text-Overlay links im Viewport, farbcodierte Marker und
Verbindungslinien. Rot = Proximity, Blau = Höhe, Gelb = Geschwindigkeit,
Magenta = Beschleunigung, Cyan = Gierrate.

**Wichtig:** in Echtzeit wird aus Rechengründen nur **ein** Proximity-Paar pro
Frame markiert. Der volle Scan ist ein eigener Button („Calculate All Proximity
Warnings"), ausdrücklich als teure Operation gekennzeichnet.

**Nicht vorhanden im Addon**, entgegen einer verbreiteten Annahme: ein
**Geofence** (das gehört zu Skybrush Live und entsteht dort aus der konvexen
Hülle der Trajektorien plus Margins), ein **Bodenabstands-Check** und ein
**Neigungswinkel-Check**.

`Validate Trajectories` sampelt mit **4 fps** über den Frame-Bereich, schickt
das an den Studio Server und öffnet das Ergebnis als Diagramme im Skybrush
Viewer. Dieser Operator erzeugt **kein** PDF — das ist ein eigener Export.

### 1.9 Export

`.skyc`, `.csv` (ZIP, je Drohne eine Datei), **Validierungsreport `.pdf`**,
`.skyc + .pdf` in einem Durchgang (sampelt nur einmal), Google Earth `.kmz`,
Finale 3D `.vviz`, FWSim `.vviz`, Depence `.ddsf`, und mit Pro-Lizenz DSS PATH,
DSS PATH3, Drotek, EVSKY, HighGreat DAC, LiteBee.

Optionen: nur ausgewählte Drohnen, Frame-Range (`Storyboard`/`Render`/
`Preview`/`Current formation or transition`), **Trajectory FPS** und **Light
FPS 
getrennt** (Default je 4; Empfehlung 4–5 für Bahnen, 24–30 für Licht),
Redraw-Verhalten, Yaw-Export (Pro), **Audio-Export** (MP3 aus dem Video
Sequence Editor), **Kamera-Export**.

PDF-Plots: positions, velocities, accelerations, yaw und yaw rate, projected
drift, nearest neighbour, all nearest neighbours, individual drone plots.

Farbe im Export: 8 Bit je Kanal, Zeitstempel auf 3 Nachkommastellen (also
Millisekunden), Vereinfachung der Keypoints mit **Epsilon 4/255**,
JSON-Form `{"data": [[t, [r,g,b], fade]], "version": 1}`.

---

## 2. Was wir davon haben — und was nicht

Legende: **✅** vorhanden · **◐** teilweise · **❌** fehlt · **⊘** bewusst nicht

### Stufe 1 — Pflicht, fehlt oder ist zu dünn

| Skybrush | Wir | Was zu tun ist |
|---|---|---|
| **Light Effects Stack** (3 Quellen, 10 Mappings, 8 Blend-Modi, Fades, Masken) | ❌ nur statische Punktfarbe | **Die mit Abstand größte Lücke.** Ohne animiertes Licht ist es keine Showsoftware, sondern ein Bahnplaner. |
| **Create Takeoff Grid** | ❌ | Rows/Columns/Spacing, erzeugt Formation 0. Kleine Arbeit, große Wirkung. |
| **Takeoff / Land / RTH** mit automatischem Layering | ❌ | Eine Show ohne Start und Landung ist keine Show. Das Layering bei zu engem Grid ist der eigentliche Sicherheitshebel. |
| **Musik** und Zeitachse dazu | ❌ | Aulos ist im Haus. Ohne Ton kein Timing. |
| **Trajectory FPS ≠ Light FPS** | ❌ ein `fps` für alles | Trennen. Bahnen 4–5 Hz, Licht 24–30 Hz — sonst wird die Datei unnötig groß oder das Licht ruckelt. |
| **Proximity scope** (`All` vs `Above min alt`) | ❌ | Ohne das meldet unsere Validierung jedes dichte Startraster als Kollision. Direkte Folge von Takeoff. |
| Min. Navigationshöhe | ❌ | Gehört zum selben Thema. |
| Getrennte Limits auf/ab | ❌ ein `v_max_ms` | Steigen und Sinken sind physikalisch nicht dasselbe. |

### Stufe 2 — Sicherheit und Nachweis, teilweise da

| Skybrush | Wir | Anmerkung |
|---|---|---|
| Mindestabstand, v_max, a_max, Höhenlimit | ✅ | Wir prüfen zusätzlich **Bodenabstand** und einen **echten Geofence** — beides hat das Addon nicht. |
| Proximity-Prüfung | ✅ **besser** | Skybrush markiert live nur **ein** Paar je Frame und braucht für den Rest einen eigenen Button. Wir prüfen die **gesamte Zeitachse** mit Broadphase, gesweept zwischen den Ticks, und melden alles sortiert. |
| Yaw und Gierrate | ❌ | Wir haben keine Gier-Achse. Für eine Showdrohne mit isotroper Kugel-LED ist das verschmerzbar — für gerichtete Optik nicht. |
| **Validierungsreport als PDF** | ❌ | Für die Diplomarbeit fast schon Pflicht: Flugstatistik, Höhen-, Geschwindigkeits-, Abstandsdiagramme. Wir haben alle Zahlen, es fehlt nur die Ausgabe. |
| Verstoß anklickbar, im Viewport markiert | ✅ | Rote Ringe, Verbindungslinie, Label, Sprung auf den Moment. |

### Stufe 3 — Komfort, später

| Skybrush | Wir |
|---|---|
| Marker-Import aus CSV / SVG / QR / PATH | ❌ (wir sampeln aus dem Mesh — mächtiger, aber anders) |
| Reorder: Shuffle, Reverse, Sort by X/Y/Z, Ensure safety distance | ❌ |
| Drone Groups | ❌ (Voraussetzung für gezielte Lichteffekte) |
| Formation Stats (Bounding Box, Markerzahl, min. Abstand) | ◐ min. Abstand ✅, Rest ❌ |
| Storyboard-Eintrag: Purpose, Locked, Custom Name | ❌ |
| Schedule overrides je Marker | ❌ |
| Transition einzeln neu rechnen | ❌ (wir rechnen immer alles) |
| Light-Effekte als Bibliothek im- und exportieren | ❌ |
| Kamera-Export in die .skyc | ❌ |

### Stufe 4 — bewusst nicht

| Skybrush | Warum nicht |
|---|---|
| Pyro | Eigenes Gewerk, eigene Zulassung. Nicht in einer Diplomarbeit. |
| 11 Fremdformate (DSS, Drotek, LiteBee, EVSKY, HighGreat…) | Wir fliegen ArduPilot über `.skyc`. Alles andere ist Vertriebsarbeit. |
| Finale 3D `.vviz`, Depence `.ddsf`, Google Earth `.kmz` | Beide importieren ohnehin Skybrush-Ausgaben. Kein eigener Wert. |
| Cloud, Gateway, Lizenzstufen | Wir haben keinen Server, den man umgehen müsste. |
| Blender-Eigenheiten (Collections, Vertex Groups, Constraints, Outliner-Reihenfolge) | Existieren nur, weil das Addon in Blender lebt. Wir sind der Editor. |

---

## 3. Was wir haben und Skybrush nicht

Das gehört in die Arbeit, sonst liest sie sich wie ein Nachbau.

* **Kein Server, kein Limit.** Transition-Planung und Export laufen bei uns
  lokal. Die freie Skybrush-Variante hört bei **64 Drohnen** auf; wir haben
  10.000 gemessen (8,16 s für die ganze Kette).
* **Determinismus als Zusage.** `state(n+1) = step(state(n), input(n))` — keine
  Uhr, kein ungeseedeter Zufall, feste Reduktionsreihenfolge. Sagt das Werkzeug
  „kollisionsfrei", ist diese Antwort auf jedem Rechner dieselbe. Das ist bei
  einer Sicherheitsaussage kein Stil, sondern die Voraussetzung dafür, dass sie
  etwas wert ist.
* **Exakte Zuordnung lokal.** Jonker-Volgenant bis 2.000 Drohnen, darüber ein
  räumlich zerlegter Auktionsalgorithmus — mit gemessenem Abstand zum Optimum
  (`gap_percent`), statt „optimal" zu behaupten.
* **Validierung über die ganze Zeitachse**, gesweept zwischen den Ticks, nicht
  nur an Keyframes. Skybrush' Echtzeitprüfung ist ein Frame-Sample.
* **Ehrliche Restliste.** Was der Separator nicht auflösen konnte, wird als
  `unresolved` gezählt und benannt, statt hinter „garantiert, wenn dünn genug"
  zu verschwinden. Formationsfehler werden getrennt gezählt, weil sie an einer
  anderen Stelle zu reparieren sind.
* **Silhouetten-Sampling.** Punkte auf die Kontur aus Publikumsrichtung — genau
  das, was eine Figur am Nachthimmel lesbar macht. Skybrush hat Surface und
  Volume, aber nichts Vergleichbares.
* **Machbarkeitsprüfung vor dem Sampeln.** `max_points`, `required_scale`,
  `achievable_spacing` — die Antwort auf „passen 500 Drohnen in dieses Herz"
  kommt, **bevor** gerechnet wird.
* **Eigener Editor.** Kein Blender nötig, kein Addon zu installieren, ein
  Binary. Und derselbe Kern, der Spiele rendert.

---

## 4. Empfohlene Reihenfolge

1. **Takeoff-Grid + Takeoff + Land** samt automatischem Layering, dazu
   `proximity_scope` und Mindest-Navigationshöhe. Ohne das ist keine Show
   vollständig, und ohne `proximity_scope` meldet die Validierung Unsinn.
2. **Light Effects.** Erst Farbrampe über die Zeit, dann Output-Mappings
   (Index, Gradient, Temporal, Distance), dann Blend-Modi und Fades. Das ist
   die größte Lücke und der sichtbarste Fortschritt.
3. **Getrennte Licht- und Bahn-Abtastrate** beim Export.
4. **PDF-Validierungsreport.** Wir haben die Zahlen; es fehlt die Ausgabe. Für
   die Abgabe die billigste große Wirkung.
5. **Musik** über Aulos, mit der Zeitachse gekoppelt.
6. Danach Komfort: Drone Groups, Reorder, Purpose/Locked, Formation Stats.

**Nicht** anfangen mit Fremdformaten, Pyro oder Yaw. Das sieht nach viel aus
und bringt für eine eigene Flotte nichts.

---

## Quellen

* <https://docs.skybrush.io/public/skybrush-studio-for-blender/latest/>
  (Panels, Concepts, Glossary, Export, Safety Check, Light Effects, Install)
* <https://github.com/skybrush-io/studio-blender> — insbesondere
  `src/modules/sbstudio/plugin/model/safety_check.py`,
  `src/modules/sbstudio/plugin/model/light_effects.py`,
  `src/modules/sbstudio/math/colors.py`,
  `src/modules/sbstudio/model/light_program.py`,
  `src/modules/sbstudio/plugin/utils/sampling.py`,
  `src/modules/sbstudio/plugin/operators/validate_trajectories.py`,
  `CHANGELOG.md`
* <https://github.com/skybrush-io/libskybrush> — `include/skybrush/lights.h`
* <https://skybrush.io/blog/2025-02-04-skybrush-store/> — Community-Limits
* <https://doc.collmot.com/public/skybrush-live-doc/latest/dialogs/geofence.html>
  — Geofence gehört zu Live, nicht zu Studio
* <https://www.droneshowsoftware.com/> — SPH Engineering, der einzige echte
  Vollkonkurrent
* <https://finale3d.com/documentation/importing-drone-shows/> — „Finale 3D is
  not used to design drone animations"
