#include "stdafx.h"

#include "MapCollision.h"

#include "ExportNaming.h"

#include <set>
#include <vector>

namespace Maps
{
namespace
{
    using ExportNaming::At;

    class Reader
    {
    public:
        Reader(const ClipSource& Src, const ClipLayout& L, std::map<std::string, size_t>& Counters)
            : Src(Src), Img(*Src.Image), L(L), Counters(Counters)
        {
        }

        uint64_t Pointer(int64_t Pos) const
        {
            return Src.PointerSize == 4 ? (uint64_t)At<uint32_t>(Img, Pos) : At<uint64_t>(Img, Pos);
        }
        int64_t Follow(int64_t Pos) const
        {
            const uint64_t P = Pointer(Pos);
            return P ? Src.Off(P) : -1;
        }

        // One table of brushes (clipMap_t's or a ClipInfo's), Info its start
        struct Table
        {
            int64_t Brushes = -1, Bounds = -1, Contents = -1, Nodes = -1, Planes = -1;
            int64_t Tables = -1;   // where the brushes' tables are (ClipLayout::Tables)
            uint32_t BrushCount = 0, NodeCount = 0, PlaneCount = 0;
            std::vector<std::string> Materials;
            std::vector<std::pair<uint32_t, uint32_t>> MaterialFlags;   // surface, contents
        };
        Table Open(int64_t Info) const
        {
            Table T;
            const ClipInfoLayout& I = L.Info;
            T.Tables = Info;
            if (L.Tables >= 0)
            {
                const int64_t Holder = Follow(Info + L.Tables);
                T.Tables = Holder >= 0 ? Holder + L.TablesAt : -1;
            }
            const int64_t B = T.Tables;
            if (B >= 0 && I.BrushCount >= 0) T.BrushCount = I.BrushCountBytes == 4 ? At<uint32_t>(Img, B + I.BrushCount) : At<uint16_t>(Img, B + I.BrushCount);
            if (B >= 0 && I.Brushes >= 0) T.Brushes = Follow(B + I.Brushes);
            if (B >= 0 && I.BrushBounds >= 0) T.Bounds = Follow(B + I.BrushBounds);
            if (B >= 0 && I.BrushContents >= 0) T.Contents = Follow(B + I.BrushContents);
            if (I.LeafBrushNodes >= 0) T.Nodes = Follow(Info + I.LeafBrushNodes);
            if (I.LeafBrushNodeCount >= 0) T.NodeCount = At<uint32_t>(Img, Info + I.LeafBrushNodeCount);
            if (I.Planes >= 0) T.Planes = Follow(Info + I.Planes);
            if (I.PlaneCount >= 0) T.PlaneCount = At<uint32_t>(Img, Info + I.PlaneCount);
            const int64_t Mats = I.Materials >= 0 ? Follow(Info + I.Materials) : -1;
            const uint32_t MatCount = I.MaterialCount >= 0 ? At<uint32_t>(Img, Info + I.MaterialCount) : 0;
            for (uint32_t K = 0; Mats >= 0 && K < MatCount && K < 65536; K++)
            {
                const int64_t M = Mats + (int64_t)K * L.MaterialSize;
                std::string Name;
                if (L.MaterialNameInline)
                {
                    for (int64_t C = 0; C < 64 && M + L.MaterialName + C < (int64_t)Img.size(); C++)
                    {
                        const char Ch = (char)Img[(size_t)(M + L.MaterialName + C)];
                        if (Ch == 0) break;
                        Name += Ch;
                    }
                }
                else if (L.MaterialName >= 0)
                    Name = Src.String(Pointer(M + L.MaterialName));
                T.Materials.push_back(Name);
                T.MaterialFlags.emplace_back(L.MaterialSurface >= 0 ? At<uint32_t>(Img, M + L.MaterialSurface) : 0,
                                             L.MaterialContents >= 0 ? At<uint32_t>(Img, M + L.MaterialContents) : 0);
            }
            return T;
        }

