#include "stdafx.h"

#include "MapCollisionMeshes.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace Maps
{
namespace
{
    // C# Math.Round of a float x 8: ties to even (std::nearbyint in the default rounding mode), 21 bits an axis
    uint64_t Pack(std::span<const float> P)
    {
        const auto Q = [](float V) { return static_cast<uint64_t>(static_cast<int64_t>(std::nearbyint(static_cast<double>(V * 8.0f)))) & 0x1FFFFFull; };
        return (Q(P[0]) << 42) | (Q(P[1]) << 21) | Q(P[2]);
    }

    // The 21 bit key wraps every 262,144 units; two triangles with the same key are twins only when their corners are really within the 1/8 grid of each other
    bool SameCorners(std::span<const float> Collision, const std::array<uint32_t, 3>& I, const RenderTriangle& R)
    {
        for (int K = 0; K < 3; K++)
        {
            bool Found = false;
            for (size_t J = 0; J < 3 && !Found; J++)
            {
                bool Near = true;
                for (size_t A = 0; A < 3; A++) Near = Near && std::fabs(static_cast<double>(Collision[static_cast<size_t>(I[K]) * 3 + A]) - R.Position[J * 3 + A]) <= 0.125;
                Found = Near;
            }
            if (!Found) return false;
        }
        return true;
    }
}

std::array<uint64_t, 3> TwinKey(std::span<const float> A, std::span<const float> B, std::span<const float> C)
{
    std::array<uint64_t, 3> K = { Pack(A), Pack(B), Pack(C) };
    std::sort(K.begin(), K.end());
    return K;
}

CollisionTwins MatchTwins(std::span<const float> CollisionPositions, std::span<const uint32_t> Triangles, const std::vector<RenderTriangle>& Render)
{
    CollisionTwins R;
    const size_t Ct = Triangles.size() / 3;
    R.Twin.assign(Ct, -1);
    // flat sorted keys, not a map of tuples: a map has a million triangles
    struct Entry
    {
        std::array<uint64_t, 3> Key;
        uint32_t Index;
        bool operator<(const Entry& O) const { return Key != O.Key ? Key < O.Key : Index < O.Index; }
    };
    std::vector<Entry> Keys;
    Keys.reserve(Render.size());
    for (size_t T = 0; T < Render.size(); T++)
    {
        const std::span<const float> Pos(Render[T].Position);
        Keys.push_back({ TwinKey(Pos.subspan(0, 3), Pos.subspan(3, 3), Pos.subspan(6, 3)), static_cast<uint32_t>(T) });
    }
    std::sort(Keys.begin(), Keys.end());
    // the collision triangles in the order of their keys (then their numbers), so the choice among shared candidates does not depend on how they were listed
    std::vector<Entry> Order;
    Order.reserve(Ct);
    for (size_t C = 0; C < Ct; C++)
    {
        const std::array<uint32_t, 3> I = { Triangles[C * 3], Triangles[C * 3 + 1], Triangles[C * 3 + 2] };
        if (static_cast<size_t>(I[0]) * 3 + 2 >= CollisionPositions.size() || static_cast<size_t>(I[1]) * 3 + 2 >= CollisionPositions.size() || static_cast<size_t>(I[2]) * 3 + 2 >= CollisionPositions.size()) continue;
        Order.push_back({ TwinKey(CollisionPositions.subspan(I[0] * 3, 3), CollisionPositions.subspan(I[1] * 3, 3), CollisionPositions.subspan(I[2] * 3, 3)), static_cast<uint32_t>(C) });
    }
    std::sort(Order.begin(), Order.end());
    std::vector<char> Used(Render.size(), 0);
    for (const Entry& Collision : Order)
    {
        const size_t C = Collision.Index;
        const std::array<uint32_t, 3> I = { Triangles[C * 3], Triangles[C * 3 + 1], Triangles[C * 3 + 2] };
        // the collision triangle's own facing: out where (b - a) x (c - a) points
        std::array<double, 3> Face{ 0, 0, 0 };
        {
            std::array<double, 3> A{}, B{};
            for (size_t K = 0; K < 3; K++)
            {
                A[K] = static_cast<double>(CollisionPositions[static_cast<size_t>(I[1]) * 3 + K]) - CollisionPositions[static_cast<size_t>(I[0]) * 3 + K];
                B[K] = static_cast<double>(CollisionPositions[static_cast<size_t>(I[2]) * 3 + K]) - CollisionPositions[static_cast<size_t>(I[0]) * 3 + K];
            }
            Face = { (A[1] * B[2]) - (A[2] * B[1]), (A[2] * B[0]) - (A[0] * B[2]), (A[0] * B[1]) - (A[1] * B[0]) };
        }
        int32_t Best = -1;
        bool BestAgrees = false;
        size_t Surfaces = 0, Decals = 0, ByBrush = 0, Free = 0;
        for (auto It = std::lower_bound(Keys.begin(), Keys.end(), Entry{ Collision.Key, 0 }); It != Keys.end() && It->Key == Collision.Key; ++It)
        {
            const RenderTriangle& T = Render[It->Index];
            if (!SameCorners(CollisionPositions, I, T)) continue;
            if (T.Decal)
            {
                Decals++;
                continue;
            }
            if (T.DrawnByBrush)
            {
                ByBrush++;
                continue;
            }
            Surfaces++;
            if (Used[It->Index]) continue;
            Free++;
            const bool Agrees = (Face[0] * T.Front[0]) + (Face[1] * T.Front[1]) + (Face[2] * T.Front[2]) > 0;
            const bool Better = Best < 0 || (Agrees && !BestAgrees) || (Agrees == BestAgrees && T.Area > Render[static_cast<size_t>(Best)].Area);
            if (Better) Best = static_cast<int32_t>(It->Index), BestAgrees = Agrees;
        }
        if (Best < 0)
        {
            if (Surfaces > 0 && Free == 0) R.Exhausted++;
            else if (ByBrush > 0 && Surfaces == 0) R.DrawnByBrushOnly++;
            else if (Decals > 0 && Surfaces == 0) R.DecalOnly++;
            continue;
        }
        R.Twin[C] = Best;
        Used[static_cast<size_t>(Best)] = 1;
        R.Twinned++;
        R.Ambiguous += Surfaces > 1 ? 1 : 0;
        R.DecalBeside += Decals > 0 ? 1 : 0;
    }
    return R;
}

CollisionSoupResult CollisionSoup(std::span<const float> CollisionPositions, std::span<const uint32_t> Triangles, std::span<const int32_t> Labels, const std::vector<RenderTriangle>& Render,
                                  const CollisionTwins& Twins)
{
    CollisionSoupResult Out;
    RenderSoup& S = Out.Soup;
    const size_t Ct = Triangles.size() / 3;
    S.Positions.reserve(Ct * 9);
    S.Corners.reserve(Ct * 3);
    S.Indices.reserve(Ct * 3);
    std::map<std::pair<int32_t, int32_t>, int32_t> Dense;   // (collision label, twin material + 1) -> soup label
    for (size_t C = 0; C < Ct; C++)
    {
        const int32_t Twin = C < Twins.Twin.size() ? Twins.Twin[C] : -1;
        const RenderTriangle* T = Twin >= 0 ? &Render[static_cast<size_t>(Twin)] : nullptr;
        std::array<uint64_t, 3> Packed{};
        if (T != nullptr)
            for (size_t K = 0; K < 3; K++) Packed[K] = Pack(std::span<const float>(T->Position).subspan(K * 3, 3));
        for (int K = 0; K < 3; K++)
        {
            const std::span<const float> P = CollisionPositions.subspan(static_cast<size_t>(Triangles[C * 3 + static_cast<size_t>(K)]) * 3, 3);
            S.Positions.insert(S.Positions.end(), { P[0], P[1], P[2] });
            RenderCorner Corner;
            Corner.HasData = false;
            if (T != nullptr)
                for (int J = 0; J < 3; J++)
                    if (Packed[static_cast<size_t>(J)] == Pack(P))
                    {
                        Corner = T->Corner[J];
                        Corner.HasData = true;
                        break;
                    }
            S.Corners.push_back(Corner);
            S.Indices.push_back(static_cast<int32_t>(C * 3 + static_cast<size_t>(K)));
        }
        // Different twin materials never pair, and a twinned triangle never pairs with an untwinned one: one visible twin cannot turn untwinned triangles into a visible mesh
        const std::pair<int32_t, int32_t> What = { !Labels.empty() ? (C < Labels.size() ? Labels[C] : -1) : 0, T != nullptr ? T->Material + 1 : 0 };
        const auto Found = Dense.emplace(What, static_cast<int32_t>(Dense.size()));
        if (Found.second) Out.LabelOf.push_back({ What.first, What.second - 1 });
        S.Label.push_back(Found.first->second);
        S.Front.push_back(T != nullptr ? T->Front : std::array<double, 3>{ 0, 0, 0 });   // no twin: unknown, the collision winding stands (counted)
        S.Normal.push_back(T != nullptr ? T->Normal : std::array<double, 3>{ 0, 0, 0 });
        S.TwinMaterial.push_back(T != nullptr ? T->Material : -1);
    }
    return Out;
}

RenderTaken TakenFromRenderPass(const CollisionTwins& Twins, const std::vector<RenderTriangle>& Render, const RenderMeshResult& Meshes)
{
    RenderTaken R;
    R.Taken.assign(Render.size(), 0);
    for (const RenderMesh& M : Meshes.Meshes)
    {
        if (M.Material < 0 || !M.Mesh.HasData()) continue;   // a mesh with no material or no attributes draws nothing of the surface
        for (const int32_t C : M.Triangles)
        {
            if (C < 0 || static_cast<size_t>(C) >= Twins.Twin.size()) continue;
            const int32_t T = Twins.Twin[static_cast<size_t>(C)];
            if (T >= 0 && Render[static_cast<size_t>(T)].Material == M.Material) R.Taken[static_cast<size_t>(T)] = 1;
        }
    }
    std::vector<char> Twin(Render.size(), 0);
    for (const int32_t T : Twins.Twin)
        if (T >= 0) Twin[static_cast<size_t>(T)] = 1;
    for (size_t T = 0; T < Render.size(); T++)
    {
        R.Count += R.Taken[T] ? 1 : 0;
        R.Stayed += (Twin[T] && !R.Taken[T]) ? 1 : 0;
    }
    return R;
}
}
