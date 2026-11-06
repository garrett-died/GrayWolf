#include "stdafx.h"

#include "ModToolsSky.h"
#include "Strings.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <vector>

// sky-bake-selftest: the lighting numbers, the Mod Tools locator, the GDT scanner and the chain (on a Mod Tools tree built
// in a temp folder, with empty files where only the path matters), the sky axes against Direct3D's cube faces, the cache
// and the bake of tiny EXR skies. No game data. The cases port HydraX's SkyCatalogTests / SkyCubeLayoutTests (the
// lat-long that encodes its own direction, one distinct colour per face) onto GrayWolf's convention, docs/hydrax-port/sky-axes.md.
namespace ModToolsSky
{
namespace
{
    namespace fs = std::filesystem;

    struct Harness
    {
        std::string& Failures;
        int Failed = 0;
        void Check(bool Ok, const std::string& Name, const std::string& Got = std::string())
        {
            if (Ok) return;
            Failed++;
            Failures += std::format("FAIL SkyBake {} (got '{}')\n", Name, Got);
        }
    };

    std::string Num(double V) { return std::format("{:.6f}", V); }
    bool Near(double A, double B, double Tol) { return std::fabs(A - B) <= Tol; }

    void Touch(const fs::path& P, const std::string& Text = std::string())
    {
        std::error_code Ec;
        fs::create_directories(P.parent_path(), Ec);
        std::ofstream(P, std::ios::binary | std::ios::trunc) << Text;
    }

    // ---- a tiny uncompressed float EXR (R, G, B real32): the files the bake reads

    void Put32(std::string& B, uint32_t V) { for (int I = 0; I < 4; I++) B += (char)((V >> (8 * I)) & 0xFF); }
    void Attr(std::string& B, const std::string& Name, const std::string& Type, const std::string& Value)
    {
        B += Name; B += '\0'; B += Type; B += '\0'; Put32(B, (uint32_t)Value.size()); B += Value;
    }

    template <class F>
    void WriteExr(const fs::path& Path, int W, int H, F Pixel)
    {
        std::string B;
        Put32(B, 0x01312f76);
        B += std::string("\x02\x00\x00\x00", 4);
        std::string Ch;
        for (const char* Name : { "B", "G", "R" })
        {
            Ch += Name; Ch += '\0';
            Put32(Ch, 2); Put32(Ch, 0); Put32(Ch, 1); Put32(Ch, 1);
        }
        Ch += '\0';
        Attr(B, "channels", "chlist", Ch);
        Attr(B, "compression", "compression", std::string(1, '\0'));
        std::string Box;
        Put32(Box, 0); Put32(Box, 0); Put32(Box, (uint32_t)(W - 1)); Put32(Box, (uint32_t)(H - 1));
        Attr(B, "dataWindow", "box2i", Box);
        Attr(B, "displayWindow", "box2i", Box);
        Attr(B, "lineOrder", "lineOrder", std::string(1, '\0'));
        B += '\0';
        const size_t Table = B.size();
        B.append((size_t)H * 8, '\0');
        const size_t RowBytes = (size_t)W * 12;
        for (int Y = 0; Y < H; Y++)
        {
            const uint64_t At = B.size();
            for (int I = 0; I < 8; I++) B[Table + (size_t)Y * 8 + I] = (char)((At >> (8 * I)) & 0xFF);
            Put32(B, (uint32_t)Y);
            Put32(B, (uint32_t)RowBytes);
            for (int C = 0; C < 3; C++)   // B, G, R planes
                for (int X = 0; X < W; X++)
                {
                    float V = Pixel(X, Y, 2 - C);   // plane 0 is B (channel 2)
                    uint32_t Bits;
                    std::memcpy(&Bits, &V, 4);
                    Put32(B, Bits);
                }
        }
        std::error_code Ec;
        fs::create_directories(Path.parent_path(), Ec);
        std::ofstream(Path, std::ios::binary | std::ios::trunc).write(B.data(), (std::streamsize)B.size());
    }

    // ---- the axes, written out here from the format description and Direct3D's cube table (not from the code under test)

    // The Mod Tools' faces in game space: where each looks (forward), which way its columns run (right) and its rows climb (up)
    struct Axes { float F[3], R[3], U[3]; };
    const Axes kEngine[6] = {
        { { 1, 0, 0 },  { 0, -1, 0 }, { 0, 0, 1 } },   // ft
        { { -1, 0, 0 }, { 0, 1, 0 },  { 0, 0, 1 } },   // bk
        { { 0, 1, 0 },  { 1, 0, 0 },  { 0, 0, 1 } },   // lf
        { { 0, -1, 0 }, { -1, 0, 0 }, { 0, 0, 1 } },   // rt
        { { 0, 0, 1 },  { 0, 1, 0 },  { 1, 0, 0 } },   // up
        { { 0, 0, -1 }, { 0, -1, 0 }, { 1, 0, 0 } },   // dn
    };