        bool ReadBrush(const Table& T, uint32_t Index, Brush& Out) const
        {
            if (T.Brushes < 0 || Index >= T.BrushCount) return false;
            const int64_t B = T.Brushes + (int64_t)Index * L.BrushSize;
            if (B + L.BrushSize > (int64_t)Img.size()) return false;
            float Mins[3], Maxs[3];
            for (int K = 0; K < 3; K++)
            {
                if (L.BrushMins >= 0)
                {
                    Mins[K] = At<float>(Img, B + L.BrushMins + K * 4);
                    Maxs[K] = At<float>(Img, B + L.BrushMaxs + K * 4);
                }
                else
                {
                    // Bounds: midPoint, halfSize
                    const int64_t Bo = T.Bounds + (int64_t)Index * 24;
                    const float Mid = At<float>(Img, Bo + K * 4), Half = At<float>(Img, Bo + 12 + K * 4);
                    Mins[K] = Mid - Half;
                    Maxs[K] = Mid + Half;
                }
            }
            if (L.BrushMins < 0 && T.Bounds < 0) return false;
            Out.Contents = L.BrushContents >= 0 ? At<uint32_t>(Img, B + L.BrushContents)
                         : T.Contents >= 0      ? At<uint32_t>(Img, T.Contents + (int64_t)Index * L.BrushContentsSize)
                                                : 0;
            const auto Named = [&T](BrushSide& S, uint32_t Material) {
                if (Material >= T.Materials.size()) return;
                S.Material = T.Materials[Material];
                S.Surface = T.MaterialFlags[Material].first;
                S.Contents = T.MaterialFlags[Material].second;
            };
            // the bounding box's six sides: min then max per axis, as the game keeps their materials
            for (int Side = 0; Side < 2; Side++)
                for (int K = 0; K < 3; K++)
                {
                    BrushSide S;
                    S.Normal[0] = S.Normal[1] = S.Normal[2] = 0;
                    S.Normal[K] = Side ? 1.0f : -1.0f;
                    S.Dist = Side ? Maxs[K] : -Mins[K];
                    if (L.AxialMaterial >= 0) Named(S, (uint32_t)At<int16_t>(Img, B + L.AxialMaterial + (Side * 3 + K) * 2));
                    if (L.AxialContents >= 0) S.Contents = At<uint32_t>(Img, B + L.AxialContents + (Side * 3 + K) * 4);
                    if (L.AxialSurface >= 0) S.Surface = At<uint32_t>(Img, B + L.AxialSurface + (Side * 3 + K) * 4);
                    Out.Sides.push_back(S);
                }
            const uint32_t Count = L.BrushSideCountBytes == 2 ? At<uint16_t>(Img, B + L.BrushSideCount) : At<uint32_t>(Img, B + L.BrushSideCount);
            const int64_t Sides = Count ? Follow(B + L.BrushSides) : -1;
            if (Count && Sides < 0)
            {
                Counters["collision_brush_sides_unread"]++;
                return false;
            }
            for (uint32_t K = 0; K < Count && K < 4096; K++)
            {
                const int64_t S = Sides + (int64_t)K * L.SideSize;
                int64_t Plane = -1;
                if (L.SidePlaneInline)
                    Plane = S + L.SidePlane;
                else if (!L.SidePlaneNumber)
                    Plane = Follow(S + L.SidePlane);
                else if (const uint32_t Number = At<uint32_t>(Img, S + L.SidePlane); T.Planes >= 0 && Number < T.PlaneCount)
                    Plane = T.Planes + (int64_t)Number * 0x14;
                if (Plane < 0 || Plane + 16 > (int64_t)Img.size())
                {
                    Counters["collision_planes_unread"]++;
                    return false;
                }
                BrushSide Out1;
                for (int J = 0; J < 3; J++) Out1.Normal[J] = At<float>(Img, Plane + J * 4);
                Out1.Dist = At<float>(Img, Plane + 12);
                if (L.SideMaterial >= 0)
                    Named(Out1, L.SideMaterialBytes == 2 ? At<uint16_t>(Img, S + L.SideMaterial) : At<uint32_t>(Img, S + L.SideMaterial));
                if (L.SideContents >= 0) Out1.Contents = At<uint32_t>(Img, S + L.SideContents);
                if (L.SideSurface >= 0) Out1.Surface = At<uint32_t>(Img, S + L.SideSurface);
                Out.Sides.push_back(Out1);
            }
            if (L.CheckContentsUnion)
            {
                uint32_t Union = 0;
                for (const BrushSide& Side : Out.Sides) Union |= Side.Contents;
                Counters[Union == Out.Contents ? "collision_union_match" : (Union & Out.Contents) == Union ? "collision_union_subset" : (Union & Out.Contents) == Out.Contents ? "collision_union_superset" : "collision_union_other"]++;
            }
            // the corners: a brush whose pointer or count does not read keeps its sides alone
            if (L.BrushVertexCount >= 0 && L.BrushVertices >= 0)
            {
                const uint32_t Corners = L.BrushVertexCountBytes == 2 ? At<uint16_t>(Img, B + L.BrushVertexCount) : At<uint32_t>(Img, B + L.BrushVertexCount);
                if (Corners)
                {
                    const int64_t V = Follow(B + L.BrushVertices);
                    if (V >= 0 && Corners <= 65536 && V + (int64_t)Corners * 12 <= (int64_t)Img.size())
                    {
                        Out.Vertices.resize((size_t)Corners * 3);
                        for (size_t K = 0; K < Out.Vertices.size(); K++) Out.Vertices[K] = At<float>(Img, V + (int64_t)K * 4);
                        Counters["collision_brush_vertices_read"]++;
                    }
                    else
                        Counters["collision_brush_vertices_unread"]++;
                }
                else
                    Counters["collision_brush_vertices_none"]++;
            }
            return true;
        }

