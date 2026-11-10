#pragma once

// Map mesh recovery: the pure geometry that turns a triangle soup into
// real meshes (patches) and to account for every triangle. Ported from HydraX's HydraX.Formats/Map
// (MapMesh, QuadMesher, PatchPairModel, PatchBorderModel, VertexWeld, TriangleOverlap, PatchCoverage,
// DegenerateMeshUv, BrushFaceUv; docs/hydrax-port/m2a.md). No file I/O, no game knowledge, no UI,
// and nothing outside the self-test calls it yet (unit M2a).
//
// Inputs are flat arrays: positions (three floats per vertex), indices (three per triangle) and one
// label per triangle. HydraX keeps its lookups in dictionaries of tuples; here they are sorted keys
// or an open-addressing table, so a 1,000,000 triangle soup runs in seconds. No result depends on
// the order a table is walked in (MeshSelfTest checks this with different table seeds).

#include "MapRadiantGeometry.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Maps
{
    using RadiantGeometry::V3;

    // ---- data

    // Per-point attributes of a mesh, as the render data keeps them. U and V are engine units (repeat = 1).
    struct MeshPointData
    {
        double U = 0, V = 0;
        uint8_t R = 0, G = 0, B = 0, A = 0;
    };

    // A mesh (patch): a grid of GroupCount groups of GroupSize points, group by group. The surface
    // normal is group direction x point direction. Data is empty, or one entry per point.
    struct MapMesh
    {
        int GroupCount = 0, GroupSize = 0;
        std::vector<V3> Points;
        std::vector<MeshPointData> Data;
        std::string Material, Source;
        bool Exact = false;                       // false when the grid layout or the material is a reconstruction
        int TextureWidth = 0, TextureHeight = 0;  // size of the colour map in texels, 0 when not known
        bool NonColliding = false;
        std::string DecalLayer;                   // the layerSortDecal layer a decal sorts in, empty when not a decal

        const V3& At(int Group, int Point) const { return Points[static_cast<size_t>(Group) * GroupSize + Point]; }
        const MeshPointData& DataAt(int Group, int Point) const { return Data[static_cast<size_t>(Group) * GroupSize + Point]; }
        bool HasData() const { return !Data.empty(); }
    };

    // A grid of vertex indices recovered from triangles. Every group has GroupSize points (HydraX keeps
    // a list per group; a mesh needs equal groups, so one flat array holds them).
    struct QuadGrid
    {
        int GroupCount = 0, GroupSize = 0;
        std::vector<int32_t> Points;      // group by group
        bool IsTriangle = false;          // one triangle written as a degenerate cell (no quad partner exists)
        std::vector<int32_t> Triangles;   // the input triangles the cells were made from
        // A fan: lone triangles around one pivot written as the 2 x (n + 1) grid [R0 .. Rn], [P .. P]. Triangles then lists
        // them in cell order (cell i is R(i), R(i+1), P); the pivot copies are separate points.
        bool IsFan = false;
        // A strip: 2 groups of N + 1 points, cell i between points i and i + 1 of both groups. A cell is a quad (two triangles) or one triangle written as a degenerate cell, so the
        // number of triangles a cell has varies: CellStart[i] is where cell i's triangles begin in Triangles (CellStart.size() = cells + 1). Empty for every other grid.
        bool IsStrip = false;
        std::vector<int32_t> CellStart;

        int At(int Group, int Point) const { return Points[static_cast<size_t>(Group) * GroupSize + Point]; }
        std::span<const int32_t> Group(int G) const { return std::span<const int32_t>(Points).subspan(static_cast<size_t>(G) * GroupSize, GroupSize); }

        // A grid from its groups, which must all have the first group's length.
        static QuadGrid FromGroups(const std::vector<std::vector<int32_t>>& Groups, bool IsTriangle = false);
    };

    // ---- the vertex-order models (logistic regressions fitted on one game's stock maps; the caller decides whether they apply)

    namespace PatchPairModel
    {
        // Pairs whose logit is below this are never matched (the cost of a pair is the negative logit)
        constexpr double MinimumLogit = -6.0;

        // The logit that two triangles sharing the edge A-B are one cell. C and D are the triangles' other vertices.
        double Logit(std::span<const float> Positions, int First, int Second, int A, int B, int C, int D, double Squareness);
    }

    namespace PatchBorderModel
    {
        // The logit above which an edge is cut
        constexpr double CutLogit = -0.4009;

        // Scores the edge A-B between two cells (each as its four vertex ids in cyclic order); nullopt when the edge is not a side of both.
        std::optional<double> Logit(std::span<const float> Positions, const std::array<int32_t, 4>& RingX, const std::array<int32_t, 4>& RingY, int A, int B);
    }

    // ---- the mesher

    namespace QuadMesher
    {
        struct BuiltGrid
        {
            QuadGrid Grid;
            int Group = 0;   // the label of its triangles
        };

        struct BuildOptions
        {
            // True when the edge between two triangles (first, second, edge vertex, edge vertex) is a patch border by other evidence
            // (texture, colour): a grid is not extended across it. Empty means no borders.
            std::function<bool(int, int, int, int)> IsBorder;
            // True when the caller knows the vertex numbering follows the order the patch compiler wrote: the pair and border models then rank and cut.
            bool VertexOrderBorders = false;
            // Seeds the lookup tables only. No output may depend on it; MeshSelfTest builds with several seeds and compares.
            uint64_t HashSeed = 0;
            // Optional: false when two triangles (by index) may not be the two halves of one cell, for a reason the positions do not show (they are drawn on opposite sides).
            // Empty means any pair. HydraX has no such test.
            std::function<bool(int, int)> CanPair;
            // The most pairs of triangles sharing an edge Build will rank (memory is about 100 bytes a pair). A soup above it (tens of thousands of triangles on one
            // edge) gets no pairing: every triangle stays lone, BuildResult says so, and coverage still passes. Clamped to 2^30.
            int64_t MaxCandidates = 16000000;
            // Chain the cells that are left in small grids (a rectangle one cell wide or high, a fan, a lone triangle) into strips (docs/hydrax-port/strips.md): 2 x N grids
            // that walk from cell to cell across a shared edge. Rectangles at least two cells wide and high stay as they are. Off by default: the mesher's grids are then
            // exactly HydraX's.
            bool Strips = false;
            // The most points in a row of a strip (a strip of N cells has N + 1); the compiler's ParsePatch refuses a dimension over 128.
            int MaxStripPoints = 128;
            // Like IsBorder, but called on edges the strip step only wants to judge (it may ask about one edge several times), so a caller that counts the borders it finds
            // must not count these. Empty: IsBorder is used.
            std::function<bool(int, int, int, int)> IsBorderQuiet;
        };

        struct BuildResult
        {
            std::vector<BuiltGrid> Grids;
            int64_t OrderScored = 0, OrderCut = 0;   // cell edges the border model scored and cut
            int64_t CandidatePotential = 0;          // pairs of triangles sharing an edge (what MaxCandidates is compared with)
            bool CandidatesCapped = false;           // MaxCandidates was exceeded: no pairing was tried
            int64_t TrianglesLeftLone = 0;           // the triangles that stayed lone because of it (all of them when capped)
            // the strip step (BuildOptions.Strips): grids the small pieces would have made without it, strips of two or more cells, the cells in them, lone triangles and fan
            // triangles that ended in one, and the zero-area cells that joined two strips at a point
            int64_t GridsBeforeStrips = 0, StripsMade = 0, StripCells = 0, StripLoneAbsorbed = 0, StripFanTrianglesAbsorbed = 0, StripStitches = 0;
        };

        // The facing of a grid: the sum over its cells of (next group direction) x (next point direction)
        V3 GridFacing(const QuadGrid& Grid, const std::function<V3(int)>& Position);

        // The grid facing the other way: its groups in the opposite order (same points, same triangles)
        QuadGrid Reversed(const QuadGrid& Grid);

        // Builds grids: pairs of neighbouring triangles that form a convex quad are cells (most rectangular first, then augmenting paths),
        // cells are laid out on integer coordinates by walking shared edges and cut into rectangles; lone triangles that turn around one
        // vertex become fans, the rest degenerate 2x2 meshes. Triangles only pair with the same label.
        // Throws std::invalid_argument for an index outside the positions or fewer labels than triangles. A non-finite position does not throw: as in HydraX
        // its triangles find no partner and stay lone.
        BuildResult Build(std::span<const float> Positions, std::span<const int32_t> Indices, std::span<const int32_t> Group, const BuildOptions& Options = {});
    }

    namespace MeshInternal
    {
        struct CellRect
        {
            int X = 0, Y = 0, W = 0, H = 0;
        };

        // Cuts a set of unit cells into rectangles: each round takes the largest rectangle that starts at a corner cell (no cell left of
        // it or above), growing it width first and height first; ties go to the first corner in (Y, X) order. Exposed for the self-test,
        // which compares it with the plain HydraX loop.
        std::vector<CellRect> CutRectangles(std::span<const std::pair<int, int>> Cells);
    }

    // ---- vertex weld

    // The distribution of the distance from each used vertex to its nearest other vertex at a different position
    struct NearestDistances
    {
        int Vertices = 0;                // distinct vertex ids that triangles use
        int SharedPosition = 0;          // used vertices with exactly the same position as another used vertex id (distance 0)
        std::vector<double> Edges;       // upper edges of the buckets, ascending
        std::vector<int> Counts;         // vertices per bucket (the last entry holds the rest); Edges.size() + 1 entries
        double Maximum = 0;              // largest distance searched

        std::string ToText() const;
    };

    struct WeldResult
    {
        std::vector<int> Representative;  // per vertex id, the id it was welded to (itself when alone)
        int Clusters = 0;                 // clusters with more than one vertex
        int Welded = 0;                   // vertices that now point at another vertex
        double LargestSpan = 0;           // largest distance between two vertices of one cluster of 64 or fewer
    };

    struct ToleranceChoice
    {
        double Tolerance = 0;
        std::string Reason;
    };

    namespace VertexWeld
    {
        // Distances from each used vertex to its nearest other vertex at a different position, bucketed 0.0001 ... 16
        NearestDistances Measure(std::span<const float> Positions, std::span<const int32_t> Indices);

        // The weld tolerance a measured distribution supports: 0.01 when nothing lies between noise (0.0001) and spacing (0.1), else 0
        ToleranceChoice ChooseTolerance(const NearestDistances& Distances);

        // Welds vertices closer than the tolerance (single linkage); the representative of a cluster is its lowest vertex id
        WeldResult Weld(std::span<const float> Positions, std::span<const int32_t> Indices, double Tolerance);
    }

    // ---- exact copies and overlaps

    struct TriangleDuplicate
    {
        int Kept = 0, Removed = 0;
        bool SameWinding = false;   // the corners run in the same cyclic order; false for a back-to-back copy
        bool SameLabel = false;     // a copy with another label is a different surface and is not removed
        bool IsDeletable() const { return SameWinding && SameLabel; }
    };

    struct TrianglePartialOverlap
    {
        int First = 0, Second = 0;
        double Area = 0, FractionOfSmaller = 0;
        bool SameLabel = false, SameSide = true;
        int Smaller = -1;   // the one with the smaller area (the higher index when equal)
        bool IsContained() const { return FractionOfSmaller >= 0.999; }
    };

    struct ContainedRemoval
    {
        int Triangle = 0;
        TrianglePartialOverlap Overlap;
    };

    // Triangle index to its removal proof, ascending by triangle index
    using ProofList = std::vector<std::pair<int, std::string>>;

    struct OverlapResult
    {
        std::vector<TriangleDuplicate> Duplicates;               // ordered by the copy's index
        std::vector<TrianglePartialOverlap> PartialOverlaps;     // ordered by (First, Second)
        int CoplanarPairsTested = 0;

        std::string Summary(const std::string& Label, bool Applied = true) const;
        std::string ToText(size_t Limit = 20000) const;
        // The triangles that lie entirely inside a larger one with the same label and facing, each with the overlap that proves it; ascending by triangle
        std::vector<ContainedRemoval> ContainedRemovals() const;
        ProofList Proofs(std::span<const float> Positions, std::span<const int32_t> Indices) const;
    };

    namespace TriangleOverlap
    {
        constexpr double NormalDot = 0.99995;     // coplanar when the unit normals' dot is at least this ...
        constexpr double PlaneDistance = 0.01;    // ... and the corners of one are at most this far from the plane of the other
        constexpr double MinimumArea = 0.01;      // an overlap smaller than this is rounding at a shared edge

        // Finds exact copies (float bits, no tolerance) and, unless FindPartial is false, partly overlapping coplanar pairs.
        // Throws std::invalid_argument for an index outside the positions or fewer labels than triangles. A non-finite position does not throw (HydraX computes
        // with it: NaN compares false, so its triangle is tested against the triangles in cell 0 and finds no overlap).
        OverlapResult Find(std::span<const float> Positions, std::span<const int32_t> Indices, std::span<const int32_t> Labels, bool FindPartial = true);
    }

    // ---- coverage

    struct CoverageLine
    {
        std::string Kind;     // missing, leftover or removed
        int Triangle = 0;     // the input triangle, -1 for an output cell with no input triangle
        std::string Reason;
    };

    struct CoverageResult
    {
        int InputTriangles = 0, ZeroAreaInput = 0, Meshes = 0, LoneTriangles = 0, Cells = 0, Covered = 0, Removed = 0, Missing = 0, Leftover = 0;
        double InputArea = 0, RemovedArea = 0, OutputArea = 0, OtherDiagonalArea = 0;
        // cells whose two diagonals both have both halves among the input triangles (the surface was tessellated both ways): credited to two grids; and of them the cells whose four
        // points are not planar (a mesh has no diagonal to choose, so the second tessellation is a different surface)
        int CellsBothDiagonals = 0, CellsBothDiagonalsNonPlanar = 0;
        std::vector<CoverageLine> Lines;

        bool IsComplete() const { return Missing == 0 && Leftover == 0; }
        std::string Summary(const std::string& Label) const;
        std::string ToText(const std::string& Label) const;
    };

    namespace PatchCoverage
    {
        // Checks that the grids hold every input triangle once. Reads only the grids' vertex indices and the input triangles, never the mesher's
        // bookkeeping. Removed names the triangles taken out on purpose (with the proof), in any order (a copy is sorted inside when it is not ascending);
        // Representative, when not empty, is the vertex weld and must have an entry per vertex.
        // Throws std::invalid_argument for an index outside the positions (in the input or in a grid), a grid whose Points do not number GroupCount x GroupSize,
        // or a Representative shorter than the vertex count. A non-finite position does not throw: the areas are NaN and the matching goes on by index.
        CoverageResult Check(std::span<const float> Positions, std::span<const int32_t> Indices, std::span<const QuadGrid> Grids, const ProofList& Removed = {}, std::span<const int32_t> Representative = {});
    }

    // ---- texture coordinates

    // Finds meshes whose copied texture coordinates are degenerate and counts them for the report
    class DegenerateMeshUv
    {
    public:
        static constexpr double MaxRatio = 32;           // the median ratio above which a mesh is degenerate
        static constexpr double CollapseFraction = 1e-6; // a singular value below this fraction of the larger one is a collapsed axis

        int Replaced = 0;     // meshes whose texture coordinates were replaced
        double MaxSeen = 0;   // the largest median ratio seen on a replaced mesh (capped at 1e9)

        void Add(const DegenerateMeshUv& Other);
        // Tests one mesh and counts it when degenerate
        bool Check(const MapMesh& Mesh);
        // The area-weighted median singular-value ratio and the share of the area with a collapsed UV axis; false when the mesh has no area or no data
        static bool Measure(const MapMesh& Mesh, double& Median, double& CollapsedShare);
        std::string Line() const;   // the report line, ending in a newline

    private:
        std::map<std::string, int> ByMaterial;
    };

    // A render vertex lying on a brush face: its world position and texture coordinates in repeats
    struct FaceSample
    {
        V3 Position = { 0, 0, 0 };
        double U = 0, V = 0;
    };

    // The six numbers that follow the material on a brush face line
    struct BrushFaceNumbers
    {
        double ScaleX = 0, ScaleY = 0, ShiftX = 0, ShiftY = 0, Rotation = 0, Shear = 0;
    };

    // Counts of how the brush face texture numbers were decided over a whole write
    struct BrushUvReport
    {
        int Solved = 0, NonAxial = 0, NoRenderPoints = 0;

        void Add(const BrushUvReport& Other);
        std::string Line() const;
    };

    namespace BrushFaceUv
    {
        // The texture coordinates a point of a face gets under the six numbers
        std::pair<double, double> Forward(const V3& P1, const V3& P2, const V3& P3, const BrushFaceNumbers& Numbers, const V3& Point);

        // Recovers the six numbers from render vertices on a face (triangle corners in groups of three, the biggest triangles first;
        // each triangle may carry its own whole-number offset), or nullopt when the texture is not an axial projection
        std::optional<BrushFaceNumbers> Solve(const V3& P1, const V3& P2, const V3& P3, std::span<const FaceSample> Samples);
    }

    // ---- self-test (MapMeshSelfTest.cpp)

    int MeshSelfTest(std::string& Failures);
}
