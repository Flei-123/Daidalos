// The built-in icon set and the atlas it is packed into.
//
// The sources below are ordinary SVG - 24x24 viewBox, 2 unit strokes, round
// caps and joins. They are written out in full rather than generated because
// an icon is a drawing, and a drawing in a table of numbers is unreadable and
// unfixable. Anyone can paste one of these into a browser and see it.

#include "dai_icons.h"
#include "dai_svg.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// A stroked icon: fill nothing, draw the outline. The wrapper is repeated per
// icon rather than concatenated at build time so each string is a valid,
// standalone SVG file - which is what makes them checkable in a browser.
#define STROKE_HEAD                                                            \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' " \
    "stroke='currentColor' stroke-width='2' stroke-linecap='round' "           \
    "stroke-linejoin='round'>"
#define SOLID_HEAD                                                             \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' "             \
    "fill='currentColor' stroke='none'>"
// Explicit fills carry their own colour - asset kinds should read in colour,
// not as silhouettes of one tint.
#define FILL_HEAD                                                              \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' stroke='none'>"
#define TAIL "</svg>"

struct Builtin { const char *name; const char *svg; };

const Builtin BUILTIN[] = {
// ---- gizmo modes --------------------------------------------------------
{ "move", STROKE_HEAD
  "<polyline points='5 9 2 12 5 15'/><polyline points='9 5 12 2 15 5'/>"
  "<polyline points='15 19 12 22 9 19'/><polyline points='19 9 22 12 19 15'/>"
  "<line x1='2' y1='12' x2='22' y2='12'/><line x1='12' y1='2' x2='12' y2='22'/>" TAIL },

{ "rotate", STROKE_HEAD
  "<polyline points='21 4 21 10 15 10'/>"
  "<path d='M19.4 15a8.5 8.5 0 1 1-2-8.9L21 10'/>" TAIL },

{ "scale", STROKE_HEAD
  "<polyline points='15 3 21 3 21 9'/><polyline points='9 21 3 21 3 15'/>"
  "<line x1='21' y1='3' x2='14' y2='10'/><line x1='3' y1='21' x2='10' y2='14'/>" TAIL },

// ---- transport ----------------------------------------------------------
{ "play",  SOLID_HEAD "<path d='M7 4.5 19 12 7 19.5Z'/>" TAIL },
{ "pause", SOLID_HEAD "<rect x='6' y='4' width='4' height='16' rx='1'/>"
                      "<rect x='14' y='4' width='4' height='16' rx='1'/>" TAIL },
{ "stop",  SOLID_HEAD "<rect x='6' y='6' width='12' height='12' rx='1.5'/>" TAIL },

// ---- editing ------------------------------------------------------------
{ "undo", STROKE_HEAD
  "<polyline points='9 14 4 9 9 4'/><path d='M20 20v-7a4 4 0 0 0-4-4H4'/>" TAIL },
{ "redo", STROKE_HEAD
  "<polyline points='15 14 20 9 15 4'/><path d='M4 20v-7a4 4 0 0 1 4-4h12'/>" TAIL },
{ "copy", STROKE_HEAD
  "<rect x='9' y='9' width='13' height='13' rx='2'/>"
  "<path d='M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1'/>" TAIL },
{ "trash", STROKE_HEAD
  "<polyline points='3 6 5 6 21 6'/>"
  "<path d='M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2'/>"
  "<line x1='10' y1='11' x2='10' y2='17'/><line x1='14' y1='11' x2='14' y2='17'/>" TAIL },
{ "save", STROKE_HEAD
  "<path d='M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z'/>"
  "<polyline points='17 21 17 13 7 13 7 21'/><polyline points='7 3 7 8 15 8'/>" TAIL },

// ---- chrome -------------------------------------------------------------
{ "layout", STROKE_HEAD
  "<rect x='3' y='3' width='18' height='18' rx='2'/>"
  "<line x1='3' y1='9' x2='21' y2='9'/><line x1='9' y1='21' x2='9' y2='9'/>" TAIL },
{ "chevron-right", STROKE_HEAD "<polyline points='9 18 15 12 9 6'/>" TAIL },
{ "chevron-down",  STROKE_HEAD "<polyline points='6 9 12 15 18 9'/>" TAIL },
{ "plus",  STROKE_HEAD "<line x1='12' y1='5' x2='12' y2='19'/>"
                       "<line x1='5' y1='12' x2='19' y2='12'/>" TAIL },
{ "check", STROKE_HEAD "<polyline points='20 6 9 17 4 12'/>" TAIL },
{ "close", STROKE_HEAD "<line x1='18' y1='6' x2='6' y2='18'/>"
                       "<line x1='6' y1='6' x2='18' y2='18'/>" TAIL },
{ "search", STROKE_HEAD "<circle cx='11' cy='11' r='8'/>"
                        "<line x1='21' y1='21' x2='16.65' y2='16.65'/>" TAIL },
{ "settings", STROKE_HEAD
  "<circle cx='12' cy='12' r='3'/>"
  "<path d='M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.6a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z'/>" TAIL },

// ---- components and scene objects ---------------------------------------
{ "box", STROKE_HEAD
  "<path d='M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z'/>"
  "<polyline points='3.3 7 12 12 20.7 7'/><line x1='12' y1='22' x2='12' y2='12'/>" TAIL },
{ "sphere", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><ellipse cx='12' cy='12' rx='4' ry='9'/>"
  "<line x1='3' y1='12' x2='21' y2='12'/>" TAIL },
{ "capsule", STROKE_HEAD
  "<rect x='7' y='3' width='10' height='18' rx='5'/>"
  "<path d='M7 12h10'/>" TAIL },
{ "eye", STROKE_HEAD
  "<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z'/>"
  "<circle cx='12' cy='12' r='3'/>" TAIL },
{ "eye-off", STROKE_HEAD
  "<path d='M17.9 17.9A10 10 0 0 1 12 20C5 20 1 12 1 12a18.4 18.4 0 0 1 5.1-5.9'/>"
  "<path d='M9.9 4.2A9.1 9.1 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.2 3.2'/>"
  "<path d='M14.1 14.1a3 3 0 1 1-4.2-4.2'/>"
  "<line x1='1' y1='1' x2='23' y2='23'/>" TAIL },
// ---- asset kinds: what a file IS, at a glance ---------------------------
// Unity marks a script with a hash, a model with a cube, audio with a note.
// The browser used one grey page for everything, so a folder of assets read
// as a folder of nothing.
//
// The FILL_HEAD variants are the same shapes in colour - the kind of a file
// should hit the eye before the filename is even read.
{ "script", FILL_HEAD "<path fill='#4a9edb' d='M8 2h8v20H8z M4 8h17v2H4z M3 14h18v2H3z'/>" TAIL },
{ "model", FILL_HEAD "<path fill='#e0813d' d='M12 2 21 7v10l-9 5-9-5V7z'/>"
  "<path fill='#c96a28' d='M12 12 21 7l0 0.001L12 12z M12 12v10l9-5V7z'/>" TAIL },
{ "audio", FILL_HEAD "<path fill='#4dc28a' d='M9 18V5l12-2v13z M6 15a3 3 0 1 0 0 6 3 3 0 0 0 0-6z M18 13a3 3 0 1 0 0 6 3 3 0 0 0 0-6z'/>" TAIL },
{ "image", FILL_HEAD "<path fill='#8e6ee0' d='M3 3h18v18H3z M8.5 7a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z M5 21l11-11 5 5v6z'/>" TAIL },
{ "scene", FILL_HEAD "<path fill='#e0bc3d' d='M3 7l9-4 9 4-9 4z M3 12l9 4 9-4-9 8z'/>" TAIL },
{ "model", STROKE_HEAD
  "<path d='M12 2 21 7v10l-9 5-9-5V7z'/>"
  "<polyline points='3 7 12 12 21 7'/><line x1='12' y1='12' x2='12' y2='22'/>" TAIL },
{ "audio", STROKE_HEAD
  "<path d='M9 18V5l12-2v13'/><circle cx='6' cy='18' r='3'/><circle cx='18' cy='16' r='3'/>" TAIL },
{ "image", STROKE_HEAD
  "<rect x='3' y='3' width='18' height='18' rx='2'/>"
  "<circle cx='8.5' cy='8.5' r='1.5'/><polyline points='21 15 16 10 5 21'/>" TAIL },
{ "scene", STROKE_HEAD
  "<path d='M3 7l9-4 9 4-9 4z'/><path d='M3 12l9 4 9-4'/><path d='M3 17l9 4 9-4'/>" TAIL },
{ "console", STROKE_HEAD
  "<rect x='2' y='4' width='20' height='16' rx='2'/>"
  "<polyline points='6 9 9 12 6 15'/><line x1='12' y1='15' x2='17' y2='15'/>" TAIL },
{ "layers", STROKE_HEAD
  "<polygon points='12 2 2 7 12 12 22 7 12 2'/>"
  "<polyline points='2 17 12 22 22 17'/><polyline points='2 12 12 17 22 12'/>" TAIL },
{ "sun", STROKE_HEAD
  "<circle cx='12' cy='12' r='5'/>"
  "<line x1='12' y1='1' x2='12' y2='3'/><line x1='12' y1='21' x2='12' y2='23'/>"
  "<line x1='4.2' y1='4.2' x2='5.6' y2='5.6'/><line x1='18.4' y1='18.4' x2='19.8' y2='19.8'/>"
  "<line x1='1' y1='12' x2='3' y2='12'/><line x1='21' y1='12' x2='23' y2='12'/>"
  "<line x1='4.2' y1='19.8' x2='5.6' y2='18.4'/><line x1='18.4' y1='5.6' x2='19.8' y2='4.2'/>" TAIL },
{ "camera", STROKE_HEAD
  "<path d='M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z'/>"
  "<circle cx='12' cy='13' r='4'/>" TAIL },
{ "grid", STROKE_HEAD
  "<rect x='3' y='3' width='18' height='18' rx='2'/>"
  "<line x1='3' y1='9' x2='21' y2='9'/><line x1='3' y1='15' x2='21' y2='15'/>"
  "<line x1='9' y1='3' x2='9' y2='21'/><line x1='15' y1='3' x2='15' y2='21'/>" TAIL },

// ---- what a hierarchy row IS -------------------------------------------
// An editor that writes the object's type into its NAME ("MainCamera") is
// spending the one column the user reads on information a 16 px glyph carries
// for free - and then the name column no longer holds the name.
{ "light", STROKE_HEAD
  "<line x1='12' y1='1' x2='12' y2='4'/><line x1='12' y1='20' x2='12' y2='23'/>"
  "<line x1='4.2' y1='4.2' x2='6.3' y2='6.3'/><line x1='17.7' y1='17.7' x2='19.8' y2='19.8'/>"
  "<line x1='1' y1='12' x2='4' y2='12'/><line x1='20' y1='12' x2='23' y2='12'/>"
  "<circle cx='12' cy='12' r='4.5'/>" TAIL },
{ "empty", STROKE_HEAD
  "<path d='M12 3 3 7.5v9L12 21l9-4.5v-9z' stroke-dasharray='3 2.4'/>" TAIL },
{ "cube", STROKE_HEAD
  "<path d='M12 2.6 21 7v10l-9 4.4L3 17V7z'/><path d='M3 7l9 4.4L21 7'/>"
  "<line x1='12' y1='11.4' x2='12' y2='21.4'/>" TAIL },
{ "sprite", STROKE_HEAD
  "<rect x='3' y='3' width='18' height='18' rx='2'/><circle cx='8.5' cy='8.5' r='1.8'/>"
  "<path d='M21 15l-5-5L5 21'/>" TAIL },
{ "volume", STROKE_HEAD
  "<polygon points='11 5 6 9 2 9 2 15 6 15 11 19'/>"
  "<path d='M15.5 8.5a5 5 0 0 1 0 7'/><path d='M18.5 5.5a9 9 0 0 1 0 13'/>" TAIL },
{ "volume-x", STROKE_HEAD
  "<polygon points='11 5 6 9 2 9 2 15 6 15 11 19'/>"
  "<line x1='22' y1='9' x2='16' y2='15'/><line x1='16' y1='9' x2='22' y2='15'/>" TAIL },
{ "material", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><path d='M12 3a9 9 0 0 0 0 18'/>"
  "<circle cx='15.5' cy='8.5' r='1.3'/><circle cx='17' cy='13' r='1.3'/>" TAIL },
{ "prefab", STROKE_HEAD
  "<path d='M12 2.6 21 7v10l-9 4.4L3 17V7z'/><path d='M3 7l9 4.4L21 7'/>"
  "<line x1='12' y1='11.4' x2='12' y2='21.4'/><circle cx='12' cy='11.6' r='2.2'/>" TAIL },
{ "info", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><line x1='12' y1='11' x2='12' y2='16.5'/>"
  "<line x1='12' y1='7.6' x2='12' y2='7.7'/>" TAIL },
{ "warning", STROKE_HEAD
  "<path d='M10.3 3.6 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.6a2 2 0 0 0-3.4 0z'/>"
  "<line x1='12' y1='9' x2='12' y2='13.5'/><line x1='12' y1='17' x2='12' y2='17.1'/>" TAIL },
{ "error", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><line x1='15.5' y1='8.5' x2='8.5' y2='15.5'/>"
  "<line x1='8.5' y1='8.5' x2='15.5' y2='15.5'/>" TAIL },
{ "more", STROKE_HEAD
  "<circle cx='12' cy='5' r='1.4'/><circle cx='12' cy='12' r='1.4'/>"
  "<circle cx='12' cy='19' r='1.4'/>" TAIL },
{ "reset", STROKE_HEAD
  "<polyline points='2.5 5 2.5 11 8.5 11'/>"
  "<path d='M4.6 15a9 9 0 1 0 2.1-9.4L2.5 11'/>" TAIL },
{ "arrow-up", STROKE_HEAD
  "<line x1='12' y1='19' x2='12' y2='5'/><polyline points='5 12 12 5 19 12'/>" TAIL },
{ "arrow-down", STROKE_HEAD
  "<line x1='12' y1='5' x2='12' y2='19'/><polyline points='19 12 12 19 5 12'/>" TAIL },
{ "target", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><circle cx='12' cy='12' r='4'/>"
  "<line x1='12' y1='1.5' x2='12' y2='4'/><line x1='12' y1='20' x2='12' y2='22.5'/>"
  "<line x1='1.5' y1='12' x2='4' y2='12'/><line x1='20' y1='12' x2='22.5' y2='12'/>" TAIL },

// ---- assets -------------------------------------------------------------
{ "window", STROKE_HEAD
  "<rect x='2.5' y='3.5' width='19' height='17' rx='2'/>"
  "<line x1='2.5' y1='8.5' x2='21.5' y2='8.5'/><line x1='9.5' y1='8.5' x2='9.5' y2='20.5'/>" TAIL },
{ "minus", SOLID_HEAD
  "<rect x='4' y='10.7' width='16' height='2.6' rx='1.3'/>" TAIL },
{ "refresh", STROKE_HEAD
  "<polyline points='21 4 21 10 15 10'/><polyline points='3 20 3 14 9 14'/>"
  "<path d='M5.6 9a8 8 0 0 1 13.2-3L21 8'/><path d='M18.4 15a8 8 0 0 1-13.2 3L3 16'/>" TAIL },
{ "folder-open", STROKE_HEAD
  "<path d='M2 19V5a2 2 0 0 1 2-2h5l2 3h7a2 2 0 0 1 2 2v2'/>"
  "<path d='M2 19l3.2-8h17L19 19a2 2 0 0 1-1.9 1.4H4A2 2 0 0 1 2 19z'/>" TAIL },
{ "code", STROKE_HEAD
  "<polyline points='9 6 3.5 12 9 18'/><polyline points='15 6 20.5 12 15 18'/>" TAIL },
{ "lock", STROKE_HEAD
  "<rect x='4' y='10.5' width='16' height='10.5' rx='2'/>"
  "<path d='M8 10.5V7a4 4 0 0 1 8 0v3.5'/>" TAIL },

// ---- component glyphs ---------------------------------------------------
// Line drawings, tinted by whoever draws them. A 16 px square filled with one
// colour is not an icon, it is a swatch - and that is what the previous set
// rasterised to, because this SVG reader treats `fill` as a yes/no.
{ "transform", STROKE_HEAD
  "<polyline points='12 3 12 21'/><polyline points='3 12 21 12'/>"
  "<polyline points='9.6 5.4 12 3 14.4 5.4'/><polyline points='9.6 18.6 12 21 14.4 18.6'/>"
  "<polyline points='5.4 9.6 3 12 5.4 14.4'/><polyline points='18.6 9.6 21 12 18.6 14.4'/>"
  "<circle cx='12' cy='12' r='2.4'/>" TAIL },
{ "mesh", STROKE_HEAD
  "<path d='M12 2.8 20.6 7.4v9.2L12 21.2 3.4 16.6V7.4z'/>"
  "<path d='M3.4 7.4 12 12l8.6-4.6'/><line x1='12' y1='12' x2='12' y2='21.2'/>"
  "<path d='M7.7 5.1 16.3 9.7'/>" TAIL },
{ "collider", STROKE_HEAD
  "<path d='M12 2.8 20.6 7.4v9.2L12 21.2 3.4 16.6V7.4z' stroke-dasharray='2.6 2'/>"
  "<circle cx='12' cy='12' r='1.6'/>" TAIL },
{ "body", STROKE_HEAD
  "<circle cx='12' cy='7.5' r='3.6'/>"
  "<line x1='12' y1='12.4' x2='12' y2='19'/><polyline points='8.6 15.8 12 19.4 15.4 15.8'/>" TAIL },
{ "braces", STROKE_HEAD
  "<path d='M9.4 3.5C7 3.5 7.6 8.6 6 10.2c-.7.7-1.4 1-2 1.8.6.8 1.3 1.1 2 1.8 1.6 1.6 1 6.7 3.4 6.7'/>"
  "<path d='M14.6 3.5c2.4 0 1.8 5.1 3.4 6.7.7.7 1.4 1 2 1.8-.6.8-1.3 1.1-2 1.8-1.6 1.6-1 6.7-3.4 6.7'/>" TAIL },
{ "type", STROKE_HEAD
  "<polyline points='4.5 7.4 4.5 4.8 19.5 4.8 19.5 7.4'/>"
  "<line x1='12' y1='4.8' x2='12' y2='19.2'/>"
  "<line x1='8.6' y1='19.2' x2='15.4' y2='19.2'/>" TAIL },
{ "cam", STROKE_HEAD
  "<rect x='2.6' y='7.4' width='12.6' height='9.2' rx='2'/>"
  "<path d='M15.2 11 21.4 7.6v8.8L15.2 13z'/>" TAIL },
{ "bulb", STROKE_HEAD
  "<path d='M9 17.5a6.5 6.5 0 1 1 6 0'/><line x1='9.4' y1='19.4' x2='14.6' y2='19.4'/>"
  "<line x1='10.2' y1='21.6' x2='13.8' y2='21.6'/>" TAIL },
{ "speaker", STROKE_HEAD
  "<path d='M11 5 6.4 9H3v6h3.4L11 19z'/>"
  "<path d='M15 9.4a4 4 0 0 1 0 5.2'/><path d='M17.8 6.8a8 8 0 0 1 0 10.4'/>" TAIL },
{ "surface", STROKE_HEAD
  "<circle cx='12' cy='12' r='8.6'/><path d='M4.6 16.2A8.6 8.6 0 0 1 16.2 4.6'/>"
  "<circle cx='15.4' cy='8.6' r='1.5'/>" TAIL },
{ "quad", STROKE_HEAD
  "<rect x='3.2' y='4.6' width='17.6' height='14.8' rx='2'/>"
  "<circle cx='8.4' cy='9.4' r='1.6'/><path d='M20.8 15.6 15.2 10 6 19.4'/>" TAIL },
{ "package", STROKE_HEAD
  "<path d='M12 2.8 20.6 7.4v9.2L12 21.2 3.4 16.6V7.4z'/>"
  "<path d='M3.4 7.4 12 12l8.6-4.6'/><line x1='12' y1='12' x2='12' y2='21.2'/>"
  "<circle cx='12' cy='12' r='2'/>" TAIL },
{ "folder", STROKE_HEAD
  "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>" TAIL },
// The same folder with something in it. Unity draws an empty folder hollow and
// a full one solid, and that one difference answers "is there anything in
// here" without opening it - which is most of what a file browser is for.
{ "folder-full", FILL_HEAD
  "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>" TAIL },
{ "file", STROKE_HEAD
  "<path d='M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z'/>"
  "<polyline points='13 2 13 9 20 9'/>" TAIL },
};

