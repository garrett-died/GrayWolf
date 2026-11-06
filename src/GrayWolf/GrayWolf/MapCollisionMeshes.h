#pragma once

// Collision triangles as meshes that carry the drawn surface (M2c). A collision triangle whose three corners are those of a render triangle (HydraX RenderFit's key: positions
// rounded to 1/8 unit, 21 bits an axis, the corners sorted; here the positions are also compared, since the key wraps every 262,144 units) takes that triangle's texture
// coordinates, colour, front and material; the collision soup then goes through BuildRenderMeshes (MapRenderMeshes.h) with Options.Collision. No file I/O, no scene: the
// writer builds the inputs.
//
// A render triangle leaves the render pass only after the mesh that holds its collision triangle has been written with that material and with attributes (TakenFromRenderPass);
// until then it stays in the render pass, non-colliding, so a drawn surface is never lost.

#include "MapRenderMeshes.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Maps
{
    // One drawn triangle of the world that can explain a collision triangle
    struct RenderTriangle
    {
        std::array<float, 9> Position{};       // three corners
        RenderCorner Corner[3];
        std::array<double, 3> Front{ 0, 0, 0 }, Normal{ 0, 0, 0 };
        double Area = 0;
        int32_t Material = -1;                 // an id of the caller's
        bool Decal = false;                    // a decal never explains a collision triangle: it stays in the render pass, non-colliding
        bool DrawnByBrush = false;             // a brush side draws it already: it gives no material (its collision triangle is a tool mesh, so nothing is drawn twice; HydraX differs on purpose)
    };

    struct CollisionTwins
    {
        std::vector<int32_t> Twin;             // per collision triangle: the render triangle it takes its data from, -1 for none
        size_t Twinned = 0;                    // collision triangles with a twin
        size_t Ambiguous = 0;                  // collision triangles with more than one surface candidate
        size_t Exhausted = 0;                  // collision triangles whose candidates were all taken by other collision triangles (one render triangle explains at most one)
        size_t DecalOnly = 0;                  // collision triangles whose only candidates are decals: no twin, a tool mesh
        size_t DecalBeside = 0;                // collision triangles with a surface twin that also have a decal on their corners (the decal stays in the render pass)
        size_t DrawnByBrushOnly = 0;           // collision triangles whose only candidates a brush side draws already: no twin
    };

    // The 1/8 unit, 21 bit key HydraX uses (exposed for the tests): ties to even, as C# Math.Round.
    std::array<uint64_t, 3> TwinKey(std::span<const float> A, std::span<const float> B, std::span<const float> C);

    // CollisionPositions three floats a vertex, Triangles three vertex numbers each (facing out where (b - a) x (c - a) points out). One render triangle explains one collision
    // triangle at most; of several candidates a collision triangle takes, among the unused, the one whose front agrees with its own facing, then the larger area, then the lowest index.
    CollisionTwins MatchTwins(std::span<const float> CollisionPositions, std::span<const uint32_t> Triangles, const std::vector<RenderTriangle>& Render);

    // The soup BuildRenderMeshes takes: every collision triangle with vertices of its own (the data of one corner differs per triangle where patches meet), corners from the
    // twin where its render corner sits on the collision corner (else HasData false), the twin's front and normal (zero without), TwinMaterial. Label = (the collision label where
    // Labels is not empty, -1 unread; the twin's material + 1, or 0 for no twin): triangles of different twin materials, and twinned with untwinned ones, never share a label.
    // LabelOf[i] = (collision label, twin material or -1) for soup label i.
    struct CollisionSoupResult
    {
        RenderSoup Soup;
        std::vector<std::pair<int32_t, int32_t>> LabelOf;
    };
    CollisionSoupResult CollisionSoup(std::span<const float> CollisionPositions, std::span<const uint32_t> Triangles, std::span<const int32_t> Labels, const std::vector<RenderTriangle>& Render,
                                      const CollisionTwins& Twins);

    // Which render triangles leave the render pass: those whose collision triangle was written in a mesh of that render material with attributes. Per render triangle.
    struct RenderTaken
    {
        std::vector<char> Taken;
        size_t Count = 0;
        size_t Stayed = 0;   // twin render triangles (not drawn by a brush) that stay in the render pass: their collision triangle is in no mesh, or in one without that material or attributes
    };
    RenderTaken TakenFromRenderPass(const CollisionTwins& Twins, const std::vector<RenderTriangle>& Render, const RenderMeshResult& Meshes);

    int CollisionMeshesSelfTest(std::string& Failures);
}