    void Normalise(float (&D)[3])
    {
        const float L = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
        for (int I = 0; I < 3; I++) D[I] /= L;
    }

    // The direction of texel (X, Y) of a Mod Tools face of Size
    void EngineDir(int Face, int X, int Y, int Size, float (&D)[3])
    {
        const float S = 2.0f * (X + 0.5f) / Size - 1, T = 1.0f - 2.0f * (Y + 0.5f) / Size;
        for (int I = 0; I < 3; I++) D[I] = kEngine[Face].F[I] + S * kEngine[Face].R[I] + T * kEngine[Face].U[I];
        Normalise(D);
    }

    // Direct3D's cube face F (+X -X +Y -Y +Z -Z) texel direction, the standard table in the world's axes
    void D3dDir(int Face, int X, int Y, int Size, float (&D)[3])
    {
        const float U = 2.0f * (X + 0.5f) / Size - 1, V = 2.0f * (Y + 0.5f) / Size - 1;
        const float T[6][3] = { { 1, -V, -U }, { -1, -V, U }, { U, 1, V }, { U, -1, -V }, { U, -V, 1 }, { -U, -V, -1 } };
        for (int I = 0; I < 3; I++) D[I] = T[Face][I];
        Normalise(D);
    }

    // A source texel that encodes the direction it looks along, (d + 1) / 2
    void EncodeDir(const float (&D)[3], std::span<float, 4> Rgba)
    {
        Rgba[0] = (D[0] + 1) / 2; Rgba[1] = (D[1] + 1) / 2; Rgba[2] = (D[2] + 1) / 2; Rgba[3] = 1;
    }

    EnvironmentImage CubeOfDirections(int Size)
    {
        EnvironmentImage I;
        I.Kind = EnvironmentImage::Layout::CubeFaces;
        I.Width = I.Height = Size;
        I.Rgba.assign((size_t)6 * Size * Size * 4, 0.0f);
        for (int F = 0; F < 6; F++)
            for (int Y = 0; Y < Size; Y++)
                for (int X = 0; X < Size; X++)
                {
                    float D[3];
                    EngineDir(F, X, Y, Size, D);
                    EncodeDir(D, std::span<float, 4>(&I.Rgba[(((size_t)F * Size + Y) * Size + X) * 4], 4));
                }
        return I;
    }

    EnvironmentImage LatLongOfDirections(int W, int H)
    {
        EnvironmentImage I;
        I.Kind = EnvironmentImage::Layout::LatLong;
        I.Width = W;
        I.Height = H;
        I.Rgba.assign((size_t)W * H * 4, 0.0f);
        const double Pi = 3.14159265358979323846;
        for (int Y = 0; Y < H; Y++)
            for (int X = 0; X < W; X++)
            {
                const double Lon = (0.5 - (X + 0.5) / W) * 2 * Pi, Lat = (0.5 - (Y + 0.5) / H) * Pi;   // the image centre looks along +X, its left edge towards +Y, its top is +Z
                const float D[3] = { (float)(std::cos(Lat) * std::cos(Lon)), (float)(std::cos(Lat) * std::sin(Lon)), (float)std::sin(Lat) };
                EncodeDir(D, std::span<float, 4>(&I.Rgba[((size_t)Y * W + X) * 4], 4));
            }
        return I;
    }

    // Every texel of the source cube, as Direct3D's faces see it, must decode to its own world direction; the worst error
    int WorstDirectionError(const EnvironmentImage& Source, int Size, double& Worst, std::string& Where)
    {
        std::vector<float> Cube;
        if (!EnvironmentSourceCube(Source, Size, Cube)) return -1;
        Worst = 0;
        int Bad = 0;
        for (int F = 0; F < 6; F++)
            for (int Y = 0; Y < Size; Y++)
                for (int X = 0; X < Size; X++)
                {
                    float D[3], Want[4];
                    D3dDir(F, X, Y, Size, D);
                    EncodeDir(D, std::span<float, 4>(Want));
                    const std::span<const float, 4> Got(&Cube[(((size_t)F * Size + Y) * Size + X) * 4], 4);
                    double E = 0;
                    for (int C = 0; C < 3; C++) E = std::max(E, (double)std::fabs(Got[C] - Want[C]));
                    if (E > 0.03)
                    {
                        if (Bad == 0) Where = std::format("face {} texel ({},{}) got ({:.3f},{:.3f},{:.3f}) want ({:.3f},{:.3f},{:.3f})", F, X, Y, Got[0], Got[1], Got[2], Want[0], Want[1], Want[2]);
                        Bad++;
                    }
                    Worst = std::max(Worst, E);
                }
        return Bad;
    }

    // ================================================================ the cases

