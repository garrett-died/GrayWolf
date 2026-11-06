#include "stdafx.h"

#include "IwBsp.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace IW12
{
namespace
{
    // the lumps a map export reads, by version
    struct Layout
    {
        size_t Lumps, Materials, Soups, Vertices, Indices, Entities, VertexSize;
        size_t Planes, BrushSides, Brushes, Models;
        // CoD1: collision surfaces, their vertices, indices, the models' runs of them; CoD2:
        // collision vertices, triangles, partitions, AABB tree (SIZE_MAX: none)
        size_t Patches, CollisionVertices, CollisionIndices, PatchRefs;
        size_t Triangles, Partitions, AabbTree;
    };
    constexpr size_t None = SIZE_MAX;
    const Layout V59{ 33, 0, 6, 7, 8, 29, 44, 2, 3, 4, 27, 24, 25, 26, 23, None, None, None };   // CoD1
    const Layout V4{ 39, 0, 7, 8, 9, 37, 68, 4, 5, 6, 35, None, 29, None, None, 31, 33, 34 };    // CoD2

    template <class T> T At(const uint8_t* P)
    {
        T V;
        std::memcpy(&V, P, sizeof(T));
        return V;
    }
}

bool ParseBsp(const uint8_t* Data, size_t Size, BspFile& Out, std::string& Error)
{
    Out = BspFile();
    if (Size < 8 || std::memcmp(Data, "IBSP", 4) != 0)
    {
        Error = "not an IBSP";
        return false;
    }
    Out.Version = At<int32_t>(Data + 4);
    const Layout* L = Out.Version == 59 ? &V59 : Out.Version == 4 ? &V4 : nullptr;
    if (!L)
    {
        Error = "version " + std::to_string(Out.Version);
        return false;
    }
    Out.LumpCount = L->Lumps;
    const size_t TableEnd = 8 + 8 * L->Lumps;
    if (Size < TableEnd)
    {
        Error = "lump table past the end";
        return false;
    }
    std::vector<std::pair<uint32_t, uint32_t>> Lumps(L->Lumps);   // length, offset
    std::vector<std::tuple<uint32_t, uint32_t, size_t>> ByOffset;
    for (size_t I = 0; I < L->Lumps; I++)
    {
        Lumps[I] = { At<uint32_t>(Data + 8 + 8 * I), At<uint32_t>(Data + 12 + 8 * I) };
        if (Lumps[I].first) ByOffset.emplace_back(Lumps[I].second, Lumps[I].first, I);
    }
    std::sort(ByOffset.begin(), ByOffset.end());
    size_t Pos = TableEnd;
    for (const auto& [Offset, Length, Index] : ByOffset)
    {
        const size_t Aligned = (Pos + 3) & ~size_t(3);
        if (Offset != Aligned)
        {
            Error = "lump " + std::to_string(Index) + " at " + std::to_string(Offset) + ", expected " + std::to_string(Aligned);
            return false;
        }
        Pos = (size_t)Offset + Length;
    }
    if (((Pos + 3) & ~size_t(3)) != Size && Pos != Size)
    {
        Error = "lumps end at " + std::to_string(Pos) + " of " + std::to_string(Size);
        return false;
    }
    const auto Lump = [&](size_t I, size_t Record, size_t& Count) -> const uint8_t* {
        Count = Lumps[I].first / Record;
        if (Lumps[I].first % Record)
        {
            Error = "lump " + std::to_string(I) + ": " + std::to_string(Lumps[I].first) + " bytes, records of " + std::to_string(Record);
            return nullptr;
        }
        return Data + Lumps[I].second;
    };
    size_t N;
    const uint8_t* P;
    if (!(P = Lump(L->Materials, 72, N))) return false;
    Out.Materials.resize(N);
    for (size_t I = 0; I < N; I++)
    {
        const char* Name = reinterpret_cast<const char*>(P + 72 * I);
        Out.Materials[I].Name.assign(Name, strnlen(Name, 64));
        Out.Materials[I].SurfaceFlags = At<uint32_t>(P + 72 * I + 64);
        Out.Materials[I].ContentFlags = At<uint32_t>(P + 72 * I + 68);
    }
    if (!(P = Lump(L->Vertices, L->VertexSize, N))) return false;
    Out.Vertices.resize(N);
    for (size_t I = 0; I < N; I++)
    {
        const uint8_t* V = P + L->VertexSize * I;
        Maps::Vertex& O = Out.Vertices[I];
        std::memcpy(O.Pos, V, 12);
        if (Out.Version == 59)   // pos, uv, lightmap uv, normal, colour
        {
            std::memcpy(O.Uv, V + 12, 8);
            std::memcpy(O.Normal, V + 28, 12);
            O.NormalRead = true;
            std::memcpy(O.Color, V + 40, 4);
        }
        else   // pos, normal, colour, uv, lightmap uv, tangent, binormal
        {
            std::memcpy(O.Normal, V + 12, 12);
            O.NormalRead = true;
            std::memcpy(O.Color, V + 24, 4);
            std::memcpy(O.Uv, V + 28, 8);
        }
    }
    if (!(P = Lump(L->Indices, 2, N))) return false;
    Out.Indices.resize(N);
    if (N) std::memcpy(Out.Indices.data(), P, N * 2);
    if (!(P = Lump(L->Soups, 16, N))) return false;
    Out.Soups.resize(N);
    for (size_t I = 0; I < N; I++)
    {
        auto& S = Out.Soups[I];
        const uint8_t* R = P + 16 * I;
        S.Material = At<uint16_t>(R);
        S.DrawOrder = At<uint16_t>(R + 2);
        S.FirstVertex = At<uint32_t>(R + 4);
        S.VertexCount = At<uint16_t>(R + 8);
        S.IndexCount = At<uint16_t>(R + 10);
        S.FirstIndex = At<uint32_t>(R + 12);
        if (S.Material >= Out.Materials.size() || (size_t)S.FirstVertex + S.VertexCount > Out.Vertices.size() ||
            (size_t)S.FirstIndex + S.IndexCount > Out.Indices.size() || S.IndexCount % 3)
        {
            Error = "soup " + std::to_string(I) + " outside its lumps";
            return false;
        }
        for (uint32_t K = 0; K < S.IndexCount; K++)
            if (Out.Indices[S.FirstIndex + K] >= S.VertexCount)
            {
                Error = "soup " + std::to_string(I) + " index past its vertices";
                return false;
            }
    }
    const auto& E = Lumps[L->Entities];
    if (E.first == 0 || Data[E.second + E.first - 1] != 0 || std::memchr(Data + E.second, 0, E.first) != Data + E.second + E.first - 1)
    {
        Error = "entity string does not end its lump";
        return false;
    }
    Out.Entities.assign(reinterpret_cast<const char*>(Data + E.second), E.first - 1);

    // the collision
    const auto Fail = [&Error](const std::string& What, size_t I) {
        Error = What + " " + std::to_string(I) + " outside its lumps";
        return false;
    };
    if (!(P = Lump(L->Planes, 16, N))) return false;
    Out.Planes.resize(N);
    for (size_t I = 0; I < N; I++)
    {
        std::memcpy(Out.Planes[I].Normal, P + 16 * I, 12);
        Out.Planes[I].Dist = At<float>(P + 16 * I + 12);
    }
    if (!(P = Lump(L->BrushSides, 8, N))) return false;
    Out.BrushSides.resize(N);
    for (size_t I = 0; I < N; I++) Out.BrushSides[I] = { At<uint32_t>(P + 8 * I), At<uint32_t>(P + 8 * I + 4) };
    if (!(P = Lump(L->Brushes, 4, N))) return false;
    Out.Brushes.resize(N);
    size_t Side = 0;
    for (size_t I = 0; I < N; I++)
    {
        auto& B = Out.Brushes[I];
        B = { At<uint16_t>(P + 4 * I), At<uint16_t>(P + 4 * I + 2) };
        if (B.Sides < 6 || B.Material >= Out.Materials.size() || Side + B.Sides > Out.BrushSides.size()) return Fail("brush", I);
        for (size_t K = 0; K < B.Sides; K++)
        {
            const auto& S = Out.BrushSides[Side + K];
            if (S.Material >= Out.Materials.size() || (K >= 6 && S.PlaneOrBound >= Out.Planes.size())) return Fail("brush side", Side + K);
        }
        Side += B.Sides;
    }
    if (Side != Out.BrushSides.size())
    {
        Error = "brushes use " + std::to_string(Side) + " of " + std::to_string(Out.BrushSides.size()) + " brush sides";
        return false;
    }
    if (!(P = Lump(L->Models, 48, N))) return false;
    Out.Models.resize(N);
    for (size_t I = 0; I < N; I++)
    {
        auto& M = Out.Models[I];
        const uint8_t* R = P + 48 * I;
        std::memcpy(M.Mins, R, 12);
        std::memcpy(M.Maxs, R + 12, 12);
        M.FirstSoup = At<uint32_t>(R + 24), M.Soups = At<uint32_t>(R + 28);
        M.FirstCollision = At<uint32_t>(R + 32), M.Collisions = At<uint32_t>(R + 36);
        M.FirstBrush = At<uint32_t>(R + 40), M.Brushes = At<uint32_t>(R + 44);
        if ((size_t)M.FirstBrush + M.Brushes > Out.Brushes.size()) return Fail("model", I);
    }
    if (L->CollisionVertices != None)
    {
        const size_t Stride = Out.Version == 59 ? 12 : 16;   // CoD2: a u32 before each
        if (!(P = Lump(L->CollisionVertices, Stride, N))) return false;
        Out.CollisionVertices.resize(N * 3);
        for (size_t I = 0; I < N; I++) std::memcpy(&Out.CollisionVertices[3 * I], P + Stride * I + Stride - 12, 12);
    }
    const size_t CollisionVertexCount = Out.CollisionVertices.size() / 3;
    if (L->Patches != None)
    {
        if (!(P = Lump(L->CollisionIndices, 2, N))) return false;
        Out.CollisionIndices.resize(N);
        if (N) std::memcpy(Out.CollisionIndices.data(), P, N * 2);
        if (!(P = Lump(L->Patches, 16, N))) return false;
        Out.Patches.resize(N);
        for (size_t I = 0; I < N; I++)
        {
            auto& S = Out.Patches[I];
            const uint8_t* R = P + 16 * I;
            S = { At<uint16_t>(R), At<uint16_t>(R + 2), At<uint16_t>(R + 4), At<uint16_t>(R + 6), At<uint32_t>(R + 8), At<uint32_t>(R + 12) };
            bool Ok = S.Material < Out.Materials.size();
            if (S.Type == 1)   // Width vertices, Height indices from Other, FirstVertex
            {
                Ok = Ok && (size_t)S.Other + S.Width <= CollisionVertexCount && (size_t)S.FirstVertex + S.Height <= Out.CollisionIndices.size() &&
                     S.Height % 3 == 0;
                for (size_t K = 0; Ok && K < S.Height; K++) Ok = Out.CollisionIndices[S.FirstVertex + K] < S.Width;
            }
            else
                Ok = Ok && S.Type == 0 && S.Width % 2 == 1 && S.Height % 2 == 1 && (size_t)S.FirstVertex + (size_t)S.Width * S.Height <= CollisionVertexCount;
            if (!Ok) return Fail("collision surface", I);
        }
        if (!(P = Lump(L->PatchRefs, 4, N))) return false;
        Out.PatchRefs.resize(N);
        for (size_t I = 0; I < N; I++)
            if ((Out.PatchRefs[I] = At<uint32_t>(P + 4 * I)) >= Out.Patches.size()) return Fail("collision surface reference", I);
        for (size_t I = 0; I < Out.Models.size(); I++)
            if ((size_t)Out.Models[I].FirstCollision + Out.Models[I].Collisions > Out.PatchRefs.size()) return Fail("model", I);
    }
    if (L->Triangles != None)
    {
        if (!(P = Lump(L->Triangles, 72, N))) return false;
        Out.Triangles.resize(N);
        for (size_t I = 0; I < N; I++)
        {
            auto& T = Out.Triangles[I];
            std::memcpy(T.Plane, P + 72 * I, 16);
            std::memcpy(T.S, P + 72 * I + 16, 16);
            std::memcpy(T.T, P + 72 * I + 32, 16);
            std::memcpy(T.Vertices, P + 72 * I + 48, 12);
            for (const uint32_t V : T.Vertices)
                if (V >= CollisionVertexCount && V != 0xFFFFFFFF) return Fail("collision triangle", I);
        }
        if (!(P = Lump(L->Partitions, 12, N))) return false;
        Out.Partitions.resize(N);
        for (size_t I = 0; I < N; I++)
        {
            Out.Partitions[I] = { P[12 * I + 2], At<uint32_t>(P + 12 * I + 4) };
            if ((size_t)Out.Partitions[I].FirstTriangle + Out.Partitions[I].Triangles > Out.Triangles.size()) return Fail("partition", I);
        }
        if (!(P = Lump(L->AabbTree, 32, N))) return false;
        Out.AabbTree.resize(N);
        for (size_t I = 0; I < N; I++)
        {
            auto& A = Out.AabbTree[I];
            A = { At<uint16_t>(P + 32 * I + 24), At<uint16_t>(P + 32 * I + 26), At<uint32_t>(P + 32 * I + 28) };
            if (A.Material >= Out.Materials.size() || (A.Children ? (size_t)A.First + A.Children > N : A.First >= Out.Partitions.size()))
                return Fail("collision tree node", I);
        }
        for (size_t I = 0; I < Out.Models.size(); I++)
            if ((size_t)Out.Models[I].FirstCollision + Out.Models[I].Collisions > Out.AabbTree.size()) return Fail("model", I);
    }
    return true;
}
}
