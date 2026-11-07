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
#include <charconv>
#include <cstring>
#include <format>
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
        if (!Seen.insert(Type + "|" + LowerOf(Name)).second)
        {
            // a material the map draws on its geometry keeps its row whatever named it first (a lutmaterial of the same name)
            if (Type == "material" && Detail.rfind("facing=", 0) == 0)
                for (MapDependency& D : Out)
                    if (D.Type == Type && LowerOf(D.Name) == LowerOf(Name)) D.Via = Clean(Via), D.Detail = Clean(Detail);
            return;
        }
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

    for (const auto& [Name, Use] : GeometryUse(Scene)) Add("material", Name, std::string(Use.Sky ? "sky:" : "surface:") + Use.Via, FormatMapUse(Use));
    return Out;
}

bool IsDecalMaterial(const MapScene& Scene, const std::string& MaterialName)
{
    const auto Mat = Scene.Materials.find(MaterialName);
    if (Mat == Scene.Materials.end()) return false;
    return LowerOf(Mat->second.Techset).find("decal") != std::string::npos || SurfaceTypes::IsDecal(LowerOf(Scene.Game), Mat->second.RenderFlags);
}

int WindingOf(const std::string& Game)
{
    const std::string G = LowerOf(Game);
    if (G == "mwr") return -1;   // measured on mp_crash: the triangle cross runs opposite the vertex normal on 1,080,760 of 1,080,760
    return G == "bo3" || G == "bo4" ? -1 : 1;
}

bool VertexOrderIsPatchOrder(const std::string& Game) { return LowerOf(Game) == "bo3"; }

bool StrictMaterials(const std::string& Game) { return LowerOf(Game) != "bo3"; }

std::map<std::string, Bo3Target::Techsets::MapUse> GeometryUse(const MapScene& Scene, const std::set<std::string>* Only, FacingStats* Stats)
{
    using RadiantGeometry::V3;
    std::map<std::string, Bo3Target::Techsets::MapUse> Drawn;
    const int Winding = WindingOf(Scene.Game);
    // a reader that read the vertex normals says so per vertex (Vertex::NormalRead); judged per surface. The value (0, 0, 1) cannot tell unread from read (a flat
    // floor reads exactly that), so unset means unknown and the game's winding decides
    const auto SurfaceHasNormals = [](const Surface& S) {
        for (const Vertex& V : S.Vertices)
            if (V.NormalRead) return true;
        return false;
    };
    bool NormalsRead = false;   // any surface of the scene (the stats only)
    for (const Mesh& M : Scene.Objects)
        for (const Surface& S : M.Surfaces)
            if (SurfaceHasNormals(S)) NormalsRead = true;
    if (Stats != nullptr) Stats->NormalsRead = NormalsRead;
    // the area of one mesh's surfaces by facing, into Drawn; Terrain / Sky: the role of the mesh
    const auto Measure = [&](const Mesh& M, const std::string& Label, bool Terrain, bool Sky) {
        for (size_t N = 0; N < M.Surfaces.size(); N++)
        {
            const Surface& S = M.Surfaces[N];
            if (S.Material.empty() || (Only != nullptr && !Only->count(S.Material))) continue;
            Bo3Target::Techsets::MapUse& U = Drawn[S.Material];
            if (U.Via.empty()) U.Via = Label + "#" + std::to_string(N);
            U.Terrain = U.Terrain || Terrain;
            U.Sky = U.Sky || Sky;
            if (Sky) continue;   // the sky model's triangles are in the model's space: they say nothing about a floor or a wall
            FacingAreas A;
            const bool SurfaceNormals = SurfaceHasNormals(S);
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            {
                if (S.Indices[K] >= S.Vertices.size() || S.Indices[K + 1] >= S.Vertices.size() || S.Indices[K + 2] >= S.Vertices.size()) continue;
                const auto P = [&](size_t J) {
                    const Vertex& V = S.Vertices[S.Indices[K + J]];
                    return V3{ V.Pos[0], V.Pos[1], V.Pos[2] };
                };
                // the vertex normal when the reader gives one (the mean of the three, a unit vector), else the game's winding
                V3 Mean = { 0, 0, 0 };
                bool HaveNormal = false;
                if (SurfaceNormals)
                {
                    for (size_t J = 0; J < 3; J++)
                        for (size_t Axis = 0; Axis < 3; Axis++) Mean[Axis] += S.Vertices[S.Indices[K + J]].Normal[Axis];
                    const double Len = std::sqrt(Mean[0] * Mean[0] + Mean[1] * Mean[1] + Mean[2] * Mean[2]);
                    if (Len > 0.5)
                    {
                        for (size_t Axis = 0; Axis < 3; Axis++) Mean[Axis] /= Len;
                        HaveNormal = true;
                    }
                }
                A.Add(P(0), P(1), P(2), HaveNormal ? &Mean : nullptr, Winding);
            }
            if (Stats != nullptr)
            {
                Stats->WithNormal += A.WithNormal;
                Stats->CrossAgrees += A.CrossAgrees;
                Stats->CrossAgainst += A.CrossAgainst;
                Stats->Triangles += S.Indices.size() / 3;
            }
            U.Floor += A.Floor;
            U.Ceiling += A.Ceiling;
            U.Wall += A.Wall;
        }
    };
    for (const Mesh& M : Scene.Objects) Measure(M, M.Name.empty() ? std::string("world") : M.Name, LowerOf(M.Name).rfind("terrain", 0) == 0, false);
    // the sky model is the map's own sky: its materials are named by the map's geometry
    if (!Scene.Sky.Model.empty())
        if (const auto Sky = Scene.Models.find(Scene.Sky.Model); Sky != Scene.Models.end()) Measure(Sky->second, Scene.Sky.Model, false, true);
    return Drawn;
}