        // The brushes a leaf brush node reaches: a node with brushes lists their numbers, another
        // points at its two children (offsets from itself)
        std::set<uint32_t> NodeBrushes(const Table& T, int32_t First) const
        {
            std::set<uint32_t> Out;
            if (T.Nodes < 0 || First < 0 || (uint32_t)First >= T.NodeCount) return Out;
            std::vector<uint32_t> Stack = { (uint32_t)First };
            std::set<uint32_t> Seen;
            while (!Stack.empty())
            {
                const uint32_t N = Stack.back();
                Stack.pop_back();
                if (N >= T.NodeCount || !Seen.insert(N).second) continue;
                const int64_t P = T.Nodes + (int64_t)N * L.NodeSize;
                const int32_t Count = (L.NodeWide || L.NodeCountBytes == 4) ? At<int32_t>(Img, P + L.NodeCount) : At<int16_t>(Img, P + L.NodeCount);
                if (Count > 0)
                {
                    const int64_t List = Follow(P + L.NodeBrushes);
                    for (int32_t K = 0; List >= 0 && K < Count && K < 65536; K++)
                        Out.insert((L.NodeWide || L.NodeBrushBytes == 4) ? At<uint32_t>(Img, List + K * 4) : At<uint16_t>(Img, List + K * 2));
                    if (List < 0) Counters["collision_node_brushes_unread"]++;
                    continue;
                }
                for (int K = 0; K < 2; K++)
                    if (const uint32_t Off = L.NodeWide ? At<uint32_t>(Img, P + L.NodeChildren + K * 4) : At<uint16_t>(Img, P + L.NodeChildren + K * 2))
                        Stack.push_back(N + Off);
            }
            return Out;
        }

