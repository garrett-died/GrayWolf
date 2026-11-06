#pragma once

// The drawn triangles of one owner as meshes (patches), unit M2b of the HydraX port (docs/hydrax-port/m2b.md). The caller (the Radiant writer) flattens
// the kept triangles of an owner's surfaces into a RenderSoup; BuildRenderMeshes welds, removes exact copies with a proof, pairs triangles into cells
// (QuadMesher, MapMesh.h), cuts the cells where the render data shows a border, turns every grid to face as the render triangles do, gives every
// point the render data of its own triangle, and checks that every triangle is accounted for (PatchCoverage). MeshText writes a mesh. No file I/O.

#include "MapMesh.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Maps
{
    // What the render data keeps at one vertex
    struct RenderCorner
    {
        float U = 0, V = 0;
        uint8_t Color[4] = { 255, 255, 255, 255 };
    };

    // The kept triangles of one owner, all materials together. Positions and Corners have one entry per vertex (three floats a vertex in Positions);
    // Indices three per triangle; Label (a material id), Front and Normal one per triangle.
    struct RenderSoup
    {
        std::vector<float> Positions;
        std::vector<RenderCorner> Corners;
        std::vector<int32_t> Indices;
        std::vector<int32_t> Label;
        // the side the engine draws, unit or zero when unknown: the vertex normals' sum when the reader gave them, else the game's winding applied to the corners
        std::vector<std::array<double, 3>> Front;
        // the sum of the three vertex normals (zero when the reader gave none): what the facing counters compare with
        std::vector<std::array<double, 3>> Normal;
    };

    struct RenderMeshOptions
    {
        bool Decal = false;                  // decals: no weld, no borders, no removal; the overlap is only reported
        bool VertexOrderBorders = false;     // the trait: the vertex numbering follows the patch compiler's order
        int MaxPoints = 128;                 // points per dimension of a mesh; a bigger grid is split (the compiler's ParsePatch limit, see m2b.md)
        double UvTolerance = 1e-3;           // texture coordinates (modulo the repeat) closer than this are equal
        int Threads = 0;                     // 0: the machine's cores, capped at 8
        uint64_t HashSeed = 0;               // forwarded to the mesher (tests only)
        // Forwarded to the mesher, per material. A material is meshed by one worker and a run has at most 8 (Threads is clamped), so memory is bounded by 8 x this
        // x about 100 bytes (1.6 GB at the default) whatever the thread count, and which material is capped does not depend on it (the output equals for 1 and 4 threads).
        int64_t MaxCandidates = 2000000;
        int FailLabel = -1;                  // tests only: the material whose worker throws, to prove the exception reaches the caller
    };

    struct RenderMeshCounters
    {
        size_t TrianglesIn = 0, CopiesRemoved = 0, CollapsedByWeld = 0, Covered = 0;
        size_t CopiesKeptFront = 0, CopiesKeptUv = 0, CopiesKeptColour = 0;   // triangles with the corners of another that differ in front, texture coordinate or colour: kept
        size_t CellsBothDiagonals = 0, CellsBothDiagonalsNonPlanar = 0;       // cells whose two diagonals are both input triangles (credited to two grids)
        int64_t MaxCandidatePotential = 0;   // the most pairs sharing an edge any one material had (the mesher cap counts these)
        size_t Grids = 0, Fans = 0, LoneTriangles = 0, Cells = 0;
        size_t BordersCutTexture = 0, BordersCutColour = 0, BordersCutFacing = 0;
        size_t Flipped = 0, FacingKept = 0, FacingUnknown = 0, FacingNormal = 0, FacingAgainst = 0, FacingUndecided = 0;
        size_t GridsSplit = 0, Meshes = 0, MeshesColoured = 0;
        size_t DegenerateUv = 0;
        size_t CoverageMissing = 0, CoverageLeftover = 0, LabelsFailed = 0, Labels = 0;
        size_t AccountingFailed = 0;         // owners and kinds whose triangles in are not covered + copies removed + collapsed (set by the writer)
        size_t UvTrianglesShifted = 0;       // triangles whose texture coordinates were moved by whole repeats to agree with their neighbours in the mesh
        size_t UvConflictCorners = 0;        // corners that still disagree with their point after that (a loop of triangles that does not close)
        size_t CandidatesCapped = 0;         // labels whose pairing was skipped by the mesher's cap
        size_t OverlapPartial = 0, OverlapContained = 0, OverlapTested = 0;   // decals: reported, nothing removed
        double MaxAbsT = 0;                  // the largest |t| written (UV x 1024)
        double WeldTolerance = 0;
        std::string WeldReason, NearestText;   // the tolerance's reason and the measured distances (VertexWeld), per owner
        size_t WeldClusters = 0, WeldedVertices = 0;
        double WeldSpan = 0;
        void Add(const RenderMeshCounters& O);
    };

    struct RenderMesh
    {
        MapMesh Mesh;          // points with render data (U, V, colour)
        int Label = 0;
        bool ReplaceUv = false;   // DegenerateMeshUv: write the running coordinate instead of the copied one
    };

    struct RenderMeshResult
    {
        std::vector<RenderMesh> Meshes;       // by label, then in the order the mesher found them
        RenderMeshCounters Count;
        std::vector<std::string> CoverageLines;   // one block per label that failed
        std::vector<std::string> BothDiagonalLines;   // the non-planar cells whose two diagonals are both input, one line each (the first of each material)
    };

    // LabelNames[label] is the material name written on its meshes' Source. Deterministic whatever the thread count.
    RenderMeshResult BuildRenderMeshes(const RenderSoup& Soup, const std::vector<std::string>& LabelNames, const RenderMeshOptions& Options = {});

    // A mesh as the .map writes it
    struct MeshTextOptions
    {
        std::string Prefix;            // before the "mesh" line (the brush number comment and the brace)
        bool NonColliding = true;      // "contents nonColliding;"
        bool ToolFlags = false;        // the stock layout's "toolFlags;" line
        int LightmapSize = 16;         // the third number of the size line
        bool LightmapCoords = true;    // each vertex ends with its plane's texture axes (a texel per 16 units); else "0 0"
    };
    // The text of a mesh named Material; ReplaceUv writes the running coordinate in place of the copied one. Statistics: the largest |t| is returned in MaxAbsT when not null.
    std::string MeshText(const MapMesh& Mesh, const std::string& Material, bool ReplaceUv, const MeshTextOptions& Options = {}, double* MaxAbsT = nullptr);

    // A grid over the point limit as pieces that share their border row and column (the compiler's ParsePatch refuses a dimension over 128)
    std::vector<MapMesh> SplitToLimit(const MapMesh& Mesh, int MaxPoints);

    int RenderMeshesSelfTest(std::string& Failures);
}
