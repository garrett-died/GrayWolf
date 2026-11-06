#include "stdafx.h"

#include "SkyLighting.h"
#include "ModelPreviewGPU.h"   // PreviewLighting
#include "PreviewEnvironment.h"
#include "SettingsManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace SkyLighting
{
    using ModToolsSky::LightState;
    namespace fs = std::filesystem;

    namespace
    {
        // The sun as the preview's key light takes it. HydraX shades the sun as albedo / pi * SunRadiance * N.L (SunRadiance = colour *
        // 2^stops / exposure, Shading.Shade) and the preview's light as albedo * LightColor * N.L, so LightColor is SunRadiance / pi for the
        // same pixels. The sky is baked at the same units (SkyGain), so sun and sky keep HydraX's ratio.
        constexpr double kSunToPreview = 0.318309886183790671538;   // 1 / pi
    }

    const char* StateKey(LightState S)
    {
        switch (S)
        {
        case LightState::Morning: return "morning";
        case LightState::Day: return "day";
        case LightState::Sunset: return "sunset";
        case LightState::Night: return "night";
        }
        return "day";
    }

    bool ParseState(const std::string& Text, LightState& S)
    {
        for (LightState X : ModToolsSky::kStates)
            if (Text == StateKey(X)) { S = X; return true; }
        return false;
    }

    bool ParseColor(const std::string& Text, float (&Rgb)[3])
    {
        if (Text.size() != 7 || Text[0] != '#') return false;
        int V[3];
        for (int I = 0; I < 3; I++)
        {
            int Hi = 0, Lo = 0;
            const auto Hex = [](char C, int& Out) {
                if (C >= '0' && C <= '9') Out = C - '0';
                else if (C >= 'a' && C <= 'f') Out = C - 'a' + 10;
                else if (C >= 'A' && C <= 'F') Out = C - 'A' + 10;
                else return false;
                return true;
            };
            if (!Hex(Text[1 + I * 2], Hi) || !Hex(Text[2 + I * 2], Lo)) return false;
            V[I] = Hi * 16 + Lo;
        }
        for (int I = 0; I < 3; I++) Rgb[I] = (float)V[I] / 255.0f;
        return true;
    }

    std::string FormatColor(const float (&Rgb)[3])
    {
        char B[16];
        const auto To = [](float F) { return (int)std::lround(std::clamp(F, 0.0f, 1.0f) * 255.0f); };
        std::snprintf(B, sizeof(B), "#%02x%02x%02x", To(Rgb[0]), To(Rgb[1]), To(Rgb[2]));
        return B;
    }

    // ---------------------------------------------------------------- the menu

    Menu BuildMenu(const Status& St, const Choice& Current)
    {
        Menu Out;
        Out.FullbrightChecked = Current.Fullbright;
        const bool NoTools = !St.Looking && !St.Catalog.Found;
        if (NoTools)
            Out.NoToolsLine = "Black Ops III skies need the Mod Tools, and GrayWolf did not find them. Set their folder in Settings > Tools > Mod Tools folder.";
        for (LightState S : ModToolsSky::kStates)
        {
            StateRow R;
            R.State = S;
            R.Label = ModToolsSky::StateName(S);
            const auto Src = St.Catalog.Sources.find(S);
            const auto Failed = St.Failed.find(S);
            if (St.Looking) R.Reason = "Looking for the Mod Tools...";
            else if (NoTools) R.Reason = "No Mod Tools found. Set their folder in Settings > Tools > Mod Tools folder.";
            else if (Src == St.Catalog.Sources.end())
            {
                const auto Why = St.Catalog.Missing.find(S);
                R.Reason = "This sky was not found in the Mod Tools" + (Why != St.Catalog.Missing.end() && !Why->second.empty() ? ": " + Why->second : std::string(".")) ;
            }
            else if (Failed != St.Failed.end()) R.Reason = "This sky could not be prepared: " + Failed->second;
            else R.Enabled = true;
            R.Checked = Current.Sky && Current.State == S && !Current.Fullbright;
            R.Preparing = St.Baking && St.BakingState == S;
            R.Progress = R.Preparing ? St.Progress : 0.0f;
            Out.States.push_back(std::move(R));
        }
        return Out;
    }

    // ---------------------------------------------------------------- what a pick does to the lighting

    void ApplyBakedSky(PreviewLighting& L, LightState State, const std::string& SkyId, const std::string& LightId)
    {
        const ModToolsSky::LightingPreset& P = ModToolsSky::PresetOf(State);
        float Toward[3];
        P.SunDirection(Toward);
        const double K = P.SunIntensity() / P.Exposure() * kSunToPreview;
        L.Environment = LightId;
        L.BackdropEnvironment = SkyId;
        L.Sun.On = true;
        for (int I = 0; I < 3; I++)
        {
            L.Sun.Toward[(size_t)I] = Toward[I];
            L.Sun.Radiance[(size_t)I] = (float)(P.SunColor[I] * K);
        }
    }

    void ApplyEnvironment(PreviewLighting& L, const std::string& Id)
    {
        L.Environment = Id;
        L.BackdropEnvironment.clear();
        L.Sun.On = false;
    }

    void ApplyFullbright(PreviewLighting& L, bool On) { L.Fullbright = On; }

    // ---------------------------------------------------------------- the service

    namespace
    {
        std::atomic<bool> g_ShotMode{ false };   // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): ui-shot sets it before any Service exists
    }
    void SetShotModeDefault(bool On) { g_ShotMode = On; }

    struct Service::Discovery
    {
        std::atomic<bool> Cancel{ false };
        std::atomic<bool> Done{ false };
        ModToolsSky::Catalog Result;
        std::thread Worker;
        explicit Discovery(ModToolsSky::LocateInput Input)
        {
            Worker = std::thread([this, Input = std::move(Input)] {
                Result = ModToolsSky::Discover(Input, &Cancel);
                Done = true;
            });
        }
        ~Discovery()
        {
            Cancel = true;
            if (Worker.joinable()) Worker.join();
        }
    };

    Service::Service() = default;

    Service::~Service()
    {
        if (Disc) Disc->Cancel = true;
        if (Job) Job->Cancel();
        for (auto& R : Retired) R->Cancel();
        // the destructors join
    }

    std::string Service::CacheDirOrDefault() const { return CacheDir.empty() ? ModToolsSky::SkyCacheDir() : CacheDir; }

    void Service::Configure(bool IsDemo, const std::string& Dir)
    {
        Demo = IsDemo;
        CacheDir = Dir;
        ModToolsSky::UseCacheForEnvironments(CacheDirOrDefault());
        if (Demo)
        {
            Disc.reset();
            St.Looking = false;
            St.Catalog = ModToolsSky::DemoCatalog();
            St.Failed.clear();
        }
    }

    void Service::Init(PreviewLighting& L)
    {
        Inited = true;
        if (g_ShotMode) ShotMode = true;
        ModToolsSky::UseCacheForEnvironments(CacheDirOrDefault());
        Chosen.Env = L.Environment;
        Chosen.Fullbright = SettingsManager::GetSetting(kFullbrightKey, "false") == "true";
        ApplyFullbright(L, Chosen.Fullbright);
        LightState S;
        if (ParseState(SettingsManager::GetSetting(kStateKey, ""), S)) { HasRemembered = true; Remembered = S; }
        FolderSeen = ShotMode ? SettingsManager::GetSetting(kFolderKey, "") : std::string("\x01");   // not read yet: the first Tick looks
    }

    void Service::SetFolder(const std::string& Folder) { SettingsManager::SetSetting(kFolderKey, Folder); }

    void Service::StartDiscovery()
    {
        ModToolsSky::LocateInput In;
        In.Setting = SettingsManager::GetSetting(kFolderKey, "");
        Disc = std::make_unique<Discovery>(std::move(In));
        St.Looking = true;
        St.Failed.clear();
    }

    void Service::RetireBake()
    {
        if (Job)
        {
            Job->Cancel();
            Retired.push_back(std::move(Job));
        }
        St.Baking = false;
    }

    void Service::StartBake(LightState S)
    {
        RetireBake();
        Retired.clear();   // joined: a cancelled bake of the same sky shares its temporary folder, so it must be gone first (it stops at the next row)
        const auto Src = St.Catalog.Sources.find(S);
        if (Src == St.Catalog.Sources.end()) return;
        ModToolsSky::BakeSettings Settings;
        Settings.CacheDir = CacheDirOrDefault();
        Job = std::make_unique<ModToolsSky::SkyBakeJob>(Src->second, Settings);
        JobState = S;
        St.Baking = true;
        St.BakingState = S;
        St.Progress = 0.0f;
    }

    void Service::Tick(PreviewLighting& L)
    {
        if (!Inited) return;
        if (!Demo)
        {
            const std::string Folder = SettingsManager::GetSetting(kFolderKey, "");
            if (Folder != FolderSeen)
            {
                FolderSeen = Folder;
                StartDiscovery();
            }
        }
        if (Disc && Disc->Done)
        {
            St.Catalog = std::move(Disc->Result);
            St.Looking = false;
            Disc.reset();
        }
        // the remembered state, once it resolves (not again after it failed)
        if (HasRemembered && !Chosen.Sky && !St.Looking && St.Catalog.Sources.count(Remembered) != 0 && St.Failed.count(Remembered) == 0)
        {
            Chosen.Sky = true;
            Chosen.State = Remembered;
            StartBake(Remembered);
        }
        if (Job)
        {
            St.Progress = Job->Progress();
            if (Job->Finished())
            {
                const ModToolsSky::BakeOutcome& Out = Job->Outcome();
                if (Out.Ok && Chosen.Sky && Chosen.State == JobState) ApplyBakedSky(L, JobState, Out.Id, Out.LightId);
                else if (!Out.Ok && Out.Message != "cancelled")
                {
                    St.Failed[JobState] = Out.Message;
                    HasRemembered = false;
                    if (Chosen.Sky && Chosen.State == JobState) Chosen.Sky = false;   // the look it had stays
                }
                Job.reset();
                St.Baking = false;
            }
        }
        std::erase_if(Retired, [](const auto& R) { return R->Finished(); });
    }

    bool Service::Busy() const { return Disc != nullptr || (Job != nullptr && !Job->Finished()); }

    void Service::PickState(LightState S, PreviewLighting& L)
    {
        if (St.Catalog.Sources.count(S) == 0) return;
        HasRemembered = false;
        Chosen.Sky = true;
        Chosen.State = S;
        Chosen.Fullbright = false;
        ApplyFullbright(L, false);
        SettingsManager::SetSetting(kStateKey, StateKey(S));
        SettingsManager::SetSetting(kFullbrightKey, "false");
        St.Failed.erase(S);
        if (Job && JobState == S) return;   // already being prepared
        StartBake(S);
    }

    void Service::PickEnvironment(const std::string& Id, PreviewLighting& L)
    {
        HasRemembered = false;
        RetireBake();   // a bake under way is no longer wanted; the look changes now
        Chosen.Sky = false;
        Chosen.Env = Id;
        Chosen.Fullbright = false;
        ApplyEnvironment(L, Id);
        ApplyFullbright(L, false);
        SettingsManager::SetSetting(kStateKey, "");
        SettingsManager::SetSetting(kFullbrightKey, "false");
    }

    void Service::SetFullbright(bool On, PreviewLighting& L)
    {
        Chosen.Fullbright = On;
        ApplyFullbright(L, On);
        SettingsManager::SetSetting(kFullbrightKey, On ? "true" : "false");
    }

    Menu Service::GetMenu() const { return BuildMenu(St, Chosen); }
    Choice Service::Current() const { return Chosen; }
    Status Service::Snapshot() const { return St; }

    std::string Service::Label(const PreviewLighting& L) const
    {
        if (Chosen.Fullbright) return "Fullbright";
        if (Chosen.Sky) return ModToolsSky::StateName(Chosen.State);
        for (const auto& E : PreviewEnvironments())
            if (E.Id == L.Environment) return E.Name;
        return L.Environment;
    }

    std::string Service::FoundLine() const
    {
        if (St.Looking) return "Looking for the Mod Tools...";
        if (Demo) return "Made-up skies (screenshot run).";
        if (St.Catalog.Found)
        {
            std::string Out = "Found: " + St.Catalog.Where.Root + " (" + St.Catalog.Where.Source + ").";
            int Ready = (int)St.Catalog.Sources.size();
            Out += " " + std::to_string(Ready) + " of 4 skies found.";
            return Out;
        }
        std::string Out = "Not found.";
        if (!Inited) return "Not looked for yet.";
        return Out + " Looked at the folder setting, TA_TOOLS_PATH and the Steam libraries.";
    }

    // ---------------------------------------------------------------- self-test

    namespace
    {
        bool Near(float A, float B) { return std::fabs(A - B) < 1e-6f; }
    }

    bool SelfTest(std::string& Report)
    {
        bool Ok = true;
        const auto Fail = [&](const std::string& What) { Ok = false; Report += "FAIL " + What + "\n"; };
        SetEnvironmentVariableA("GRAYWOLF_SETTINGS_READONLY", "1");   // the settings of the machine are neither read back nor written

        // ---- the menu's states and the reasons for the greyed ones
        {
            Status St;
            St.Looking = true;
            Menu M = BuildMenu(St, Choice{});
            if (M.States.size() != 4) Fail("the menu has four states");
            for (const StateRow& R : M.States)
                if (R.Enabled || R.Reason.find("Looking") == std::string::npos) Fail("while looking, a state is greyed with the reason");
            if (!M.NoToolsLine.empty()) Fail("no 'not found' line while still looking");

            St.Looking = false;   // searched, nothing
            M = BuildMenu(St, Choice{});
            for (const StateRow& R : M.States)
                if (R.Enabled || R.Reason.find("Settings") == std::string::npos) Fail("no Mod Tools: a state is greyed and points to Settings");
            if (M.NoToolsLine.find("Mod Tools") == std::string::npos || M.NoToolsLine.find("Settings") == std::string::npos || M.NoToolsLine.find('\n') != std::string::npos)
                Fail("no Mod Tools: one plain line that points to Settings");

            St.Catalog = ModToolsSky::DemoCatalog();   // found, four skies
            M = BuildMenu(St, Choice{});
            for (const StateRow& R : M.States)
                if (!R.Enabled || !R.Reason.empty()) Fail("found: every state is on");
            if (!M.NoToolsLine.empty()) Fail("found: no 'not found' line");
            const char* Names[] = { "Morning", "Day", "Sunset", "Night" };
            for (size_t I = 0; I < 4; I++)
                if (M.States[I].Label != Names[I]) Fail("the states are Morning, Day, Sunset, Night in that order");

            St.Catalog.Sources.erase(LightState::Sunset);
            St.Catalog.Missing[LightState::Sunset] = "skybox_default_sunset has no image";
            St.Failed[LightState::Night] = "exr.bad_magic: x.exr";
            M = BuildMenu(St, Choice{});
            if (M.States[2].Enabled || M.States[2].Reason.find("skybox_default_sunset has no image") == std::string::npos) Fail("an unresolved state is greyed with the chain's reason");
            if (M.States[3].Enabled || M.States[3].Reason.find("exr.bad_magic") == std::string::npos) Fail("a failed bake is greyed with its reason");
            if (!M.States[0].Enabled || !M.States[1].Enabled) Fail("the other states stay on");
            if (!M.NoToolsLine.empty()) Fail("a missing state is not 'no Mod Tools'");

            Choice C;
            C.Sky = true;
            C.State = LightState::Day;
            St.Baking = true;
            St.BakingState = LightState::Day;
            St.Progress = 0.4f;
            M = BuildMenu(St, C);
            if (!M.States[1].Checked || M.States[0].Checked || !M.States[1].Preparing || !Near(M.States[1].Progress, 0.4f)) Fail("the chosen state is checked and shows its progress");
            C.Fullbright = true;
            M = BuildMenu(St, C);
            if (M.States[1].Checked || !M.FullbrightChecked) Fail("Fullbright takes the check from the state");
        }

        // ---- the remembered choice and the colour
        {
            for (LightState S : ModToolsSky::kStates)
            {
                LightState Back = LightState::Day;
                if (!ParseState(StateKey(S), Back) || Back != S) Fail("a state's key reads back");
            }
            LightState X = LightState::Night;
            if (ParseState("", X) || ParseState("noon", X) || X != LightState::Night) Fail("an empty or unknown key is no state");
            float C[3] = { 0.1f, 0.2f, 0.3f };
            if (ParseColor("#12", C) || ParseColor("12ab34", C) || ParseColor("#12ab3g", C) || !Near(C[0], 0.1f)) Fail("a bad colour is refused and leaves the value");
            if (!ParseColor("#ff8000", C) || !Near(C[0], 1.0f) || std::fabs(C[1] - 128.0f / 255.0f) > 1e-6f || !Near(C[2], 0.0f)) Fail("a colour reads");
            if (FormatColor(C) != "#ff8000") Fail("a colour writes back the same");

            // a Service with the made-up skies: a pick is remembered in the settings and comes back at the next start
            const fs::path Cache = fs::temp_directory_path() / "graywolf-ui-selftest-sky";
            std::error_code Ec;
            fs::remove_all(Cache, Ec);
            SettingsManager::SetSetting(kStateKey, "");
            SettingsManager::SetSetting(kFullbrightKey, "false");
            PreviewLighting L;
            const std::string StartEnv = L.Environment;
            {
                Service S;
                S.SetShotMode();
                S.Configure(true, Cache.string());
                S.Init(L);
                S.PickState(LightState::Sunset, L);
                if (SettingsManager::GetSetting(kStateKey, "") != "sunset") Fail("a picked state is remembered");
                if (L.Environment != StartEnv) Fail("the look stays until the bake is ready");
                for (int I = 0; I < 3000 && S.Busy(); I++) { Sleep(10); S.Tick(L); }
                S.Tick(L);
                if (L.BackdropEnvironment.rfind("sky-sunset-", 0) != 0 || L.Environment != ModToolsSky::LightId(LightState::Sunset) || !L.Sun.On) Fail("a finished bake sets the sky behind, the state's light and the sun");
                else
                {
                    float Toward[3];
                    ModToolsSky::PresetOf(LightState::Sunset).SunDirection(Toward);
                    if (!Near(L.Sun.Toward[0], Toward[0]) || !Near(L.Sun.Toward[2], Toward[2])) Fail("the sun is the preset's");
                    if (L.Sun.Radiance[0] <= 0.0f) Fail("the sun has light");
                }
                if (!fs::exists(Cache / (L.BackdropEnvironment + "_radiance.dds"), Ec) || !fs::exists(Cache / (L.Environment + "_irradiance.dds"), Ec)) Fail("the bake is in the cache folder");

                // a bake switched away from: the look changes at once and the bake does not land
                S.PickState(LightState::Night, L);
                S.PickEnvironment(kStudioEnvironment, L);
                for (int I = 0; I < 300; I++) { Sleep(10); S.Tick(L); }
                if (L.Environment != kStudioEnvironment || L.Sun.On || !L.BackdropEnvironment.empty()) Fail("switching away from a bake keeps the new pick (no late sky)");
                if (SettingsManager::GetSetting(kStateKey, "x") != "") Fail("an environment pick forgets the state");
            }
            {
                SettingsManager::SetSetting(kStateKey, "day");
                PreviewLighting L2;
                Service S;
                S.SetShotMode();
                S.Configure(true, Cache.string());
                S.Init(L2);
                S.Tick(L2);
                if (!S.Current().Sky || S.Current().State != LightState::Day) Fail("the remembered state is picked at the next start");
                for (int I = 0; I < 3000 && S.Busy(); I++) { Sleep(10); S.Tick(L2); }
                S.Tick(L2);
                if (L2.BackdropEnvironment.rfind("sky-day-", 0) != 0) Fail("the remembered sky is applied");
            }
            fs::remove_all(Cache, Ec);
        }

        // ---- Fullbright leaves the other values alone, so switching back restores them
        {
            PreviewLighting L;
            L.Exposure = 1.5f;
            L.Lights = 0.8f;
            L.Specular = false;
            L.EnvRotation = 40.0f;
            L.EnvIntensity = 2.0f;
            L.BloomStrength = 0.4f;
            L.Shadow.Softness = 0.9f;
            ApplyBakedSky(L, LightState::Day, "sky-day-0123456789abcdef", "skylight-day-v1");
            const PreviewLighting Before = L;
            ApplyFullbright(L, true);
            if (!L.Fullbright) Fail("Fullbright turns on");
            PreviewLighting Same = L;
            Same.Fullbright = false;
            if (!(Same == Before)) Fail("Fullbright changes no other lighting value");
            ApplyFullbright(L, false);
            if (!(L == Before)) Fail("switching Fullbright off restores the look exactly");
            ApplyEnvironment(L, kStudioEnvironment);
            if (L.Sun.On || L.Environment != kStudioEnvironment || L.Exposure != 1.5f) Fail("a shipped environment turns the sun off and keeps the rest");
        }
        {
            // the exposure rule: Day's sun is 2^14 over its exposure 2^(log2(1941.254) + 5 - 3) = 4 * 1941.254 (inside the EV range), then the preview's scale
            PreviewLighting L;
            ApplyBakedSky(L, LightState::Day, "sky-day-x", "skylight-day-v1");
            const double Want = 16384.0 / (1941.2540 * 4.0) / 3.14159265358979;
            if (std::fabs(L.Sun.Radiance[0] - Want) > 0.002) Fail("the sun follows the preset's stops and exposure rule");
        }
        {
            // the sky and the sun turn together: a quarter turn of the environment takes the sun from +X to -Y
            PreviewLighting::SunLight Sun;
            Sun.Toward = { 1.0f, 0.0f, 0.5f };
            const auto Q = TurnedSunDirection(Sun, 90.0f);
            if (!Near(Q[0], 0.0f) && std::fabs(Q[0]) > 1e-5f) Fail("a turned sun: x");
            if (std::fabs(Q[1] + 1.0f) > 1e-5f || !Near(Q[2], 0.5f)) Fail("a turned sun: -Y, height kept");
            const auto Z = TurnedSunDirection(Sun, 0.0f);
            if (!Near(Z[0], 1.0f) || !Near(Z[1], 0.0f)) Fail("no turn keeps the sun");
        }
        char Line[96];
        std::snprintf(Line, sizeof(Line), "sky lighting menu ok=%d\n", Ok ? 1 : 0);
        Report += Line;
        return Ok;
    }
}