    void Lighting(Harness& T)
    {
        const struct { LightState S; double Pitch, Yaw, Stops, EvMin, EvMax, Probe; float Sun[3]; float Avg[3]; } Want[4] = {
            { LightState::Morning, 165, 263, 11.3, -32, 31, 442.3148, { 1, 0.7758f, 0.5088f }, { 0.9580f, 1.0122f, 1.0028f } },
            { LightState::Day, 125, 150, 14, 1, 16, 1941.2540, { 1, 0.9472f, 0.8879f }, { 0.7713f, 1.0135f, 1.5398f } },
            { LightState::Sunset, 158, 300, 11, 8, 12.5, 219.8245, { 1, 0.5512f, 0.2588f }, { 0.7665f, 1.0251f, 1.4388f } },
            { LightState::Night, 130, 140, -2.2, 3, 3.5, 0.022297, { 0.5892f, 1, 1 }, { 0.3945f, 1.1274f, 1.5211f } },
        };
        for (const auto& W : Want)
        {
            const LightingPreset& P = PresetOf(W.S);
            const std::string N = P.Name;
            T.Check(P.State == W.S && P.SunPitch == W.Pitch && P.SunYaw == W.Yaw && P.SunStops == W.Stops && P.EvMin == W.EvMin && P.EvMax == W.EvMax && P.ProbeExposure == W.Probe,
                    "LightingPreset " + N + ": the numbers of lighting-and-environment.md");
            bool Colours = true;
            for (int C = 0; C < 3; C++) Colours = Colours && P.SunColor[C] == W.Sun[C] && P.ProbeColor[C] == W.Avg[C];
            T.Check(Colours, "LightingPreset " + N + ": sun and probe colours");
        }
        T.Check(std::string(PresetOf(LightState::Morning).Name) == "Morning" && std::string(StateName(LightState::Night)) == "Night", "state names");

        // The sun direction (-cos p cos y, -cos p sin y, sin p): Day worked by hand (cos 125 = -0.573576, cos 150 = -0.866025)
        float D[3];
        PresetOf(LightState::Day).SunDirection(D);
        T.Check(Near(D[0], -0.496732, 1e-5) && Near(D[1], 0.286788, 1e-5) && Near(D[2], 0.819152, 1e-5), "Day sun direction", std::format("{} {} {}", D[0], D[1], D[2]));
        T.Check(Near(PresetOf(LightState::Day).SunIntensity(), 16384.0, 1e-9) && Near(PresetOf(LightState::Night).SunIntensity(), std::pow(2.0, -2.2), 1e-12), "sun intensity is 2^stops");

        // The exposure rule: ev = clamp(log2(probe) + 5, EvMin, EvMax), exposure = 2^(ev - 3)
        T.Check(Near(PresetOf(LightState::Day).ExposureValue(), 15.9227, 1e-3), "Day ev is log2(1941.254) + 5, inside 1..16", Num(PresetOf(LightState::Day).ExposureValue()));
        T.Check(Near(PresetOf(LightState::Morning).ExposureValue(), 13.7886, 1e-3), "Morning ev", Num(PresetOf(LightState::Morning).ExposureValue()));
        T.Check(PresetOf(LightState::Sunset).ExposureValue() == 12.5, "Sunset ev clamps to EvMax 12.5", Num(PresetOf(LightState::Sunset).ExposureValue()));
        T.Check(PresetOf(LightState::Night).ExposureValue() == 3.0 && PresetOf(LightState::Night).Exposure() == 1.0, "Night ev clamps to EvMin 3, exposure 2^0", Num(PresetOf(LightState::Night).ExposureValue()));
        T.Check(Near(PresetOf(LightState::Sunset).Exposure(), std::pow(2.0, 9.5), 1e-6), "Sunset exposure 2^(12.5 - 3)");
        T.Check(TargetMid(LightState::Day) == 0.18f && TargetMid(LightState::Night) == 0.06f, "target mid greys 0.18, Night 0.06 (SkyImage.cs:130)");
        // HydraX's model light: the probe colour times 0.55, Night x 0.1 (ProceduralEnvironment.cs:19-25); horizon (colour * 0.5 + 0.5) * gain; ground (0.35 0.32 0.28) * gain
        const ProceduralSky Day = ProceduralSkyOf(LightState::Day), Night = ProceduralSkyOf(LightState::Night);
        T.Check(Near(Day.Zenith[0], 0.7713 * 0.55, 1e-5) && Near(Day.Horizon[0], (0.7713 * 0.5 + 0.5) * 0.55, 1e-5) && Near(Day.Ground[1], 0.32 * 0.55, 1e-5), "Day's gradient is the probe colour times ProbeScale 0.55");
        T.Check(Near(Night.Zenith[0], 0.3945 * 0.055, 1e-5) && Near(Night.Ground[2], 0.28 * 0.055, 1e-5), "Night's gradient is a tenth of that");
        // the exposure estimate: the geometric mean luminance, a texel above 1000 (the sun disc) left out (SkyImage.cs:198-224)
        EnvironmentImage Flat;
        Flat.Kind = EnvironmentImage::Layout::CubeFaces;
        Flat.Width = Flat.Height = 2;
        Flat.Rgba.assign((size_t)6 * 4 * 4, 0.5f);
        Flat.Rgba[0] = Flat.Rgba[1] = Flat.Rgba[2] = 5000.0f;
        T.Check(Near(ExposureEstimate(Flat), 0.5001, 1e-3), "the exposure estimate leaves a texel above 1000 out", Num(ExposureEstimate(Flat)));
    }

