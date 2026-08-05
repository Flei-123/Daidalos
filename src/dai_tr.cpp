// Daidalos - the translation tables. See dai_tr.h for why a language is a
// table and not a branch.
//
// One row per user-facing string: { key (English), German }. Keys stay in
// English on purpose - the key IS the fallback, and a table that has to
// invent both languages for a label nobody translated yet is a table people
// stop adding rows to.

#include "dai_tr.h"

#include <cstring>

static int g_lang = DAI_LANG_EN;

extern "C" {

void dai_tr_lang(int lang) { g_lang = (lang == DAI_LANG_DE) ? DAI_LANG_DE : DAI_LANG_EN; }
int  dai_tr_lang_get(void) { return g_lang; }

struct Row { const char *en; const char *de; };

static const Row TABLE[] = {
    // panels
    { "Hierarchy", "Hierarchie" },
    { "Inspector", "Inspektor" },
    { "Project", "Projekt" },
    { "Console", "Konsole" },
    { "Audio", "Audio" },
    { "Settings", "Einstellungen" },
    { "Scene", "Szene" },
    { "Game", "Spiel" },
    // inspector
    { "nothing selected", "nichts ausgewählt" },
    { "move them with the gizmo", "mit dem Gizmo bewegen" },
    { "Tag", "Tag" },
    { "Asset", "Asset" },
    { "Transform", "Transform" },
    { "Position", "Position" },
    { "Rotation", "Rotation" },
    { "Scale", "Skalierung" },
    { "Mesh Renderer", "Mesh Renderer" },
    { "Rigidbody", "Rigidbody" },
    { "Motion", "Bewegung" },
    { "Density", "Dichte" },
    { "Friction", "Reibung" },
    { "Bounce", "Abprall" },
    { "no rigidbody - nothing drives this", "kein Rigidbody - nichts bewegt das hier" },
    { "no collider - nothing can hit this", "kein Collider - nichts kann das hier treffen" },
    { "no scripts - drag a .js or .cpp from Project onto this object",
      "keine Skripte - .js oder .cpp aus dem Projekt hierher ziehen" },
    { "Add Component...", "Komponente hinzufügen..." },
    { "Materials", "Materialien" },
    { "Static", "Statisch" },
    { "as a tab", "als Tab" },
    { "left of", "links davon" },
    { "right of", "rechts davon" },
    { "above", "darüber" },
    { "below", "darunter" },
    { "as a window", "als Fenster" },
    { "Create: JS Script", "Neu: JS-Skript" },
    { "Create: C++ Behaviour", "Neu: C++-Verhalten" },
    { "Create: Folder", "Neu: Ordner" },
    { "Open in VS Code", "In VS Code öffnen" },
    { "prefab placed", "Prefab platziert" },
    { "prefab saved", "Prefab gespeichert" },
    { "Colour", "Farbe" },
    { "Size", "Größe" },
    // add component categories
    { "Physics", "Physik" },
    { "Rendering", "Darstellung" },
    { "Scripts", "Skripte" },
    { "Camera", "Kamera" },
    { "Light", "Licht" },
    { "Sprite (2D)", "Sprite (2D)" },
    { "Audio Source", "Audioquelle" },
    { "Remove Rigidbody", "Rigidbody entfernen" },
    { "Remove Collider", "Collider entfernen" },
    { "Remove Camera", "Kamera entfernen" },
    { "Remove Light", "Licht entfernen" },
    { "Remove Sprite (2D)", "Sprite (2D) entfernen" },
    { "Remove Audio Source", "Audioquelle entfernen" },
    // component menu
    { "Reset", "Zurücksetzen" },
    { "Copy Component", "Komponente kopieren" },
    { "Paste Component Values", "Komponentenwerte einfügen" },
    { "Remove Component", "Komponente entfernen" },
    { "component copied", "Komponente kopiert" },
    { "component pasted", "Komponente eingefügt" },
    { "component reset", "Komponente zurückgesetzt" },
    { "component removed", "Komponente entfernt" },
    // console
    { "Clear", "Leeren" },
    { "Copy", "Kopieren" },
    { "copied", "kopiert" },
    { "console copied", "Konsole kopiert" },
    { "no messages - script print() and engine warnings land here",
      "keine Meldungen - print() und Warnungen landen hier" },
    // settings
    { "Preferences", "Einstellungen" },
    { "Project Settings", "Projekteinstellungen" },
    { "Gizmos", "Gizmos" },
    { "Appearance", "Aussehen" },
    { "UI size", "UI-Größe" },
    { "Theme", "Theme" },
    { "Language", "Sprache" },
    { "Viewport", "Ansicht" },
    { "Cam speed", "Kamerageschwindigkeit" },
    { "Gizmo px", "Gizmo-Größe" },
    { "Snap step", "Rasterschritt" },
    { "Floor grid", "Bodenraster" },
    { "Collider frames", "Collider-Umrisse" },
    { "Camera frustums", "Kamera-Frustums" },
    { "Values apply immediately.", "Werte gelten sofort." },
    { "These belong to the project, not to you:", "Das gehört zum Projekt, nicht zu dir:" },
    { "they are saved in settings/project.txt and",
      "gespeichert in settings/project.txt und" },
    { "shared with everyone who opens it.", "geteilt mit allen, die es öffnen." },
    { "no project open", "kein Projekt offen" },
    { "Changes are saved immediately.", "Änderungen werden sofort gespeichert." },
    { "Tags", "Tags" },
    { "Gravity", "Schwerkraft" },
    { "Tick Hz", "Tick-Rate" },
    { "Max bodies", "Max. Körper" },
    { "App name", "App-Name" },
    { "Defaults for NEW rigidbodies", "Standard für NEUE Rigidbodys" },
    { "Def. friction", "Standard-Reibung" },
    { "Def. bounce", "Standard-Abprall" },
    // project window
    { "Create: JS Script", "Neu: JS-Skript" },
    { "Create: C++ Behaviour", "Neu: C++-Behaviour" },
    { "Create: Folder", "Neu: Ordner" },
    { "Rename", "Umbenennen" },
    { "Save scene", "Szene speichern" },
    { "Refresh", "Aktualisieren" },
    { "Assign to selection", "Der Auswahl zuweisen" },
    { "drag onto an object to attach", "auf ein Objekt ziehen zum Anhängen" },
    // misc
    { "prefab created", "Prefab erstellt" },
    { "prefab saved", "Prefab gespeichert" },
    { "prefab save failed", "Prefab-Speichern fehlgeschlagen" },
    { "component reset", "Komponente zurückgesetzt" },
    { "select an object first", "erst ein Objekt auswählen" },
    { "Search components...", "Komponenten suchen..." },
    { "nothing matches", "nichts gefunden" },
    { "From shape (auto)", "Aus Form (auto)" },
    { "From asset file", "Aus Asset-Datei" },
    { "Element", "Element" },
    { "Name", "Name" },
};

const char *dai_tr(const char *key) {
    if (!key || !*key) return key;
    if (g_lang == DAI_LANG_EN) return key;
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); ++i)
        if (std::strcmp(TABLE[i].en, key) == 0) return TABLE[i].de;
    return key;   // untranslated is English, not blank
}

} // extern "C"
