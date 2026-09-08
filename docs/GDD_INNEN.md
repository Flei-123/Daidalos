# INNEN — Game Design Document (v0.1, 08.09.2026)

> Arbeitstitel: **INNEN** („Es ist größer als du.")
> Genre: First-Person Psycho-Horror / Exploration, prozedural, Single-Player, 4–8 h.
> Engine: Daidalos (eigene C++-Engine, Modul-Prefabs + Shader-Portale).
> Rechtliches: KEIN Doctor-Who-Bezug. Kein Name „TARDIS", keine Police-Box-Optik, keine Time Lords. Die Kabine ist eine rostige österreichische **Post-Telefonzelle** (gelb/grau, Baujahr ~1978). Das Konzept „innen größer als außen" ist frei.

---

## 1. Elevator Pitch

Nachts, Passstraße in Tirol, Schneetreiben. Du fährst ins Bankett, Auto Schrott, kein Empfang. 200 m weiter steht eine alte Telefonzelle mit Licht. Du gehst rein, um Hilfe zu rufen. Die Tür fällt zu. Hinter dir ist kein Pass mehr — sondern ein Flur. Und der Flur ist aus deinem Elternhaus.

Die Zelle ist keine Zelle. Sie ist ein **gestrandetes Gehäuse**, ein Fahrzeug, dessen Inneres aus den Erinnerungen der Leute gebaut ist, die es je betreten haben. Es braucht einen Piloten. Es hat schon 41 gehabt. Du bist Nr. 42.

---

## 2. Design-Säulen

1. **Der Raum ist der Gegner.** Die Karte ist nicht statisch, sie *reagiert*. Was du nicht ansiehst, darf sich ändern.
2. **Vertraut = gefährlich.** Horror kommt nicht aus Monstern, sondern daraus, dass die Küche exakt deine Küche ist — bis auf ein Detail.
3. **Alles, was du tust, wird aufgezeichnet.** Und irgendwann gegen dich abgespielt.
4. **Keine Waffen. Kein Sanity-Meter.** Deine Ressourcen sind Licht, Anker und Wissen.
5. **Lore erklärt Mechanik.** Jede Regel des Spiels hat einen Grund in der Welt.

---

## 3. Lore

### 3.1 Was die Zelle ist
Ein **Gehäuse** — ein Fahrzeug einer nicht-menschlichen Herkunft (nie erklärt, nie gezeigt). Es bewegt sich nicht durch Raum, sondern durch *Bedeutung*: Es „ankert" sich an Orte, an denen Menschen sich verloren fühlen (Passstraße bei Nacht, Bahnhof um 3 Uhr, Parkhaus Ebene -4). Es tarnt sich als das, was man dort erwartet: eine Telefonzelle.

Sein Inneres hat keine eigene Form. Es **kopiert Räume aus dem Gedächtnis** seiner Passagiere, um bewohnbar zu sein. Deshalb ist alles vertraut und alles falsch: Es baut aus Erinnerung, und Erinnerung hat Lücken. Wo die Lücke ist, ist die Wand aus grauem Putz ohne Textur, das Fenster zeigt nichts, das Buch hat leere Seiten.

### 3.2 Der Pilot
Das Gehäuse ist seit ~1978 (Bau der Zelle) **ohne Piloten** gestrandet. Ohne Piloten kann es nicht weiter, also sammelt es Passagiere und versucht, aus ihnen einen zu machen. Ein Pilot wird man, wenn das Gehäuse einen **vollständig kopiert** hat — dann ersetzt die Kopie das Original. Die 41 Vorgänger sind nicht tot: Sie sind *Teile des Innenraums*. Der Hausmeister-Flur ist Passagier 7. Die Krankenhausstation ist Passagier 19. Die Wärter sind das, was von Menschen übrig bleibt, die zu lange drin waren, aber nicht vollständig kopiert wurden — halbfertige Versionen.

### 3.3 Das Herz
Ganz innen, hinter dem Maschinenraum, liegt das **Herz**: der eigentliche Steuerraum. Er sieht aus wie nichts Menschliches — der einzige Ort, der *nicht* aus Erinnerung gebaut ist. Dort sitzt der letzte Pilot-Versuch (Passagier 41, „Anna", 1994), halb verwachsen mit dem Stuhl, noch ansprechbar. Sie ist Quest-Geberin, Warnerin und am Ende Gegnerin — je nach Ende.

### 3.4 Die Telefone
Jedes Telefon im Innenraum ist eine **Leitung zum Gehäuse selbst**. Es spricht mit den Stimmen früherer Passagiere (es hat sie ja). Es sagt teils Wahrheit, teils lockt es. Aber jedes Abheben gibt ihm deine Stimme — und irgendwann ruft es dich mit deiner eigenen an.

### 3.5 Warum DU
Du hast am Anfang ein Formular ausgefüllt (siehe 5.1). Das Gehäuse „liest" dich daraus. Twist in Akt 4: Du findest im Archiv **dein eigenes Formular — 3 Mal**. Du warst schon hier. Du bist rausgekommen. Du hast es vergessen (weil das Gehäuse beim Rauslassen die Erinnerung behält, nicht du). Der Unfall ist kein Zufall: Das Gehäuse hat dich zurückgeholt, weil du der einzige bist, der je raus ist.

---

## 4. Ziel & Enden

**Hauptziel:** Die 5 **Kernstücke** finden (Teile des Pilotenschlüssels, verteilt in 5 Zonen) → Maschinenraum → Herz.

Im Herz drei Optionen, die aus den Mechaniken hervorgehen:

| Ende | Bedingung | Was passiert |
|---|---|---|
| **A — Pilot** | Alle 5 Kernstücke, ≥ 60 % Kopie-Fortschritt | Du setzt dich. Das Gehäuse startet. Du siehst die Passstraße aus dem Fenster wegziehen. Credits mit deiner Stimme, die den nächsten Anrufer begrüßt. |
| **B — Abschalten** | Alle 5 Kernstücke, Annas Vertrauen (≥ 3 ihrer Bitten erfüllt) | Anna zeigt dir den Notaus. Alle Räume verfallen, die Wärter werden wieder Menschen und sterben friedlich. Du auch. Die Zelle steht leer im Schnee. |
| **C — Vergessen** (geheim) | Kopie-Fortschritt < 25 % beim Erreichen des Herzens | Das Gehäuse hat zu wenig von dir. Du bist ihm fremd. Die Eingangstür ist wieder da. Du gehst raus — es ist Morgen, dein Auto steht unversehrt. Auf dem Beifahrersitz liegt ein Formular. Leer. |

Ende C ist das „wahre" Ende und **nur** erreichbar, wenn man das Spiel absichtlich *anders* spielt (wenig abheben, wenig Zeit in Erinnerungsräumen, Anker richtig setzen). Das Spiel sagt das nie direkt, nur über Annas Andeutungen.

---

## 5. Spielmechaniken

### 5.1 Das Formular (Start)
Nach dem Unfall, im Auto, im Handschuhfach: ein Versicherungs-Unfallbericht. Der Spieler füllt aus (Freitext, kurz):
- Name, Geburtsort, Heimatstadt
- „Beruf / Ausbildung"
- „Name eines Angehörigen"
- „Haustier (falls im Fahrzeug)"
- „Wovor haben Sie beim Unfall am meisten Angst gehabt?" (Multiple Choice: Ertrinken · Ersticken · Feuer · Allein sterben · Nicht gefunden werden)

**Diese Daten sind der Generator-Seed.** Sie landen als Text auf Schildern, Türklingeln, Zeugnissen, Grabsteinen, in Telefonanrufen. Die Angst wählt die Zone-3-Variante (Wasser / Enge / Rauch / Leere / Vermisst). Der Spieler baut sein eigenes Horrorhaus, ohne es zu merken.

### 5.2 Prozedurale Welt (kein Gitter, ein Graph)

**Raum-Typen (Module):**
- `Korridor` (Verbinder, 3 Längen, mit/ohne Abzweig)
- `Zimmer` — Erinnerungsräume: Küche, Kinderzimmer, Klassenzimmer, Wartezimmer, Bad, Wohnzimmer, Büro, Hotelzimmer, Bahnhofsklo, Keller
- `Halle` — große Räume: Turnhalle, Parkgarage, Schwimmbad (leer), Kirchenschiff, Supermarkt
- `Schacht` — Vertikal: Treppenhaus, Aufzug, Lüftungsschacht, Brunnen
- `Archiv` — Regale mit Akten früherer Passagiere (Lore-Zone)
- `Nasszelle` — Zone-3-Angstvariante
- `Maschine` — Technik, Rohre, das Brummen wird lauter
- `Herz` — handgebaut, einmalig
- `Vorraum` — der Innenraum der Telefonzelle (Startraum, Hub, Save-Punkt)

**Generator („Wachstum statt Karte"):**
```
Welt = Graph(Knoten=Raum, Kante=Tür)
Beim Öffnen einer Tür, hinter der noch nichts liegt:
  1. Zonen-Tiefe z = Distanz zum Vorraum (0..5)
  2. Raumtyp ziehen aus Zonen-Tabelle[z] (gewichtet)
  3. Variante ziehen aus Formular-Seed + Zufalls-Seed
  4. Raum instanzieren, Türsockel mit Kanten belegen
     - 1 Kante zurück (immer)
     - 1..3 Kanten weiter (Zone-abhängig)
     - Sackgassen erlaubt, 15 %
  5. „Verdrahtung": Mit 10 % Wahrscheinlichkeit verbindet eine
     neue Tür mit einem SCHON EXISTIERENDEN Raum (Loop).
     Räumlich passt das nicht → egal, das ist der Punkt.
```
Jede Zone hat einen **Ankerraum** (handgebaut, garantiert), in dem das Kernstück liegt. Der Generator garantiert, dass der Ankerraum nach 6–12 Räumen der Zone erreichbar ist.

**Umbau-Regel (Kernmechanik):**
Ein Raum, der **nicht angeankert** ist und den der Spieler **nicht sieht** (kein Sichtkontakt, Tür zu), darf sich beim nächsten Betreten *verändern*:
- 50 %: identisch
- 30 %: Detail geändert (Möbel verschoben, Bild anders, eine Tür weniger)
- 15 %: Raum durch anderen desselben Typs ersetzt
- 5 %: Raum durch etwas Falsches ersetzt (Küche → dieselbe Küche, aber 4 m hoch, Möbel an der Decke)
Eine Kante bleibt aber immer erhalten: die, durch die du reingekommen bist. Zurück geht immer. Nur nicht dorthin, wo du herkamst.

**Anti-Pingpong (ergänzt 08.09.2026, Justins Einwand):** Ohne Zusatzregel könnte der Spieler A→B→A→B durch dieselbe Tür pendeln und Räume „farmen". Deshalb zwei Regeln übereinander:
1. **Warme Räume:** Die letzten **3 besuchten Räume** sind eingefroren, egal ob Tür zu oder Sichtkontakt. Ein Raum wird erst wieder „kalt" (würfelbar), wenn der Spieler ≥ 3 Räume Graph-Distanz entfernt war. Folge: der direkte Rückweg stimmt immer; erst wer nach 10 Räumen umkehrt, merkt, dass die Wohnung hinter ihm nicht mehr die Wohnung ist.
2. **Das Gehäuse merkt Pingpong:** 3× hintereinander dieselbe Tür hin und zurück → kein Reroll, sondern *Reaktion* (gewichtet): Tür geht beim 4. Mal nicht mehr auf (40 %) · hinter der Tür steht die Kopie (30 %) · das Telefon im Raum klingelt (20 %) · Dunkelphase startet sofort (10 %). Zähler +2 % Kopie-Fortschritt. Lore: es zeichnet dich auf, es sieht, dass du es testest. Der Exploit wird zum Horror-Moment.

**Nicht-euklidisch:** Türen sind Portale (Stencil-Portal in Daidalos). Ein 3-m-Flur führt in eine Turnhalle. Von außen sieht man durch die Tür in den nächsten Raum — korrekt gerendert. Das ist das technisch teuerste Feature und gleichzeitig der wichtigste Horror-Trick.

### 5.3 Anker
Du hast Gegenstände aus deinem Auto: **Warndreieck, Erste-Hilfe-Box, Eiskratzer, Tankquittung, ein Foto vom Armaturenbrett** (Angehöriger aus Formular). Jeder ist ein **Anker**: In einen Raum gelegt, friert er den Raum ein — er ändert sich nie mehr. Max. 5 Anker im Spiel, holbar, aber jeder Weg zurück ist ein Risiko.

Strategie: Womit ankert man? Den Weg zum Kernstück? Den Save-Raum? Den Fluchtweg?
Lore-Haken: Anker funktionieren, weil sie **echt** sind — von draußen, nicht aus Erinnerung. Das Gehäuse kann Echtes nicht überschreiben. (Deshalb kann man die Anker in Ende C auch *alle* im Vorraum stapeln und der Eingangstür damit „Echtheit" zurückgeben — versteckter Alternativ-Trigger.)

### 5.4 Licht & Puls
Das Gehäuse **atmet**. Zyklus ~4–7 Minuten (zufällig):
- **Hellphase:** Deckenlichter an, Räume stabil, Wärter ruhen.
- **Dunkelphase (40–90 s):** Alles aus außer deiner Taschenlampe. Umbau-Regel greift auch auf Räume, die du *siehst* — aber nur außerhalb deines Lichtkegels. Wärter wandern.
Vor der Dunkelphase flackert es 5 s. Das ist deine Warnung.

**Taschenlampe:** Batterien sind endlich, in Räumen findbar (formular-abhängig: Kinderzimmer haben eher welche). Kein Licht = Tod ist möglich? Nein. Kein Licht = du siehst nicht, dass sich der Raum um dich baut. Du hörst es nur.

### 5.5 Die Kopie (Signature-Mechanik)
Das Gehäuse zeichnet dich auf. Ab Zone 2 taucht **die Kopie** auf: ein Wesen, das aussieht wie du von hinten (du siehst nie das Gesicht) und **deine Eingaben von vor 90 Sekunden exakt nachspielt**. Echt aufgezeichnet, nicht KI.
- Sie geht deinen Weg. Wenn du umkehrst, läufst du in sie hinein.
- Berührung = Kopie-Fortschritt +15 %, Bildschirm-Riss, Teleport in den Vorraum.
- Sie hat Kollision. Du kannst sie in einem Raum einsperren, wenn du vor 90 s die Tür zugemacht hast — d. h. du planst gegen dein eigenes Verhalten aus der Vergangenheit.
- Ab Zone 4 gibt es zwei (90 s und 180 s). Im Maschinenraum drei.
- Jedes abgehobene Telefon gibt ihr mehr: erst Schritte, dann Atem, dann deine Stimme, die deinen Namen sagt.

**Kopie-Fortschritt (0–100 %)** ist das versteckte Hauptmeter (nie als Balken angezeigt, nur über Symptome: dein Spiegelbild reagiert verzögert; Schilder tragen deine Handschrift; die Kopie wird detaillierter):
- +1 % pro Minute in Erinnerungsräumen
- +5 % pro abgehobenem Telefon
- +15 % pro Kopie-Berührung
- −10 % pro gesetztem Anker (das Gehäuse verliert Zugriff)
- −5 % pro gelöschter Akte (siehe Archiv)

### 5.6 Die Wärter
Unfertige Passagiere. Blind, hören, langsam, aber stoppen nie. Jeder Wärter hat **einen** Erinnerungsraum, der „seiner" ist — dort ist er unbesiegbar, außerhalb kann man ihn mit Licht (Taschenlampe direkt ins Gesicht, 3 s) kurz einfrieren.
Sie sprechen. Immer denselben Satz aus ihrem Formular. („Ich hab Angst vor dem Ersticken." — Wärter aus einer Nasszelle, 1983.)
Töten geht nicht. Man kann sie aber **befreien**: Ihre Akte im Archiv finden und verbrennen (Feuerzeug aus dem Auto, 3 Ladungen). Dann bleibt er stehen, sagt seinen Namen, zerfällt. Sein Raum wird grau und leer. → Pfad zu Ende B.

### 5.7 Telefone
In ~jedem 4. Raum. Klingeln zufällig, häufiger wenn du lange nicht abgehoben hast.
- Abheben: Lore, Hinweise (welche Zone das nächste Kernstück hat), Lügen (falsche Richtung, 30 %). +5 % Kopie.
- Nicht abheben: Klingeln wird lauter, zieht Wärter an. Nach 60 s hört es auf.
- Speichern: **nur** an Telefonen, indem du **anrufst** (Nummer aus dem Formular = die des Angehörigen). Es meldet sich ein Anrufbeantworter mit der Stimme des Gehäuses, das deinen Angehörigen imitiert. Speichern kostet also Kopie-Fortschritt. Save-Scumming wird bestraft. Das ist gewollt.

### 5.8 Archiv (Zone 4)
Aktenschränke, endlose Regale. 41 Akten (Formulare früherer Passagiere, echte Lore-Texte, ~150 Wörter je) + deine 3. Akten lesen = Hinweise für Ende B/C. Akten verbrennen = Wärter befreien. Deine eigenen verbrennen = −20 % Kopie je Akte, aber Anna wird sauer (sie braucht das Gehäuse funktionsfähig, um zu sterben).

### 5.9 Inventar
Klein, physisch (kein Menü): Taschenlampe (Hand), 4 Slots Jacke. Anker sind groß und nehmen 2 Slots. Kein Crafting.

---

## 6. Zonen / Struktur

| Zone | Name | Räume | Neue Mechanik | Kernstück-Ankerraum |
|---|---|---|---|---|
| 0 | Vorraum | 1 | Hub, Save, Tutorial-Telefon | — |
| 1 | Wohnung | 6–10 | Umbau-Regel, Anker, erstes Telefon | Kinderzimmer mit deinem Namen an der Tür |
| 2 | Schule/Amt | 8–14 | Puls/Dunkelphase, erste Kopie, erster Wärter | Turnhalle, Geräteraum |
| 3 | Angst | 8–12 | Formular-Angst-Variante (5 Varianten), Wärter in eigenem Raum | Variante: Schwimmbad / Lüftungsschacht / verrauchte Küche / leere Wohnung / Wald aus Flurtüren |
| 4 | Archiv | 10–16 | Akten, Verbrennen, 2 Kopien, Twist (deine 3 Akten) | Aktenraum „42" |
| 5 | Maschine | 6–8 | 3 Kopien, kein Umbau mehr (alles ist fest — und laut), Anna per Telefon | Herz |

Zone 3 ist die einzige, die je nach Spieler ganz anders ist → Replay-Grund.

---

## 7. Horror-Werkzeugkasten (was der Generator streuen darf)

- **Fehlerdetail:** Steckdose an der Decke. Treppe mit einer Stufe zu viel. Uhr rückwärts. Tür ohne Klinke innen.
- **Formular-Text:** Dein Geburtsort auf einem Ortsschild in einem Kellerflur. Der Name deines Angehörigen auf einem Zettel: „bin einkaufen, bleib drinnen".
- **Gelöschtes:** Fotos mit rausgeschnittenen Gesichtern. Bücher mit leeren Seiten. Fernseher mit Standbild eines Raumes, in dem du gerade warst.
- **Audio:** Dein eigener Schritt-Sound, 200 ms verzögert aus dem Nebenraum. Telefon im Raum, in dem kein Telefon ist.
- **Never-Jumpscare-Regel:** Maximal 3 Jumpscares im ganzen Spiel, alle mit Aufbau. Der Rest ist Unbehagen.

---

## 8. Art Direction

- **Optik:** Late-70s/80s Österreich. PVC-Böden, Lamperien, Neonröhren, Raufaser. Ausgeblichen. Farbraum: Gelb (Post), Grau, Kaltweiß, ein einziges Rot (Warndreieck).
- **Grau-Wand-Regel:** Alles, was das Gehäuse nicht weiß, ist flacher grauer Putz ohne Normalmap. Der Spieler lernt: Grau = Lücke = Ort ohne Erinnerung = hier kann alles passieren.
- **Render:** Leichtes VHS-Grain + Chromatic Aberration, das mit Kopie-Fortschritt zunimmt. Kein Blut, kein Gore.
- **Kamera:** FOV 80, Kopfwackeln minimal, Lean-Mechanik für Türspalte.

## 9. Audio

- Grundton: 50-Hz-Brummen des Gehäuses, lauter Richtung Zone 5.
- Puls: Neonröhren-Zischen, Relais-Klacken vor Dunkelphase.
- Kein Musik-Score außer im Herz (eine Zither, verstimmt, 1978er Aufnahme).
- Stimmen: Alle Passagiere Tiroler/österreichische Dialekte, Anna spricht Hochdeutsch (sie ist aus Hamburg, 1994 auf Durchreise).

---

## 10. Technik (Daidalos)

- **Module:** Jeder Raumtyp = Prefab mit `DoorSocket[]` (Position, Normale, Breite). Generator belegt Sockets.
- **Portale:** Stencil-Portale pro Tür; Rekursionstiefe 2 (man sieht durch 2 Türen). Räume liegen physisch weit auseinander (10-km-Raster), Portale teleportieren Kamera + Spieler beim Durchschreiten.
- **Umbau:** Sichtbarkeits-Check = Frustum + Occlusion gegen Türsockel. Raum-„Fingerprint" (Seed) wird beim Verlassen neu gewürfelt, wenn Regel greift.
- **Kopie:** Ringbuffer der Spieler-Transform + Aktionen (90 s @ 20 Hz = 1800 Einträge). Playback als Actor mit Kollision.
- **Formular-Seed:** SHA256(Formular-Text) → 64-bit Seed + Wortliste für Text-Slots (Schilder-Shader mit SDF-Font, Text zur Laufzeit).
- **Save:** JSON: Graph, Raum-Seeds, Anker, Kopie-%, Akten-Status, Formular.

## 11. Umfang / Meilensteine

| MS | Inhalt | Ziel |
|---|---|---|
| **M0 Prototyp** (2–3 Wo) | Vorraum + 6 Raum-Module, Graph-Generator, Portale, Anker, Taschenlampe | „Läuft man 10 Min drin rum und es ist creepy?" |
| M1 Vertical Slice | Zone 1 komplett, Puls, Umbau-Regel, 1 Telefon mit 5 Anrufen, Kopie | Spielbar 30 Min, ein Kernstück |
| M2 | Zone 2–3, Wärter, Formular-Seed, 5 Angstvarianten | 2 h |
| M3 | Archiv, Akten (41 Texte), Verbrennen, Twist | 4 h |
| M4 | Maschine, Herz, Anna, 3 Enden | Content-complete |
| M5 | Polish, Audio, Steam-Page | Release |

## 12. Offene Fragen an Justin

1. Formular als Freitext (mehr Immersion, mehr Aufwand) oder nur Multiple-Choice?
2. Sprache: nur Deutsch mit Dialekt-Voice, oder von Anfang an EN-Text?
3. Wie viel darf der Prototyp in Daidalos an Engine-Features (Stencil-Portale) neu bauen?
4. Soll die Kopie wirklich Eingaben *aufzeichnen* (echt, aber braucht Ringbuffer + Netcode-artige Playback) — oder reicht ein Pfad-Follower fürs Erste?