    void Locator(Harness& T, const fs::path& Tmp)
    {
        const fs::path Good = Tmp / "tools-a", Good2 = Tmp / "tools-b", Empty = Tmp / "empty", Lib = Tmp / "lib";
        fs::create_directories(Good / "model_export" / "t7_skybox");
        fs::create_directories(Good2 / "share" / "raw" / "maps");
        fs::create_directories(Empty);
        T.Check(IsModToolsRoot(Good) && IsModToolsRoot(Good2) && !IsModToolsRoot(Empty), "a root holds model_export\\t7_skybox or share\\raw\\maps");

        const auto Env = [&](const std::string& Value) { return [Value](const char* Name) { return std::string(Name) == "TA_TOOLS_PATH" ? Value : std::string(); }; };
        Location L;
        LocateInput In;
        In.ScanSteam = false;
        In.Env = Env(Good2.string());
        In.Setting = Good.string();
        T.Check(Locate(In, L) && L.Source == "setting" && L.Root == Good.string(), "the setting wins over TA_TOOLS_PATH", L.Root);
        In.Setting = "\"" + Good.string() + "\"";
        T.Check(Locate(In, L) && L.Root == Good.string(), "a quoted setting is unquoted");
        In.Setting = Empty.string();
        T.Check(Locate(In, L) && L.Source == "TA_TOOLS_PATH" && L.Root == Good2.string(), "a stale setting falls through to TA_TOOLS_PATH", L.Source);
        T.Check(L.Tried.size() == 2 && L.Tried[0].find("no model_export") != std::string::npos, "the places tried say why one was skipped");
        In.Setting.clear();
        In.Env = Env(Empty.string());
        T.Check(!Locate(In, L) && L.Root.empty() && L.Tried.size() == 2, "nothing found is a plain false, with the places tried");

        // Steam: the app manifest's installdir first, then the default folder name; a library without it is skipped
        fs::create_directories(Lib / "steamapps" / "common" / "Black Ops 3 Renamed" / "model_export" / "t7_skybox");
        Touch(Lib / "steamapps" / "appmanifest_311210.acf", "\"AppState\"\n{\n\t\"installdir\"\t\t\"Black Ops 3 Renamed\"\n}\n");
        In.ScanSteam = true;
        In.SteamLibraries = { Tmp / "no-such-library", Lib };
        T.Check(Locate(In, L) && L.Source == "Steam" && fs::path(L.Root).filename() == "Black Ops 3 Renamed", "Steam: the manifest's installdir", L.Root);
        fs::create_directories(Tmp / "lib2" / "steamapps" / "common" / "Call of Duty Black Ops III" / "share" / "raw" / "maps");
        In.SteamLibraries = { Tmp / "lib2" };
        T.Check(Locate(In, L) && fs::path(L.Root).filename() == "Call of Duty Black Ops III", "Steam: the default folder name");
        In.Setting = Good.string();
        T.Check(Locate(In, L) && L.Source == "setting", "the setting is tried before Steam");
    }

    void Scanner(Harness& T, const fs::path& Tmp)
    {
        const fs::path F = Tmp / "scan.gdt";
        Touch(F,
              "\"a_model\" ( \"xmodel.gdf\" )\r\n{\r\n\t\"skinOverride\" \"mtl_x mtl_sky\"\r\n\t\"path\" \"C:\\\\dir\\\\file \\\"q\\\" tab\\tend\"\r\n\t\"skinOverride\" \"IGNORED\"\r\n}\r\n"
              "\"other\" ( \"xmodel.gdf\" )\r\n{\r\n\t\"skinOverride\" \"nope\"\r\n}\r\n"
              "\"a_model\" ( \"material.gdf\" )\r\n{\r\n\t\"colorMap\" \"img\"\r\n}\r\n"
              "\"a_model\" ( \"xmodel.gdf\" )\r\n{\r\n\t\"skinOverride\" \"second occurrence\"\r\n}\r\n");
        std::vector<GdtKey> Wanted = { { "xmodel.gdf", "a_model" }, { "material.gdf", "a_model" }, { "image.gdf", "missing" } };
        std::map<GdtKey, GdtEntry> Found;
        ScanGdt(F, Wanted, Found, nullptr);
        T.Check(Found.size() == 2 && Wanted.size() == 1 && Wanted[0].second == "missing", "the scanner returns only the wanted entries and leaves the rest wanted");
        const GdtEntry& E = Found[{ "xmodel.gdf", "a_model" }];
        T.Check(E.Get("skinOverride") && *E.Get("skinOverride") == "mtl_x mtl_sky", "the first occurrence of a key and of an entry wins", E.Get("skinOverride") ? *E.Get("skinOverride") : "none");
        T.Check(E.Get("path") && *E.Get("path") == "C:\\dir\\file \"q\" tab end", "values unescape: \\\\ and \\\" and \\t", E.Get("path") ? *E.Get("path") : "none");
        T.Check(Found.count({ "material.gdf", "a_model" }) == 1 && E.Get("nothing") == nullptr, "the gdf is part of the key; a missing key is null");
        std::vector<GdtKey> W2 = { { "xmodel.gdf", "a_model" } };
        std::map<GdtKey, GdtEntry> F2;
        ScanGdt(Tmp / "no-such.gdt", W2, F2, nullptr);
        T.Check(F2.empty() && W2.size() == 1, "a missing GDT file is not an error");
    }

