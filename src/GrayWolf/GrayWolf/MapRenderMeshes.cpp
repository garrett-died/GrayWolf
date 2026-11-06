#include "stdafx.h"

#include "MapRenderMeshes.h"

#include "MapRadiantRules.h"
#include "MapRadiantTextures.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

// See MapRenderMeshes.h. HydraX's BuildMeshes/AddDecals work on collision triangles matched to render triangles; here the triangles ARE the render
// triangles, so a point's render data is its own vertex's, and the twin and interpolation steps of HydraX drop out. What stays: the weld, the proofs, the
// border test (IsPatchBorder), RestoreIds, OrientToRender, the fan corner rule (FanCorner) and the coverage check.
namespace Maps
{
using namespace RadiantGeometry;

void RenderMeshCounters::Add(const RenderMeshCounters& O)
{
    TrianglesIn += O.TrianglesIn;
    CopiesRemoved += O.CopiesRemoved;
    CopiesKeptFront += O.CopiesKeptFront;
    CopiesKeptUv += O.CopiesKeptUv;
    CopiesKeptColour += O.CopiesKeptColour;
    ContainedRemoved += O.ContainedRemoved;
    MeshesNoAttributes += O.MeshesNoAttributes;
    MeshesNoMaterial += O.MeshesNoMaterial;
    CellsBothDiagonals += O.CellsBothDiagonals;
    CellsBothDiagonalsNonPlanar += O.CellsBothDiagonalsNonPlanar;
    MaxCandidatePotential = (std::max)(MaxCandidatePotential, O.MaxCandidatePotential);
    CollapsedByWeld += O.CollapsedByWeld;
    Covered += O.Covered;
    Grids += O.Grids;
    Fans += O.Fans;
    LoneTriangles += O.LoneTriangles;
    Cells += O.Cells;
    BordersCutTexture += O.BordersCutTexture;
    BordersCutColour += O.BordersCutColour;
    BordersCutFacing += O.BordersCutFacing;
    Flipped += O.Flipped;
    FacingKept += O.FacingKept;
    FacingUnknown += O.FacingUnknown;
    FacingNormal += O.FacingNormal;
    FacingAgainst += O.FacingAgainst;
    FacingUndecided += O.FacingUndecided;
    GridsSplit += O.GridsSplit;
    Meshes += O.Meshes;
    MeshesColoured += O.MeshesColoured;
    DegenerateUv += O.DegenerateUv;
    CoverageMissing += O.CoverageMissing;
    CoverageLeftover += O.CoverageLeftover;
    LabelsFailed += O.LabelsFailed;
    AccountingFailed += O.AccountingFailed;
    Labels += O.Labels;
    UvTrianglesShifted += O.UvTrianglesShifted;
    UvConflictCorners += O.UvConflictCorners;
    CandidatesCapped += O.CandidatesCapped;
    OverlapPartial += O.OverlapPartial;
    OverlapContained += O.OverlapContained;
    OverlapTested += O.OverlapTested;
    MaxAbsT = (std::max)(MaxAbsT, O.MaxAbsT);
}

namespace
{
    // The facing of one cell: the sum over its four corners of (group step) x (point step). QuadMesher::GridFacing reads one corner a cell, which a sliver or a folded cell
    // whose first corner is nearly straight turns into noise; for a planar cell the four corners agree.
    V3 CellVector(const V3& P00, const V3& P10, const V3& P11, const V3& P01)
    {
        V3 Sum = Cross(Sub(P10, P00), Sub(P01, P00));
        Sum = Add(Sum, Cross(Sub(P10, P00), Sub(P11, P10)));
        Sum = Add(Sum, Cross(Sub(P11, P01), Sub(P01, P00)));
        return Add(Sum, Cross(Sub(P11, P01), Sub(P11, P10)));
    }

    // How well a grid's facing agrees with a vector per triangle (the front normals, or the vertex normals): the votes of its cells, +1 when (the cell's facing) . (the vectors of
    // the cell's triangles) is positive, -1 when negative. Positive: the grid shows on that side. Judged cell by cell, not on the sum of all of them: a tube wraps its normals round and their sum is nothing.
    // Triangles are listed in cell order (HydraX's QuadGrid.Triangles): two a cell in a rectangle, one a cell in a fan, the one of a lone triangle.
    double FacingScore(const QuadGrid& Grid, const std::function<V3(int)>& Position, const std::function<V3(int32_t)>& Vector)
    {
        double Score = 0;
        const size_t Count = Grid.Triangles.size();
        const bool Pairs = !Grid.IsFan && !Grid.IsTriangle;
        const int Rows = Grid.GroupSize - 1;
        for (int G = 0; G + 1 < Grid.GroupCount; G++)
            for (int I = 0; I + 1 < Grid.GroupSize; I++)
            {
                const V3 Cell = CellVector(Position(Grid.At(G, I)), Position(Grid.At(G + 1, I)), Position(Grid.At(G + 1, I + 1)), Position(Grid.At(G, I + 1)));
                const size_t Index = Pairs ? static_cast<size_t>(2) * (static_cast<size_t>(G) * static_cast<size_t>(Rows) + static_cast<size_t>(I)) : static_cast<size_t>(Grid.IsTriangle ? 0 : I);
                if (Index >= Count) continue;
                V3 Sum = Vector(Grid.Triangles[Index]);
                if (Pairs && Index + 1 < Count) Sum = Add(Sum, Vector(Grid.Triangles[Index + 1]));
                const double Agree = Dot(Cell, Sum);
                Score += Agree > 0 ? 1.0 : Agree < 0 ? -1.0 : 0.0;   // a vote a cell: one very large cell must not outvote the rest
            }
        return Score;
    }

