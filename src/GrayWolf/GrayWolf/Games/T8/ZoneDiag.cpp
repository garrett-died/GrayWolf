#include "stdafx.h"

#include <CLI/CLI.hpp>

#include "CliCommands.h"
#include "CliOptions.h"
#include "ExportNaming.h"
#include "GameCommand.h"
#include "ZoneDiag.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace
{
    void AddOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--profile", Options.ZoneWalkProfile, "zonekit profile (default bo4)");
    }

    const GameCommand Command{
        "bo4-zone-diag",
        "BO4 zones walked with the zonekit tables, for diagnosis: per-zone summary, and with --out --type strings|matimages|refs|imagekeys (assets: disk-export --game bo4)",
        "A zone (.ff/.bin), a folder of zones, a .txt list, or casc:<pattern> (from --install, e.g. casc:*.ff)",
        RunBo4ZoneDiag,
        AddOptions,
    };
    const GameCommandRegistration Registered(Command);
}

namespace Bo4ZoneDiag
{
namespace
{
    using ExportNaming::At;

    std::string HexOf(ByteView Image, int64_t Pos, size_t N)
    {
        std::string S;
        for (size_t K = 0; Pos >= 0 && K < N && (size_t)Pos + K < Image.size(); K++)
        {
            char B[4];
            std::snprintf(B, sizeof B, "%02x", Image[(size_t)Pos + K]);
            S += B;
        }
        return S;
    }

    // a record as 8-byte words, each also as two u32 and two f32
    void Words(FILE* Out, const char* Tag, ByteView Image, int64_t Pos, int64_t Bytes)
    {
        for (int64_t W = 0; W < Bytes; W += 8)
            std::fprintf(Out, "%s +0x%02llx u64 0x%016llx  u32 %u %u  f32 %g %g\n", Tag, (long long)W, (unsigned long long)At<uint64_t>(Image, Pos + W),
                         At<uint32_t>(Image, Pos + W), At<uint32_t>(Image, Pos + W + 4), (double)At<float>(Image, Pos + W), (double)At<float>(Image, Pos + W + 4));
    }