std::vector<std::string> DrawnAssetKeys(const std::string& Name)
{
    const std::string Asset = LowerOf(MaterialGdt::AssetName(Name));
    return { Asset, LowerOf(Bo3Target::Placement::Shorten(Asset)) };
}

bool ReadDrawnMaterials(const fs::path& MapFolder, std::map<std::string, std::string>& Drawn, std::string& Why)
{
    std::vector<std::pair<std::string, std::string>> Index;
    if (!ReadMapIndex(MapFolder, Index))
    {
        Why = std::string("no ") + MapIndexFile + " in " + MapFolder.string();
        return false;
    }
    bool Ok = true;
    size_t Lists = 0;
    for (const auto& [Kind, Name] : Index)
    {
        if (Kind != "list" || LowerOf(Name).size() < std::strlen(FullMapListSuffix) || LowerOf(Name).compare(Name.size() - std::strlen(FullMapListSuffix), std::string::npos, FullMapListSuffix) != 0) continue;
        std::ifstream File(MapFolder / Name, std::ios::binary);
        const std::string Text((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        MapDependencyList List;
        if (!File || !ParseDependencyList(Text, List))
        {
            Why += "the map list " + Name + " does not read; ";
            Ok = false;
            continue;
        }
        Lists++;
        for (const MapDependency& D : List.Dependencies)
        {
            if (D.Type != "material") continue;
            Bo3Target::Techsets::MapUse Use;
            if (D.Detail.rfind("usage=", 0) == 0 || (D.Detail.rfind("facing=", 0) == 0 && !ParseMapUse(D.Detail, D.Via, Use)))
            {
                Why += "a material row of " + Name + " is in an old or unreadable form (" + D.Name + "); ";
                Ok = false;
            }
            else if (D.Detail.rfind("facing=", 0) == 0)
                for (const std::string& Key : DrawnAssetKeys(D.Name)) Drawn.emplace(Key, Use.Via);
        }
    }
    if (Lists == 0)
    {
        Why += "the index names no map list; ";
        Ok = false;
    }
    return Ok;
}

std::string FormatMapUse(const Bo3Target::Techsets::MapUse& Use)
{
    std::string Out = std::format("facing={:.3f}:{:.3f}:{:.3f}", Use.Floor, Use.Ceiling, Use.Wall);
    if (Use.Terrain) Out += ";role=terrain";
    if (Use.Sky) Out += ";role=sky";
    return Out;
}

bool ParseMapUse(const std::string& Detail, const std::string& Via, Bo3Target::Techsets::MapUse& Use)
{
    Use = Bo3Target::Techsets::MapUse();
    if (Detail.rfind("facing=", 0) != 0) return false;
    size_t At = 7;
    double V[3] = { 0, 0, 0 };
    for (int I = 0; I < 3; I++)
    {
        const size_t End = Detail.find_first_of(":;", At);
        const std::string Part = Detail.substr(At, End == std::string::npos ? std::string::npos : End - At);
        const char* const PartEnd = std::to_address(Part.end());
        const auto R = std::from_chars(Part.data(), PartEnd, V[I]);
        if (Part.empty() || R.ec != std::errc() || R.ptr != PartEnd || !(V[I] >= 0)) return false;
        if (I < 2)
        {
            if (End == std::string::npos || Detail[End] != ':') return false;
            At = End + 1;
        }
        else At = End == std::string::npos ? Detail.size() : End;
    }
    Use.Floor = V[0], Use.Ceiling = V[1], Use.Wall = V[2];
    while (At < Detail.size())
    {
        if (Detail[At] != ';') return false;
        const size_t End = Detail.find(';', At + 1);
        const std::string Part = Detail.substr(At + 1, End == std::string::npos ? std::string::npos : End - At - 1);
        if (Part == "role=terrain") Use.Terrain = true;
        else if (Part == "role=sky") Use.Sky = true;
        else return false;
        At = End == std::string::npos ? Detail.size() : End;
    }
    // the surface: "surface:<mesh>#<n>" or "sky:<model>#<n>"
    const size_t Colon = Via.find(':');
    Use.Via = Colon == std::string::npos ? Via : Via.substr(Colon + 1);
    if (Use.Via.empty()) Use.Via = "(unnamed surface)";
    return true;
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
    std::mutex RunFilesLock;   // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): the lock of the run's file table, shared by the export threads
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

bool WriteFullMapFiles(const MapScene& Scene, const Materials::IAdapter& Adapter, const fs::path& ModelsDir, const std::string& Stem,
                       std::map<std::string, size_t>& Counters, std::string& Error)
{
    MapDependencyList List;
    List.Map = fs::path(Stem).filename().string();
    List.Dependencies = CollectMapDependencies(Scene);
    for (const MapDependency& D : List.Dependencies) Counters["map_full_dependencies_" + D.Type]++;
    {
        // how the facing was decided, per game: the vertex normal against the cross product's direction
        FacingStats Facing;
        GeometryUse(Scene, nullptr, &Facing);
        Counters["map_facing_triangles"] += Facing.Triangles;
        Counters["map_facing_with_normal"] += Facing.WithNormal;
        Counters["map_facing_cross_with_normal"] += Facing.CrossAgrees;
        Counters["map_facing_cross_against_normal"] += Facing.CrossAgainst;
        Counters["map_facing_normals_read"] += Facing.NormalsRead ? 1 : 0;
    }

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
        "{ \"classname\" \"script_model\" \"model\" \"p7_c\" }\n"
        "{ \"classname\" \"worldspawn\" \"lutmaterial\" \"mtl_floor\" }\n";   // the lut names a material the world also draws: the geometry row must stay
    Mesh World;
    World.Name = "world";
    World.Surfaces = { Triangle("mtl_floor", 1), Triangle("mtl_ceiling", -1) };
    Mesh Terrain;
    Terrain.Name = "terrain_0";
    Terrain.Surfaces = { Triangle("mtl_layer", 1), Triangle("mtl_layer", 1) };
    Scene.Objects = { World, Terrain };
    Mesh Sky;
    Sky.Name = "skybox_synth";
    Sky.Surfaces = { Triangle("mtl_skybox", 1) };
    Scene.Models["skybox_synth"] = Sky;
    Scene.Sky.Model = "skybox_synth";
    Scene.Materials["mtl_skybox"].Name = "mtl_skybox";
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
    Check(Floor && Floor->Detail == "facing=50.000:0.000:0.000" && Floor->Via == "surface:world#0", "a render surface material carries its facing areas and the surface that names it",
          Floor ? Floor->Detail + " / " + Floor->Via : "(none)");
    Check(Find(Deps, "material", "mtl_ceiling") && Find(Deps, "material", "mtl_ceiling")->Detail == "facing=0.000:50.000:0.000" && Find(Deps, "material", "mtl_ceiling")->Via == "surface:world#1",
          "a ceiling surface");
    const MapDependency* Layer = Find(Deps, "material", "mtl_layer");
    Check(Layer && Layer->Detail == "facing=100.000:0.000:0.000;role=terrain" && Layer->Via == "surface:terrain_0#0", "a terrain mesh's material has the terrain role and both triangles", Layer ? Layer->Detail + " / " + Layer->Via : "(none)");
    const MapDependency* SkyDep = Find(Deps, "material", "mtl_skybox");
    Check(SkyDep && SkyDep->Detail == "facing=0.000:0.000:0.000;role=sky" && SkyDep->Via == "sky:skybox_synth#0", "the sky model's material has the sky role and no facing", SkyDep ? SkyDep->Detail + " / " + SkyDep->Via : "(none)");
    {
        Bo3Target::Techsets::MapUse Round;
        Check(Layer && ParseMapUse(Layer->Detail, Layer->Via, Round) && Round.Floor == 100 && Round.Terrain && !Round.Sky && Round.Via == "terrain_0#0", "a use reads back as it was written", Round.Via);
        Check(!ParseMapUse("usage=floor", "surface:floor", Round) && !ParseMapUse("facing=1:2", "x", Round) && !ParseMapUse("facing=1.000:2.000:x", "x", Round) &&
                  !ParseMapUse("facing=1.000:2.000:3.000;role=nonsense", "x", Round) && !ParseMapUse("facing=-1.000:2.000:3.000", "x", Round),
              "text that is not a use is refused");
    }
    Check(Find(Deps, "material", "mtl_prop_only") == nullptr, "a prop's material alone is no geometry material");
    const auto Usage = GeometryUse(Scene);
    Check(Usage.size() == 4 && !Usage.count("mtl_prop_only"), "a measure only for what a world surface, a terrain mesh or the sky draws", std::to_string(Usage.size()));
    const std::set<std::string> Only = { "mtl_floor" };
    Check(GeometryUse(Scene, &Only).size() == 1, "a measure only for the materials asked");

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
    Check(Counters["map_full_geometry_materials"] == 4, "four geometry materials", std::to_string(Counters["map_full_geometry_materials"]));
    fs::remove_all(Dir, Ec);

    // the facing of a game's triangles: by the vertex normal when the reader gives one, else by the game's measured winding
    {
        MapScene Winding;
        Winding.Game = "bo4";
        Mesh W;
        W.Name = "world";
        W.Surfaces = { Triangle("m_cross_down", -1) };          // cross(b - a, c - a) points down; no reader normal: BO4 winding -1 makes it a floor
        Winding.Objects = { W };
        const auto UseA = GeometryUse(Winding);
        Check(UseA.count("m_cross_down") == 1 && UseA.at("m_cross_down").Floor > 0 && UseA.at("m_cross_down").Ceiling == 0, "bo4 without a reader normal: cross down is a floor (winding -1)");
        Winding.Game = "iw3";
        const auto UseB = GeometryUse(Winding);
        Check(UseB.at("m_cross_down").Ceiling > 0, "a game with no measured winding keeps the cross product's sign (+1)");
        MapScene Normal;
        Normal.Game = "bo4";
        Mesh N;
        N.Name = "world";
        Surface S = Triangle("m_normal_down", 1);               // cross up, but the reader's normal says down
        for (Vertex& V : S.Vertices) V.Normal[0] = 0, V.Normal[1] = 0, V.Normal[2] = -1, V.NormalRead = true;
        N.Surfaces = { S };
        Normal.Objects = { N };
        FacingStats Stats;
        const auto UseC = GeometryUse(Normal, nullptr, &Stats);
        Check(UseC.at("m_normal_down").Ceiling > 0 && UseC.at("m_normal_down").Floor == 0 && Stats.NormalsRead && Stats.WithNormal == 1 && Stats.CrossAgainst == 1,
              "the reader's vertex normal decides, and the stats compare it with the cross product");
        // a scene where one surface has real normals and another has the default (0, 0, 1): the default one must follow the winding, not count as a floor
        MapScene Mixed;
        Mixed.Game = "bo4";
        Mesh X;
        X.Name = "world";
        Surface Real = Triangle("m_real_down", 1);
        for (Vertex& V : Real.Vertices) V.Normal[0] = 0, V.Normal[1] = 0, V.Normal[2] = -1, V.NormalRead = true;
        X.Surfaces = { Real, Triangle("m_unread_down", -1) };   // the second: cross down, no read normal
        Mixed.Objects = { X };
        const auto UseD = GeometryUse(Mixed);
        Check(UseD.at("m_real_down").Ceiling > 0 && UseD.at("m_unread_down").Floor > 0, "a surface without read normals follows the winding even when another surface has normals");
        Mixed.Game = "iw3";   // winding +1: the unread surface's cross product points down, so it is a ceiling by the sign convention, not a floor by the default normal
        const auto UseE = GeometryUse(Mixed);
        Check(UseE.at("m_unread_down").Ceiling > 0 && UseE.at("m_unread_down").Floor == 0, "an unread surface is not counted as a floor by the default normal");
        {
            const float At[3] = { 350.8f, 1180.3f, 389.0f };
            Check(Maps::LightGuid(At, 0) != Maps::LightGuid(At, 1) && Maps::LightGuid(At, 7) == Maps::LightGuid(At, 7), "two light records at one origin get different guids (the record index is in the hash)");
        }
        // a real flat floor reads exactly (0, 0, 1): it is a floor by its read normal, whichever way the winding runs
        MapScene Flat;
        Flat.Game = "bo4";
        Mesh Fl;
        Fl.Name = "world";
        Surface Floor = Triangle("m_flat_floor", 1);            // cross up: bo4 winding -1 alone would call it a ceiling; only the read normal (0, 0, 1) makes it a floor
        for (Vertex& V : Floor.Vertices) V.Normal[0] = 0, V.Normal[1] = 0, V.Normal[2] = 1, V.NormalRead = true;
        Fl.Surfaces = { Floor };
        Flat.Objects = { Fl };
        FacingStats FlatStats;
        const auto UseF = GeometryUse(Flat, nullptr, &FlatStats);
        Check(UseF.at("m_flat_floor").Floor > 0 && UseF.at("m_flat_floor").Ceiling == 0 && FlatStats.NormalsRead, "a surface whose read normal is exactly (0, 0, 1) is a floor (the value is no sentinel)");
        // mwr: the cross product runs opposite the vertex normal, so a surface with no read normal follows winding -1
        MapScene Mwr;
        Mwr.Game = "mwr";
        Mesh Mw;
        Mw.Name = "world";
        Mw.Surfaces = { Triangle("m_mwr_cross_down", -1) };
        Mwr.Objects = { Mw };
        Check(GeometryUse(Mwr).at("m_mwr_cross_down").Floor > 0, "mwr has winding -1: a surface whose cross product points down is a floor");
    }

    // what the maps draw, read back from the lists the index names (the stand-alone gate and the hook share this)
    {
        const fs::path Folder = fs::temp_directory_path() / ("graywolf_selftest_drawn_" + std::to_string(GetCurrentProcessId()));
        fs::remove_all(Folder, Ec);
        fs::create_directories(Folder, Ec);
        const std::string Long = "mtl_a_very_long_material_name_that_goes_well_past_the_sixty_three_character_limit_of_an_asset";
        MapDependencyList One;
        One.Map = "mp_d";
        One.Dependencies = { { "material", Long, "surface:world#4", "facing=10.000:0.000:0.000" }, { "material", "mtl_short", "surface:world#5", "facing=0.000:0.000:3.000" } };
        std::ofstream(Folder / ("mp_d" + std::string(FullMapListSuffix)), std::ios::binary) << FormatDependencyList(One);
        BeginRunMapFiles(Folder);
        NoteRunMapFile(Folder, "mp_d" + std::string(FullMapListSuffix), "list");
        std::map<std::string, std::string> Drawn;
        std::string Why;
        const bool Ok = ReadDrawnMaterials(Folder, Drawn, Why);
        const std::vector<std::string> Keys = DrawnAssetKeys(Long);
        Check(Ok && Why.empty() && Drawn.count("mtl_short") == 1 && Keys.size() == 2 && Keys[1].size() <= 63 && Keys[1] != Keys[0] && Drawn.count(Keys[0]) == 1 && Drawn.count(Keys[1]) == 1 && Drawn.at(Keys[1]) == "world#4",
              "the drawn materials are read under the full and the shortened asset name", Why + " / " + std::to_string(Drawn.size()));
        // a list that does not read, and one with an old row, make the evidence incomplete
        std::ofstream(Folder / ("mp_bad" + std::string(FullMapListSuffix)), std::ios::binary) << "not a list";
        NoteRunMapFile(Folder, "mp_bad" + std::string(FullMapListSuffix), "list");
        std::map<std::string, std::string> Drawn2;
        std::string Why2;
        Check(!ReadDrawnMaterials(Folder, Drawn2, Why2) && Why2.find("mp_bad") != std::string::npos, "an unreadable list makes the drawn-materials evidence incomplete", Why2);
        MapDependencyList Old;
        Old.Map = "mp_o";
        Old.Dependencies = { { "material", "mtl_old", "surface:floor", "usage=floor" } };
        BeginRunMapFiles(Folder);
        std::ofstream(Folder / ("mp_o" + std::string(FullMapListSuffix)), std::ios::binary) << FormatDependencyList(Old);
        NoteRunMapFile(Folder, "mp_o" + std::string(FullMapListSuffix), "list");
        std::map<std::string, std::string> Drawn3;
        std::string Why3;
        Check(!ReadDrawnMaterials(Folder, Drawn3, Why3) && Why3.find("mtl_old") != std::string::npos, "an old usage= row makes the evidence incomplete", Why3);
        BeginRunMapFiles(Folder);
        std::map<std::string, std::string> Drawn4;
        std::string Why4;
        Check(!ReadDrawnMaterials(Folder, Drawn4, Why4), "an index naming no list makes the evidence incomplete", Why4);
        fs::remove(Folder / MapIndexFile, Ec);
        Check(!ReadDrawnMaterials(Folder, Drawn4, Why4), "no index makes the evidence incomplete", Why4);
        fs::remove_all(Folder, Ec);
    }
    return Failed;
}
}
