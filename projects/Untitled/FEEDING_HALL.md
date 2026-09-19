# THE FEEDING HALL - in Daidalos

Nachbau des KFC-Fiebertraum-Clips als spielbare Szene. Alles ueber den
MCP-Server (`tools/dai_mcp.py`) gebaut, kein Editor-Klick.

## Dateien

| Datei | was |
|---|---|
| `scenes/feeding_hall.daiscene` | die fertige Szene, 339 Knoten |
| `../../examples/scripts/feeding_hall.js` | Bauskript - baut die Szene komplett neu |
| `assets/fh_game.js` | das Spiel (Behaviour auf dem Knoten `GameLogic`) |
| `assets/materials/fh_*.daimat` | 17 Materialien |
| `assets/textures/fh_*.png` | Kachel-, Bucket- und Schildtexturen |
| `boot.cfg` | startet die Szene im Runtime |

Szene neu bauen (MCP): `js files:["examples/scripts/feeding_hall.js"]`,
danach `save projects/Untitled/scenes/feeding_hall.daiscene`.

## Spielen

Szene oeffnen, **Ctrl+P**. Oder ohne Editor:
`build/daidalos_runtime projects/Untitled`

* **WASD** laufen bzw. fahren, **Shift** rennen bzw. Handbremse
* **Maus** umsehen, **Linksklick** einen Wing werfen
* **Enter** weiter, **R** Level neu

### Die drei Level

| # | Name | Pferde | Zeit | Besonderheit |
|---|---|---|---|---|
| 1 | THE FEEDING HALL | 12 | 3:00 | leere Halle, zum Warmwerfen |
| 2 | THE WET WING | 20 | 3:20 | vierzehn Kisten stehen im Weg |
| 3 | THE DRIVE-THRU | 28 | 4:00 | im Pickup, Lenkrad im Bild, Wings aus dem Fenster |

Ziel: alle Pferde treffen. Ein Treffer gibt Punkte (Combo bis x5) und zwei
Wings zurueck; gefuetterte Pferde bekommen einen leuchtenden Wing-Kranz und
folgen dir. Aus ist es bei 0:00, bei leerem Bucket oder wenn alle satt sind.

Felder am Knoten `GameLogic` (Inspector): Tempo, Maus-Empfindlichkeit,
Wurfgeschwindigkeit, Schwerkraft - dazu `startLevel` (1-3, praktisch zum
Testen) und `autoplay`, das den Selbsttest einschaltet: das Spiel spielt sich
dann allein, und genau so ist die Trefferlogik headless geprueft.

## Was beim Bauen gegen die Engine ging

1. `light.mode 0` ist **kein** Licht. Punkt 1, Spot 2, Sonne 3.
2. Ein Knoten ohne Blockout wird trotzdem als 1-m-Wuerfel gezeichnet -
   Gruppen, Lichter und der Manager brauchen `renderer.enabled 0`.
3. ARRAY-Modifier zeichnen in einer kleinen Testszene, in dieser Szene kamen
   die Instanzen nicht an. Boden und Waende laufen deshalb ueber Texturen,
   Saeulen und Neonroehren sind echte Knoten.
4. `text.value` rendert im `modeling_shot` nicht, und eine Schrift-Textur
   verschmiert, weil Triplanar in **Welt**koordinaten projiziert: auf einer
   Flaeche landet irgendein Ausschnitt der Kachel. Der Schriftzug besteht
   darum aus Balken in einem 5x7-Raster (`writeWord` im Bauskript).
5. Behaviours sprechen `node.setPos/setRot(id, ...)` und `scene.find` liefert
   eine **id**, kein Objekt. Vorwaerts ist **-Z** bei yaw 0.
6. **Eine bewegte Gruppe liess ihre Kinder stehen.** `dai_doc_sync` wendet nur
   Knoten an, deren Revision sich geaendert hat - die eines Kindes aendert
   sich nicht, wenn der Vater umzieht. Gefixt in `src/dai_doc_sync.cpp` (ein
   bewegter Vater macht seinen Teilbaum mit dirty) und in den beiden Hosts,
   die einen Knoten MIT Kindern jetzt auch ins Dokument schreiben statt nur in
   die Szene. Vorher haette der Pickup seine Raeder stehen lassen.

## Ton