const uint32_t BUILTIN_COUNT = (uint32_t)(sizeof(BUILTIN) / sizeof(BUILTIN[0]));

// Some icons carry their own color: a warning is yellow and an error is red
// in every theme, and tinting them with the text color is how they turned
// gray. Written as shape overrides onto the parsed document at load.
const struct { const char *name; uint32_t shape; uint32_t rgba; } COLOR_RULES[] = {
    { "warning", 0, 0xFF36C8F0u },
    { "warning", 1, 0xFF1A1A1Au },
    { "warning", 2, 0xFF1A1A1Au },
    { "error",   0, 0xFF4B55D9u },
    { "error",   1, 0xFFFFFFFFu },
    { "error",   2, 0xFFFFFFFFu },
    { "info",    0, 0xFFCE8C4Cu },
    { "info",    1, 0xFFFFFFFFu },
    { "info",    2, 0xFFFFFFFFu },
    { "camera",  0, 0xFFCE8C4Cu },
    { "camera",  1, 0xFFFFFFFFu },
    { "light",   0, 0xFF36C8F0u },
    { "light",   1, 0xFF36C8F0u },
    { "volume",  0, 0xFF5FD47Cu },
    { "volume",  1, 0xFF5FD47Cu },
    { "volume",  2, 0xFF5FD47Cu },
    { "volume-x",0, 0xFF4B55D9u },
    { "volume-x",1, 0xFF4B55D9u },
    { "volume-x",2, 0xFF4B55D9u },
};
const uint32_t COLOR_RULES_COUNT = (uint32_t)(sizeof(COLOR_RULES) / sizeof(COLOR_RULES[0]));

