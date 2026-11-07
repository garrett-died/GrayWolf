#pragma once

// A MapScene as a Radiant source ("iwmap 4": Black Ops III's mod tools, World at War's, Black
// Ops'), to port a level: <map>.map with worldspawn and the clip map's world brushes (a tool
// material from their content flags; a solid side on drawn world triangles their material and
// projection, MapRadiantTextures.h), every entity of the entity string (a brush entity with its
// brush model's brushes, textured the same way from its drawn triangles), the drawn triangles no
// brush side draws as patches (a 2x2 mesh each, nonColliding), a misc_model per placement (and
// per piece of world geometry a patch cannot carry); and <map>_radiant/ the models it names as
// XMODEL_EXPORT with a GDT of them.

#include "MapLive.h"
#include "MapRadiantRules.h"
#include "MapRadiantTextures.h"
#include "MapRenderMeshes.h"
#include "MapScene.h"

#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace Maps
{
    // A side of a brush that bounds something: its polygon (counter-clockwise seen from outside)
    struct BrushFace
    {
        size_t Side = 0;   // the side it lies on (a hull face on no side: -1)
        std::vector<std::array<double, 3>> Points;
        std::array<double, 3> Normal = { 0, 0, 1 };   // its plane, outward: Normal . p == Dist
        double Dist = 0;
    };
    // The faces of a convex brush (its sides clipped by one another); a side bounding nothing (a
    // bevel, a duplicate) has none; nor has a sliver, or a face lying on another side's plane, when
    // the brush stays within Within of its plane without it. Fewer than four faces: not a closed
    // volume.
    std::vector<BrushFace> BrushFaces(const Brush& B, double Within = 0.01);
    // A collision triangle as the brush this export writes for one of the world's: a prism Thick
    // units deep behind its face (P facing out along (P1 - P0) x (P2 - P0)); false for one without
    // area (a game's reader making a brush model's triangles into brushes uses it)
    bool TrianglePrism(const std::array<double, 3> P[3], double Thick, Brush& B);

    // A prefab file written beside the map (--map-prefabs): <map>_prefab_<part>.map, a worldspawn and the part
    struct RadiantPrefabFile
    {
        PrefabPart Part = PrefabPart::Brushes;
        std::string File;   // the file name
        size_t Entities = 0, Brushes = 0, Meshes = 0;   // entities besides the worldspawn; its brushes, meshes (patches) in all
    };

    struct RadiantStats
    {
        size_t Brushes = 0, BrushesDropped = 0, BrushesUnsound = 0, Faces = 0, Entities = 0, BrushEntities = 0, BrushEntitiesEmpty = 0;
        size_t Placements = 0, WorldModels = 0, ModelFiles = 0, ModelsElsewhere = 0, TriggerBrushes = 0;
        size_t ModelsPublished = 0;   // --full-map: placed models left to the publish step
        // the sides given the drawn world's materials: the clip map's world brushes', the collision
        // triangles' prisms', the brush entities'
        RadiantTextureStats Textures, PrismTextures, EntityTextures;
        // the drawn triangles no brush side draws: patches (of them vertex-coloured), left out
        // (no area), left to the world's misc_models (no material, a coordinate not finite)
        size_t Patches = 0, PatchesColoured = 0, TrianglesDegenerate = 0, TrianglesInModels = 0;
        // brushes written as the hull of the clip map's corners (pieces of them, when split to Radiant's
        // limits); brushes with corners whose hull was no solid or no sound brush (the sides instead);
        // split; over the limits and not written (listed in Unresolved); the hull faces that lie on a
        // side and on none (the brush's most common material)
        size_t BrushesHull = 0, BrushPieces = 0, BrushesHullNone = 0, BrushesHullUnsound = 0, BrushesSplit = 0, BrushesOverLimit = 0;
        size_t HullFacesMatched = 0, HullFacesUnmatched = 0;
        // light entities written; static models at pitch +-90 (the horizontal part of their forward under
        // 1e-4: roll 0, yaw from the left vector), of them the ones in [1e-6, 1e-4) the plain formulas
        // gave a noise yaw and roll; entity keys moved to graywolf_unresolved_<key>
        // patches whose front (as written) agrees with the vertex normals of the triangle, and does not
        size_t PatchesFacingNormal = 0, PatchesFacingAgainst = 0;
        // the render meshes (MapRenderMeshes.h): what the mesher did with the drawn triangles, summed over every owner and kind; owners whose weld tolerance the
        // measured distances gave, and owners where noise and spacing were not separated (tolerance 0)
        RenderMeshCounters Mesh;
        size_t OwnersWelded = 0, OwnersNotSeparated = 0;
        double MeshTextMaxAbsT = 0;
        size_t MeshMilliseconds = 0;   // wall time of the mesher (BuildRenderMeshes), all owners
        // M2c: the collision triangles as meshes (MapCollisionMeshes.h). CollisionTaken: per object, surface and triangle, the world triangles that leave the render pass
        RenderMeshCounters CollisionMesh;
        size_t CollisionTriangles = 0, CollisionTwinned = 0, CollisionAmbiguous = 0, CollisionDecalOnly = 0, CollisionDecalBeside = 0, CollisionTwinDrawnByBrush = 0, CollisionRenderTaken = 0;
        size_t CollisionRenderCandidates = 0, RenderWorldTaken = 0, RenderWorldByBrush = 0, RenderWorldInPass = 0;   // the world patch-fate triangles, and where each went
        size_t CollisionExhausted = 0, CollisionRenderStayed = 0, CollisionToolRejected = 0;
        std::map<uint64_t, size_t> BrushToolRows;   // brush sides named by a profile row (ClipToolKey), by row
        std::string CollisionToolSummary;           // the tool meshes by name and contents, as written to <map>.meshes.txt (for <map>.ownercheck.txt)
        size_t CollisionSurfaceCompared = 0, CollisionSurfaceAgree = 0;   // twin pairs whose clip surface word and material surface word could both be read, and of them equal
        // decal layers: meshes written with an editor layer (and per layer), meshes whose sort has no evidenced name, meshes with no sort read or sort 0
        size_t DecalMeshesLayered = 0, DecalMeshesSortUnnamed = 0, DecalMeshesNoSort = 0;
        size_t DecalSortNonzeroOnDecal = 0, DecalSortZeroOnDecal = 0, DecalSortNonzeroOnSurface = 0, DecalSortZeroOnSurface = 0;   // the world triangles by decal sort (nonzero / zero) and kind of material
        std::map<std::string, size_t> DecalLayerMeshes;
        // M3: the layers of blended surfaces as decal meshes (WriteLayerPass), by the layer's place above the base (index 0 = the first)
        std::array<size_t, MaxBlendLayers> LayerMeshes{}, LayerTriangles{}, LayerTrianglesSkippedZero{};
        size_t LayerBytes = 0, LayerSurfacesNoMaterial = 0, LayerTrianglesUnread = 0, LayerNoColour = 0;
        std::vector<std::vector<std::vector<char>>> CollisionTaken;
        // one block per material whose coverage failed (written to <map>.coverage.txt)
        std::vector<std::string> CoverageLines;
        // one line per owner and kind (<map>.meshes.txt): the triangles, the measured vertex distances and the weld tolerance they gave, and what the mesher made of them
        std::vector<std::string> MeshLines;
        // <map>.ownercheck.txt: what to open in Radiant and where to look (M2c)
        std::vector<std::string> OwnerCheckLines;
        size_t Probes = 0, ProbesNoSize = 0, SunVolumes = 0, EffectPlacements = 0, EffectPlacementsRefused = 0, SsiStockValues = 0;   // M4: reflection probes (and those with no size read), volume_sun entities, script-placed effects written
        size_t Lights = 0, PlacementsPolar = 0, PlacementsPolarNoise = 0, KeysUnresolved = 0;
        // what was not written or not resolved, one line each (<map>.unresolved.txt)
        std::vector<std::string> Unresolved;
        // the prefab files written (empty parts write none) and what the main map keeps: its entities besides the
        // worldspawn and the misc_prefab calls, its brushes and meshes
        std::vector<RadiantPrefabFile> PrefabFiles;
        size_t MainEntities = 0, MainBrushes = 0, MainMeshes = 0;
    };
    // With Gdt (the material_gdt setting), the model files name their materials by their GDT asset
    // names (MaterialGdt::AssetName) and the GDT is left to the caller: its model entries and the
    // scene materials the model files name are returned there.
    struct RadiantGdt
    {
        std::filesystem::path Root;            // <map>_radiant
        std::string Map;                       // the map's folder name below model_export
        std::vector<std::string> ModelEntries;  // the xmodel entries (LF)
        std::set<std::string> Materials;       // scene material names
    };
    // The writer on small synthetic scenes (a temp folder, no game files); the number failed
    int RadiantWriterSelfTest(std::string& Failures);
    // --full-map: the placed models are not written here (the model export wrote them as XMODEL_BIN and the publish step
    // places them: the map names them by FullMapAssetName), the pieces of the world are XMODEL_BIN in ModelsDir too, and
    // there is no <map>_radiant folder and no GDT of its own
    struct RadiantFullMap
    {
        std::filesystem::path ModelsDir;
    };
    // The scene's entities as the Radiant writer reads them: the entity string parsed, the numbered keys named
    std::vector<Entity> SceneEntities(const MapScene& Scene);
    // Prefabs: the parts written to a standalone map each, the main map then holding one misc_prefab per file
    // (Prefabs empty: one map, as ever)
    bool WriteRadiant(const MapScene& Scene, const std::string& MapPath, RadiantStats& Stats, std::string& Error,
                      RadiantGdt* ForGdt = nullptr, const std::vector<PrefabPart>& Prefabs = {}, const RadiantFullMap* Full = nullptr);
}