Echte Aufnahmen, keine Sinustoene: 13 WAVs aus Wikimedia Commons (public
domain / CC0), geschnitten und auf 48 kHz Mono normalisiert. Herkunft und
Lizenz je Datei in `assets/audio/HERKUNFT.md`. Die Bank ist
`assets/audio/feeding_hall.json` (Aulos): Wurf, Treffer, Platscher, Wiehern
(zwei Varianten, zufaellig), Fressen, Schritte (drei Varianten), Munition,
Level-Fanfare, Applaus und eine Hallen-Schleife aus vorbeigaloppierenden
Pferden. Raeumlich, mit Rolloff - das Wiehern kommt aus der Richtung des
Pferdes, weil das Skript pro Frame `audio.listener(...)` setzt.

Gehoert wird es so:

```
build/daidalos_runtime projects/Untitled --headless 900 --audio-dump mix.wav
```

Gemessen am 16.09.2026: 15 s Mix, Spitzenpegel 0.84, 40 unterscheidbare
Ereignisse, Kanaldifferenz 0.068 - der Ton ist also wirklich da und wirklich
raeumlich.

## Offen

~~**Der Runtime faerbt nichts.**~~ `build/daidalos_runtime projects/Untitled`
startet die Szene, das Skript laeuft, die Physik laeuft - aber
`examples/runtime_main.cpp` liest das `materials`-Feld eines Knotens nicht
(nur glTF-Modelle bringen Materialien mit). Blockout + `.daimat` ist damit
heute reines Editor-Feature, und der exportierte Build ist grau. Im Editor
(Ctrl+P) stimmen die Farben.

**Erledigt am 16.09.2026** - der Runtime zieht jetzt dieselben Seams wie der
Editor (`dai_material_host.inl`, `dai_blockout_host.inl`), sammelt die
Lichtkomponenten aus dem Dokument und faerbt die Szene richtig. Dazu kamen
drei Loecher zum Vorschein:

1. Ohne `dai_blockout_host.inl` war jede Box, jeder Zylinder und jeder
   CSG-Schnitt im Runtime der Standardwuerfel.
2. Die Lichter der Szene wurden nie eingesammelt - die Halle lag unter der
   Default-Sonne.
3. **Der Cull-Kasten**: ein Blockout-Knoten behaelt `half_extent` 0.5 m, wenn
   ihn kein Editor-Widget angefasst hat. Die Szene wird danach gecullt, also
   verschwand der 26x52-m-Boden, sobald sein Mittelpunkt aus dem Bild lief -
   Waende (Mittelpunkt im Bild) blieben sichtbar. `rt_fix_blockout_extents()`
   nimmt den Kasten jetzt beim Laden aus `blockout.size`.

Geprueft am 16.09.2026: `./build/daidalos_runtime projects/Untitled --headless 3600`
mit `autoplay` faettert 11 von 12 Pferden - Wurfbahn, Treffer, Folge-KI,
Punkte und Nachschub arbeiten also wirklich.

## Was an der Engine geaendert wurde

| Datei | Aenderung |
|---|---|
| `include/dai_script.h`, `src/dai_script.cpp` | `audio` fuer Behaviours: `play`, `play3d`, `stop`, `listener` (`dai_script_bind_audio`) |
| `include/dai_audio.h` | `dai_audio_open/close/update/listener/render/voices` sind jetzt oeffentlich - ein Spiel ist ein Host wie jeder andere |
| `include/dai_vfs.h`, `src/dai_vfs.cpp` | `audio_bank` in boot.cfg |
| `examples/runtime_main.cpp` | Material- und Blockout-Seam, Lichter aus dem Dokument, Cull-Kasten-Reparatur, Audio-Backend, `--audio-dump` |
| `examples/editor_demo.cpp` | Ton auch im Editor: beim ersten Ctrl+P wird die erste `.json` in `assets/audio` als Bank geoeffnet |

Der Aulos-Teil braucht `libaulos.a`: `bash /root/projects/aulos/build.sh`,
danach `./build.sh` (sonst baut sich die Engine mit `-DDAI_NO_AUDIO` und
bleibt stumm) und `tools/build_runtime.sh linux`.


## Ton: Master-Limiter statt Uebersteuerung (17.09.2026)

Der erste vollstaendige Autoplay-Lauf ueber alle drei Level (30 000 Frames,
500 s Offline-Mix) hatte **Peak 2.47** - also 8 dB ueber Vollaussteuerung. Ein
Treffer loest vier Sounds gleichzeitig aus (wing_hit, horse_feed, horse_neigh,
ammo_pickup), und bei einer Combo liegen mehrere Treffer in derselben
Zehntelsekunde. Zwei Aenderungen:

1. **Aulos bekam einen Master-Limiter** (`src/aulos.cpp`, `aulRenderBlock`):
   Peak-Follower mit 1 ms Attack und 250 ms Release auf ein Ceiling von 0.97,
   Zustand `aul_system::limGain`. Er sitzt vor der Peak-Statistik, gilt also
   fuer Geraete-Ausgabe UND Offline-Dump - ein Host kann nicht mehr clippen,
   egal wie viele Stimmen er startet. Aulos-Tests: 46 passed, 0 failed.
2. **Sound-Bremse im Spiel** (`assets/fh_game.js`): jedes Event darf nur alle
   80 ms neu anfangen (`sndOk`), und der Treffer-Stapel wurde gedaempft
   (hit 0.8, feed 0.7, neigh 0.55, pickup 0.35, throw 0.7, step 0.5).

**Gemessen danach**, gleicher Lauf: Peak **0.450**, **0 geclippte Samples**,
max |L-R| 0.44 (echtes Stereo), alle drei Level durchgespielt.


## Im Editor wirklich gespielt (17.09.2026)

Bis hierher war "Ctrl+P im Editor" nur behauptet - ein Fenster gab es auf dem
Server nicht. Jetzt schon: **Xvfb + matchbox-window-manager**, Editor darin,
Eingaben ueber xdotool, Bilder ueber ffmpeg x11grab, HUD-Text per OCR gelesen.

Drei Dinge, die dabei im Weg standen:

1. **Der Editor kennt `projects/<p>/scenes/*.daiscene` nicht mehr.** Er laedt
   `assets/Scenes/*.daidalos` (main.daidalos, sonst alphabetisch die erste) und
   ignoriert dabei `default-scene` aus `settings/project.txt` und das
   Kommandozeilen-Argument - die Hauptschleife setzt die Projektszene sofort
   wieder. Die Szene liegt jetzt als
   `projects/Untitled/assets/Scenes/feeding_hall.daidalos`, der Runtime nimmt
   weiter `scenes/feeding_hall.daiscene` ueber boot.cfg.
2. **Ohne Window-Manager kommt keine Taste an.** Ohne WM gibt es kein
   _NET_ACTIVE_WINDOW, `xdotool windowfocus` allein reicht dem Fenster nicht,
   und Ctrl+P verpufft. Mit matchbox laeuft es.
3. **Der Editor updatet sich beim Start selbst** (daidalos.fleitec.com) und
   ersetzt das frisch gebaute Binary beim Schliessen. Zum Testen eines eigenen
   Builds: `unshare -rn` davor - kein Netz, kein Update.

**Was gesehen wurde:** Titelkarte "THE FEEDING HALL / WIRF HOT WINGS AUF JEDES
PFERD", HUD mit `0/12` und `TIME 03:00`, nach Enter die Ego-Sicht mit SCORE,
W bewegt die Kamera, Klicks werfen. Der Server rendert mit 2 fps (SwiftShader,
keine GPU) - das ist die Software-Emulation, nicht das Spiel.

Reproduzieren:
```
Xvfb :78 -screen 0 1600x900x24 &
DISPLAY=:78 matchbox-window-manager -use_titlebar no &
unshare -rn env DISPLAY=:78 DAI_SHADER_DIR=shaders ./build/editor_demo
DISPLAY=:78 xdotool search --name Daidalos windowactivate --sync %1 key ctrl+p
```


## Als eigenstaendiges Spiel exportiert - mit Ton (17.09.2026)

`dai_pack export` macht aus Projekt + Runtime EINE Datei. Beim ersten Versuch
war die Datei stumm, und zwar aus drei unabhaengigen Gruenden - alle gefixt:

1. **Der Windows-Runtime wurde grundsaetzlich ohne Audio gebaut**
   (`-DDAI_NO_AUDIO` in `build_win.sh` und `tools/build_runtime.sh`, mit der
   Begruendung "Aulos ist ein eigenes Projekt"). Aulos ist jetzt fuer mingw
   cross-gebaut (`/root/projects/aulos/build-win/libaulos.a`, miniaudio nutzt
   WASAPI), und beide Skripte linken ihn, sobald das Archiv da ist
   (`-lole32 -lwinmm -lavrt -lksuser`).