struct Icon {
    std::string name;
    std::vector<uint8_t> px;      // size x size coverage
    std::vector<uint8_t> rgba;    // size x size x 4, colored icons only
    bool  colored = false;        // carries its own colors, never tinted
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

} // namespace

struct dai_icons {
    float size = 16.0f;
    int   cell = 18;
    std::vector<Icon> icons;
    std::vector<uint8_t> atlas;
    std::vector<uint8_t> atlas_rgba;
    uint32_t aw = 0, ah = 0;
    float disp = 0.0f;   // logical size, 0 = same as `size`
    bool dirty = true;
};

namespace {

void repack(dai_icons *ic) {
    if (!ic->dirty) return;
    ic->dirty = false;
    uint32_t n = (uint32_t)ic->icons.size();
    if (!n) { ic->aw = ic->ah = 0; ic->atlas.clear(); return; }

    // A grid, not a shelf packer: every icon is the same square, so the
    // clever version would produce exactly the same layout.
    uint32_t cols = 1;
    while (cols * cols < n) ++cols;
    uint32_t rows = (n + cols - 1) / cols;
    uint32_t w = 1, h = 1;
    while (w < cols * (uint32_t)ic->cell) w *= 2;
    while (h < rows * (uint32_t)ic->cell) h *= 2;
    ic->aw = w; ic->ah = h;
    ic->atlas.assign((size_t)w * h, 0);
    ic->atlas_rgba.clear();

    int s = (int)ic->size;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t cx = (i % cols) * (uint32_t)ic->cell;
        uint32_t cy = (i / cols) * (uint32_t)ic->cell;
        Icon &it = ic->icons[i];
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x)
                ic->atlas[(size_t)(cy + 1 + (uint32_t)y) * w + (cx + 1 + (uint32_t)x)] =
                    it.px[(size_t)y * s + x];
        // Half a texel in from the edge of the icon: sampling exactly on the
        // boundary picks up the neighbour's first column when the quad is
        // scaled, which is how an icon grows a stray line down one side.
        it.u0 = (float)(cx + 1) / (float)w;
        it.v0 = (float)(cy + 1) / (float)h;
        it.u1 = (float)(cx + 1 + (uint32_t)s) / (float)w;
        it.v1 = (float)(cy + 1 + (uint32_t)s) / (float)h;
    }
}

