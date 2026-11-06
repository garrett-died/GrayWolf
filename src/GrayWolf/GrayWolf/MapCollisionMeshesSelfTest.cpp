#include "stdafx.h"

#include "MapCollisionMeshes.h"
#include "MapCollision.h"

#include <cmath>
#include <format>
#include <span>
#include <map>
#include <set>
#include <string>
#include <vector>

// Self-test of the collision meshes (M2c): the twin choice, the data a mesh point may take (r46), the partition of the world's triangles, a two-sided sheet, and that the
// thread count changes nothing.
namespace Maps
{
namespace
{
    RenderTriangle Tri(const std::array<float, 9>& P, int32_t Material, bool Decal = false, bool Brush = false, float U = 0)
    {
        RenderTriangle T;
        T.Position = P;
        T.Material = Material;
        T.Decal = Decal;
        T.DrawnByBrush = Brush;
        T.Front = { 0, 0, 1 };
        T.Normal = { 0, 0, 1 };
        for (int K = 0; K < 3; K++) T.Corner[K].U = U + P[static_cast<size_t>(K) * 3] / 16, T.Corner[K].V = P[static_cast<size_t>(K) * 3 + 1] / 16;   // data by position, so neighbours agree
        const double X = (static_cast<double>(P[3]) - P[0]) * (P[7] - P[1]) - (static_cast<double>(P[4]) - P[1]) * (P[6] - P[0]);
        T.Area = std::fabs(X) / 2;
        return T;
    }

