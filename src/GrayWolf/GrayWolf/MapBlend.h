#pragma once

// A level's blended surfaces (MapScene.h: Surface::Blend), a game's reader fills them. What this file measures and what the Radiant writer builds from them are
// the game-independent part: the weights are 0..255 per vertex and layer (-1: not read), the layers are the game's, bottom to top.

#include "MapScene.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Maps
{
    // What the level's blended surfaces hold (docs/hydrax-port/m3.md): counted per layer (index 0 = the first above the base)
    struct BlendMeasure
    {
        size_t Surfaces = 0;            // scene surfaces (one per object and material) that carry blend data
        size_t SurfacesUnread = 0;      // of them with a vertex weight not read
        size_t Triangles = 0;           // their triangles
        size_t TrianglesBaseOnly = 0;   // triangles where no layer has a weight above 0 at any corner
        size_t Vertices = 0, VerticesUnread = 0;
        std::array<size_t, MaxBlendLayers> LayerSurfaces{};         // surfaces that have the layer
        std::array<size_t, MaxBlendLayers> LayerVertices{};         // vertices (of surfaces that have the layer) with the layer's weight read
        std::array<size_t, MaxBlendLayers> LayerVerticesAbove0{};   // of them above 0
        std::array<size_t, MaxBlendLayers> LayerNeverVisible{};     // surfaces where every weight of the layer is 0 (all read)
        std::array<size_t, MaxBlendLayers> LayerTriangles{};        // triangles with the layer's weight above 0 at a corner
        std::array<size_t, MaxBlendLayers> LayerTrianglesAllCorners{};   // at all three corners
        // the weights of the read vertices in six bins: 0, 1..63, 64..127, 128..191, 192..254, 255
        std::array<std::array<size_t, 6>, MaxBlendLayers> Histogram{};
    };
    BlendMeasure MeasureBlends(const MapScene& Scene);
    // The measure as report counters (map_blend_*)
    void BlendCounters(const BlendMeasure& M, std::map<std::string, size_t>& Out);
    // The measure as lines of text (docs and <map>.blends.txt)
    std::vector<std::string> BlendLines(const BlendMeasure& M);

    // Evidence that does not depend on the stride (the reader's field positions): per layer, (1) the layer's texture coordinates change steadily with position and at a rate
    // comparable to the base layer's, (2) two vertices at one position that carry the same layer id get the same weight, (3) weights vary smoothly along an edge. IdOf gives
    // the id of layer K (1 = the first above the base) of a material, empty when unknown.
    void MeasureBlendEvidence(const MapScene& Scene, const std::function<std::string(const std::string& Material, uint32_t Layer)>& IdOf, std::map<std::string, size_t>& Out);

    // The vertex alpha (0..255) a layer's weight W (0..255) is written as. A layer with a reveal map (sharpness K = revealParams.x) is MWR's B = sat((w^2 - R) * K); BO3's
    // reveal decal gives alpha = sat((r - lo) / (hi - lo)) of a = sat(va * 0.998 + 0.001), lo = sat(1 - a - s*a^p), hi = sat(1 - a + s*(1-a)^p), which with the reveal image
    // r = 1 - R, ramp p = 0 and soft edge s = 1/(2K) is the same where w^2 >= s + 0.001: va = (w^2 - s - 0.001) / 0.998 (mwr-materials.md 12.3; exact above that, BO3 shows a
    // little more below it). A layer with no reveal is B = w: va = w. A weight not read (< 0) is 0. The soft edge s is limited to 0.01..1 (material.awi's range for alphaRevealSoftEdge: a K above 50 cannot be written as 1/(2K)).
    uint8_t BlendAlpha(int Weight, bool Reveal, float Sharpness);
    // BO3's editor decal layer of the K-th layer above the base (K = 1..), bottom to top: the list of material.awi's layerSortDecal read from the bottom. The order is a
    // HYPOTHESIS: the two ends are named in the list ("Asphalt (bottom)", "Debris (top)") and HydraX's terrain export uses this order for its layers; nothing measured says
    // the middle is the draw order. nullptr past the list.
    const char* BlendLayerName(uint32_t K);

    // Counts, per material, the blended surfaces with a layer weight that was not read (Material::SurfacesUnread)
    void MarkUnreadBlends(MapScene& Scene);
    // MWR squares the interpolated weight per pixel (B = sat((w^2 - R) K)); BO3 interpolates the vertex alpha, which holds w^2 - s. On an edge between weights a and b (0..1) the
    // layer's w^2 at its middle is ((a + b) / 2)^2 in MWR and (a^2 + b^2) / 2 in BO3: they differ by ((a - b) / 2)^2 (0 for equal weights, 0.25 for 0 to 1). Over the edges of
    // the triangles of reveal layers (Material::Layers[k].Reveal): the edges, the mean difference x1000 and the edges whose difference is above 0.05 (counters map_blend_interp_*)
    void MeasureBlendInterpolation(const MapScene& Scene, std::map<std::string, size_t>& Out);

    int BlendSelfTest(std::string& Failures);
}