2. **Aulos konnte nur vom Dateisystem lesen.** Ein exportiertes Spiel hat keine
   `assets/audio/`-Ordner, nur Archiv-Eintraege - der Runtime hat deshalb
   "audio bank skipped" geloggt und still gespielt. Aulos hat jetzt
   `aul_set_reader(sys, read, release, user)`: der Host liefert Bytes zu einem
   Pfad, die Bank wird daraus geparst und Samples gehen ueber
   `ma_decoder_init_memory` statt `ma_decoder_init_file`. Daidalos reicht das
   mit `dai_audio_open_reader(...)` durch, der Runtime haengt `dai_vfs_read`
   dran.
3. **`dai_pack` hat `audio_bank` nie in die exportierte boot.cfg geschrieben.**
   Es baute die Konfiguration nur aus `settings/project.txt`, und das kennt
   keinen Ton. Jetzt liest es zuerst die `boot.cfg` des Projekts (Bank, Titel,
   Szene, Fenstermasse, MSAA), Kommandozeile gewinnt weiterhin.

**Gemessen am exportierten Spiel** (`FH-auto`, Autoplay ueber alle drei Level,
9 000 Frames): `audio: bank 'assets/audio/feeding_hall.json' out of the archive`,
150 s Mix, **Peak 0.456, 0 geclippte Samples, max |L-R| 0.40**.

Bauen:
```
tools/build_runtime.sh linux      # oder: windows
build/dai_pack export projects/Untitled build/daidalos_runtime /tmp/FH
build/dai_pack verify /tmp/FH
```


## Der erste echte Spieltest auf Windows - vier Fehler (17.09.2026)

Justin hat die exportierte .exe gestartet: Halle in Zufallsfarben, kein HUD,
WASD tot, Mauszeiger sichtbar und frei. Vier getrennte Fehler, alle IM
RUNTIME - im Editor lief dasselbe Spiel richtig, was sie so lange versteckt
hat.

1. **WASD tot.** `sp_key` in `examples/runtime_main.cpp` rechnete einen
   Buchstaben in GROSSschrift um (`'w'` -> `'W'`), die Fensterbackends legen
   ihn klein ab (`dai_key_from_vk`: `'A'..'Z'` + 0x20). `dai_window_key_down`
   bekam also einen Code, den kein Fenster je setzt - konstant 0. Der Editor
   fragt klein, deshalb ging es dort.
2. **Alles in Zufallsfarben.** `rt_apply_materials` begann mit
   `if (g_project_dir.empty()) return;` - und leer ist es genau dann, wenn das
   Spiel aus seinem eigenen Archiv laeuft. Materialien wurden also NUR im
   Dev-Lauf angewandt. Jetzt liest der Materialpfad wahlweise aus einem Ordner
   oder ueber einen Reader aus dem Archiv: `dai_ext_host.read/release`,
   `dai_material_host_load_tex`, `dai_matfile_from_text` und das neue
   `dai_render_texture_load_memory`.
3. **Kein HUD.** Der Runtime band `nodes`, `play` und `audio` an die Skripte,
   aber nie `gui` - jedes `gui.text()` lief ins Leere. Jetzt gibt es denselben
   Sammel-und-abspielen-Weg wie im Editor (`RtGuiCmd`, `dai_script_bind_gui`),
   gezeichnet in `draw_hud`.
4. **Maus weder versteckt noch gefangen.** Es gab schlicht kein API dafuer.
   Neu: `dai_window_mouse_capture / _captured / _delta` fuer X11 (leerer
   Cursor, `XGrabPointer`, Warp in die Mitte) und Win32 (`ShowCursor`,
   `ClipCursor`, `SetCursorPos`, Freigabe bei `WM_ACTIVATE`), Wayland meldet
   ehrlich 0. Der Runtime faengt die Maus beim Start, **Escape** gibt sie
   frei, ein Klick ins Bild holt sie zurueck, Escape mit freier Maus beendet.

**Nachgemessen im Fenster** (Xvfb, exportierte Datei): Bodenzeile wechselt
gelb (172,153,75) und pink (177,75,122) - das Schachbrett ist da; HUD liest
sich als `LEVEL 1 - THE FEEDING HALL`, `FEED ALL HORSES 0/12`, `TIME 02:52`,
`SCORE 0`; W aendert das Bild um 38.4 gegen 11.0 im Leerlauf, A um 45.8;
Mausbewegung um 52.3.