    std::string AllText(const RenderMeshResult& R)
    {
        std::string Out;
        for (const RenderMesh& M : R.Meshes) Out += MeshText(M.Mesh, "m", M.ReplaceUv);
        return Out;
    }
}

int CollisionMeshesSelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* Name, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::format("FAIL CollisionMeshes {} (got '{}')\n", Name, Got);
    };
    // one collision triangle and the corners of a quad: a b c d on z = 0
    const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0 };
    const std::array<float, 9> Abc = { 0, 0, 0, 8, 0, 0, 8, 8, 0 }, Acd = { 0, 0, 0, 8, 8, 0, 0, 8, 0 };

    RenderMeshOptions Col;
    Col.Collision = true;
    Col.Threads = 1;
    const auto Build = [&](const CollisionSoupResult& S) { return BuildRenderMeshes(S.Soup, std::vector<std::string>(S.LabelOf.size(), "m"), Col); };

    // ---- the twin: a decal never explains a collision triangle; among surfaces the one whose front agrees, then the larger area, then the lowest index
    {
        std::array<float, 9> Shifted = Abc;
        Shifted[7] += 0.03f;   // the same 1/8 unit corners, a larger area
        const std::vector<RenderTriangle> R = { Tri(Abc, 1, true), Tri(Abc, 2), Tri(Shifted, 3), Tri(Acd, 4, true) };
        const std::vector<uint32_t> T = { 0, 1, 2, 0, 2, 3 };
        const CollisionTwins W = MatchTwins(P, T, R);
        Check(W.Twin[0] == 2 && W.Ambiguous == 1 && W.DecalBeside == 1, "a decal over the same corners does not take the twin: the larger surface does, and it is counted", std::to_string(W.Twin[0]));
        Check(W.Twin[1] == -1 && W.DecalOnly == 1, "a collision triangle with only a decal on its corners has no twin and is counted", std::to_string(W.Twin[1]));
    }

    // ---- a tie: equal fronts, equal areas: the lowest index
    {
        const std::vector<RenderTriangle> R = { Tri(Abc, 1), Tri(Abc, 2) };
        const CollisionTwins W = MatchTwins(P, std::vector<uint32_t>{ 0, 1, 2 }, R);
        Check(W.Twin[0] == 0 && W.Ambiguous == 1, "a tie between two surfaces goes to the lowest index and is counted", std::to_string(W.Twin[0]));
    }

    // ---- one render triangle explains one collision triangle at most; a two-sided sheet takes a different twin for each side (the one whose front agrees)
    {
        RenderTriangle Up = Tri(Abc, 1), Down = Tri(Abc, 2);
        Up.Front = { 0, 0, 1 };
        Down.Front = { 0, 0, -1 };
        const std::vector<RenderTriangle> R = { Down, Up };   // the downward twin is listed first
        const std::vector<uint32_t> T = { 0, 1, 2, 0, 2, 1 };   // up (counter-clockwise), then down
        const CollisionTwins W = MatchTwins(P, T, R);
        Check(W.Twin[0] == 1 && W.Twin[1] == 0 && W.Twinned == 2, "the two sides of a sheet take the twin whose front agrees, one each", std::to_string(W.Twin[0]) + " " + std::to_string(W.Twin[1]));
        // two collision triangles on one render triangle: the second has none
        const std::vector<RenderTriangle> One = { Tri(Abc, 1) };
        const CollisionTwins W2 = MatchTwins(P, std::vector<uint32_t>{ 0, 1, 2, 0, 1, 2 }, One);
        Check(W2.Twinned == 1 && W2.Exhausted == 1, "one render triangle explains one collision triangle at most", std::to_string(W2.Twinned) + " " + std::to_string(W2.Exhausted));
    }

    // ---- a twin a brush side draws already gives no material: its collision triangle is a tool mesh, nothing is drawn twice
    {
        const std::vector<RenderTriangle> R = { Tri(Abc, 1, false, true) };
        const CollisionTwins W = MatchTwins(P, std::vector<uint32_t>{ 0, 1, 2 }, R);
        Check(W.Twin[0] == -1 && W.DrawnByBrushOnly == 1, "a render triangle a brush side draws gives no twin", std::to_string(W.Twin[0]));
    }

    // ---- the key wraps every 262,144 units: a triangle translated by that is not a twin
    {
        std::array<float, 9> Far = Abc;
        for (size_t K = 0; K < 3; K++) Far[K * 3] += 262144.0f;
        const std::vector<RenderTriangle> R = { Tri(Far, 1) };
        Check(TwinKey(std::span<const float>(Abc).subspan(0, 3), std::span<const float>(Abc).subspan(3, 3), std::span<const float>(Abc).subspan(6, 3)) ==
                  TwinKey(std::span<const float>(Far).subspan(0, 3), std::span<const float>(Far).subspan(3, 3), std::span<const float>(Far).subspan(6, 3)),
              "the translated triangle has the same 21 bit key (the premise)");
        const CollisionTwins W = MatchTwins(P, std::vector<uint32_t>{ 0, 1, 2 }, R);
        Check(W.Twin[0] == -1, "a triangle 262,144 units away is not a twin although its key is equal", std::to_string(W.Twin[0]));
        // ties go to even, as C# Math.Round: 0.0625 x 8 = 0.5 -> 0, 0.1875 x 8 = 1.5 -> 2
        const std::array<float, 3> Half = { 0.0625f, 0.1875f, 0.3125f }, Even = { 0.0f, 0.25f, 0.25f };
        Check(TwinKey(Half, Half, Half) == TwinKey(Even, Even, Even), "ties round to even like C# Math.Round (0.5 -> 0, 1.5 -> 2, 2.5 -> 2)");
    }

    // ---- different twin materials, and twinned with untwinned, never share a mesh; a surface is drawn by its own material only (B: both surfaces stay in the render pass if no mesh took them)
    {
        const std::vector<RenderTriangle> R = { Tri(Abc, 1), Tri(Acd, 2) };
        const std::vector<uint32_t> T = { 0, 1, 2, 0, 2, 3 };
        const std::vector<int32_t> Labels = { 5, 5 };   // one clip label for both
        const CollisionTwins W = MatchTwins(P, T, R);
        const CollisionSoupResult S = CollisionSoup(P, T, Labels, R, W);
        const RenderMeshResult M = Build(S);
        Check(M.Meshes.size() == 2 && M.Meshes[0].Material != M.Meshes[1].Material && M.Meshes[0].Mesh.HasData() && M.Meshes[1].Mesh.HasData() && M.Count.CoverageMissing == 0 && M.Count.CoverageLeftover == 0,
              "two twins of different materials make two meshes, each with attributes (r46)", std::to_string(M.Meshes.size()));
        const RenderTaken Taken = TakenFromRenderPass(W, R, M);
        Check(Taken.Count == 2 && Taken.Stayed == 0, "both surfaces left the render pass: each is in a mesh of its own material", std::to_string(Taken.Count));
        // twinned and untwinned never pair
        const std::vector<RenderTriangle> Half = { Tri(Abc, 1) };
        const CollisionTwins W4 = MatchTwins(P, T, Half);
        const RenderMeshResult M4 = Build(CollisionSoup(P, T, Labels, Half, W4));
        Check(M4.Meshes.size() == 2, "a triangle with a twin and one without are never one mesh", std::to_string(M4.Meshes.size()));
        // no twin at all: no material, no attributes, facing unknown, colliding
        const RenderMeshResult M3 = Build(CollisionSoup(P, T, Labels, {}, MatchTwins(P, T, {})));
        Check(M3.Meshes.size() == 1 && M3.Meshes[0].Material == -1 && M3.Count.MeshesNoMaterial == 1 && M3.Count.FacingUnknown == 1 && !M3.Meshes[0].Mesh.HasData(), "a mesh with no twin has no material, no attributes and an unknown facing", std::to_string(M3.Meshes.size()));
        Check(!M3.Meshes[0].Mesh.NonColliding, "a collision mesh collides");
    }

    // ---- B: a render triangle whose collision triangle was removed (contained, copy) or has no mesh with attributes stays in the render pass
    {
        const std::vector<RenderTriangle> R = { Tri(Abc, 1) };
        CollisionTwins W;
        W.Twin = { 0 };
        W.Twinned = 1;
        RenderMeshResult None;   // no mesh covers the collision triangle (it was removed)
        const RenderTaken Removed = TakenFromRenderPass(W, R, None);
        Check(Removed.Count == 0 && Removed.Stayed == 1, "a surface whose collision triangle is in no mesh stays in the render pass", std::to_string(Removed.Count) + " " + std::to_string(Removed.Stayed));
        RenderMesh Bare;   // a mesh of the material without attributes
        Bare.Material = 1;
        Bare.Triangles = { 0 };
        RenderMeshResult NoData;
        NoData.Meshes.push_back(Bare);
        const RenderTaken NoAttr = TakenFromRenderPass(W, R, NoData);
        Check(NoAttr.Count == 0 && NoAttr.Stayed == 1, "a surface whose collision mesh has no attributes stays in the render pass", std::to_string(NoAttr.Count));
    }

    // ---- C: a twinned triangle is never removed by containment (a triangle with no twin is)
    {
        const std::vector<float> Pos = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 2, 0.5f, 0, 4, 0.5f, 0, 4, 2, 0 };
        const std::vector<uint32_t> T = { 0, 1, 2, 3, 4, 5 };
        // the small triangle has a twin of the same material as the big one: same label, contained, still kept
        const std::array<float, 9> Big = { 0, 0, 0, 8, 0, 0, 8, 8, 0 }, Small = { 2, 0.5f, 0, 4, 0.5f, 0, 4, 2, 0 };
        const std::vector<RenderTriangle> R = { Tri(Big, 1), Tri(Small, 1) };
        const RenderMeshResult M = Build(CollisionSoup(Pos, T, std::vector<int32_t>{ 4, 4 }, R, MatchTwins(Pos, T, R)));
        Check(M.Count.ContainedRemoved == 0 && M.Count.Covered == 2, "a twinned triangle inside a larger one is not removed by containment", std::to_string(M.Count.ContainedRemoved));
    }


    // ---- a two-sided sheet: both triangles stay
    {
        const std::vector<uint32_t> T = { 0, 1, 2, 0, 2, 1 };
        const CollisionSoupResult S = CollisionSoup(P, T, std::vector<int32_t>{ 7, 7 }, {}, MatchTwins(P, T, {}));
        RenderMeshOptions O;
        O.Collision = true;
        O.Threads = 1;
        const RenderMeshResult M = BuildRenderMeshes(S.Soup, std::vector<std::string>(S.LabelOf.size(), "m"), O);
        Check(M.Count.TrianglesIn == 2 && M.Count.Covered == 2 && M.Count.ContainedRemoved == 0 && M.Count.CopiesRemoved == 0 && M.Count.CoverageMissing == 0 && M.Count.CoverageLeftover == 0,
              "a sheet colliding on both sides keeps both triangles", std::to_string(M.Count.Covered) + " " + std::to_string(M.Count.ContainedRemoved));
    }

    // ---- a triangle inside a larger one of the same label and facing is removed with its proof, counted apart from the copies
    {
        const std::vector<float> Pos = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 2, 0.5f, 0, 4, 0.5f, 0, 4, 2, 0 };
        const std::vector<uint32_t> T = { 0, 1, 2, 3, 4, 5 };
        const CollisionSoupResult S = CollisionSoup(Pos, T, std::vector<int32_t>{ 4, 4 }, {}, MatchTwins(Pos, T, {}));
        RenderMeshOptions O;
        O.Collision = true;
        O.Threads = 1;
        const RenderMeshResult M = BuildRenderMeshes(S.Soup, std::vector<std::string>(S.LabelOf.size(), "m"), O);
        Check(M.Count.TrianglesIn == 2 && M.Count.ContainedRemoved == 1 && M.Count.CopiesRemoved == 0 && M.Count.Covered == 1 && M.Count.CoverageMissing == 0 && M.Count.CoverageLeftover == 0,
              "a collision triangle inside a larger one of its label is removed as contained, with a proof", std::to_string(M.Count.ContainedRemoved) + " " + std::to_string(M.Count.Covered));
        // another label: it stays
        const CollisionSoupResult S2 = CollisionSoup(Pos, T, std::vector<int32_t>{ 4, 5 }, {}, MatchTwins(Pos, T, {}));
        const RenderMeshResult M2 = BuildRenderMeshes(S2.Soup, std::vector<std::string>(S2.LabelOf.size(), "m"), O);
        Check(M2.Count.ContainedRemoved == 0 && M2.Count.Covered == 2, "a triangle inside one of another label stays", std::to_string(M2.Count.ContainedRemoved));
    }

    // ---- the stock layout of a collision mesh: toolFlags in stock position, no contents line, W H 0 8
    {
        MapMesh M;
        M.GroupCount = 2;
        M.GroupSize = 2;
        M.Points = { { 0, 0, 0 }, { 0, 4, 0 }, { 8, 0, 0 }, { 8, 4, 0 } };
        MeshTextOptions Colliding;
        Colliding.NonColliding = false;
        const std::string Text = MeshText(M, "clip_full", false, Colliding);
        Check(Text.rfind("  mesh\n  {\n  toolFlags;\n   clip_full\n   lightmap_gray\n   2 2 0 8\n   (\n", 0) == 0 && Text.find("contents") == std::string::npos, "a collision mesh is written in the stock layout: toolFlags, no contents line, W H 0 8", Text);
    }

    // ---- the AABB tree's leaf walk on a built buffer: first leaf wins, a repeat with the same material is fine, a bad material is counted and not used
    {
        std::vector<uint8_t> Buf(1024, 0);
        const auto Put16 = [&](size_t At, uint16_t V) { Buf[At] = static_cast<uint8_t>(V & 0xff), Buf[At + 1] = static_cast<uint8_t>(V >> 8); };
        const auto Put32 = [&](size_t At, uint32_t V) { Put16(At, static_cast<uint16_t>(V & 0xffff)), Put16(At + 2, static_cast<uint16_t>(V >> 16)); };
        // partitions at 0 (16 bytes: triangle count u8 at 0): 4, 5 and 6 triangles
        Buf[0] = 4, Buf[16] = 5, Buf[32] = 6;
        // tree at 256, 32-byte records: 0 internal (2 children), 1 leaf partition 0 material 7, 2 leaf partition 1 material 3, 3 the same again, 4 leaf partition 2 material 99
        const auto Node = [&](size_t K, uint16_t Material, uint16_t Children, uint32_t Partition) {
            Put16(256 + (K * 32) + 12, Material);
            Put16(256 + (K * 32) + 14, Children);
            Put32(256 + (K * 32) + 28, Partition);
        };
        Node(0, 0, 2, 0);
        Node(1, 7, 0, 0);
        Node(2, 3, 0, 1);
        Node(3, 3, 0, 1);
        Node(4, 99, 0, 2);
        ClipLayout L;
        L.PartitionSize = 16, L.PartitionTris = 0, L.TreeSize = 32, L.TreeMaterial = 12, L.TreeChildren = 14, L.TreePartition = 28;
        std::map<std::string, size_t> Counters;
        const TreeWalk W = WalkCollisionTree(Buf, L, 0, 3, 256, 5, 10, Counters);
        Check(W.Material == std::vector<int32_t>({ 7, 3, -1 }) && W.LeafTriangles == 15 && W.Node[0] == 256 + 32 && Counters["collision_tree_leaves"] == 4 && Counters["collision_tree_partition_reached_twice"] == 1 &&
                  Counters["collision_tree_partition_material_conflict"] == 0 && Counters["collision_tree_material_out_of_range"] == 1 && Counters["collision_tree_partition_unreached"] == 0,
              "the tree's leaf walk: the first leaf names the partition's material, repeats are counted, a material out of range is counted and unread", std::to_string(W.LeafTriangles));
    }

    // ---- a grid of collision triangles, two labels: one thread and four give the same meshes
    {
        std::vector<float> Pos;
        std::vector<uint32_t> T;
        std::vector<int32_t> Labels;
        const int N = 12;
        for (int J = 0; J <= N; J++)
            for (int I = 0; I <= N; I++) Pos.insert(Pos.end(), { I * 8.0f, J * 8.0f, 0.0f });
        for (int J = 0; J < N; J++)
            for (int I = 0; I < N; I++)
            {
                const uint32_t A = static_cast<uint32_t>(J * (N + 1) + I), B = A + 1, C = A + static_cast<uint32_t>(N) + 2, D = A + static_cast<uint32_t>(N) + 1;
                T.insert(T.end(), { A, B, C, A, C, D });
                Labels.push_back(I < N / 2 ? 1 : 2);
                Labels.push_back(I < N / 2 ? 1 : 2);
            }
        const CollisionSoupResult S = CollisionSoup(Pos, T, Labels, {}, MatchTwins(Pos, T, {}));
        RenderMeshOptions One, Four;
        One.Collision = Four.Collision = true;
        One.Threads = 1;
        Four.Threads = 4;
        const std::vector<std::string> Names(S.LabelOf.size(), "m");
        const RenderMeshResult A = BuildRenderMeshes(S.Soup, Names, One), B = BuildRenderMeshes(S.Soup, Names, Four);
        Check(AllText(A) == AllText(B) && A.Count.Covered == T.size() / 3 && A.Count.CoverageMissing == 0 && A.Count.CoverageLeftover == 0 && A.Meshes.size() >= 2, "one thread and four give the same collision meshes, every triangle covered",
              std::to_string(A.Meshes.size()) + " " + std::to_string(A.Count.Covered));
    }
    return Failed;
}
}
