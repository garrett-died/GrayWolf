#pragma once

// A surface type for materials whose own word names several (M4a, docs/hydrax-port/m4.md).
//
// A layered blend of Modern Warfare Remastered holds the union of its layers' surface bits; BO3 takes one. The reader resolves the first layer by intersecting the words
// of the level's blends (Games/H1/LayerBits.h); what that leaves ambiguous is measured here against two other sources, in this order:
//   1. the collision twins: a render triangle of the material that a collision triangle sits on (MapCollisionMeshes.h MatchTwins) gives the clip material's surface index of that
//      triangle's partition. The index maps to a surface bit by the game's rule (Config). A bit wins only when it is one of the material's own bits, at least MinTriangles
//      triangles were twinned, and its share of all the twinned triangles is at least MinShare. The share and the count are in the evidence (kind=collision).
//   2. for a layer material (non-colliding, so it has no twin): the same art (colour image) in a plain material of the level that has exactly one surface bit, when every such
//      plain material agrees and the bit is one of the layer's own (kind=same_art).
// Nothing else is taken: a material that neither source settles keeps its ambiguous word (the gate reports it). The evidence goes to Material::SurfaceVia and is written as a
// material.surface_from_collision row.

#include "MapScene.h"

#include <cstdint>
#include <map>
#include <string>

namespace Maps
{
    struct SurfaceFromCollisionConfig
    {
        bool Enabled = false;     // a game that has not measured its clip surface rule leaves this off
        uint32_t Shift = 0;       // the clip material's surface word >> Shift & Mask is the clip surface index
        uint32_t Mask = 0;
        int Offset = 0;           // surface bit = index + Offset (index 0: the clip material names no surface)
        double MinShare = 0.9;    // the winner's share of all the twinned triangles of the material
        size_t MinTriangles = 8;  // twinned triangles a material needs before its tally counts
    };

    // The winning bit of a tally (clip surface index -> triangles) for a material whose word is Word, or -1. Total is the twinned triangles counted, Share the winner's share.
    int WinningBit(const std::map<uint32_t, size_t>& ByIndex, uint64_t Word, const SurfaceFromCollisionConfig& Config, size_t& Total, double& Share);

    // Sets SurfaceFlags and SurfaceVia of the scene's materials whose word has two or more bits (see above). Counters: map_surface_collision_* and map_surface_same_art_*.
    void ResolveSurfaceFromCollision(MapScene& Scene, const SurfaceFromCollisionConfig& Config, std::map<std::string, size_t>& Counters);

    int SurfaceFromCollisionSelfTest(std::string& Failures);
}