    // T8 clipMap_t fields the map reader uses (DiskGame.cpp ClipLayout), and the terrain's (terrain-spec.md)
    constexpr int64_t ClipVertexCount = 0x70, ClipVertices = 0x78, ClipTriangleCount = 0x80, ClipTriangles = 0x88, ClipHeader = 0x2d8;
    constexpr int64_t ClipSectorCount = 0x1d0, ClipSectors = 0x1d8, ClipSectorSize = 0xa8, TileSize = 80, TileSamples = 33;
    constexpr int64_t TerrainSectorCount = 0x10, TerrainSectors = 0x18, TerrainSets = 0x28, SectorSize = 0x148, SetSize = 0x20, LayerSize = 0x108;
}

void Collision(FILE* Out, const ZoneAccess& Z, const Maps::MapScene& Scene)
{
    const ByteView Img = Z.Image;
    const int64_t Clip = Z.Root("clipmap");
    std::fprintf(Out, "[collision] clipmap assets in the walk %zu, first at zone+0x%llx\n", Z.Count("clipmap"), (long long)Clip);
    if (Clip < 0) return;
    const uint32_t Vertices = At<uint32_t>(Img, Clip + ClipVertexCount), Triangles = At<uint32_t>(Img, Clip + ClipTriangleCount);
    std::fprintf(Out, "[collision] raw u32 +0x70 (vertices) %u, +0x80 (triangles) %u; vertices ptr %s, triangles ptr %s\n", Vertices, Triangles,
                 Z.Describe(At<uint64_t>(Img, Clip + ClipVertices)).c_str(), Z.Describe(At<uint64_t>(Img, Clip + ClipTriangles)).c_str());
    for (int64_t P = 0x60; P < 0xb0; P += 8)
        std::fprintf(Out, "[collision] header +0x%02llx u64 0x%016llx  u32 %u %u\n", (long long)P, (unsigned long long)At<uint64_t>(Img, Clip + P),
                     At<uint32_t>(Img, Clip + P), At<uint32_t>(Img, Clip + P + 4));
    // every u32 of the header equal to a count the reader or HydraX names
    for (int64_t P = 0; P + 4 <= ClipHeader; P += 4)
    {
        const uint32_t V = At<uint32_t>(Img, Clip + P);
        if (V == Triangles || V == Vertices || V == 58117u || V == 58109u || V == 296445u)
            std::fprintf(Out, "[collision] header u32 +0x%llx = %u\n", (long long)P, V);
    }
    // the brush table is embedded at +0x260 (ClipLayout): brush count +0x50, material count +0x60
    std::fprintf(Out, "[collision] brush table (+0x260): brushes u32 +0x50 %u, materials +0x60 %u; terrain sectors u32 +0x1d0 %u\n", At<uint32_t>(Img, Clip + 0x260 + 0x50),
                 At<uint32_t>(Img, Clip + 0x260 + 0x60), At<uint32_t>(Img, Clip + ClipSectorCount));
    const auto& C = Scene.Collision;
    const size_t Held = C.Triangles.size() / 3;
    std::set<std::array<float, 9>> Seen;
    size_t Duplicate = 0, Degenerate = 0, Up = 0, Down = 0, Steep = 0;
    for (size_t T = 0; T < Held; T++)
    {
        std::array<std::array<float, 3>, 3> P;
        for (int J = 0; J < 3; J++)
            for (int A = 0; A < 3; A++) P[J][A] = C.Vertices[(size_t)C.Triangles[T * 3 + J] * 3 + A];
        float E1[3], E2[3];
        for (int A = 0; A < 3; A++) E1[A] = P[1][A] - P[0][A], E2[A] = P[2][A] - P[0][A];
        const float N[3] = { E1[1] * E2[2] - E1[2] * E2[1], E1[2] * E2[0] - E1[0] * E2[2], E1[0] * E2[1] - E1[1] * E2[0] };
        const float Len2 = N[0] * N[0] + N[1] * N[1] + N[2] * N[2];
        if (Len2 == 0) Degenerate++;
        else if (N[2] > 0 && N[2] * N[2] > 0.25f * Len2) Up++;
        else if (N[2] < 0 && N[2] * N[2] > 0.25f * Len2) Down++;
        else Steep++;
        std::sort(P.begin(), P.end());
        std::array<float, 9> Key;
        for (int J = 0; J < 9; J++) Key[J] = P[J / 3][J % 3];
        if (!Seen.insert(Key).second) Duplicate++;
    }
    std::fprintf(Out, "[collision] reader holds vertices %zu, triangles %zu (index entries %zu); raw +0x80 minus held = %lld\n", C.Vertices.size() / 3, Held, C.Triangles.size(),
                 (long long)Triangles - (long long)Held);
    std::fprintf(Out, "[collision] held triangles: exact copies the writer drops %zu, unique %zu, degenerate %zu, facing up %zu, down %zu, steep %zu\n", Duplicate, Held - Duplicate,
                 Degenerate, Up, Down, Steep);
    std::fprintf(Out, "[collision] clip brushes read: world %zu, brush models %zu, trigger volumes %zu\n", C.Brushes.size(), C.Models.size(), C.Triggers.size());
}

namespace
{
    struct TileSet
    {
        uint32_t TilesX = 0, TilesY = 0;
        int64_t Tiles = -1;             // the 80-byte records
        std::vector<int64_t> Heights;   // per tile, the zone offset of its 33x33 u16, -1 when none
    };

    // how many shared edge samples of one tile order and sample order are bitwise equal
    struct Edges
    {
        size_t Compared = 0, Equal = 0;
    };

