#pragma once

// The Black Ops 3 skies of the user's Mod Tools, for the model preview's lighting (HydraX's HydraX.Application/Sky and
// HydraX.Preview/LightingPreset, ported): where the Mod Tools are, the chain that leads from a lighting state to its sky
// EXRs, the four states' numbers, and the bake of a sky into the preview's environment cubes.
//
//   source_data\ssi.gdt (ssi default_<state>) "skyboxmodel"
//     -> the xmodel's GDT entry "skinOverride" (its last name)
//       -> that material's "colorMap"
//         -> that image's "baseImage": an EXR, a cube face (_ft: its five siblings complete the cube) or a lat-long
//   with the planner's cube-face paths (Presets) when a link is missing.
//
// Everything here only READS the Mod Tools. A decoded sky never leaves memory and a baked one never leaves the cache
// folder (SkyCacheDir): no Mod Tools pixels are written anywhere else. No Mod Tools found is a normal result.
// Conventions (face axes, lat-long): docs/hydrax-port/sky-axes.md. Self-test: sky-bake-selftest.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "PreviewEnvironment.h"

namespace ModToolsSky
{
    // ---------------------------------------------------------------- the four lighting states

    enum class LightState { Morning, Day, Sunset, Night };
    inline constexpr int kStateCount = 4;
    inline constexpr LightState kStates[kStateCount] = { LightState::Morning, LightState::Day, LightState::Sunset, LightState::Night };
    const char* StateName(LightState S);   // "Morning" ... "Night"

    // One state: the sun and the exposure range, with HydraX's LightingPreset numbers (docs/references/apex/lighting-and-
    // environment.md: the values the Mod Tools' ssi.gdt and assetviewer.led hold)
    struct LightingPreset
    {
        LightState State;
        const char* Name;
        double SunPitch, SunYaw;       // degrees
        float SunColor[3];             // linear
        double SunStops;               // intensity = 2^stops
        double EvMin, EvMax;
        double ProbeExposure;          // the LED's global probe exposure
        float ProbeColor[3];           // the probe's average colour

        double SunIntensity() const;
        void SunDirection(float (&Out)[3]) const;   // (-cos p cos y, -cos p sin y, sin p), towards the sun
        double ExposureValue() const;            // clamp(log2(ProbeExposure) + 5, EvMin, EvMax)
        double Exposure() const;                 // 2^(ExposureValue - 3)
    };
    const LightingPreset& PresetOf(LightState S);
    // The mid grey HydraX exposes a state's sky to (linear): 0.18, Night 0.06 (calibrated by eye there, not the game's)
    float TargetMid(LightState S);

    // ---------------------------------------------------------------- where the Mod Tools are

    struct LocateInput
    {
        std::string Setting;                                    // the Mod Tools folder setting ("" : not set)
        std::function<std::string(const char*)> Env;            // reads an environment variable (null: the real one)
        std::vector<std::filesystem::path> SteamLibraries;      // null/empty with ScanSteam: found from the registry
        bool ScanSteam = true;
    };

    struct Location
    {
        std::string Root;
        std::string Source;                  // "setting", "TA_TOOLS_PATH" or "Steam"
        std::vector<std::string> Tried;      // every place looked at, in order, with why it was skipped
    };

    // A root is valid when it holds model_export\t7_skybox or share\raw\maps
    bool IsModToolsRoot(const std::filesystem::path& Root);
    // Setting, then TA_TOOLS_PATH, then every Steam library's Black Ops III folder (the app manifest's installdir first).
    // false: not found; Out.Tried says where it looked. Never an error.
    bool Locate(const LocateInput& Input, Location& Out);
    // Steam's install folder and every library its libraryfolders.vdf lists
    std::vector<std::filesystem::path> DetectSteamLibraries();

    // ---------------------------------------------------------------- the chain

    enum class SourceKind { CubeFaces, LatLong };

    struct SkySource
    {
        LightState State = LightState::Morning;
        SourceKind Kind = SourceKind::CubeFaces;
        std::vector<std::string> Faces;   // CubeFaces: six absolute paths, ft bk lf rt up dn
        std::string LatLong;              // LatLong: the file
        bool Fallback = false;            // the planner's cube-face paths, a link of the chain was missing
        bool Demo = false;                // a made-up gradient sky with a sun disc at the preset's sun (DemoCatalog), no file behind it
        std::string SkyboxModel, Material, Image;
        std::vector<std::string> Notes;   // what each link found
        std::vector<std::string> Files() const;
    };

