#include "stdafx.h"

#include "MapMesh.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

// Self-test of the map mesh module. Every case of HydraX's tests/HydraX.Formats.Tests/Map QuadMesherTests, PatchCoverageTests,
// TriangleOverlapTests, VertexWeldTests, BrushFaceUvTests, PatchModelsTests and (the ones that do not go through the writer)
// DegenerateMeshUvTests is here with the same expected values, then checks of what the port changed (scale structures against the
// plain HydraX loops, table-seed independence). docs/hydrax-port/m2a.md maps each case to its HydraX test.
namespace Maps
{
namespace
{
    using QuadMesher::BuildOptions;
    using QuadMesher::BuiltGrid;

    struct Soup
    {
        std::vector<float> P;
        std::vector<int32_t> I;
    };

    // A (columns x rows) cell grid on z = 0, counter-clockwise, split along the same diagonal in every cell
    Soup GridSoup(int Columns, int Rows)
    {
        Soup S;
        for (int J = 0; J <= Rows; J++)
            for (int I = 0; I <= Columns; I++)
            {
                S.P.push_back(I * 8.0f);
                S.P.push_back(J * 8.0f);
                S.P.push_back(0.0f);
            }
        const auto At = [&](int I, int J) { return (J * (Columns + 1)) + I; };
        for (int J = 0; J < Rows; J++)
            for (int I = 0; I < Columns; I++)
                for (const int V : { At(I, J), At(I + 1, J), At(I + 1, J + 1), At(I, J), At(I + 1, J + 1), At(I, J + 1) }) S.I.push_back(V);
        return S;
    }

    // A fan of triangles around a pivot, counter-clockwise from above. The rim points alternate above and below the plane, so neighbouring triangles are folded against
    // each other and never form a convex quad (the mesher leaves them lone). The pivot is the first new vertex, then the rim; triangle i is (pivot, rim i, rim i+1).
    void AddFan(Soup& S, int Triangles, float StepDegrees, float Zig, float OffsetX)
    {
        const int Pivot = static_cast<int>(S.P.size() / 3);
        for (const float V : { OffsetX, 0.0f, 0.0f }) S.P.push_back(V);
        for (int J = 0; J <= Triangles; J++)
        {
            const float Step = static_cast<float>(J) * StepDegrees;
            const double Angle = Step * std::acos(-1.0) / 180;
            S.P.push_back(OffsetX + static_cast<float>(8 * std::cos(Angle)));
            S.P.push_back(static_cast<float>(8 * std::sin(Angle)));
            S.P.push_back((J % 2 == 0) ? Zig : -Zig);
        }
        for (int I = 0; I < Triangles; I++)
            for (const int V : { Pivot, Pivot + 1 + I, Pivot + 2 + I }) S.I.push_back(V);
    }

    Soup FanSoup(int Triangles, float StepDegrees = 30.0f, float Zig = 3.0f)
    {
        Soup S;
        AddFan(S, Triangles, StepDegrees, Zig, 0.0f);
        return S;
    }

    std::vector<int32_t> Zeros(size_t N) { return std::vector<int32_t>(N, 0); }

    std::vector<BuiltGrid> Build(const Soup& S, const std::vector<int32_t>& Labels, const BuildOptions& O = {}) { return QuadMesher::Build(S.P, S.I, Labels, O).Grids; }

    size_t PointTotal(const QuadGrid& G) { return G.Points.size(); }

    std::set<int32_t> Distinct(const QuadGrid& G) { return std::set<int32_t>(G.Points.begin(), G.Points.end()); }

    std::vector<int32_t> Sorted(std::vector<int32_t> V)
    {
        std::sort(V.begin(), V.end());
        return V;
    }

    std::vector<int32_t> Row(const QuadGrid& G, int Group)
    {
        const auto R = G.Group(Group);
        return std::vector<int32_t>(R.begin(), R.end());
    }

    std::vector<QuadGrid> Plain(const std::vector<BuiltGrid>& Grids)
    {
        std::vector<QuadGrid> Out;
        for (const BuiltGrid& G : Grids) Out.push_back(G.Grid);
        return Out;
    }

    bool Near(double A, double B, double Tolerance) { return std::fabs(A - B) <= Tolerance; }

    bool Contains(const std::string& Haystack, const std::string& Needle) { return Haystack.find(Needle) != std::string::npos; }

    // A small deterministic generator for the property checks
    struct Lcg
    {
        uint64_t State;
        uint32_t Next()
        {
            State = (State * 6364136223846793005ull) + 1442695040888963407ull;
            return static_cast<uint32_t>(State >> 33);
        }
        double Unit() { return Next() / 2147483648.0; }
        int Below(int N) { return static_cast<int>(Next() % static_cast<uint32_t>(N)); }
    };

    // The HydraX loop that cuts laid-out cells into rectangles, as it is written there (a rescan of every corner each round)
    std::vector<MeshInternal::CellRect> NaiveCut(const std::vector<std::pair<int, int>>& Cells)
    {
        std::set<std::pair<int, int>> Remaining(Cells.begin(), Cells.end());
        std::vector<MeshInternal::CellRect> Out;
        const auto Has = [&](int X, int Y) { return Remaining.count({ X, Y }) != 0; };
        while (!Remaining.empty())
        {
            MeshInternal::CellRect Best{ 0, 0, 0, 0 };
            std::vector<std::pair<int, int>> Starts;   // (Y, X)
            for (const auto& [X, Y] : Remaining)
                if (!Has(X - 1, Y) && !Has(X, Y - 1)) Starts.emplace_back(Y, X);
            std::sort(Starts.begin(), Starts.end());
            for (const auto& [Y, X] : Starts)
            {
                int Wide = 1;
                while (Has(X + Wide, Y)) Wide++;
                int High = 1;
                for (;;)
                {
                    bool All = true;
                    for (int I = 0; I < Wide; I++) All = All && Has(X + I, Y + High);
                    if (!All) break;
                    High++;
                }
                int Tall = 1;
                while (Has(X, Y + Tall)) Tall++;
                int Broad = 1;
                for (;;)
                {
                    bool All = true;
                    for (int J = 0; J < Tall; J++) All = All && Has(X + Broad, Y + J);
                    if (!All) break;
                    Broad++;
                }
                if (Wide * High > Best.W * Best.H) Best = { X, Y, Wide, High };
                if (Broad * Tall > Best.W * Best.H) Best = { X, Y, Broad, Tall };
            }
            for (int I = 0; I < Best.W; I++)
                for (int J = 0; J < Best.H; J++) Remaining.erase({ Best.X + I, Best.Y + J });
            Out.push_back(Best);
        }
        return Out;
    }

    // A jittered grid with holes, two labels, random diagonals, a fan, noise triangles and exact copies: every kind of input the mesher takes
    Soup MixedSoup(uint64_t Seed, int N)
    {
        Lcg R{ Seed };
        Soup S;
        for (int J = 0; J <= N; J++)
            for (int I = 0; I <= N; I++)
            {
                S.P.push_back(I * 8.0f + static_cast<float>(R.Unit()) * 0.2f);
                S.P.push_back(J * 8.0f + static_cast<float>(R.Unit()) * 0.2f);
                S.P.push_back(static_cast<float>(R.Unit()) * 0.1f);
            }
        const auto At = [&](int I, int J) { return (J * (N + 1)) + I; };
        for (int J = 0; J < N; J++)
            for (int I = 0; I < N; I++)
            {
                if (R.Below(10) == 0) continue;   // a hole
                if (R.Below(2) == 0)
                    for (const int V : { At(I, J), At(I + 1, J), At(I + 1, J + 1), At(I, J), At(I + 1, J + 1), At(I, J + 1) }) S.I.push_back(V);
                else
                    for (const int V : { At(I, J), At(I + 1, J), At(I, J + 1), At(I + 1, J), At(I + 1, J + 1), At(I, J + 1) }) S.I.push_back(V);
            }
        AddFan(S, 9, 30.0f, 3.0f, 1000.0f);
        for (int K = 0; K < N * 2; K++)   // noise: three fresh vertices each, anywhere
        {
            const int Base = static_cast<int>(S.P.size() / 3);
            for (int V = 0; V < 3; V++)
            {
                S.P.push_back(static_cast<float>(R.Unit()) * 300.0f);
                S.P.push_back(static_cast<float>(R.Unit()) * 300.0f);
                S.P.push_back(static_cast<float>(R.Unit()) * 20.0f);
            }
            for (int V = 0; V < 3; V++) S.I.push_back(Base + V);
        }
        const size_t Before = S.I.size();
        for (int K = 0; K < 5 && Before >= 3; K++)   // exact copies on their own vertices
        {
            const size_t T = static_cast<size_t>(R.Below(static_cast<int>(Before / 3)));
            const int Base = static_cast<int>(S.P.size() / 3);
            for (int V = 0; V < 3; V++)
            {
                const size_t Src = static_cast<size_t>(S.I[T * 3 + V]) * 3;
                for (size_t C = 0; C < 3; C++) S.P.push_back(S.P[Src + C]);
                S.I.push_back(Base + V);
            }
        }
        return S;
    }

    std::vector<int32_t> QuadrantLabels(const Soup& S)
    {
        std::vector<int32_t> L(S.I.size() / 3);
        for (size_t T = 0; T < L.size(); T++) L[T] = S.P[static_cast<size_t>(S.I[T * 3]) * 3] < 40.0f ? 0 : 1;
        return L;
    }

    // The grids as text, the same layout the HydraX C# cross-check prints (Separator replaces the newline)
    std::string Canon(const QuadMesher::BuildResult& B, const char* Separator)
    {
        std::string Out = "grids " + std::to_string(B.Grids.size()) + " order " + std::to_string(B.OrderScored) + "/" + std::to_string(B.OrderCut) + Separator;
        for (const BuiltGrid& G : B.Grids)
        {
            Out += "G " + std::to_string(G.Group) + (G.Grid.IsFan ? " fan" : G.Grid.IsTriangle ? " tri" : " mesh") + " " + std::to_string(G.Grid.GroupCount) + "x" + std::to_string(G.Grid.GroupSize) + " pts";
            for (const int32_t V : G.Grid.Points) Out += " " + std::to_string(V);
            Out += " tris";
            for (const int32_t T : G.Grid.Triangles) Out += " " + std::to_string(T);
            Out += Separator;
        }
        return Out;
    }

    uint64_t Fnv(const std::string& S)
    {
        uint64_t H = 14695981039346656037ull;
        for (const char C : S)
        {
            H ^= static_cast<uint8_t>(C);
            H *= 1099511628211ull;
        }
        return H;
    }

    // A 14 x 14 cell grid with holes, three labels, mixed diagonals and the vertex ids scattered (id * 37 mod 225), so the vertex-order models score and cut.
    // Built by integer formulas so the HydraX C# builds the same soup (docs/hydrax-port/m2a.md).
    Soup GoldenSoup(std::vector<int32_t>& Labels)
    {
        constexpr int N = 14, V = (N + 1) * (N + 1);
        const auto Vid = [&](int I, int J) { return ((J * (N + 1)) + I) * 37 % V; };
        Soup S;
        S.P.assign(static_cast<size_t>(V) * 3, 0.0f);
        for (int J = 0; J <= N; J++)
            for (int I = 0; I <= N; I++)
            {
                const size_t Id = static_cast<size_t>(Vid(I, J));
                S.P[Id * 3] = (I * 8.0f) + (static_cast<float>(((I * 7) + (J * 13)) % 10) * 0.05f);
                S.P[Id * 3 + 1] = (J * 8.0f) + (static_cast<float>(((I * 11) + (J * 5)) % 10) * 0.05f);
                S.P[Id * 3 + 2] = static_cast<float>(((I * 3) + (J * 7)) % 5) * 0.04f;
            }
        for (int J = 0; J < N; J++)
            for (int I = 0; I < N; I++)
            {
                if (((I * 5) + (J * 3)) % 17 == 0) continue;
                const int A = Vid(I, J), B = Vid(I + 1, J), C = Vid(I + 1, J + 1), D = Vid(I, J + 1);
                if (I * J % 3 == 0)
                    for (const int X : { A, B, C, A, C, D }) S.I.push_back(X);
                else
                    for (const int X : { A, B, D, B, C, D }) S.I.push_back(X);
                Labels.push_back(I / 5);
                Labels.push_back(I / 5);
            }
        return S;
    }

