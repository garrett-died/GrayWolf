#include "stdafx.h"

#include "MapSurfaceFromCollision.h"

#include "MapCollisionMeshes.h"
#include "MapFullExport.h"
#include "MapRadiantTextures.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace Maps
{
    namespace
    {
        int Popcount(uint64_t V)
        {
            int N = 0;
            for (; V != 0; V &= V - 1) N++;
            return N;
        }
    }

    int WinningBit(const std::map<uint32_t, size_t>& ByIndex, uint64_t Word, const SurfaceFromCollisionConfig& Config, size_t& Total, double& Share)
    {
        Total = 0, Share = 0;
        for (const auto& [Index, N] : ByIndex) Total += N;
        if (Total == 0 || Total < Config.MinTriangles) return -1;
        int Best = -1;
        size_t BestN = 0;
        for (const auto& [Index, N] : ByIndex)
        {
            const int Bit = static_cast<int>(Index) + Config.Offset;
            if (Index == 0 || Bit < 0 || Bit > 63 || ((Word >> Bit) & 1) == 0) continue;   // not one of the material's own bits: says nothing about it
            if (N > BestN) Best = Bit, BestN = N;
        }
        if (Best < 0) return -1;
        Share = static_cast<double>(BestN) / static_cast<double>(Total);
        return Share >= Config.MinShare ? Best : -1;
    }

    void ResolveSurfaceFromCollision(MapScene& Scene, const SurfaceFromCollisionConfig& Config, std::map<std::string, size_t>& Counters)
    {
        if (!Config.Enabled) return;
        const auto Ambiguous = [](const Material& M) { return M.SurfaceFlags > 0 && Popcount(static_cast<uint64_t>(M.SurfaceFlags)) >= 2; };
        size_t Wanted = 0;
        for (const auto& [Name, M] : Scene.Materials) Wanted += Ambiguous(M);
        Counters["map_surface_ambiguous_materials"] += Wanted;
        if (Wanted == 0) return;

        // 1. the collision twins
        const Collision& Col = Scene.Collision;
        std::map<std::string, std::map<uint32_t, size_t>> Tally;
        const size_t Ct = Col.Triangles.size() / 3;
        if (Ct != 0 && Col.TriangleLabel.size() == Ct && Col.MaterialSurface.size() == Col.Materials.size())
        {
            std::vector<RenderTriangle> Render;
            std::vector<std::string> Mats;
            std::map<std::string, int32_t> MatId;
            const int Winding = WindingOf(Scene.Game);
            for (const Mesh& M : Scene.Objects)
            {
                if (DrawnWorld::Owner(M.Name) != 0) continue;
                for (const Surface& S : M.Surfaces)
                {
                    if (S.Material.empty() || IsDecalMaterial(Scene, S.Material)) continue;
                    for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                    {
                        if (S.Indices[K] >= S.Vertices.size() || S.Indices[K + 1] >= S.Vertices.size() || S.Indices[K + 2] >= S.Vertices.size()) continue;
                        RenderTriangle T;
                        double P[3][3];
                        bool Finite = true;
                        for (int J = 0; J < 3; J++)
                            for (int A = 0; A < 3; A++)
                            {
                                const float F = S.Vertices[S.Indices[K + static_cast<size_t>(J)]].Pos[A];
                                Finite = Finite && std::isfinite(F);
                                T.Position[static_cast<size_t>(J) * 3 + static_cast<size_t>(A)] = F;
                                P[J][A] = F;
                            }
                        if (!Finite) continue;
                        const double E1[3] = { P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2] }, E2[3] = { P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2] };
                        const double C[3] = { E1[1] * E2[2] - E1[2] * E2[1], E1[2] * E2[0] - E1[0] * E2[2], E1[0] * E2[1] - E1[1] * E2[0] };
                        const double L = std::sqrt(C[0] * C[0] + C[1] * C[1] + C[2] * C[2]);
                        if (L < 1e-6) continue;
                        T.Area = L / 2;
                        T.Front = { C[0] * Winding / L, C[1] * Winding / L, C[2] * Winding / L };
                        const auto Id = MatId.emplace(S.Material, static_cast<int32_t>(Mats.size()));
                        if (Id.second) Mats.push_back(S.Material);
                        T.Material = Id.first->second;
                        Render.push_back(T);
                    }
                }
            }
            const CollisionTwins Twins = MatchTwins(Col.Vertices, Col.Triangles, Render);
            for (size_t C = 0; C < Ct; C++)
            {
                const int32_t Label = Col.TriangleLabel[C], Twin = Twins.Twin[C];
                if (Label < 0 || static_cast<size_t>(Label) >= Col.MaterialSurface.size() || Twin < 0) continue;
                const std::string& Name = Mats[static_cast<size_t>(Render[static_cast<size_t>(Twin)].Material)];
                const auto It = Scene.Materials.find(Name);
                if (It == Scene.Materials.end() || !Ambiguous(It->second)) continue;
                Tally[Name][(Col.MaterialSurface[static_cast<size_t>(Label)] >> Config.Shift) & Config.Mask]++;
                Counters["map_surface_collision_twinned_triangles"]++;
            }
        }
        for (auto& [Name, M] : Scene.Materials)
        {
            if (!Ambiguous(M)) continue;
            const auto It = Tally.find(Name);
            if (It == Tally.end())
            {
                Counters["map_surface_collision_no_twin"]++;   // no collision triangle sits on any triangle of this material
                continue;
            }
            size_t Total = 0;
            double Share = 0;
            const int Bit = WinningBit(It->second, static_cast<uint64_t>(M.SurfaceFlags), Config, Total, Share);
            if (Bit < 0)
            {
                Counters[Total < Config.MinTriangles ? "map_surface_collision_too_few" : "map_surface_collision_no_winner"]++;
                continue;
            }
            M.SurfaceVia = std::format("kind=collision, bit=0x{:X}, union=0x{:X}, share={:.3f}, twinned_triangles={}, min_share={:.2f}, min_triangles={}", uint64_t(1) << Bit,
                                       static_cast<uint64_t>(M.SurfaceFlags), Share, Total, Config.MinShare, Config.MinTriangles);
            M.SurfaceFlags = static_cast<int64_t>(uint64_t(1) << Bit);
            Counters["map_surface_collision_resolved"]++;
        }

        // 2. the same art in a plain material with one bit of its own (read, not inferred), for a layer only: a layer is drawn as a decal and has no collision of its own.
        // A material that had collision evidence and failed the winner test above stays ambiguous (the art may be shared, the collision is what the game stands on).
        std::map<std::string, std::set<uint64_t>> PlainBits;
        std::map<std::string, std::string> PlainName;
        for (const auto& [Name, M] : Scene.Materials)
        {
            if (M.ColorMap.empty() || !M.Layers.empty() || M.SurfaceFlags <= 0 || !M.SurfaceVia.empty() || Popcount(static_cast<uint64_t>(M.SurfaceFlags)) != 1) continue;
            PlainBits[M.ColorMap].insert(static_cast<uint64_t>(M.SurfaceFlags));
            PlainName.emplace(M.ColorMap, Name);
        }
        for (auto& [Name, M] : Scene.Materials)
        {
            if (!Ambiguous(M) || M.ColorMap.empty() || !M.IsLayer) continue;
            if (Tally.count(Name) != 0)
            {
                Counters["map_surface_same_art_skipped_collision_evidence"]++;
                continue;
            }
            const auto It = PlainBits.find(M.ColorMap);
            if (It == PlainBits.end()) continue;
            if (It->second.size() != 1 || (*It->second.begin() & static_cast<uint64_t>(M.SurfaceFlags)) != *It->second.begin())
            {
                Counters["map_surface_same_art_disagree"]++;
                continue;
            }
            M.SurfaceVia = std::format("kind=same_art, bit=0x{:X}, union=0x{:X}, plain_material={}", *It->second.begin(), static_cast<uint64_t>(M.SurfaceFlags), PlainName.at(M.ColorMap));
            M.SurfaceFlags = static_cast<int64_t>(*It->second.begin());
            Counters["map_surface_same_art_resolved"]++;
        }
        size_t Left = 0;
        for (const auto& [Name, M] : Scene.Materials) Left += Ambiguous(M);
        Counters["map_surface_still_ambiguous_materials"] += Left;
    }

    int SurfaceFromCollisionSelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("surface from collision: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        SurfaceFromCollisionConfig Config;
        Config.Enabled = true, Config.Shift = 20, Config.Mask = 0x1f, Config.Offset = -1, Config.MinShare = 0.9, Config.MinTriangles = 8;
        size_t Total = 0;
        double Share = 0;
        // concrete is bit 4 (index 5), plaster bit 16 (index 17)
        const uint64_t Word = (uint64_t(1) << 4) | (uint64_t(1) << 16);
        Check(WinningBit({ { 5, 95 }, { 17, 5 } }, Word, Config, Total, Share) == 4 && Total == 100 && std::abs(Share - 0.95) < 1e-9, "a clear winner is its bit, with the share", std::to_string(Share));
        Check(WinningBit({ { 5, 60 }, { 17, 40 } }, Word, Config, Total, Share) == -1, "a 60 to 40 split names nothing");
        Check(WinningBit({ { 5, 7 } }, Word, Config, Total, Share) == -1, "fewer triangles than the minimum name nothing", std::to_string(Total));
        Check(WinningBit({ { 9, 100 } }, Word, Config, Total, Share) == -1, "a clip surface that is not one of the material's bits names nothing");
        Check(WinningBit({ { 0, 100 } }, Word, Config, Total, Share) == -1, "index 0 (no surface) names nothing");
        Check(WinningBit({ { 5, 91 }, { 0, 9 } }, Word, Config, Total, Share) == 4, "triangles with no clip surface count in the total");
        Check(WinningBit({ { 5, 89 }, { 0, 11 } }, Word, Config, Total, Share) == -1, "the share is of all the twinned triangles, none-triangles included");

        // end to end: one blend, ten render triangles, ten collision triangles on them, partitions saying concrete
        MapScene Scene;
        Scene.Game = "mwr";
        Mesh World;
        Surface S;
        S.Material = "1n_2n";
        for (uint32_t T = 0; T < 10; T++)
        {
            const float X = static_cast<float>(T) * 10.0f;
            Vertex A, B, C;
            A.Pos[0] = X, A.Pos[1] = 0, A.Pos[2] = 0;
            B.Pos[0] = X + 8, B.Pos[1] = 0, B.Pos[2] = 0;
            C.Pos[0] = X, C.Pos[1] = 8, C.Pos[2] = 0;
            S.Vertices.push_back(A), S.Vertices.push_back(B), S.Vertices.push_back(C);
            S.Indices.push_back(T * 3), S.Indices.push_back(T * 3 + 1), S.Indices.push_back(T * 3 + 2);
        }
        World.Surfaces.push_back(S);
        Scene.Objects.push_back(World);
        Material Blend;
        Blend.Name = "1n_2n", Blend.Layers.push_back({ "x", false, -1.0f });
        Blend.ColorMap = "art_blend";
        Blend.SurfaceFlags = static_cast<int64_t>(Word);
        Scene.Materials["1n_2n"] = Blend;
        Scene.Collision.Materials = { { "a", 0 }, { "b", 0 } };
        Scene.Collision.MaterialSurface = { 5u << 20, 17u << 20 };
        for (const Vertex& V : S.Vertices)
            for (int A = 0; A < 3; A++) Scene.Collision.Vertices.push_back(V.Pos[A]);
        Scene.Collision.Triangles = S.Indices;
        Scene.Collision.TriangleLabel.assign(10, 0);
        std::map<std::string, size_t> Counters;
        ResolveSurfaceFromCollision(Scene, Config, Counters);
        const Material& After = Scene.Materials.at("1n_2n");
        Check(After.SurfaceFlags == (int64_t(1) << 4) && After.SurfaceVia.rfind("kind=collision", 0) == 0 && After.SurfaceVia.find("share=1.000") != std::string::npos,
              "ten twinned triangles that say concrete make the blend concrete, with evidence", After.SurfaceVia);
        // the same, with a 6 to 4 split of labels: stays ambiguous
        Scene.Materials["1n_2n"] = Blend;
        for (size_t T = 0; T < 4; T++) Scene.Collision.TriangleLabel[T] = 1;
        Counters.clear();
        ResolveSurfaceFromCollision(Scene, Config, Counters);
        Check(Scene.Materials.at("1n_2n").SurfaceFlags == static_cast<int64_t>(Word) && Scene.Materials.at("1n_2n").SurfaceVia.empty() && Counters["map_surface_collision_no_winner"] == 1,
              "a split twin set leaves the word ambiguous and counts it");
        // same art: a layer material with the colour image of a plain, one-bit material
        Scene.Materials["1n_2n"] = Blend;
        Scene.Collision = Collision();
        Material Layer, Plain, Other;
        Layer.Name = "layer_dec", Layer.ColorMap = "art_col", Layer.SurfaceFlags = static_cast<int64_t>(Word), Layer.IsLayer = true;
        Plain.Name = "plain", Plain.ColorMap = "art_col", Plain.SurfaceFlags = int64_t(1) << 16;
        Other.Name = "other", Other.ColorMap = "art_col", Other.SurfaceFlags = int64_t(1) << 4;
        Scene.Materials["layer_dec"] = Layer, Scene.Materials["plain"] = Plain;
        Counters.clear();
        ResolveSurfaceFromCollision(Scene, Config, Counters);
        Check(Scene.Materials.at("layer_dec").SurfaceFlags == (int64_t(1) << 16) && Scene.Materials.at("layer_dec").SurfaceVia.rfind("kind=same_art", 0) == 0, "a layer takes the one bit of the same art in a plain material",
              Scene.Materials.at("layer_dec").SurfaceVia);
        Scene.Materials["layer_dec"] = Layer, Scene.Materials["other"] = Other;
        Counters.clear();
        ResolveSurfaceFromCollision(Scene, Config, Counters);
        Check(Scene.Materials.at("layer_dec").SurfaceFlags == static_cast<int64_t>(Word) && Counters["map_surface_same_art_disagree"] == 1, "plain materials of one art that disagree name nothing");
        // a plain material of the same art whose bit is none of the layer's own: the layer keeps its word
        Scene.Materials.erase("other");
        Scene.Materials["plain"].SurfaceFlags = int64_t(1) << 9;
        Scene.Materials["layer_dec"] = Layer;
        Counters.clear();
        ResolveSurfaceFromCollision(Scene, Config, Counters);
        Check(Scene.Materials.at("layer_dec").SurfaceFlags == static_cast<int64_t>(Word), "a plain material's bit that is not one of the layer's names nothing");
        // a material that is no layer (a blend with no twin) is not given a plain material's bit, and a layer that had twins which named nothing keeps its word
        {
            Scene.Materials.erase("plain");
            Scene.Materials["plain"].Name = "plain";
            Scene.Materials["plain"].ColorMap = "art_col";
            Scene.Materials["plain"].SurfaceFlags = int64_t(1) << 16;
            Material NotLayer = Layer;
            NotLayer.Name = "not_layer", NotLayer.IsLayer = false;
            Scene.Materials["not_layer"] = NotLayer;
            Scene.Materials["layer_dec"] = Layer;
            Counters.clear();
            ResolveSurfaceFromCollision(Scene, Config, Counters);
            Check(Scene.Materials.at("not_layer").SurfaceFlags == static_cast<int64_t>(Word) && Scene.Materials.at("layer_dec").SurfaceFlags == (int64_t(1) << 16), "the same-art rule is for layers only");
            Scene.Materials.erase("not_layer");
            // collision evidence that named no winner: the same art does not settle it
            Material Evidence = Blend;
            Evidence.IsLayer = true;
            Evidence.ColorMap = "art_col";
            Scene.Materials["1n_2n"] = Evidence;
            Scene.Collision = Collision();
            Scene.Collision.Materials = { { "a", 0 }, { "b", 0 } };
            Scene.Collision.MaterialSurface = { 5u << 20, 17u << 20 };
            for (const Vertex& V : S.Vertices)
                for (int A = 0; A < 3; A++) Scene.Collision.Vertices.push_back(V.Pos[A]);
            Scene.Collision.Triangles = S.Indices;
            Scene.Collision.TriangleLabel.assign(10, 0);
            for (size_t T = 0; T < 4; T++) Scene.Collision.TriangleLabel[T] = 1;
            Counters.clear();
            ResolveSurfaceFromCollision(Scene, Config, Counters);
            Check(Scene.Materials.at("1n_2n").SurfaceFlags == static_cast<int64_t>(Word) && Counters["map_surface_same_art_skipped_collision_evidence"] == 1, "a layer with collision evidence that named no winner stays ambiguous");
            Scene.Materials["1n_2n"] = Blend;
        }
        Config.Enabled = false;
        Scene.Materials["layer_dec"] = Layer;
        ResolveSurfaceFromCollision(Scene, Config, Counters);
        Check(Scene.Materials.at("layer_dec").SurfaceFlags == static_cast<int64_t>(Word), "a game without the rule is left alone");
        return Failed;
    }
}
