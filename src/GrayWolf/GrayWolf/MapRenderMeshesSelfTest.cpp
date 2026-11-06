#include "stdafx.h"

#include "MapRadiantRules.h"
#include "MapRenderMeshes.h"

#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Self-test of the render meshes: the stock mesh layout (HydraX's The_writer_emits_the_stock_mesh_layout), borders by colour and by texture coordinate, a texture
// coordinate that crosses a repeat, facing, the point limit, the degenerate texture swap, copies with their proof, decals, and that the thread count changes nothing.
namespace Maps
{
namespace
{
    using V3 = std::array<double, 3>;

    // A grid of columns x rows cells on z = 0 (8 units a cell, counter-clockwise from above). Each cell has four vertices of its own (at the same positions as its
    // neighbours'), so the render data may differ across a shared edge; Corner gives the data of a cell's corner (0 to 3: (i, j), (i+1, j), (i+1, j+1), (i, j+1)).
    RenderSoup CellSoup(int Columns, int Rows, const std::function<RenderCorner(int Cell, int Corner)>& Corner, double FrontZ = 1.0)
    {
        RenderSoup S;
        for (int J = 0; J < Rows; J++)
            for (int I = 0; I < Columns; I++)
            {
                const int Cell = (J * Columns) + I;
                const int Base = static_cast<int>(S.Corners.size());
                const int Cx[4] = { I, I + 1, I + 1, I }, Cy[4] = { J, J, J + 1, J + 1 };
                for (int K = 0; K < 4; K++)
                {
                    S.Positions.insert(S.Positions.end(), { Cx[K] * 8.0f, Cy[K] * 8.0f, 0.0f });
                    S.Corners.push_back(Corner(Cell, K));
                }
                for (const int K : { 0, 1, 2, 0, 2, 3 }) S.Indices.push_back(Base + K);
                for (int T = 0; T < 2; T++)
                {
                    S.Label.push_back(0);
                    S.Front.push_back({ 0, 0, FrontZ });
                    S.Normal.push_back({ 0, 0, FrontZ });
                }
            }
        return S;
    }

    RenderCorner Plain(int, int) { return RenderCorner(); }

    // the sum over a mesh's cells of (next group direction) x (next point direction), z only
    double FacingZ(const MapMesh& M)
    {
        double Z = 0;
        for (int G = 0; G + 1 < M.GroupCount; G++)
            for (int P = 0; P + 1 < M.GroupSize; P++)
            {
                const V3 A = M.At(G, P), B = M.At(G + 1, P), C = M.At(G, P + 1);
                Z += ((B[0] - A[0]) * (C[1] - A[1])) - ((B[1] - A[1]) * (C[0] - A[0]));
            }
        return Z;
    }

    std::string AllText(const RenderMeshResult& R)
    {
        std::string Out;
        for (const RenderMesh& M : R.Meshes) Out += MeshText(M.Mesh, "m", M.ReplaceUv);
        return Out;
    }