int add_svg(dai_icons *ic, const char *name, const char *svg) {
    if (!ic || !name || !svg) return 0;
    char err[128];
    dai_svg *doc = dai_svg_parse(svg, 0, err, sizeof(err));
    if (!doc) return 0;
    for (uint32_t ri = 0; ri < COLOR_RULES_COUNT; ++ri) {
        if (std::strcmp(COLOR_RULES[ri].name, name) != 0) continue;
        dai_svg_shape_color(doc, COLOR_RULES[ri].shape, COLOR_RULES[ri].rgba);
    }
    bool colored = false;
    for (uint32_t si = 0; si < dai_svg_shape_count(doc); ++si)
        if (dai_svg_shape_color_get(doc, si)) { colored = true; break; }
    int s = (int)ic->size;
    Icon it;
    it.name = name;
    it.colored = colored;
    it.px.assign((size_t)s * s, 0);
    if (colored) {
        it.rgba.assign((size_t)s * s * 4, 0);
        dai_svg_rasterize_rgba(doc, it.rgba.data(), s, s, ic->size * (1.0f / 16.0f));
    }
    // One pixel of padding inside the cell: round caps and joins reach half a
    // stroke past the path, and a 2 unit stroke on a 24 unit box that touches
    // the edge would otherwise lose its outer half.
    int ok = dai_svg_rasterize(doc, it.px.data(), s, s, ic->size * (1.0f / 16.0f));
    dai_svg_free(doc);
    if (!ok) return 0;

    for (Icon &existing : ic->icons) {
        if (existing.name == name) { existing.px = std::move(it.px); ic->dirty = true; return 1; }
    }
    ic->icons.push_back(std::move(it));
    ic->dirty = true;
    return 1;
}

} // namespace

