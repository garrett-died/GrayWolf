#include "stdafx.h"

#include "ModToolsSky.h"

#include "ExrReader.h"
#include "Strings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace ModToolsSky
{
namespace
{
    constexpr double Pi = 3.14159265358979323846;
    constexpr int kBakeVersion = 1;   // bump when the decode or bake changes what a source gives
    constexpr float kSunCutoff = 1000.0f;   // HydraX's SkyMath.ExposureCutoff: texels above it are the sun disc

    const LightingPreset kPresets[kStateCount] = {
        { LightState::Morning, "Morning", 165, 263, { 1.0f, 0.7758f, 0.5088f }, 11.3, -32, 31, 442.3148, { 0.9580f, 1.0122f, 1.0028f } },
        { LightState::Day,     "Day",     125, 150, { 1.0f, 0.9472f, 0.8879f }, 14,    1,   16, 1941.2540, { 0.7713f, 1.0135f, 1.5398f } },
        { LightState::Sunset,  "Sunset",  158, 300, { 1.0f, 0.5512f, 0.2588f }, 11,    8,   12.5, 219.8245, { 0.7665f, 1.0251f, 1.4388f } },
        { LightState::Night,   "Night",   130, 140, { 0.5892f, 1.0f, 1.0f },   -2.2,  3,   3.5, 0.022297, { 0.3945f, 1.1274f, 1.5211f } },
    };

    // The planner's known sky per state: the ssi preset, its sky xmodel, the folder and the prefix of its cube faces
    struct PresetPaths { const char* Ssi; const char* Model; const char* Folder; const char* Prefix; };
    const PresetPaths kPaths[kStateCount] = {
        { "default_morning", "skybox_default_day_clear_0700", "skybox_default", "skybox_default_day_clear_0700" },
        { "default_day", "skybox_default_day", "skybox_default", "skybox_default_day" },
        { "default_sunset", "skybox_default_sunset", "skybox_default", "skybox_default_sunset" },
        { "default_night", "skybox_default_night", "zm_factory\\Temp", "zm_skybox_factory_05" },
    };
    const char* const kFaceSuffix[6] = { "ft", "bk", "lf", "rt", "up", "dn" };

    std::string Lower(std::string S)
    {
        for (char& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    }
    bool EqualsNoCase(const std::string& A, const std::string& B) { return Lower(A) == Lower(B); }

    std::string Trim(const std::string& S)
    {
        size_t A = 0, B = S.size();
        while (A < B && std::isspace((unsigned char)S[A])) A++;
        while (B > A && std::isspace((unsigned char)S[B - 1])) B--;
        return S.substr(A, B - A);
    }

    std::vector<std::string> SplitWords(const std::string& S)
    {
        std::vector<std::string> Out;
        std::string Cur;
        for (char C : S)
        {
            if (C == ' ' || C == '\t') { if (!Cur.empty()) Out.push_back(Cur); Cur.clear(); }
            else Cur += C;
        }
        if (!Cur.empty()) Out.push_back(Cur);
        return Out;
    }

    std::string ReadRegString(HKEY Root, const wchar_t* SubKey, const wchar_t* Value, DWORD Extra)
    {
        HKEY Key = nullptr;
        if (RegOpenKeyExW(Root, SubKey, 0, KEY_READ | Extra, &Key) != ERROR_SUCCESS) return "";
        wchar_t Buf[1024] = {};
        DWORD Size = sizeof(Buf) - sizeof(wchar_t), Type = 0;
        const bool Ok = RegQueryValueExW(Key, Value, nullptr, &Type, reinterpret_cast<LPBYTE>(Buf), &Size) == ERROR_SUCCESS && (Type == REG_SZ || Type == REG_EXPAND_SZ);
        RegCloseKey(Key);
        return Ok ? Strings::ToNormalString(Buf) : "";
    }

    std::string UnescapeVdf(const std::string& Raw)
    {
        std::string P;
        for (size_t K = 0; K < Raw.size(); K++)   // the vdf doubles its backslashes
        {
            if (Raw[K] == '\\' && K + 1 < Raw.size() && Raw[K + 1] == '\\') K++;
            P += Raw[K];
        }
        return P;
    }

    bool Cancelled(const std::atomic<bool>* C) { return C && C->load(); }

    uint64_t Fnv(uint64_t H, const std::string& S)
    {
        for (unsigned char C : S) { H ^= C; H *= 1099511628211ull; }
        H ^= 0xFF; H *= 1099511628211ull;   // a separator: ("ab","c") is not ("a","bc")
        return H;
    }
}

// ======== the four states

const char* StateName(LightState S) { return PresetOf(S).Name; }
const LightingPreset& PresetOf(LightState S)
{
    const int I = (int)S;
    return kPresets[(I >= 0 && I < kStateCount) ? I : 0];
}
float TargetMid(LightState S) { return S == LightState::Night ? 0.06f : 0.18f; }

double LightingPreset::SunIntensity() const { return std::pow(2.0, SunStops); }
void LightingPreset::SunDirection(float (&Out)[3]) const
{
    const double P = SunPitch * Pi / 180, Y = SunYaw * Pi / 180;
    Out[0] = (float)(-std::cos(P) * std::cos(Y));
    Out[1] = (float)(-std::cos(P) * std::sin(Y));
    Out[2] = (float)std::sin(P);
}
double LightingPreset::ExposureValue() const { return std::clamp(std::log2(ProbeExposure) + 5, EvMin, EvMax); }
double LightingPreset::Exposure() const { return std::pow(2.0, ExposureValue() - 3); }

// ======== the locator

bool IsModToolsRoot(const fs::path& Root)
{
    std::error_code Ec;
    return fs::is_directory(Root / "model_export" / "t7_skybox", Ec) || fs::is_directory(Root / "share" / "raw" / "maps", Ec);
}

std::vector<fs::path> DetectSteamLibraries()
{
    std::vector<fs::path> Out;
    std::string Steam = ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
    if (Steam.empty()) Steam = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY);
    if (Steam.empty()) return Out;
    Out.push_back(fs::path(Steam));
    std::ifstream In(fs::path(Steam) / "steamapps" / "libraryfolders.vdf");
    const std::regex PathRe(R"RX("path"\s+"([^"]+)")RX");
    std::string Line;
    std::error_code Ec;
    while (std::getline(In, Line))
    {
        std::smatch M;
        if (!std::regex_search(Line, M, PathRe)) continue;
        const std::string P = UnescapeVdf(M[1].str());
        if (fs::is_directory(P, Ec)) Out.push_back(fs::path(P));
    }
    return Out;
}

bool Locate(const LocateInput& Input, Location& Out)
{
    Out = Location();
    const auto Env = [&](const char* Name) -> std::string {
        if (Input.Env) return Input.Env(Name);
        char Buf[1024] = {};
        const DWORD N = GetEnvironmentVariableA(Name, Buf, (DWORD)sizeof(Buf));
        return (N > 0 && N < sizeof(Buf)) ? std::string(Buf, N) : std::string();
    };
    const auto Explicit = [&](const std::string& Label, const std::string& Value, const char* Source) {
        std::string Root = Trim(Value);
        if (Root.size() >= 2 && Root.front() == '"' && Root.back() == '"') Root = Root.substr(1, Root.size() - 2);
        if (Root.empty()) { Out.Tried.push_back(Label + " (not set)"); return false; }
        if (IsModToolsRoot(fs::path(Strings::ToUnicodeString(Root))))
        {
            Out.Tried.push_back(Label + ": " + Root);
            Out.Root = Root;
            Out.Source = Source;
            return true;
        }
        Out.Tried.push_back(Label + ": " + Root + " (no model_export\\t7_skybox or share\\raw\\maps)");
        return false;
    };

    if (Explicit("the Mod Tools folder setting", Input.Setting, "setting")) return true;
    if (Explicit("TA_TOOLS_PATH", Env("TA_TOOLS_PATH"), "TA_TOOLS_PATH")) return true;

    if (Input.ScanSteam)
    {
        const std::vector<fs::path> Libraries = Input.SteamLibraries.empty() ? DetectSteamLibraries() : Input.SteamLibraries;
        if (Libraries.empty()) Out.Tried.push_back("Steam (not found in the registry)");
        std::error_code Ec;
        for (const fs::path& Library : Libraries)
        {
            std::vector<std::string> Names;
            std::ifstream Manifest(Library / "steamapps" / "appmanifest_311210.acf");
            const std::regex InstallRe(R"RX("installdir"\s+"([^"]+)")RX");
            std::string Line;
            while (std::getline(Manifest, Line))
            {
                std::smatch M;
                if (std::regex_search(Line, M, InstallRe)) Names.push_back(M[1].str());
            }
            Names.push_back("Call of Duty Black Ops III");
            std::set<std::string> Seen;
            for (const std::string& Name : Names)
            {
                if (Name.empty() || Name.find_first_of("/\\") != std::string::npos || Name.find("..") != std::string::npos) continue;
                if (!Seen.insert(Lower(Name)).second) continue;
                const fs::path Candidate = Library / "steamapps" / "common" / Name;
                if (IsModToolsRoot(Candidate))
                {
                    Out.Tried.push_back("Steam library: " + Candidate.string());
                    Out.Root = Candidate.string();
                    Out.Source = "Steam";
                    return true;
                }
                Out.Tried.push_back("Steam library: " + Candidate.string() + " (not a Mod Tools root)");
            }
        }
    }
    return false;
}

// ======== GDT

const std::string* GdtEntry::Get(const std::string& Key) const
{
    const auto It = Props.find(Key);
    return It == Props.end() ? nullptr : &It->second;
}

namespace
{
    // A quoted string at P (which must be at the opening quote), unescaped; false when there is none
    bool ReadQuoted(const std::string& Text, size_t& P, std::string& Out)
    {
        Out.clear();
        if (P >= Text.size() || Text[P] != '"') return false;
        P++;
        while (P < Text.size())
        {
            const char C = Text[P++];
            if (C == '"') return true;
            if (C == '\\' && P < Text.size())
            {
                const char Next = Text[P++];
                if (Next == '\\') Out += '\\';
                else if (Next == '"') Out += '"';
                else if (Next == 'n' || Next == 'r' || Next == 't') Out += ' ';
                else { Out += '\\'; Out += Next; }
            }
            else Out += C;
        }
        return false;   // unterminated
    }
}

void ScanGdt(const fs::path& File, std::vector<GdtKey>& Wanted, std::map<GdtKey, GdtEntry>& Found, const std::atomic<bool>* Cancel)
{
    std::ifstream In(File, std::ios::binary);
    if (!In) return;

    const auto Wants = [&](const GdtKey& K) { return std::find(Wanted.begin(), Wanted.end(), K) != Wanted.end(); };
    bool Open = false;
    GdtEntry Cur;
    const auto Flush = [&] {
        if (!Open) return;
        const GdtKey K{ Cur.Gdf, Cur.Name };
        Found.emplace(K, Cur);
        Wanted.erase(std::remove(Wanted.begin(), Wanted.end(), K), Wanted.end());
        Open = false;
    };

    std::string Line;
    size_t LineNo = 0;
    while (std::getline(In, Line))
    {
        if ((++LineNo & 0x1FFF) == 0 && Cancelled(Cancel)) return;
        const std::string Text = Trim(Line);
        if (Text.size() < 2 || Text[0] != '"') continue;

        if (Text.back() == ')')
        {
            // Header: "name" ( "gdf" )
            Flush();
            size_t P = 0;
            std::string Name, Gdf;
            if (!ReadQuoted(Text, P, Name)) continue;
            while (P < Text.size() && Text[P] == ' ') P++;
            if (P >= Text.size() || Text[P] != '(') continue;
            P++;
            while (P < Text.size() && Text[P] == ' ') P++;
            if (ReadQuoted(Text, P, Gdf) && Wants({ Gdf, Name }))
            {
                Cur = GdtEntry();
                Cur.Name = Name;
                Cur.Gdf = Gdf;
                Open = true;
            }
            continue;
        }

        if (Open)
        {
            size_t P = 0;
            std::string Key, Value;
            if (!ReadQuoted(Text, P, Key)) continue;
            while (P < Text.size() && Text[P] == ' ') P++;
            if (P >= Text.size() || !ReadQuoted(Text, P, Value)) continue;
            Cur.Props.emplace(Key, Value);   // the first occurrence wins
        }
    }
    Flush();
}

// ======== the chain

std::vector<std::string> SkySource::Files() const
{
    return Kind == SourceKind::CubeFaces ? Faces : std::vector<std::string>{ LatLong };
}

namespace
{
    // Looks entries up across GDT files: the preferred files first, the rest of the folder only when something is missing
    struct GdtResolver
    {
        const std::atomic<bool>* Cancel;
        std::map<GdtKey, GdtEntry> All;
        std::vector<fs::path> Hits;

        std::map<GdtKey, GdtEntry> Find(std::vector<GdtKey> Wanted, const std::vector<fs::path>& Preferred, const fs::path& Dir)
        {
            std::map<GdtKey, GdtEntry> Result;
            for (const GdtKey& K : std::vector<GdtKey>(Wanted))
            {
                const auto It = All.find(K);
                if (It == All.end()) continue;
                Result.emplace(K, It->second);
                Wanted.erase(std::remove(Wanted.begin(), Wanted.end(), K), Wanted.end());
            }

            std::vector<fs::path> Order;
            const auto AddOnce = [&](const fs::path& P) {
                for (const fs::path& Q : Order) if (EqualsNoCase(Q.string(), P.string())) return;
                Order.push_back(P);
            };
            for (const fs::path& P : Preferred) AddOnce(P);
            for (const fs::path& P : std::vector<fs::path>(Hits)) AddOnce(P);
            for (const fs::path& File : Order)
            {
                if (Wanted.empty() || Cancelled(Cancel)) break;
                Scan(File, Wanted, Result);
            }

            if (!Wanted.empty())
            {
                std::vector<fs::path> Files;
                std::error_code Ec;
                for (auto It = fs::directory_iterator(Dir, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
                    if (It->is_regular_file(Ec) && EqualsNoCase(It->path().extension().string(), ".gdt")) Files.push_back(It->path());
                std::sort(Files.begin(), Files.end(), [](const fs::path& A, const fs::path& B) { return Lower(A.string()) < Lower(B.string()); });
                for (const fs::path& File : Files)
                {
                    if (Wanted.empty() || Cancelled(Cancel)) break;
                    bool Skip = false;
                    for (const fs::path& Q : Order) Skip = Skip || EqualsNoCase(Q.string(), File.string());
                    if (!Skip) Scan(File, Wanted, Result);
                }
            }
            return Result;
        }

        void Scan(const fs::path& File, std::vector<GdtKey>& Wanted, std::map<GdtKey, GdtEntry>& Result)
        {
            const size_t Before = Result.size();
            ScanGdt(File, Wanted, Result, Cancel);
            if (Result.size() > Before)
            {
                bool Known = false;
                for (const fs::path& Q : Hits) Known = Known || EqualsNoCase(Q.string(), File.string());
                if (!Known) Hits.push_back(File);
                for (const auto& [K, E] : Result) All.emplace(K, E);
            }
        }
    };

    // "mtl_skybox_default mtl_skybox_default_day": the original material, then the sky material
    std::string SkyMaterialOf(const GdtEntry& Xmodel)
    {
        if (const std::string* Skin = Xmodel.Get("skinOverride"))
        {
            const auto W = SplitWords(*Skin);
            if (!W.empty()) return W.back();
        }
        if (const std::string* Mats = Xmodel.Get("materials"))
        {
            const auto W = SplitWords(*Mats);
            if (!W.empty()) return W.front();
        }
        return std::string();
    }

    std::vector<std::string> FacePaths(const fs::path& Dir, const std::string& Prefix)
    {
        std::vector<std::string> Out;
        for (const char* Suffix : kFaceSuffix) Out.push_back((Dir / (Prefix + "_" + Suffix + ".exr")).string());
        return Out;
    }

    bool Exists(const std::string& P)
    {
        std::error_code Ec;
        return fs::is_regular_file(fs::path(Strings::ToUnicodeString(P)), Ec);
    }

    bool EndsWithNoCase(const std::string& S, const std::string& Suffix)
    {
        return S.size() >= Suffix.size() && EqualsNoCase(S.substr(S.size() - Suffix.size()), Suffix);
    }

    // The files of one state; false with Failure when nothing exists
    bool ResolveFiles(const std::string& Root, LightState State, const std::string& Model, const std::string& Material, const std::string& Image,
                      const std::string& BaseImage, std::vector<std::string>& Notes, SkySource& Out, std::string& Failure)
    {
        const PresetPaths& Preset = kPaths[(int)State];
        std::string FirstMissing;
        Out = SkySource();
        Out.State = State;
        Out.SkyboxModel = Model;
        Out.Material = Material;
        Out.Image = Image;

        if (!Trim(BaseImage).empty())
        {
            std::string Norm = BaseImage;
            std::replace(Norm.begin(), Norm.end(), '/', '\\');
            const bool Rooted = Norm.size() > 1 && (Norm[1] == ':' || Norm[0] == '\\');
            const fs::path Absolute = Rooted ? fs::path(Norm) : fs::path(Strings::ToUnicodeString(Root)) / Norm;
            const fs::path Directory = Absolute.parent_path();
            const std::string FileName = Absolute.stem().string();
            std::string Prefix;
            bool HavePrefix = false;
            if (EndsWithNoCase(FileName, "_ft")) { Prefix = FileName.substr(0, FileName.size() - 3); HavePrefix = true; }
            else if (EndsWithNoCase(Image, "_ft")) { Prefix = Image.substr(0, Image.size() - 3); HavePrefix = true; }   // the image is named after its cube set even when its baseImage is the lat-long

            if (HavePrefix)
            {
                const auto Faces = FacePaths(Directory, Prefix);
                const auto Missing = std::find_if(Faces.begin(), Faces.end(), [](const std::string& F) { return !Exists(F); });
                if (Missing == Faces.end())
                {
                    Notes.push_back("cube faces " + Prefix + "_{ft,bk,lf,rt,up,dn}.exr");
                    Out.Kind = SourceKind::CubeFaces;
                    Out.Faces = Faces;
                    Out.Notes = Notes;
                    return true;
                }
                FirstMissing = *Missing;
                Notes.push_back("cube set " + Prefix + " incomplete (missing " + fs::path(*Missing).filename().string() + ")");
            }

            std::error_code Ec;
            if (fs::is_regular_file(Absolute, Ec) && !EndsWithNoCase(FileName, "_ft"))
            {
                Notes.push_back("lat-long " + Absolute.filename().string());
                Out.Kind = SourceKind::LatLong;
                Out.LatLong = Absolute.string();
                Out.Notes = Notes;
                return true;
            }
            if (FirstMissing.empty()) FirstMissing = Absolute.string();
        }

        // Fallback: the planner's known cube-face paths
        const fs::path FallbackDir = fs::path(Strings::ToUnicodeString(Root)) / "model_export" / "t7_skybox" / Preset.Folder;
        const auto Faces = FacePaths(FallbackDir, Preset.Prefix);
        const auto Missing = std::find_if(Faces.begin(), Faces.end(), [](const std::string& F) { return !Exists(F); });
        if (Missing == Faces.end())
        {
            Notes.push_back(std::string("fallback cube faces ") + Preset.Prefix);
            Out.Kind = SourceKind::CubeFaces;
            Out.Faces = Faces;
            Out.Fallback = true;
            Out.Notes = Notes;
            return true;
        }
        Failure = std::string("the ") + StateName(State) + " sky is missing the file " + (FirstMissing.empty() ? *Missing : FirstMissing);
        return false;
    }
}

Catalog Resolve(const Location& Where, const std::atomic<bool>* Cancel)
{
    Catalog Cat;
    Cat.Found = true;
    Cat.Where = Where;
    const fs::path Root = fs::path(Strings::ToUnicodeString(Where.Root));
    GdtResolver Resolver{ Cancel, {}, {} };

    // Link 1: ssi.gdt -> skyboxmodel per lighting preset
    std::vector<GdtKey> SsiWanted;
    for (const PresetPaths& P : kPaths) SsiWanted.push_back({ "ssi.gdf", P.Ssi });
    const auto Ssi = Resolver.Find(SsiWanted, { Root / "source_data" / "ssi.gdt" }, Root / "source_data");

    // Link 2: the sky xmodel entries
    std::string Models[kStateCount];
    std::vector<GdtKey> ModelWanted;
    for (int S = 0; S < kStateCount; S++)
    {
        const auto It = Ssi.find({ "ssi.gdf", kPaths[S].Ssi });
        const std::string* Named = It == Ssi.end() ? nullptr : It->second.Get("skyboxmodel");
        Models[S] = (Named && !Trim(*Named).empty()) ? Trim(*Named) : kPaths[S].Model;
        ModelWanted.push_back({ "xmodel.gdf", Models[S] });
    }
    const fs::path ModelDir = Root / "model_export";
    const auto Xmodels = Resolver.Find(ModelWanted, { ModelDir / "t7_skybox.gdt", ModelDir / "t7_techart_props.gdt" }, ModelDir);

    // Link 3: the sky materials named by the skin overrides
    std::string Materials[kStateCount];
    std::vector<GdtKey> MaterialWanted;
    for (int S = 0; S < kStateCount; S++)
    {
        const auto It = Xmodels.find({ "xmodel.gdf", Models[S] });
        if (It != Xmodels.end()) Materials[S] = SkyMaterialOf(It->second);
        if (!Materials[S].empty()) MaterialWanted.push_back({ "material.gdf", Materials[S] });
    }
    const auto MaterialEntries = Resolver.Find(MaterialWanted, Resolver.Hits, ModelDir);

    // Link 4: the colour image entries
    std::string Images[kStateCount];
    std::vector<GdtKey> ImageWanted;
    for (int S = 0; S < kStateCount; S++)
    {
        if (Materials[S].empty()) continue;
        const auto It = MaterialEntries.find({ "material.gdf", Materials[S] });
        const std::string* Color = It == MaterialEntries.end() ? nullptr : It->second.Get("colorMap");
        if (Color && !Trim(*Color).empty()) { Images[S] = Trim(*Color); ImageWanted.push_back({ "image.gdf", Images[S] }); }
    }
    const auto ImageEntries = Resolver.Find(ImageWanted, Resolver.Hits, ModelDir);

    for (int S = 0; S < kStateCount; S++)
    {
        if (Cancelled(Cancel)) break;
        const LightState State = (LightState)S;
        std::vector<std::string> Notes;
        Notes.push_back(Ssi.count({ "ssi.gdf", kPaths[S].Ssi }) ? std::string("ssi ") + kPaths[S].Ssi + " -> skyboxmodel " + Models[S]
                                                                 : std::string("ssi ") + kPaths[S].Ssi + " not found; using planner model " + Models[S]);
        Notes.push_back(Materials[S].empty() ? "sky material not found in the xmodel's skin override" : "material " + Materials[S]);
        std::string BaseImage;
        if (!Images[S].empty())
        {
            const auto It = ImageEntries.find({ "image.gdf", Images[S] });
            if (It != ImageEntries.end())
            {
                const std::string* Base = It->second.Get("baseImage");
                const std::string* Type = It->second.Get("imageType");
                BaseImage = Base ? *Base : std::string();
                Notes.push_back("image " + Images[S] + " (" + (Type ? *Type : std::string("?")) + ") baseImage " + (Base ? *Base : std::string("(none)")));
            }
            else Notes.push_back("image " + Images[S] + " not found");
        }

        SkySource Source;
        std::string Failure;
        if (ResolveFiles(Where.Root, State, Models[S], Materials[S], Images[S], BaseImage, Notes, Source, Failure)) Cat.Sources[State] = Source;
        else Cat.Missing[State] = Failure + " (" + [&] { std::string J; for (const auto& N : Notes) J += (J.empty() ? "" : "; ") + N; return J; }() + ")";
    }
    return Cat;
}

Catalog Discover(const LocateInput& Input, const std::atomic<bool>* Cancel)
{
    Location Where;
    if (!Locate(Input, Where))
    {
        Catalog Cat;
        Cat.Notes = Where.Tried;
        return Cat;   // Found = false: nothing is wrong, the built-in presets stay
    }
    return Resolve(Where, Cancel);
}

// ======== decode, cache, bake

bool LoadSkyImage(const SkySource& Source, int MaxFace, EnvironmentImage& Out, std::string& Error, const std::atomic<bool>* Cancel)
{
    const Exr::ReadOptions Options{ false, true };
    const auto ReadOne = [&](const std::string& Path, int MaxW, int MaxH, Exr::Pixels& Px) {
        std::vector<uint8_t> Bytes;
        Exr::Status S = Exr::LoadFile(fs::path(Strings::ToUnicodeString(Path)), Bytes);
        if (S.Ok())
        {
            Exr::Reader R;
            S = Exr::Reader::Open(Bytes, R);
            if (S.Ok()) S = R.ReadDownsampled(MaxW, MaxH, Options, Px, Cancel);
        }
        if (!S.Ok())
        {
            Error = std::string(Exr::CodeName(S.Kind)) + ": " + S.Message + " (" + fs::path(Strings::ToUnicodeString(Path)).filename().string() + ")";
            return false;
        }
        return true;
    };
    const auto ToRgba = [](const Exr::Pixels& Px, std::span<float> Dst) {
        const size_t N = (size_t)Px.Width * Px.Height;
        for (size_t I = 0; I < N; I++)
        {
            Dst[I * 4] = Px.Data[I * 3]; Dst[I * 4 + 1] = Px.Data[I * 3 + 1]; Dst[I * 4 + 2] = Px.Data[I * 3 + 2]; Dst[I * 4 + 3] = 1.0f;
        }
    };

    if (Source.Kind == SourceKind::LatLong)
    {
        Exr::Pixels Px;
        if (!ReadOne(Source.LatLong, 4 * MaxFace, 2 * MaxFace, Px)) return false;
        Out.Kind = EnvironmentImage::Layout::LatLong;
        Out.Width = Px.Width;
        Out.Height = Px.Height;
        Out.Rgba.assign((size_t)Px.Width * Px.Height * 4, 0.0f);
        ToRgba(Px, Out.Rgba);
        return true;
    }

    if (Source.Faces.size() != 6) { Error = "a cube sky needs six face files"; return false; }
    int Size = 0;
    for (int F = 0; F < 6; F++)
    {
        Exr::Pixels Px;
        if (!ReadOne(Source.Faces[(size_t)F], MaxFace, MaxFace, Px)) return false;
        if (Px.Width != Px.Height || (F > 0 && Px.Width != Size))
        {
            Error = "sky.face_mismatch: the faces differ in size or are not square (" + fs::path(Source.Faces[(size_t)F]).filename().string() + " is " +
                    std::to_string(Px.Width) + "x" + std::to_string(Px.Height) + ")";
            return false;
        }
        if (F == 0)
        {
            Size = Px.Width;
            Out.Kind = EnvironmentImage::Layout::CubeFaces;
            Out.Width = Out.Height = Size;
            Out.Rgba.assign((size_t)6 * Size * Size * 4, 0.0f);
        }
        ToRgba(Px, std::span<float>(Out.Rgba).subspan((size_t)F * Size * Size * 4));
    }
    return true;
}

std::string SkyCacheDir()
{
    wchar_t Buf[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", Buf, (DWORD)std::size(Buf)) == 0) return (fs::temp_directory_path() / L"GrayWolf" / L"sky-cache").string();
    return (fs::path(Buf) / L"GrayWolf" / L"sky-cache").string();
}

std::string CacheId(const SkySource& Source, const BakeSettings& Settings)
{
    uint64_t H = 14695981039346656037ull;
    H = Fnv(H, "graywolf-sky");
    H = Fnv(H, std::to_string(kBakeVersion));
    H = Fnv(H, std::to_string(Settings.Size));
    H = Fnv(H, std::to_string(Settings.MaxFace));
    H = Fnv(H, Source.Kind == SourceKind::CubeFaces ? "cube" : "latlong");
    for (const std::string& File : Source.Files())
    {
        std::error_code Ec;
        const fs::path P(Strings::ToUnicodeString(File));
        const auto Size = fs::file_size(P, Ec);
        if (Ec) return std::string();
        const auto Time = fs::last_write_time(P, Ec);
        if (Ec) return std::string();
        H = Fnv(H, Lower(fs::absolute(P, Ec).string()));
        H = Fnv(H, std::to_string(Size));
        H = Fnv(H, std::to_string(Time.time_since_epoch().count()));
    }
    return std::format("sky-{}-{:016x}", Lower(StateName(Source.State)), H);
}

void UseCacheForEnvironments(const std::string& CacheDir)
{
    SetExtraEnvironmentDir(CacheDir.empty() ? SkyCacheDir() : CacheDir);
}

namespace
{
    struct CacheEntry { std::string Id; int64_t Bytes = 0; fs::file_time_type Time{}; };

    std::vector<CacheEntry> Entries(const fs::path& Dir)
    {
        std::map<std::string, CacheEntry> ById;
        std::error_code Ec;
        for (auto It = fs::directory_iterator(Dir, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
        {
            std::error_code E2;
            if (!It->is_regular_file(E2)) continue;
            const std::string Name = It->path().filename().string();
            for (const char* Suffix : { "_radiance.dds", "_irradiance.dds" })
            {
                const std::string S = Suffix;
                if (Name.size() <= S.size() || Name.compare(Name.size() - S.size(), S.size(), S) != 0) continue;
                CacheEntry& E = ById[Name.substr(0, Name.size() - S.size())];
                E.Id = Name.substr(0, Name.size() - S.size());
                E.Bytes += (int64_t)It->file_size(E2);
                const auto T = It->last_write_time(E2);
                if (T > E.Time) E.Time = T;
            }
        }
        std::vector<CacheEntry> Out;
        for (auto& [Id, E] : ById) Out.push_back(E);
        return Out;
    }
}

int64_t PruneCache(const std::string& CacheDir, int64_t CapBytes, const std::string& KeepId)
{
    const fs::path Dir(Strings::ToUnicodeString(CacheDir));
    std::vector<CacheEntry> E = Entries(Dir);
    int64_t Total = 0;
    for (const CacheEntry& X : E) Total += X.Bytes;
    std::sort(E.begin(), E.end(), [](const CacheEntry& A, const CacheEntry& B) { return A.Time < B.Time; });   // oldest first
    std::error_code Ec;
    for (const CacheEntry& X : E)
    {
        if (Total <= CapBytes) break;
        if (X.Id == KeepId) continue;
        fs::remove(Dir / (X.Id + "_radiance.dds"), Ec);
        fs::remove(Dir / (X.Id + "_irradiance.dds"), Ec);
        Total -= X.Bytes;
    }
    return Total;
}

BakeOutcome BakeState(const SkySource& Source, const BakeSettings& Settings, const std::atomic<bool>* Cancel, const std::function<void(float)>& Progress)
{
    BakeOutcome Out;
    const auto Report = [&](float V) { if (Progress) Progress(V); };
    const std::string Dir = Settings.CacheDir.empty() ? SkyCacheDir() : Settings.CacheDir;
    const fs::path Cache(Strings::ToUnicodeString(Dir));
    Out.Id = CacheId(Source, Settings);
    if (Out.Id.empty())
    {
        Out.Message = std::string("the ") + StateName(Source.State) + " sky has a missing file";
        return Out;
    }

    std::error_code Ec;
    const fs::path Rad = Cache / (Out.Id + "_radiance.dds"), Irr = Cache / (Out.Id + "_irradiance.dds");
    if (fs::exists(Rad, Ec) && fs::exists(Irr, Ec))
    {
        fs::last_write_time(Rad, fs::file_time_type::clock::now(), Ec);   // oldest-first eviction then drops what was not used lately
        fs::last_write_time(Irr, fs::file_time_type::clock::now(), Ec);
        Out.Ok = true;
        Out.FromCache = true;
        Out.Message = "from the cache";
        Report(1.0f);
        return Out;
    }

    EnvironmentImage Image;
    std::string Error;
    Report(0.01f);
    if (!LoadSkyImage(Source, Settings.MaxFace, Image, Error, Cancel))
    {
        Out.Message = Cancelled(Cancel) ? "cancelled" : Error;
        return Out;
    }
    Report(0.3f);

    // baked beside the cache and moved in, so a crash or a cancel leaves no half entry
    const fs::path Temp = Cache / ("tmp-" + Out.Id);
    fs::create_directories(Temp, Ec);
    EnvironmentBakeControl Control;
    Control.Cancel = Cancel;
    Control.Progress = [&](float V) { Report(0.3f + 0.7f * V); };
    Control.ClampTexel = kSunCutoff;   // the sun disc is the preset's sun, not the image based light
    Control.ScaleMultiplier = TargetMid(Source.State) / TargetMid(LightState::Day);   // Night darker, as HydraX exposes it (hypothesis: calibrated by eye there)
    std::string BakeReport;
    const bool Baked = BakePreviewEnvironment(Image, Temp.string(), Out.Id, Settings.Size, BakeReport, Control);
    Image = EnvironmentImage();
    if (!Baked)
    {
        fs::remove_all(Temp, Ec);
        Out.Message = BakeReport;
        return Out;
    }

    fs::rename(Temp / (Out.Id + "_radiance.dds"), Rad, Ec);
    if (!Ec) fs::rename(Temp / (Out.Id + "_irradiance.dds"), Irr, Ec);
    fs::remove_all(Temp, Ec);
    if (!fs::exists(Rad, Ec) || !fs::exists(Irr, Ec))
    {
        Out.Message = "cannot move the baked cubes into the cache";
        return Out;
    }

    // older entries of the same state can never be hit again
    const std::string Prefix = Out.Id.substr(0, Out.Id.rfind('-') + 1);
    for (const CacheEntry& E : Entries(Cache))
        if (E.Id != Out.Id && E.Id.compare(0, Prefix.size(), Prefix) == 0)
        {
            fs::remove(Cache / (E.Id + "_radiance.dds"), Ec);
            fs::remove(Cache / (E.Id + "_irradiance.dds"), Ec);
        }
    PruneCache(Dir, Settings.CapBytes, Out.Id);

    Out.Ok = true;
    Out.Message = BakeReport;
    return Out;
}

SkyBakeJob::SkyBakeJob(const SkySource& Source, const BakeSettings& Settings)
{
    Worker = std::thread([this, Source, Settings] {
        Result = BakeState(Source, Settings, &CancelFlag, [this](float V) { ProgressValue = V; });
        ProgressValue = 1.0f;
        Done = true;
    });
}

SkyBakeJob::~SkyBakeJob()
{
    CancelFlag = true;
    if (Worker.joinable()) Worker.join();
}
}

// ---- sky-bake <Mod Tools folder | -> --out <dir> [--size px] [--state Morning,...]: resolve the four states and bake them
// into <dir>\cache (a diagnostic; the preview bakes the same way into SkyCacheDir)

#include "CliOptions.h"
#include "CliSupport.h"
#include "GameCommand.h"
#include <CLI/CLI.hpp>

namespace
{
    int SkyBakeSize = 256;           // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): CLI11 binds the option here, as bake-environment does
    std::string SkyBakeStates;       // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): the same

    int RunSkyBake(const CliOptions& Options, const std::string& Input)
    {
        using namespace ModToolsSky;
        std::string Out;
        if (Options.Out.empty())
        {
            CliReport(Options, "result=error\nreason=--out <dir> is required (the baked cubes go there, nothing else is written)\n");
            return 5;
        }
        LocateInput In;
        if (Input != "-") In.Setting = Input;
        const Catalog Cat = Discover(In);
        if (!Cat.Found)
        {
            for (const std::string& N : Cat.Notes) Out += "tried=" + N + "\n";
            CliReport(Options, Out + "result=ok\nmod_tools=not found\n");
            return 0;
        }
        Out += "mod_tools=" + Cat.Where.Root + " (" + Cat.Where.Source + ")\n";
        for (const std::string& N : Cat.Where.Tried) Out += "tried=" + N + "\n";
        BakeSettings Settings;
        Settings.CacheDir = (fs::path(Strings::ToUnicodeString(Options.Out)) / "cache").string();
        Settings.Size = SkyBakeSize;
        int Failed = 0;
        for (const LightState State : kStates)
        {
            const std::string Name = StateName(State);
            if (!SkyBakeStates.empty() && SkyBakeStates.find(Name) == std::string::npos) continue;
            const auto It = Cat.Sources.find(State);
            if (It == Cat.Sources.end())
            {
                Out += "[" + Name + "] not resolved: " + Cat.Missing.at(State) + "\n";
                Failed++;
                continue;
            }
            const SkySource& S = It->second;
            Out += "[" + Name + "] " + (S.Kind == SourceKind::CubeFaces ? "cube faces" : "lat-long") + (S.Fallback ? " (fallback)" : " (chain)") + "\n";
            for (const std::string& N : S.Notes) Out += "  link: " + N + "\n";
            for (const std::string& F : S.Files()) Out += "  file: " + F + "\n";
            const auto T0 = std::chrono::steady_clock::now();
            const BakeOutcome B = BakeState(S, Settings, nullptr, nullptr);
            Out += std::format("  bake: {} id={} {} ms={}\n", B.Ok ? "ok" : "FAILED", B.Id, B.FromCache ? "(cache)" : "",
                               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count());
            if (!B.Ok) { Out += "  reason: " + B.Message + "\n"; Failed++; }
        }
        CliReport(Options, Out + std::format("result={}\nfailed={}\n", Failed == 0 ? "ok" : "error", Failed));
        return Failed == 0 ? 0 : 5;
    }

    void AddSkyBakeOptions(CLI::App& Sub, CliOptions&)
    {
        Sub.add_option("--size", SkyBakeSize, "The radiance cube's face in pixels, a power of two (256)");
        Sub.add_option("--state", SkyBakeStates, "Only these states, e.g. Morning,Night (default: all four)");
    }

    const GameCommand SkyBakeCommand{
        "sky-bake",
        "Resolve the four BO3 lighting states' skies in the Mod Tools (read only) and bake them into <--out>/cache",
        "The Mod Tools folder, or - to find it (setting, TA_TOOLS_PATH, Steam)",
        RunSkyBake,
        AddSkyBakeOptions,
    };
    const GameCommandRegistration SkyBakeRegistered(SkyBakeCommand);
}