    bool Contains(const std::string& Haystack, const std::string& Needle) { return Haystack.find(Needle) != std::string::npos; }
    bool Near(double A, double B, double Tolerance) { return std::fabs(A - B) <= Tolerance; }
}

int RenderMeshesSelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* Name, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::format("FAIL RenderMeshes {} (got '{}')\n", Name, Got);
    };
    const std::vector<std::string> One = { "m" };
    RenderMeshOptions Serial;
    Serial.Threads = 1;

    // ---- the stock layout (HydraX The_writer_emits_the_stock_mesh_layout)
    {
        MapMesh M;
        M.GroupCount = 2;
        M.GroupSize = 2;
        M.Points = { { 0, 0, 0 }, { 0, 4, 0 }, { 8, 0, 0 }, { 8, 4, 0 } };
        MeshTextOptions Stock;
        Stock.NonColliding = false;
        const std::string Text = MeshText(M, "snow", false, Stock);
        Check(Contains(Text, "  mesh\n  {\n  toolFlags;\n   snow\n   lightmap_gray\n   2 2 0 8\n   (\n") && Contains(Text, "\tv 8 4 0 t 128 -64 0 0\n"), "the writer emits the stock mesh layout", Text);
    }
    // the layout this export writes (the defaults: the stock zm_giant_geo.map layout): nonColliding, toolFlags, the size line "W H 0 8", the lightmap pair "0 0"
    {
        MapMesh M;
        M.GroupCount = 2;
        M.GroupSize = 2;
        M.Points = { { 0, 0, 0 }, { 8, 0, 0 }, { 0, 8, 0 }, { 0, 8, 0 } };
        M.Data = { { 0.5, 0.25, 255, 255, 255, 255 }, { 1, 0.25, 10, 20, 30, 40 }, { 0.5, 1, 255, 255, 255, 255 }, { 0.5, 1, 255, 255, 255, 255 } };
        MeshTextOptions Here;
        Here.Prefix = "// brush 7\n {\n";
        const std::string Text = MeshText(M, "mtl", false, Here);
        Check(Text.rfind("// brush 7\n {\n  mesh\n  {\n  contents nonColliding;\n  toolFlags;\n   mtl\n   lightmap_gray\n   2 2 0 8\n   (\n\tv 0 0 0 t 512 256 0 0\n\tv 8 0 0 c 10 20 30 40 t 1024 256 0 0\n   )\n", 0) == 0 &&
                  Contains(Text, "\tv 0 8 0 t 512 1024 0 0\n"),
              "a mesh as this export writes it by default: toolFlags in stock position, size line W H 0 8, colour only when not white, t = UV x 1024, lightmap pair 0 0", Text);
    }

    // a decal mesh's editor layer: decalLayerSort and decalEditorSort after the rows, inside the mesh (stock zm_giant_geo.map, HydraX IwMapWriter); none without a layer
    {
        MapMesh M;
        M.GroupCount = 2;
        M.GroupSize = 2;
        M.Points = { { 0, 0, 0 }, { 0, 4, 0 }, { 8, 0, 0 }, { 8, 4, 0 } };
        MeshTextOptions Layered;
        Layered.DecalLayer = "Grunge";
        const std::string With = MeshText(M, "decal", false, Layered), Without = MeshText(M, "decal", false);
        Check(Contains(With, "   )\n   decalLayerSort \"Grunge\"\n   decalEditorSort -1\n  }\n") && !Contains(Without, "decalLayerSort") && !Contains(Without, "decalEditorSort"), "a decal layer is written after the rows as decalLayerSort and decalEditorSort -1, and only when there is one", With);
    }

    // ---- one flat grid becomes one mesh and every triangle is accounted for
    {
        const RenderSoup S = CellSoup(3, 2, Plain);
        const RenderMeshResult R = BuildRenderMeshes(S, One, Serial);
        const RenderMeshCounters& C = R.Count;
        Check(R.Meshes.size() == 1 && C.TrianglesIn == 12 && C.Covered == 12 && C.Cells == 6 && C.CoverageMissing == 0 && C.CoverageLeftover == 0 && C.LabelsFailed == 0 && C.FacingAgainst == 0,
              "a flat grid is one mesh with every triangle covered", std::to_string(R.Meshes.size()) + " " + std::to_string(C.Covered));
        if (!R.Meshes.empty()) Check(R.Meshes[0].Mesh.GroupCount * R.Meshes[0].Mesh.GroupSize == 12, "its points are the grid's twelve vertices");
    }

    // ---- borders: a different texture coordinate, a different colour
    {
        const auto Right = [](double DU, uint8_t Red) {
            return [DU, Red](int Cell, int Corner) {
                RenderCorner C;
                // the second cell's four corners; a cell's own UV runs 0 to 1 across it
                const float Ux[4] = { 0, 1, 1, 0 }, Uy[4] = { 0, 0, 1, 1 };
                C.U = Ux[Corner] + (Cell == 1 ? static_cast<float>(DU) : 0.0f);
                C.V = Uy[Corner];
                if (Cell == 1) C.Color[0] = Red;
                return C;
            };
        };
        const RenderMeshResult Same = BuildRenderMeshes(CellSoup(2, 1, Right(0, 255)), One, Serial);
        const RenderMeshResult Uv = BuildRenderMeshes(CellSoup(2, 1, Right(0.5, 255)), One, Serial);
        const RenderMeshResult Col = BuildRenderMeshes(CellSoup(2, 1, Right(0, 128)), One, Serial);
        Check(Same.Meshes.size() == 1 && Same.Count.BordersCutTexture == 0 && Same.Count.BordersCutColour == 0, "equal render data across the edge: the cells join", std::to_string(Same.Meshes.size()));
        Check(Uv.Meshes.size() == 2 && Uv.Count.BordersCutTexture > 0 && Uv.Count.CoverageMissing == 0 && Uv.Count.CoverageLeftover == 0, "a texture coordinate that differs across the edge splits the cell off", std::to_string(Uv.Meshes.size()));
        bool Exact = Col.Meshes.size() == 2 && Col.Count.BordersCutColour > 0;
        for (const RenderMesh& M : Col.Meshes)
            for (const MeshPointData& D : M.Mesh.Data) Exact = Exact && (D.R == 255 || D.R == 128) && D.G == 255 && D.B == 255 && D.A == 255;
        Check(Exact, "a vertex colour that differs across the edge splits the cell off and every point keeps its exact colour", std::to_string(Col.Meshes.size()));
        // an alpha weight (a blend layer) survives to the text
        const std::string Text = AllText(Col);
        Check(Contains(Text, " c 128 255 255 255 "), "the colour is written per vertex", Text.substr(0, 200));
    }

    // ---- a texture coordinate that crosses a repeat: a whole repeat apart is the same coordinate (modulo the repeat), the cells join, the point keeps one value
    {
        const auto Wrapped = [](int Cell, int Corner) {
            RenderCorner C;
            const float Ux[4] = { 0, 1, 1, 0 };
            C.U = Ux[Corner] + (Cell == 1 ? 1.0f : 0.0f);   // the second cell's coordinates run 1 to 2 where the first's ran 0 to 1: its first edge is the first cell's second
            C.V = (Corner == 2 || Corner == 3) ? 1.0f : 0.0f;
            return C;
        };
        const RenderMeshResult R = BuildRenderMeshes(CellSoup(2, 1, Wrapped), One, Serial);
        // cell 1 corner 0 is at x = 8 and has U = 1; cell 0 corner 1 is at x = 8 with U = 1: equal. The far edge: cell 1 U = 2
        Check(R.Meshes.size() == 1 && R.Count.BordersCutTexture == 0, "continuous coordinates that pass a repeat do not split", std::to_string(R.Meshes.size()));
        const auto Offset = [](int, int Corner) {
            RenderCorner C;
            const float Ux[4] = { 0, 1, 1, 0 };
            C.U = Ux[Corner];   // both cells run 0 to 1
            C.V = (Corner == 2 || Corner == 3) ? 1.0f : 0.0f;
            return C;
        };
        // cell 1 restarts at 0 on its shared edge (0 against the first cell's 1): a whole repeat apart, so no split (modulo the repeat), and the second cell is moved by one
        // repeat so U grows by one a cell along x (up to one constant) instead of 0, 1, 0 and 1
        const RenderMeshResult Restart = BuildRenderMeshes(CellSoup(2, 1, Offset), One, Serial);
        bool Continuous = Restart.Meshes.size() == 1;
        if (Continuous)
            for (int G = 0; G < Restart.Meshes[0].Mesh.GroupCount; G++)
                for (int P = 0; P < Restart.Meshes[0].Mesh.GroupSize; P++)
                    Continuous = Continuous && Near(Restart.Meshes[0].Mesh.DataAt(G, P).U - (Restart.Meshes[0].Mesh.At(G, P)[0] / 8.0), Restart.Meshes[0].Mesh.DataAt(0, 0).U - (Restart.Meshes[0].Mesh.At(0, 0)[0] / 8.0), 1e-6);
        Check(Restart.Meshes.size() == 1 && Restart.Count.BordersCutTexture == 0 && Restart.Count.UvTrianglesShifted > 0 && Restart.Count.UvConflictCorners == 0 && Continuous,
              "a coordinate a whole repeat apart is the same modulo the repeat: no split, the neighbour is moved by whole repeats and U grows one per cell",
              std::to_string(Restart.Meshes.size()) + " cut " + std::to_string(Restart.Count.BordersCutTexture) + " shifted " + std::to_string(Restart.Count.UvTrianglesShifted) + " conflicts " + std::to_string(Restart.Count.UvConflictCorners) + " continuous " + std::to_string(Continuous));
        double Largest = 0;
        MeshText(R.Meshes[0].Mesh, "m", false, {}, &Largest);
        Check(Largest == 2048, "t = UV x 1024 stays in range (2 repeats is 2048)", std::to_string(Largest));
    }

    // ---- facing: a mesh shows on the side its group direction x point direction points to, and must be the front the render data gives
    {
        const RenderMeshResult Up = BuildRenderMeshes(CellSoup(2, 2, Plain, 1.0), One, Serial);
        const RenderMeshResult Down = BuildRenderMeshes(CellSoup(2, 2, Plain, -1.0), One, Serial);
        Check(Up.Meshes.size() == 1 && Down.Meshes.size() == 1 && FacingZ(Up.Meshes[0].Mesh) * 1.0 > 0 && FacingZ(Down.Meshes[0].Mesh) < 0, "a mesh faces the front the render data gives, either way", Up.Meshes.empty() ? "" : std::to_string(FacingZ(Up.Meshes[0].Mesh)));
        Check(Up.Count.FacingNormal == 1 && Up.Count.FacingAgainst == 0 && Down.Count.FacingNormal == 1 && Down.Count.FacingAgainst == 0 && (Up.Count.Flipped + Down.Count.Flipped) == 1,
              "the facing counters agree with the vertex normals (0 against); exactly one of the two was turned", std::to_string(Up.Count.Flipped) + " " + std::to_string(Down.Count.Flipped));
        // no front known: the mesher's own order is kept and counted
        RenderSoup Unknown = CellSoup(2, 1, Plain);
        for (auto& F : Unknown.Front) F = { 0, 0, 0 };
        const RenderMeshResult U = BuildRenderMeshes(Unknown, One, Serial);
        Check(U.Count.FacingUnknown == 1 && U.Count.Flipped == 0, "a grid with no known front keeps the mesher's order and is counted", std::to_string(U.Count.FacingUnknown));
    }

    // ---- the point limit: a bigger grid is split into pieces that share their border points
    {
        const RenderSoup Long = CellSoup(200, 1, Plain);
        RenderMeshOptions O = Serial;
        const RenderMeshResult R = BuildRenderMeshes(Long, One, O);
        size_t Cells = 0;
        bool Within = true;
        for (const RenderMesh& M : R.Meshes)
        {
            Cells += static_cast<size_t>(M.Mesh.GroupCount - 1) * static_cast<size_t>(M.Mesh.GroupSize - 1);
            Within = Within && M.Mesh.GroupCount <= 128 && M.Mesh.GroupSize <= 128;
        }
        Check(R.Meshes.size() == 2 && Within && Cells == 200 && R.Count.GridsSplit == 1 && R.Count.CoverageMissing == 0, "a grid over 128 points is split in two, no cell lost", std::to_string(R.Meshes.size()) + " " + std::to_string(Cells));
        if (R.Meshes.size() == 2)
        {
            const MapMesh &A = R.Meshes[0].Mesh, &B = R.Meshes[1].Mesh;
            Check((A.GroupCount > 2 && A.At(A.GroupCount - 1, 0) == B.At(0, 0) && A.At(A.GroupCount - 1, 1) == B.At(0, 1)) || (A.GroupSize > 2 && A.At(0, A.GroupSize - 1) == B.At(0, 0) && A.At(1, A.GroupSize - 1) == B.At(1, 0)), "the pieces share their border points");
        }
        MapMesh Big;
        Big.GroupCount = 130;
        Big.GroupSize = 130;
        for (int G = 0; G < 130; G++)
            for (int P = 0; P < 130; P++) Big.Points.push_back({ static_cast<double>(G), static_cast<double>(P), 0 });
        const std::vector<MapMesh> Pieces = SplitToLimit(Big, 128);
        size_t Total = 0;
        for (const MapMesh& M : Pieces) Total += static_cast<size_t>(M.GroupCount - 1) * static_cast<size_t>(M.GroupSize - 1);
        Check(Pieces.size() == 4 && Total == 129u * 129u, "a 130 x 130 grid is four pieces covering its 129 x 129 cells", std::to_string(Pieces.size()) + " " + std::to_string(Total));
    }

    // ---- degenerate texture coordinates are swapped for the running coordinate and counted
    {
        const auto Collapsed = [](int, int Corner) {
            RenderCorner C;
            C.U = (Corner == 1 || Corner == 2) ? 1.0f : 0.0f;
            C.V = 0.0f;   // V constant: one singular value of 0
            return C;
        };
        const RenderMeshResult R = BuildRenderMeshes(CellSoup(1, 1, Collapsed), One, Serial);
        Check(R.Meshes.size() == 1 && R.Meshes[0].ReplaceUv && R.Count.DegenerateUv == 1, "a mesh with a collapsed texture axis is marked for the running coordinate and counted", std::to_string(R.Count.DegenerateUv));
        if (!R.Meshes.empty())
        {
            const std::string Swapped = MeshText(R.Meshes[0].Mesh, "m", true), Copied = MeshText(R.Meshes[0].Mesh, "m", false);
            // the running coordinate along a group is 16 per unit: the second point of the second group is 8 units from the first along the group
            Check(Swapped != Copied && Contains(Swapped, " t 128 0"), "the swapped text has the running coordinate (16 texels a unit along the group)", Swapped);
        }
    }

    // ---- copies are removed with a proof, and the sum still adds up
    {
        RenderSoup S = CellSoup(2, 1, Plain);
        // a copy of triangle 0 on vertices of its own (same label, same winding)
        const int Base = static_cast<int>(S.Corners.size());
        for (int K = 0; K < 3; K++)
        {
            const size_t Src = static_cast<size_t>(S.Indices[static_cast<size_t>(K)]) * 3;
            S.Positions.insert(S.Positions.end(), { S.Positions[Src], S.Positions[Src + 1], S.Positions[Src + 2] });
            S.Corners.push_back(RenderCorner());
            S.Indices.push_back(Base + K);
        }
        S.Label.push_back(0);
        S.Front.push_back({ 0, 0, 1 });
        S.Normal.push_back({ 0, 0, 1 });
        const RenderMeshResult R = BuildRenderMeshes(S, One, Serial);
        const RenderMeshCounters& C = R.Count;
        Check(C.TrianglesIn == 5 && C.CopiesRemoved == 1 && C.Covered == 4 && C.CoverageMissing == 0 && C.CoverageLeftover == 0 && C.CopiesRemoved + C.CollapsedByWeld + C.Covered == C.TrianglesIn,
              "an exact copy is removed with a proof: triangles in = covered + removed", std::to_string(C.CopiesRemoved) + " " + std::to_string(C.Covered));
        // decals: the same soup keeps the copy (layers overlap) and only reports
        RenderMeshOptions D = Serial;
        D.Decal = true;
        const RenderMeshResult Dr = BuildRenderMeshes(S, One, D);
        Check(Dr.Count.CopiesRemoved == 0 && Dr.Count.Covered == 5 && Dr.Count.CoverageMissing == 0 && Dr.Count.CoverageLeftover == 0, "a decal soup removes nothing and covers every triangle", std::to_string(Dr.Count.Covered));
    }

    // ---- the side a triangle is drawn on decides what joins: opposite sides never pair or share a mesh; the edge between the two halves of a cell is a border too
    {
        // two triangles on the same four positions, each with vertices of its own, drawn on opposite sides: two meshes, not one cell
        RenderSoup S = CellSoup(1, 1, Plain);
        S.Front[1] = { 0, 0, -1 };
        S.Normal[1] = { 0, 0, -1 };
        const RenderMeshResult Opposite = BuildRenderMeshes(S, One, Serial);
        Check(Opposite.Meshes.size() == 2 && Opposite.Count.CoverageMissing == 0 && Opposite.Count.CoverageLeftover == 0 && Opposite.Count.FacingAgainst == 0, "triangles drawn on opposite sides are never the two halves of one cell", std::to_string(Opposite.Meshes.size()));
        // two cells side by side, the second drawn from the other side: the edge between them is a border by facing
        RenderSoup T = CellSoup(2, 1, Plain);
        for (size_t I = 2; I < 4; I++)
        {
            T.Front[I] = { 0, 0, -1 };
            T.Normal[I] = { 0, 0, -1 };
        }
        const RenderMeshResult Sides = BuildRenderMeshes(T, One, Serial);
        Check(Sides.Meshes.size() == 2 && Sides.Count.BordersCutFacing > 0 && Sides.Count.FacingAgainst == 0, "the edge between triangles drawn on opposite sides is a border by facing", std::to_string(Sides.Meshes.size()) + " " + std::to_string(Sides.Count.BordersCutFacing));
    }
    {
        // the two halves of a cell have vertices of their own at the shared edge; when their colours differ they are not one cell (the mesh gives a point one colour)
        RenderSoup S;
        const float Tri[2][9] = { { 0, 0, 0, 8, 0, 0, 8, 8, 0 }, { 0, 0, 0, 8, 8, 0, 0, 8, 0 } };
        for (int T = 0; T < 2; T++)
        {
            for (int K = 0; K < 3; K++)
            {
                S.Positions.insert(S.Positions.end(), { Tri[T][K * 3], Tri[T][K * 3 + 1], Tri[T][K * 3 + 2] });
                RenderCorner C;
                if (T == 1) C.Color[1] = 100;
                S.Corners.push_back(C);
                S.Indices.push_back(T * 3 + K);
            }
            S.Label.push_back(0);
            S.Front.push_back({ 0, 0, 1 });
            S.Normal.push_back({ 0, 0, 1 });
        }
        const RenderMeshResult R = BuildRenderMeshes(S, One, Serial);
        bool Exact = true;
        for (const RenderMesh& M : R.Meshes)
            for (const MeshPointData& D : M.Mesh.Data) Exact = Exact && (D.G == 255 || D.G == 100);
        Check(R.Meshes.size() == 2 && Exact && R.Count.CoverageMissing == 0, "halves of a cell with different colours at their shared edge are two meshes, each point keeps its colour", std::to_string(R.Meshes.size()));
    }

    // ---- a winding that runs against its neighbour's (the front normal is the same) is a border; a tube that closes on itself is judged cell by cell
    {
        RenderSoup S = CellSoup(2, 1, Plain);
        for (size_t T = 2; T < 4; T++) std::swap(S.Indices[T * 3 + 1], S.Indices[T * 3 + 2]);   // the second cell is wound the other way, drawn on the same side
        const RenderMeshResult R = BuildRenderMeshes(S, One, Serial);
        Check(R.Meshes.size() == 2 && R.Count.BordersCutFacing > 0 && R.Count.CoverageMissing == 0 && R.Count.FacingAgainst == 0, "a cell wound against its neighbour is not laid out in the same mesh", std::to_string(R.Meshes.size()));
        // the two halves of one cell wound against each other (drawn on the same side) are two meshes
        RenderSoup Half = CellSoup(1, 1, Plain);
        std::swap(Half.Indices[4], Half.Indices[5]);
        const RenderMeshResult Halves = BuildRenderMeshes(Half, One, Serial);
        Check(Halves.Meshes.size() == 2 && Halves.Count.CoverageMissing == 0 && Halves.Count.CoverageLeftover == 0, "two triangles wound against each other are not the halves of one cell", std::to_string(Halves.Meshes.size()));
        // eight cells round a tube, every front pointing outwards: their sum is nothing, each cell is judged on its own
        RenderSoup Tube;
        const double Pi = std::acos(-1.0);
        for (int I = 0; I < 8; I++)
        {
            const double A0 = 2 * Pi * I / 8, A1 = 2 * Pi * (I + 1) / 8;
            const float X0 = static_cast<float>(10 * std::cos(A0)), Y0 = static_cast<float>(10 * std::sin(A0)), X1 = static_cast<float>(10 * std::cos(A1)), Y1 = static_cast<float>(10 * std::sin(A1));
            const int Base = static_cast<int>(Tube.Corners.size());
            // (x0, y0, 0) (x1, y1, 0) (x1, y1, 8) (x0, y0, 8): counter-clockwise seen from outside
            for (const float V : { X0, Y0, 0.0f, X1, Y1, 0.0f, X1, Y1, 8.0f, X0, Y0, 8.0f }) Tube.Positions.push_back(V);
            for (int K = 0; K < 4; K++) Tube.Corners.push_back(RenderCorner());
            for (const int K : { 0, 1, 2, 0, 2, 3 }) Tube.Indices.push_back(Base + K);
            const double Mid = (A0 + A1) / 2;
            for (int T = 0; T < 2; T++)
            {
                Tube.Label.push_back(0);
                Tube.Front.push_back({ std::cos(Mid), std::sin(Mid), 0 });
                Tube.Normal.push_back({ std::cos(Mid), std::sin(Mid), 0 });
            }
        }
        const RenderMeshResult Round = BuildRenderMeshes(Tube, One, Serial);
        bool Outwards = !Round.Meshes.empty();
        for (const RenderMesh& M : Round.Meshes)
            for (int G = 0; G + 1 < M.Mesh.GroupCount && Outwards; G++)
                for (int P = 0; P + 1 < M.Mesh.GroupSize; P++)
                {
                    const V3 A = M.Mesh.At(G, P), B = M.Mesh.At(G + 1, P), C = M.Mesh.At(G, P + 1);
                    const V3 N = { (B[1] - A[1]) * (C[2] - A[2]) - (B[2] - A[2]) * (C[1] - A[1]), (B[2] - A[2]) * (C[0] - A[0]) - (B[0] - A[0]) * (C[2] - A[2]), 0 };
                    Outwards = Outwards && (N[0] * A[0] + N[1] * A[1]) > 0;   // the cell's facing points away from the axis
                }
        Check(Outwards && Round.Count.FacingAgainst == 0 && Round.Count.CoverageMissing == 0, "a tube's cells all face outwards and none counts against its normals", std::to_string(Round.Meshes.size()) + " " + std::to_string(Round.Count.FacingAgainst));
    }

    // ---- materials stay apart, and the thread count changes nothing
    {
        RenderSoup S = CellSoup(6, 6, Plain);
        for (size_t T = 0; T < S.Label.size(); T++) S.Label[T] = static_cast<int32_t>((T / 2) % 3 == 0 ? 0 : (T / 2) % 3);
        const std::vector<std::string> Three = { "a", "b", "c" };
        RenderMeshOptions Many = Serial;
        Many.Threads = 4;
        const RenderMeshResult One_ = BuildRenderMeshes(S, Three, Serial), Four = BuildRenderMeshes(S, Three, Many);
        bool Pure = true;
        for (const RenderMesh& M : One_.Meshes) Pure = Pure && M.Label >= 0 && M.Label < 3;
        Check(Pure && One_.Count.Labels == 3 && One_.Count.CoverageMissing == 0 && One_.Count.CoverageLeftover == 0, "materials are meshed apart and each is covered", std::to_string(One_.Count.Labels));
        Check(AllText(One_) == AllText(Four) && One_.Meshes.size() == Four.Meshes.size() && One_.Count.Grids == Four.Count.Grids, "one thread and four give the same meshes in the same order");
    }

    // ---- a copy that differs in front, texture coordinate or colour is other data: it is kept and counted, not removed
    {
        const auto WithCopy = [](const std::function<void(RenderSoup&, int Base)>& Change) {
            RenderSoup S = CellSoup(2, 1, Plain);
            const int Base = static_cast<int>(S.Corners.size());
            for (int K = 0; K < 3; K++)
            {
                const size_t Src = static_cast<size_t>(S.Indices[static_cast<size_t>(K)]) * 3;
                S.Positions.insert(S.Positions.end(), { S.Positions[Src], S.Positions[Src + 1], S.Positions[Src + 2] });
                S.Corners.push_back(RenderCorner());
                S.Indices.push_back(Base + K);
            }
            S.Label.push_back(0);
            S.Front.push_back({ 0, 0, 1 });
            S.Normal.push_back({ 0, 0, 1 });
            Change(S, Base);
            return S;
        };
        const RenderMeshResult Same = BuildRenderMeshes(WithCopy([](RenderSoup&, int) {}), One, Serial);
        const RenderMeshResult Front = BuildRenderMeshes(WithCopy([](RenderSoup& S, int) { S.Front.back() = { 0, 0, -1 }; S.Normal.back() = { 0, 0, -1 }; }), One, Serial);
        const RenderMeshResult Uv = BuildRenderMeshes(WithCopy([](RenderSoup& S, int Base) { S.Corners[static_cast<size_t>(Base)].U = 0.25f; }), One, Serial);
        const RenderMeshResult Colour = BuildRenderMeshes(WithCopy([](RenderSoup& S, int Base) { S.Corners[static_cast<size_t>(Base) + 1].Color[2] = 7; }), One, Serial);
        const auto Sums = [](const RenderMeshResult& R) { return R.Count.CopiesRemoved + R.Count.CollapsedByWeld + R.Count.Covered == R.Count.TrianglesIn && R.Count.CoverageMissing == 0 && R.Count.CoverageLeftover == 0; };
        Check(Same.Count.CopiesRemoved == 1 && Same.Count.CopiesKeptFront + Same.Count.CopiesKeptUv + Same.Count.CopiesKeptColour == 0 && Sums(Same), "a copy equal in front, texture coordinate and colour is removed", std::to_string(Same.Count.CopiesRemoved));
        Check(Front.Count.CopiesRemoved == 0 && Front.Count.CopiesKeptFront == 1 && Sums(Front), "a copy drawn on the other side is kept and counted", std::to_string(Front.Count.CopiesKeptFront) + " " + std::to_string(Front.Count.CopiesRemoved));
        Check(Uv.Count.CopiesRemoved == 0 && Uv.Count.CopiesKeptUv == 1 && Sums(Uv), "a copy with other texture coordinates is kept and counted", std::to_string(Uv.Count.CopiesKeptUv) + " " + std::to_string(Uv.Count.CopiesRemoved));
        Check(Colour.Count.CopiesRemoved == 0 && Colour.Count.CopiesKeptColour == 1 && Sums(Colour), "a copy with another vertex colour is kept and counted", std::to_string(Colour.Count.CopiesKeptColour) + " " + std::to_string(Colour.Count.CopiesRemoved));
    }

    // ---- a failure in one material's worker reaches the caller, with one thread and with several, after every worker has stopped; a bad index is refused before any worker starts
    {
        RenderSoup S = CellSoup(6, 6, Plain);
        for (size_t T = 0; T < S.Label.size(); T++) S.Label[T] = static_cast<int32_t>((T / 2) % 4);
        const std::vector<std::string> Four = { "a", "b", "c", "d" };
        for (const int Threads : { 1, 4 })
        {
            RenderMeshOptions O = Serial;
            O.Threads = Threads;
            O.FailLabel = 2;
            bool Threw = false;
            try
            {
                BuildRenderMeshes(S, Four, O);
            }
            catch (const std::runtime_error&)
            {
                Threw = true;
            }
            Check(Threw, "an exception in a worker is rethrown to the caller", std::to_string(Threads));
        }
        RenderSoup Bad = S;
        Bad.Indices[Bad.Indices.size() / 2] = 100000;
        bool Refused = false;
        try
        {
            BuildRenderMeshes(Bad, Four, Serial);
        }
        catch (const std::invalid_argument&)
        {
            Refused = true;
        }
        Check(Refused, "an index outside the vertices in a multi-material soup is refused");
        // the run is usable again after a failure
        const RenderMeshResult After = BuildRenderMeshes(S, Four, Serial);
        Check(After.Count.Labels == 4 && After.Count.CoverageMissing == 0, "a run after a failed one is unaffected", std::to_string(After.Count.Labels));
    }

    // ---- a texture coordinate too far from its neighbour to shift by whole repeats is a conflict with shift 0, never a conversion of an out-of-range number
    {
        const auto Far = [](int Cell, int Corner) {
            RenderCorner C;
            const float Ux[4] = { 0, 1, 1, 0 };
            C.U = Ux[Corner] + (Cell == 1 ? 3e9f : 0.0f);
            C.V = (Corner == 2 || Corner == 3) ? 1.0f : 0.0f;
            return C;
        };
        const RenderMeshResult R = BuildRenderMeshes(CellSoup(2, 1, Far), One, Serial);
        double Largest = 0;
        std::string Text;
        for (const RenderMesh& M : R.Meshes) Text += MeshText(M.Mesh, "m", M.ReplaceUv, {}, &Largest);
        Check(R.Count.CoverageMissing == 0 && R.Count.CoverageLeftover == 0 && !Contains(Text, "nan") && !Contains(Text, "inf") && std::isfinite(Largest),
              "a texture coordinate 3e9 next to 0 gives finite text and no loss", std::to_string(R.Meshes.size()) + " conflicts " + std::to_string(R.Count.UvConflictCorners) + " shifted " + std::to_string(R.Count.UvTrianglesShifted));
        Check(R.Count.UvTrianglesShifted == 0 && R.Count.UvConflictCorners > 0, "the far coordinate is a counted conflict and no shift is applied", std::to_string(R.Count.UvConflictCorners) + " " + std::to_string(R.Meshes.size()));
    }

    // ---- a number beyond what a long long holds is written as text, not converted (llround of such a number is undefined)
    {
        const std::string Big = RadiantNumber(1e30), Inf = RadiantNumber(std::numeric_limits<double>::infinity()), Small = RadiantNumber(-12.5), Whole = RadiantNumber(3.0);
        Check(Big.find("nan") == std::string::npos && Big.size() > 20 && Inf.find("inf") != std::string::npos && Small == "-12.5" && Whole == "3", "RadiantNumber: huge and infinite numbers do not go through llround", Big + " " + Inf);
    }

    // ---- the candidate cap is per material, so what is capped and the output do not depend on the thread count; a capped material is still fully covered
    {
        RenderSoup S = CellSoup(6, 6, Plain);
        for (size_t T = 0; T < S.Label.size(); T++) S.Label[T] = static_cast<int32_t>((T / 2) % 3);
        const std::vector<std::string> Three = { "a", "b", "c" };
        RenderMeshOptions Capped = Serial;
        Capped.MaxCandidates = 4;   // each material has more than four pairs sharing an edge
        RenderMeshOptions CappedMany = Capped;
        CappedMany.Threads = 4;
        const RenderMeshResult One_ = BuildRenderMeshes(S, Three, Capped), Four = BuildRenderMeshes(S, Three, CappedMany);
        Check(One_.Count.CandidatesCapped == 3 && Four.Count.CandidatesCapped == 3 && One_.Count.MaxCandidatePotential > 4 && One_.Count.MaxCandidatePotential == Four.Count.MaxCandidatePotential,
              "the cap is judged per material: the same materials are capped with one thread and four", std::to_string(One_.Count.CandidatesCapped) + " " + std::to_string(Four.Count.CandidatesCapped));
        Check(AllText(One_) == AllText(Four) && One_.Count.CoverageMissing == 0 && One_.Count.CoverageLeftover == 0 && Four.Count.CoverageMissing == 0, "capped output is equal for one thread and four and loses nothing");
    }

    // ---- a report counter from a large statistic (the largest t of a square with UV up to 1e20) is capped, not converted out of range
    {
        const auto Far = [](int, int Corner) {
            RenderCorner C;
            C.U = Corner == 1 || Corner == 2 ? 1e20f : 0.0f;
            C.V = Corner == 2 || Corner == 3 ? 1e20f : 0.0f;
            return C;
        };
        const RenderMeshResult R = BuildRenderMeshes(CellSoup(1, 1, Far), One, Serial);
        double Largest = 0;
        for (const RenderMesh& M : R.Meshes) MeshText(M.Mesh, "m", M.ReplaceUv, {}, &Largest);
        Check(Largest > 1e20 && ReportCounter(Largest) == 9007199254740992ull && ReportCounter(-1.0) == 0 && ReportCounter(std::nan("")) == 0 && ReportCounter(1e6 + 0.4) == 1000000u && ReportCounter(HUGE_VAL) == 9007199254740992ull,
              "ReportCounter: the largest t of a square with UV up to 1e20 is capped at 2^53, a negative number or NaN is 0", std::to_string(Largest));
    }

    return Failed;
}
}