    private:
        const ClipSource& Src;
        ByteView Img;
        const ClipLayout& L;
        std::map<std::string, size_t>& Counters;
    };
}

int64_t BoundedTable(ByteView Img, int64_t Where, uint32_t& Count, int64_t RecordSize, std::map<std::string, size_t>& Counters)
{
    if (Where < 0) Count = 0;   // a null or unread pointer: no records, whatever the count says
    if (Count == 0) return -1;
    if (RecordSize <= 0 || Where >= static_cast<int64_t>(Img.size()) || static_cast<int64_t>(Count) > (static_cast<int64_t>(Img.size()) - Where) / RecordSize)
    {
        Counters["collision_table_count_bad"]++;
        Count = 0;
        return -1;
    }
    return Where;
}

TreeWalk WalkCollisionTree(ByteView Img, const ClipLayout& L, int64_t Parts, uint32_t PartCount, int64_t Trees, uint32_t TreeCount, size_t MaterialCount, std::map<std::string, size_t>& Counters)
{
    TreeWalk W;
    W.Material.assign(PartCount, -1);
    W.Node.assign(PartCount, -1);
    std::vector<uint8_t> Reached(PartCount, 0);
    for (uint32_t K = 0; Trees >= 0 && Parts >= 0 && K < TreeCount; K++)
    {
        const int64_t Node = Trees + (int64_t)K * L.TreeSize;
        if (Node + L.TreeSize > (int64_t)Img.size()) break;
        if (At<uint16_t>(Img, Node + L.TreeChildren) != 0) continue;
        Counters["collision_tree_leaves"]++;
        const uint32_t Part = At<uint32_t>(Img, Node + L.TreePartition);
        const uint32_t Material = At<uint16_t>(Img, Node + L.TreeMaterial);
        if (Part >= PartCount)
        {
            Counters["collision_tree_partition_out_of_range"]++;
            continue;
        }
        if (Reached[Part]++)
        {
            Counters["collision_tree_partition_reached_twice"]++;
            if (W.Material[Part] != static_cast<int32_t>(Material)) Counters["collision_tree_partition_material_conflict"]++;
            continue;   // the first leaf in the table keeps it
        }
        if (Material >= MaterialCount)
            Counters["collision_tree_material_out_of_range"]++;
        else
            W.Material[Part] = static_cast<int32_t>(Material);
        W.Node[Part] = Node;
        W.LeafTriangles += At<uint8_t>(Img, Parts + (int64_t)Part * L.PartitionSize + L.PartitionTris);
    }
    for (uint32_t P = 0; P < PartCount; P++)
        if (!Reached[P]) Counters["collision_tree_partition_unreached"]++;
    if (Trees < 0) Counters["collision_tree_unread"]++;
    return W;
}

bool ReadCollision(Collision& Out, const ClipSource& Src, int64_t ClipMap, const ClipLayout& L, std::map<std::string, size_t>& Counters)
{
    if (!Src.Image || ClipMap < 0) return false;
    const auto& Img = *Src.Image;
    Reader R(Src, L, Counters);
    const int64_t Info = L.InfoPointer ? R.Follow(ClipMap + L.InfoAt) : ClipMap + L.InfoAt;
    if (Info < 0) return false;
    const Reader::Table World = R.Open(Info);
    if (L.Tables >= 0 && World.Tables < 0) Counters["collision_tables_unread"]++;
    auto Triggers = std::move(Out.Triggers);   // read before, kept
    Out = Collision();
    Out.Triggers = std::move(Triggers);
    Out.ClipBrushes = World.BrushCount;
    for (size_t K = 0; K < World.Materials.size(); K++)
    {
        Out.Materials.emplace_back(World.Materials[K], World.MaterialFlags[K].second);
        Out.MaterialSurface.push_back(World.MaterialFlags[K].first);
    }

    // the brush models: their own tables, or the brushes their leaf's node tree reaches
    const uint32_t ModelCount = L.ModelCount >= 0 ? At<uint32_t>(Img, ClipMap + L.ModelCount) : 0;
    const int64_t Models = L.Models >= 0 && ModelCount ? R.Follow(ClipMap + L.Models) : -1;
    std::set<uint32_t> Owned;   // the world's brushes a model owns (node trees)
    if (Models >= 0) Out.Models.resize(ModelCount);
    for (uint32_t M = 1; Models >= 0 && M < ModelCount && M < 65536; M++)
    {
        const int64_t Rec = Models + (int64_t)M * L.ModelSize;
        std::vector<Brush>& Into = Out.Models[M];
        // a ClipInfo of its own with brushes (Black Ops III; Black Ops II has the field, null; CODOL
        // one naming the world's brushes), else the brushes its leaf's node tree reaches in the world's
        const int64_t Own = L.ModelInfo >= 0 ? R.Follow(Rec + L.ModelInfo) : -1;
        if (L.ModelNeedsInfo && Own < 0) continue;
        if (const Reader::Table T = Own >= 0 ? R.Open(Own) : Reader::Table(); T.BrushCount && T.Brushes != World.Brushes)
        {
            Out.ClipBrushes += T.BrushCount;
            for (uint32_t B = 0; B < T.BrushCount; B++)
            {
                Brush Br;
                if (R.ReadBrush(T, B, Br)) Into.push_back(std::move(Br));
                else Counters["collision_brushes_unread"]++;
            }
            continue;
        }
        if (L.ModelLeafBrushNode < 0) continue;
        for (const uint32_t B : R.NodeBrushes(World, At<int32_t>(Img, Rec + L.ModelLeafBrushNode)))
        {
            Brush Br;
            if (!R.ReadBrush(World, B, Br))
            {
                Counters["collision_brushes_unread"]++;
                continue;
            }
            Owned.insert(B);
            Into.push_back(std::move(Br));
        }
    }
    for (uint32_t B = 0; B < World.BrushCount; B++)
    {
        if (Owned.count(B)) continue;
        Brush Br;
        if (R.ReadBrush(World, B, Br)) Out.Brushes.push_back(std::move(Br));
        else Counters["collision_brushes_unread"]++;
    }
    // the collision triangles, in clipMap_t or in the asset holding the brushes' tables
    const int64_t Holder = L.Tables < 0 ? ClipMap : World.Tables >= 0 ? World.Tables - L.TablesAt : -1;
    const uint32_t Vertices = Holder >= 0 && L.VertexCount >= 0 ? At<uint32_t>(Img, Holder + L.VertexCount) : 0;
    const uint32_t Triangles = Holder >= 0 && L.TriangleCount >= 0 ? At<uint32_t>(Img, Holder + L.TriangleCount) : 0;
    const int64_t V = Vertices ? R.Follow(Holder + L.Vertices) : -1, T = Triangles ? R.Follow(Holder + L.Triangles) : -1;
    // A table of Count records at the pointer: the count is what the zone can hold, or the table is unread (a bad count is counted, never walked). Returns where the records start, -1 for none.
    const auto TableAt = [&](int64_t CountAt, int64_t PointerAt, int64_t RecordSize, uint32_t& Count) -> int64_t {
        Count = At<uint32_t>(Img, Holder + CountAt);
        return Count == 0 ? -1 : BoundedTable(Img, R.Follow(Holder + PointerAt), Count, RecordSize, Counters);
    };
    // the partition table is read once (a bad count is counted once)
    uint32_t PartTableCount = 0;
    const int64_t PartTable = L.PartitionCount >= 0 && Holder >= 0 ? TableAt(L.PartitionCount, L.Partitions, L.PartitionSize, PartTableCount) : -1;
    // a clip material index per partition, from the AABB tree's leaves; -1 where no leaf names the partition (unknown, never zero). Checked and counted: every partition must be
    // reached by a leaf (WalkCollisionTree), the triangles over the leaves must add up to the triangle count, every material index must be in range.
    std::vector<int32_t> PartitionMaterial, TriangleMaterial;
    std::vector<int64_t> PartitionNode;
    if (L.PartitionCount >= 0 && L.TreeCount >= 0 && Triangles)
    {
        const uint32_t PartCount = PartTableCount;
        const int64_t Parts = PartTable;
        uint32_t TreeCount = 0;
        const int64_t Trees = TableAt(L.TreeCount, L.Trees, L.TreeSize, TreeCount);
        TreeWalk W = WalkCollisionTree(Img, L, Parts, PartCount, Trees, TreeCount, World.Materials.size(), Counters);
        if (W.LeafTriangles != Triangles) Counters["collision_tree_triangle_sum_mismatch"]++;
        Counters["collision_tree_leaf_triangles"] += W.LeafTriangles;
        PartitionMaterial = std::move(W.Material);
        PartitionNode = std::move(W.Node);
        TriangleMaterial.assign(Triangles, -1);
    }
    // each triangle's vertex base, from the partition holding it
    std::vector<uint32_t> Base;
    if (L.PartitionCount >= 0 && Triangles)
    {
        Base.assign(Triangles, 0);
        const uint32_t Count = PartTableCount;
        const int64_t Parts = PartTable;
        for (uint32_t P = 0; Parts >= 0 && P < Count; P++)
        {
            const int64_t Rec = Parts + (int64_t)P * L.PartitionSize;
            const uint32_t First = At<uint32_t>(Img, Rec + L.PartitionFirstTri), N = At<uint8_t>(Img, Rec + L.PartitionTris);
            const uint32_t Segment = (L.PartitionSegmentBytes == 2 ? At<uint16_t>(Img, Rec + L.PartitionSegment) : At<uint8_t>(Img, Rec + L.PartitionSegment)) * (uint32_t)L.SegmentVertices;
            for (uint32_t K = First; K - First < N && K < Triangles; K++) Base[K] = Segment;
            if (!TriangleMaterial.empty() && P < PartitionMaterial.size())
                for (uint32_t K = First; K - First < N && K < Triangles; K++) TriangleMaterial[K] = PartitionMaterial[P];
        }
        if (Parts < 0) Counters["collision_partitions_unread"]++;
    }
    if (V >= 0 && T >= 0 && V + (int64_t)Vertices * 12 <= (int64_t)Img.size() &&
        T + (int64_t)Triangles * 3 * L.TriangleIndexBytes <= (int64_t)Img.size())
    {
        Out.Vertices.resize((size_t)Vertices * 3);
        for (size_t K = 0; K < Out.Vertices.size(); K++) Out.Vertices[K] = At<float>(Img, V + (int64_t)K * 4);
        // stored clockwise seen from outside, as Direct3D draws (every game measured, CoD4 to
        // Black Ops 4: against the drawn surface's normals where one lies on the triangle):
        // turned round to face out along (b - a) x (c - a)
        for (uint32_t K = 0; K < Triangles; K++)
        {
            uint32_t I[3];
            for (uint32_t J = 0; J < 3; J++)
                I[J] = (L.TriangleIndexBytes == 2 ? At<uint16_t>(Img, T + (K * 3 + J) * 2) : At<uint32_t>(Img, T + (K * 3 + J) * 4)) +
                       (Base.empty() ? 0 : Base[K]);
            if (I[0] >= Vertices || I[1] >= Vertices || I[2] >= Vertices)
            {
                // a number outside the vertices: loud, never read as segment 0 (the counters are in the report; the rest of the triangles are not read)
                Counters["collision_triangles_unread"] += Triangles - K;
                Counters["collision_index_out_of_range"]++;
                break;
            }
            Out.Triangles.insert(Out.Triangles.end(), { I[0], I[2], I[1] });
        }
        // the leaf's bounds (midpoint at 0, half size at 16) contain its partition's triangles: counted, never assumed
        if (!PartitionNode.empty() && L.PartitionCount >= 0)
        {
            const uint32_t PartCount = PartTableCount;
            const int64_t Parts = PartTable;
            const size_t TriangleTotal = Out.Triangles.size() / 3;
            for (uint32_t P = 0; Parts >= 0 && P < PartCount && P < PartitionNode.size(); P++)
            {
                if (PartitionNode[P] < 0) continue;
                const int64_t Rec = Parts + (int64_t)P * L.PartitionSize;
                const uint32_t First = At<uint32_t>(Img, Rec + L.PartitionFirstTri), N = At<uint8_t>(Img, Rec + L.PartitionTris);
                float Mid[3], Half[3];
                for (int A = 0; A < 3; A++) Mid[A] = At<float>(Img, PartitionNode[P] + A * 4), Half[A] = At<float>(Img, PartitionNode[P] + 16 + A * 4);
                for (uint32_t K = First; K - First < N && K < TriangleTotal; K++)
                {
                    bool Inside = true;
                    for (int J = 0; J < 3; J++)
                        for (int A = 0; A < 3; A++)
                            Inside = Inside && std::fabs(Out.Vertices[static_cast<size_t>(Out.Triangles[K * 3 + static_cast<size_t>(J)]) * 3 + static_cast<size_t>(A)] - Mid[A]) <= Half[A] + 0.05f;
                    Counters[Inside ? "collision_tree_triangles_inside_leaf_bounds" : "collision_tree_triangles_outside_leaf_bounds"]++;
                }
            }
        }
        if (!TriangleMaterial.empty())
        {
            TriangleMaterial.resize(Out.Triangles.size() / 3, -1);
            Out.TriangleLabel = std::move(TriangleMaterial);
            Out.TriangleLabelIsClipMaterial = true;
        }
        // a label per triangle from the partitions that hold one (LabelCount); a triangle no partition names stays -1 (unknown, never zero)
        if (L.LabelCount >= 0 && !Out.Triangles.empty())
        {
            const size_t TriangleTotal = Out.Triangles.size() / 3;
            Out.TriangleLabel.assign(TriangleTotal, -1);
            Out.TriangleLabelIsClipMaterial = true;   // the labels index the clip map's materials (names, contents and surface words of the per-map table)
            uint32_t Count = 0;
            const int64_t Parts = TableAt(L.LabelCount, L.Labels, L.LabelSize, Count);
            for (uint32_t P = 0; Parts >= 0 && P < Count; P++)
            {
                const int64_t Rec = Parts + (int64_t)P * L.LabelSize;
                const uint32_t First = At<uint32_t>(Img, Rec + L.LabelFirst), N = At<uint8_t>(Img, Rec + L.LabelTris);
                const int32_t Label = At<uint16_t>(Img, Rec + L.LabelValue);
                if (static_cast<uint64_t>(First) + N > TriangleTotal)
                {
                    // a partition outside the triangles: no label is trusted (HydraX reads none for the whole map)
                    Out.TriangleLabel.assign(TriangleTotal, -1);
                    Counters["collision_labels_unread"]++;
                    break;
                }
                for (uint32_t K = First; K < First + N; K++) Out.TriangleLabel[K] = Label;
            }
            if (Parts < 0) Counters["collision_labels_unread"]++;
        }
    }
    else if (Triangles)
        Counters["collision_triangles_unread"] += Triangles;
    return !Out.Empty();
}
}

