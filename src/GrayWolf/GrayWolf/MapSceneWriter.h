#pragma once

// Port: a MapScene written in one format. The map exporter (Maps::WriteMapFiles) asks each
// enabled writer in turn - the .glb always, the Cast scene with map_cast, the Radiant source with map_radiant - with one
// failure and counter handling; another format is one more implementation of this.

#include "MapScene.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Materials { class IAdapter; }

namespace Maps
{
    struct SceneWriteResult
    {
        std::vector<std::pair<std::string, size_t>> Counters;   // report counters, by name
        // work deferred until the run's dependencies are written (the .glb's auto alpha): it adds
        // its result to the counter LaterCounter
        std::function<size_t()> Later;
        std::string LaterCounter;
        // the same, for work that measures several things: it adds them to the counters it is given
        std::function<void(std::map<std::string, size_t>& Counters)> LaterCounters;
        // other files the writer made beside Path (the Radiant prefab files): their names, in Path's folder
        std::vector<std::string> Files;
        // data files the writer made under <map>_radiant (source_data, model_export: the ssi and sky GDTs, the sky EXR), as paths from Path's folder with / separators: the publish step
        // places each at the same path below <map>_radiant in the Mod Tools folder
        std::vector<std::string> DataFiles;
    };

    class ISceneWriter
    {
    public:
        virtual ~ISceneWriter() = default;
        virtual const char* Extension() const = 0;   // ".glb"
        virtual bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter,
                           SceneWriteResult& Result, std::string& Error) const = 0;
    };

    const ISceneWriter& GlbWriter();    // MapGlbWriter.cpp: the Blender scene, with its import script
    const ISceneWriter& CastWriter();   // MapCastWriter.cpp: the Cast scene (Maya, the Cast plugins)
    // MapRadiantWriter.cpp: the Radiant source (the mod tools); Materials: with the material GDT
    // entries of its models' materials (the material_gdt setting)
    // Prefabs: the --map-prefabs list ("" none): each part a standalone map beside it (MapRadiantRules.h)
    // FullMapModels: --full-map, the run's models folder; the models then go there as XMODEL_BIN for the publish step
    const ISceneWriter& RadiantWriter(bool Materials = false, const std::string& Prefabs = std::string(), const std::string& FullMapModels = std::string());
    const ISceneWriter& CollisionWriter();   // MapCollisionWriter.cpp: the clip map's collision as JSON
}
