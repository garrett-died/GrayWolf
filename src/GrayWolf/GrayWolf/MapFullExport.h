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
#include "MaterialDescription.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Maps
{
    // Type: xmodel, material, fx, xanim. Via: where the map names it (staticModel, model, surface:floor, ...).
    // Detail: "usage=floor" for a geometry material.
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

    // The usage Radiant lists a geometry material under (MaterialUsage), by the area of the render triangles
    // that draw it; Only: just those materials. A material no world surface draws has none.
    std::map<std::string, std::string> GeometryUsage(const MapScene& Scene, const std::set<std::string>* Only = nullptr);

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

    // The stronger of two usages a material has in two maps (decal, window, floor, ceiling, interior wall): the more
    // specific one, so the result does not depend on the order of the maps
    std::string StrongerUsage(const std::string& A, const std::string& B);

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
