#pragma once

#include <string>

// ui-record: the shell hidden on a folder, driven by a JSON script (steps: ImGuiShell.Record.cpp),
// one fixed time step a frame, written as <OutDir>/frame_NNNNN.png + audio.wav (what the player
// played) + timeline.txt (the frame each labelled step starts at). Frames are held while a
// selection loads.
struct UiRecordOptions
{
    std::string Script;
    std::string OutDir;
    int Width = 1920, Height = 1080;
    int Fps = 30;
    double HoldTimeout = 180;   // seconds a selection may load before recording goes on anyway
    // ui-shot: no frame sequence. Only a step with a "shot" name is written, as <OutDir>/<name>.png,
    // once the step has settled (nothing loading, a few frames drawn), and the settings file is not
    // written (GRAYWOLF_SETTINGS_READONLY).
    bool Shots = false;
    float Dpi = 0;   // the fonts' and sizes' scale; 0: the window's (the screen's), ui-shot: 1 unless asked
};