    bool SameGrids(const std::vector<BuiltGrid>& A, const std::vector<BuiltGrid>& B)
    {
        if (A.size() != B.size()) return false;
        for (size_t I = 0; I < A.size(); I++)
            if (A[I].Group != B[I].Group || A[I].Grid.GroupCount != B[I].Grid.GroupCount || A[I].Grid.GroupSize != B[I].Grid.GroupSize || A[I].Grid.Points != B[I].Grid.Points || A[I].Grid.Triangles != B[I].Grid.Triangles
                || A[I].Grid.IsFan != B[I].Grid.IsFan || A[I].Grid.IsTriangle != B[I].Grid.IsTriangle)
                return false;
        return true;
    }

    // ---- the plain HydraX loops, as references for the scale structures

    NearestDistances PlainMeasure(const std::vector<float>& P, const std::vector<int32_t>& Indices)
    {
        const std::vector<double> Edges = { 0.0001, 0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 4, 8, 16 };
        std::set<int32_t> UsedSet;
        for (const int32_t V : Indices)
            if (V >= 0 && static_cast<size_t>(V) * 3 + 2 < P.size()) UsedSet.insert(V);
        const double Cell = Edges.back();
        const auto Pt = [&](int V) { return RadiantGeometry::V3{ P[static_cast<size_t>(V) * 3], P[static_cast<size_t>(V) * 3 + 1], P[static_cast<size_t>(V) * 3 + 2] }; };
        NearestDistances R;
        R.Vertices = static_cast<int>(UsedSet.size());
        R.Edges = Edges;
        R.Counts.assign(Edges.size() + 1, 0);
        R.Maximum = Cell;
        for (const int32_t V : UsedSet)
        {
            const auto A = Pt(V);
            double Best = 1e300;
            bool Same = false;
            for (const int32_t O : UsedSet)
            {
                if (O == V) continue;
                const auto B = Pt(O);
                // only the 27 cells around the vertex are searched
                bool Near27 = true;
                for (size_t K = 0; K < 3; K++) Near27 = Near27 && std::abs(static_cast<int>(std::floor(A[K] / Cell)) - static_cast<int>(std::floor(B[K] / Cell))) <= 1;
                if (!Near27) continue;
                const double D = RadiantGeometry::Length(RadiantGeometry::Sub(B, A));
                if (D == 0)
                    Same = true;
                else if (D < Best)
                    Best = D;
            }
            if (Same) R.SharedPosition++;
            size_t Bucket = Edges.size();
            for (size_t I = 0; I < Edges.size(); I++)
                if (Best <= Edges[I])
                {
                    Bucket = I;
                    break;
                }
            R.Counts[Bucket]++;
        }
        return R;
    }