dai_icons *dai_icons_create(float pixel_size) {
    if (pixel_size < 6.0f) pixel_size = 6.0f;
    if (pixel_size > 256.0f) pixel_size = 256.0f;
    dai_icons *ic = new dai_icons();
    ic->size = std::floor(pixel_size + 0.5f);
    ic->cell = (int)ic->size + 2;
    ic->icons.reserve(BUILTIN_COUNT);
    for (uint32_t i = 0; i < BUILTIN_COUNT; ++i)
        add_svg(ic, BUILTIN[i].name, BUILTIN[i].svg);
    repack(ic);
    return ic;
}

void dai_icons_free(dai_icons *ic) { delete ic; }

int dai_icons_add(dai_icons *ic, const char *name, const char *svg_text) {
    if (!add_svg(ic, name, svg_text)) return 0;
    repack(ic);
    return 1;
}

int dai_icons_add_file(dai_icons *ic, const char *name, const char *path) {
    if (!ic || !path) return 0;
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return 0; }
    std::string buf((size_t)n, '\0');
    size_t got = std::fread(&buf[0], 1, (size_t)n, f);
    std::fclose(f);
    buf.resize(got);
    return dai_icons_add(ic, name, buf.c_str());
}

const uint8_t *dai_icons_atlas(const dai_icons *ic, uint32_t *w, uint32_t *h) {
    if (!ic) return nullptr;
    repack(const_cast<dai_icons *>(ic));
    if (w) *w = ic->aw;
    if (h) *h = ic->ah;
    return ic->atlas.empty() ? nullptr : ic->atlas.data();
}