namespace Maps
{
bool ReadTriggers(Collision& Out, const ClipSource& Src, int64_t Triggers, const TriggerLayout& L)
{
    if (!Src.Image || Triggers < 0) return false;
    const auto& Img = *Src.Image;
    const int P = Src.PointerSize;
    const auto Follow = [&](int64_t Pos) {
        const uint64_t A = P == 4 ? (uint64_t)At<uint32_t>(Img, Pos) : At<uint64_t>(Img, Pos);
        return A ? Src.Off(A) : -1;
    };
    const uint32_t Count = At<uint32_t>(Img, Triggers), HullCount = At<uint32_t>(Img, Triggers + 2 * P),
                   SlabCount = At<uint32_t>(Img, Triggers + 4 * P);
    const int64_t Models = Follow(Triggers + P), Hulls = Follow(Triggers + 3 * P), Slabs = Follow(Triggers + 5 * P);
    if (Models < 0 || Hulls < 0 || Count > 65536) return false;
    Out.Triggers.assign(Count, {});
    for (uint32_t M = 0; M < Count; M++)
    {
        const int64_t Model = Models + (int64_t)M * L.ModelSize;
        const uint16_t N = At<uint16_t>(Img, Model + L.ModelHullCount), First = At<uint16_t>(Img, Model + L.ModelFirstHull);
        for (uint32_t H = First; H < (uint32_t)First + N && H < HullCount; H++)
        {
            const int64_t Hull = Hulls + (int64_t)H * L.HullSize;
            Brush B;
            B.Contents = At<uint32_t>(Img, Hull + L.HullContents);
            for (int Side = 0; Side < 2; Side++)
                for (int K = 0; K < 3; K++)
                {
                    const float Mid = At<float>(Img, Hull + K * 4), Half = At<float>(Img, Hull + 12 + K * 4);
                    BrushSide S;
                    S.Normal[0] = S.Normal[1] = S.Normal[2] = 0;
                    S.Normal[K] = Side ? 1.0f : -1.0f;
                    S.Dist = Side ? Mid + Half : -(Mid - Half);
                    B.Sides.push_back(S);
                }
            const uint16_t SlabN = At<uint16_t>(Img, Hull + L.HullSlabCount), SlabFirst = At<uint16_t>(Img, Hull + L.HullFirstSlab);
            for (uint32_t S = SlabFirst; Slabs >= 0 && S < (uint32_t)SlabFirst + SlabN && S < SlabCount; S++)
            {
                const int64_t Slab = Slabs + (int64_t)S * 0x14;
                const float Mid = At<float>(Img, Slab + 12), Half = At<float>(Img, Slab + 16);
                BrushSide Up, Down;
                for (int K = 0; K < 3; K++) Up.Normal[K] = At<float>(Img, Slab + K * 4), Down.Normal[K] = -Up.Normal[K];
                Up.Dist = Mid + Half;
                Down.Dist = -(Mid - Half);
                B.Sides.push_back(Up);
                B.Sides.push_back(Down);
            }
            Out.Triggers[M].push_back(std::move(B));
        }
    }
    return true;
}
}