    WeldResult PlainWeld(const std::vector<float>& P, const std::vector<int32_t>& Indices, double Tolerance)
    {
        const size_t Count = P.size() / 3;
        std::vector<int32_t> Parent(Count);
        for (size_t I = 0; I < Count; I++) Parent[I] = static_cast<int32_t>(I);
        const auto Find = [&](int32_t V) {
            while (Parent[static_cast<size_t>(V)] != V) V = Parent[static_cast<size_t>(V)];
            return V;
        };
        std::set<int32_t> UsedSet;
        for (const int32_t V : Indices)
            if (V >= 0 && static_cast<size_t>(V) < Count) UsedSet.insert(V);
        const auto Pt = [&](int V) { return RadiantGeometry::V3{ P[static_cast<size_t>(V) * 3], P[static_cast<size_t>(V) * 3 + 1], P[static_cast<size_t>(V) * 3 + 2] }; };
        for (const int32_t V : UsedSet)
            for (const int32_t O : UsedSet)
                if (O > V && RadiantGeometry::Length(RadiantGeometry::Sub(Pt(O), Pt(V))) <= Tolerance)
                {
                    const int32_t A = Find(V), B = Find(O);
                    if (A != B) Parent[static_cast<size_t>((std::max)(A, B))] = (std::min)(A, B);
                }
        WeldResult R;
        R.Representative.resize(Count);
        for (size_t V = 0; V < Count; V++) R.Representative[V] = Find(static_cast<int32_t>(V));
        std::map<int32_t, std::vector<int32_t>> Clusters;
        for (const int32_t V : UsedSet) Clusters[R.Representative[static_cast<size_t>(V)]].push_back(V);
        for (const auto& [Root, Members] : Clusters)
        {
            if (Members.size() < 2) continue;
            R.Clusters++;
            R.Welded += static_cast<int>(Members.size()) - 1;
            for (size_t A = 0; A < Members.size(); A++)
                for (size_t B = A + 1; B < Members.size() && Members.size() <= 64; B++)
                    R.LargestSpan = (std::max)(R.LargestSpan, RadiantGeometry::Length(RadiantGeometry::Sub(Pt(Members[A]), Pt(Members[B]))));
        }
        return R;
    }
}

int MeshSelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* Name, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::format("FAIL MapMesh {} (got '{}')\n", Name, Got);
    };
    const auto Num = [](double V) { return std::format("{}", V); };
    const auto AtV = [](const std::vector<float>& P) {
        return [&P](int V) { return RadiantGeometry::V3{ P[static_cast<size_t>(V) * 3], P[static_cast<size_t>(V) * 3 + 1], P[static_cast<size_t>(V) * 3 + 2] }; };
    };

    // ======== QuadMesherTests ========
    {
        const Soup S = GridSoup(2, 2);
        // Points run along +x inside a group, groups run along +y: (0,1,0) x (1,0,0) points down.
        const QuadGrid Grid = QuadGrid::FromGroups({ { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 } });
        const double Z = QuadMesher::GridFacing(Grid, AtV(S.P))[2];
        const QuadGrid Reversed = QuadMesher::Reversed(Grid);
        const double ZRev = QuadMesher::GridFacing(Reversed, AtV(S.P))[2];
        Check(Z < 0 && ZRev > 0 && Row(Reversed, 0) == std::vector<int32_t>({ 6, 7, 8 }), "QuadMesher: a grid's facing is group direction cross point direction and reversing flips it", Num(Z) + " " + Num(ZRev));
    }
    {
        const Soup S = GridSoup(3, 2);
        const auto Grids = Build(S, Zeros(S.I.size() / 3));
        const bool One = Grids.size() == 1;
        const QuadGrid& G = Grids.empty() ? QuadGrid() : Grids[0].Grid;
        Check(One && !G.IsTriangle && PointTotal(G) == 12 && (G.GroupCount == 3 || G.GroupCount == 4) && Distinct(G).size() == 12, "QuadMesher: a regular grid becomes one mesh with every point", std::to_string(Grids.size()));
    }
    {
        const Soup S = GridSoup(1, 1);
        const auto Grids = Build(S, { 0, 1 });
        Check(Grids.size() == 2 && Grids[0].Grid.IsTriangle && Grids[1].Grid.IsTriangle, "QuadMesher: triangles with different labels never join", std::to_string(Grids.size()));
    }
    {
        const Soup S = GridSoup(2, 1);
        const auto Grids = Build(S, Zeros(S.I.size() / 3));
        Check(Grids.size() == 1 && Sorted(Grids[0].Grid.Triangles) == std::vector<int32_t>({ 0, 1, 2, 3 }), "QuadMesher: a grid reports the triangles it was made from", std::to_string(Grids.size()));
    }
    {
        // Two cells side by side; the left cell is label 0, the right one label 1: they share an edge but must not become one mesh.
        const Soup S = GridSoup(2, 1);
        const auto Grids = Build(S, { 0, 0, 1, 1 });
        std::vector<int> Labels;
        bool Four = true;
        for (const BuiltGrid& G : Grids)
        {
            Labels.push_back(G.Group);
            Four = Four && PointTotal(G.Grid) == 4;
        }
        std::sort(Labels.begin(), Labels.end());
        Check(Grids.size() == 2 && Labels == std::vector<int>({ 0, 1 }) && Four, "QuadMesher: a grid never spans two labels", std::to_string(Grids.size()));
    }
    {
        Soup S = GridSoup(2, 2);
        // Drop the two triangles of the cell in the upper right corner (cells run row by row: 0, 1 / 2, 3).
        S.I.erase(S.I.begin() + 18, S.I.begin() + 24);
        const auto Grids = Build(S, Zeros(S.I.size() / 3));
        int Cells = 0;
        for (const BuiltGrid& G : Grids) Cells += (G.Grid.GroupCount - 1) * (G.Grid.GroupSize - 1);
        Check(Grids.size() == 2 && Cells == 3, "QuadMesher: an L shaped region is cut into the fewest rectangles from its corner", std::to_string(Grids.size()) + " " + std::to_string(Cells));
    }
    {
        const Soup S = GridSoup(4, 1);
        const auto Whole = Build(S, Zeros(S.I.size() / 3));
        // Triangles 0-3 are the two left cells, 4-7 the two right ones: the edge between triangle 3 and triangle 4 is reported as a border.
        BuildOptions O;
        O.IsBorder = [](int A, int B, int, int) { return (A <= 3) != (B <= 3); };
        const auto Cut = Build(S, Zeros(S.I.size() / 3), O);
        bool Halves = Cut.size() == 2;
        for (const BuiltGrid& G : Cut) Halves = Halves && G.Grid.Triangles.size() / 2 == 2;
        Check(Whole.size() == 1 && PointTotal(Whole[0].Grid) / 2 == 5 && Halves, "QuadMesher: a border the caller reports stops the grid there", std::to_string(Whole.size()) + " " + std::to_string(Cut.size()));
    }
    {
        // Cells with very different widths and a jittered top row: the pairs across a short side are as rectangular as the true diagonals, so a greedy pairing strands triangles.
        const float Xs[] = { 0, 1, 9, 10, 30, 31 };
        Soup S;
        for (int J = 0; J <= 2; J++)
            for (int I = 0; I < 6; I++)
            {
                S.P.push_back(Xs[I] + (static_cast<float>(J) * 0.3f));
                S.P.push_back(static_cast<float>(J) * 2.0f);
                S.P.push_back(static_cast<float>(((I * 7) + (J * 3)) % 5) * 0.2f);
            }
        const auto At = [](int I, int J) { return (J * 6) + I; };
        for (int J = 0; J < 2; J++)
            for (int I = 0; I < 5; I++)
            {
                if (((I + J) % 2) == 0)
                    for (const int V : { At(I, J), At(I + 1, J), At(I + 1, J + 1), At(I, J), At(I + 1, J + 1), At(I, J + 1) }) S.I.push_back(V);
                else
                    for (const int V : { At(I, J), At(I + 1, J), At(I, J + 1), At(I + 1, J), At(I + 1, J + 1), At(I, J + 1) }) S.I.push_back(V);
            }
        const auto Grids = Build(S, Zeros(S.I.size() / 3));
        bool AnyTriangle = false;
        size_t Total = 0;
        for (const BuiltGrid& G : Grids)
        {
            AnyTriangle = AnyTriangle || G.Grid.IsTriangle;
            Total += G.Grid.Triangles.size();
        }
        Check(!AnyTriangle && Total == S.I.size() / 3, "QuadMesher: a skewed grid leaves no triangle without a partner", std::to_string(Total));
    }
    {
        Soup S = GridSoup(3, 1);
        // Reverse the winding of the middle cell's two triangles (triangles 2 and 3).
        for (size_t T = 2; T <= 3; T++) std::swap(S.I[(T * 3) + 1], S.I[(T * 3) + 2]);
        const auto Grids = Build(S, Zeros(S.I.size() / 3));
        Check(Grids.size() == 1 && PointTotal(Grids[0].Grid) == 8, "QuadMesher: a cell wound against its neighbours still joins the grid", std::to_string(Grids.size()));
    }
    {
        const auto Result = QuadMesher::Build(std::vector<float>{ 0, 0, 0, 1, 0, 0, 0, 1, 0 }, std::vector<int32_t>{ 0, 1, 2 }, std::vector<int32_t>{ 0 }).Grids;
        Check(Result.size() == 1 && Result[0].Grid.IsTriangle && Result[0].Grid.GroupCount == 2 && Result[0].Grid.At(1, 0) == Result[0].Grid.At(1, 1), "QuadMesher: a lone triangle is a degenerate two by two mesh", std::to_string(Result.size()));
    }
    {
        const Soup S = FanSoup(7);
        // The input order does not matter: the triangles are handed over backwards.
        Soup Back;
        Back.P = S.P;
        for (int T = 6; T >= 0; T--) Back.I.insert(Back.I.end(), S.I.begin() + T * 3, S.I.begin() + T * 3 + 3);
        const auto Grids = Build(Back, Zeros(7));
        const bool One = Grids.size() == 1;
        const QuadGrid& G = One ? Grids[0].Grid : QuadGrid();
        Check(One && G.IsFan && !G.IsTriangle && G.GroupCount == 2 && Row(G, 0) == std::vector<int32_t>({ 1, 2, 3, 4, 5, 6, 7, 8 }) && Row(G, 1) == std::vector<int32_t>(8, 0) && G.Triangles.size() == 7,
              "QuadMesher: seven lone triangles around a vertex become one two by eight mesh", std::to_string(Grids.size()));
        if (One)
        {
            const CoverageResult C = PatchCoverage::Check(Back.P, Back.I, std::vector<QuadGrid>{ G });
            Check(C.IsComplete() && C.Covered == 7 && C.Meshes == 1 && C.LoneTriangles == 0 && Near(C.InputArea, C.OutputArea, 1e-6), "QuadMesher: the fan's area is kept and coverage is complete", C.Summary("fan"));
            // Same facing convention as a lone triangle: against the triangles' winding (OrientToRender then turns it where the render data says so).
            RadiantGeometry::V3 Wound = { 0, 0, 0 };
            const auto At = AtV(S.P);
            for (int T = 0; T < 7; T++)
                Wound = RadiantGeometry::Add(Wound, RadiantGeometry::Cross(RadiantGeometry::Sub(At(S.I[T * 3 + 1]), At(S.I[T * 3])), RadiantGeometry::Sub(At(S.I[T * 3 + 2]), At(S.I[T * 3]))));
            Check(RadiantGeometry::Dot(QuadMesher::GridFacing(G, AtV(S.P)), Wound) < 0, "QuadMesher: a fan faces against the triangles' winding");
        }
    }
    {
        const Soup S = FanSoup(7);
        // Triangles 0-3 on one side of the texture seam, 4-6 on the other.
        BuildOptions O;
        O.IsBorder = [](int A, int B, int, int) { return (A <= 3) != (B <= 3); };
        const auto Grids = Build(S, Zeros(7), O);
        bool Fans = true;
        for (const BuiltGrid& G : Grids) Fans = Fans && G.Grid.IsFan;
        const bool Two = Grids.size() == 2;
        Check(Two && Grids[0].Grid.Triangles.size() == 4 && Grids[1].Grid.Triangles.size() == 3 && Fans && Row(Grids[0].Grid, 0) == std::vector<int32_t>({ 1, 2, 3, 4, 5 }) && Row(Grids[1].Grid, 0) == std::vector<int32_t>({ 5, 6, 7, 8 })
                  && PatchCoverage::Check(S.P, S.I, Plain(Grids)).IsComplete(),
              "QuadMesher: a fan is cut where the caller reports a border on a shared edge", std::to_string(Grids.size()));
    }
    {
        Soup S;
        AddFan(S, 5, 30.0f, 3.0f, 0.0f);
        AddFan(S, 4, 30.0f, 3.0f, 100.0f);
        const auto Grids = Build(S, Zeros(9));
        bool Fans = true;
        std::vector<size_t> Counts;
        std::set<int32_t> Pivots;
        for (const BuiltGrid& G : Grids)
        {
            Fans = Fans && G.Grid.IsFan;
            Counts.push_back(G.Grid.Triangles.size());
            Pivots.insert(G.Grid.At(1, 0));
        }
        std::sort(Counts.begin(), Counts.end());
        const CoverageResult C = PatchCoverage::Check(S.P, S.I, Plain(Grids));
        Check(Grids.size() == 2 && Fans && Counts == std::vector<size_t>({ 4, 5 }) && Pivots.size() == 2 && C.IsComplete() && Near(C.InputArea, C.OutputArea, 1e-6), "QuadMesher: two fans with different pivots stay two meshes", std::to_string(Grids.size()));
    }
    {
        Soup S = FanSoup(7);
        // Triangle 3 is missing: 0-2 and 4-6 touch the pivot but are not edge-adjacent.
        S.I.erase(S.I.begin() + 9, S.I.begin() + 12);
        const auto Grids = Build(S, Zeros(6));
        std::vector<std::vector<int32_t>> Sets;
        bool Fans = true;
        for (const BuiltGrid& G : Grids)
        {
            Fans = Fans && G.Grid.IsFan;
            Sets.push_back(Sorted(G.Grid.Triangles));
        }
        std::sort(Sets.begin(), Sets.end());
        Check(Grids.size() == 2 && Fans && Sets == std::vector<std::vector<int32_t>>({ { 0, 1, 2 }, { 3, 4, 5 } }) && PatchCoverage::Check(S.P, S.I, Plain(Grids)).IsComplete(), "QuadMesher: a gap in the fan is not bridged", std::to_string(Grids.size()));
    }
    {
        const Soup S = FanSoup(5);
        // Labels 0 0 1 0 0: the middle triangle cannot join, so two pairs and one lone triangle remain.
        const auto Grids = Build(S, { 0, 0, 1, 0, 0 });
        int Fans = 0, Lone = 0;
        size_t Total = 0;
        for (const BuiltGrid& G : Grids)
        {
            Fans += G.Grid.IsFan ? 1 : 0;
            Lone += G.Grid.IsTriangle ? 1 : 0;
            Total += G.Grid.Triangles.size();
        }
        Check(Fans == 2 && Lone == 1 && Total == 5, "QuadMesher: a triangle of another label splits the fan and a single triangle stays lone", std::to_string(Fans) + " " + std::to_string(Lone));
    }
    {
        const Soup S = GridSoup(1, 1);
        const auto Grids = Build(S, Zeros(2));
        Check(Grids.size() == 1 && !Grids[0].Grid.IsFan && !Grids[0].Grid.IsTriangle && PointTotal(Grids[0].Grid) == 4 && Distinct(Grids[0].Grid).size() == 4, "QuadMesher: a triangle pair that pairs into a quad is not made a fan", std::to_string(Grids.size()));
    }
    {
        // Twelve triangles of 30 degrees close the ring: the rim would repeat its first point.
        Soup Ring = FanSoup(12);
        Ring.I.back() = 1;
        const auto Rings = Build(Ring, Zeros(12));
        bool Distinct0 = true;
        size_t Total = 0;
        for (const BuiltGrid& G : Rings)
        {
            const auto Row0 = Row(G.Grid, 0);
            Distinct0 = Distinct0 && std::set<int32_t>(Row0.begin(), Row0.end()).size() == Row0.size();
            Total += G.Grid.Triangles.size();
        }
        Check(Distinct0 && Total == 12, "QuadMesher: a closed ring of triangles is not one fan", std::to_string(Total));

        const Soup Long = FanSoup(120, 0.5f, 0.05f);
        const auto Grids = Build(Long, Zeros(120));
        std::vector<size_t> Counts;
        bool Short = true;
        for (const BuiltGrid& G : Grids)
        {
            Counts.push_back(G.Grid.Triangles.size());
            Short = Short && G.Grid.GroupSize <= 100;
        }
        std::sort(Counts.begin(), Counts.end());
        Check(Counts == std::vector<size_t>({ 21, 99 }) && Short && PatchCoverage::Check(Long.P, Long.I, Plain(Grids)).IsComplete(), "QuadMesher: a long fan is cut at one hundred points", std::to_string(Counts.size()));
    }

    // ======== PatchCoverageTests ========
    {
        const Soup S = GridSoup(3, 2);
        const CoverageResult R = PatchCoverage::Check(S.P, S.I, Plain(Build(S, Zeros(S.I.size() / 3))));
        Check(R.IsComplete() && R.InputTriangles == 12 && R.Covered == 12 && R.Cells == 6 && Near(R.InputArea, 6 * 64.0, 1e-6) && Near(R.InputArea, R.OutputArea, 1e-6), "PatchCoverage: a rebuilt grid covers every triangle and the area matches", R.Summary("grid"));
    }
    {
        const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 0, 8, 0 };
        const std::vector<int32_t> I = { 0, 1, 2 };
        const CoverageResult R = PatchCoverage::Check(P, I, Plain(QuadMesher::Build(P, I, std::vector<int32_t>{ 0 }).Grids));
        Check(R.IsComplete() && R.LoneTriangles == 1 && Near(R.OutputArea, 32.0, 1e-6), "PatchCoverage: a lone triangle is covered by its degenerate cell", R.Summary("lone"));
    }
    {
        const Soup S = GridSoup(2, 1);
        const auto Grids = Plain(Build(S, Zeros(S.I.size() / 3)));
        const QuadGrid& G = Grids[0];
        const QuadGrid Cut = QuadGrid::FromGroups({ { G.At(0, 0), G.At(0, 1) }, { G.At(1, 0), G.At(1, 1) } });
        const CoverageResult R = PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{ Cut });
        bool Words = true;
        for (const CoverageLine& L : R.Lines)
            if (L.Kind == "missing") Words = Words && Contains(L.Reason, "no output cell has the three corners");
        Check(!R.IsComplete() && R.Missing == 2 && R.Covered == 2 && Words && R.OutputArea < R.InputArea, "PatchCoverage: a dropped cell is reported missing with its corners and the area gap shows", R.Summary("cut"));
    }
    {
        const Soup S = GridSoup(1, 1);
        const QuadGrid Grid = Build(S, Zeros(2))[0].Grid;
        const CoverageResult Twice = PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{ Grid, Grid });
        // A quad whose halves are not input triangles.
        const CoverageResult Stray = PatchCoverage::Check(S.P, std::vector<int32_t>{ 0, 1, 3 }, std::vector<QuadGrid>{ Grid });
        Check(Twice.Leftover == 2 && Twice.Missing == 0 && Stray.Missing == 0 && Stray.Leftover >= 1, "PatchCoverage: a cell used twice and a cell with no input are leftovers", Twice.Summary("twice") + " | " + Stray.Summary("stray"));
    }
    {
        const Soup S = GridSoup(1, 1);
        const ProofList Removed = { { 1, "exact duplicate of triangle 0" } };
        const CoverageResult R = PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{}, Removed);
        bool Listed = false;
        for (const CoverageLine& L : R.Lines) Listed = Listed || (L.Kind == "removed" && Contains(L.Reason, "duplicate"));
        Check(R.Removed == 1 && R.Missing == 1 && Listed && Near(R.RemovedArea, 32.0, 1e-6), "PatchCoverage: a triangle removed on purpose is listed with its proof and not missing", R.Summary("removed"));
    }
    {
        const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 16, 0, 0 };
        const CoverageResult R = PatchCoverage::Check(P, std::vector<int32_t>{ 0, 1, 2 }, std::vector<QuadGrid>{});
        Check(R.ZeroAreaInput == 1 && R.Lines.size() == 1 && Contains(R.Lines[0].Reason, "zero-area") && Contains(R.Summary("test"), "MISSING 1"), "PatchCoverage: a zero area triangle is counted and its missing line says so", R.Summary("zero"));
    }
    {
        // VertexWeldTests: coverage compares by welded ids and a removed twin does not stand in for the kept one
        const std::vector<int32_t> Indices = { 0, 1, 2, 0, 1, 2 };
        const QuadGrid Grid = QuadGrid::FromGroups({ { 0, 0 }, { 1, 2 } }, true);
        const std::vector<int32_t> Weld = { 0, 1, 2 };
        const CoverageResult R = PatchCoverage::Check(std::vector<float>{ 0, 0, 0, 8, 0, 0, 0, 8, 0 }, Indices, std::vector<QuadGrid>{ Grid }, ProofList{ { 0, "test twin" } }, Weld);
        Check(R.Removed == 1 && R.Covered == 1 && R.Missing == 0, "VertexWeld: coverage compares by welded ids and a removed twin does not stand in for the kept one", R.Summary("weld"));
    }

    // ======== TriangleOverlapTests ========
    {
        const std::vector<float> Base = { 0, 0, 0, 8, 0, 0, 0, 8, 0 };
        const auto Plus = [&](std::initializer_list<float> More) {
            std::vector<float> V = Base;
            V.insert(V.end(), More.begin(), More.end());
            return V;
        };
        {
            const std::vector<float> P = Plus({ 0, 0, 0, 8, 0, 0, 0, 8, 0 });
            const std::vector<int32_t> I = { 0, 1, 2, 3, 4, 5 };
            const OverlapResult R = TriangleOverlap::Find(P, I, std::vector<int32_t>{ 7, 7 });
            const ProofList Proofs = R.Proofs(P, I);
            Check(R.Duplicates.size() == 1 && R.Duplicates[0].Kept == 0 && R.Duplicates[0].Removed == 1 && R.Duplicates[0].IsDeletable() && Proofs.size() == 1 && Proofs[0].first == 1 && Contains(Proofs[0].second, "exact duplicate of triangle 0")
                      && R.PartialOverlaps.empty(),
                  "TriangleOverlap: an exact copy with other vertex ids is found and deletable", Proofs.empty() ? "" : Proofs[0].second);
        }
        {
            const OverlapResult Shifted = TriangleOverlap::Find(Base, std::vector<int32_t>{ 0, 1, 2, 1, 2, 0 }, std::vector<int32_t>{ 0, 0 });
            const std::vector<int32_t> Rev = { 0, 1, 2, 2, 1, 0 };
            const OverlapResult Reversed = TriangleOverlap::Find(Base, Rev, std::vector<int32_t>{ 0, 0 });
            Check(Shifted.Duplicates.size() == 1 && Shifted.Duplicates[0].SameWinding && Reversed.Duplicates.size() == 1 && !Reversed.Duplicates[0].SameWinding && !Reversed.Duplicates[0].IsDeletable() && Reversed.Proofs(Base, Rev).empty(),
                  "TriangleOverlap: a cyclic shift is the same winding and the reverse is back to back");
        }
        {
            const OverlapResult R = TriangleOverlap::Find(Base, std::vector<int32_t>{ 0, 1, 2, 0, 1, 2 }, std::vector<int32_t>{ 1, 2 });
            Check(R.Duplicates.size() == 1 && !R.Duplicates[0].SameLabel && !R.Duplicates[0].IsDeletable(), "TriangleOverlap: a copy with another label is a different surface and stays");
        }
        {
            const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 0, 8, 0, 8, 8, 0 };
            const OverlapResult R = TriangleOverlap::Find(P, std::vector<int32_t>{ 0, 1, 2, 1, 3, 2 }, std::vector<int32_t>{ 0, 0 });
            Check(R.Duplicates.empty() && R.PartialOverlaps.empty() && R.CoplanarPairsTested == 1, "TriangleOverlap: neighbouring triangles sharing only an edge are no overlap", std::to_string(R.CoplanarPairsTested));
        }
        {
            // The second triangle is the first moved 4 units along x: the overlap is the triangle (4,0) (8,0) (4,4), area 8.
            const OverlapResult R = TriangleOverlap::Find(Plus({ 4, 0, 0, 12, 0, 0, 4, 8, 0 }), std::vector<int32_t>{ 0, 1, 2, 3, 4, 5 }, std::vector<int32_t>{ 0, 0 });
            Check(R.PartialOverlaps.size() == 1 && Near(R.PartialOverlaps[0].Area, 8.0, 1e-6) && Near(R.PartialOverlaps[0].FractionOfSmaller, 0.25, 1e-6) && R.Duplicates.empty(), "TriangleOverlap: a shifted coplanar triangle overlaps by the computed area",
                  R.PartialOverlaps.empty() ? "" : Num(R.PartialOverlaps[0].Area));
        }
        {
            const std::vector<int32_t> I = { 0, 1, 2, 3, 4, 5 };
            const OverlapResult Lifted = TriangleOverlap::Find(Plus({ 0, 0, 1, 8, 0, 1, 0, 8, 1 }), I, std::vector<int32_t>{ 0, 0 });
            const OverlapResult Tilted = TriangleOverlap::Find(Plus({ 0, 0, 0, 8, 0, 0, 0, 7, 3 }), I, std::vector<int32_t>{ 0, 0 });
            Check(Lifted.PartialOverlaps.empty() && Tilted.PartialOverlaps.empty(), "TriangleOverlap: a triangle above the plane or tilted is not coplanar");
        }
    }
    {
        // A big triangle (0,0,0) (16,0,0) (0,16,0) and a small one (1,1,0) (4,1,0) (1,4,0) inside it.
        const std::vector<float> Nested = { 0, 0, 0, 16, 0, 0, 0, 16, 0, 1, 1, 0, 4, 1, 0, 1, 4, 0 };
        const std::vector<int32_t> I = { 0, 1, 2, 3, 4, 5 };
        {
            const OverlapResult R = TriangleOverlap::Find(Nested, I, std::vector<int32_t>{ 3, 3 });
            const ProofList Proofs = R.Proofs(Nested, I);
            Check(R.PartialOverlaps.size() == 1 && R.PartialOverlaps[0].IsContained() && Proofs.size() == 1 && Contains(Proofs[0].second, "contained in triangle 0") && Proofs[0].first == 1,
                  "TriangleOverlap: a triangle inside a larger one with the same label and facing is removed with the container named", Proofs.empty() ? "" : Proofs[0].second);
        }
        {
            const std::vector<int32_t> Flipped = { 0, 1, 2, 3, 5, 4 };
            Check(TriangleOverlap::Find(Nested, I, std::vector<int32_t>{ 3, 4 }).Proofs(Nested, I).empty() && TriangleOverlap::Find(Nested, Flipped, std::vector<int32_t>{ 3, 3 }).Proofs(Nested, Flipped).empty(),
                  "TriangleOverlap: a contained triangle with another label or the other side stays");
        }
        {
            // Triangle 2 (the small one) is inside triangle 1 (medium), which is inside triangle 0 (big): both inner ones go.
            const std::vector<float> P = { 0, 0, 0, 16, 0, 0, 0, 16, 0, 1, 1, 0, 8, 1, 0, 1, 8, 0, 2, 2, 0, 4, 2, 0, 2, 4, 0 };
            const std::vector<int32_t> Ix = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
            const ProofList Proofs = TriangleOverlap::Find(P, Ix, std::vector<int32_t>{ 1, 1, 1 }).Proofs(P, Ix);
            std::vector<int> Keys;
            for (const auto& Pr : Proofs) Keys.push_back(Pr.first);
            Check(Keys == std::vector<int>({ 1, 2 }), "TriangleOverlap: of a chain of containers only the outermost stays", std::to_string(Keys.size()));
        }
    }

    // ======== VertexWeldTests ========
    {
        // Two squares side by side on a grid of 8 units; the shared edge is written twice (vertices 1,2 and 4,5), once exactly and once 0.00005 off.
        const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0, 8.00005f, 0, 0, 8, 8.00005f, 0, 16, 0, 0, 16, 8, 0 };
        const std::vector<int32_t> I = { 0, 1, 2, 0, 2, 3, 4, 6, 7, 4, 7, 5 };
        {
            const NearestDistances D = VertexWeld::Measure(P, I);
            int Sum = 0, Mid = 0;
            for (size_t K = 0; K < D.Counts.size(); K++)
            {
                Sum += D.Counts[K];
                Mid += (K >= 1 && K <= 5) ? D.Counts[K] : 0;
            }
            Check(D.Vertices == 8 && D.SharedPosition == 0 && D.Counts[0] == 4 && Sum == 8 && Mid == 0, "VertexWeld: the distribution counts noise and real spacing in separate buckets", D.ToText());
        }
        {
            const ToleranceChoice Gap = VertexWeld::ChooseTolerance(VertexWeld::Measure(P, I));
            // A vertex 0.03 from its neighbour is neither noise nor spacing: only identical positions are welded.
            const std::vector<float> Mixed = { 0, 0, 0, 0.03f, 0, 0, 20, 0, 0 };
            const ToleranceChoice None = VertexWeld::ChooseTolerance(VertexWeld::Measure(Mixed, std::vector<int32_t>{ 0, 1, 2 }));
            Check(Gap.Tolerance == 0.01 && Contains(Gap.Reason, "none between") && None.Tolerance == 0 && Contains(None.Reason, "not separated"), "VertexWeld: a tolerance is chosen only when the data has a gap", Gap.Reason + " | " + None.Reason);
            Check(Gap.Reason == "nearest-vertex distances: 4 vertices within 0.0001 of another position, 0 at an identical position, none between 0.0001 and 0.1: tolerance 0.01 (the same set is welded by any value in the gap)", "VertexWeld: the tolerance reason reads as HydraX's", Gap.Reason);
        }
        {
            const WeldResult W = VertexWeld::Weld(P, I, 0.01);
            Check(W.Representative[4] == 1 && W.Representative[5] == 2 && W.Representative[6] == 6 && W.Clusters == 2 && W.Welded == 2 && W.LargestSpan < 0.001, "VertexWeld: vertices within the tolerance share the lowest id and distant ones stay", std::to_string(W.Clusters));
        }
    }

    // ======== BrushFaceUvTests ========
    {
        using V3 = RadiantGeometry::V3;
        const auto Sampled = [](const V3& P1, const V3& P2, const V3& P3, const BrushFaceNumbers& N, bool Curved) {
            std::vector<FaceSample> Samples;
            const V3 A = RadiantGeometry::Sub(P2, P1), B = RadiantGeometry::Sub(P3, P1);
            for (int I = 0; I < 4; I++)
                for (int J = 0; J < 4; J++)
                    for (const auto& C : { std::pair<int, int>{ I, J }, { I + 1, J }, { I, J + 1 } })
                    {
                        const V3 Corner = RadiantGeometry::Add(RadiantGeometry::Add(P1, RadiantGeometry::Mul(A, C.first / 4.0)), RadiantGeometry::Mul(B, C.second / 4.0));
                        const auto [U, Vv] = Curved ? std::pair<double, double>{ Corner[0] * Corner[0] / 500, Corner[1] / 32 } : BrushFaceUv::Forward(P1, P2, P3, N, Corner);
                        Samples.push_back({ Corner, U + (I * 3) - J, Vv - I + (J * 2) });
                    }
            return Samples;
        };
        // Face points wound so that (p3 - p1) x (p2 - p1) is the outward normal: +Z, +X and +Y.
        struct Face
        {
            const char* Axis;
            V3 P1, P2, P3;
        };
        const std::vector<Face> Faces = { { "Z", { 0, 0, 8 }, { 0, 64, 8 }, { 64, 0, 8 } }, { "X", { 8, 0, 0 }, { 8, 0, 64 }, { 8, 64, 0 } }, { "Y", { 0, 8, 0 }, { 64, 8, 0 }, { 0, 8, 64 } } };
        const std::vector<BrushFaceNumbers> Sets = { { 48, 32, 5, 7, 30, 0.25 }, { -24, 64, 3.5, 11, 0, 0 }, { 0.5, 0.25, 0.125, 0.0625, -120, -0.5 }, { 256, 512, 100, 200, 90, 0 } };
        for (const Face& F : Faces)
            for (const BrushFaceNumbers& N : Sets)
            {
                const std::vector<FaceSample> Samples = Sampled(F.P1, F.P2, F.P3, N, false);
                const std::optional<BrushFaceNumbers> Solved = BrushFaceUv::Solve(F.P1, F.P2, F.P3, Samples);
                bool Ok = Solved.has_value();
                if (Ok)
                {
                    const BrushFaceNumbers& G = *Solved;
                    Ok = Near(N.ScaleX, G.ScaleX, 1e-6) && Near(N.ScaleY, G.ScaleY, 1e-6) && Near(N.ShiftX, G.ShiftX, 1e-6) && Near(N.ShiftY, G.ShiftY, 1e-6) && Near(N.Rotation, G.Rotation, 1e-6) && Near(N.Shear, G.Shear, 1e-6);
                    // The same UV modulo 1 whatever the representation.
                    const V3 Probe = RadiantGeometry::Add(RadiantGeometry::Add(F.P1, RadiantGeometry::Mul(RadiantGeometry::Sub(F.P2, F.P1), 0.37)), RadiantGeometry::Mul(RadiantGeometry::Sub(F.P3, F.P1), 0.61));
                    const auto [U0, V0] = BrushFaceUv::Forward(F.P1, F.P2, F.P3, N, Probe);
                    const auto [U1, V1] = BrushFaceUv::Forward(F.P1, F.P2, F.P3, G, Probe);
                    Ok = Ok && Near((U0 - U1) - std::nearbyint(U0 - U1), 0, 1e-6) && Near((V0 - V1) - std::nearbyint(V0 - V1), 0, 1e-6);
                }
                Check(Ok, "BrushFaceUv: forward then inverse recovers the numbers for every axis class", std::string(F.Axis) + " scale " + Num(N.ScaleX) + " " + Num(N.ScaleY));
            }
        {
            // a shift beyond the scale is reduced to [0, |scale|): 100 mod 48 and 70 mod 32 (HydraX's Canonical)
            const Face& F = Faces[0];
            const BrushFaceNumbers Big = { 48, 32, 100, 70, 0, 0 };
            const std::optional<BrushFaceNumbers> Solved = BrushFaceUv::Solve(F.P1, F.P2, F.P3, Sampled(F.P1, F.P2, F.P3, Big, false));
            Check(Solved && Near(Solved->ShiftX, 4, 1e-6) && Near(Solved->ShiftY, 6, 1e-6), "BrushFaceUv: a shift beyond the scale comes back reduced", Solved ? Num(Solved->ShiftX) + " " + Num(Solved->ShiftY) : "none");
        }
        {
            // A 45 degree face between +Z and +X: Z comes first in the table, so S is the X axis. u grows with x at 1/16 per unit.
            const auto [U, Vv] = BrushFaceUv::Forward({ 0, 0, 0 }, { 0, 1, 0 }, { -1, 0, 1 }, { 16, 16, 0, 0, 0, 0 }, { 32, 0, 0 });
            Check(Near(U, 2, 1e-9), "BrushFaceUv: base axis ties go to the first table entry", Num(U) + " " + Num(Vv));
        }
        {
            const V3 P1 = { 0, 0, 8 }, P2 = { 0, 64, 8 }, P3 = { 64, 0, 8 };
            Check(!BrushFaceUv::Solve(P1, P2, P3, Sampled(P1, P2, P3, {}, true)).has_value(), "BrushFaceUv: a non axial texture does not solve");
            Check(!BrushFaceUv::Solve(P1, P2, P3, std::vector<FaceSample>{ { P1, 0, 0 }, { P2, 1, 0 } }).has_value(), "BrushFaceUv: too few render vertices do not solve");
        }
    }

    // ======== PatchModelsTests ========
    {
        std::vector<float> P(3 * 200100, 0.0f);
        const auto Set = [&](int Id, float X, float Y) {
            P[static_cast<size_t>(Id) * 3] = X;
            P[static_cast<size_t>(Id) * 3 + 1] = Y;
            P[static_cast<size_t>(Id) * 3 + 2] = 0;
        };
        Set(0, 0, 0); Set(1, 8, 0); Set(2, 16, 0); Set(3, 0, 8); Set(4, 8, 8); Set(5, 16, 8);
        Set(200000, 8, 0); Set(200001, 16, 0); Set(200002, 16, 8); Set(200003, 8, 8);
        const auto Close = PatchBorderModel::Logit(P, { 0, 1, 4, 3 }, { 1, 2, 5, 4 }, 1, 4);
        Check(Close && *Close < PatchBorderModel::CutLogit, "PatchBorderModel: two cells whose vertex ids are close are not a border", Close ? Num(*Close) : "none");
        const auto NoEdge = PatchBorderModel::Logit(P, { 0, 1, 4, 3 }, { 200000, 200001, 200002, 200003 }, 1, 4);
        const auto Shared = PatchBorderModel::Logit(P, { 0, 1, 4, 3 }, { 1, 200001, 200002, 4 }, 1, 4);
        Check(!NoEdge && Shared && *Shared > PatchBorderModel::CutLogit, "PatchBorderModel: two cells whose vertex ids are far apart are a border", Shared ? Num(*Shared) : "none");
        Check(!PatchBorderModel::Logit(P, { 0, 1, 4, 3 }, { 1, 2, 5, 4 }, 0, 5), "PatchBorderModel: an edge that is not a side of both cells has no score");
        const double Square = PatchPairModel::Logit(P, 0, 1, 0, 4, 1, 3, 0.0);
        Check(Square > PatchPairModel::MinimumLogit, "PatchPairModel: two triangles that make a square with consecutive ids are a likely cell", Num(Square));
        const double Far = PatchPairModel::Logit(P, 0, 100000, 1, 200002, 4, 200000, 6.0);
        Check(Far < PatchPairModel::MinimumLogit, "PatchPairModel: two slivers with far ids and a long diagonal are an unlikely cell", Num(Far));
        // The four values below were printed by HydraX's own C# models (a scratch console project holding copies of PatchPairModel.cs and PatchBorderModel.cs) on these inputs
        const auto Shared2 = PatchBorderModel::Logit(P, { 0, 1, 4, 3 }, { 1, 200001, 200002, 4 }, 1, 4);
        Check(Close && Shared2 && Near(*Close, -4.603970264191519, 1e-9) && Near(*Shared2, 9.234256624549904, 1e-9), "PatchBorderModel: the logits equal the values HydraX's C# prints", Close && Shared2 ? Num(*Close) + " " + Num(*Shared2) : "none");
        Check(Near(Square, 5.621017693359698, 1e-9) && Near(Far, -13.387197094288595, 1e-9), "PatchPairModel: the logits equal the values HydraX's C# prints", Num(Square) + " " + Num(Far));
    }

    // ======== DegenerateMeshUvTests (the cases that do not go through the writer) ========
    {
        // A 64 x 64 square in the XY plane, 2 groups of 2 points; the UV of the corner at (group, point) is (u(g), v(i)) in repeats.
        const auto Square = [](double U1, double V1, const char* Material = "mtl_decal", uint8_t Alpha = 128) {
            MapMesh M;
            M.GroupCount = 2;
            M.GroupSize = 2;
            M.Points = { { 0, 0, 0 }, { 0, 64, 0 }, { 64, 0, 0 }, { 64, 64, 0 } };
            M.Data = { { 0, 0, 255, 255, 255, 255 }, { 0, V1, 10, 20, 30, Alpha }, { U1, 0, 255, 255, 255, 255 }, { U1, V1, 10, 20, 30, Alpha } };
            M.Material = Material;
            M.Source = "test";
            return M;
        };
        {
            // V is constant (0) over the mesh: the Jacobian has one singular value of 0.
            DegenerateMeshUv Report;
            const bool Hit = Report.Check(Square(1, 0));
            Check(Hit && Report.Replaced == 1 && Report.MaxSeen == 1e9, "DegenerateMeshUv: a collapsed axis is counted with ratio 1e9", Num(Report.MaxSeen));
        }
        {
            // 8 repeats along U and 1 along V over a square: a 2048x256 image, ratio 8, under the 32 threshold.
            DegenerateMeshUv Report;
            Check(!Report.Check(Square(8, 1)) && Report.Replaced == 0, "DegenerateMeshUv: eight to one aspect is left alone");
        }
        {
            DegenerateMeshUv Normal, Stretched;
            Check(!Normal.Check(Square(1, 1)) && Normal.Replaced == 0 && Stretched.Check(Square(40, 1)) && Stretched.Replaced == 1 && Near(Stretched.MaxSeen, 40, 1e-6), "DegenerateMeshUv: a normal mesh is untouched and a stretch above the threshold is replaced", Num(Stretched.MaxSeen));
        }
        {
            DegenerateMeshUv Report;
            const bool A = Report.Check(Square(1, 0, "mtl_a")), B = Report.Check(Square(1, 0, "mtl_a")), C = Report.Check(Square(0, 1, "mtl_b")), D = Report.Check(Square(1, 1, "mtl_c"));
            const std::string Line = Report.Line();
            Check(A && B && C && !D && Report.Replaced == 3 && Contains(Line, "3 meshes flagged") && Contains(Line, "mtl_a 2, mtl_b 1") && Contains(Line, "is above 32,") && Contains(Line, "largest ratio 1000000000"), "DegenerateMeshUv: the report line counts by material", Line);
        }
        {
            MapMesh M = Square(1, 0);
            M.Data.clear();
            Check(!DegenerateMeshUv().Check(M), "DegenerateMeshUv: a mesh without render data is not measured");
        }
        {
            // One triangle keeps its UV (ratio 1), the other is collapsed (ratio 1e9): the median of the two is far above 32.
            MapMesh M = Square(1, 1);
            M.Data = { { 0, 0, 255, 255, 255, 255 }, { 0, 1, 255, 255, 255, 255 }, { 1, 0, 255, 255, 255, 255 }, { 1, 0, 255, 255, 255, 255 } };
            double Median = 0, Collapsed = 0;
            const bool Ok = DegenerateMeshUv::Measure(M, Median, Collapsed);
            Check(Ok && Median > DegenerateMeshUv::MaxRatio && Near(Collapsed, 0.5, 1e-6) && DegenerateMeshUv().Check(M), "DegenerateMeshUv: a half collapsed cell is degenerate because the two equal triangles average", Num(Median));
        }
    }

    // ======== what the port changed: scale structures against the plain HydraX loops ========
    {
        // CutRectangles against the rescan loop, on random cell sets with holes
        Lcg R{ 7 };
        int Differ = 0;
        std::string First;
        for (int Round = 0; Round < 120; Round++)
        {
            const int W = 3 + R.Below(14), H = 3 + R.Below(14), KeepPercent = 55 + R.Below(45);
            std::vector<std::pair<int, int>> Cells;
            for (int Y = -2; Y < H; Y++)
                for (int X = -3; X < W; X++)
                    if (R.Below(100) < KeepPercent) Cells.emplace_back(X, Y);
            const auto Fast = MeshInternal::CutRectangles(Cells);
            const auto Plain_ = NaiveCut(Cells);
            bool Same = Fast.size() == Plain_.size();
            for (size_t I = 0; Same && I < Fast.size(); I++) Same = Fast[I].X == Plain_[I].X && Fast[I].Y == Plain_[I].Y && Fast[I].W == Plain_[I].W && Fast[I].H == Plain_[I].H;
            if (!Same && Differ++ == 0)
            {
                size_t At = 0;
                while (At < Fast.size() && At < Plain_.size() && Fast[At].X == Plain_[At].X && Fast[At].Y == Plain_[At].Y && Fast[At].W == Plain_[At].W && Fast[At].H == Plain_[At].H) At++;
                First = std::format("round {} ({} cells) {} vs {} rectangles, first difference at {}: heap {},{} {}x{} plain {},{} {}x{}", Round, Cells.size(), Fast.size(), Plain_.size(), At, Fast[At].X, Fast[At].Y, Fast[At].W, Fast[At].H, Plain_[At].X,
                                    Plain_[At].Y, Plain_[At].W, Plain_[At].H);
            }
        }
        Check(Differ == 0, "CutRectangles: the heap version cuts exactly as the HydraX rescan loop on 120 random cell sets", First);
    }
    {
        // Measure and Weld against the plain loops, on points with exact copies, float noise, clusters and far singles
        Lcg R{ 99 };
        std::vector<float> P;
        std::vector<int32_t> Indices;
        for (int I = 0; I < 700; I++)
        {
            float X = static_cast<float>(R.Unit()) * 40.0f, Y = static_cast<float>(R.Unit()) * 40.0f, Z = static_cast<float>(R.Unit()) * 5.0f;
            const int Kind = R.Below(10);
            if (Kind == 0 && I > 0)   // an exact copy of an earlier vertex
            {
                const size_t Src = static_cast<size_t>(R.Below(I)) * 3;
                X = P[Src];
                Y = P[Src + 1];
                Z = P[Src + 2];
            }
            else if (Kind == 1 && I > 0)   // float noise away from an earlier vertex
            {
                const size_t Src = static_cast<size_t>(R.Below(I)) * 3;
                X = P[Src] + 0.00005f;
                Y = P[Src + 1];
                Z = P[Src + 2];
            }
            else if (Kind == 2)   // far single
            {
                X += 500.0f + static_cast<float>(I);
            }
            P.insert(P.end(), { X, Y, Z });
            Indices.push_back(I);
        }
        const NearestDistances Fast = VertexWeld::Measure(P, Indices), Plain_ = PlainMeasure(P, Indices);
        Check(Fast.Vertices == Plain_.Vertices && Fast.SharedPosition == Plain_.SharedPosition && Fast.Counts == Plain_.Counts && Fast.ToText() == Plain_.ToText(), "VertexWeld: the k-d tree Measure gives the HydraX 27 cell loop's distribution on 700 points", Fast.ToText());
        for (const double Tolerance : { 0.0, 0.01, 0.6 })
        {
            const WeldResult A = VertexWeld::Weld(P, Indices, Tolerance), B = PlainWeld(P, Indices, Tolerance);
            Check(A.Representative == B.Representative && A.Clusters == B.Clusters && A.Welded == B.Welded && A.LargestSpan == B.LargestSpan, "VertexWeld: the k-d tree Weld gives the plain loop's clusters and span", Num(Tolerance) + ": " + std::to_string(A.Clusters) + " vs " + std::to_string(B.Clusters));
        }
        // A cluster of more than 64 vertices reports no span
        std::vector<float> Dense;
        std::vector<int32_t> DenseIdx;
        for (int I = 0; I < 70; I++)
        {
            Dense.insert(Dense.end(), { 0.0001f * static_cast<float>(I), 0.0f, 0.0f });
            DenseIdx.push_back(I);
        }
        const WeldResult Big = VertexWeld::Weld(Dense, DenseIdx, 0.0002);
        Check(Big.Clusters == 1 && Big.Welded == 69 && Big.LargestSpan == 0, "VertexWeld: the span is only measured for clusters of 64 or fewer", std::to_string(Big.Clusters) + " " + Num(Big.LargestSpan));
    }
    {
        // TriangleOverlap: the pairs tested are those whose boxes meet, whatever the grid; a triangle too big for the grid is tested against all
        Lcg R{ 5 };
        Soup S;
        const auto AddTri = [&](float X, float Y, float Z, float Size) {
            const int Base = static_cast<int>(S.P.size() / 3);
            S.P.insert(S.P.end(), { X, Y, Z, X + Size, Y, Z, X, Y + Size, Z });
            for (int K = 0; K < 3; K++) S.I.push_back(Base + K);
        };
        for (int I = 0; I < 250; I++) AddTri(static_cast<float>(R.Unit()) * 200.0f, static_cast<float>(R.Unit()) * 200.0f, R.Below(2) == 0 ? 0.0f : 0.005f, 5.0f + static_cast<float>(R.Unit()) * 40.0f);
        AddTri(-3000.0f, -3000.0f, 0.0f, 9000.0f);   // reaches ~190 x 190 cells: above the grid limit
        AddTri(10.0f, 10.0f, 0.0f, 20.0f);           // an exact copy of nothing, but inside the big one
        const OverlapResult Result = TriangleOverlap::Find(S.P, S.I, Zeros(S.I.size() / 3));
        // brute force: every pair of triangles that are not copies, with area, whose boxes meet, that are coplanar
        const size_t Count = S.I.size() / 3;
        std::set<size_t> Copies;
        for (const TriangleDuplicate& D : Result.Duplicates) Copies.insert(static_cast<size_t>(D.Removed));
        const auto At = AtV(S.P);
        int Expected = 0;
        for (size_t A = 0; A < Count; A++)
            for (size_t B = A + 1; B < Count; B++)
            {
                if (Copies.count(A) || Copies.count(B)) continue;
                RadiantGeometry::V3 Lo[2], Hi[2], N[2];
                bool Skip = false;
                const size_t Ids[2] = { A, B };
                for (int K = 0; K < 2; K++)
                {
                    const auto P0 = At(S.I[Ids[K] * 3]), P1 = At(S.I[Ids[K] * 3 + 1]), P2 = At(S.I[Ids[K] * 3 + 2]);
                    const auto Cr = RadiantGeometry::Cross(RadiantGeometry::Sub(P1, P0), RadiantGeometry::Sub(P2, P0));
                    const double L = RadiantGeometry::Length(Cr);
                    Skip = Skip || L / 2 < TriangleOverlap::MinimumArea;
                    N[K] = { Cr[0] / L, Cr[1] / L, Cr[2] / L };
                    for (size_t C = 0; C < 3; C++)
                    {
                        Lo[K][C] = (std::min)(P0[C], (std::min)(P1[C], P2[C]));
                        Hi[K][C] = (std::max)(P0[C], (std::max)(P1[C], P2[C]));
                    }
                }
                if (Skip) continue;
                bool Meet = true;
                for (size_t C = 0; C < 3; C++) Meet = Meet && !(Hi[0][C] < Lo[1][C] || Hi[1][C] < Lo[0][C]);
                if (!Meet || std::fabs(RadiantGeometry::Dot(N[0], N[1])) < TriangleOverlap::NormalDot) continue;
                bool Flat = true;
                for (int K = 0; K < 3; K++) Flat = Flat && std::fabs(RadiantGeometry::Dot(N[0], RadiantGeometry::Sub(At(S.I[B * 3 + K]), At(S.I[A * 3])))) <= TriangleOverlap::PlaneDistance;
                if (Flat) Expected++;
            }
        Check(Result.CoplanarPairsTested == Expected && Expected > 100, "TriangleOverlap: the cell grid and the big triangle path test the same pairs as all pairs", std::to_string(Result.CoplanarPairsTested) + " vs " + std::to_string(Expected));
        // a pair listing is stable: First < Second, ascending, unique
        bool Ordered = true;
        for (size_t I = 0; I < Result.PartialOverlaps.size(); I++)
        {
            const auto& O = Result.PartialOverlaps[I];
            Ordered = Ordered && O.First < O.Second && (I == 0 || Result.PartialOverlaps[I - 1].First < O.First || (Result.PartialOverlaps[I - 1].First == O.First && Result.PartialOverlaps[I - 1].Second < O.Second));
        }
        Check(Ordered && !Result.PartialOverlaps.empty(), "TriangleOverlap: the overlaps are listed once each in (First, Second) order");
    }
    {
        // Augmenting paths: a strip of four triangles whose middle pair is a perfect square and whose outer pairs are sheared. The greedy step takes the middle pair and
        // strands the ends; the augmenting step moves each end onto its neighbour and the neighbour's partner onto the next one. (HydraX has no case for this.)
        Soup S;
        S.P = { -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 1, 0 };   // b0 b1 b2 t0 t1 t2
        S.I = { 0, 1, 3, 3, 1, 4, 1, 2, 4, 4, 2, 5 };
        const auto Grids = Build(S, Zeros(4));
        std::vector<std::vector<int32_t>> Sets;
        bool Lone = false;
        for (const BuiltGrid& G : Grids)
        {
            Lone = Lone || G.Grid.IsTriangle;
            Sets.push_back(Sorted(G.Grid.Triangles));
        }
        std::sort(Sets.begin(), Sets.end());
        // two sheared cells side by side: one mesh of four triangles (greedy alone gives a mesh of two and two lone triangles)
        Check(Grids.size() == 1 && !Lone && Sets == std::vector<std::vector<int32_t>>({ { 0, 1, 2, 3 } }), "QuadMesher: an augmenting path re-pairs a strip the greedy step strands", std::to_string(Grids.size()));
    }
    {
        // Determinism: the same input gives the same grids for any table seed, in both ranking modes; every input triangle ends in exactly one grid
        bool Same = true, Once = true, Complete = true;
        std::string Why;
        for (const uint64_t SoupSeed : { 1ull, 2ull, 3ull })
        {
            const Soup S = MixedSoup(SoupSeed, 24);
            const std::vector<int32_t> Labels = QuadrantLabels(S);
            for (const bool Vob : { false, true })
            {
                BuildOptions O;
                O.VertexOrderBorders = Vob;
                O.IsBorder = [](int A, int B, int, int) { return (A + B) % 37 == 0; };
                const auto Reference = Build(S, Labels, O);
                for (const uint64_t Seed : { 1ull, 0xDEADBEEFull, 987654321ull })
                {
                    O.HashSeed = Seed;
                    if (!SameGrids(Reference, Build(S, Labels, O)))
                    {
                        Same = false;
                        Why = std::format("soup {} vob {} seed {}", SoupSeed, Vob, Seed);
                    }
                }
                std::vector<int> Use(S.I.size() / 3, 0);
                for (const BuiltGrid& G : Reference)
                    for (const int32_t T : G.Grid.Triangles) Use[static_cast<size_t>(T)]++;
                Once = Once && std::all_of(Use.begin(), Use.end(), [](int U) { return U == 1; });
                const CoverageResult C = PatchCoverage::Check(S.P, S.I, Plain(Reference));
                Complete = Complete && C.IsComplete();
                if (!C.IsComplete()) Why = C.Summary("mixed soup");
            }
        }
        Check(Same, "QuadMesher: the output is the same for every table seed (independent of hash order)", Why);
        Check(Once, "QuadMesher: every input triangle is in exactly one grid on a mixed soup");
        Check(Complete, "PatchCoverage: a mixed soup (holes, noise, copies, fans, two labels) is complete after Build", Why);
    }
    {
        // A removed copy is not missing: build on the kept triangles only, check against all of them with the proofs
        const Soup S = MixedSoup(4, 12);
        const std::vector<int32_t> Labels = QuadrantLabels(S);
        const OverlapResult Overlap = TriangleOverlap::Find(S.P, S.I, Labels);
        const ProofList Proofs = Overlap.Proofs(S.P, S.I);
        std::vector<char> Gone(S.I.size() / 3, 0);
        for (const auto& P : Proofs) Gone[static_cast<size_t>(P.first)] = 1;
        Soup Kept;
        Kept.P = S.P;
        std::vector<int32_t> KeptLabels;
        for (size_t T = 0; T < Gone.size(); T++)
            if (!Gone[T])
            {
                Kept.I.insert(Kept.I.end(), S.I.begin() + static_cast<std::ptrdiff_t>(T * 3), S.I.begin() + static_cast<std::ptrdiff_t>(T * 3 + 3));
                KeptLabels.push_back(Labels[T]);
            }
        const auto Grids = Plain(Build(Kept, KeptLabels));
        const CoverageResult C = PatchCoverage::Check(S.P, S.I, Grids, Proofs);
        Check(!Proofs.empty() && C.Missing == 0 && C.Leftover == 0 && C.Removed == static_cast<int>(Proofs.size()) && C.Covered == static_cast<int>(Gone.size() - Proofs.size()) && Contains(Overlap.Summary("mixed soup"), "exact copies"),
              "PatchCoverage: triangles removed with a proof are not missing when the mesher was given only the kept ones", C.Summary("mixed") + " proofs " + std::to_string(Proofs.size()));
        // and without the proofs the same grids leave exactly those triangles missing
        const CoverageResult Bare = PatchCoverage::Check(S.P, S.I, Grids);
        Check(Bare.Missing == static_cast<int>(Proofs.size()) && Bare.Removed == 0, "PatchCoverage: without the proofs the removed triangles are missing", Bare.Summary("mixed"));
        // the removed list in any order gives the same result
        ProofList Backwards(Proofs.rbegin(), Proofs.rend());
        const CoverageResult Order = PatchCoverage::Check(S.P, S.I, Grids, Backwards);
        Check(Order.ToText("x") == C.ToText("x"), "PatchCoverage: the removed list may come in any order (sorted in a copy)");
    }
    {
        // A NaN coordinate: HydraX computes with it, so nothing throws and the triangles around it find no partner. Expected text printed by the HydraX C#.
        Soup S = GridSoup(3, 2);
        S.P[5 * 3] = std::nanf("");
        const std::vector<int32_t> Zero12 = Zeros(12);
        const QuadMesher::BuildResult B = QuadMesher::Build(S.P, S.I, Zero12);
        const std::string Want = "grids 5 order 0/0|G 0 mesh 3x2 pts 3 2 7 6 11 10 tris 4 5 10 11|G 0 fan 2x6 pts 0 1 6 10 9 4 5 5 5 5 5 5 tris 0 3 8 9 6|G 0 tri 2x2 pts 0 5 4 4 tris 1|G 0 tri 2x2 pts 1 2 6 6 tris 2|G 0 tri 2x2 pts 4 9 8 8 tris 7|";
        Check(Canon(B, "|") == Want, "NaN: Build leaves the triangles of a NaN vertex lone, as HydraX does", Canon(B, "|"));
        const OverlapResult F = TriangleOverlap::Find(S.P, S.I, Zero12);
        Check(F.Duplicates.empty() && F.PartialOverlaps.empty() && F.CoplanarPairsTested == 62, "NaN: Find tests the NaN triangles as HydraX does (62 pairs tested)", std::to_string(F.CoplanarPairsTested));
        Soup S2 = S;
        S2.P.insert(S2.P.end(), { 8, 0, 0, 16, 0, 0, 8, 8, 0 });
        S2.I.insert(S2.I.end(), { 12, 13, 14 });
        const OverlapResult F2 = TriangleOverlap::Find(S2.P, S2.I, Zeros(13));
        Check(F2.Duplicates.empty() && F2.PartialOverlaps.size() == 1 && F2.CoplanarPairsTested == 74, "NaN: Find with a triangle that overlaps a finite one", std::to_string(F2.PartialOverlaps.size()) + " " + std::to_string(F2.CoplanarPairsTested));
        const CoverageResult C = PatchCoverage::Check(S.P, S.I, Plain(B.Grids));
        Check(C.Summary("nan") == "Coverage of the nan: 12 input triangles (0 with zero area) -> 2 meshes (any size), 3 lone triangles, 10 cells; covered 12, removed on purpose 0, MISSING 0, LEFTOVER 0; area in NaN (0 removed on purpose), area out NaN (cut along the other diagonal NaN).",
              "NaN: Check counts the cells and prints NaN areas", C.Summary("nan"));
    }
    {
        // input errors: an index outside the positions, too few labels, grids that do not add up
        const Soup S = GridSoup(1, 1);
        const auto Throws = [](const auto& Call) {
            try
            {
                Call();
            }
            catch (const std::invalid_argument&)
            {
                return true;
            }
            return false;
        };
        const std::vector<int32_t> BadIndex = { 0, 1, 99 };
        Check(Throws([&] { QuadMesher::Build(S.P, BadIndex, Zeros(1)); }), "errors: Build refuses an index outside the positions");
        Check(Throws([&] { QuadMesher::Build(S.P, S.I, Zeros(1)); }), "errors: Build refuses fewer labels than triangles");
        Check(Throws([&] { TriangleOverlap::Find(S.P, BadIndex, Zeros(1)); }), "errors: Find refuses an index outside the positions");
        Check(Throws([&] { TriangleOverlap::Find(S.P, S.I, Zeros(1)); }), "errors: Find refuses fewer labels than triangles");
        Check(Throws([&] { PatchCoverage::Check(S.P, BadIndex, std::vector<QuadGrid>{}); }), "errors: Check refuses an index outside the positions");
        QuadGrid Short = QuadGrid::FromGroups({ { 0, 1 }, { 2, 3 } });
        Short.Points.pop_back();
        QuadGrid Wild = QuadGrid::FromGroups({ { 0, 1 }, { 2, 99 } });
        QuadGrid Negative = QuadGrid::FromGroups({ { 0, 1 }, { 2, -1 } });
        Check(Throws([&] { PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{ Short }); }), "errors: Check refuses a grid whose points do not number GroupCount x GroupSize");
        Check(Throws([&] { PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{ Wild }); }) && Throws([&] { PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{ Negative }); }), "errors: Check refuses a grid point outside the positions");
        const std::vector<int32_t> TooShort = { 0, 1, 2 };
        Check(Throws([&] { PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{}, ProofList{}, TooShort); }), "errors: Check refuses a weld shorter than the vertex count");
        // the removed list in any order
        const ProofList Mixed = { { 1, "second" }, { 0, "first" }, { 1, "again" } };
        const CoverageResult R = PatchCoverage::Check(S.P, S.I, std::vector<QuadGrid>{}, Mixed);
        Check(R.Removed == 2 && R.Missing == 0 && Contains(R.ToText("x"), "removed\t1\tsecond") && !Contains(R.ToText("x"), "again"), "errors: an unsorted removed list is sorted in a copy and the first proof of a triangle wins", R.ToText("x"));
    }
    {
        // Weld with a negative or NaN tolerance welds nothing, as HydraX; the values below are HydraX's
        const std::vector<float> P = { 0, 0, 0, 0, 0, 0, 1, 0, 0, 1.005f, 0, 0 };
        const std::vector<int32_t> I = { 0, 1, 2, 2, 3, 0 };
        const WeldResult Neg = VertexWeld::Weld(P, I, -1.0), Nan = VertexWeld::Weld(P, I, std::nan("")), Zero = VertexWeld::Weld(P, I, 0.0), Small = VertexWeld::Weld(P, I, 0.01);
        Check(Neg.Clusters == 0 && Neg.Representative == std::vector<int>({ 0, 1, 2, 3 }) && Nan.Clusters == 0 && Nan.Representative == std::vector<int>({ 0, 1, 2, 3 }), "VertexWeld: a negative or NaN tolerance welds nothing", std::to_string(Neg.Clusters) + " " + std::to_string(Nan.Clusters));
        Check(Zero.Clusters == 1 && Zero.Welded == 1 && Zero.Representative == std::vector<int>({ 0, 0, 2, 3 }) && Small.Clusters == 2 && Small.Representative == std::vector<int>({ 0, 0, 2, 2 }), "VertexWeld: tolerance 0 welds identical positions and 0.01 the 0.005 pair", std::to_string(Zero.Clusters));
    }
    {
        // a NaN texture coordinate: NaN sorts first (as CompareTo), ties by order met; the values are HydraX's (median 1, nothing flagged)
        MapMesh M;
        M.GroupCount = 3;
        M.GroupSize = 3;
        for (int G = 0; G < 3; G++)
            for (int I = 0; I < 3; I++)
            {
                M.Points.push_back({ static_cast<double>(G), static_cast<double>(I), 0 });
                const double U = G * (I == 2 ? 40 : 1);
                M.Data.push_back({ (G == 0 && I == 0) ? std::nan("") : U, static_cast<double>(I), 1, 2, 3, 4 });
            }
        double Median = 0, Collapsed = 0;
        const bool Ok = DegenerateMeshUv::Measure(M, Median, Collapsed);
        DegenerateMeshUv Report;
        const bool Hit = Report.Check(M);
        Check(Ok && Median == 1 && Collapsed == 0 && !Hit && Report.Replaced == 0 && Report.MaxSeen == 0, "DegenerateMeshUv: a NaN texture coordinate gives HydraX's values", Num(Median) + " " + Num(Collapsed));
    {
        // NaN in a different corner of each triangle (HydraX: 15 pairs tested, triangles 0 and 4 overlap)
        const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 0, 8, 0, NAN, 0, 0, 8, 0, 0, 0, 8, 0, 0, 0, 0, NAN, 0, 0, 0, 8, 0, 0, 0, 0, 8, 0, 0, NAN, 8, 0, 4, 0, 0, 12, 0, 0, 4, 8, 0, 1000, 0, 0, 1008, 0, 0, NAN, 8, 0 };
        const std::vector<int32_t> I = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 };
        const OverlapResult F = TriangleOverlap::Find(P, I, Zeros(6));
        Check(F.Duplicates.empty() && F.PartialOverlaps.size() == 1 && F.PartialOverlaps[0].First == 0 && F.PartialOverlaps[0].Second == 4 && F.CoplanarPairsTested == 15, "NaN: a NaN in any corner keeps the whole box NaN (Math.Min and Math.Max semantics)", std::to_string(F.CoplanarPairsTested));
        // a NaN texture coordinate among four triangles: NaN sorts first, so the median is taken over the rest (HydraX 3.369924076215481)
        MapMesh M;
        M.GroupCount = 3;
        M.GroupSize = 2;
        const double U[3][2] = { { 0, 1 }, { 3, 4 }, { 12, std::nan("") } };
        for (int G = 0; G < 3; G++)
            for (int K = 0; K < 2; K++)
            {
                M.Points.push_back({ static_cast<double>(G), static_cast<double>(K), 0 });
                M.Data.push_back({ U[G][K], static_cast<double>(K), 1, 2, 3, 4 });
            }
        double Median = 0, Collapsed = 0;
        const bool Ok = DegenerateMeshUv::Measure(M, Median, Collapsed);
        Check(Ok && Near(Median, 3.369924076215481, 1e-12) && Collapsed == 0, "DegenerateMeshUv: NaN sorts first and the median is HydraX's", Num(Median));
    }
    }
    {
        // the candidate cap: above it no pairing is tried, every triangle stays lone, the result says so and coverage still passes; below it nothing changes
        const Soup S = GridSoup(3, 2);
        QuadMesher::BuildOptions O;
        O.MaxCandidates = 3;
        const QuadMesher::BuildResult Capped = QuadMesher::Build(S.P, S.I, Zeros(12), O);
        bool AllLone = true;
        for (const BuiltGrid& G : Capped.Grids) AllLone = AllLone && (G.Grid.IsTriangle || G.Grid.IsFan);
        const QuadMesher::BuildResult Free = QuadMesher::Build(S.P, S.I, Zeros(12));
        Check(Capped.CandidatesCapped && Capped.TrianglesLeftLone == 12 && AllLone && PatchCoverage::Check(S.P, S.I, Plain(Capped.Grids)).IsComplete() && !Free.CandidatesCapped && Free.Grids.size() == 1,
              "cap: a small MaxCandidates leaves every triangle lone and says so; without it the grid is one mesh", std::to_string(Capped.Grids.size()));
        // 70,000 triangles on one edge: 2.4 billion pairs, more than an int counts; the default cap returns them lone at once
        Soup Edge;
        Edge.P = { 0, 0, 0, 1, 0, 0 };
        for (int T = 0; T < 70000; T++)
        {
            Edge.P.insert(Edge.P.end(), { 0.5f, 1.0f + (static_cast<float>(T) * 0.001f), 0 });
            Edge.I.insert(Edge.I.end(), { 0, 1, 2 + T });
        }
        const QuadMesher::BuildResult Many = QuadMesher::Build(Edge.P, Edge.I, Zeros(70000));
        Check(Many.CandidatesCapped && Many.TrianglesLeftLone == 70000 && Many.Grids.size() == 70000 && PatchCoverage::Check(Edge.P, Edge.I, Plain(Many.Grids)).IsComplete(), "cap: 70,000 triangles on one edge come back lone, counted, and covered", std::to_string(Many.Grids.size()));
    }
    {
        // cells at the edge of int: neighbours are looked up without overflow
        constexpr int Max = std::numeric_limits<int>::max(), Min = std::numeric_limits<int>::min();
        const auto One = MeshInternal::CutRectangles(std::vector<std::pair<int, int>>{ { Max, 0 } });
        const auto Two = MeshInternal::CutRectangles(std::vector<std::pair<int, int>>{ { Max - 1, 0 }, { Max, 0 }, { Min, 0 } });
        const auto Low = MeshInternal::CutRectangles(std::vector<std::pair<int, int>>{ { Min, Min }, { Min + 1, Min } });
        Check(One.size() == 1 && One[0].X == Max && One[0].Y == 0 && One[0].W == 1 && One[0].H == 1 && Two.size() == 2 && Two[0].X == Max - 1 && Two[0].W == 2 && Two[1].X == Min && Two[1].W == 1 && Low.size() == 1 && Low[0].W == 2,
              "CutRectangles: cells at INT_MAX and INT_MIN do not overflow", std::to_string(One.size()) + " " + std::to_string(Two.size()));
    }
    {
        // the vertex-order models against the HydraX C# on a fixed soup: grids, order counts and a hash of the whole text
        std::vector<int32_t> Labels;
        const Soup S = GoldenSoup(Labels);
        QuadMesher::BuildOptions Plainly, Vob, Border;
        Vob.VertexOrderBorders = true;
        Border.VertexOrderBorders = true;
        Border.IsBorder = [](int A, int B, int, int) { return (A + B) % 37 == 0; };
        const QuadMesher::BuildResult P = QuadMesher::Build(S.P, S.I, Labels, Plainly), V = QuadMesher::Build(S.P, S.I, Labels, Vob), B = QuadMesher::Build(S.P, S.I, Labels, Border);
        Check(Labels.size() == 370 && P.Grids.size() == 23 && Fnv(Canon(P, "\n")) == 14234557769330965866ull, "golden: Build without the vertex-order models equals HydraX", std::to_string(Fnv(Canon(P, "\n"))));
        Check(V.Grids.size() == 23 && V.OrderScored == 188 && V.OrderCut == 6 && Fnv(Canon(V, "\n")) == 10340567226926813531ull, "golden: Build with VertexOrderBorders equals HydraX (188 edges scored, 6 cut)", std::to_string(V.OrderScored) + "/" + std::to_string(V.OrderCut) + " " + std::to_string(Fnv(Canon(V, "\n"))));
        Check(B.Grids.size() == 23 && B.OrderScored == 195 && B.OrderCut == 6 && Fnv(Canon(B, "\n")) == 7586027190325157625ull, "golden: Build with VertexOrderBorders and a border callback equals HydraX", std::to_string(B.OrderScored) + "/" + std::to_string(B.OrderCut) + " " + std::to_string(Fnv(Canon(B, "\n"))));
    }
    {
        // the "0.###" text: 15 significant digits rounded half away from zero, as .NET prints them (values from the HydraX C# run)
        const auto Area = [](double V) {
            CoverageResult R;
            R.InputArea = V;
            const std::string T = R.Summary("x");
            const size_t At = T.find("area in ") + 8;
            return T.substr(At, T.find(' ', At) - At);
        };
        Check(Area(2.0005) == "2.001" && Area(0.0625) == "0.063" && Area(0.0015) == "0.002" && Area(1234.5675) == "1234.568" && Area(0.0004999999) == "0" && Area(2.5) == "2.5" && Area(100.0005) == "100.001" && Area(1.0000000000000002) == "1" && Area(0) == "0",
              "format: 0.### rounds the 15 digit text half away from zero like .NET", Area(2.0005) + " " + Area(0.0625) + " " + Area(0.0004999999));
    }
    {
        // A square tessellated twice over the same four vertices (both diagonals are input): each cell must take the cut whose halves are still free, so both
        // tessellations are covered. HydraX's rule (any cut whose halves exist) takes the same diagonal twice and reports two leftovers and two missing triangles.
        const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0 };
        const std::vector<int32_t> I = { 0, 1, 2, 0, 2, 3, 0, 1, 3, 1, 2, 3 };
        const QuadGrid Cell = QuadGrid::FromGroups({ { 0, 1 }, { 3, 2 } });
        const CoverageResult C = PatchCoverage::Check(P, I, std::vector<QuadGrid>{ Cell, Cell });
        Check(C.IsComplete() && C.Covered == 4, "PatchCoverage: a square tessellated both ways is complete (the cut with free halves wins)", C.Summary("both diagonals"));
    }
    {
        // A two-sided surface: the same four vertices as two cells of opposite winding. Both cells land on one lattice cell; the second must start a grid of its own
        // instead of being dropped (HydraX loses its two triangles).
        const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0 };
        const std::vector<int32_t> I = { 0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2 };
        const auto Built = QuadMesher::Build(P, I, Zeros(4));
        std::vector<int> Use(4, 0);
        for (const BuiltGrid& G : Built.Grids)
            for (const int32_t T : G.Grid.Triangles) Use[static_cast<size_t>(T)]++;
        const CoverageResult C = PatchCoverage::Check(P, I, Plain(Built.Grids));
        Check(Use == std::vector<int>({ 1, 1, 1, 1 }) && C.IsComplete() && Built.Grids.size() == 2, "QuadMesher: the two sides of a surface on the same four vertices are two grids, no triangle lost", std::to_string(Built.Grids.size()) + " " + C.Summary("two-sided"));
        // The second side must also land on a lattice cell its first side's neighbour reaches: a third cell next to the first one, which the second side's four vertices
        // share an edge with, walks onto the first side's own lattice cell. 2 x 1 cells (triangles 0-3) and the other side of the first cell (4-5).
        {
            const std::vector<float> P2 = { 0, 0, 0, 8, 0, 0, 16, 0, 0, 0, 8, 0, 8, 8, 0, 16, 8, 0 };
            const std::vector<int32_t> I2 = { 0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4, 0, 4, 1, 0, 3, 4 };
            const auto Built2 = QuadMesher::Build(P2, I2, Zeros(6));
            std::vector<int> Use2(6, 0);
            for (const BuiltGrid& G : Built2.Grids)
                for (const int32_t T : G.Grid.Triangles) Use2[static_cast<size_t>(T)]++;
            const CoverageResult C2 = PatchCoverage::Check(P2, I2, Plain(Built2.Grids));
            Check(Use2 == std::vector<int>({ 1, 1, 1, 1, 1, 1 }) && C2.IsComplete(), "QuadMesher: the other side of a cell is not lost when a neighbour walks onto its lattice cell", C2.Summary("two-sided strip") + " " + std::to_string(Built2.Grids.size()));
        }
    }
    {
        // Pass 1 of the cell loop. Cell 1 (a, b, c, d) has both diagonals as input triangles and is listed first; cell 2 (a, b, c, e) has only the cut abc + ace, and abc exists once.
        // A cell that takes the first cut whose halves exist (HydraX; also this module before the two passes) lets cell 1 take abc and leaves cell 2 a leftover and a
        // missing triangle. Pass 0 gives abc to cell 2, whose cut is the only one, and cell 1 then takes abd + bcd; the lone triangle acd is the rest of the second tessellation.
        const std::vector<float> P = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 0, 0, 16, 0 };   // a b c d e
        const std::vector<int32_t> I = { 0, 1, 2, 0, 2, 3, 0, 1, 3, 1, 2, 3, 0, 2, 4 };   // abc acd abd bcd ace
        const QuadGrid Cell1 = QuadGrid::FromGroups({ { 0, 1 }, { 3, 2 } });
        const QuadGrid Cell2 = QuadGrid::FromGroups({ { 0, 1 }, { 4, 2 } });
        const QuadGrid LoneAcd = QuadGrid::FromGroups({ { 0, 2 }, { 3, 3 } }, true);
        const CoverageResult C = PatchCoverage::Check(P, I, std::vector<QuadGrid>{ Cell1, Cell2, LoneAcd });
        Check(C.IsComplete() && C.Covered == 5 && C.CellsBothDiagonals == 1, "PatchCoverage: a cell with both diagonals listed before a cell that needs one of them is complete (pass 0 before pass 1)", C.Summary("pass 1"));
        // the order the grids are listed in changes nothing
        const CoverageResult Reversed = PatchCoverage::Check(P, I, std::vector<QuadGrid>{ LoneAcd, Cell2, Cell1 });
        Check(Reversed.IsComplete() && Reversed.Covered == 5 && Reversed.Summary("x") == C.Summary("x"), "PatchCoverage: the listing order of the grids does not change the result", Reversed.Summary("x"));
        // both diagonals, a cell that is not planar is listed (the fourth point 3 units above the plane of the other three)
        const std::vector<float> Bent = { 0, 0, 0, 8, 0, 0, 8, 8, 0, 0, 8, 3 };
        const CoverageResult B = PatchCoverage::Check(Bent, std::vector<int32_t>{ 0, 1, 2, 0, 2, 3, 0, 1, 3, 1, 2, 3 }, std::vector<QuadGrid>{ Cell1, Cell1 });
        bool Listed = false;
        for (const CoverageLine& L : B.Lines) Listed = Listed || L.Kind == "both_diagonals";
        Check(B.CellsBothDiagonals == 2 && B.CellsBothDiagonalsNonPlanar == 2 && Listed && B.IsComplete(), "PatchCoverage: cells with both diagonals are counted and the non-planar ones listed", B.Summary("bent"));
    }
    {
        // Five cells around a convex pentagon (vertices 0 2 3 4 5, vertex 1 unused), every cell with both cuts among the input. Greedy, in any order it is given, can
        // take a diagonal a later cell needed and report two missing and two leftover on correct output; the repair flips cells along a chain until nothing improves.
        std::vector<float> P;
        for (int V = 0; V < 6; V++)
        {
            const double Angle = (V == 1 ? 0.0 : 90.0 + (72.0 * (V == 0 ? 0 : V == 2 ? 1 : V == 3 ? 2 : V == 4 ? 3 : 4))) * 3.14159265358979 / 180.0;
            P.insert(P.end(), { static_cast<float>(10 * std::cos(Angle)), static_cast<float>(10 * std::sin(Angle)), 0.0f });
        }
        const std::vector<int32_t> I = { 0, 2, 4, 2, 3, 4, 0, 2, 3, 0, 3, 5, 0, 2, 5, 2, 3, 5, 0, 3, 4, 0, 4, 5, 0, 3, 5, 3, 4, 5 };
        const auto Cell = [](int A, int B, int C, int D) { return QuadGrid::FromGroups({ { A, B }, { D, C } }); };
        const std::vector<QuadGrid> Plain5 = { Cell(0, 2, 3, 4), Cell(0, 2, 3, 5), Cell(0, 2, 3, 5), Cell(0, 3, 4, 5), Cell(0, 3, 4, 5) };
        const CoverageResult C = PatchCoverage::Check(P, I, Plain5);
        Check(C.IsComplete() && C.Covered == 10 && C.Cells == 5, "PatchCoverage: five cells around a pentagon that greedy cannot cover are repaired to complete", C.Summary("pentagon"));
        // the same cells with the first one written from another corner (2 3 4 0)
        std::vector<QuadGrid> Rotated = Plain5;
        Rotated[0] = Cell(2, 3, 4, 0);
        const CoverageResult Rot = PatchCoverage::Check(P, I, Rotated);
        Check(Rot.IsComplete() && Rot.Covered == 10, "PatchCoverage: the rotated first cell is complete too", Rot.Summary("rotated"));
        // the order of the grids changes nothing
        std::vector<QuadGrid> Reversed(Plain5.rbegin(), Plain5.rend());
        const CoverageResult Rev = PatchCoverage::Check(P, I, Reversed);
        Check(Rev.IsComplete() && Rev.Summary("x") == C.Summary("x"), "PatchCoverage: the repair does not depend on the order the grids are listed in", Rev.Summary("x"));
        // a true loss is still reported: triangle 3 4 5 is replaced by one no cell holds (1 2 3), so one input triangle is missing and one half is no input triangle
        std::vector<int32_t> Lost = I;
        Lost[27] = 1;
        Lost[28] = 2;
        Lost[29] = 3;
        const CoverageResult L = PatchCoverage::Check(P, Lost, Plain5);
        Check(!L.IsComplete() && L.Missing >= 1 && L.Leftover >= 1, "PatchCoverage: a real loss is still reported after the repair", L.Summary("lost"));
    }
    {
        // Bad input is refused, not read
        bool Threw = false;
        try
        {
            QuadMesher::Build(std::vector<float>{ 0, 0, 0 }, std::vector<int32_t>{ 0, 1, 2 }, std::vector<int32_t>{ 0 });
        }
        catch (const std::invalid_argument&)
        {
            Threw = true;
        }
        Check(Threw, "QuadMesher: an index outside the positions is refused");
    }

    return Failed;
}
}