const uint8_t *dai_icons_atlas_rgba(dai_icons *ic, uint32_t *w, uint32_t *h) {
    if (!ic) return nullptr;
    repack(ic);
    if (ic->atlas_rgba.size() != ic->atlas.size() * 4) {
        ic->atlas_rgba.resize(ic->atlas.size() * 4);
        for (size_t i = 0; i < ic->atlas.size(); ++i) {
            ic->atlas_rgba[i * 4 + 0] = 255;
            ic->atlas_rgba[i * 4 + 1] = 255;
            ic->atlas_rgba[i * 4 + 2] = 255;
            ic->atlas_rgba[i * 4 + 3] = ic->atlas[i];
        }
    }
    // Colored icons overwrite their cell with their own colors; everything
    // else stays white-with-alpha, which is what the widget tints.
    {
        uint32_t cols = 1;
        while (cols * cols < ic->icons.size()) ++cols;
        int s = (int)ic->size;
        for (uint32_t i = 0; i < ic->icons.size(); ++i) {
            const Icon &it = ic->icons[i];
            if (!it.colored || it.rgba.empty()) continue;
            uint32_t cx = (i % cols) * (uint32_t)ic->cell;
            uint32_t cy = (i / cols) * (uint32_t)ic->cell;
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x) {
                    size_t dst = ((size_t)(cy + 1 + (uint32_t)y) * ic->aw + (cx + 1 + (uint32_t)x)) * 4;
                    size_t src = ((size_t)y * s + x) * 4;
                    ic->atlas_rgba[dst + 0] = it.rgba[src + 0];
                    ic->atlas_rgba[dst + 1] = it.rgba[src + 1];
                    ic->atlas_rgba[dst + 2] = it.rgba[src + 2];
                    ic->atlas_rgba[dst + 3] = it.rgba[src + 3];
                }
        }
    }
    if (w) *w = ic->aw;
    if (h) *h = ic->ah;
    return ic->atlas_rgba.empty() ? nullptr : ic->atlas_rgba.data();
}

