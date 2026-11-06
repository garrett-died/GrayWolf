#pragma once

// The GUI's face of BlenderLauncher: Settings > Blender (blender.exe's path) and the "Open in
// Blender" / "Re-import" buttons of an exported map. Game-agnostic: they need only the export's
// files.

#include <string>

// The Settings page's Blender controls: blender.exe's path, Browse, find it again, what was found.
void DrawBlenderSettings();

// One map's buttons, MapFile being its .glb, its _import.py or its .blend: "Open in Blender" (the
// first time it imports the map and saves <map>.blend beside it) and, once that .blend exists,
// "Re-import". Nothing for a file that is not a map export.
void DrawOpenInBlender(const std::string& MapFile);

// The maps of an export folder (it and its maps\), each with those buttons; nothing when it
// holds none.
void DrawMapsInBlender(const std::string& Folder);
