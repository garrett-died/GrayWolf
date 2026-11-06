#pragma once

// The viewport's Lighting menu: the four BO3 sky states of the user's Mod Tools (ModToolsSky.h) beside Fullbright and the
// environments that ship with GrayWolf. This file is the logic, free of ImGui: what the menu holds and why an item is greyed,
// the remembered choice, the discovery and the bakes on threads of their own, and what a pick does to PreviewLighting.
// The drawing is PreviewEnvironmentUi.cpp. Self-test: ui-selftest (SkyLighting::SelfTest).
//
// Only reads the Mod Tools. Nothing here is an error: no Mod Tools found is a line in the menu, and the other environments
// keep working.

#include "ModToolsSky.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct PreviewLighting;

namespace SkyLighting
{
    // Settings keys (graywolf.json): the Mod Tools folder, the chosen state ("morning" to "night"; empty: an environment, see
    // preview_environment), Fullbright, the background colour ("#rrggbb"). The sky chip is preview_env_backdrop.
    inline constexpr const char* kFolderKey = "modtools_dir";
    inline constexpr const char* kStateKey = "preview_light";
    inline constexpr const char* kFullbrightKey = "preview_fullbright";
    inline constexpr const char* kBackgroundKey = "preview_background";

    // "morning" ... "night" (lower case); State is set and true when Text names one
    const char* StateKey(ModToolsSky::LightState S);
    bool ParseState(const std::string& Text, ModToolsSky::LightState& S);

    // "#rrggbb" (display colour, 0..1 each). false when Text is not that; Rgb unchanged then.
    bool ParseColor(const std::string& Text, float (&Rgb)[3]);
    std::string FormatColor(const float (&Rgb)[3]);

    // What the menu knows about the Mod Tools skies at one moment
    struct Status
    {
        bool Looking = false;                                   // the Mod Tools are being looked for
        ModToolsSky::Catalog Catalog;                           // the locator's and the chain's result
        std::map<ModToolsSky::LightState, std::string> Failed;  // a bake that failed: why
        bool Baking = false;
        ModToolsSky::LightState BakingState = ModToolsSky::LightState::Day;
        float Progress = 0.0f;
    };

    // What is chosen: a sky state (Sky), else the environment Env; Fullbright beside either
    struct Choice
    {
        bool Sky = false;
        ModToolsSky::LightState State = ModToolsSky::LightState::Day;
        std::string Env;
        bool Fullbright = false;
    };

    struct StateRow
    {
        ModToolsSky::LightState State = ModToolsSky::LightState::Day;
        std::string Label;
        bool Enabled = false;
        std::string Reason;      // when not Enabled: why, for the tooltip
        bool Checked = false;
        bool Preparing = false;  // being baked
        float Progress = 0.0f;
    };

    struct Menu
    {
        std::vector<StateRow> States;
        std::string NoToolsLine;   // one plain line when no Mod Tools were found ("" otherwise)
        bool FullbrightChecked = false;
    };

    Menu BuildMenu(const Status& St, const Choice& Current);

    // The look of a picked sky once baked, as HydraX draws a state: the sky (SkyId, the cache's cube) behind the model, the model lit by the
    // state's gradient (LightId: ProceduralSkyOf) and the sun (PresetOf: colour times 2^stops over the exposure, divided by pi for the preview's
    // light model). Nothing else of L changes.
    void ApplyBakedSky(PreviewLighting& L, ModToolsSky::LightState State, const std::string& SkyId, const std::string& LightId);
    // A shipped environment: the id, the sun off (the three lights back)
    void ApplyEnvironment(PreviewLighting& L, const std::string& Id);
    // Fullbright on or off. Touches nothing else: off, the look is as it was.
    void ApplyFullbright(PreviewLighting& L, bool On);

    // ui-shot: every Service made from now on looks for no Mod Tools of its own (a run reads nothing of the machine) until its script names a folder
    void SetShotModeDefault(bool On);

    class Service
    {
    public:
        Service();
        ~Service();   // cancels the discovery and every bake, then joins them
        Service(const Service&) = delete;
        Service& operator=(const Service&) = delete;

        // ui-shot: do not look for the Mod Tools unless the script gives a folder (the run reads no settings of the machine)
        void SetShotMode() { ShotMode = true; }
        // Before the first Tick: ui-shot's made-up skies, and a cache folder of its own (empty: SkyCacheDir()). Calls UseCacheForEnvironments.
        void Configure(bool Demo, const std::string& CacheDir);
        // Once, after the settings are read: the remembered Fullbright, and the remembered state is picked when it resolves
        void Init(PreviewLighting& L);
        // Every frame: notices a changed folder setting, collects the discovery and the bake, applies a finished bake
        void Tick(PreviewLighting& L);
        // A discovery or a bake is running
        bool Busy() const;

        void PickState(ModToolsSky::LightState S, PreviewLighting& L);
        void PickEnvironment(const std::string& Id, PreviewLighting& L);
        void SetFullbright(bool On, PreviewLighting& L);

        Menu GetMenu() const;
        Choice Current() const;
        // The menu's button text: "Fullbright", "Day", "Studio sky" (and a bake's percent)
        std::string Label(const PreviewLighting& L) const;
        // Where the Mod Tools were found, for the Settings page: a line, or why not
        std::string FoundLine() const;
        Status Snapshot() const;
        // ui-shot: the folder setting set in memory
        void SetFolder(const std::string& Folder);
        // The self-test's catalogue in place of the discovered one
        void InjectCatalogForTest(const ModToolsSky::Catalog& C) { St.Catalog = C; St.Looking = false; }

    private:
        struct Discovery;
        void StartDiscovery();
        void StartBake(ModToolsSky::LightState S);
        void RetireBake();
        std::string CacheDirOrDefault() const;

        Status St;
        Choice Chosen;
        bool Demo = false;
        bool Inited = false;
        std::string CacheDir;
        std::string FolderSeen;
        bool ShotMode = false;                 // ui-shot: no search of its own, the script says where
        bool HasRemembered = false;            // the remembered state still to be picked once it resolves
        ModToolsSky::LightState Remembered = ModToolsSky::LightState::Day;
        std::unique_ptr<Discovery> Disc;
        std::unique_ptr<ModToolsSky::SkyBakeJob> Job;
        ModToolsSky::LightState JobState = ModToolsSky::LightState::Day;
        std::vector<std::unique_ptr<ModToolsSky::SkyBakeJob>> Retired;   // cancelled, finishing: collected as they end, never waited for
        Choice Applied;   // the look on screen (a pick pending or failed is not it)
    };

    // ui-selftest
    bool SelfTest(std::string& Report);
}
