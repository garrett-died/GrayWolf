#pragma once

// The GUI's face of Updater: the Settings page's update controls and the banner announcing a newer GrayWolf.

// The update controls of the Settings page (startup check, the gh token, Check now, the version).
void DrawUpdatesSettings();

// Below the main menu bar, while a newer app is known and not dismissed. Returns true when the
// user chose to restart into the update (the helper is started: the caller quits).
bool DrawUpdateBanner();