    std::string Gdt(const std::string& Name, const std::string& Gdf, const std::string& Key, const std::string& Value)
    {
        return "\"" + Name + "\" ( \"" + Gdf + "\" )\n{\n\t\"" + Key + "\" \"" + Value + "\"\n}\n";
    }

    void Chain(Harness& T, const fs::path& Tmp)
    {
        const fs::path Root = Tmp / "chain-tools";
        const fs::path Sky = Root / "model_export" / "t7_skybox";
        // Day: the whole chain, to six faces named by the image's baseImage (a folder the planner does not know)
        Touch(Root / "source_data" / "ssi.gdt", Gdt("default_day", "ssi.gdf", "skyboxmodel", "my_day_sky") + Gdt("default_sunset", "ssi.gdf", "skyboxmodel", "my_sunset_sky"));
        Touch(Root / "model_export" / "t7_skybox.gdt", Gdt("my_day_sky", "xmodel.gdf", "skinOverride", "mtl_skybox_default mtl_my_day") +
                                                       Gdt("mtl_my_day", "material.gdf", "colorMap", "img_my_day_ft") +
                                                       Gdt("img_my_day_ft", "image.gdf", "baseImage", "model_export/t7_skybox/custom/my_day_ft.exr"));
        for (const char* S : { "ft", "bk", "lf", "rt", "up", "dn" }) Touch(Sky / "custom" / (std::string("my_day_") + S + ".exr"));
        // Sunset: its xmodel is in a GDT the planner does not name (found by the folder sweep), its image names a cube set that is incomplete,
        // so the baseImage lat-long is used
        Touch(Root / "model_export" / "zzz_other.gdt", Gdt("my_sunset_sky", "xmodel.gdf", "skinOverride", "mtl_a mtl_ss") +
                                                       Gdt("mtl_ss", "material.gdf", "colorMap", "img_ss_ft") +
                                                       Gdt("img_ss_ft", "image.gdf", "baseImage", "model_export\\\\t7_skybox\\\\custom\\\\ss_ll.exr") + Gdt("img_ss_ft", "image.gdf", "imageType", "wrong"));
        Touch(Sky / "custom" / "ss_ft.exr");   // one face of the set only
        Touch(Sky / "custom" / "ss_ll.exr");
        // Morning: no ssi entry, no xmodel: the planner's cube faces
        for (const char* S : { "ft", "bk", "lf", "rt", "up", "dn" }) Touch(Sky / "skybox_default" / (std::string("skybox_default_day_clear_0700_") + S + ".exr"));
        // Night: nothing at all

        Location Where;
        Where.Root = Root.string();
        Where.Source = "setting";
        const Catalog Cat = Resolve(Where);
        T.Check(Cat.Found && Cat.Sources.size() == 3 && Cat.Missing.size() == 1 && Cat.Missing.count(LightState::Night) == 1, "chain: three states resolve, Night is left out with a note", std::to_string(Cat.Sources.size()));

        const auto Day = Cat.Sources.find(LightState::Day);
        T.Check(Day != Cat.Sources.end() && Day->second.Kind == SourceKind::CubeFaces && !Day->second.Fallback && Day->second.SkyboxModel == "my_day_sky" && Day->second.Material == "mtl_my_day" &&
                    Day->second.Image == "img_my_day_ft",
                "chain: Day ssi -> skyboxmodel -> skinOverride (its last name) -> colorMap -> baseImage");
        if (Day != Cat.Sources.end() && Day->second.Faces.size() == 6)
        {
            T.Check(fs::path(Day->second.Faces[0]).filename() == "my_day_ft.exr" && fs::path(Day->second.Faces[1]).filename() == "my_day_bk.exr" && fs::path(Day->second.Faces[2]).filename() == "my_day_lf.exr" &&
                        fs::path(Day->second.Faces[3]).filename() == "my_day_rt.exr" && fs::path(Day->second.Faces[4]).filename() == "my_day_up.exr" && fs::path(Day->second.Faces[5]).filename() == "my_day_dn.exr",
                    "chain: the faces are in ft bk lf rt up dn order");
            T.Check(fs::path(Day->second.Faces[0]).parent_path().filename() == "custom", "chain: the faces come from the baseImage's folder");
        }
        else T.Check(false, "chain: Day has six faces");

        const auto Sun = Cat.Sources.find(LightState::Sunset);
        T.Check(Sun != Cat.Sources.end() && Sun->second.Kind == SourceKind::LatLong && fs::path(Sun->second.LatLong).filename() == "ss_ll.exr" && Sun->second.Material == "mtl_ss",
                "chain: Sunset, found by the folder sweep, an incomplete cube set falls back to the baseImage lat-long");
        const auto Morn = Cat.Sources.find(LightState::Morning);
        T.Check(Morn != Cat.Sources.end() && Morn->second.Fallback && Morn->second.Kind == SourceKind::CubeFaces && Morn->second.SkyboxModel == "skybox_default_day_clear_0700" &&
                    Morn->second.Material.empty(),
                "chain: Morning has no ssi, no xmodel: the planner's model and cube faces");
        T.Check(Cat.Missing.count(LightState::Night) && Cat.Missing.at(LightState::Night).find("missing the file") != std::string::npos, "chain: Night's note names the missing file",
                Cat.Missing.count(LightState::Night) ? Cat.Missing.at(LightState::Night) : "");
        bool Noted = Morn != Cat.Sources.end() && Morn->second.Notes.size() >= 3;
        T.Check(Noted, "chain: every state carries its notes (what each link found)");

        // No Mod Tools: not an error
        LocateInput None;
        None.ScanSteam = false;
        None.Env = [](const char*) { return std::string(); };
        const Catalog NoTools = Discover(None);
        T.Check(!NoTools.Found && NoTools.Sources.empty() && !NoTools.Notes.empty(), "no Mod Tools: an empty catalogue with the places tried, nothing thrown");
        None.Setting = Root.string();
        const Catalog Found = Discover(None);
        T.Check(Found.Found && Found.Sources.size() == 3, "Discover finds the setting's root and resolves it");
    }