    Edges CheckEdges(ByteView Img, const TileSet& S, bool RowMajorTiles, bool RowMajorSamples)
    {
        Edges E;
        const auto Tile = [&](uint32_t Tx, uint32_t Ty) { return RowMajorTiles ? (size_t)Ty * S.TilesX + Tx : (size_t)Tx * S.TilesY + Ty; };
        const auto Sample = [&](int64_t Base, int Row, int Col) {
            return At<uint16_t>(Img, Base + (RowMajorSamples ? Row * TileSamples + Col : Col * TileSamples + Row) * 2);
        };
        for (uint32_t Ty = 0; Ty < S.TilesY; Ty++)
            for (uint32_t Tx = 0; Tx < S.TilesX; Tx++)
            {
                const int64_t A = S.Heights[Tile(Tx, Ty)];
                if (A < 0) continue;
                if (Tx + 1 < S.TilesX)
                    if (const int64_t B = S.Heights[Tile(Tx + 1, Ty)]; B >= 0)
                        for (int R = 0; R < TileSamples; R++, E.Compared++) E.Equal += Sample(A, R, 32) == Sample(B, R, 0);
                if (Ty + 1 < S.TilesY)
                    if (const int64_t B = S.Heights[Tile(Tx, Ty + 1)]; B >= 0)
                        for (int C = 0; C < TileSamples; C++, E.Compared++) E.Equal += Sample(A, 32, C) == Sample(B, 0, C);
            }
        return E;
    }
}

void Terrain(FILE* Out, const ZoneAccess& Z)
{
    const ByteView Img = Z.Image;
    const int64_t Gfx = Z.Root("terraingfx"), Clip = Z.Root("clipmap");
    std::fprintf(Out, "[terrain] ---- 1 header pointers ----\n");
    std::fprintf(Out, "[terrain] terraingfx assets %zu at zone+0x%llx; clipmap assets %zu at zone+0x%llx; zone bytes %zu\n", Z.Count("terraingfx"), (long long)Gfx, Z.Count("clipmap"),
                 (long long)Clip, Img.size());
    if (Gfx < 0 || Clip < 0)
    {
        std::fprintf(Out, "[terrain] no terraingfx or no clipmap in this zone\n");
        return;
    }
    Words(Out, "[terrain] gfx", Img, Gfx, 0xb8);
    const uint64_t GfxSectors = At<uint64_t>(Img, Gfx + TerrainSectorCount), Sets = At<uint64_t>(Img, Gfx + 0x20);
    const uint32_t ClipSectorsN = At<uint32_t>(Img, Clip + ClipSectorCount);
    const int64_t GfxSectorBase = Z.Off(At<uint64_t>(Img, Gfx + TerrainSectors)), SetBase = Z.Off(At<uint64_t>(Img, Gfx + TerrainSets));
    const int64_t ClipSectorBase = Z.Off(At<uint64_t>(Img, Clip + ClipSectors));
    std::fprintf(Out, "[terrain] gfx sectors %llu ptr %s -> zone+0x%llx; image sets %llu ptr %s -> zone+0x%llx\n", (unsigned long long)GfxSectors,
                 Z.Describe(At<uint64_t>(Img, Gfx + TerrainSectors)).c_str(), (long long)GfxSectorBase, (unsigned long long)Sets,
                 Z.Describe(At<uint64_t>(Img, Gfx + TerrainSets)).c_str(), (long long)SetBase);
    std::fprintf(Out, "[terrain] clip sectors u32 +0x1d0 %u ptr %s -> zone+0x%llx; counts equal: %s\n", ClipSectorsN, Z.Describe(At<uint64_t>(Img, Clip + ClipSectors)).c_str(),
                 (long long)ClipSectorBase, GfxSectors == ClipSectorsN ? "yes" : "NO");
    if (GfxSectorBase < 0 || ClipSectorBase < 0 || GfxSectors > 64) return;

    std::vector<TileSet> Sectors(GfxSectors);
    for (uint64_t S = 0; S < GfxSectors; S++)
    {
        const int64_t G = GfxSectorBase + (int64_t)S * SectorSize;
        const int64_t K = ClipSectorBase + (int64_t)S * ClipSectorSize;
        const uint64_t Layers = At<uint64_t>(Img, G + 0x110), Weights = At<uint64_t>(Img, G + 0x138);
        const int64_t LayerBase = Z.Off(At<uint64_t>(Img, G + 0x118)), WeightBase = Z.Off(At<uint64_t>(Img, G + 0x140));
        std::fprintf(Out, "[terrain] sector %llu gfx: streamed list u32 %u ptr %s; layers %llu ptr %s -> zone+0x%llx; weights %llu ptr %s -> zone+0x%llx; counts agree: %s\n",
                     (unsigned long long)S, At<uint32_t>(Img, G), Z.Describe(At<uint64_t>(Img, G + 8)).c_str(), (unsigned long long)Layers,
                     Z.Describe(At<uint64_t>(Img, G + 0x118)).c_str(), (long long)LayerBase, (unsigned long long)Weights, Z.Describe(At<uint64_t>(Img, G + 0x140)).c_str(),
                     (long long)WeightBase, Layers == Weights ? "yes" : "NO");
        std::string W;
        for (uint64_t J = 0; WeightBase >= 0 && J < Weights && J < 64; J++) W += (J ? " " : "") + std::to_string(At<uint16_t>(Img, WeightBase + (int64_t)J * 2));
        std::fprintf(Out, "[terrain] sector %llu weight table (u16 per layer): %s\n", (unsigned long long)S, W.c_str());

        TileSet& T = Sectors[S];
        T.TilesX = At<uint32_t>(Img, K + 4), T.TilesY = At<uint32_t>(Img, K + 8);
        std::fprintf(Out,
                     "[terrain] sector %llu clip: tile count u32 %u, tilesX %u, tilesY %u, samples/edge %u, tiles ptr %s, origin %g %g, bias %g, units/sample %g, scale %g, layer-surface ptr %s count %u\n",
                     (unsigned long long)S, At<uint32_t>(Img, K), T.TilesX, T.TilesY, At<uint32_t>(Img, K + 0xc), Z.Describe(At<uint64_t>(Img, K + 0x18)).c_str(),
                     (double)At<float>(Img, K + 0x20), (double)At<float>(Img, K + 0x24), (double)At<float>(Img, K + 0x70), (double)At<float>(Img, K + 0x74),
                     (double)At<float>(Img, K + 0x94), Z.Describe(At<uint64_t>(Img, K + 0x98)).c_str(), At<uint32_t>(Img, K + 0xa0));
    }

    std::fprintf(Out, "[terrain] ---- 2 clip tiles ----\n");
    for (uint64_t S = 0; S < GfxSectors; S++)
    {
        const int64_t K = ClipSectorBase + (int64_t)S * ClipSectorSize;
        TileSet& T = Sectors[S];
        T.Tiles = Z.Off(At<uint64_t>(Img, K + 0x18));
        const uint64_t Count = (uint64_t)T.TilesX * T.TilesY;
        if (T.Tiles < 0 || Count == 0 || Count > (1u << 20)) continue;
        T.Heights.assign(Count, -1);
        std::map<uint32_t, size_t> Kinds;           // u32 at +0x20
        std::map<uint16_t, size_t> Low16;           // u16 at +0x20
        std::map<std::string, size_t> MaskByKind;   // "kind:null|zone|other"
        size_t HeightsInZone = 0, HeightsConsecutive = 0, MaskInZone = 0, MaskNull = 0, MaskOther = 0, SurfaceInZone = 0, Plus22Nonzero = 0;
        int64_t Prev = -1;
        for (uint64_t N = 0; N < Count; N++)
        {
            const int64_t Rec = T.Tiles + (int64_t)N * TileSize;
            const uint32_t Kind = At<uint32_t>(Img, Rec + 0x20);
            Kinds[Kind]++, Low16[At<uint16_t>(Img, Rec + 0x20)]++;
            Plus22Nonzero += At<uint16_t>(Img, Rec + 0x22) != 0;
            const uint64_t HP = At<uint64_t>(Img, Rec + 0x28), MP = At<uint64_t>(Img, Rec + 0x30), SP = At<uint64_t>(Img, Rec + 0x38);
            const int64_t H = Z.Off(HP), M = Z.Off(MP);
            if (H >= 0 && H + TileSamples * TileSamples * 2 <= (int64_t)Img.size())
            {
                HeightsInZone++, T.Heights[N] = H;
                HeightsConsecutive += Prev >= 0 && H == Prev + TileSamples * TileSamples * 2;
                Prev = H;
            }
            const char* State = MP == 0 ? "null" : M >= 0 ? "zone" : "other";
            MaskByKind[std::to_string(Kind) + ":" + State]++;
            MaskNull += MP == 0, MaskInZone += M >= 0 && M + 137 <= (int64_t)Img.size(), MaskOther += MP != 0 && M < 0;
            SurfaceInZone += Z.Off(SP) >= 0;
        }
        std::fprintf(Out,
                     "[terrain] sector %llu tiles %llu: heights ptr in zone %zu, consecutive 2178-byte runs %zu, mask null %zu / in zone %zu / elsewhere %zu, surface block in zone %zu, u16 at +0x22 nonzero %zu\n",
                     (unsigned long long)S, (unsigned long long)Count, HeightsInZone, HeightsConsecutive, MaskNull, MaskInZone, MaskOther, SurfaceInZone, Plus22Nonzero);
        std::string KindText, LowText, MaskText;
        for (const auto& [V, N] : Kinds) KindText += " " + std::to_string(V) + "x" + std::to_string(N);
        for (const auto& [V, N] : Low16) LowText += " " + std::to_string(V) + "x" + std::to_string(N);
        for (const auto& [V, N] : MaskByKind) MaskText += " " + V + "x" + std::to_string(N);
        std::fprintf(Out, "[terrain] sector %llu +0x20 as u32 histogram (value x tiles):%s\n[terrain] sector %llu +0x20 as u16 histogram:%s\n[terrain] sector %llu mask pointer by u32 kind:%s\n",
                     (unsigned long long)S, KindText.c_str(), (unsigned long long)S, LowText.c_str(), (unsigned long long)S, MaskText.c_str());
        for (int N = 0; N < 2 && (uint64_t)N < Count; N++)
        {
            const int64_t Rec = T.Tiles + (int64_t)N * TileSize;
            std::fprintf(Out, "[terrain] sector %llu tile %d raw80 %s\n", (unsigned long long)S, N, HexOf(Img, Rec, 80).c_str());
            char Tag[64];
            std::snprintf(Tag, sizeof Tag, "[terrain] sector %llu tile %d", (unsigned long long)S, N);
            Words(Out, Tag, Img, Rec, 80);
            if (const int64_t H = Z.Off(At<uint64_t>(Img, Rec + 0x28)); H >= 0)
                std::fprintf(Out, "[terrain] sector %llu tile %d heights_hex %s\n", (unsigned long long)S, N, HexOf(Img, H, 2178).c_str());
            if (const int64_t M = Z.Off(At<uint64_t>(Img, Rec + 0x30)); M >= 0)
                std::fprintf(Out, "[terrain] sector %llu tile %d mask_hex %s\n", (unsigned long long)S, N, HexOf(Img, M, 137).c_str());
            if (const int64_t B = Z.Off(At<uint64_t>(Img, Rec + 0x38)); B >= 0)
                std::fprintf(Out, "[terrain] sector %llu tile %d surface_block_first64 %s\n", (unsigned long long)S, N, HexOf(Img, B, 64).c_str());
        }
    }

    std::fprintf(Out, "[terrain] ---- 3 height grid ----\n");
    for (uint64_t S = 0; S < GfxSectors; S++)
    {
        TileSet& T = Sectors[S];
        if (T.Heights.empty()) continue;
        const int64_t K = ClipSectorBase + (int64_t)S * ClipSectorSize;
        const float Bias = At<float>(Img, K + 0x70), Scale = At<float>(Img, K + 0x94);
        struct Combo
        {
            bool Tiles, Samples;
            const char* Name;
            Edges E;
        };
        Combo Combos[4] = { { true, true, "tile row-major, sample row-major", {} },
                            { true, false, "tile row-major, sample column-major", {} },
                            { false, true, "tile column-major, sample row-major", {} },
                            { false, false, "tile column-major, sample column-major", {} } };
        size_t Best = 0;
        for (size_t C = 0; C < 4; C++)
        {
            Combos[C].E = CheckEdges(Img, T, Combos[C].Tiles, Combos[C].Samples);
            std::fprintf(Out, "[terrain] sector %llu edges %s: %zu of %zu shared samples bitwise equal\n", (unsigned long long)S, Combos[C].Name, Combos[C].E.Equal,
                         Combos[C].E.Compared);
            if (Combos[C].E.Equal > Combos[Best].E.Equal) Best = C;
        }
        // the sector's grid in the best order, then the smoothness of bias + u16 * scale
        const uint32_t Wd = T.TilesX * 32 + 1, Ht = T.TilesY * 32 + 1;
        std::vector<uint16_t> G((size_t)Wd * Ht, 0);
        for (uint32_t Ty = 0; Ty < T.TilesY; Ty++)
            for (uint32_t Tx = 0; Tx < T.TilesX; Tx++)
            {
                const size_t Index = Combos[Best].Tiles ? (size_t)Ty * T.TilesX + Tx : (size_t)Tx * T.TilesY + Ty;
                const int64_t H = T.Heights[Index];
                if (H < 0) continue;
                for (int R = 0; R < TileSamples; R++)
                    for (int C = 0; C < TileSamples; C++)
                        G[(size_t)(Ty * 32 + R) * Wd + Tx * 32 + C] = At<uint16_t>(Img, H + (Combos[Best].Samples ? R * TileSamples + C : C * TileSamples + R) * 2);
            }
        double SumX = 0, SumY = 0, MaxX = 0, MaxY = 0, MaxSecond = 0, SumSecond = 0;
        size_t N = 0, NY = 0, NSecond = 0, Steps8 = 0;
        uint16_t Lo = 65535, Hi = 0;
        for (uint32_t R = 0; R < Ht; R++)
            for (uint32_t C = 0; C < Wd; C++)
            {
                const uint16_t V = G[(size_t)R * Wd + C];
                Lo = (std::min)(Lo, V), Hi = (std::max)(Hi, V);
                if (C + 1 < Wd)
                {
                    const double D = std::fabs(((double)G[(size_t)R * Wd + C + 1] - V) * Scale);
                    SumX += D, MaxX = (std::max)(MaxX, D), N++, Steps8 += D > 8.0;
                    if (C + 2 < Wd)
                    {
                        const double D2 = std::fabs(((double)G[(size_t)R * Wd + C + 2] - 2.0 * G[(size_t)R * Wd + C + 1] + V) * Scale);
                        SumSecond += D2, MaxSecond = (std::max)(MaxSecond, D2), NSecond++;
                    }
                }
                if (R + 1 < Ht)
                {
                    const double D = std::fabs(((double)G[(size_t)(R + 1) * Wd + C] - V) * Scale);
                    SumY += D, MaxY = (std::max)(MaxY, D), NY++;
                }
            }
        std::fprintf(Out,
                     "[terrain] sector %llu grid %ux%u in order \"%s\": z %g .. %g (bias %g + u16 %u..%u * scale %g); |dz| along x mean %g max %g, along y mean %g max %g; |d2z| mean %g max %g; x steps over 8 units %zu of %zu\n",
                     (unsigned long long)S, Wd, Ht, Combos[Best].Name, (double)Bias + Lo * (double)Scale, (double)Bias + Hi * (double)Scale, (double)Bias, Lo, Hi, (double)Scale,
                     N ? SumX / N : 0.0, MaxX, NY ? SumY / NY : 0.0, MaxY, NSecond ? SumSecond / NSecond : 0.0, MaxSecond, Steps8, N);
    }

    std::fprintf(Out, "[terrain] ---- 4 image pixels ----\n");
    std::fprintf(Out, "[terrain] markers: inline 0xffffffffffffffff, resident 0xfffffffffffffffe\n");
    for (uint64_t S = 0; SetBase >= 0 && S < Sets && S < 64; S++)
    {
        const int64_t Set = SetBase + (int64_t)S * SetSize;
        std::fprintf(Out, "[terrain] set %llu raw %s (useful width u32 +0x18 %u, stored width +0x1c %u)\n", (unsigned long long)S, HexOf(Img, Set, 32).c_str(),
                     At<uint32_t>(Img, Set + 0x18), At<uint32_t>(Img, Set + 0x1c));
        static const char* Names[3] = { "height", "cutout", "weight" };
        for (int I = 0; I < 3; I++)
        {
            const uint64_t P = At<uint64_t>(Img, Set + I * 8);
            const int64_t H = Z.Off(P);
            std::fprintf(Out, "[terrain] set %llu %s image ptr %s", (unsigned long long)S, Names[I], Z.Describe(P).c_str());
            if (H < 0)
            {
                std::fprintf(Out, " (header not in this zone)\n");
                continue;
            }
            const uint64_t Pixels = At<uint64_t>(Img, H + 0x18);
            const uint32_t Size = At<uint32_t>(Img, H + 0x60), Format = At<uint32_t>(Img, H + 0x64);
            const uint32_t W = At<uint16_t>(Img, H + 0x68), Hh = At<uint16_t>(Img, H + 0x6a);
            const int64_t Pix = Z.Off(Pixels);
            const uint64_t Expect = Format == 56 ? (uint64_t)W * Hh * 2 : Format == 80 ? (uint64_t)(W / 4) * (Hh / 4) * 8 : 0;
            const std::string Slices = Expect == 0 ? "of an unknown format" : Size % Expect == 0 ? std::to_string(Size / Expect) + " slice(s)" : "NOT a multiple";
            std::fprintf(Out, "; name %s; pixels u64 +0x18 = %s; size +0x60 %u, format +0x64 %u, %ux%u; one slice %llu bytes -> size is %s", Z.NameAt(H).c_str(),
                         Z.Describe(Pixels).c_str(), Size, Format, W, Hh, (unsigned long long)Expect, Slices.c_str());
            if (Pix >= 0)
                std::fprintf(Out, "; pixel block %s the zone (zone+0x%llx..+0x%llx of %zu)", Pix + Size <= (int64_t)Img.size() ? "fits in" : "overruns", (long long)Pix,
                             (long long)(Pix + Size), Img.size());
            std::fprintf(Out, "\n[terrain] set %llu %s image raw88 %s\n", (unsigned long long)S, Names[I], HexOf(Img, H, 0x88).c_str());
        }
    }

    std::fprintf(Out, "[terrain] ---- 5 layers ----\n");
    for (uint64_t S = 0; S < GfxSectors; S++)
    {
        const int64_t G = GfxSectorBase + (int64_t)S * SectorSize;
        const uint64_t Layers = At<uint64_t>(Img, G + 0x110);
        const int64_t Base = Z.Off(At<uint64_t>(Img, G + 0x118));
        if (Base < 0 || Layers > 64) continue;
        for (uint64_t L = 0; L < Layers; L++)
        {
            const int64_t R = Base + (int64_t)L * LayerSize;
            const uint64_t M = At<uint64_t>(Img, R);
            const int64_t MO = Z.Off(M);
            std::fprintf(Out, "[terrain] sector %llu layer %llu: material ptr %s name %s; matrix %g %g %g %g | %g %g %g %g; flags u32 +0x60 0x%x (+0x5c 0x%x, +0x64 0x%x)\n",
                         (unsigned long long)S, (unsigned long long)L, Z.Describe(M).c_str(), MO >= 0 ? Z.NameAt(MO).c_str() : "", (double)At<float>(Img, R + 0x10),
                         (double)At<float>(Img, R + 0x14), (double)At<float>(Img, R + 0x18), (double)At<float>(Img, R + 0x1c), (double)At<float>(Img, R + 0x20),
                         (double)At<float>(Img, R + 0x24), (double)At<float>(Img, R + 0x28), (double)At<float>(Img, R + 0x2c), At<uint32_t>(Img, R + 0x60),
                         At<uint32_t>(Img, R + 0x5c), At<uint32_t>(Img, R + 0x64));
        }
    }
}
}