    constexpr double MaxShift = 16777216;   // 2^24 repeats: far inside int, and far beyond any texture

    int32_t BitsOf(float F)
    {
        int32_t B;
        std::memcpy(&B, &F, sizeof(B));
        return B;
    }

    struct LabelOut
    {
        std::vector<RenderMesh> Meshes;
        RenderMeshCounters Count;
        std::string Coverage, BothDiagonals;
    };

    // One material of the owner: its triangles (Tris, ascending) are compacted into a soup of their own so the mesher's per-vertex tables stay small,
    // however many materials the owner has. The vertex order is kept (the pair and border models read the ids).
    void RunLabel(const RenderSoup& S, int Label, const std::vector<int32_t>& Tris, const std::vector<int32_t>& GlobalRep, const ProofList& Proofs, const std::string& Name,
                  const RenderMeshOptions& O, LabelOut& Out)
    {
        if (O.FailLabel == Label) throw std::runtime_error("RenderMeshes: forced failure of a material (test)");
        const size_t TN = Tris.size();
        std::vector<int32_t> Used;
        Used.reserve(TN * 3);
        for (const int32_t T : Tris)
            for (int K = 0; K < 3; K++) Used.push_back(S.Indices[static_cast<size_t>(T) * 3 + K]);
        std::sort(Used.begin(), Used.end());
        Used.erase(std::unique(Used.begin(), Used.end()), Used.end());
        const auto LocalOf = [&](int32_t G) { return static_cast<int32_t>(std::lower_bound(Used.begin(), Used.end(), G) - Used.begin()); };
        std::vector<float> LP(Used.size() * 3);
        for (size_t I = 0; I < Used.size(); I++)
            for (size_t C = 0; C < 3; C++) LP[I * 3 + C] = S.Positions[static_cast<size_t>(Used[I]) * 3 + C];
        std::vector<int32_t> LI(TN * 3);
        for (size_t P = 0; P < TN; P++)
            for (int K = 0; K < 3; K++) LI[P * 3 + K] = LocalOf(S.Indices[static_cast<size_t>(Tris[P]) * 3 + K]);

        // the weld's representative of each local vertex: the lowest local vertex of its cluster (a cluster may reach into other materials)
        std::vector<int32_t> LRep(Used.size());
        {
            std::unordered_map<int32_t, int32_t> First;
            for (size_t I = 0; I < Used.size(); I++)
            {
                const int32_t Cluster = O.Decal ? Used[I] : GlobalRep[static_cast<size_t>(Used[I])];
                LRep[I] = First.emplace(Cluster, static_cast<int32_t>(I)).first->second;
            }
        }

        // the triangles taken out with a proof (by position in Tris), and the ones that stay
        ProofList LocalProofs;
        std::vector<int32_t> Keep;
        Keep.reserve(TN);
        for (size_t P = 0; P < TN; P++)
        {
            const auto It = std::lower_bound(Proofs.begin(), Proofs.end(), Tris[P], [](const auto& E, int32_t Key) { return E.first < Key; });
            if (It != Proofs.end() && It->first == Tris[P])
                LocalProofs.emplace_back(static_cast<int>(P), It->second);
            else
                Keep.push_back(static_cast<int32_t>(P));
        }
        std::vector<int32_t> Welded(Keep.size() * 3);
        for (size_t K = 0; K < Keep.size(); K++)
            for (int C = 0; C < 3; C++) Welded[K * 3 + C] = LRep[static_cast<size_t>(LI[static_cast<size_t>(Keep[K]) * 3 + C])];

        // the vertex of triangle P (a position in Tris) that the welded vertex stands for: its render data belongs to the unwelded vertex
        const auto OwnVertex = [&](int32_t P, int32_t WeldedVertex) {
            for (int K = 0; K < 3; K++)
            {
                const int32_t V = LI[static_cast<size_t>(P) * 3 + K];
                if (LRep[static_cast<size_t>(V)] == WeldedVertex) return V;
            }
            return static_cast<int32_t>(-1);
        };
        const auto CornerOf = [&](int32_t LocalVertex) -> const RenderCorner& { return S.Corners[static_cast<size_t>(Used[static_cast<size_t>(LocalVertex)])]; };
        // A patch border the render data shows: two triangles that meet at an edge carry different texture coordinates (compared modulo the repeat, a surface restarts its
        // coordinates at whole numbers) or different vertex colours at either edge point (HydraX IsPatchBorder), or are drawn on opposite sides. A decal soup
        // is not welded, so its triangles meet only at a shared vertex, which has one set of data: no test but the side.
        const auto FrontOf = [&](int32_t Kept) -> const std::array<double, 3>& { return S.Front[static_cast<size_t>(Tris[static_cast<size_t>(Keep[static_cast<size_t>(Kept)])])]; };
        // which way a kept triangle'"'"'s winding runs against the side it is drawn on: two triangles with different answers would be laid out as a cell wound against its
        // neighbour, and a mesh shows on one side only, so they are never joined
        std::vector<int8_t> Wound(Keep.size(), 0);
        for (size_t K = 0; K < Keep.size(); K++)
        {
            const auto Pt = [&](int Corner) { return V3{ LP[static_cast<size_t>(LI[static_cast<size_t>(Keep[K]) * 3 + Corner]) * 3], LP[static_cast<size_t>(LI[static_cast<size_t>(Keep[K]) * 3 + Corner]) * 3 + 1], LP[static_cast<size_t>(LI[static_cast<size_t>(Keep[K]) * 3 + Corner]) * 3 + 2] }; };
            const double D = Dot(Cross(Sub(Pt(1), Pt(0)), Sub(Pt(2), Pt(0))), FrontOf(static_cast<int32_t>(K)));
            Wound[K] = D > 0 ? 1 : D < 0 ? -1 : 0;
        }
        enum class Differs { No, Facing, Texture, Colour };
        const auto AtVertex = [&](int First, int Second, int Vertex) {
            const int32_t X = OwnVertex(Keep[static_cast<size_t>(First)], Vertex), Y = OwnVertex(Keep[static_cast<size_t>(Second)], Vertex);
            if (X < 0 || Y < 0) return Differs::No;
            const RenderCorner &Cx = CornerOf(X), &Cy = CornerOf(Y);
            if (!Cx.HasData || !Cy.HasData) return Differs::No;   // a corner no render triangle sits on joins every neighbour (HydraX IsPatchBorder)
            const double Du = static_cast<double>(Cx.U) - Cy.U, Dv = static_cast<double>(Cx.V) - Cy.V;
            const bool Texture = std::fabs(Du - std::nearbyint(Du)) > O.UvTolerance || std::fabs(Dv - std::nearbyint(Dv)) > O.UvTolerance;
            if (Texture) return Differs::Texture;
            return std::memcmp(Cx.Color, Cy.Color, 4) != 0 ? Differs::Colour : Differs::No;
        };
        const auto Count = [&](Differs D) {
            (D == Differs::Facing ? Out.Count.BordersCutFacing : D == Differs::Texture ? Out.Count.BordersCutTexture : Out.Count.BordersCutColour)++;
        };
        QuadMesher::BuildOptions BO;
        BO.VertexOrderBorders = O.VertexOrderBorders;
        BO.HashSeed = O.HashSeed;
        BO.MaxCandidates = O.MaxCandidates;
        // Two triangles are the halves of one cell only when nothing shows a border between them: the same test on the vertices they share (a pair is a cell, and the
        // mesh gives each point the data of one triangle, so the colours and coordinates must agree).
        BO.CanPair = [&](int First, int Second) {
            if (Dot(FrontOf(First), FrontOf(Second)) < 0 || Wound[static_cast<size_t>(First)] * Wound[static_cast<size_t>(Second)] < 0) return false;
            if (O.Decal) return true;
            for (int K = 0; K < 3; K++)
            {
                const int32_t Vertex = Welded[static_cast<size_t>(First) * 3 + K];
                const bool Shared = Welded[static_cast<size_t>(Second) * 3] == Vertex || Welded[static_cast<size_t>(Second) * 3 + 1] == Vertex || Welded[static_cast<size_t>(Second) * 3 + 2] == Vertex;
                if (Shared && AtVertex(First, Second, Vertex) != Differs::No) return false;
            }
            return true;
        };
        BO.IsBorder = [&](int First, int Second, int A, int B) {
            if (Dot(FrontOf(First), FrontOf(Second)) < 0 || Wound[static_cast<size_t>(First)] * Wound[static_cast<size_t>(Second)] < 0)
            {
                Count(Differs::Facing);
                return true;
            }
            if (O.Decal) return false;   // not welded: the triangles share vertices, which have one set of data
            for (const int Vertex : { A, B })
                if (const Differs D = AtVertex(First, Second, Vertex); D != Differs::No)
                {
                    Count(D);
                    return true;
                }
            return false;
        };
        QuadMesher::BuildResult Built = QuadMesher::Build(LP, Welded, std::vector<int32_t>(Keep.size(), 0), BO);
        if (Built.CandidatesCapped) Out.Count.CandidatesCapped++;
        Out.Count.MaxCandidatePotential = (std::max)(Out.Count.MaxCandidatePotential, Built.CandidatePotential);

        // The grid holds welded ids; each point goes back to the id its own triangles use (RestoreIds), and its triangles back to positions in Tris.
        std::vector<QuadGrid> Grids;
        Grids.reserve(Built.Grids.size());
        std::vector<int32_t> OwnOf(Used.size(), -1), OwnStamp(Used.size(), 0), OwnTri(Used.size(), 0);
        struct WalkItem
        {
            int32_t Point, Tri, K;
        };
        std::vector<WalkItem> Items;
        std::vector<int32_t> Walk;
        std::vector<int> OffU, OffV;
        std::vector<char> Done;
        int32_t Stamp = 0;
        for (QuadMesher::BuiltGrid& B : Built.Grids)
        {
            QuadGrid G = std::move(B.Grid);
            for (int32_t& T : G.Triangles) T = Keep[static_cast<size_t>(T)];
            Stamp++;
            for (const int32_t T : G.Triangles)
                for (int K = 0; K < 3; K++)
                {
                    const int32_t V = LI[static_cast<size_t>(T) * 3 + K];
                    const size_t R = static_cast<size_t>(LRep[static_cast<size_t>(V)]);
                    if (OwnStamp[R] != Stamp)
                    {
                        OwnStamp[R] = Stamp;
                        OwnOf[R] = V;
                    }
                }
            for (int32_t& P : G.Points)
            {
                const size_t R = static_cast<size_t>(P);
                if (OwnStamp[R] == Stamp) P = OwnOf[R];
            }
            Grids.push_back(std::move(G));
        }

        const CoverageResult Cov = PatchCoverage::Check(LP, LI, Grids, LocalProofs, LRep);
        RenderMeshCounters& C = Out.Count;
        C.Labels++;
        C.CellsBothDiagonals += static_cast<size_t>(Cov.CellsBothDiagonals);
        C.CellsBothDiagonalsNonPlanar += static_cast<size_t>(Cov.CellsBothDiagonalsNonPlanar);
        for (const CoverageLine& L : Cov.Lines)
            if (L.Kind == "both_diagonals") Out.BothDiagonals += "material " + Name + ": " + L.Reason + '\n';
        C.TrianglesIn += TN;
        C.Covered += static_cast<size_t>(Cov.Covered);
        C.Cells += static_cast<size_t>(Cov.Cells);
        C.CoverageMissing += static_cast<size_t>(Cov.Missing);
        C.CoverageLeftover += static_cast<size_t>(Cov.Leftover);
        if (!Cov.IsComplete())
        {
            C.LabelsFailed++;
            Out.Coverage += "material " + Name + ": " + Cov.Summary("render triangles") + "\n";
            size_t Listed = 0;
            for (const CoverageLine& L : Cov.Lines)
            {
                if (L.Kind != "missing" && L.Kind != "leftover") continue;
                if (Listed++ >= 200) break;
                Out.Coverage += L.Kind + "\t" + std::to_string(L.Triangle < 0 ? -1 : Tris[static_cast<size_t>(L.Triangle)]) + "\t" + L.Reason + "\n";
            }
        }

        const auto LocalPoint = [&](int32_t V) { return V3{ LP[static_cast<size_t>(V) * 3], LP[static_cast<size_t>(V) * 3 + 1], LP[static_cast<size_t>(V) * 3 + 2] }; };
        DegenerateMeshUv Degenerate;
        for (QuadGrid& Found : Grids)
        {
            if (Found.IsFan)
                C.Fans++;
            else if (Found.IsTriangle)
                C.LoneTriangles++;
            C.Grids++;

            // A mesh shows its texture on the side its group direction x point direction points to. The triangles' winding does not say which side the engine draws, the
            // front normal does (the vertex normals, else the game's winding): a grid facing against its triangles' front normals, cell by cell, is written the other way round
            // (OrientToRender).
            const auto FrontOfTriangle = [&](int32_t T) { return S.Front[static_cast<size_t>(Tris[static_cast<size_t>(T)])]; };
            const auto NormalOfTriangle = [&](int32_t T) { return S.Normal[static_cast<size_t>(Tris[static_cast<size_t>(T)])]; };
            const double Score = FacingScore(Found, LocalPoint, FrontOfTriangle);
            QuadGrid Grid = Found;
            if (!std::isfinite(Score) || Score == 0)
            {
                C.FacingUnknown++;
            }
            else if (Score < 0)
            {
                C.Flipped++;
                Grid = QuadMesher::Reversed(Found);
            }
            else
            {
                C.FacingKept++;
            }
            // (reversing a grid negates every cell's facing; its triangles stay in the order they were found)
            const double AgainstNormals = (Score < 0 ? -1.0 : 1.0) * FacingScore(Found, LocalPoint, NormalOfTriangle);
            (AgainstNormals > 0 ? C.FacingNormal : AgainstNormals < 0 ? C.FacingAgainst : C.FacingUndecided)++;

            // Each point takes the render data of the first triangle of the grid that has it; a fan's pivot copies and its last rim point take the corner of their own triangle
            // (FanCorner). The cut is made modulo the repeat, so triangles of the grid may carry texture coordinates a whole number of repeats apart (a surface restarts its
            // coordinates at whole numbers): each triangle gets an integer shift so that it agrees with the triangles it shares points with (a walk over the grid), and a point's
            // coordinate is its triangle's corner plus that shift. Without it a cell would run its texture backwards or repeat it between two points.
            Stamp++;
            const size_t NT = Grid.Triangles.size();
            for (size_t I = 0; I < NT; I++)
                for (int K = 0; K < 3; K++)
                {
                    const int32_t V = LI[static_cast<size_t>(Grid.Triangles[I]) * 3 + K];
                    const size_t R = static_cast<size_t>(LRep[static_cast<size_t>(V)]);
                    if (OwnStamp[R] != Stamp)
                    {
                        OwnStamp[R] = Stamp;
                        OwnOf[R] = V;
                        OwnTri[R] = static_cast<int32_t>(I);
                    }
                }
            OffU.assign(NT, 0);
            OffV.assign(NT, 0);
            Done.assign(NT, 0);
            Items.clear();
            for (size_t I = 0; I < NT; I++)
                for (int K = 0; K < 3; K++) Items.push_back({ LRep[static_cast<size_t>(LI[static_cast<size_t>(Grid.Triangles[I]) * 3 + K])], static_cast<int32_t>(I), K });
            std::sort(Items.begin(), Items.end(), [](const WalkItem& A, const WalkItem& B) { return A.Point != B.Point ? A.Point < B.Point : A.Tri < B.Tri; });
            // A breadth-first walk from the first triangle (cell order) through the points the triangles share, each reached triangle shifted to agree with the one it was
            // reached from. The result is the same for the same input (the items are sorted, nothing reads a hash order), not independent of the order of the triangles: where a loop
            // of triangles does not close, the triangle reached first decides which shift wins and the others count as conflicts.
            for (size_t Seed = 0; Seed < NT; Seed++)
            {
                if (Done[Seed]) continue;
                Done[Seed] = 1;
                Walk.assign(1, static_cast<int32_t>(Seed));
                for (size_t Head = 0; Head < Walk.size(); Head++)
                {
                    const size_t T = static_cast<size_t>(Walk[Head]);
                    for (int K = 0; K < 3; K++)
                    {
                        const int32_t V = LI[static_cast<size_t>(Grid.Triangles[T]) * 3 + K];
                        const int32_t Point = LRep[static_cast<size_t>(V)];
                        const RenderCorner& Cv = CornerOf(V);
                        if (!Cv.HasData) continue;
                        const double WantU = static_cast<double>(Cv.U) + OffU[T], WantV = static_cast<double>(Cv.V) + OffV[T];
                        const auto Range = std::equal_range(Items.begin(), Items.end(), WalkItem{ Point, 0, 0 }, [](const WalkItem& A, const WalkItem& B) { return A.Point < B.Point; });
                        for (auto It = Range.first; It != Range.second; ++It)
                        {
                            if (static_cast<size_t>(It->Tri) == T) continue;
                            const RenderCorner& Co = CornerOf(LI[static_cast<size_t>(Grid.Triangles[static_cast<size_t>(It->Tri)]) * 3 + static_cast<size_t>(It->K)]);
                            if (!Co.HasData) continue;
                            // a shift beyond MaxShift repeats (or not a number) is not applied: the cast to int would be undefined, and a mesh that far from its neighbour is not one
                            // surface; it counts as a conflict below
                            const auto Shift = [](double Difference) { return std::isfinite(Difference) && std::fabs(Difference) <= MaxShift ? static_cast<int>(std::nearbyint(Difference)) : 0; };
                            const size_t Other = static_cast<size_t>(It->Tri);
                            if (!Done[Other])
                            {
                                Done[Other] = 1;
                                OffU[Other] = Shift(WantU - Co.U);
                                OffV[Other] = Shift(WantV - Co.V);
                                Walk.push_back(It->Tri);
                            }
                            // a corner that still disagrees with the shared point (a loop of triangles that does not close, or a difference that is not a whole repeat)
                            if (std::fabs((Co.U + OffU[Other]) - WantU) > O.UvTolerance || std::fabs((Co.V + OffV[Other]) - WantV) > O.UvTolerance) C.UvConflictCorners++;
                        }
                    }
                }
            }
            for (size_t I = 0; I < NT; I++) C.UvTrianglesShifted += (OffU[I] != 0 || OffV[I] != 0) ? 1 : 0;
            MapMesh Mesh;
            Mesh.GroupCount = Grid.GroupCount;
            Mesh.GroupSize = Grid.GroupSize;
            Mesh.Source = Name;
            Mesh.NonColliding = !O.Collision;
            Mesh.Points.reserve(Grid.Points.size());
            Mesh.Data.reserve(Grid.Points.size());
            bool Coloured = false;
            // The mesh's material (collision): the render material most of its triangles' twins have. A point takes data only from a corner that has some and whose twin has that
            // material (r46: a neighbouring material's data gave anisotropy 198), its own triangle's first, then any other triangle of the grid at the point; a point with none
            // leaves the mesh without attributes (written with the running coordinate).
            int32_t MeshMaterial = -1;
            if (!S.TwinMaterial.empty())
            {
                std::map<int32_t, size_t> Votes;
                for (const int32_t T : Grid.Triangles)
                    if (const int32_t M = S.TwinMaterial[static_cast<size_t>(Tris[static_cast<size_t>(T)])]; M >= 0) Votes[M]++;
                size_t Best = 0;
                for (const auto& [M, N] : Votes)
                    if (N > Best) Best = N, MeshMaterial = M;
            }
            const auto Eligible = [&](size_t Tri, int32_t Vertex) {
                if (!CornerOf(Vertex).HasData) return false;
                return S.TwinMaterial.empty() || MeshMaterial < 0 || S.TwinMaterial[static_cast<size_t>(Tris[static_cast<size_t>(Grid.Triangles[Tri])])] == MeshMaterial;
            };
            bool NoAttributes = false;
            for (int Gi = 0; Gi < Grid.GroupCount; Gi++)
                for (int Pi = 0; Pi < Grid.GroupSize; Pi++)
                {
                    const int32_t V = Grid.At(Gi, Pi);
                    const size_t R = static_cast<size_t>(LRep[static_cast<size_t>(V)]);
                    int32_t Source = OwnOf[R];
                    size_t Tri = static_cast<size_t>(OwnTri[R]);
                    if (Grid.IsFan)
                    {
                        const size_t Column = static_cast<size_t>((std::min)(Pi, static_cast<int>(NT) - 1));
                        if (const int32_t Own = OwnVertex(Grid.Triangles[Column], LRep[static_cast<size_t>(V)]); Own >= 0) Source = Own, Tri = Column;
                    }
                    if (!Eligible(Tri, Source))
                    {
                        Source = -1;
                        const auto Range = std::equal_range(Items.begin(), Items.end(), WalkItem{ static_cast<int32_t>(R), 0, 0 }, [](const WalkItem& A, const WalkItem& B) { return A.Point < B.Point; });
                        for (auto It = Range.first; It != Range.second && Source < 0; ++It)
                        {
                            const int32_t Candidate = LI[static_cast<size_t>(Grid.Triangles[static_cast<size_t>(It->Tri)]) * 3 + static_cast<size_t>(It->K)];
                            if (Eligible(static_cast<size_t>(It->Tri), Candidate)) Source = Candidate, Tri = static_cast<size_t>(It->Tri);
                        }
                    }
                    Mesh.Points.push_back(LocalPoint(V));
                    if (Source < 0)
                    {
                        NoAttributes = true;
                        Mesh.Data.push_back({ 0, 0, 255, 255, 255, 255 });
                        continue;
                    }
                    const RenderCorner& Cn = CornerOf(Source);
                    Mesh.Data.push_back({ static_cast<double>(Cn.U) + OffU[Tri], static_cast<double>(Cn.V) + OffV[Tri], Cn.Color[0], Cn.Color[1], Cn.Color[2], Cn.Color[3] });
                    Coloured = Coloured || Cn.Color[0] != 255 || Cn.Color[1] != 255 || Cn.Color[2] != 255 || Cn.Color[3] != 255;
                }
            if (NoAttributes)
            {
                Mesh.Data.clear();
                C.MeshesNoAttributes++;
                Coloured = false;
            }
            if (MeshMaterial < 0 && O.Collision) C.MeshesNoMaterial++;

            for (MapMesh& Piece : SplitToLimit(Mesh, O.MaxPoints))
            {
                RenderMesh R;
                R.Label = Label;
                R.Material = MeshMaterial;
                if (O.Collision)
                    for (const int32_t T : Grid.Triangles) R.Triangles.push_back(Tris[static_cast<size_t>(T)]);
                R.ReplaceUv = Degenerate.Check(Piece);
                R.Mesh = std::move(Piece);
                C.Meshes++;
                C.MeshesColoured += Coloured ? 1 : 0;
                Out.Meshes.push_back(std::move(R));
            }
            if (Mesh.GroupCount > O.MaxPoints || Mesh.GroupSize > O.MaxPoints) C.GridsSplit++;
        }
        C.DegenerateUv += static_cast<size_t>(Degenerate.Replaced);
    }
}

std::vector<MapMesh> SplitToLimit(const MapMesh& Mesh, int MaxPoints)
{
    if (MaxPoints < 2 || (Mesh.GroupCount <= MaxPoints && Mesh.GroupSize <= MaxPoints)) return { Mesh };
    std::vector<MapMesh> Pieces;
    // pieces of up to MaxPoints points, each starting on the last row (column) of the one before so the border points are the same
    for (int G0 = 0; G0 + 1 < Mesh.GroupCount || G0 == 0; G0 += MaxPoints - 1)
    {
        const int G1 = (std::min)(G0 + MaxPoints - 1, Mesh.GroupCount - 1);
        for (int P0 = 0; P0 + 1 < Mesh.GroupSize || P0 == 0; P0 += MaxPoints - 1)
        {
            const int P1 = (std::min)(P0 + MaxPoints - 1, Mesh.GroupSize - 1);
            MapMesh Piece = Mesh;
            Piece.Points.clear();
            Piece.Data.clear();
            Piece.GroupCount = G1 - G0 + 1;
            Piece.GroupSize = P1 - P0 + 1;
            for (int G = G0; G <= G1; G++)
                for (int P = P0; P <= P1; P++)
                {
                    Piece.Points.push_back(Mesh.At(G, P));
                    if (Mesh.HasData()) Piece.Data.push_back(Mesh.DataAt(G, P));
                }
            Pieces.push_back(std::move(Piece));
            if (P1 == Mesh.GroupSize - 1) break;
        }
        if (G1 == Mesh.GroupCount - 1) break;
    }
    return Pieces;
}

RenderMeshResult BuildRenderMeshes(const RenderSoup& Soup, const std::vector<std::string>& LabelNames, const RenderMeshOptions& Options)
{
    RenderMeshResult Result;
    const size_t TriangleCount = Soup.Indices.size() / 3;
    if (TriangleCount == 0) return Result;
    // the soup must hold together: the workers index it without looking
    if (Soup.Positions.size() % 3 != 0 || Soup.Corners.size() != Soup.Positions.size() / 3 || Soup.Label.size() < TriangleCount || Soup.Front.size() < TriangleCount || Soup.Normal.size() < TriangleCount)
        throw std::invalid_argument("BuildRenderMeshes: the soup's arrays do not match");
    for (const int32_t V : Soup.Indices)
        if (V < 0 || static_cast<size_t>(V) >= Soup.Corners.size()) throw std::invalid_argument("BuildRenderMeshes: a vertex index is outside the soup");

    // the owner-wide steps: tolerance from the measured distances, exact copies with their proofs, the weld, triangles the weld collapses
    ProofList Proofs;
    std::vector<int32_t> Rep;
    if (!Options.Decal)
    {
        const NearestDistances Nearest = VertexWeld::Measure(Soup.Positions, Soup.Indices);
        const ToleranceChoice Choice = VertexWeld::ChooseTolerance(Nearest);
        Result.Count.WeldTolerance = Choice.Tolerance;
        Result.Count.WeldReason = Choice.Reason;
        Result.Count.NearestText = Nearest.ToText();
        const OverlapResult Copies = TriangleOverlap::Find(Soup.Positions, Soup.Indices, Soup.Label, Options.Collision);
        // A copy by position, winding and material is removed only when it is a copy of the render data too: the same texture coordinate and colour at each corner and the
        // same front, bit for bit. Otherwise it is another surface on the same triangle (a decal-like layer, the other side lit differently) and stays, counted.
        const auto Bits = [&](int32_t V) { return std::array<int32_t, 3>{ BitsOf(Soup.Positions[static_cast<size_t>(V) * 3]), BitsOf(Soup.Positions[static_cast<size_t>(V) * 3 + 1]), BitsOf(Soup.Positions[static_cast<size_t>(V) * 3 + 2]) }; };
        std::vector<char> Keep(TriangleCount, 0);
        for (const TriangleDuplicate& D : Copies.Duplicates)
        {
            if (!D.IsDeletable()) continue;
            int Differs = 0;   // 1 front, 2 texture coordinate, 3 colour
            if (std::memcmp(Soup.Front[static_cast<size_t>(D.Kept)].data(), Soup.Front[static_cast<size_t>(D.Removed)].data(), sizeof(double) * 3) != 0) Differs = 1;
            for (int K = 0; K < 3 && Differs == 0; K++)
            {
                const int32_t Rv = Soup.Indices[static_cast<size_t>(D.Removed) * 3 + K];
                for (int J = 0; J < 3; J++)
                {
                    const int32_t Kv = Soup.Indices[static_cast<size_t>(D.Kept) * 3 + J];
                    if (Bits(Rv) != Bits(Kv)) continue;
                    const RenderCorner &Cr = Soup.Corners[static_cast<size_t>(Rv)], &Ck = Soup.Corners[static_cast<size_t>(Kv)];
                    if (std::memcmp(&Cr.U, &Ck.U, sizeof(float)) != 0 || std::memcmp(&Cr.V, &Ck.V, sizeof(float)) != 0) Differs = 2;
                    else if (std::memcmp(Cr.Color, Ck.Color, 4) != 0) Differs = 3;
                    break;
                }
            }
            if (Differs == 0)
                Keep[static_cast<size_t>(D.Removed)] = 1;
            else
                (Differs == 1 ? Result.Count.CopiesKeptFront : Differs == 2 ? Result.Count.CopiesKeptUv : Result.Count.CopiesKeptColour)++;
        }
        for (const auto& P : Copies.Proofs(Soup.Positions, Soup.Indices))
            if (Keep[static_cast<size_t>(P.first)]) Proofs.push_back(P);
        // collision: a triangle inside a larger one of the same label and facing is removed with its proof (HydraX does this for collision); counted apart from the copies
        if (Options.Collision)
        {
            std::vector<char> Contained(TriangleCount, 0);
            for (const ContainedRemoval& R : Copies.ContainedRemovals())
                if (R.Triangle >= 0 && static_cast<size_t>(R.Triangle) < TriangleCount && (Soup.TwinMaterial.empty() || Soup.TwinMaterial[static_cast<size_t>(R.Triangle)] < 0)) Contained[static_cast<size_t>(R.Triangle)] = 1;   // a twinned triangle is never removed by containment (as a copy is not)
            size_t Before = Proofs.size();
            for (const auto& P : Copies.Proofs(Soup.Positions, Soup.Indices))
                if (Contained[static_cast<size_t>(P.first)] && !Keep[static_cast<size_t>(P.first)]) Proofs.push_back(P);
            Result.Count.ContainedRemoved = Proofs.size() - Before;
        }
        const WeldResult Weld = VertexWeld::Weld(Soup.Positions, Soup.Indices, Choice.Tolerance);
        Rep.assign(Weld.Representative.begin(), Weld.Representative.end());
        Result.Count.CopiesRemoved = Proofs.size() - Result.Count.ContainedRemoved;
        Result.Count.WeldClusters = static_cast<size_t>(Weld.Clusters);
        Result.Count.WeldedVertices = static_cast<size_t>(Weld.Welded);
        Result.Count.WeldSpan = Weld.LargestSpan;
        std::vector<char> Has(TriangleCount, 0);
        for (const auto& P : Proofs) Has[static_cast<size_t>(P.first)] = 1;
        ProofList Collapsed;
        for (size_t T = 0; T < TriangleCount; T++)
        {
            const int32_t A = Soup.Indices[T * 3], B = Soup.Indices[T * 3 + 1], C = Soup.Indices[T * 3 + 2];
            const int32_t Ra = Rep[static_cast<size_t>(A)], Rb = Rep[static_cast<size_t>(B)], Rc = Rep[static_cast<size_t>(C)];
            if (!Has[T] && (Ra == Rb || Rb == Rc || Ra == Rc) && !(A == B || B == C || A == C))
                Collapsed.emplace_back(static_cast<int>(T), "collapsed by the vertex weld: two corners are within " + std::to_string(Choice.Tolerance) + " units of each other");
        }
        Result.Count.CollapsedByWeld = Collapsed.size();
        Proofs.insert(Proofs.end(), Collapsed.begin(), Collapsed.end());
        std::sort(Proofs.begin(), Proofs.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
    }
    else
    {
        // decals are layers: overlapping ones stay; the overlap is reported only
        const OverlapResult Overlap = TriangleOverlap::Find(Soup.Positions, Soup.Indices, Soup.Label, true);
        Result.Count.OverlapPartial = Overlap.PartialOverlaps.size();
        Result.Count.OverlapTested = static_cast<size_t>(Overlap.CoplanarPairsTested);
        for (const auto& O : Overlap.PartialOverlaps) Result.Count.OverlapContained += O.IsContained() ? 1 : 0;
    }

    // the triangles of each material, in order
    std::vector<std::vector<int32_t>> ByLabel(LabelNames.size());
    for (size_t T = 0; T < TriangleCount; T++)
        if (Soup.Label[T] >= 0 && static_cast<size_t>(Soup.Label[T]) < ByLabel.size()) ByLabel[static_cast<size_t>(Soup.Label[T])].push_back(static_cast<int32_t>(T));

    std::vector<LabelOut> Outs(ByLabel.size());
    std::atomic<size_t> Next{ 0 };
    std::exception_ptr Failure;
    std::mutex FailureLock;
    const auto Work = [&]() {
        // an exception must not leave a worker (std::thread would terminate the process): the first is kept, the other workers finish, and it is thrown from here
        try
        {
            for (size_t L = Next++; L < ByLabel.size(); L = Next++)
                if (!ByLabel[L].empty()) RunLabel(Soup, static_cast<int>(L), ByLabel[L], Rep, Proofs, LabelNames[L], Options, Outs[L]);
        }
        catch (...)
        {
            const std::lock_guard<std::mutex> Guard(FailureLock);
            if (!Failure) Failure = std::current_exception();
            Next = ByLabel.size();
        }
    };
    // at most 8 workers, whatever is asked: the mesher's memory bound counts on it. This thread is one of them.
    const unsigned Cores = (std::min)(8u, Options.Threads > 0 ? static_cast<unsigned>(Options.Threads) : (std::max)(1u, std::thread::hardware_concurrency()));
    // The threads are joined by this guard on every way out (an exception included): a joinable std::thread that is destroyed ends the process.
    struct Joiner
    {
        std::vector<std::thread>& List;
        ~Joiner()
        {
            for (std::thread& T : List)
                if (T.joinable()) T.join();
        }
    };
    std::vector<std::thread> Threads;
    if (Cores > 1 && ByLabel.size() >= 2)
    {
        Threads.reserve(Cores - 1);   // before any worker starts, and so emplace_back below never reallocates (a bad_alloc there would come after a worker began)
        Joiner Guard{ Threads };
        for (unsigned I = 1; I < Cores; I++)
        {
            try
            {
                Threads.emplace_back(Work);
            }
            catch (const std::system_error&)
            {
                break;   // no more threads to be had: the ones running and this one do the rest
            }
        }
        Work();
    }
    else
        Work();
    if (Failure) std::rethrow_exception(Failure);

    const RenderMeshCounters Owner = Result.Count;
    for (LabelOut& O : Outs)
    {
        for (RenderMesh& M : O.Meshes) Result.Meshes.push_back(std::move(M));
        Result.Count.Add(O.Count);
        if (!O.Coverage.empty()) Result.CoverageLines.push_back(std::move(O.Coverage));
        if (!O.BothDiagonals.empty()) Result.BothDiagonalLines.push_back(std::move(O.BothDiagonals));
    }
    // the counters Add sums are the labels'; the owner-wide ones were set above and are not part of a label's
    Result.Count.CopiesRemoved = Owner.CopiesRemoved;
    Result.Count.CollapsedByWeld = Owner.CollapsedByWeld;
    Result.Count.WeldTolerance = Owner.WeldTolerance;
    Result.Count.WeldReason = Owner.WeldReason;
    Result.Count.NearestText = Owner.NearestText;
    Result.Count.WeldClusters = Owner.WeldClusters;
    Result.Count.WeldedVertices = Owner.WeldedVertices;
    Result.Count.WeldSpan = Owner.WeldSpan;
    Result.Count.OverlapPartial = Owner.OverlapPartial;
    Result.Count.OverlapContained = Owner.OverlapContained;
    Result.Count.OverlapTested = Owner.OverlapTested;
    return Result;
}

std::string MeshText(const MapMesh& Mesh, const std::string& Material, bool ReplaceUv, const MeshTextOptions& Options, double* MaxAbsT)
{
    std::string Out = Options.Prefix;
    Out += "  mesh\n  {\n";
    if (Options.NonColliding) Out += "  contents nonColliding;\n";
    if (!Options.Contents.empty()) Out += "  contents " + Options.Contents + ";\n";
    if (Options.ToolFlags) Out += "  toolFlags;\n";
    Out += "   " + Material + "\n   lightmap_gray\n   " + std::to_string(Mesh.GroupCount) + " " + std::to_string(Mesh.GroupSize) + " " + std::to_string(Options.LightmapSize) + " 8\n";

    V3 Ls = { 1, 0, 0 }, Lt = { 0, 1, 0 };
    if (Options.LightmapCoords)
    {
        V3 N = { 0, 0, 0 };
        for (int G = 0; G + 1 < Mesh.GroupCount; G++)
            for (int P = 0; P + 1 < Mesh.GroupSize; P++)
            {
                const V3 A = Mesh.At(G, P);
                N = Add(N, Cross(Sub(Mesh.At(G + 1, P), A), Sub(Mesh.At(G, P + 1), A)));
            }
        const double L = Length(N);
        RadiantBaseAxes(L > 1e-12 ? Mul(N, 1.0 / L) : V3{ 0, 0, 1 }, Ls, Lt);
    }

    // the running coordinate: 16 texels per unit along the grid, V negated
    double RunU = 0;
    for (int G = 0; G < Mesh.GroupCount; G++)
    {
        if (G > 0) RunU += Length(Sub(Mesh.At(G, 0), Mesh.At(G - 1, 0))) * 16;
        Out += "   (\n";
        double RunV = 0;
        for (int P = 0; P < Mesh.GroupSize; P++)
        {
            if (P > 0) RunV += Length(Sub(Mesh.At(G, P), Mesh.At(G, P - 1))) * 16;
            const V3& Pt = Mesh.At(G, P);
            Out += "\tv " + RadiantNumber(Pt[0]) + " " + RadiantNumber(Pt[1]) + " " + RadiantNumber(Pt[2]);
            double U = RunU, V = -RunV;
            if (Mesh.HasData())
            {
                const MeshPointData& D = Mesh.DataAt(G, P);
                const uint8_t Colour[4] = { D.R, D.G, D.B, D.A };
                // a vertex without 'c' compiles to white, so only a non-white vertex has one
                Out += PatchColour(Colour);
                if (!ReplaceUv) U = D.U * 1024.0, V = D.V * 1024.0;
            }
            else if (G == 0 && P == 0)
            {
                Out += " c 255 255 255 255";
            }
            if (MaxAbsT != nullptr) *MaxAbsT = (std::max)(*MaxAbsT, (std::max)(std::fabs(U), std::fabs(V)));
            Out += " t " + RadiantNumber(U) + " " + RadiantNumber(V);
            if (Options.LightmapCoords)
                Out += " " + RadiantNumber(Dot(Pt, Ls) / 16.0) + " " + RadiantNumber(Dot(Pt, Lt) / 16.0) + "\n";
            else
                Out += " 0 0\n";
        }
        Out += "   )\n";
    }
    if (!Options.DecalLayer.empty()) Out += "   decalLayerSort \"" + Options.DecalLayer + "\"\n   decalEditorSort -1\n";
    Out += "  }\n";
    return Out;
}
}