    void Axes(Harness& T)
    {
        // One distinct colour per Mod Tools face: each Direct3D face must show the face that looks along its axis
        // (D3D +X -X +Y -Y +Z -Z = ft bk lf rt up dn). A mirrored or rotated sky fails here.
        {
            const int Size = 8;
            EnvironmentImage I;
            I.Kind = EnvironmentImage::Layout::CubeFaces;
            I.Width = I.Height = Size;
            I.Rgba.assign((size_t)6 * Size * Size * 4, 0.0f);
            for (int F = 0; F < 6; F++)
                for (int P = 0; P < Size * Size; P++)
                {
                    const size_t At = ((size_t)F * Size * Size + P) * 4;
                    I.Rgba[At] = (float)(F + 1);   // the face's number
                    I.Rgba[At + 3] = 1;
                }
            std::vector<float> Cube;
            T.Check(EnvironmentSourceCube(I, 8, Cube), "axes: the source cube builds");
            const char* Name[6] = { "+X is ft", "-X is bk", "+Y is lf", "-Y is rt", "+Z is up", "-Z is dn" };
            for (int F = 0; F < 6; F++)
            {
                bool All = Cube.size() == (size_t)6 * 8 * 8 * 4;
                for (int P = 0; All && P < 8 * 8; P++) All = std::fabs(Cube[((size_t)F * 64 + P) * 4] - (float)(F + 1)) < 1e-4f;
                T.Check(All, std::string("axes: Direct3D face ") + Name[F]);
            }
        }

        // Inside every face: each texel decodes to its own direction (orientation, not just which face)
        {
            double Worst = 0;
            std::string Where;
            const int Bad = WorstDirectionError(CubeOfDirections(64), 16, Worst, Where);
            T.Check(Bad == 0, "axes: cube faces keep their orientation (every texel lands on its own world direction)", Where + " bad=" + std::to_string(Bad));
        }
        {
            double Worst = 0;
            std::string Where;
            const int Bad = WorstDirectionError(LatLongOfDirections(512, 256), 16, Worst, Where);
            T.Check(Bad == 0, "axes: a lat-long that encodes its own direction lands on those directions (centre +X, left edge +Y, top +Z)", Where + " bad=" + std::to_string(Bad));
        }

        // A known direction: a bright blob at +Y (a quarter of the way across from the left edge, on the horizon)
        {
            const int W = 256, H = 128;
            EnvironmentImage I;
            I.Kind = EnvironmentImage::Layout::LatLong;
            I.Width = W;
            I.Height = H;
            I.Rgba.assign((size_t)W * H * 4, 0.1f);
            for (int Y = H / 2 - 4; Y < H / 2 + 4; Y++)
                for (int X = W / 4 - 4; X < W / 4 + 4; X++) I.Rgba[((size_t)Y * W + X) * 4] = 50.0f;
            std::vector<float> Cube;
            EnvironmentSourceCube(I, 16, Cube);
            const auto Centre = [&](int F) { return Cube[(((size_t)F * 16 + 8) * 16 + 8) * 4]; };
            T.Check(Centre(2) > 20.0f && Centre(3) < 1.0f && Centre(0) < 1.0f && Centre(1) < 1.0f && Centre(4) < 1.0f, "axes: a bright spot a quarter in from the left edge of a lat-long is +Y",
                    std::format("+X {} -X {} +Y {} -Y {} +Z {}", Centre(0), Centre(1), Centre(2), Centre(3), Centre(4)));
        }
    }

