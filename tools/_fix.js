var w = scene.find("Wheel");
var out = "wheel=" + w + " pos=" + node.getVec(w, "transform.position") + " kinder:";
var kids = 0;
for (var i = 0; i < editor.count(); i++) {
  var id = editor.at(i);
  var nm = node.getStr(id, "node.name");
  if (nm && nm.indexOf("Wheel.") === 0) { kids++; if (kids < 3) out += " " + nm + "@" + node.getVec(id, "transform.position"); }
}
out + "  anzahl=" + kids;