int dai_icons_uv(const dai_icons *ic, const char *name,
                 float *u0, float *v0, float *u1, float *v1) {
    if (!ic || !name) return 0;
    repack(const_cast<dai_icons *>(ic));
    for (const Icon &it : ic->icons) {
        if (it.name == name) {
            if (u0) *u0 = it.u0;
            if (v0) *v0 = it.v0;
            if (u1) *u1 = it.u1;
            if (v1) *v1 = it.v1;
            return 1;
        }
    }
    return 0;
}

float    dai_icons_size(const dai_icons *ic)  {
    return ic ? (ic->disp > 0.0f ? ic->disp : ic->size) : 0.0f;
}
void dai_icons_display_size(dai_icons *ic, float logical_px) {
    if (ic) ic->disp = logical_px > 0.0f ? logical_px : 0.0f;
}
int dai_icons_colored(const dai_icons *ic, const char *name) {
    if (!ic || !name) return 0;
    for (const Icon &it : ic->icons)
        if (it.name == name) return it.colored ? 1 : 0;
    return 0;
}

uint32_t dai_icons_count(const dai_icons *ic) { return ic ? (uint32_t)ic->icons.size() : 0; }

const char *dai_icons_name(const dai_icons *ic, uint32_t index) {
    if (!ic || index >= ic->icons.size()) return nullptr;
    return ic->icons[index].name.c_str();
}