    void BakeAndCache(Harness& T, const fs::path& Tmp)
    {
        const fs::path Cache = Tmp / "cache";
        const fs::path Sky = Tmp / "bake-sky";
        // Six 16 x 16 faces, a flat colour each; a bright sun disc in 'up' that the bake must not pass on at full strength
        const auto MakeSet = [&](const std::string& Prefix, float Level) {
            const char* Suffix[6] = { "ft", "bk", "lf", "rt", "up", "dn" };
            for (int F = 0; F < 6; F++)
                WriteExr(Sky / (Prefix + "_" + Suffix[F] + ".exr"), 16, 16, [&](int X, int Y, int C) {
                    if (F == 4 && X == 8 && Y == 8) return 60000.0f;
                    return Level * (float)(F + 1) * (C == 0 ? 1.0f : 0.5f);
                });
        };
        MakeSet("a", 0.1f);
        SkySource A;
        A.State = LightState::Day;
        for (const char* S : { "ft", "bk", "lf", "rt", "up", "dn" }) A.Faces.push_back((Sky / (std::string("a_") + S + ".exr")).string());

        EnvironmentImage Image;
        std::string Error;
        T.Check(LoadSkyImage(A, 1024, Image, Error, nullptr) && Image.Kind == EnvironmentImage::Layout::CubeFaces && Image.Width == 16 && Image.Rgba.size() == (size_t)6 * 16 * 16 * 4, "bake: six EXR faces decode", Error);
        T.Check(Image.Rgba.size() > 4 && Near(Image.Rgba[0], 0.1, 1e-6) && Near(Image.Rgba[1], 0.05, 1e-6) && Near(Image.Rgba[(size_t)4 * 16 * 16 * 4 + ((8 * 16) + 8) * 4], 60000.0, 1.0), "bake: R is the first channel, the faces keep their order", Image.Rgba.empty() ? "" : Num(Image.Rgba[0]));

        BakeSettings S;
        S.CacheDir = Cache.string();
        S.Size = 16;
        std::vector<float> Seen;
        const BakeOutcome First = BakeState(A, S, nullptr, [&](float V) { Seen.push_back(V); });
        T.Check(First.Ok && !First.FromCache && !First.Id.empty(), "bake: a sky bakes", First.Message);
        std::error_code Ec;
        T.Check(fs::exists(Cache / (First.Id + "_radiance.dds"), Ec) && !fs::exists(Cache / (First.Id + "_irradiance.dds"), Ec) && fs::file_size(Cache / (First.Id + "_radiance.dds"), Ec) > 2000,
                "bake: the sky is one sharp cube in the cache (with its mips)");
        T.Check(First.LightId == LightId(LightState::Day) && fs::exists(Cache / (First.LightId + "_radiance.dds"), Ec) && fs::exists(Cache / (First.LightId + "_irradiance.dds"), Ec),
                "bake: the state's light, a gradient, is beside it");
        T.Check(!Seen.empty() && std::is_sorted(Seen.begin(), Seen.end()) && Seen.back() == 1.0f, "bake: progress runs up to 1");
        size_t Strangers = 0;
        for (auto It = fs::directory_iterator(Cache, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
            if (It->path().extension() != ".dds") Strangers++;
        T.Check(Strangers == 0, "bake: the cache holds only the baked cubes (no temp folder, no source pixels)", std::to_string(Strangers));

        const BakeOutcome Second = BakeState(A, S, nullptr, nullptr);
        T.Check(Second.Ok && Second.FromCache && Second.Id == First.Id, "cache: the second request is a hit with the same key");

        // The key follows the files' size and write time
        MakeSet("a", 0.2f);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fs::last_write_time(Sky / "a_ft.exr", fs::file_time_type::clock::now(), Ec);
        T.Check(CacheId(A, S) != First.Id && !CacheId(A, S).empty(), "cache: a rewritten source gets a new key");
        const BakeOutcome Third = BakeState(A, S, nullptr, nullptr);
        T.Check(Third.Ok && !Third.FromCache && Third.Id != First.Id, "cache: ... and is baked again", Third.Message);
        T.Check(!fs::exists(Cache / (First.Id + "_radiance.dds"), Ec), "cache: the state's stale entry is dropped");
        BakeSettings Other = S;
        Other.Size = 32;
        T.Check(CacheId(A, Other) != CacheId(A, S), "cache: the key includes the bake size");
        SkySource Gone = A;
        Gone.Faces[2] = (Sky / "nope.exr").string();
        T.Check(CacheId(Gone, S).empty() && !BakeState(Gone, S, nullptr, nullptr).Ok, "cache: a missing face is a clean failure, not a key");

        // Eviction: oldest first, the pair together, the newest kept, under the cap
        MakeSet("b", 0.3f);
        SkySource B = A;
        B.State = LightState::Sunset;
        for (size_t F = 0; F < 6; F++) B.Faces[F] = (Sky / (std::string("b_") + std::vector<std::string>{ "ft", "bk", "lf", "rt", "up", "dn" }[F] + ".exr")).string();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const BakeOutcome Sunset = BakeState(B, S, nullptr, nullptr);
        T.Check(Sunset.Ok, "cache: a second state bakes", Sunset.Message);
        const int64_t One = [&] { int64_t N = 0; for (const char* K : { "_radiance.dds" }) N += (int64_t)fs::file_size(Cache / (Third.Id + K), Ec); return N; }();
        BakeSettings Tight = S;
        Tight.CapBytes = One + One / 2;
        const int64_t Left = PruneCache(Cache.string(), Tight.CapBytes, Sunset.Id);
        T.Check(Left <= Tight.CapBytes && fs::exists(Cache / (Sunset.Id + "_radiance.dds"), Ec) && !fs::exists(Cache / (Third.Id + "_radiance.dds"), Ec) && !fs::exists(Cache / (Third.Id + "_irradiance.dds"), Ec),
                "cache: over the cap the oldest pair goes, the kept entry stays", std::to_string(Left) + " of " + std::to_string(Tight.CapBytes));
        T.Check(PruneCache(Cache.string(), 0, Sunset.Id) == (int64_t)([&] { int64_t N = 0; for (const char* K : { "_radiance.dds" }) N += (int64_t)fs::file_size(Cache / (Sunset.Id + K), Ec); return N; }()),
                "cache: the kept entry is never pruned, even over a cap of 0");

        // Cancel: the bake stops, nothing is left half written
        std::atomic<bool> Cancel{ true };
        const fs::path Cache2 = Tmp / "cache2";
        BakeSettings S2 = S;
        S2.CacheDir = Cache2.string();
        const BakeOutcome Stopped = BakeState(A, S2, &Cancel, nullptr);
        size_t Left2 = 0;
        for (auto It = fs::recursive_directory_iterator(Cache2, Ec); !Ec && It != fs::recursive_directory_iterator(); It.increment(Ec))
            if (It->is_regular_file(Ec) && It->path().filename().string().rfind("skylight-", 0) != 0) Left2++;   // the state's gradient light is whole or absent, it is not the sky
        T.Check(!Stopped.Ok && Stopped.Message == "cancelled" && Left2 == 0, "bake: a cancelled bake reports it and leaves no sky file", Stopped.Message + " files=" + std::to_string(Left2));

        // A job on its own thread, cancelled by its destructor before it finishes, then one allowed to finish
        {
            SkyBakeJob Job(A, S2);
            Job.Cancel();
        }
        {
            SkyBakeJob Job(A, S2);
            for (int I = 0; I < 2000 && !Job.Finished(); I++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            T.Check(Job.Finished() && Job.Outcome().Ok && Job.Progress() == 1.0f, "bake: a job runs off the calling thread and finishes", Job.Outcome().Message);
        }

        // Faces that disagree in size
        WriteExr(Sky / "a_dn.exr", 8, 8, [](int, int, int) { return 1.0f; });
        EnvironmentImage Bad;
        std::string Err;
        T.Check(!LoadSkyImage(A, 1024, Bad, Err, nullptr) && Err.find("face_mismatch") != std::string::npos, "bake: faces of different sizes are refused", Err);
        SkySource Corrupt = A;
        Touch(Sky / "a_ft.exr", "not an exr");
        T.Check(!LoadSkyImage(Corrupt, 1024, Bad, Err, nullptr) && Err.find("exr.") == 0, "bake: a corrupt EXR is a typed failure naming the file", Err);

        // A lat-long source bakes too
        WriteExr(Sky / "ll.exr", 64, 32, [](int X, int Y, int C) { return 0.2f + 0.001f * (float)(X + Y + C); });
        SkySource LL;
        LL.State = LightState::Night;
        LL.Kind = SourceKind::LatLong;
        LL.LatLong = (Sky / "ll.exr").string();
        const BakeOutcome Night = BakeState(LL, S2, nullptr, nullptr);
        T.Check(Night.Ok && Night.Id.rfind("sky-night-", 0) == 0, "bake: a lat-long bakes, named for its state", Night.Message);
    }
}

int SelfTest(std::string& Failures)
{
    Harness T{ Failures };
    std::error_code Ec;
    const fs::path Tmp = fs::temp_directory_path() / std::format("graywolf-sky-selftest-{}", GetCurrentProcessId());
    fs::remove_all(Tmp, Ec);
    fs::create_directories(Tmp, Ec);

    Lighting(T);
    Locator(T, Tmp);
    Scanner(T, Tmp);
    Chain(T, Tmp);
    Axes(T);
    BakeAndCache(T, Tmp);

    fs::remove_all(Tmp, Ec);
    return T.Failed;
}
}
