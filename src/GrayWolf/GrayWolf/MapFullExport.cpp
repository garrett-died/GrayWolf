#include "stdafx.h"

#include "MapFullExport.h"

#include "Bo3Target.Placement.h"
#include "ExportNaming.h"
#include "MapMaterials.h"
#include "MapRadiantRules.h"
#include "MapRadiantWriter.h"
#include "MaterialGdt.h"
#include "SurfaceTypes.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace Maps
{
namespace
{
    bool Ends(const std::string& S, const char* Tail)
    {
        const size_t N = std::strlen(Tail);
        return S.size() >= N && S.compare(S.size() - N, N, Tail) == 0;
    }

    // "zbarrierboardmodel1" ends in "model" and digits; "script_anim2" in "anim" and digits
    bool EndsWithNumbered(const std::string& Lower, const char* Word)
    {
        size_t End = Lower.size();
        while (End > 0 && Lower[End - 1] >= '0' && Lower[End - 1] <= '9') End--;
        return End > std::strlen(Word) && Ends(Lower.substr(0, End), Word);
    }

    std::string Clean(std::string S)
    {
        for (char& C : S)
            if (C == '\t' || C == '\n' || C == '\r') C = ' ';
        return S;
    }

    std::string LowerOf(std::string S)
    {
        for (char& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    }
}

std::string EntityAssetType(const std::string& Key, const std::string& Value)
{
    if (Value.empty() || Key.empty() || Key == "classname") return std::string();
    const std::string K = LowerOf(Key), V = LowerOf(Value);
    if (K == "fxdef" || Ends(K, "efx") || Ends(V, ".efx")) return "fx";
    if (K == "lutmaterial") return "material";
    if (EndsWithNumbered(K, "anim")) return "xanim";
    // a brush model ("*N") or a trigger volume ("?N") is no xmodel
    const bool Model = K == "model" || K == "skyboxmodel" || Ends(K, "_model") || EndsWithNumbered(K, "model");
    if (Model && Value[0] != '*' && Value[0] != '?') return "xmodel";
    return std::string();
}

std::string FullMapAssetName(const std::string& GameName)
{
    return Bo3Target::Placement::Shorten(MaterialGdt::AssetName(ExportNaming::SafeFileName(GameName)));
}

std::string GeometryOwnerName(const std::string& Map) { return Map + "_geometry"; }

std::vector<MapDependency> CollectMapDependencies(const MapScene& Scene)
{
    std::vector<MapDependency> Out;
    std::set<std::string> Seen;
    const auto Add = [&](const std::string& Type, const std::string& Name, const std::string& Via, const std::string& Detail = std::string()) {
        if (Name.empty() || Name == "none") return;
        if (!Seen.insert(Type + "|" + LowerOf(Name)).second) return;
        Out.push_back({ Type, Clean(Name), Clean(Via), Clean(Detail) });
    };
    const auto Model = [&](const std::string& Name, const char* Via) {
        if (!Name.empty() && Name[0] != '*' && Name[0] != '?') Add("xmodel", Name, Via);
    };
    for (const Instance& I : Scene.StaticInstances) Model(I.Model, "staticModel"), Model(I.Destroyed, "destroyedModel");
    for (const Instance& I : Scene.DynamicInstances) Model(I.Model, "dynamicModel"), Model(I.Destroyed, "destroyedModel");
    for (const Instance& I : Scene.SplinedInstances) Model(I.Model, "splinedModel"), Model(I.Destroyed, "destroyedModel");
    Model(Scene.Sky.Model, "sky");

    for (const auto& Entity : SceneEntities(Scene))
        for (const auto& [Key, Value] : Entity)
            if (const std::string Type = EntityAssetType(Key, Value); !Type.empty()) Add(Type, Value, Key);

    const std::map<std::string, std::string> Usage = GeometryUsage(Scene);
    for (const auto& [Name, Use] : Usage) Add("material", Name, "surface:" + Use, "usage=" + Use);
    return Out;
}

bool IsDecalMaterial(const MapScene& Scene, const std::string& MaterialName)
{
    const auto Mat = Scene.Materials.find(MaterialName);
    if (Mat == Scene.Materials.end()) return false;
    return LowerOf(Mat->second.Techset).find("decal") != std::string::npos || SurfaceTypes::IsDecal(LowerOf(Scene.Game), Mat->second.RenderFlags);
}

std::map<std::string, std::string> GeometryUsage(const MapScene& Scene, const std::set<std::string>* Only)
{
    using RadiantGeometry::V3;
    std::map<std::string, FacingAreas> Drawn;
    for (const Mesh& M : Scene.Objects)
        for (const Surface& S : M.Surfaces)
        {
            if (S.Material.empty() || (Only != nullptr && !Only->count(S.Material))) continue;
            FacingAreas& A = Drawn[S.Material];
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            {
                if (S.Indices[K] >= S.Vertices.size() || S.Indices[K + 1] >= S.Vertices.size() || S.Indices[K + 2] >= S.Vertices.size()) continue;
                const auto P = [&](size_t J) {
                    const Vertex& V = S.Vertices[S.Indices[K + J]];
                    return V3{ V.Pos[0], V.Pos[1], V.Pos[2] };
                };
                A.Add(P(0), P(1), P(2));
            }
        }
    std::map<std::string, std::string> Usage;
    for (const auto& [Name, Areas] : Drawn)
    {
        const auto Mat = Scene.Materials.find(Name);
        const std::string Techset = Mat != Scene.Materials.end() ? Mat->second.Techset : std::string();
        Usage[Name] = MaterialUsage(IsDecalMaterial(Scene, Name), Techset, Areas);
    }
    return Usage;
}

std::string FormatDependencyList(const MapDependencyList& List)
{
    std::string Text = "map\ttype\tname\tvia\tdetail\n";
    if (!List.Owner.empty()) Text += Clean(List.Map) + "\towner\t" + Clean(List.Owner) + "\t\t\n";
    for (const MapDependency& D : List.Dependencies) Text += Clean(List.Map) + "\t" + D.Type + "\t" + D.Name + "\t" + D.Via + "\t" + D.Detail + "\n";
    return Text;
}

bool ParseDependencyList(const std::string& Text, MapDependencyList& List)
{
    std::istringstream In(Text);
    std::string Line;
    bool First = true;
    while (std::getline(In, Line))
    {
        if (!Line.empty() && Line.back() == '\r') Line.pop_back();
        std::vector<std::string> Cells;
        size_t Start = 0;
        for (;;)
        {
            const size_t Tab = Line.find('\t', Start);
            Cells.push_back(Line.substr(Start, Tab == std::string::npos ? std::string::npos : Tab - Start));
            if (Tab == std::string::npos) break;
            Start = Tab + 1;
        }
        if (First)
        {
            First = false;
            if (Cells.size() < 5 || Cells[0] != "map") return false;
            continue;
        }
        if (Cells.size() < 5) continue;
        List.Map = Cells[0];
        if (Cells[1] == "owner") List.Owner = Cells[2];
        else List.Dependencies.push_back({ Cells[1], Cells[2], Cells[3], Cells[4] });
    }
    return !First;
}

namespace
{
    std::mutex RunFilesLock;
    // maps folder (lower case) -> the (kind, file) rows made this run
    std::map<std::string, std::set<std::pair<std::string, std::string>>>& RunFiles()
    {
        static std::map<std::string, std::set<std::pair<std::string, std::string>>> All;
        return All;
    }
    std::string DirKey(const fs::path& Dir)
    {
        std::error_code Ec;
        fs::path A = fs::absolute(Dir, Ec);
        return LowerOf(A.lexically_normal().generic_string());
    }
}

void BeginRunMapFiles(const fs::path& MapDir)
{
    std::lock_guard<std::mutex> G(RunFilesLock);
    RunFiles()[DirKey(MapDir)].clear();
    std::error_code Ec;
    fs::remove(MapDir / MapIndexFile, Ec);   // the previous run's index: its files are not this run's
}

bool NoteRunMapFile(const fs::path& MapDir, const std::string& FileName, const char* Kind)
{
    std::lock_guard<std::mutex> G(RunFilesLock);
    auto& Rows = RunFiles()[DirKey(MapDir)];
    Rows.emplace(Kind, FileName);
    std::string Text;
    for (const auto& [K, F] : Rows) Text += K + "\t" + Clean(F) + "\n";
    std::error_code Ec;
    fs::create_directories(MapDir, Ec);
    std::ofstream Out(MapDir / MapIndexFile, std::ios::binary | std::ios::trunc);
    return Out && (Out << Text);
}

bool ReadMapIndex(const fs::path& MapDir, std::vector<std::pair<std::string, std::string>>& Rows)
{
    Rows.clear();
    std::ifstream In(MapDir / MapIndexFile, std::ios::binary);
    if (!In) return false;
    std::string Line;
    while (std::getline(In, Line))
    {
        if (!Line.empty() && Line.back() == '\r') Line.pop_back();
        const size_t Tab = Line.find('\t');
        if (Tab == std::string::npos || Tab + 1 >= Line.size()) continue;
        Rows.emplace_back(Line.substr(0, Tab), Line.substr(Tab + 1));
    }
    return true;
}

bool AppendDependencies(const fs::path& ListFile, const std::vector<MapDependency>& More)
{
    std::ifstream In(ListFile, std::ios::binary);
    if (!In) return false;
    const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    In.close();
    MapDependencyList List;
    if (!ParseDependencyList(Text, List)) return false;
    if (List.Map.empty())
    {
        List.Map = ListFile.filename().string();
        List.Map.resize(List.Map.size() - std::min(List.Map.size(), std::strlen(FullMapListSuffix)));
    }
    std::set<std::string> Have;
    for (const MapDependency& D : List.Dependencies) Have.insert(D.Type + "|" + LowerOf(D.Name));
    for (const MapDependency& D : More)
        if (!D.Name.empty() && Have.insert(D.Type + "|" + LowerOf(D.Name)).second) List.Dependencies.push_back({ D.Type, Clean(D.Name), Clean(D.Via), Clean(D.Detail) });
    const std::string Out = FormatDependencyList(List);
    std::ofstream File(ListFile, std::ios::binary | std::ios::trunc);
    return File && (File << Out);
}

std::string StrongerUsage(const std::string& A, const std::string& B)
{
    const auto Rank = [](const std::string& U) {
        return U == "decal" ? 5 : U == "window" ? 4 : U == "floor" ? 3 : U == "ceiling" ? 2 : U == "interior wall" ? 1 : 0;
    };
    const int Ra = Rank(A), Rb = Rank(B);
    return Ra != Rb ? (Ra > Rb ? A : B) : std::min(A, B);
}

bool WriteFullMapFiles(const MapScene& Scene, const Materials::IAdapter& Adapter, const fs::path& ModelsDir, const std::string& Stem,
                       std::map<std::string, size_t>& Counters, std::string& Error)
{
    MapDependencyList List;
    List.Map = fs::path(Stem).filename().string();
    List.Dependencies = CollectMapDependencies(Scene);
    for (const MapDependency& D : List.Dependencies) Counters["map_full_dependencies_" + D.Type]++;

    // the materials the render surfaces draw, and the lut material when the scene read it
    std::set<std::string> Geometry;
    for (const Mesh& M : Scene.Objects)
        for (const Surface& S : M.Surfaces)
            if (!S.Material.empty()) Geometry.insert(S.Material);
    for (const MapDependency& D : List.Dependencies)
        if (D.Type == "material" && Scene.Materials.count(D.Name)) Geometry.insert(D.Name);
    std::error_code Ec;
    if (!Geometry.empty())
    {
        List.Owner = GeometryOwnerName(List.Map);
        const std::string Safe = ExportNaming::SafeFileName(List.Owner);
        const fs::path Dir = ModelsDir / Safe;
        fs::create_directories(Dir, Ec);
        if (Ec)
        {
            Error = "cannot create " + Dir.string();
            return false;
        }
        Materials::Sidecar Side;
        Side.Game = Side.Adapter = Adapter.Name();
        for (const std::string& Name : Geometry)
        {
            auto [Src, Described] = DescribeMaterial(Scene, Name, Adapter, Dir);
            Side.Materials.push_back(std::move(Described));
            Side.Sources.push_back(std::move(Src));
        }
        if (!Materials::WriteSidecar((Dir / (Safe + "_LOD0.materials.json")).string(), Side))
        {
            Error = "cannot write the geometry materials in " + Dir.string();
            return false;
        }
        Counters["map_full_geometry_materials"] += Geometry.size();
    }
    const fs::path ListFile = Stem + FullMapListSuffix;
    fs::create_directories(ListFile.parent_path(), Ec);
    const std::string Text = FormatDependencyList(List);
    std::ofstream Out(ListFile, std::ios::binary | std::ios::trunc);
    if (!Out || !(Out << Text))
    {
        Error = "cannot write " + ListFile.string();
        return false;
    }
    return true;
}

// ---- self test

namespace
{
    Surface Triangle(const char* Material, float Nz)
    {
        Surface S;
        S.Material = Material;
        Vertex A, B, C;
        A.Pos[0] = 0, A.Pos[1] = 0, A.Pos[2] = 0;
        B.Pos[0] = 10, B.Pos[1] = 0, B.Pos[2] = 0;
        C.Pos[0] = 0, C.Pos[1] = 10, C.Pos[2] = 0;
        if (Nz < 0) std::swap(B, C);
        S.Vertices = { A, B, C };
        S.Indices = { 0, 1, 2 };
        return S;
    }
}

int FullMapSelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::string("full map: ") + What + (Got.empty() ? "" : " (" + Got.substr(0, 400) + ")") + "\n";
    };
    const auto Find = [](const std::vector<MapDependency>& V, const std::string& Type, const std::string& Name) -> const MapDependency* {
        for (const auto& D : V)
            if (D.Type == Type && D.Name == Name) return &D;
        return nullptr;
    };

    Check(EntityAssetType("model", "p7_chair") == "xmodel" && EntityAssetType("model", "*12").empty() && EntityAssetType("model", "?3").empty(),
          "a model value is an xmodel, a brush or trigger model is none");
    Check(EntityAssetType("zbarrierboardModel1", "p7_board") == "xmodel" && EntityAssetType("prop_model", "p7_prop") == "xmodel" &&
              EntityAssetType("skyboxmodel", "skybox_x") == "xmodel",
          "the BO4 model keys are xmodels");
    Check(EntityAssetType("lutmaterial", "mtl_lut_x") == "material" && EntityAssetType("walkAnim2", "a_b") == "xanim" &&
              EntityAssetType("fxdef", "fx_a") == "fx" && EntityAssetType("rattleEfx", "a/b.efx") == "fx" && EntityAssetType("script_fx", "a/b.efx") == "fx",
          "material, xanim and effect keys");
    Check(EntityAssetType("targetname", "door_1").empty() && EntityAssetType("classname", "script_model").empty() && EntityAssetType("model", "").empty(),
          "other keys and empty values are none");
    Check(FullMapAssetName("P7/Foo-Bar") == "p7_foo_bar" && FullMapAssetName("p7_chair") == "p7_chair", "a model's published name", FullMapAssetName("P7/Foo-Bar"));

    MapScene Scene;
    Scene.Name = "mp_synth";
    Instance A, B;
    A.Model = "p7_a";
    B.Model = "p7_a";
    B.Destroyed = "p7_a_destroyed";
    Scene.StaticInstances = { A, B };
    Instance D;
    D.Model = "p7_dyn";
    Scene.DynamicInstances = { D };
    Scene.MapEnts =
        "{ \"classname\" \"worldspawn\" \"lutmaterial\" \"mtl_lut_synth\" \"skyboxmodel\" \"skybox_synth\" }\n"
        "{ \"classname\" \"script_model\" \"model\" \"p7_c\" \"rattleEfx\" \"debris/a.efx\" }\n"
        "{ \"classname\" \"script_brushmodel\" \"model\" \"*3\" }\n"
        "{ \"classname\" \"trigger_use\" \"model\" \"?2\" }\n"
        "{ \"classname\" \"zbarrier\" \"zbarrierboardModel1\" \"p7_board\" \"zbarrierboardModel2\" \"p7_board\" \"walkAnim1\" \"a_walk\" }\n"
        "{ \"classname\" \"script_model\" \"model\" \"p7_c\" }\n";
    Mesh World;
    World.Name = "world";
    World.Surfaces = { Triangle("mtl_floor", 1), Triangle("mtl_ceiling", -1) };
    Scene.Objects = { World };
    Mesh Prop;
    Prop.Name = "p7_a";
    Prop.Surfaces = { Triangle("mtl_prop_only", 1) };
    Scene.Models["p7_a"] = Prop;
    Scene.Materials["mtl_floor"].Name = "mtl_floor";
    Scene.Materials["mtl_prop_only"].Name = "mtl_prop_only";

    const std::vector<MapDependency> Deps = CollectMapDependencies(Scene);
    std::set<std::string> Keys;
    for (const auto& Dep : Deps) Check(Keys.insert(Dep.Type + "|" + Dep.Name).second, "each dependency once", Dep.Type + " " + Dep.Name);
    Check(Find(Deps, "xmodel", "p7_a") && Find(Deps, "xmodel", "p7_a")->Via == "staticModel", "a static model");
    Check(Find(Deps, "xmodel", "p7_a_destroyed") && Find(Deps, "xmodel", "p7_dyn") && Find(Deps, "xmodel", "p7_c") && Find(Deps, "xmodel", "p7_board") &&
              Find(Deps, "xmodel", "skybox_synth"),
          "destroyed, dynamic, entity, zbarrier and sky models");
    Check(!Find(Deps, "xmodel", "*3") && !Find(Deps, "xmodel", "?2"), "the *N brush model and ?N trigger model are skipped");
    Check(Find(Deps, "material", "mtl_lut_synth") != nullptr, "the lut material");
    Check(Find(Deps, "fx", "debris/a.efx") != nullptr && Find(Deps, "xanim", "a_walk") != nullptr, "an effect and an xanim are named");
    const MapDependency* Floor = Find(Deps, "material", "mtl_floor");
    Check(Floor && Floor->Detail == "usage=floor" && Floor->Via == "surface:floor", "a render surface material carries its usage", Floor ? Floor->Detail : "(none)");
    Check(Find(Deps, "material", "mtl_ceiling") && Find(Deps, "material", "mtl_ceiling")->Detail == "usage=ceiling", "a ceiling surface");
    Check(Find(Deps, "material", "mtl_prop_only") == nullptr, "a prop's material alone is no geometry material");
    const auto Usage = GeometryUsage(Scene);
    Check(Usage.size() == 2 && !Usage.count("mtl_prop_only"), "usage only for what a world surface draws");

    MapDependencyList List;
    List.Map = "mp_synth";
    List.Owner = "mp_synth_geometry";
    List.Dependencies = Deps;
    MapDependencyList Back;
    Check(ParseDependencyList(FormatDependencyList(List), Back) && Back.Owner == List.Owner && Back.Dependencies.size() == Deps.size() &&
              Back.Dependencies.front().Name == Deps.front().Name,
          "the list reads back as it was written");

    // the files
    const fs::path Dir = fs::temp_directory_path() / ("graywolf_selftest_fullmap_" + std::to_string(GetCurrentProcessId()));
    std::error_code Ec;
    fs::remove_all(Dir, Ec);
    fs::create_directories(Dir / "maps", Ec);
    std::map<std::string, size_t> Counters;
    std::string Error;
    const bool Written = WriteFullMapFiles(Scene, Materials::DefaultAdapter(), Dir / "models", (Dir / "maps" / "mp_synth").string(), Counters, Error);
    Check(Written, "the files are written", Error);
    Check(fs::is_regular_file(Dir / "models" / "mp_synth_geometry" / "mp_synth_geometry_LOD0.materials.json", Ec), "the geometry materials sidecar");
    MapDependencyList Read;
    std::ifstream In(Dir / "maps" / ("mp_synth" + std::string(FullMapListSuffix)), std::ios::binary);
    const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    Check(ParseDependencyList(Text, Read) && Read.Owner == "mp_synth_geometry", "the list file names the geometry owner", Text);
    Check(Counters["map_full_geometry_materials"] == 2, "two geometry materials", std::to_string(Counters["map_full_geometry_materials"]));
    fs::remove_all(Dir, Ec);
    return Failed;
}
}
