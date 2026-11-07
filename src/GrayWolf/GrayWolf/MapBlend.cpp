#include "stdafx.h"

#include "MapBlend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <format>
#include "MapLive.h"
#include <tuple>

namespace Maps
{
    namespace
    {
        size_t BinOf(int W)
        {
            if (W <= 0) return 0;
            if (W < 64) return 1;
            if (W < 128) return 2;
            if (W < 192) return 3;
            return W < 255 ? 4 : 5;
        }
    }

    BlendMeasure MeasureBlends(const MapScene& Scene)
    {
        BlendMeasure M;
        for (const Mesh& Object : Scene.Objects)
            for (const Surface& S : Object.Surfaces)
            {
                const BlendData& B = S.Blend;
                if (!B.Present()) continue;
                M.Surfaces++;
                const size_t Vertices = S.Vertices.size();
                bool Unread = false;
                std::array<bool, MaxBlendLayers> Seen{}, Above{};
                for (size_t V = 0; V < Vertices; V++)
                {
                    M.Vertices++;
                    bool VertexUnread = false;
                    for (uint32_t L = 0; L < B.Layers && L < MaxBlendLayers; L++)
                    {
                        const int W = B.Of(V, L);
                        if (W < 0)
                        {
                            VertexUnread = true;
                            continue;
                        }
                        M.LayerVertices[L]++;
                        M.Histogram[L][BinOf(W)]++;
                        Seen[L] = true;
                        if (W > 0)
                        {
                            M.LayerVerticesAbove0[L]++;
                            Above[L] = true;
                        }
                    }
                    if (VertexUnread)
                    {
                        M.VerticesUnread++;
                        Unread = true;
                    }
                }
                if (Unread) M.SurfacesUnread++;
                for (uint32_t L = 0; L < B.Layers && L < MaxBlendLayers; L++)
                {
                    M.LayerSurfaces[L]++;
                    if (Seen[L] && !Above[L]) M.LayerNeverVisible[L]++;
                }
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    M.Triangles++;
                    bool Any = false;
                    for (uint32_t L = 0; L < B.Layers && L < MaxBlendLayers; L++)
                    {
                        int Above0 = 0;
                        for (size_t J = 0; J < 3; J++) Above0 += B.Of(S.Indices[K + J], L) > 0 ? 1 : 0;
                        if (Above0 > 0)
                        {
                            M.LayerTriangles[L]++;
                            Any = true;
                        }
                        if (Above0 == 3) M.LayerTrianglesAllCorners[L]++;
                    }
                    if (!Any) M.TrianglesBaseOnly++;
                }
            }
        return M;
    }

    void BlendCounters(const BlendMeasure& M, std::map<std::string, size_t>& Out)
    {
        Out["map_blend_scene_surfaces"] += M.Surfaces;
        Out["map_blend_scene_surfaces_unread"] += M.SurfacesUnread;
        Out["map_blend_triangles"] += M.Triangles;
        Out["map_blend_triangles_base_only"] += M.TrianglesBaseOnly;
        Out["map_blend_vertices"] += M.Vertices;
        Out["map_blend_vertices_unread"] += M.VerticesUnread;
        for (uint32_t L = 0; L < MaxBlendLayers; L++)
        {
            const std::string P = "map_blend_layer" + std::to_string(L + 1) + "_";
            Out[P + "surfaces"] += M.LayerSurfaces[L];
            Out[P + "vertices_above0"] += M.LayerVerticesAbove0[L];
            Out[P + "never_visible_surfaces"] += M.LayerNeverVisible[L];
            Out[P + "triangles"] += M.LayerTriangles[L];
            Out[P + "triangles_all_corners"] += M.LayerTrianglesAllCorners[L];
            Out[P + "vertices_read"] += M.LayerVertices[L];
            for (size_t B = 0; B < 6; B++) Out[P + "weight_bin" + std::to_string(B)] += M.Histogram[L][B];
        }
    }

    std::vector<std::string> BlendLines(const BlendMeasure& M)
    {
        std::vector<std::string> Lines;
        Lines.push_back(std::format("blended scene surfaces {} ({} with a weight not read), {} triangles ({} base only), {} vertices ({} with a weight not read)", M.Surfaces, M.SurfacesUnread, M.Triangles, M.TrianglesBaseOnly,
                                    M.Vertices, M.VerticesUnread));
        for (uint32_t L = 0; L < MaxBlendLayers; L++)
        {
            if (M.LayerSurfaces[L] == 0) continue;
            const auto& H = M.Histogram[L];
            Lines.push_back(std::format("layer {}: {} surfaces, never visible on {}; vertices read {}, above 0 {}; triangles with a corner above 0 {} (all three corners {}); weight bins 0 / 1-63 / 64-127 / 128-191 / 192-254 / 255: {} / {} / {} / {} / {} / {}",
                                        L + 1, M.LayerSurfaces[L], M.LayerNeverVisible[L], M.LayerVertices[L], M.LayerVerticesAbove0[L], M.LayerTriangles[L], M.LayerTrianglesAllCorners[L], H[0], H[1], H[2], H[3], H[4], H[5]));
        }
        return Lines;
    }

    void MeasureBlendEvidence(const MapScene& Scene, const std::function<std::string(const std::string& Material, uint32_t Layer)>& IdOf, std::map<std::string, size_t>& Out)
    {
        // (2) the same position and the same layer id: the same weight. Key: the position's float bits and the id
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, std::string>, int> First;
        for (const Mesh& Object : Scene.Objects)
            for (const Surface& S : Object.Surfaces)
            {
                const BlendData& B = S.Blend;
                if (!B.Present()) continue;
                for (uint32_t L = 0; L < B.Layers && L < MaxBlendLayers; L++)
                {
                    const std::string Id = IdOf(S.Material, L + 1);
                    if (Id.empty()) continue;
                    for (size_t V = 0; V < S.Vertices.size(); V++)
                    {
                        const int W = B.Of(V, L);
                        if (W < 0) continue;
                        uint32_t Bits[3];
                        std::memcpy(Bits, S.Vertices[V].Pos, 12);
                        const auto [It, New] = First.emplace(std::make_tuple(Bits[0], Bits[1], Bits[2], Id), W);
                        if (New) continue;
                        const char* Kind = L < 3 ? "colour" : "color1";
                        Out[std::string("map_blend_shared_") + Kind + "_pairs"]++;
                        Out[std::string("map_blend_shared_") + Kind + (It->second == W ? "_equal" : "_differ")]++;
                    }
                }
            }
        // (1) uv steps and (3) smoothness
        std::array<size_t, MaxBlendLayers> EdgeDiff{}, EdgeCount{}, PairDiff{}, PairCount{};
        for (const Mesh& Object : Scene.Objects)
            for (const Surface& S : Object.Surfaces)
            {
                const BlendData& B = S.Blend;
                if (!B.Present() || S.Vertices.empty()) continue;
                const auto Ratios = [&](int Layer) {
                    std::vector<double> R;
                    for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                        for (size_t J = 0; J < 3; J++)
                        {
                            const size_t A = S.Indices[K + J], C = S.Indices[K + (J + 1) % 3];
                            if (A >= S.Vertices.size() || C >= S.Vertices.size()) continue;
                            double D2 = 0;
                            for (int Axis = 0; Axis < 3; Axis++) D2 += static_cast<double>(S.Vertices[A].Pos[Axis] - S.Vertices[C].Pos[Axis]) * (S.Vertices[A].Pos[Axis] - S.Vertices[C].Pos[Axis]);
                            const double Len = std::sqrt(D2);
                            if (Len < 0.5) continue;
                            double U, Vv;
                            if (Layer < 0)
                                U = S.Vertices[A].Uv[0] - S.Vertices[C].Uv[0], Vv = S.Vertices[A].Uv[1] - S.Vertices[C].Uv[1];
                            else
                            {
                                if (B.Of(A, static_cast<uint32_t>(Layer)) < 0 || B.Of(C, static_cast<uint32_t>(Layer)) < 0) continue;
                                const size_t I = (A * B.Layers + static_cast<size_t>(Layer)) * 2, J2 = (C * B.Layers + static_cast<size_t>(Layer)) * 2;
                                U = B.Uv[I] - B.Uv[J2], Vv = B.Uv[I + 1] - B.Uv[J2 + 1];
                            }
                            R.push_back(std::sqrt(U * U + Vv * Vv) / Len);
                        }
                    return R;
                };
                const auto Quartiles = [](std::vector<double>& R, double& Q1, double& Med, double& Q3) {
                    std::sort(R.begin(), R.end());
                    Q1 = R[R.size() / 4], Med = R[R.size() / 2], Q3 = R[(R.size() * 3) / 4];
                };
                std::vector<double> Base = Ratios(-1);
                double B1 = 0, BM = 0, B3 = 0;
                const bool BaseOk = Base.size() >= 8;
                if (BaseOk) Quartiles(Base, B1, BM, B3);
                for (uint32_t L = 0; L < B.Layers && L < MaxBlendLayers; L++)
                {
                    std::vector<double> R = Ratios(static_cast<int>(L));
                    if (R.size() < 8) continue;
                    double Q1, Med, Q3;
                    Quartiles(R, Q1, Med, Q3);
                    const std::string P = "map_blend_uv_layer" + std::to_string(L + 1) + "_";
                    Out[P + "surfaces"]++;
                    if (Med > 1e-9 && (Q3 - Q1) / Med < 0.25) Out[P + "steady"]++;
                    if (BaseOk && BM > 1e-9 && Med > 0)
                    {
                        const double Q = Med / BM;
                        const char* Bin = Q < 0.25 ? "lt025" : Q < 0.5 ? "lt05" : Q < 0.99 ? "lt099" : Q <= 1.01 ? "same" : Q <= 2 ? "le2" : Q <= 4 ? "le4" : "gt4";
                        Out[P + "vs_base_" + Bin]++;
                    }
                }
                // (3) along triangle edges against pairs of vertices far apart in the surface's own order
                for (uint32_t L = 0; L < B.Layers && L < MaxBlendLayers; L++)
                {
                    for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                        for (size_t J = 0; J < 3; J++)
                        {
                            const int A = B.Of(S.Indices[K + J], L), C = B.Of(S.Indices[K + (J + 1) % 3], L);
                            if (A < 0 || C < 0) continue;
                            EdgeDiff[L] += static_cast<size_t>(std::abs(A - C)), EdgeCount[L]++;
                        }
                    const size_t N = S.Vertices.size();
                    for (size_t V = 0; V < N; V++)
                    {
                        const int A = B.Of(V, L), C = B.Of((V * 7919 + 13) % N, L);
                        if (A < 0 || C < 0) continue;
                        PairDiff[L] += static_cast<size_t>(std::abs(A - C)), PairCount[L]++;
                    }
                }
            }
        for (uint32_t L = 0; L < MaxBlendLayers; L++)
        {
            const std::string P = "map_blend_smooth_layer" + std::to_string(L + 1) + "_";
            Out[P + "edge_mean_x100"] += EdgeCount[L] ? EdgeDiff[L] * 100 / EdgeCount[L] : 0;
            Out[P + "random_pair_mean_x100"] += PairCount[L] ? PairDiff[L] * 100 / PairCount[L] : 0;
        }
    }

    uint8_t BlendAlpha(int Weight, bool Reveal, float Sharpness)
    {
        if (Weight <= 0) return 0;
        const double W = std::min(Weight, 255) / 255.0;
        double Va = W;
        if (Reveal && Sharpness >= 1.0f)
        {
            const double S = std::clamp(1.0 / (2.0 * Sharpness), 0.01, 1.0);   // material.awi limits alphaRevealSoftEdge to 0.01..1: the alpha uses the value the material can hold
            Va = (W * W - S - 0.001) / 0.998;
        }
        Va = std::clamp(Va, 0.0, 1.0);
        return static_cast<uint8_t>(std::lround(Va * 255.0));
    }

    const char* BlendLayerName(uint32_t K)
    {
        static const char* const Bottom[] = { "Asphalt (bottom)", "Rock", "Brick", "Concrete", "Plaster", "Signage - Old", "Damage - Old", "Grunge", "Carpet", "Signage - New", "Dirt",
                                              "Gravel", "Sand", "Mud", "Grass", "Foliage", "Ice", "Snow", "Water", "Paper", "Damage - New", "Debris (top)" };
        return K >= 1 && K <= std::size(Bottom) ? Bottom[K - 1] : nullptr;
    }

    void MarkUnreadBlends(MapScene& Scene)
    {
        for (Mesh& Object : Scene.Objects)
            for (const Surface& S : Object.Surfaces)
            {
                if (!S.Blend.Present()) continue;
                const bool Unread = std::any_of(S.Blend.Weight.begin(), S.Blend.Weight.end(), [](int16_t W) { return W < 0; });
                if (const auto It = Scene.Materials.find(S.Material); Unread && It != Scene.Materials.end()) It->second.SurfacesUnread++;
            }
    }

    void MeasureBlendInterpolation(const MapScene& Scene, std::map<std::string, size_t>& Out)
    {
        size_t Edges = 0, Large = 0;
        double Sum = 0;
        for (const Mesh& Object : Scene.Objects)
            for (const Surface& S : Object.Surfaces)
            {
                const BlendData& B = S.Blend;
                const auto Mat = Scene.Materials.find(S.Material);
                if (!B.Present() || Mat == Scene.Materials.end()) continue;
                for (uint32_t L = 0; L < B.Layers && L < MaxBlendLayers && L < Mat->second.Layers.size(); L++)
                {
                    if (!Mat->second.Layers[L].Reveal) continue;
                    for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                        for (size_t J = 0; J < 3; J++)
                        {
                            const int A = B.Of(S.Indices[K + J], L), C = B.Of(S.Indices[K + (J + 1) % 3], L);
                            if (A < 0 || C < 0 || (A == 0 && C == 0)) continue;
                            const double D = (A - C) / 510.0, Err = D * D;
                            Edges++;
                            Sum += Err;
                            Large += Err > 0.05 ? 1 : 0;
                        }
                }
            }
        Out["map_blend_interp_edges"] += Edges;
        Out["map_blend_interp_mean_err_x1000"] += Edges ? static_cast<size_t>(Sum / static_cast<double>(Edges) * 1000.0 + 0.5) : 0;
        Out["map_blend_interp_edges_err_above_0_05"] += Large;
    }

    int BlendSelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("blend: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        // two triangles over four vertices, two layers: layer 1 is 0 at vertices 0..2 and 200 at vertex 3; layer 2 is 255 at vertex 0, -1 (unread) at vertex 2
        MapScene Scene;
        Mesh World;
        Surface S;
        S.Material = "m";
        S.Vertices.resize(4);
        S.Indices = { 0, 1, 2, 1, 2, 3 };
        S.Blend.Layers = 2;
        S.Blend.Weight = { 0, 255, 0, 0, 0, -1, 200, 0 };
        World.Surfaces.push_back(S);
        Scene.Objects.push_back(World);
        const BlendMeasure M = MeasureBlends(Scene);
        Check(M.Surfaces == 1 && M.Triangles == 2 && M.Vertices == 4, "the surface, its triangles and vertices are counted", std::to_string(M.Surfaces));
        Check(M.VerticesUnread == 1 && M.SurfacesUnread == 1, "a vertex with an unread weight is counted unread, never as zero", std::to_string(M.VerticesUnread));
        Check(M.LayerVerticesAbove0[0] == 1 && M.LayerVertices[0] == 4 && M.LayerVerticesAbove0[1] == 1 && M.LayerVertices[1] == 3, "vertices above 0 are counted per layer", std::to_string(M.LayerVerticesAbove0[0]));
        Check(M.LayerTriangles[0] == 1 && M.LayerTrianglesAllCorners[0] == 0 && M.LayerTriangles[1] == 1, "a triangle is a layer's when a corner is above 0", std::to_string(M.LayerTriangles[0]) + "/" + std::to_string(M.LayerTriangles[1]));
        Check(M.TrianglesBaseOnly == 0, "no triangle of this surface is base only");
        Check(M.LayerNeverVisible[0] == 0 && M.LayerNeverVisible[1] == 0, "a layer with a vertex above 0 is visible");
        Check(M.Histogram[0][0] == 3 && M.Histogram[0][4] == 1 && M.Histogram[1][5] == 1 && M.Histogram[1][0] == 2, "the weights are binned", std::to_string(M.Histogram[0][0]));
        // a layer that is 0 at every read vertex is never visible; one that is unread everywhere is not "never visible" (unknown)
        Scene.Objects[0].Surfaces[0].Blend.Weight = { 0, -1, 0, -1, 0, -1, 0, -1 };
        const BlendMeasure N = MeasureBlends(Scene);
        Check(N.LayerNeverVisible[0] == 1 && N.LayerNeverVisible[1] == 0 && N.TrianglesBaseOnly == 2, "a layer of zeros is never visible, a layer never read is unknown", std::to_string(N.LayerNeverVisible[0]) + "/" + std::to_string(N.LayerNeverVisible[1]));
        // the vertex alpha: no reveal is the weight; a reveal layer is (w^2 - s - 0.001) / 0.998 with s = 1/(2K)
        Check(BlendAlpha(255, false, 0) == 255 && BlendAlpha(128, false, 0) == 128 && BlendAlpha(0, false, 0) == 0 && BlendAlpha(-1, false, 0) == 0, "no reveal: the vertex alpha is the weight, unread is 0");
        Check(BlendAlpha(255, true, 4.0f) == 223 && BlendAlpha(0, true, 4.0f) == 0 && BlendAlpha(100, true, 4.0f) == 7 && BlendAlpha(200, true, 4.0f) == 125, "a reveal layer (K 4): w^2 - 1/8 over 0.998", std::to_string(BlendAlpha(255, true, 4.0f)) + "/" + std::to_string(BlendAlpha(200, true, 4.0f)));
        Check(BlendAlpha(255, true, 512.0f) == 253 && BlendAlpha(255, true, 2.0f) == 191, "the sharper the reveal, the closer to w^2", std::to_string(BlendAlpha(255, true, 512.0f)) + "/" + std::to_string(BlendAlpha(255, true, 2.0f)));
        Check(std::string(BlendLayerName(1)) == "Asphalt (bottom)" && std::string(BlendLayerName(22)) == "Debris (top)" && BlendLayerName(0) == nullptr && BlendLayerName(23) == nullptr && BlendLayerName(6) != nullptr, "BO3's decal layers, bottom to top");
        // two world surfaces of one material that share a vertex number: each keeps a corner of its own (its layer data is read for it), counted; a vertex below the surface's first
        // still has the material's layers (the blend is not turned off for the whole scene surface)
        {
            int Calls = 0;
            WorldBuilder::Fetch Read = [](uint32_t I, Vertex& V) {
                V.Pos[0] = static_cast<float>(I);
                return true;
            };
            WorldBuilder::FetchBlend Layers = [&](uint32_t I, const Vertex&, BlendVertex& Out) {
                Calls++;
                if (I >= 100) return 0u;   // an index no surface owns: the reader still gives the material's layer count
                Out.Weight[0] = static_cast<int16_t>(10 * Calls);
                return 1u;
            };
            size_t Shared = 0;
            WorldBuilder Builder("w", Read, {}, Layers);
            Builder.CountShared(&Shared);
            Builder.SetSource(1);
            Builder.Triangle("m", 0, 1, 2);
            Builder.SetSource(2);
            Builder.Triangle("m", 1, 2, 3);
            const Mesh Made = Builder.Take();
            Check(Made.Surfaces.size() == 1 && Made.Surfaces[0].Vertices.size() == 6 && Shared == 2, "a vertex two surfaces share is read for each, and counted", std::to_string(Made.Surfaces.empty() ? 0 : Made.Surfaces[0].Vertices.size()) + "/" + std::to_string(Shared));
            Check(Made.Surfaces[0].Blend.Layers == 1 && Made.Surfaces[0].Blend.Weight.size() == 6 && Made.Surfaces[0].Blend.Weight[3] == 40, "each copy has the data read for its own surface", std::to_string(Made.Surfaces[0].Blend.Weight[3]));
            // a plain game (no blend reader) reads a shared vertex once, as ever
            WorldBuilder Plain("w", Read);
            Plain.SetSource(1);
            Plain.Triangle("m", 0, 1, 2);
            Plain.SetSource(2);
            Plain.Triangle("m", 1, 2, 3);
            Check(Plain.Take().Surfaces[0].Vertices.size() == 4, "without a blend reader a shared vertex is one vertex");
            // the first vertex of a surface below the surface's base index still gives the layer count
            int Calls2 = 0;
            WorldBuilder::FetchBlend Outside = [&](uint32_t I, const Vertex&, BlendVertex& Out) {
                Calls2++;
                if (I >= 100) return 2u;
                Out.Weight[0] = 1;
                return 2u;
            };
            WorldBuilder Below("w", Read, {}, Outside);
            Below.Triangle("m", 200, 1, 2);
            Check(Below.Take().Surfaces[0].Blend.Layers == 2, "a first vertex outside the surface does not make the surface a non-blend");
        }
        // unread blends are marked on their material; the interpolation measure is ((a - b) / 2)^2 on the edges of reveal layers
        {
            MapScene Marked = Scene;
            Marked.Materials["m"].Name = "m";
            MarkUnreadBlends(Marked);
            Check(Marked.Materials.at("m").SurfacesUnread == 1, "a surface with an unread weight is marked on its material", std::to_string(Marked.Materials.at("m").SurfacesUnread));
            MapScene Edge;
            Mesh World2;
            Surface Tri;
            Tri.Material = "r";
            Tri.Vertices.resize(3);
            Tri.Indices = { 0, 1, 2 };
            Tri.Blend.Layers = 1;
            Tri.Blend.Weight = { 0, 255, 0 };
            World2.Surfaces.push_back(Tri);
            Edge.Objects.push_back(World2);
            Edge.Materials["r"].Layers.push_back({ "r_dec", true, 4.0f });
            std::map<std::string, size_t> Interp;
            MeasureBlendInterpolation(Edge, Interp);
            // edges 0-255 (0.25), 255-0 (0.25) and 0-0 (skipped): two edges of 0.25
            Check(Interp["map_blend_interp_edges"] == 2 && Interp["map_blend_interp_mean_err_x1000"] == 250 && Interp["map_blend_interp_edges_err_above_0_05"] == 2, "a 0 to 1 edge of a reveal layer differs by 0.25 in w^2 at its middle", std::to_string(Interp["map_blend_interp_mean_err_x1000"]));
            Edge.Materials["r"].Layers[0].Reveal = false;
            std::map<std::string, size_t> None;
            MeasureBlendInterpolation(Edge, None);
            Check(None["map_blend_interp_edges"] == 0, "a layer with no reveal is linear in both: no difference");
        }
        std::map<std::string, size_t> C;
        BlendCounters(N, C);
        Check(C["map_blend_layer1_never_visible_surfaces"] == 1 && C["map_blend_scene_surfaces_unread"] == 1, "the counters carry the measure");
        return Failed;
    }
}
