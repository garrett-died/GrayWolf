#pragma once

#include "ByteView.h"

// A level's collision brushes out of its clip map (clipMap_t), the same for every game: where a
// game keeps the fields is its ClipLayout (a fact of the game, written beside its map reader); the
// reading is here. What every Call of Duty keeps since Quake 3: planes, brush sides pointing at
// them, brushes owning a run of sides plus the six axis-aligned sides of their bounding box (kept
// as bounds, not as sides), and brush models (cmodel_t) owning their brushes either through a
// ClipInfo of their own (Black Ops II, Black Ops III) or through the leaf brush node tree of their
// leaf (the other games: a node with brushes lists them, the others point at two children).

#include "MapScene.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Maps
{
    // The tables of brushes: in clipMap_t itself (the older games) or in a ClipInfo (Black Ops II
    // and III: one embedded in clipMap_t, one per brush model). Places from the struct's start; -1
    // when the game has no such field.
    struct ClipInfoLayout
    {
        int64_t MaterialCount = -1, Materials = -1;   // u32, pointer
        int64_t BrushCount = -1, Brushes = -1;        // u16 (BrushCountBytes), pointer
        int BrushCountBytes = 2;
        // the brushes' bounds (Bounds: midPoint, halfSize) and content flags as arrays of their
        // own (IW4, IW5, Black Ops II, III), -1 when a brush keeps its own
        int64_t BrushBounds = -1, BrushContents = -1;
        int64_t LeafBrushNodeCount = -1, LeafBrushNodes = -1;   // u32, pointer to cLeafBrushNode_s
        int64_t PlaneCount = -1, Planes = -1;   // u32, pointer to cplane_s (0x14): where a side's plane number points
    };

    struct ClipLayout
    {
        ClipInfoLayout Info;          // the world's tables, from clipMap_t (+ InfoAt)
        int64_t InfoAt = 0;           // where they are in clipMap_t (an embedded ClipInfo)
        bool InfoPointer = false;     // InfoAt holds a pointer to the ClipInfo (WWII's addon_map_ents)
        // the brushes' tables (Info's BrushCount, Brushes, BrushBounds, BrushContents) and the
        // collision triangles in another asset a ClipInfo points at (Tables, from the ClipInfo's
        // start; WWII: col_map_trzone), from TablesAt in it; -1: in the ClipInfo and clipMap_t
        int64_t Tables = -1, TablesAt = 0;

        // a material: its size, its name (a pointer, or a char[64] kept inline; -1: none), flags (u32)
        int64_t MaterialSize = 0, MaterialName = 0, MaterialSurface = -1, MaterialContents = -1;
        bool MaterialNameInline = false;

        // a brush: its size; bounds (vec3 mins, maxs) and contents (u32) when it keeps them; its
        // side count (u16 or u32) and side pointer; the materials of its six axis-aligned sides
        // (i16[2][3], min then max per axis) or their flags (u32[2][3] contents, then surface)
        int64_t BrushSize = 0, BrushMins = -1, BrushMaxs = -1, BrushContents = -1;
        int64_t BrushSideCount = 0, BrushSides = 0;
        int BrushSideCountBytes = 4;
        int64_t BrushContentsSize = 4;   // a record of Info's BrushContents array, the flags first (WWII 8)
        int64_t AxialMaterial = -1, AxialContents = -1, AxialSurface = -1;
        // the brush's corners: their count (BrushVertexCountBytes: u16 or u32) and a pointer to that many
        // vec3 (-1: the game keeps none read). A brush with them is their convex hull; its sides give
        // the hull's faces their materials and flags.
        int64_t BrushVertexCount = -1, BrushVertices = -1;
        int BrushVertexCountBytes = 4;

        // a side: its size, its plane pointer (cplane_s: normal, dist), its material number (u16
        // or u32) or its flags (u32)
        int64_t SideSize = 0, SidePlane = 0, SideMaterial = -1, SideContents = -1, SideSurface = -1;
        int SideMaterialBytes = 4;
        bool SidePlaneNumber = false;   // the side's plane is a u32 number into the table's planes (AW on)
        bool SidePlaneInline = false;   // the side is its plane, at SidePlane (Black Ops 4)
        // the brushes' side and other pointers are back references the walk left as the game
        // stores them (block, offset), not relocated pointers (Black Ops 4)
        bool BackRefs = false;

        // brush models: count (u32) and array in clipMap_t, a model's size, its ClipInfo pointer
        // (-1: none) and its leaf's leafBrushNode (i32, -1: none)
        int64_t ModelCount = -1, Models = -1, ModelSize = 0, ModelInfo = -1, ModelLeafBrushNode = -1;
        // a model whose ClipInfo pointer is null has no brush (WWII's addon lists every model
        // number, the world's left empty)
        bool ModelNeedsInfo = false;

        // a leaf brush node (cLeafBrushNode_s): its size; leafBrushCount at NodeCount
        // (NodeCountBytes, signed); a node with brushes points at their numbers (NodeBrushBytes
        // each) at NodeBrushes, another at its children, offsets from itself, at NodeChildren.
        // NodeWide: the count i32, the numbers and the children's offsets u32 (Black Ops 4)
        int64_t NodeSize = 0x14, NodeBrushes = 8, NodeChildren = 0x10, NodeCount = 2;
        int NodeCountBytes = 2, NodeBrushBytes = 2;
        bool NodeWide = false;

        // the collision triangles, in clipMap_t (or in Tables): vertex count (u32) and vec3 array,
        // triangle count (u32) and its vertex numbers (three of TriangleIndexBytes each, clockwise
        // seen from outside: read turned round), -1: none read
        int64_t VertexCount = -1, Vertices = -1, TriangleCount = -1, Triangles = -1;
        int TriangleIndexBytes = 2;
        // the partitions that add a base to their triangles' vertex numbers (WWII): count (u32) and
        // array where the triangles are, a record's size, its triangle count (u8) and vertex
        // segment (a u8, or a u16 where PartitionSegmentBytes says: Modern Warfare Remastered; the base is the segment times SegmentVertices) and first triangle (i32);
        // -1: the numbers are the vertices'
        int64_t PartitionCount = -1, Partitions = -1, PartitionSize = 0, PartitionTris = 0, PartitionSegment = 2,
                PartitionFirstTri = 4, SegmentVertices = 0;
        int PartitionSegmentBytes = 1;   // the segment's width: 1 (S2, as before), 2 (H1: measured)
        // A label per triangle from the partitions (Black Ops 4): count (u32) and array in clipMap_t, a record's size, its first triangle (u32), triangle count (u8) and
        // label (u16, an index into a table the game keeps elsewhere, not a material name). -1: the game's partition record holds none read.
        int64_t LabelCount = -1, Labels = -1, LabelSize = 0, LabelFirst = 0, LabelTris = 0, LabelValue = 0;
        // The partitions' clip material from the AABB tree (Infinity Ward's CollisionAabbTree, 32 bytes: midpoint, material index u16, child count u16, half size, then the
        // first child or, in a leaf, the partition number): TreeCount (u32) and array in clipMap_t's info, a record's size. A leaf (child count 0) names one partition and the
        // clip material (an index into the info's materials) of all its triangles. -1: none read.
        int64_t TreeCount = -1, Trees = -1, TreeSize = 0, TreeMaterial = 0, TreeChildren = 0, TreePartition = 0;
        // count, per brush, whether the union of its sides' contents (through the clip map's materials) equals its contents word: collision_union_match / _subset / _superset / _other
        bool CheckContentsUnion = false;
    };

    // The AABB tree's leaves (Treyarch and Infinity Ward alike, ClipLayout::Tree*) read from Img: per partition the clip material index of the first leaf that names it (-1: none,
    // or the index out of range) and that leaf's record, and the triangles over the partitions reached. Counted in Counters (collision_tree_*): leaves, partitions reached twice
    // and of them with another material, unreached, out of range, material out of range.
    struct TreeWalk
    {
        std::vector<int32_t> Material;
        std::vector<int64_t> Node;
        uint64_t LeafTriangles = 0;
    };
    TreeWalk WalkCollisionTree(ByteView Img, const ClipLayout& L, int64_t Parts, uint32_t PartCount, int64_t Trees, uint32_t TreeCount, size_t MaterialCount, std::map<std::string, size_t>& Counters);
    // Where the records of a table start when Count of them of RecordSize bytes fit in Img from Where on; else -1, Count 0 and collision_table_count_bad counted (a bad count is never walked).
    int64_t BoundedTable(ByteView Img, int64_t Where, uint32_t& Count, int64_t RecordSize, std::map<std::string, size_t>& Counters);

    // The served zone a layout is read in: its bytes, its pointer width, and where a relocated
    // pointer's data is (-1: not in the zone).
    struct ClipSource
    {
        const ByteView* Image = nullptr;
        int PointerSize = 8;
        std::function<int64_t(uint64_t Address)> Off;
        std::function<std::string(uint64_t Address)> String;   // a relocated string pointer's text
    };

    // Out.Triggers from the MapTriggers at Triggers (in MapEnts since IW4; Black Ops III): a count
    // and five pointer-wide fields after it (models, hull count, hulls, slab count, slabs);
    // TriggerModel 8 (contents, u16 hull count, u16 first hull), TriggerHull 0x20 (Bounds: midPoint,
    // halfSize; contents 0x18, u16 slab count 0x1c, u16 first slab 0x1e), TriggerSlab 0x14 (dir,
    // midPoint, halfSize: the volume between two planes). A hull is a brush: its box and its slabs.
    // A title that widened the records says so in a TriggerLayout (WWII: models 0x10, the hull
    // count at 8; hulls 0x28, the slab count at 0x20).
    struct TriggerLayout
    {
        int64_t ModelSize = 8, ModelHullCount = 4, ModelFirstHull = 6;
        int64_t HullSize = 0x20, HullContents = 0x18, HullSlabCount = 0x1c, HullFirstSlab = 0x1e;
    };
    bool ReadTriggers(Collision& Out, const ClipSource& Src, int64_t Triggers, const TriggerLayout& T = {});

    // Fills Out from the clipMap_t at ClipMap; Counters says what could not be read
    // (collision_*). False when the clip map holds no brush.
    bool ReadCollision(Collision& Out, const ClipSource& Src, int64_t ClipMap, const ClipLayout& L,
                       std::map<std::string, size_t>& Counters);
}
