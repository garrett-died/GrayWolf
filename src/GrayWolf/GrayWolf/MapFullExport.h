#pragma once

// --full-map: what a map names, as data, for the publish step (Bo3Target.Publish.h). Taken from a MapScene,
// so every game with a map reader gets it (docs/hydrax-port/full-export-spec.md 2 and 5.1):
//
//   - CollectMapDependencies: the static, dynamic and splined models, a destructible's other model, the sky
//     model, the entity string's model values (never "*N" brush or "?N" trigger models), the asset keys
//     (skyboxmodel, *_model, *Model<n>: xmodels; lutmaterial: a material; *Anim<n>: xanims; fxdef, *efx,
//     ".efx" values: effects) and the materials of the map's own render surfaces;
//   - WriteFullMapFiles: the map's geometry materials as the sidecar of a model that has no file (they are
//     then published with the models' materials, the usage Radiant lists them under carried in the list),
//     and the list itself, <map>.full_map.tsv beside the .map;
//   - the publish step reads the list, publishes through the same path as models, and writes
//     graywolf_full_map_report.tsv.

#include "MapScene.h"
#include "Bo3Target.Techsets.h"
#include "MaterialDescription.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Maps
{
    // Type: xmodel, material, fx, xanim. Via: where the map names it (staticModel, model, surface:floor, ...).
    // Detail: for a geometry material what the map measured (FormatMapUse: "facing=<floor>:<ceiling>:<wall>[;role=terrain][;role=sky]");
    // the usage itself is chosen later, from the BO3 techset the material gets (Techsets::UsageRules). Via is then
    // "surface:<mesh>#<number>" (or "sky:<model>#<number>"): the first surface that draws it.
    struct MapDependency
    {
        std::string Type, Name, Via, Detail;
    };

    // Whether an entity key names an xmodel (model, skyboxmodel, *_model, *Model<n>), a material (lutmaterial),
    // an xanim (*Anim<n>) or an effect (fxdef, *efx); the value of a model key is not "*N" or "?N". "" for none.
    std::string EntityAssetType(const std::string& Key, const std::string& Value);

    // The asset name a game's model is published under: the name the model export gives its folder, as the
    // GDT names it (lower case, a-z 0-9 and _), shortened to 63 characters as the publish step does. The
    // Radiant map names its models by it.
    std::string FullMapAssetName(const std::string& GameName);

    // Each (type, name) once, in the order the scene names it
    std::vector<MapDependency> CollectMapDependencies(const MapScene& Scene);

    // Whether a scene material is a decal: its techset names one, or (Black Ops 4, whose techset names are hashes) the techset's
    // renderFlags say so (SurfaceTypes::IsDecal, zonekit/profiles/<game>/flag_names.txt). False for a name the scene does not hold.
    bool IsDecalMaterial(const MapScene& Scene, const std::string& MaterialName);

    // What the map measured about each material its own geometry draws (Techsets::MapUse): the area of the render triangles by the way
    // they face, a terrain mesh (a world mesh named terrain*) or the sky model as the role, and the first surface that names it. Every
    // surface with a material counts, whatever game the techset name came from; the usage is chosen later from the BO3 techset.
    // Only: just those materials. A material no world surface draws has none.
    // Stats: how the facing was decided (see FacingAreas): triangles measured, those placed by a vertex normal, and how the cross product
    // lay against that normal. The winding of a game whose reader gives no normal is Maps::WindingOf.
    struct FacingStats
    {
        size_t Triangles = 0, WithNormal = 0, CrossAgrees = 0, CrossAgainst = 0;
        bool NormalsRead = false;
    };
    std::map<std::string, Bo3Target::Techsets::MapUse> GeometryUse(const MapScene& Scene, const std::set<std::string>* Only = nullptr, FacingStats* Stats = nullptr);
    // +1 when cross(b - a, c - a) points out of the front of a triangle of this game, -1 when it points into it. Measured: Black Ops 4 -1
    // (6,337 of 6,346 triangles, HydraX map-export-bo4 section 21) and Black Ops III -1 ((p2 - p0) x (p1 - p0) is the vertex normal). Other
    // games: +1 is NOT measured (their readers either give normals, which are used, or this is a hypothesis).
    int WindingOf(const std::string& Game);

    // The lower-case GDT asset names a map material goes by: its asset name and the name shortened to 63 characters
    std::vector<std::string> DrawnAssetKeys(const std::string& Name);
    // The materials the run's maps draw (asset names from DrawnAssetKeys -> first surface), from the lists the index in MapFolder names.
    // False, with Why, when the index is missing, a list does not read, a material row is old or unreadable, or no list was found:
    // the evidence is then incomplete and Drawn must not be taken for all of it.
    bool ReadDrawnMaterials(const std::filesystem::path& MapFolder, std::map<std::string, std::string>& Drawn, std::string& Why);

    // The Detail text of a MapUse and its reading back; false for a text that is not one
    std::string FormatMapUse(const Bo3Target::Techsets::MapUse& Use);
    bool ParseMapUse(const std::string& Detail, const std::string& Via, Bo3Target::Techsets::MapUse& Use);

    // The run's setting (--full-map sets "full_map" to "on", with a --target)
    inline bool FullMapOn(const std::map<std::string, std::string>& Settings)
    {
        const auto It = Settings.find("full_map");
        return It != Settings.end() && It->second == "on";
    }

    constexpr const char* FullMapListSuffix = ".full_map.tsv";
    // The folder (and so the model name) the geometry materials are published with
    std::string GeometryOwnerName(const std::string& Map);

    // Writes <ModelsDir>/<map>_geometry/<map>_geometry_LOD0.materials.json (the render surfaces' materials, their
    // images named from the scene's image files, which must be set) and Stem + FullMapListSuffix. Counters:
    // map_full_dependencies_<type>, map_full_geometry_materials. False with Error when a file cannot be written.
    bool WriteFullMapFiles(const MapScene& Scene, const Materials::IAdapter& Adapter, const std::filesystem::path& ModelsDir,
                           const std::string& Stem, std::map<std::string, size_t>& Counters, std::string& Error);

    // The files the map export made THIS run (the .map and prefab files, the lists), one row "kind<TAB>file"
    // (kind map or list) in <maps folder>/graywolf_map_index.tsv. The publish step handles only these: a stale
    // .map or list from an earlier run in the folder is not published. BeginRunMapFiles at the run's start
    // (clears the set and removes the index); NoteRunMapFile for each file made (thread safe, rewrites the index);
    // ReadMapIndex in the publish step. False from ReadMapIndex: no index (a folder not made by a run of this build).
    constexpr const char* MapIndexFile = "graywolf_map_index.tsv";
    void BeginRunMapFiles(const std::filesystem::path& MapDir);
    bool NoteRunMapFile(const std::filesystem::path& MapDir, const std::string& FileName, const char* Kind);
    bool ReadMapIndex(const std::filesystem::path& MapDir, std::vector<std::pair<std::string, std::string>>& Rows);

    // Adds dependencies to a list file already written (the Radiant writer makes the world's tile models after
    // the list is written): the ones the list does not name yet. False when the list cannot be read or written.
    bool AppendDependencies(const std::filesystem::path& ListFile, const std::vector<MapDependency>& More);

    struct MapDependencyList
    {
        std::string Map, Owner;                  // Owner: the geometry model's folder ("" when none)
        std::vector<MapDependency> Dependencies;
    };
    std::string FormatDependencyList(const MapDependencyList& List);
    bool ParseDependencyList(const std::string& Text, MapDependencyList& List);

    // The dependency collection, the list file and the names on synthetic scenes; the number failed
    int FullMapSelfTest(std::string& Failures);
}