    struct Catalog
    {
        bool Found = false;                          // Mod Tools located
        Location Where;
        std::map<LightState, SkySource> Sources;     // the states that resolved
        std::map<LightState, std::string> Missing;   // why each other state did not
        std::vector<std::string> Notes;              // the locator's, when nothing was found
    };

    // Locates the Mod Tools (Input) and resolves the four states. Only reads. Cancel is checked per GDT file.
    Catalog Discover(const LocateInput& Input, const std::atomic<bool>* Cancel = nullptr);
    // The same for a known root
    Catalog Resolve(const Location& Where, const std::atomic<bool>* Cancel = nullptr);

    // The four states as made-up skies (ui-shot's lighting script, the self-tests): no Mod Tools and no file needed
    Catalog DemoCatalog();

    // One GDT entry, and the scanner the chain uses (public for the self-test)
    struct GdtEntry
    {
        std::string Name, Gdf;
        std::map<std::string, std::string> Props;
        const std::string* Get(const std::string& Key) const;
    };
    using GdtKey = std::pair<std::string, std::string>;   // gdf, name
    void ScanGdt(const std::filesystem::path& File, std::vector<GdtKey>& Wanted, std::map<GdtKey, GdtEntry>& Found, const std::atomic<bool>* Cancel);

    // ---------------------------------------------------------------- decode and bake

    // The sky's EXRs into RGBA floats: six faces (each at most MaxFace per side, halved while reading), or a lat-long
    // (at most 4 * MaxFace by 2 * MaxFace). false with Error (an "exr.*" code and the file name) on a bad file.
    bool LoadSkyImage(const SkySource& Source, int MaxFace, EnvironmentImage& Out, std::string& Error, const std::atomic<bool>* Cancel);

    struct BakeSettings
    {
        std::string CacheDir;                       // "" : SkyCacheDir()
        int Size = 256;                             // the radiance cube's face
        int MaxFace = 1024;                         // the largest source face kept
        int64_t CapBytes = int64_t(200) * 1024 * 1024;
    };

    struct BakeOutcome
    {
        bool Ok = false;
        bool FromCache = false;
        std::string Id;           // the environment id (files <CacheDir>\<Id>_radiance.dds and _irradiance.dds)
        std::string Message;      // the failure, or the bake's report
    };

    // %LOCALAPPDATA%\GrayWolf\sky-cache
    std::string SkyCacheDir();
    // The id a source bakes to: sky-<state>-<hash of the files' paths, sizes and write times, the size and the bake's version>.
    // "" when a file is missing.
    std::string CacheId(const SkySource& Source, const BakeSettings& Settings);
    // Sets the preview's extra environment folder to the cache folder (PreviewEnvironment.h)
    void UseCacheForEnvironments(const std::string& CacheDir = std::string());
    // Deletes the oldest entries (a radiance and irradiance pair is one) until the folder is within CapBytes; Keep is spared.
    // Returns the bytes left.
    int64_t PruneCache(const std::string& CacheDir, int64_t CapBytes, const std::string& KeepId);

    // Decodes (unless cached) and bakes one state into the cache. Bake runs on the calling thread: SkyBakeJob is the one off it.
    BakeOutcome BakeState(const SkySource& Source, const BakeSettings& Settings, const std::atomic<bool>* Cancel,
                          const std::function<void(float)>& Progress);

    // A bake on a thread of its own: Cancel() stops it at the next row, the destructor cancels and joins.
    class SkyBakeJob
    {
    public:
        SkyBakeJob(const SkySource& Source, const BakeSettings& Settings);
        ~SkyBakeJob();
        SkyBakeJob(const SkyBakeJob&) = delete;
        SkyBakeJob& operator=(const SkyBakeJob&) = delete;
        void Cancel() { CancelFlag = true; }
        float Progress() const { return ProgressValue.load(); }
        bool Finished() const { return Done.load(); }
        const BakeOutcome& Outcome() const { return Result; }   // read after Finished()

    private:
        std::atomic<bool> CancelFlag{ false };
        std::atomic<float> ProgressValue{ 0.0f };
        std::atomic<bool> Done{ false };
        BakeOutcome Result;
        std::thread Worker;
    };

    // sky-bake-selftest
    int SelfTest(std::string& Failures);
}
