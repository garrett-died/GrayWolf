#include "stdafx.h"

#include "MapRadiantWriter.h"

#include "ExportNaming.h"
#include "GdtWriter.h"
#include "MapFullExport.h"
#include "MapLive.h"
#include "MapMaterials.h"
#include "MapRadiantGeometry.h"
#include "MapRadiantRules.h"
#include "SurfaceTypes.h"
#include "MapRadiantTextures.h"
#include "MaterialGdt.h"
#include "MapSceneWriter.h"
#include "WraithModel.h"
#include "XMBExport.h"
#include "XMEExport.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Maps
{
namespace
{
    using namespace RadiantGeometry;

    constexpr double OnPlane = 1e-5;     // a point this close to a plane is on it (the planes are exact)
    constexpr double Huge = 262144.0;    // the half size of a side's starting polygon

    struct Plane
    {
        V3 N;
        double D;
    };

    // Q3's BaseWindingForPlane: a square on the plane larger than any level
    std::vector<V3> BaseWinding(const Plane& P)
    {
        int Major = 0;
        for (int K = 1; K < 3; K++)
            if (std::fabs(P.N[K]) > std::fabs(P.N[Major])) Major = K;
        V3 Up = Major == 2 ? V3{ 1, 0, 0 } : V3{ 0, 0, 1 };
        Up = Sub(Up, Mul(P.N, Dot(Up, P.N)));
        Up = Mul(Up, 1.0 / Length(Up));
        const V3 Right = Cross(Up, P.N);
        const V3 Org = Mul(P.N, P.D);
        const V3 U = Mul(Up, Huge), R = Mul(Right, Huge);
        return { Add(Sub(Org, R), U), Add(Add(Org, R), U), Sub(Add(Org, R), U), Sub(Sub(Org, R), U) };
    }

    // ---- the text

    std::string Num(double V) { return RadiantNumber(V); }

    // Three points of a face (Quake's order: (p0 - p1) x (p2 - p1) points out), as written (Num:
    // four decimals): three of its corners, snapped to whole units when that keeps the plane, else
    // as they are, else (a sliver of a face, whose corners give its plane badly) three points far
    // apart on the plane. Kept: the plane they give passes within 0.0005 of every corner; with
    // Brush (the brush's corners), its distance to each of them within 0.0005 of the true plane's
    // (a sliver's plane tilted by its corners' rounding moves the corners of its neighbours), the
    // points closest to that when none does.
    double Written(double V) { return std::round(V * 1e4) / 1e4; }

    std::array<V3, 3> ThreePoints(const BrushFace& F, const Plane& P, const std::vector<V3>* Brush = nullptr)
    {
        const auto& W = F.Points;
        const auto Out = [&P](std::array<V3, 3> Pts) {
            if (Dot(Cross(Sub(Pts[0], Pts[1]), Sub(Pts[2], Pts[1])), P.N) < 0) std::swap(Pts[0], Pts[2]);
            return Pts;
        };
        // how far the plane the points give strays from the true one (-1: not that plane)
        const auto Strays = [&W, &P, Brush](const std::array<V3, 3>& Pts) {
            V3 N = Cross(Sub(Pts[0], Pts[1]), Sub(Pts[2], Pts[1]));
            const double L = Length(N);
            if (L <= 0) return -1.0;
            N = Mul(N, 1.0 / L);
            if (Dot(N, P.N) < (Brush ? 0.0 : 0.99)) return -1.0;
            const double D = Dot(N, Pts[1]);
            double Most = 0;
            for (const V3& V : Brush ? *Brush : W) Most = (std::max)(Most, std::fabs(Dot(N, V) - D - (Brush ? Dot(P.N, V) - P.D : 0.0)));
            return Most;
        };
        const auto Keeps = [&Strays](const std::array<V3, 3>& Pts) {
            const double S = Strays(Pts);
            return S >= 0 && S <= 0.0005;
        };
        const V3 A = W[0];
        size_t Bi = 1;
        for (size_t K = 1; K < W.size(); K++)
            if (Length(Sub(W[K], A)) > Length(Sub(W[Bi], A))) Bi = K;
        const V3 B = W[Bi];
        const V3 Dir = Mul(Sub(B, A), 1.0 / Length(Sub(B, A)));
        size_t Ci = 0;
        double Best = -1;
        for (size_t K = 0; K < W.size(); K++)
        {
            const V3 D = Sub(W[K], A);
            const double Off = Length(Sub(D, Mul(Dir, Dot(D, Dir))));
            if (Off > Best) Best = Off, Ci = K;
        }
        const std::array<V3, 3> Corners = Out({ A, B, W[Ci] });
        // a tiny or skinny face (corners spanning a cross product under 16, or a sine under 0.1 at the first:
        // Radiant's "brush plane with no normal" and "degenerate brush") is written with three points of its
        // plane 64 units apart around its centre
        const bool Tiny = IsTinyFace(A, B, W[Ci]);
        for (const double Snap : { 0.002, 0.0 })
        {
            if (Tiny) break;
            std::array<V3, 3> Pts = Corners;
            for (auto& Pt : Pts)
                for (auto& C : Pt) C = std::fabs(C - std::round(C)) < Snap ? std::round(C) : Written(C);
            if (Keeps(Pts)) return Pts;
        }
        // far apart on the plane, around the face's centre
        V3 Centre = { 0, 0, 0 };
        for (const V3& V : W) Centre = Add(Centre, Mul(V, 1.0 / (double)W.size()));
        std::array<V3, 3> Closest{};
        double Least = -1;
        for (const double Far : Tiny ? std::vector<double>{ TinyFacePointSpacing, 4096.0, 65536.0 } : std::vector<double>{ 4096.0, 65536.0 })
        {
            const V3 U = Mul(Dir, Far), Vv = Mul(Cross(P.N, Dir), Far);
            std::array<V3, 3> Pts = Out({ Add(Centre, U), Centre, Add(Centre, Vv) });
            for (auto& Pt : Pts)
                for (auto& C : Pt) C = Written(C);
            if (!Brush) return Pts;
            const double S = Strays(Pts);
            if (S >= 0 && S <= 0.0005) return Pts;
            if (Least < 0 || (S >= 0 && S < Least)) Closest = Pts, Least = S;
        }
        return Closest;
    }

    // The written brush as a reader of the .map gets it back (radiant_check's test): each side's
    // plane from its three points as written, its face that plane clipped by the others (a point
    // within 0.001 of a plane on it); sound when every side bounds a face, the faces close and
    // enclose a volume
    bool Sound(const std::vector<std::array<V3, 3>>& Sides)
    {
        constexpr double Eps = 0.001;
        std::vector<Plane> Planes;
        for (const auto& Pts : Sides)
        {
            std::array<V3, 3> Read;
            for (int K = 0; K < 3; K++)
                for (int C = 0; C < 3; C++) Read[K][C] = std::strtod(Num(Pts[K][C]).c_str(), nullptr);
            V3 N = Cross(Sub(Read[0], Read[1]), Sub(Read[2], Read[1]));
            const double L = Length(N);
            if (L <= 0) return false;
            N = Mul(N, 1.0 / L);
            Planes.push_back({ N, Dot(N, Read[1]) });
        }
        std::vector<std::vector<V3>> Polys;
        V3 Centre = { 0, 0, 0 };
        size_t Corners = 0;
        for (size_t I = 0; I < Planes.size(); I++)
        {
            std::vector<V3> W = BaseWinding(Planes[I]);
            for (size_t J = 0; J < Planes.size() && W.size() >= 3; J++)
                if (J != I) W = Clip(W, Planes[J].N, Planes[J].D, Eps);
            if (W.size() < 3) return false;
            for (const V3& V : W) Centre = Add(Centre, V);
            Corners += W.size();
            Polys.push_back(std::move(W));
        }
        Centre = Mul(Centre, 1.0 / (double)Corners);
        V3 Total = { 0, 0, 0 };
        double Surface = 0, Volume = 0;
        for (size_t I = 0; I < Polys.size(); I++)
        {
            V3 A = { 0, 0, 0 };
            double Vol = 0;
            const auto& W = Polys[I];
            for (size_t K = 1; K + 1 < W.size(); K++)
            {
                const V3 C = Cross(Sub(W[K], W[0]), Sub(W[K + 1], W[0]));
                A = Add(A, Mul(C, 0.5));
                Vol += Dot(Sub(W[0], Centre), C) / 6.0;
            }
            // each face counter-clockwise seen from outside
            if (Dot(A, Planes[I].N) < 0) A = Mul(A, -1), Vol = -Vol;
            Total = Add(Total, A);
            Surface += Length(A);
            Volume += Vol;
        }
        return Length(Total) <= 1e-3 * (std::max)(Surface, 1.0) && Volume > 0;
    }

    bool Starts(const std::string& S, const char* Prefix) { return S.rfind(Prefix, 0) == 0; }

    // The tool material a brush is written with: a side's own when the clip map names one per side
    // and it is a tool material (IW), else the one Black Ops III's mod tools give the brush's
    // content flags (Quake's lineage, the same bits from CoD4 to Black Ops). A drawn surface's
    // brush (Black Ops: bullets and missiles, 0x2080; CoD4: solid with a drawn material) stood for
    // geometry now drawn by a misc_model: clip_full, which blocks everything and seals nothing.
    class ToolNames
    {
    public:
        explicit ToolNames(const Collision& C) : Keywords(C.ContentsKeywords)
        {
            for (const auto& [Name, Contents] : C.Materials)
                if (Sky.empty() && Starts(ExportNaming::Lower(Name), "sky")) Sky = ExportNaming::Lower(Name);
            if (Sky.empty()) Sky = "sky";
        }
        std::string Of(const Brush& B, const BrushSide& S) const
        {
            std::string M = ExportNaming::Lower(S.Material);
            if (RadiantTool(M)) return M;
            constexpr uint32_t Solid = 0x1, Glass = 0x10, CanShootClip = 0x40, MissileClip = 0x80, VehicleClip = 0x200,
                               ItemClip = 0x400, SkyFlag = 0x800, NoSight = 0x1000, ClipShot = 0x2000, PlayerClip = 0x10000,
                               MonsterClip = 0x20000, Mantle = 0x1000000, Detail = 0x8000000, Structural = 0x10000000,
                               Trigger = 0x40000000;
            const uint32_t C = B.Contents & ~Detail;
            if (C & Trigger) return "trigger";
            if (C & SkyFlag) return Sky;
            if (C & Solid) return M.empty() || RadiantTool(M) ? "caulk" : "clip_full";
            const bool Player = C & PlayerClip, Monster = C & MonsterClip;
            if (Player && Monster) return C & NoSight ? "clip_nosight" : "clip";
            if (Player) return "clip_player";
            if (Monster) return "clip_ai";
            if ((C & ClipShot) && (C & MissileClip)) return "clip_full";
            if (C & Mantle) return "mantle_on";
            if (C & Glass) return "clip";
            if (C & VehicleClip) return "clip_vehicle";
            if (C & ItemClip) return "clip_physics";
            if (C & NoSight) return "nosight_noclip";
            if (C & ClipShot) return "clip_weapon";
            if (C & MissileClip) return "clip_missile";
            if (C & CanShootClip) return "clip_weapon";
            if (C & Structural) return "hint";
            return "clip";
        }

        // the game's contents bits are Radiant's `contents ...;` keywords (Black Ops III)
        const bool Keywords;

    private:
        std::string Sky;
    };

    // A drawn material's name in the .map: as the model files name it (its GDT asset name with a
    // GDT, which then lists it)
    std::string MaterialName(const std::string& Material, RadiantGdt* Gdt)
    {
        if (!Gdt) return Material.empty() ? "default" : ExportNaming::SafeFileName(Material);
        Gdt->Materials.insert(Material);
        return MaterialGdt::AssetName(Material);
    }

    // The drawn triangles a brush's solid sides take their materials from (none: tool materials
    // only): Owner's (DrawnWorld::Owner), counted in Stats; the GDT naming them
    struct Texturing
    {
        DrawnWorld* Drawn = nullptr;
        RadiantGdt* Gdt = nullptr;
        uint32_t Owner = 0;
        RadiantTextureStats* Stats = nullptr;
        const DrawnWorld::HiddenTest* Hidden = nullptr;   // inside another brush (the world's)
    };

    // The world's solid brushes, to tell a point inside one (what hides a side's face): each
    // brush's planes, its bounds in a grid of Cell units
    class Solids
    {
    public:
        explicit Solids(const Collision& C)
        {
            for (const Brush& B : C.Brushes)
            {
                if (!(B.Contents & 1)) continue;
                const std::vector<BrushFace> Faces = BrushFaces(B);
                if (Faces.size() < 4) continue;
                std::vector<Plane> P;
                for (const BrushSide& S : B.Sides)
                {
                    const double L = std::sqrt((double)S.Normal[0] * S.Normal[0] + (double)S.Normal[1] * S.Normal[1] + (double)S.Normal[2] * S.Normal[2]);
                    if (L > 0.5) P.push_back({ { S.Normal[0] / L, S.Normal[1] / L, S.Normal[2] / L }, S.Dist / L });
                }
                V3 Lo = Faces[0].Points[0], Hi = Lo;
                for (const BrushFace& F : Faces)
                    for (const V3& V : F.Points)
                        for (int A = 0; A < 3; A++) Lo[A] = (std::min)(Lo[A], V[A]), Hi[A] = (std::max)(Hi[A], V[A]);
                const uint32_t Number = (uint32_t)Planes.size();
                Planes.push_back(std::move(P));
                for (int64_t X = CellOf(Lo[0]); X <= CellOf(Hi[0]); X++)
                    for (int64_t Y = CellOf(Lo[1]); Y <= CellOf(Hi[1]); Y++)
                        for (int64_t Z = CellOf(Lo[2]); Z <= CellOf(Hi[2]); Z++) Grid[Key(X, Y, Z)].push_back(Number);
            }
        }
        bool Inside(const V3& P) const
        {
            const auto It = Grid.find(Key(CellOf(P[0]), CellOf(P[1]), CellOf(P[2])));
            if (It == Grid.end()) return false;
            for (const uint32_t B : It->second)
            {
                bool In = true;
                for (const Plane& S : Planes[B]) In = In && Dot(S.N, P) - S.D <= 0.01;
                if (In) return true;
            }
            return false;
        }

    private:
        static constexpr double Cell = 512.0;
        static int64_t CellOf(double V) { return (int64_t)std::floor(V / Cell); }
        static uint64_t Key(int64_t X, int64_t Y, int64_t Z)
        {
            constexpr int64_t Bias = 1 << 20;
            return ((uint64_t)(X + Bias) << 42) | ((uint64_t)(Y + Bias) << 21) | (uint64_t)(Z + Bias);
        }
        std::vector<std::vector<Plane>> Planes;
        std::unordered_map<uint64_t, std::vector<uint32_t>> Grid;
    };

    // The faces a brush writes, with the three points of each as written
    struct Solved
    {
        std::vector<BrushFace> Faces;
        std::vector<std::array<V3, 3>> Points;
    };

    // The plane of a face (unit normal, distance)
    Plane PlaneOf(const BrushFace& F) { return { F.Normal, F.Dist }; }

    // The three points of each face of Faces, Radiant's reading of them sound as a brush; false when
    // they are not (the second try checks the points against the whole brush)
    bool SolveHull(const std::vector<BrushFace>& Faces, Solved& Out)
    {
        for (int Try = 0; Try < 2; Try++)
        {
            std::vector<V3> Corners;
            for (const BrushFace& F : Faces) Corners.insert(Corners.end(), F.Points.begin(), F.Points.end());
            Out.Faces = Faces;
            Out.Points.clear();
            for (const BrushFace& F : Faces) Out.Points.push_back(ThreePoints(F, PlaneOf(F), Try ? &Corners : nullptr));
            if (Sound(Out.Points)) return true;
        }
        return false;
    }

    // Writes one brush, Origin and Axis moving it (a brush model's, to its entity), all its sides
    // of material Only when set; false when its planes bound no closed volume. A brush that keeps
    // its corners is their convex hull (split into pieces within Radiant's 64 faces and 64 points,
    // else listed and not written); the sides give the hull's faces their material, a face on no
    // side takes the brush's most common one. A brush without corners, or whose hull is no solid, is
    // its sides clipped by one another. Its solid sides (caulk, clip_full) lying on Tex's drawn
    // triangles take their material, its other solid sides then caulk.
    bool WriteBrush(std::string& Out, const Brush& B, size_t Number, const float* Origin, const float* Axis, const ToolNames& Tools,
                    const std::string& Only, RadiantStats& Stats, const Texturing& Tex = {})
    {
        Brush Moved = B;
        const std::span<const float> Ax(Axis, Axis ? 9 : 0), Or(Origin, Origin ? 3 : 0);
        if (Origin)
        {
            for (auto& S : Moved.Sides)
            {
                // a plane N . p = D moved by p' = Axis^T p + O: N' = Axis^T N, D' = D + N' . O
                const float N[3] = { S.Normal[0], S.Normal[1], S.Normal[2] };
                for (int K = 0; K < 3; K++) S.Normal[K] = N[0] * Ax[K] + N[1] * Ax[3 + K] + N[2] * Ax[6 + K];
                S.Dist += S.Normal[0] * Or[0] + S.Normal[1] * Or[1] + S.Normal[2] * Or[2];
            }
            for (size_t K = 0; K + 2 < Moved.Vertices.size(); K += 3)
            {
                const float P[3] = { Moved.Vertices[K], Moved.Vertices[K + 1], Moved.Vertices[K + 2] };
                for (int J = 0; J < 3; J++) Moved.Vertices[K + J] = P[0] * Ax[J] + P[1] * Ax[3 + J] + P[2] * Ax[6 + J] + Or[J];
            }
        }
        const std::string Keywords = Tools.Keywords && Only.empty() ? ContentsKeywords(Moved.Contents) : std::string();

        // the sides' planes (unit normal, distance), to match hull faces to
        std::vector<std::pair<V3, double>> SidePlanes;
        std::vector<size_t> SideOf;
        for (size_t K = 0; K < Moved.Sides.size(); K++)
        {
            const BrushSide& S = Moved.Sides[K];
            const double L = std::sqrt((double)S.Normal[0] * S.Normal[0] + (double)S.Normal[1] * S.Normal[1] + (double)S.Normal[2] * S.Normal[2]);
            if (L <= 0.5) continue;
            SidePlanes.push_back({ { S.Normal[0] / L, S.Normal[1] / L, S.Normal[2] / L }, S.Dist / L });
            SideOf.push_back(K);
        }

        // the faces to write: pieces of the hull of the corners, or the sides' faces
        std::vector<Solved> Pieces;
        bool FromHull = false;
        if (!Moved.Vertices.empty())
        {
            std::vector<V3> Corners;
            for (size_t K = 0; K + 2 < Moved.Vertices.size(); K += 3) Corners.push_back({ Moved.Vertices[K], Moved.Vertices[K + 1], Moved.Vertices[K + 2] });
            const std::vector<HullFace> Hull = HullFaces(Corners);
            if (Hull.empty())
                Stats.BrushesHullNone++;
            else
            {
                std::vector<std::vector<V3>> Parts;
                if (WithinRadiantLimits(Hull))
                    Parts.push_back(Corners);
                else
                {
                    Split S = SplitForRadiant(Corners, &Hull);
                    if (S.Pieces.empty())
                    {
                        Stats.BrushesOverLimit++;
                        Stats.Unresolved.push_back("brush " + std::to_string(Number) + " (" + std::to_string(Hull.size()) + " faces, " +
                                                   std::to_string(Corners.size()) + " vertices): not written, " + S.Failure);
                        return false;
                    }
                    Stats.BrushesSplit++;
                    Parts = std::move(S.Pieces);
                }
                bool AllSound = true;
                for (const auto& Part : Parts)
                {
                    std::vector<BrushFace> Faces;
                    for (const HullFace& H : HullFaces(Part))
                    {
                        BrushFace F;
                        F.Normal = H.Normal, F.Dist = H.Dist;
                        const int Side = MatchSide(H, SidePlanes);
                        F.Side = Side < 0 ? (size_t)-1 : SideOf[(size_t)Side];
                        for (const size_t I : H.Index) F.Points.push_back(Part[I]);
                        Faces.push_back(std::move(F));
                    }
                    Solved Piece;
                    if (Faces.size() < 4 || !SolveHull(Faces, Piece))
                    {
                        AllSound = false;
                        break;
                    }
                    Pieces.push_back(std::move(Piece));
                }
                if (AllSound)
                    FromHull = true;
                else
                {
                    Pieces.clear();
                    if (Parts.size() > 1)
                    {
                        Stats.BrushesOverLimit++;
                        Stats.Unresolved.push_back("brush " + std::to_string(Number) + ": a piece of its split is not a sound brush, not written");
                        return false;
                    }
                    Stats.BrushesHullUnsound++;   // the sides' faces instead
                }
            }
        }
        if (!FromHull)
        {
            // the faces and their three points; when the brush they write is not sound, the points
            // checked against the whole brush, then also the faces with slivers up to 0.1 thick left out
            Solved Piece;
            std::vector<BrushFace> Faces;
            bool Written = false;
            for (int Try = 0; Try < 3 && !Written; Try++)
            {
                if (Try != 1) Faces = BrushFaces(Moved, Try == 0 ? 0.01 : 0.1);
                if (Faces.size() < 4) break;
                std::vector<V3> Corners;
                for (const BrushFace& F : Faces) Corners.insert(Corners.end(), F.Points.begin(), F.Points.end());
                Piece.Faces = Faces;
                Piece.Points.clear();
                for (const BrushFace& F : Faces) Piece.Points.push_back(ThreePoints(F, PlaneOf(F), Try ? &Corners : nullptr));
                Written = Sound(Piece.Points);
            }
            if (!Written)
            {
                (Faces.size() < 4 ? Stats.BrushesDropped : Stats.BrushesUnsound)++;
                return false;
            }
            Pieces.push_back(std::move(Piece));
        }
        else
        {
            Stats.BrushesHull++;
            Stats.BrushPieces += Pieces.size();
        }

        // each piece: its sides' materials and projections, then the text
        for (const Solved& Piece : Pieces)
        {
            const std::vector<BrushFace>& Faces = Piece.Faces;
            const std::vector<std::array<V3, 3>>& Points = Piece.Points;
            std::vector<std::string> Names(Faces.size());
            std::map<std::string, size_t> Counts;
            for (size_t K = 0; K < Faces.size(); K++)
                if (Faces[K].Side != (size_t)-1)
                {
                    Names[K] = Only.empty() ? Tools.Of(Moved, Moved.Sides[Faces[K].Side]) : Only;
                    Counts[Names[K]]++;
                }
            // a face on no side: the brush's most common material (else the brush's own tool material)
            std::string Common;
            size_t Most = 0;
            for (const auto& [Name, N] : Counts)
                if (N > Most) Common = Name, Most = N;
            if (Common.empty()) Common = Only.empty() ? Tools.Of(Moved, Moved.Sides.empty() ? BrushSide() : Moved.Sides[0]) : Only;
            for (size_t K = 0; K < Faces.size(); K++)
                if (Faces[K].Side == (size_t)-1)
                {
                    Names[K] = Common;
                    Stats.HullFacesUnmatched++;
                }
                else if (FromHull)
                    Stats.HullFacesMatched++;
            std::vector<SideTexture> Textures(Faces.size());
            std::vector<char> Textured(Faces.size(), 0);
            if (Tex.Drawn && Tex.Stats)
            {
                RadiantTextureStats& TS = *Tex.Stats;
                const auto Solid = [](const std::string& Name) { return Name == "caulk" || Name == "clip_full"; };
                bool Any = false;
                for (size_t K = 0; K < Faces.size(); K++)
                {
                    if (!Solid(Names[K])) continue;
                    TS.Sides++;
                    const V3 N = Faces[K].Normal;
                    // the normal Radiant reads back from the three points as written
                    std::array<V3, 3> Read;
                    for (int J = 0; J < 3; J++)
                        for (int C = 0; C < 3; C++) Read[J][C] = std::strtod(Num(Points[K][J][C]).c_str(), nullptr);
                    V3 Back = Cross(Sub(Read[0], Read[1]), Sub(Read[2], Read[1]));
                    Back = Mul(Back, 1.0 / (std::max)(Length(Back), 1e-12));
                    if (Tex.Drawn->Texture(Faces[K].Points, N, Faces[K].Dist, Back, Tex.Owner, Tex.Hidden, Textures[K], TS)) Textured[K] = 1, Any = true;
                }
                if (Any)
                {
                    for (size_t K = 0; K < Faces.size(); K++)
                        if (!Textured[K] && Solid(Names[K])) Names[K] = "caulk";
                    TS.BrushesTextured++;
                }
            }
            Out += "// brush " + std::to_string(Number) + "\n{\n";
            if (!Keywords.empty()) Out += "  contents " + Keywords + ";\n";
            for (size_t K = 0; K < Faces.size(); K++)
            {
                const auto& Pts = Points[K];
                Out += " ";
                for (const V3& Pt : Pts) Out += "( " + Num(Pt[0]) + " " + Num(Pt[1]) + " " + Num(Pt[2]) + " ) ";
                if (Textured[K])
                {
                    const SideTexture& T = Textures[K];
                    Out += MaterialName(T.Material, Tex.Gdt) + " " + Num(T.Width) + " " + Num(T.Height) + " " + Num(T.ShiftX) + " " + Num(T.ShiftY) + " " + Num(T.Rotation) +
                           " 0 lightmap_gray 16384 16384 0 0 0 0\n";
                }
                else
                    Out += Names[K] + " 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n";
            }
            Out += "}\n";
            Stats.Faces += Faces.size();
        }
        Stats.Brushes++;
        return true;
    }

    // What becomes of drawn triangle K (indices 3K..3K+2) of a surface no brush side draws: a
    // patch, nothing (no area: drawn as nothing), or a misc_model (no material to give a patch,
    // or a value Radiant cannot read)
    enum class Fate { Patch, None, Model };
    Fate FateOf(const Surface& S, size_t K)
    {
        const uint32_t* I = &S.Indices[K * 3];
        if (I[0] >= S.Vertices.size() || I[1] >= S.Vertices.size() || I[2] >= S.Vertices.size()) return Fate::None;
        V3 P[3];
        for (int J = 0; J < 3; J++)
        {
            const Vertex& V = S.Vertices[I[J]];
            for (const float F : { V.Pos[0], V.Pos[1], V.Pos[2], V.Uv[0], V.Uv[1] })
                if (!std::isfinite(F)) return Fate::Model;
            P[J] = { V.Pos[0], V.Pos[1], V.Pos[2] };
        }
        if (Length(Cross(Sub(P[1], P[0]), Sub(P[2], P[0]))) < 1e-6) return Fate::None;
        return S.Material.empty() ? Fate::Model : Fate::Patch;
    }

    // The drawn triangles of Owner's meshes (DrawnWorld::Owner) no brush side draws, each a patch:
    // a 2x2 mesh whose fourth vertex repeats the second, one triangle whichever way Radiant splits
    // the cell, facing as the game's (a compiled mesh's triangles face against (row 1 - row 0) x
    // (column 1 - column 0)); nonColliding (the clip map's brushes collide); each vertex its
    // position, colour ("c", left out when white), UV ("t": UV x 1024) and lightmap coordinates
    // (the plane's texture axes, a texel per 16 units, as "2 2 16 8" says)
    enum class PatchKind { All, NotDecal, Decal };
    // Whether a surface's material is a decal, as MaterialUsage sees it (Maps::IsDecalMaterial: the techset's name, or its renderFlags where the name is a hash)
    bool IsDecalSurface(const MapScene& Scene, const Surface& S)
    {
        return Maps::IsDecalMaterial(Scene, S.Material);
    }

#ifdef GW_MESH_DUMP
    std::string EnvText(const char* Name)
    {
        const DWORD Size = GetEnvironmentVariableA(Name, nullptr, 0);
        if (Size == 0) return std::string();
        std::string Text(Size, ' ');
        GetEnvironmentVariableA(Name, Text.data(), Size);
        Text.resize(Size - 1);
        return Text;
    }
#endif

    // One pass of WritePatches: the kept triangles of one owner's surfaces (decals, or not) as a RenderSoup, through the mesher (MapRenderMeshes.h), written as meshes
    // (nonColliding: the clip map's brushes collide). Each vertex keeps its position, colour ("c", left out when white), UV ("t": UV x 1024) and the lightmap
    // coordinates of the mesh's plane (a texel per 16 units, as "16 8" in the size line says).
    void WritePatchPass(std::string& Out, const MapScene& Scene, const DrawnWorld& Drawn, uint32_t Owner, RadiantGdt* Gdt, RadiantStats& Stats, size_t& Number, bool Decal)
    {
        const int Winding = WindingOf(Scene.Game);
        RenderSoup Soup;
        std::vector<std::string> Names;
        std::map<std::string, int> Ids;
        for (size_t Oi = 0; Oi < Scene.Objects.size(); Oi++)
        {
            const Mesh& M = Scene.Objects[Oi];
            if (DrawnWorld::Owner(M.Name) != Owner) continue;
            for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
            {
                const Surface& S = M.Surfaces[Si];
                if (IsDecalSurface(Scene, S) != Decal) continue;
                int Label = -1;
                size_t Base = 0;
                for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
                {
                    if (Drawn.Removed(Oi, Si, K / 3)) continue;
                    const Fate F = FateOf(S, K / 3);
                    if (F == Fate::None) Stats.TrianglesDegenerate++;
                    if (F != Fate::Patch) continue;
                    if (Label < 0)
                    {
                        // the surface's vertices join the soup once; the triangles name them
                        Label = Ids.emplace(S.Material, static_cast<int>(Names.size())).first->second;
                        if (static_cast<size_t>(Label) == Names.size()) Names.push_back(S.Material);
                        Base = Soup.Corners.size();
                        for (const Vertex& V : S.Vertices)
                        {
                            Soup.Positions.insert(Soup.Positions.end(), { V.Pos[0], V.Pos[1], V.Pos[2] });
                            RenderCorner C;
                            C.U = V.Uv[0];
                            C.V = V.Uv[1];
                            std::memcpy(C.Color, V.Color, 4);
                            Soup.Corners.push_back(C);
                        }
                    }
                    V3 P[3], Vn = { 0, 0, 0 };
                    bool Normals = true;   // the vertex normals count only when all three corners have one that was read
                    for (int J = 0; J < 3; J++)
                    {
                        const Vertex& V = S.Vertices[S.Indices[K + J]];
                        P[J] = { V.Pos[0], V.Pos[1], V.Pos[2] };
                        Normals = Normals && V.NormalRead;
                        for (int A = 0; A < 3; A++) Vn[A] += V.Normal[A];
                        Soup.Indices.push_back(static_cast<int32_t>(Base + S.Indices[K + J]));
                    }
                    // the front: the vertex normals' direction when the reader gave them, else the game's winding applied to the corners
                    // (WindingOf: -1 when cross(b - a, c - a) points into the front)
                    V3 Cr = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
                    const double CrLen = Length(Cr);
                    V3 Front = Mul(Cr, Winding / (CrLen > 0 ? CrLen : 1.0));
                    if (Normals && Length(Vn) > 0.5) Front = Mul(Vn, 1.0 / Length(Vn));
                    Soup.Label.push_back(Label);
                    Soup.Front.push_back(Front);
                    Soup.Normal.push_back(Normals ? Vn : V3{ 0, 0, 0 });
                }
            }
        }
        if (Soup.Indices.empty()) return;

        RenderMeshOptions Options;
        Options.Decal = Decal;
        Options.VertexOrderBorders = VertexOrderIsPatchOrder(Scene.Game);
        const auto Started = std::chrono::steady_clock::now();
        RenderMeshResult Meshes = BuildRenderMeshes(Soup, Names, Options);
        Stats.MeshMilliseconds += static_cast<size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Started).count());
        // triangles in = covered + removed with a proof, per owner and kind; a mismatch fails the export like a coverage failure
        if (Meshes.Count.TrianglesIn != Meshes.Count.Covered + Meshes.Count.CopiesRemoved + Meshes.Count.CollapsedByWeld)
        {
            Meshes.Count.AccountingFailed++;
            Meshes.CoverageLines.push_back(std::format("owner {} {}: {} triangles in are not {} covered + {} copies removed + {} collapsed by the weld\n", Owner, Decal ? "decals" : "surfaces", Meshes.Count.TrianglesIn,
                                                       Meshes.Count.Covered, Meshes.Count.CopiesRemoved, Meshes.Count.CollapsedByWeld));
        }
        std::vector<std::string> Written(Names.size());
        for (size_t L = 0; L < Names.size(); L++) Written[L] = MaterialName(Names[L], Gdt);
        for (const RenderMesh& R : Meshes.Meshes)
        {
            MeshTextOptions Text;
            Text.Prefix = "// brush " + std::to_string(Number++) + "\n {\n";
            Out += MeshText(R.Mesh, Written[static_cast<size_t>(R.Label)], R.ReplaceUv, Text, &Stats.MeshTextMaxAbsT) + " }\n";
        }
        Stats.Patches += Meshes.Count.Meshes;
        Stats.PatchesColoured += Meshes.Count.MeshesColoured;
        Stats.PatchesFacingNormal += Meshes.Count.FacingNormal;
        Stats.PatchesFacingAgainst += Meshes.Count.FacingAgainst;
        if (!Decal) (Meshes.Count.WeldTolerance > 0 ? Stats.OwnersWelded : Stats.OwnersNotSeparated)++;
        {
            const RenderMeshCounters& M = Meshes.Count;
            Stats.MeshLines.push_back(std::format("owner {} {}: {} triangles in, {} meshes ({} grids: {} fans, {} lone triangles, {} cells), covered {}, removed with proof {} copies + {} collapsed by the weld (kept as other data: {} by front, {} by texture coordinate, {} by colour), coverage MISSING {} LEFTOVER {}; "
                                                  "borders cut {} by texture, {} by colour and {} by facing; facing turned {}, kept {}, unknown {}, against the vertex normals {}, undecided {}; split {}; degenerate texture {}; triangles shifted by whole repeats {}, texture conflicts {}; "
                                                  "cells with both diagonals as input {} (not planar {}); most pairs sharing an edge in a material {}; weld tolerance {} ({}); weld {} vertices in {} clusters, largest span {}; {}; decal overlap {} partial, {} contained, {} pairs tested",
                                                  Owner, Decal ? "decals" : "surfaces", M.TrianglesIn, M.Meshes, M.Grids, M.Fans, M.LoneTriangles, M.Cells, M.Covered, M.CopiesRemoved, M.CollapsedByWeld, M.CopiesKeptFront, M.CopiesKeptUv, M.CopiesKeptColour, M.CoverageMissing,
                                                  M.CoverageLeftover, M.BordersCutTexture, M.BordersCutColour, M.BordersCutFacing, M.Flipped, M.FacingKept, M.FacingUnknown, M.FacingAgainst, M.FacingUndecided, M.GridsSplit, M.DegenerateUv,
                                                  M.UvTrianglesShifted, M.UvConflictCorners, M.CellsBothDiagonals, M.CellsBothDiagonalsNonPlanar, M.MaxCandidatePotential, M.WeldTolerance, M.WeldReason, M.WeldedVertices, M.WeldClusters, M.WeldSpan, M.NearestText, M.OverlapPartial, M.OverlapContained, M.OverlapTested));
        }
        for (const std::string& Block : Meshes.BothDiagonalLines)
            Stats.MeshLines.push_back(std::format("owner {} {} cells with both diagonals as input and not planar (Radiant's choice of diagonal for them is unknown):\n{}", Owner, Decal ? "decals" : "surfaces", Block));
        Stats.Mesh.Add(Meshes.Count);
#ifdef GW_MESH_DUMP   // Dev builds only
        // GH_MESHDUMP=<folder>: the soup of an owner whose coverage failed or that has a mesh facing against its normals, to reproduce it outside the export (a developer aid, nothing else reads it)
        if (!Meshes.CoverageLines.empty() || Meshes.Count.FacingAgainst > 0)
            if (std::string Dump = EnvText("GH_MESHDUMP"); !Dump.empty())
            {
                std::ofstream F(fs::path(Dump) / ("soup_" + std::to_string(Owner) + (Decal ? "_decal.bin" : ".bin")), std::ios::binary);
                const auto Put = [&](const auto& V) {
                    const uint64_t N = V.size();
                    F.write(reinterpret_cast<const char*>(&N), 8);
                    if (N) F.write(reinterpret_cast<const char*>(V.data()), static_cast<std::streamsize>(N * sizeof(V[0])));
                };
                Put(Soup.Positions), Put(Soup.Corners), Put(Soup.Indices), Put(Soup.Label), Put(Soup.Front), Put(Soup.Normal);
                uint64_t Names_ = Names.size();
                F.write(reinterpret_cast<const char*>(&Names_), 8);
                for (const std::string& N : Names)
                {
                    const uint64_t L = N.size();
                    F.write(reinterpret_cast<const char*>(&L), 8);
                    F.write(N.data(), static_cast<std::streamsize>(L));
                }
            }
#endif
        for (std::string& Lines : Meshes.CoverageLines) Stats.CoverageLines.push_back(std::move(Lines));
    }

    // The drawn triangles of Owner's meshes (DrawnWorld::Owner) no brush side draws, as meshes: one pass for the plain surfaces and one for the decals (Kind)
    void WritePatches(std::string& Out, const MapScene& Scene, const DrawnWorld& Drawn, uint32_t Owner, RadiantGdt* Gdt, RadiantStats& Stats,
                      size_t& Number, PatchKind Kind = PatchKind::All)
    {
        if (Kind != PatchKind::Decal) WritePatchPass(Out, Scene, Drawn, Owner, Gdt, Stats, Number, false);
        if (Kind != PatchKind::NotDecal) WritePatchPass(Out, Scene, Drawn, Owner, Gdt, Stats, Number, true);
    }

    std::string Quote(const std::string& Key, const std::string& Value)
    {
        std::string V = Value;
        for (auto& C : V)
            if (C == '"') C = '\'';
        return "\"" + Key + "\" \"" + V + "\"\n";
    }

    std::string Vec(const float V[3]) { return Num(V[0]) + " " + Num(V[1]) + " " + Num(V[2]); }

    // ---- the models it names, as XMODEL_EXPORT (game units)

    // One rigid model of Surfaces (each split below the per-surface vertex limit); with ForGdt, its
    // materials named by their GDT asset names and added to ForGdt->Materials
    bool WriteXmodel(const std::vector<const Surface*>& Surfaces, const MapScene& Scene, const std::string& Name,
                     const fs::path& Path, RadiantGdt* ForGdt, bool Bin = false)
    {
        WraithModel Model;
        Model.AssetName = Name;
        WraithBone& Bone = Model.AddBone();
        Bone.TagName = "tag_origin";
        std::map<std::string, int32_t> MaterialIndex;
        for (const Surface* S : Surfaces)
        {
            if (S->Indices.size() < 3) continue;
            auto [It, New] = MaterialIndex.emplace(S->Material, (int32_t)Model.Materials.size());
            if (New)
            {
                WraithMaterial M;
                M.MaterialName = S->Material.empty() ? "default" : ExportNaming::SafeFileName(S->Material);
                if (ForGdt && !S->Material.empty())
                {
                    M.MaterialName = MaterialGdt::AssetName(S->Material);
                    ForGdt->Materials.insert(S->Material);
                }
                if (const auto Mat = Scene.Materials.find(S->Material); Mat != Scene.Materials.end() && !Mat->second.ColorMap.empty())
                    M.DiffuseMapName = Mat->second.ColorMap;   // the image's name, as model exports give it
                Model.AddMaterial(M);
            }
            // pieces of at most Limit vertices, each vertex copied once per piece
            constexpr size_t Limit = 65000;
            WraithSubmesh* Mesh = nullptr;
            std::map<uint32_t, uint32_t> Remap;
            for (size_t K = 0; K + 2 < S->Indices.size(); K += 3)
            {
                if (!Mesh || Mesh->Verticies.size() + 3 > Limit)
                {
                    Mesh = &Model.AddSubmesh();
                    Mesh->AddMaterial(It->second);
                    Remap.clear();
                }
                uint32_t Tri[3];
                for (int J = 0; J < 3; J++)
                {
                    const uint32_t I = S->Indices[K + J];
                    if (I >= S->Vertices.size()) return false;
                    auto [R, Added] = Remap.emplace(I, (uint32_t)Mesh->Verticies.size());
                    if (Added)
                    {
                        const Vertex& V = S->Vertices[I];
                        WraithVertex& W = Mesh->AddVertex();
                        W.Position = Vector3(V.Pos[0], V.Pos[1], V.Pos[2]);
                        W.Normal = Vector3(V.Normal[0], V.Normal[1], V.Normal[2]);
                        W.AddUVLayer(V.Uv[0], V.Uv[1]);
                        W.AddVertexWeight(0, 1.0f);
                        for (int C = 0; C < 4; C++) W.Color[C] = V.Color[C];
                    }
                    Tri[J] = R->second;
                }
                Mesh->AddFace(Tri[0], Tri[1], Tri[2]);
            }
        }
        if (Model.Submeshes.empty()) return false;
        try
        {
            if (Bin) CodXMB::ExportXMB(Model, Path.string());
            else CodXME::ExportXME(Model, Path.string());
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    class RadiantSceneWriter final : public ISceneWriter
    {
    public:
        RadiantSceneWriter(bool Materials, std::vector<PrefabPart> Parts, fs::path FullModels)
            : WithMaterials(Materials), Prefabs(std::move(Parts)), FullModels(std::move(FullModels)) {}
        const char* Extension() const override { return ".map"; }
        bool Write(const MapScene& Scene, const std::string& Path, const Materials::IAdapter& Adapter, SceneWriteResult& Result,
                   std::string& Error) const override
        {
            RadiantStats Stats;
            RadiantGdt G;
            const RadiantFullMap Full{ FullModels };
            // --full-map: the models, their materials and images are the publish step's; the writer only names the materials
            // (the GDT of its own is not written)
            if (!WriteRadiant(Scene, Path, Stats, Error, WithMaterials || !FullModels.empty() ? &G : nullptr, Prefabs, FullModels.empty() ? nullptr : &Full))
            {
                Error = "radiant map: " + Error;
                return false;
            }
            if (WithMaterials && FullModels.empty()) Result.LaterCounters = MaterialsLater(Scene, Adapter, std::move(G));
            Result.Counters = { { "map_radiant_maps", 1 },
                                { "map_radiant_clip_brushes", Scene.Collision.ClipBrushes },
                                { "map_radiant_brushes", Stats.Brushes },
                                { "map_radiant_brushes_dropped", Stats.BrushesDropped },
                                { "map_radiant_brushes_unsound", Stats.BrushesUnsound },
                                { "map_radiant_trigger_brushes", Stats.TriggerBrushes },
                                { "map_radiant_triangles", Stats.Triangles },
                                { "map_radiant_triangles_dropped", Stats.TrianglesDropped },
                                { "map_radiant_triangles_duplicate", Stats.TrianglesDuplicate },
                                { "map_radiant_entities", Stats.Entities },
                                { "map_radiant_brush_entities", Stats.BrushEntities },
                                { "map_radiant_brush_entities_empty", Stats.BrushEntitiesEmpty },
                                { "map_radiant_placements", Stats.Placements },
                                { "map_radiant_world_models", Stats.WorldModels },
                                { "map_radiant_model_files", Stats.ModelFiles },
                                { "map_radiant_models_published", Stats.ModelsPublished },
                                { "map_radiant_models_elsewhere", Stats.ModelsElsewhere } };
            Result.Counters.insert(Result.Counters.end(), { { "map_radiant_brushes_hull", Stats.BrushesHull },
                                                            { "map_radiant_brush_pieces", Stats.BrushPieces },
                                                            { "map_radiant_brushes_hull_none", Stats.BrushesHullNone },
                                                            { "map_radiant_brushes_hull_unsound", Stats.BrushesHullUnsound },
                                                            { "map_radiant_brushes_split", Stats.BrushesSplit },
                                                            { "map_radiant_brushes_over_limit", Stats.BrushesOverLimit },
                                                            { "map_radiant_hull_faces_on_side", Stats.HullFacesMatched },
                                                            { "map_radiant_hull_faces_on_no_side", Stats.HullFacesUnmatched },
                                                            { "map_radiant_lights", Stats.Lights },
                                                            { "map_radiant_placements_polar", Stats.PlacementsPolar },
                                                            { "map_radiant_placements_polar_noise", Stats.PlacementsPolarNoise },
                                                            { "map_radiant_keys_unresolved", Stats.KeysUnresolved },
                                                            { "map_radiant_patches_facing_normal", Stats.PatchesFacingNormal },
                                                            { "map_radiant_patches_facing_against", Stats.PatchesFacingAgainst } });
            Result.Counters.insert(Result.Counters.end(), { { "map_radiant_patches", Stats.Patches },
                                                            { "map_radiant_patches_coloured", Stats.PatchesColoured },
                                                            { "map_radiant_triangles_degenerate", Stats.TrianglesDegenerate },
                                                            { "map_radiant_triangles_in_models", Stats.TrianglesInModels } });
            {
                const RenderMeshCounters& M = Stats.Mesh;
                Result.Counters.insert(Result.Counters.end(), { { "map_radiant_mesh_triangles_in", M.TrianglesIn },
                                                                { "map_radiant_mesh_copies_removed", M.CopiesRemoved },
                                                                { "map_radiant_mesh_copies_kept_front", M.CopiesKeptFront },
                                                                { "map_radiant_mesh_copies_kept_uv", M.CopiesKeptUv },
                                                                { "map_radiant_mesh_copies_kept_colour", M.CopiesKeptColour },
                                                                { "map_radiant_mesh_cells_both_diagonals", M.CellsBothDiagonals },
                                                                { "map_radiant_mesh_cells_both_diagonals_non_planar", M.CellsBothDiagonalsNonPlanar },
                                                                { "map_radiant_mesh_accounting_failed", M.AccountingFailed },
                                                                { "map_radiant_mesh_max_candidate_potential", static_cast<size_t>(M.MaxCandidatePotential) },
                                                                { "map_radiant_mesh_collapsed_by_weld", M.CollapsedByWeld },
                                                                { "map_radiant_mesh_triangles_covered", M.Covered },
                                                                { "map_radiant_mesh_grids", M.Grids },
                                                                { "map_radiant_mesh_fans", M.Fans },
                                                                { "map_radiant_mesh_lone_triangles", M.LoneTriangles },
                                                                { "map_radiant_mesh_cells", M.Cells },
                                                                { "map_radiant_mesh_borders_cut_texture", M.BordersCutTexture },
                                                                { "map_radiant_mesh_borders_cut_colour", M.BordersCutColour },
                                                                { "map_radiant_mesh_borders_cut_facing", M.BordersCutFacing },
                                                                { "map_radiant_mesh_facing_flipped", M.Flipped },
                                                                { "map_radiant_mesh_facing_kept", M.FacingKept },
                                                                { "map_radiant_mesh_facing_unknown", M.FacingUnknown },
                                                                { "map_radiant_mesh_facing_undecided", M.FacingUndecided },
                                                                { "map_radiant_mesh_grids_split", M.GridsSplit },
                                                                { "map_radiant_mesh_degenerate_uv", M.DegenerateUv },
                                                                { "map_radiant_mesh_coverage_materials", M.Labels },
                                                                { "map_radiant_mesh_coverage_missing", M.CoverageMissing },
                                                                { "map_radiant_mesh_coverage_leftover", M.CoverageLeftover },
                                                                { "map_radiant_mesh_coverage_failed_materials", M.LabelsFailed },
                                                                { "map_radiant_mesh_uv_triangles_shifted", M.UvTrianglesShifted },
                                                                { "map_radiant_mesh_uv_conflict_corners", M.UvConflictCorners },
                                                                { "map_radiant_mesh_candidates_capped", M.CandidatesCapped },
                                                                { "map_radiant_mesh_decal_overlap_partial", M.OverlapPartial },
                                                                { "map_radiant_mesh_decal_overlap_contained", M.OverlapContained },
                                                                { "map_radiant_mesh_ms", Stats.MeshMilliseconds },
                                                                { "map_radiant_mesh_owners_welded", Stats.OwnersWelded },
                                                                { "map_radiant_mesh_owners_not_separated", Stats.OwnersNotSeparated },
                                                                { "map_radiant_mesh_max_abs_t", ReportCounter(Stats.MeshTextMaxAbsT) } });
            }
            for (const auto& [Prefix, T] : { std::make_pair("map_radiant_texture_", &Stats.Textures), std::make_pair("map_radiant_prism_texture_", &Stats.PrismTextures),
                                             std::make_pair("map_radiant_entity_texture_", &Stats.EntityTextures) })
            for (const auto& [Key, N] : std::initializer_list<std::pair<const char*, size_t>>{
                     { "sides_solid", T->Sides }, { "sides_textured", T->SidesTextured }, { "sides_split", T->SidesSplit },
                     { "sides_no_fit", T->SidesNoFit }, { "sides_mixed_maps", T->SidesMixedMaps }, { "sides_uv_missed", T->SidesUvMissed }, { "sides_undrawn", T->SidesUndrawn }, { "brushes_textured", T->BrushesTextured },
                     { "uv_error_below_1e4", T->UvExact }, { "uv_error_below_1e3", T->UvBelow1e3 },
                     { "uv_error_below_1e2", T->UvBelow1e2 }, { "uv_error_below_1e1", T->UvBelow1e1 }, { "uv_error_above_1e1", T->UvAbove },
                     { "uv_error_mean_1e6", T->SidesTextured + T->SidesUvMissed ? ReportCounter(1e6 * T->UvErrorSum / (double)(T->SidesTextured + T->SidesUvMissed)) : 0 },
                     { "triangles_drawn", T->TrianglesDrawn }, { "triangles_removed", T->TrianglesRemoved },
                     { "triangles_coloured", T->TrianglesColoured },
                     { "plane_distance_0", T->Distance[0] }, { "plane_distance_below_1e3", T->Distance[1] },
                     { "plane_distance_below_1e2", T->Distance[2] }, { "plane_distance_below_tolerance", T->Distance[3] },
                     { "coverage_below_10", T->Coverage[0] }, { "coverage_below_50", T->Coverage[1] }, { "coverage_below_90", T->Coverage[2] },
                     { "coverage_90_up", T->Coverage[3] } })
                Result.Counters.emplace_back(std::string(Prefix) + Key, N);
            // the prefab files: counts by part (the file is <map>_prefab_<part>.map), and what the main map keeps
            for (const RadiantPrefabFile& F : Stats.PrefabFiles) Result.Files.push_back(F.File);
            if (!Prefabs.empty())
            {
                Result.Counters.emplace_back("map_radiant_prefab_files", Stats.PrefabFiles.size());
                Result.Counters.emplace_back("map_radiant_prefab_main_entities", Stats.MainEntities);
                Result.Counters.emplace_back("map_radiant_prefab_main_brushes", Stats.MainBrushes);
                Result.Counters.emplace_back("map_radiant_prefab_main_meshes", Stats.MainMeshes);
                for (const RadiantPrefabFile& F : Stats.PrefabFiles)
                {
                    const std::string Key = "map_radiant_prefab_" + PrefabPartFileName(F.Part) + "_";
                    Result.Counters.emplace_back(Key + "entities", F.Entities);
                    Result.Counters.emplace_back(Key + "brushes", F.Brushes);
                    Result.Counters.emplace_back(Key + "meshes", F.Meshes);
                }
            }
            return true;
        }

    private:
        bool WithMaterials;
        std::vector<PrefabPart> Prefabs;
        fs::path FullModels;   // --full-map: the run's models folder (the publish step's input); empty: the private <map>_radiant

        // The GDT with the material and image entries of the materials its model files name, once
        // the run has written the images (some come from other zones): each image hard-linked (else
        // copied) to <map>_radiant/model_export/<map>/images/, which the image entries name.
        static std::function<void(std::map<std::string, size_t>&)> MaterialsLater(const MapScene& Scene, const Materials::IAdapter& Adapter,
                                                                                 RadiantGdt G)
        {
            std::vector<Materials::Description> Described;
            std::vector<Materials::Source> Sources;     // parallel: the writer's surface evidence is the source's flag word
            for (const std::string& Name : G.Materials)
            {
                auto [Src, Desc] = DescribeMaterial(Scene, Name, Adapter, G.Root);
                Sources.push_back(std::move(Src));
                Described.push_back(std::move(Desc));
            }
            // the materials the map's own render surfaces use are listed in Radiant under a usage by what
            // they draw (decal, window, floor, ceiling, interior wall); the others stay <not in editor>
            std::map<std::string, Bo3Target::Techsets::MapUse> Usage = GeometryUse(Scene, &G.Materials);
            return [G = std::move(G), Described = std::move(Described), Sources = std::move(Sources), GameId = Scene.Game, Usage = std::move(Usage)](std::map<std::string, size_t>& Counters) {
                const fs::path Images = G.Root / "model_export" / G.Map / "images";
                // an image's file name there, the first source to take a name keeping it
                std::map<fs::path, std::string> Named;
                std::set<std::string> Taken;
                MaterialGdt::Builder B([&](const fs::path& File) {
                    auto [It, New] = Named.emplace(File, std::string());
                    if (New)
                    {
                        // a plain file name ("$specular.png" as "specular.png")
                        std::string Stem = MaterialGdt::AssetName(File.stem().string());
                        if (Stem.empty()) Stem = "image";
                        const std::string Ext = ExportNaming::Lower(File.extension().string());
                        std::string Name = Stem + Ext;
                        for (int N = 2; !Taken.insert(Name).second; N++) Name = Stem + "_" + std::to_string(N) + Ext;
                        It->second = Name;
                    }
                    return "model_export/" + G.Map + "/images/" + It->second;
                }, "material_gdt_");
                B.SetMapUse(Usage);
                for (size_t K = 0; K < Described.size(); K++) B.Add(Described[K], G.Root, &Sources[K], GameId);
                std::vector<std::string> Entries = G.ModelEntries;
                const std::vector<std::string> Mine = B.Entries("\n");
                // every usage fallback leaves a row, as the publish step's approximations file does: <map>_radiant/graywolf_approximations.tsv
                {
                    std::string Rows = "code\tasset\tdetail\tmessage\n";
                    size_t Fallbacks = 0;
                    for (const auto& M : B.Resolve().Materials)
                        for (const auto& N : M.Notes)
                            if (N.Code == "material.usage_fallback")
                            {
                                Rows += N.Code + "\t" + M.Name + "\t" + N.Detail + "\t" + N.Message + "\n";
                                Fallbacks++;
                            }
                    Counters["material_usage_fallback"] += Fallbacks;
                    std::error_code RowEc;
                    fs::create_directories(G.Root, RowEc);
                    if (Fallbacks != 0) std::ofstream(G.Root / "graywolf_approximations.tsv", std::ios::binary | std::ios::trunc) << Rows;
                    else fs::remove(G.Root / "graywolf_approximations.tsv", RowEc);
                }
                Entries.insert(Entries.end(), Mine.begin(), Mine.end());
                std::error_code Ec;
                fs::create_directories(Images, Ec);
                for (const auto& [File, Name] : Named)
                {
                    const fs::path Target = Images / Name;
                    fs::remove(Target, Ec);
                    fs::create_hard_link(File, Target, Ec);
                    if (Ec) fs::copy_file(File, Target, fs::copy_options::overwrite_existing, Ec);
                    Counters[Ec ? "material_gdt_images_not_copied" : "material_gdt_images_copied"]++;
                }
                for (const auto& [K, N] : B.Counters()) Counters[K] += N;
                fs::create_directories(G.Root / "source_data", Ec);
                const fs::path GdtFile = G.Root / "source_data" / (G.Map + ".gdt");
                std::ofstream(GdtFile, std::ios::binary) << Gdt::Document(Entries, "\n");
                std::ofstream(fs::path(GdtFile).replace_extension(".names.json"), std::ios::binary) << B.NamesJson();
            };
        }
    };
}

namespace
{
    // The faces of the sides Used, each oriented counter-clockwise seen from outside; Bounded false
    // when a face reaches the starting polygon's edge (an open brush)
    std::vector<BrushFace> Faces(const std::vector<Plane>& Planes, const std::vector<bool>& Used, bool& Bounded)
    {
        std::vector<BrushFace> Out;
        Bounded = true;
        for (size_t I = 0; I < Planes.size(); I++)
        {
            if (!Used[I]) continue;
            std::vector<V3> W = BaseWinding(Planes[I]);
            for (size_t J = 0; J < Planes.size() && W.size() >= 3; J++)
                if (J != I && Used[J]) W = Clip(W, Planes[J].N, Planes[J].D, OnPlane);
            if (W.size() < 3 || Area(W) < 1e-6) continue;
            V3 A = { 0, 0, 0 };
            for (size_t K = 1; K + 1 < W.size(); K++) A = Add(A, Cross(Sub(W[K], W[0]), Sub(W[K + 1], W[0])));
            if (Dot(A, Planes[I].N) < 0) std::reverse(W.begin(), W.end());
            for (const V3& V : W)
                if (std::fabs(V[0]) > Huge / 2 || std::fabs(V[1]) > Huge / 2 || std::fabs(V[2]) > Huge / 2) Bounded = false;
            BrushFace Face;
            Face.Side = I;
            Face.Points = std::move(W);
            Face.Normal = Planes[I].N;
            Face.Dist = Planes[I].D;
            Out.push_back(std::move(Face));
        }
        return Out;
    }

    // how thin a face is: its area over its longest edge
    double Thickness(const std::vector<V3>& W)
    {
        double Longest = 0;
        for (size_t K = 0; K < W.size(); K++) Longest = (std::max)(Longest, Length(Sub(W[(K + 1) % W.size()], W[K])));
        return Longest > 0 ? Area(W) / Longest : 0;
    }
}

std::vector<BrushFace> BrushFaces(const Brush& B, double Within)
{
    std::vector<Plane> Planes;
    std::vector<bool> Used;
    for (const BrushSide& S : B.Sides)
    {
        const V3 N = { S.Normal[0], S.Normal[1], S.Normal[2] };
        const double L = Length(N);
        Planes.push_back(L > 0.5 ? Plane{ Mul(N, 1.0 / L), S.Dist / L } : Plane{ { 0, 0, 0 }, 0 });
        // a plane an earlier side already has bounds nothing more
        bool Duplicate = L <= 0.5;
        for (size_t J = 0; J + 1 < Planes.size() && !Duplicate; J++)
            Duplicate = Used[J] && Dot(Planes[J].N, Planes.back().N) > 0.999999 && std::fabs(Planes[J].D - Planes.back().D) < OnPlane;
        Used.push_back(!Duplicate);
    }
    bool Bounded = true;
    std::vector<BrushFace> Out = Faces(Planes, Used, Bounded);
    // a side that bounds nothing is left out; so is one whose face is a sliver or lies on another
    // side's plane (a bevel, a plane nearly another's: three points give its plane badly, Radiant
    // would drop it) when the brush stays closed and within Within of its plane without it
    std::vector<bool> Bounds(Planes.size(), false);
    for (const BrushFace& F : Out) Bounds[F.Side] = true;
    for (size_t I = 0; I < Planes.size(); I++) Used[I] = Used[I] && Bounds[I];
    std::vector<std::pair<double, size_t>> Thin;
    for (const BrushFace& F : Out)
    {
        // thin, or lying on another side's plane (two planes nearly one, crossing on the face)
        bool OnAnother = false;
        for (size_t J = 0; J < Planes.size() && !OnAnother; J++)
        {
            if (J == F.Side || !Used[J] || Dot(Planes[J].N, Planes[F.Side].N) < 0.99) continue;
            OnAnother = true;
            for (const V3& V : F.Points) OnAnother = OnAnother && std::fabs(Dot(Planes[J].N, V) - Planes[J].D) < Within;
        }
        if (const double T = Thickness(F.Points); T < (std::max)(0.05, Within) || OnAnother) Thin.emplace_back(OnAnother ? 0.0 : T, F.Side);
    }
    std::sort(Thin.begin(), Thin.end());
    for (const auto& [T, Side] : Thin)
    {
        Used[Side] = false;
        bool Bounded2 = true;
        std::vector<BrushFace> Without = Faces(Planes, Used, Bounded2);
        // left out when the brush without it stays closed and within Within of its plane
        double Out1 = 0;
        for (const BrushFace& F : Without)
            for (const V3& V : F.Points) Out1 = (std::max)(Out1, Dot(Planes[Side].N, V) - Planes[Side].D);
        if (Bounded2 && Without.size() >= 4 && Out1 <= Within)
            Out = std::move(Without);
        else
            Used[Side] = true;
    }
    return Out;
}

namespace
{
    // A collision triangle as a brush: its face, the face Thick units behind, a side per edge;
    // false for one thinner than 1/20 unit (no face to stand on, its planes would not survive
    // three points)
    bool Prism(const V3 P[3], double Thick, Brush& B)
    {
        V3 N = Cross(Sub(P[1], P[0]), Sub(P[2], P[0]));
        const double Twice = Length(N);
        double Longest = 0;
        for (int J = 0; J < 3; J++) Longest = (std::max)(Longest, Length(Sub(P[(J + 1) % 3], P[J])));
        if (Twice < 0.01 || Twice / Longest < 0.05) return false;
        N = Mul(N, 1.0 / Twice);
        const auto Side = [&B](const V3& Normal, double Dist) {
            BrushSide S;
            for (int J = 0; J < 3; J++) S.Normal[J] = (float)Normal[J];
            S.Dist = (float)Dist;
            B.Sides.push_back(S);
        };
        Side(N, Dot(N, P[0]));
        Side(Mul(N, -1), -(Dot(N, P[0]) - Thick));
        for (int J = 0; J < 3; J++)
        {
            V3 E = Cross(Sub(P[(J + 1) % 3], P[J]), N);
            E = Mul(E, 1.0 / Length(E));
            if (Dot(E, Sub(P[(J + 2) % 3], P[J])) > 0) E = Mul(E, -1);
            Side(E, Dot(E, P[J]));
        }
        return true;
    }

    // The entity string as Radiant reads it: a key numbered 0 is no script string, its name
    // follows quoted (MWR: 0 "materiallayering" "1")
    std::string EntityText(const std::string& Ents)
    {
        std::string Text;
        for (size_t I = 0; I < Ents.size();)
        {
            const size_t End = (std::min)(Ents.find('\n', I), Ents.size());
            const size_t From = Ents.compare(I, 3, "0 \"") == 0 ? I + 2 : I;
            Text.append(Ents, From, End - From);
            Text += '\n';
            I = End + 1;
        }
        return Text;
    }

    bool Numbered(const std::string& Key) { return !Key.empty() && std::all_of(Key.begin(), Key.end(), [](char C) { return C >= '0' && C <= '9'; }); }
    // three numbers, as an origin or angles are
    bool Vector3(const std::string& V)
    {
        const char* P = V.c_str();
        for (int K = 0; K < 3; K++)
        {
            char* End = nullptr;
            std::strtof(P, &End);
            if (End == P) return false;
            P = End;
        }
        while (*P == ' ') P++;
        return *P == 0;
    }

    // The keys an entity string names by number (a script string the title keeps in its exe:
    // Ghosts on) that its values tell apart, added to Names when it has none yet: classname (the
    // key worldspawn is the value of), model (values "*N" or "?N"), origin (the vector a brush
    // entity has), angles (the other vector a spawn point has)
    void InferKeys(const std::vector<Entity>& Ents, std::map<std::string, std::string>& Names)
    {
        std::map<std::string, std::string> Found;
        for (const Entity& E : Ents)
            for (const auto& [K, V] : E)
                if (Numbered(K) && V == "worldspawn") Found.emplace(K, "classname");
        if (Found.empty()) return;
        const std::string Class = Found.begin()->first;
        std::map<std::string, size_t> Models, Origins, Angles;
        for (const Entity& E : Ents)
            for (const auto& [K, V] : E)
                if (Numbered(K) && V.size() > 1 && (V[0] == '*' || V[0] == '?') && std::isdigit((unsigned char)V[1])) Models[K]++;
        const std::string Model = Models.empty() ? std::string() : std::max_element(Models.begin(), Models.end(),
                                      [](const auto& A, const auto& B) { return A.second < B.second; })->first;
        for (const Entity& E : Ents)
        {
            const bool Brush = !Model.empty() && E.count(Model);
            const auto C = E.find(Class);
            const bool Spawn = C != E.end() && C->second.find("spawn") != std::string::npos;
            for (const auto& [K, V] : E)
            {
                if (!Numbered(K) || !Vector3(V)) continue;
                if (Brush) Origins[K]++;
                if (Spawn) Angles[K]++;
            }
        }
        const auto Most = [](const std::map<std::string, size_t>& M, const std::string& Not) {
            std::string Best;
            size_t N = 0;
            for (const auto& [K, C] : M)
                if (K != Not && C > N) Best = K, N = C;
            return Best;
        };
        const std::string Origin = Most(Origins, "");
        const std::string Angle = Most(Angles, Origin);
        if (!Model.empty()) Found.emplace(Model, "model");
        if (!Origin.empty()) Found.emplace(Origin, "origin");
        if (!Angle.empty()) Found.emplace(Angle, "angles");
        for (const auto& [K, V] : Found) Names.emplace(K, V);
    }
}

bool TrianglePrism(const std::array<double, 3> P[3], double Thick, Brush& B) { return Prism(P, Thick, B); }

std::vector<Entity> SceneEntities(const MapScene& Scene)
{
    std::vector<Entity> Ents;
    const std::string Text = EntityText(Scene.MapEnts);
    if (!Text.empty() && !ParseEntities(Text, Ents)) Ents.clear();
    std::map<std::string, std::string> Names = Scene.EntityKeys;
    InferKeys(Ents, Names);
    for (Entity& E : Ents)
        for (const auto& [Number, Key] : Names)
            if (const auto It = E.find(Number); It != E.end())
            {
                E[Key] = It->second;
                E.erase(It);
            }
    return Ents;
}

namespace
{
    // An entity of the map text as written: where it starts in it and the part it goes to; the worldspawn also
    // where its brushes start and end
    struct Block
    {
        size_t Begin = 0;
        PrefabPart Part = PrefabPart::Entities;
        bool World = false;
        size_t BodyBegin = 0, BodyEnd = 0;
    };

    size_t CountOf(std::string_view Text, std::string_view What)
    {
        size_t N = 0;
        for (size_t At = Text.find(What); At != std::string_view::npos; At = Text.find(What, At + What.size())) N++;
        return N;
    }
    // The brushes and meshes of a piece of map text: every "// brush" line is one, a mesh among them has "  mesh"
    void CountBrushes(std::string_view Text, size_t& Brushes, size_t& Meshes)
    {
        const size_t All = CountOf(Text, "// brush "), Mesh = CountOf(Text, "\n  mesh\n");
        Meshes += Mesh;
        Brushes += All - Mesh;
    }
    // An entity's text with the number of its "// entity N" line replaced
    void WriteEntity(std::ostream& To, std::string_view Text, size_t Number)
    {
        const size_t Eol = Text.find('\n');
        To << "// entity " << Number << "\n";
        if (Eol != std::string_view::npos) To.write(Text.data() + Eol + 1, (std::streamsize)(Text.size() - Eol - 1));
    }

    // The map as Main and one standalone map per selected part with something in it (HydraX's MapPrefabSplitter):
    // the file is a worldspawn and the part; the main map keeps the rest, then a misc_prefab per file, its "model" the
    // file relative to map_source (PrefabFolder), origin 0 0 0. False when a file cannot be written.
    bool WritePrefabs(const std::string& Out, const std::vector<Block>& Blocks, const std::vector<PrefabPart>& Selected, const fs::path& MainPath,
                      std::ostream& Main, RadiantStats& Stats, std::string& Error)
    {
        const std::string_view All(Out);
        const std::string_view Header = All.substr(0, Blocks.front().Begin);
        const auto Has = [&](PrefabPart P) { return std::find(Selected.begin(), Selected.end(), P) != Selected.end(); };
        const auto TextOf = [&](size_t K) {
            const size_t End = K + 1 < Blocks.size() ? Blocks[K + 1].Begin : All.size();
            return All.substr(Blocks[K].Begin, End - Blocks[K].Begin);
        };
        // what the world's brushes are worth: nothing when none were written
        const Block& World = Blocks.front();
        const std::string_view WorldBrushes = All.substr(World.BodyBegin, World.BodyEnd - World.BodyBegin);
        const bool BrushesOut = Has(PrefabPart::Brushes) && !WorldBrushes.empty();

        // the main map: the world (with its brushes unless they went out), the entities that stay
        Main << Header << All.substr(World.Begin, World.BodyBegin - World.Begin);
        if (!BrushesOut) Main << WorldBrushes;
        Main << "}\n";
        CountBrushes(BrushesOut ? std::string_view() : WorldBrushes, Stats.MainBrushes, Stats.MainMeshes);
        size_t Number = 1;
        std::vector<size_t> Moved[PrefabPartCount];
        for (size_t K = 1; K < Blocks.size(); K++)
        {
            if (Has(Blocks[K].Part))
            {
                Moved[(int)Blocks[K].Part].push_back(K);
                continue;
            }
            WriteEntity(Main, TextOf(K), Number++);
            Stats.MainEntities++;
            CountBrushes(TextOf(K), Stats.MainBrushes, Stats.MainMeshes);
        }

        // the files, in part order
        const std::string Stem = MainPath.stem().string();
        for (const PrefabPart Part : Selected)
        {
            const bool Brushes = Part == PrefabPart::Brushes;
            if (Brushes ? !BrushesOut : Moved[(int)Part].empty()) continue;
            RadiantPrefabFile Info;
            Info.Part = Part;
            Info.File = Stem + "_prefab_" + PrefabPartFileName(Part) + ".map";
            std::ofstream File(MainPath.parent_path() / Info.File, std::ios::binary);
            if (!File)
            {
                Error = "cannot create " + (MainPath.parent_path() / Info.File).string();
                return false;
            }
            File << Header << "// entity 0\n{\n\"classname\" \"worldspawn\"\n";
            if (Brushes)
            {
                File << WorldBrushes;
                CountBrushes(WorldBrushes, Info.Brushes, Info.Meshes);
            }
            File << "}\n";
            size_t N = 1;
            for (const size_t K : Moved[(int)Part])
            {
                WriteEntity(File, TextOf(K), N++);
                Info.Entities++;
                CountBrushes(TextOf(K), Info.Brushes, Info.Meshes);
            }
            if (!File.good())
            {
                Error = "cannot write " + Info.File;
                return false;
            }
            Stats.PrefabFiles.push_back(Info);
            Main << "// entity " << Number++ << "\n{\n\"classname\" \"misc_prefab\"\n\"model\" \"" << PrefabFolder << Info.File << "\"\n\"origin\" \"0 0 0\"\n}\n";
        }
        return Main.good();
    }
}

bool WriteRadiant(const MapScene& Scene, const std::string& MapPath, RadiantStats& Stats, std::string& Error, RadiantGdt* ForGdt,
                  const std::vector<PrefabPart>& Prefabs, const RadiantFullMap* Full)
{
    const fs::path Path(MapPath);
    // A report from an earlier run must not outlive this one: a run that passes leaves neither file.
    {
        std::error_code Gone;
        fs::remove(fs::path(Path).replace_extension(".coverage.txt"), Gone);
        fs::remove(fs::path(Path).replace_extension(".meshes.txt"), Gone);
    }
    const std::string Map = ExportNaming::SafeFileName(Path.stem().string());
    const fs::path Root = Path.parent_path() / (Map + "_radiant");
    const fs::path ModelDir = Root / "model_export" / Map;
    std::error_code Ec;
    if (Full == nullptr)
    {
        fs::create_directories(ModelDir, Ec);
        if (Ec)
        {
            Error = "cannot create " + ModelDir.string();
            return false;
        }
    }
    // a model's name in the map: as the publish step names it with --full-map
    const auto ModelNamed = [Full](const std::string& Name) { return Full != nullptr ? FullMapAssetName(Name) : Name; };

    std::vector<Entity> Ents = SceneEntities(Scene);
    // with --full-map the models are published by the publish step, under the names it gives them
    if (Full != nullptr)
        for (Entity& E : Ents)
            for (auto& [K, V] : E)
                if (V[0] != '#' && EntityAssetType(K, V) == "xmodel") V = FullMapAssetName(V);
    // a key naming an effect or an asset the Mod Tools lack stays under graywolf_unresolved_<key>: Radiant ignores it
    for (Entity& E : Ents)
    {
        std::vector<std::pair<std::string, std::string>> Moved;
        for (const auto& [K, V] : E)
            if (IsUnresolvedReference(Scene.Game, K, V)) Moved.emplace_back(K, V);
        for (const auto& [K, V] : Moved)
        {
            E.erase(K);
            E[UnresolvedKey(K)] = V;
            Stats.KeysUnresolved++;
        }
    }

    std::string Out = "iwmap 4\n\"000_Global\" flags  active\n\"The Map\" flags \n";
    size_t EntityNumber = 0;
    std::vector<Block> Blocks;
    const auto Begin = [&](const Entity* Keys, const char* Class) {
        Blocks.push_back({ Out.size() });
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
        Out += Quote("classname", Keys && Keys->count("classname") ? Keys->at("classname") : Class);
        if (Keys)
            for (const auto& [K, V] : *Keys)
                if (K != "classname") Out += Quote(K, V);
    };

    // worldspawn: its keys, the world's brushes
    const ToolNames Tools(Scene.Collision);
    DrawnWorld Drawn(Scene, Stats.Textures);
    Stats.Textures.TrianglesDrawn = Drawn.Count();
    const Solids SolidBrushes(Scene.Collision);
    const DrawnWorld::HiddenTest Hidden = [&SolidBrushes](const V3& P) { return SolidBrushes.Inside(P); };
    const Texturing Tex{ &Drawn, ForGdt, 0, &Stats.Textures, &Hidden };
    // the brush models a mesh draws
    std::set<uint32_t> Owners;
    for (const Mesh& M : Scene.Objects)
        if (const uint32_t O = DrawnWorld::Owner(M.Name)) Owners.insert(O);
    std::set<uint32_t> Patched;   // the brush models whose triangles their entity holds as patches
    const Entity* World = nullptr;
    for (const Entity& E : Ents)
        if (E.count("classname") && E.at("classname") == "worldspawn")
        {
            World = &E;
            break;
        }
    Begin(World, "worldspawn");
    Blocks.back().World = true;
    Blocks.back().BodyBegin = Out.size();
    size_t BrushNumber = 0;
    for (const Brush& B : Scene.Collision.Brushes)
        if (WriteBrush(Out, B, BrushNumber, nullptr, nullptr, Tools, "", Stats, Tex)) BrushNumber++;
    Blocks.back().BodyEnd = Out.size();
    Out += "}\n";

    // the collision triangles (patches, terrain): each a prism Thick units deep behind its face
    // (they face out), in a func_group (the world's at compile time). A triangle given twice (the
    // same corners) is written once: facing the same way, a copy; facing the other way (a sheet
    // colliding on both sides), the one facing up, else the first.
    constexpr double Thick = 2.0;   // how deep a prism is
    const auto& Tv = Scene.Collision.Vertices;
    const auto Prisms = [&](const std::vector<uint32_t>& Tris, size_t& Number) {
        std::map<std::array<float, 9>, size_t> Seen;   // corners, sorted -> the triangle written
        std::vector<char> Kept(Tris.size() / 3, 1);
        const auto Corner = [&](size_t T, int J) {
            const size_t I = (size_t)Tris[T * 3 + J] * 3;
            return V3{ Tv[I], Tv[I + 1], Tv[I + 2] };
        };
        const auto Up = [&](size_t T) {
            return Cross(Sub(Corner(T, 1), Corner(T, 0)), Sub(Corner(T, 2), Corner(T, 0)))[2] > 0;
        };
        for (size_t T = 0; T < Kept.size(); T++)
        {
            std::array<std::array<float, 3>, 3> C;
            for (int J = 0; J < 3; J++)
                for (int A = 0; A < 3; A++) C[J][A] = Tv[(size_t)Tris[T * 3 + J] * 3 + A];
            std::sort(C.begin(), C.end());
            std::array<float, 9> Key;
            for (int J = 0; J < 9; J++) Key[J] = C[J / 3][J % 3];
            const auto [It, New] = Seen.emplace(Key, T);
            if (New) continue;
            Stats.TrianglesDuplicate++;
            // the other way round and facing up where the first does not: this one instead
            const size_t First = It->second;
            const V3 A = Cross(Sub(Corner(First, 1), Corner(First, 0)), Sub(Corner(First, 2), Corner(First, 0)));
            const V3 B = Cross(Sub(Corner(T, 1), Corner(T, 0)), Sub(Corner(T, 2), Corner(T, 0)));
            if (Dot(A, B) < 0 && Up(T) && !Up(First))
            {
                Kept[First] = 0;
                It->second = T;
            }
            else
                Kept[T] = 0;
        }
        for (size_t T = 0; T < Kept.size(); T++)
        {
            if (!Kept[T]) continue;
            V3 P[3];
            for (int J = 0; J < 3; J++) P[J] = Corner(T, J);
            Brush B;
            if (!Prism(P, Thick, B))
            {
                Stats.TrianglesDropped++;
                continue;
            }
            // counted as triangles, not as the clip map's brushes
            const size_t Dropped = Stats.BrushesDropped, Unsound = Stats.BrushesUnsound;
            if (WriteBrush(Out, B, Number, nullptr, nullptr, Tools, "clip_full", Stats, { Tex.Drawn, ForGdt, 0, &Stats.PrismTextures, &Hidden }))
            {
                Number++;
                Stats.Brushes--, Stats.Triangles++;
            }
            else
            {
                Stats.BrushesDropped = Dropped, Stats.BrushesUnsound = Unsound;
                Stats.TrianglesDropped++;
            }
        }
    };
    if (!Scene.Collision.Triangles.empty())
    {
        Begin(nullptr, "func_group");
        Blocks.back().Part = PrefabPart::Patches;
        BrushNumber = 0;
        Prisms(Scene.Collision.Triangles, BrushNumber);
        Out += "}\n";
    }

    // the other entities; a brush entity's brushes moved to where it stands
    for (const Entity& E : Ents)
    {
        if (&E == World) continue;
        // a brush model ("*N") or a trigger volume ("?N"), each in its own space: its brushes moved
        // to where the entity stands
        const auto Model = E.find("model");
        const char Kind = Model != E.end() && !Model->second.empty() ? Model->second[0] : 0;
        const bool BrushEntity = Kind == '*' || Kind == '?';
        const std::vector<std::vector<Brush>>& From = Kind == '?' ? Scene.Collision.Triggers : Scene.Collision.Models;
        size_t N = 0;
        if (BrushEntity)
        {
            N = (size_t)std::strtoul(Model->second.c_str() + 1, nullptr, 10);
            // a brush model without brushes still holds its drawn triangles' patches
            const bool HasDrawn = Kind == '*' && N > 0 && Owners.count((uint32_t)N);
            if ((N >= From.size() || From[N].empty()) && !HasDrawn)
            {
                // no brush to give it: kept as the game has it
                Stats.BrushEntitiesEmpty++;
                Begin(&E, "info_null");
                Blocks.back().Part = ClassifyEntity(E, false);
                Out += "}\n";
                Stats.Entities++;
                continue;
            }
        }
        Entity Keys = E;
        if (BrushEntity) Keys.erase("model");
        Begin(&Keys, "info_null");
        const size_t BodyStart = Out.size();
        if (BrushEntity)
        {
            float Origin[3] = { 0, 0, 0 }, Ang[3] = { 0, 0, 0 }, Axis[9];
            EntityFloats(E, "origin", Origin, 3);
            EntityFloats(E, "angles", Ang, 3);
            AnglesToAxis(Ang, Axis);
            // a trigger's brushes are its volume, whatever the clip map gives them
            const auto Class = E.find("classname");
            const bool Trigger = Kind == '?' || (Class != E.end() && Class->second.rfind("trigger", 0) == 0);
            size_t Number = 0;
            // a brush model's brushes take the materials its drawn triangles give them, the others patches
            const Texturing Own{ Kind == '*' ? Tex.Drawn : nullptr, ForGdt, (uint32_t)N, &Stats.EntityTextures, &Hidden };
            if (N < From.size())
                for (const Brush& B : From[N])
                    if (WriteBrush(Out, B, Number, Origin, Axis, Tools, Trigger ? "trigger" : "", Stats, Own))
                    {
                        Number++;
                        if (Kind == '?') Stats.Brushes--, Stats.TriggerBrushes++;   // not the clip map's
                    }
            if (Kind == '*' && N > 0 && Patched.insert((uint32_t)N).second) WritePatches(Out, Scene, Drawn, (uint32_t)N, ForGdt, Stats, Number);
            Stats.BrushEntities++;
        }
        {
            Entity ForClass = Keys;
            if (!ForClass.count("classname")) ForClass["classname"] = "info_null";
            Blocks.back().Part = ClassifyEntity(ForClass, Out.size() > BodyStart);
        }
        Out += "}\n";
        Stats.Entities++;
    }

    // the world's drawn triangles no brush side draws, and those of brush models no entity holds,
    // as patches
    {
        std::string Group, DecalGroup;
        size_t Number = 0, DecalNumber = 0;
        // the decals are a group of their own, so the decals file (--map-prefabs) takes them whole and the
        // entities of a split map add up to the unsplit one's
        WritePatches(Group, Scene, Drawn, 0, ForGdt, Stats, Number, PatchKind::NotDecal);
        WritePatches(DecalGroup, Scene, Drawn, 0, ForGdt, Stats, DecalNumber, PatchKind::Decal);
        for (const uint32_t O : Owners)
            if (!Patched.count(O))
            {
                WritePatches(Group, Scene, Drawn, O, ForGdt, Stats, Number, PatchKind::NotDecal);
                WritePatches(DecalGroup, Scene, Drawn, O, ForGdt, Stats, DecalNumber, PatchKind::Decal);
            }
        if (!Group.empty())
        {
            Begin(nullptr, "func_group");
            Blocks.back().Part = PrefabPart::Patches;
            Out += Group + "}\n";
        }
        if (!DecalGroup.empty())
        {
            Begin(nullptr, "func_group");
            Blocks.back().Part = PrefabPart::Decals;
            Out += DecalGroup + "}\n";
        }
    }

    // the compiled primary lights (Black Ops 4): a light entity each, colour and intensity under unresolved keys
    for (const Light& L : Scene.Lights)
    {
        const std::string Keys = LightEntityKeys(L, "graywolf_");
        if (Keys.empty()) continue;
        Blocks.push_back({ Out.size(), PrefabPart::Lights });
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n" + Keys + "}\n";
        Stats.Entities++;
        Stats.Lights++;
    }

    // the placements, then the world geometry left, as misc_models; the models they name as files
    std::set<std::string> Named;
    const auto Place = [&](const Instance& I) {
        if (const double Horizontal = std::hypot((double)I.Axis[0], (double)I.Axis[1]); Horizontal < 1e-4)
        {
            Stats.PlacementsPolar++;
            if (Horizontal >= 1e-6) Stats.PlacementsPolarNoise++;
        }
        Blocks.push_back({ Out.size(), PrefabPart::Models });
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
        Out += Quote("classname", "misc_model") + Quote("model", ModelNamed(I.Model)) + Quote("origin", Vec(I.Origin)) +
               Quote("angles", StaticModelAngles(I.Axis));
        if (std::fabs(I.Scale - 1.0f) > 1e-4f) Out += Quote("modelscale", Num(I.Scale));
        Out += "}\n";
        Stats.Placements++;
        Named.insert(I.Model);
    };
    for (const auto* List : { &Scene.StaticInstances, &Scene.DynamicInstances, &Scene.SplinedInstances })
        for (const Instance& I : *List)
            if (!I.Model.empty()) Place(I);

    // the GDT listing them, LF as it has always been written
    std::vector<std::string> Entries;
    const auto Listed = [&](const std::string& Name, const std::string& File) {
        Entries.push_back(Gdt::Entry(Name, "xmodel.gdf", { { "filename", Gdt::Path(Map + "/" + File) }, { "type", "rigid" } }, "\n"));
    };
    for (const std::string& Name : Named)
    {
        if (Full != nullptr)
        {
            Stats.ModelsPublished++;   // the model export wrote it as XMODEL_BIN, the publish step places it
            continue;
        }
        const auto It = Scene.Models.find(Name);
        if (It == Scene.Models.end())
        {
            Stats.ModelsElsewhere++;
            continue;
        }
        std::vector<const Surface*> Surfaces;
        for (const Surface& S : It->second.Surfaces) Surfaces.push_back(&S);
        const std::string File = ExportNaming::SafeFileName(Name) + ".XMODEL_EXPORT";
        if (!WriteXmodel(Surfaces, Scene, Name, ModelDir / File, ForGdt)) continue;
        Listed(Name, File);
        Stats.ModelFiles++;
    }

    // the world's triangles neither a brush side nor a patch draws in tiles of Tile units (by
    // triangle centre), each its own model; a brush model's one model each
    constexpr double Tile = 4096.0;
    std::map<std::string, std::vector<Surface>> Pieces;
    for (size_t Oi = 0; Oi < Scene.Objects.size(); Oi++)
    {
        const Mesh& M = Scene.Objects[Oi];
        const bool IsWorld = M.Name == "world" || M.Name.compare(0, 11, "brushmodel_") != 0;
        for (size_t Si = 0; Si < M.Surfaces.size(); Si++)
        {
            const Surface& S = M.Surfaces[Si];
            std::map<std::string, std::map<uint32_t, uint32_t>> Remaps;
            for (size_t K = 0; K + 2 < S.Indices.size(); K += 3)
            {
                if (Drawn.Removed(Oi, Si, K / 3)) continue;
                if (FateOf(S, K / 3) != Fate::Model) continue;
                const uint32_t Tri[3] = { S.Indices[K], S.Indices[K + 1], S.Indices[K + 2] };
                if (Tri[0] >= S.Vertices.size() || Tri[1] >= S.Vertices.size() || Tri[2] >= S.Vertices.size()) continue;
                Stats.TrianglesInModels++;
                std::string Piece = Map + "_" + ExportNaming::SafeFileName(M.Name);
                if (IsWorld)
                {
                    double C[2] = { 0, 0 };
                    for (int J = 0; J < 3; J++)
                        for (int A = 0; A < 2; A++) C[A] += S.Vertices[Tri[J]].Pos[A] / 3.0;
                    Piece = Map + "_world_" + std::to_string((long long)std::floor(C[0] / Tile)) + "_" +
                            std::to_string((long long)std::floor(C[1] / Tile));
                    for (auto& Ch : Piece)
                        if (Ch == '-') Ch = 'm';
                }
                auto& Surfaces = Pieces[Piece];
                auto& Remap = Remaps[Piece];
                if (Surfaces.empty() || Surfaces.back().Material != S.Material || Remap.empty())
                {
                    Surfaces.push_back({ S.Material, {}, {} });
                    Remap.clear();
                }
                Surface& Into = Surfaces.back();
                for (const uint32_t I : Tri)
                {
                    auto [R, Added] = Remap.emplace(I, (uint32_t)Into.Vertices.size());
                    if (Added) Into.Vertices.push_back(S.Vertices[I]);
                    Into.Indices.push_back(R->second);
                }
            }
        }
    }
    std::vector<MapDependency> WorldTiles;   // --full-map: the models made here, which the list written before them lacks
    for (const auto& [Name, Surfaces] : Pieces)
    {
        std::vector<const Surface*> List;
        for (const Surface& S : Surfaces) List.push_back(&S);
        const std::string File = Name + ".XMODEL_EXPORT";
        if (Full != nullptr)
        {
            // a piece of the world is a model of its own: an XMODEL_BIN in the models folder, as a game's model is
            const fs::path Folder = Full->ModelsDir / ExportNaming::SafeFileName(Name);
            fs::create_directories(Folder, Ec);
            if (Ec || !WriteXmodel(List, Scene, Name, Folder / (ExportNaming::SafeFileName(Name) + "_LOD0.XMODEL_BIN"), ForGdt, true)) continue;
        }
        else
        {
            if (!WriteXmodel(List, Scene, Name, ModelDir / File, ForGdt)) continue;
            Listed(Name, File);
        }
        Stats.WorldModels++;
        if (Full != nullptr) WorldTiles.push_back({ "xmodel", Name, "worldTile", "" });
        Blocks.push_back({ Out.size(), PrefabPart::Models });
        Out += "// entity " + std::to_string(EntityNumber++) + "\n{\n";
        Out += Quote("classname", "misc_model") + Quote("model", ModelNamed(Name)) + Quote("origin", "0 0 0") + Quote("angles", "0 0 0");
        Out += "}\n";
    }
    // the list of what the map names was written before this writer ran (MapSceneExport): the world's tile models are
    // made here, so they are appended, or the report would never see them
    if (Full != nullptr && !WorldTiles.empty())
    {
        fs::path ListFile = Path;
        ListFile.replace_extension(FullMapListSuffix);
        if (!AppendDependencies(ListFile, WorldTiles)) Stats.Unresolved.push_back("the world tile models are not in " + ListFile.string());
    }
    std::ofstream File(Path, std::ios::binary);
    if (!File)
    {
        Error = "cannot create " + Path.string();
        return false;
    }
    if (Prefabs.empty())
        File << Out;
    else if (!WritePrefabs(Out, Blocks, Prefabs, Path, File, Stats, Error))
        return false;
    // The mesh coverage check (PatchCoverage): every drawn triangle is in a mesh or removed with a proof. A material that fails is listed here, and the export fails
    // (below, once its files are written): MISSING and LEFTOVER must be 0.
    if (!Stats.MeshLines.empty())
    {
        std::ofstream Lines(fs::path(Path).replace_extension(".meshes.txt"), std::ios::binary);
        for (const std::string& Line : Stats.MeshLines) Lines << Line << "\n";
    }
    const bool CoverageFailed = !Stats.CoverageLines.empty();
    if (CoverageFailed)
    {
        std::ofstream Cov(fs::path(Path).replace_extension(".coverage.txt"), std::ios::binary);
        Cov << "Mesh coverage failed for " << Stats.Mesh.LabelsFailed << " material(s): MISSING " << Stats.Mesh.CoverageMissing << ", LEFTOVER " << Stats.Mesh.CoverageLeftover << ", ACCOUNTING " << Stats.Mesh.AccountingFailed << "\n";
        for (const std::string& Block : Stats.CoverageLines) Cov << Block;
        Stats.Unresolved.push_back("mesh coverage failed for " + std::to_string(Stats.Mesh.LabelsFailed) + " material(s), see " + fs::path(Path).replace_extension(".coverage.txt").filename().string());
    }
    if (!Stats.Unresolved.empty())
    {
        std::ofstream List(fs::path(Path).replace_extension(".unresolved.txt"), std::ios::binary);
        for (const std::string& Line : Stats.Unresolved) List << Line << "\n";
    }
    if (CoverageFailed)
    {
        Error = "mesh coverage failed for " + std::to_string(Stats.Mesh.LabelsFailed) + " material(s) (MISSING " + std::to_string(Stats.Mesh.CoverageMissing) + ", LEFTOVER " +
                std::to_string(Stats.Mesh.CoverageLeftover) + ", ACCOUNTING " + std::to_string(Stats.Mesh.AccountingFailed) + "): " + fs::path(Path).replace_extension(".coverage.txt").string();
        return false;
    }
    if (ForGdt)
    {
        ForGdt->Root = Root;
        ForGdt->Map = Map;
        ForGdt->ModelEntries = std::move(Entries);
        return File.good();
    }
    if (Full != nullptr) return File.good();   // no GDT of its own
    fs::create_directories(Root / "source_data", Ec);
    std::ofstream(Root / "source_data" / (Map + ".gdt"), std::ios::binary) << Gdt::Document(Entries, "\n");
    return File.good();
}

const ISceneWriter& RadiantWriter(bool Materials, const std::string& Prefabs, const std::string& FullMapModels)
{
    // one writer per (materials, list, models folder): the lists are few and live as long as the program
    static std::mutex Lock;
    static std::map<std::tuple<bool, std::string, std::string>, std::unique_ptr<RadiantSceneWriter>> Writers;
    std::vector<std::string> Unknown;
    const std::vector<PrefabPart> Parts = ParsePrefabParts(Prefabs, Unknown);
    const std::tuple<bool, std::string, std::string> Key{ Materials, PrefabPartList(Parts), FullMapModels };
    std::lock_guard<std::mutex> Guard(Lock);
    auto& Slot = Writers[Key];
    if (!Slot) Slot = std::make_unique<RadiantSceneWriter>(Materials, Parts, fs::path(FullMapModels));
    return *Slot;
}
}

namespace Maps
{
// The writer on small synthetic scenes (stand-alone: a temp folder, no game files)
int RadiantWriterSelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::string("radiant writer: ") + What + (Got.empty() ? "" : " (" + Got.substr(0, 400) + ")") + "\n";
    };
    std::error_code Ec;
    const fs::path Dir = fs::temp_directory_path() / ("graywolf_selftest_radiant_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(Dir, Ec);
    fs::create_directories(Dir, Ec);
    const auto Box = [](float X0, float Y0, float Z0, float X1, float Y1, float Z1) {
        Brush B;
        B.Contents = 0x2081;   // solid, weaponClip, detail
        const float C[8][3] = { { X0, Y0, Z0 }, { X1, Y0, Z0 }, { X1, Y1, Z0 }, { X0, Y1, Z0 }, { X0, Y0, Z1 }, { X1, Y0, Z1 }, { X1, Y1, Z1 }, { X0, Y1, Z1 } };
        for (const auto& P : C) B.Vertices.insert(B.Vertices.end(), std::begin(P), std::end(P));
        // only three of the six sides: the hull's other faces lie on none
        const auto Side = [&B](float Nx, float Ny, float Nz, float D) {
            BrushSide S;
            S.Normal[0] = Nx, S.Normal[1] = Ny, S.Normal[2] = Nz, S.Dist = D;
            B.Sides.push_back(S);
        };
        Side(0, 0, 1, Z1);
        Side(1, 0, 0, X1);
        Side(0, 1, 0, Y1);
        return B;
    };
    const auto Read = [](const fs::path& P) {
        std::ifstream F(P, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(F), std::istreambuf_iterator<char>());
    };
    const auto Count = [](const std::string& Text, const std::string& What) {
        size_t N = 0;
        for (size_t At = Text.find(What); At != std::string::npos; At = Text.find(What, At + What.size())) N++;
        return N;
    };

    // BO3: a hull of eight corners, three sides; the contents line; unmatched faces take the brush's material
    {
        MapScene Scene;
        Scene.Game = "bo3";
        Scene.Collision.ContentsKeywords = true;
        Scene.Collision.Brushes.push_back(Box(0, 0, 0, 128, 96, 64));
        RadiantStats Stats;
        std::string Error;
        const fs::path Map = Dir / "bo3.map";
        const bool Ok = WriteRadiant(Scene, Map.string(), Stats, Error);
        const std::string Text = Read(Map);
        Check(Ok, "writes a BO3 scene", Error);
        Check(Text.find("// brush 0\n{\n  contents weaponClip detail;\n") != std::string::npos, "BO3 brush has the contents line", Text);
        Check(Count(Text, "lightmap_gray 16384 16384 0 0 0 0\n") == 6, "a hull of a box has six faces even from three sides", std::to_string(Count(Text, "lightmap_gray 16384 16384 0 0 0 0\n")));
        Check(Stats.BrushesHull == 1 && Stats.HullFacesMatched == 3 && Stats.HullFacesUnmatched == 3, "three hull faces on a side, three on none",
              std::to_string(Stats.BrushesHull) + "/" + std::to_string(Stats.HullFacesMatched) + "/" + std::to_string(Stats.HullFacesUnmatched));
        Check(Count(Text, " clip_full 64 64") == 6 || Count(Text, " caulk 64 64") == 6, "every face has the brush's material", Text);
    }
    // BO4: no contents line, a light entity, rattleEfx and an unresolved hash moved, a polar placement
    {
        MapScene Scene;
        Scene.Game = "bo4";
        Scene.Collision.Brushes.push_back(Box(0, 0, 0, 128, 96, 64));
        Scene.MapEnts = "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"rattle\"\n\"rattleEfx\" \"debris/fx8_paper_rattle.efx\"\n\"script_sound\" \"#1a2b\"\n\"targetname\" \"x\"\n}\n";
        Light L;
        L.Compiled = true;
        L.Kind = Light::Type::Point;
        L.Guid = 0x1234;
        L.Color[0] = 4;
        Scene.Lights.push_back(L);
        Instance In;
        In.Model = "some_model";
        In.Axis[0] = 5e-5f, In.Axis[1] = 0, In.Axis[2] = -1;   // forward almost straight down, left +y
        In.Axis[3] = 0, In.Axis[4] = 1, In.Axis[5] = 0;
        In.Axis[6] = 1, In.Axis[7] = 0, In.Axis[8] = 0;
        Scene.StaticInstances.push_back(In);
        RadiantStats Stats;
        std::string Error;
        const fs::path Map = Dir / "bo4.map";
        const bool Ok = WriteRadiant(Scene, Map.string(), Stats, Error);
        const std::string Text = Read(Map);
        Check(Ok, "writes a BO4 scene", Error);
        Check(Text.find("contents") == std::string::npos, "BO4 brush has no contents line");
        Check(Text.find("\"graywolf_unresolved_rattleEfx\" \"debris/fx8_paper_rattle.efx\"") != std::string::npos && Text.find("\"rattleEfx\"") == std::string::npos,
              "rattleEfx is written under graywolf_unresolved_rattleEfx", Text);
        Check(Text.find("\"graywolf_unresolved_script_sound\" \"#1a2b\"") != std::string::npos && Text.find("\"targetname\" \"x\"") != std::string::npos,
              "an unresolved hash value is moved, a named one kept");
        Check(Text.find("\"classname\" \"light\"\n\"origin\" \"0 0 0\"") != std::string::npos && Text.find("\"name\" \"graywolf_light_00001234\"") != std::string::npos &&
                  Text.find("_color") == std::string::npos,
              "a light entity with name and no _color", Text);
        Check(Text.find("\"angles\" \"90 0 0\"") != std::string::npos && Stats.PlacementsPolar == 1, "pitch 90 gives roll 0 and yaw from the left vector", Text);
    }
    // a tiny brush: every face is written with three points 64 units apart
    {
        MapScene Scene;
        Scene.Game = "bo3";
        Scene.Collision.Brushes.push_back(Box(0, 0, 0, 3, 3, 3));
        RadiantStats Stats;
        std::string Error;
        const fs::path Map = Dir / "tiny.map";
        const bool Ok = WriteRadiant(Scene, Map.string(), Stats, Error);
        const std::string Text = Read(Map);
        size_t Faces = 0, Far = 0;
        for (size_t At = Text.find("\n ( "); At != std::string::npos; At = Text.find("\n ( ", At + 1))
        {
            double V[9];
            size_t P = At + 1;
            int Got = 0;
            for (; P < Text.size() && Text[P] != '\n' && Got < 9; P++)
            {
                if (Text[P] == '(' || Text[P] == ')' || Text[P] == ' ') continue;
                char* End = nullptr;
                V[Got++] = std::strtod(&Text[P], &End);
                P = static_cast<size_t>(End - Text.c_str()) - 1;
            }
            if (Got != 9) continue;
            Faces++;
            const V3 A = { V[0], V[1], V[2] }, B = { V[3], V[4], V[5] }, C = { V[6], V[7], V[8] };
            if (Length(Sub(A, B)) > 60 && Length(Sub(C, B)) > 60) Far++;
        }
        Check(Ok && Faces == 6 && Far == 6, "a 3 unit brush's faces have points 64 apart", std::to_string(Faces) + "/" + std::to_string(Far) + " " + Error);
    }
    // a brush over the limits is split into pieces within them, not lost: 96 corners on three rings
    {
        MapScene Scene;
        Scene.Game = "bo3";
        Brush B;
        B.Contents = 1;
        for (int Z = 0; Z < 3; Z++)
            for (int K = 0; K < 32; K++)
            {
                const double A = 2 * 3.14159265358979323846 * K / 32, R = Z == 1 ? 300.0 : 256.0;
                const float P[3] = { (float)(R * std::cos(A)), (float)(R * std::sin(A)), (float)(Z * 64.0) };
                B.Vertices.insert(B.Vertices.end(), std::begin(P), std::end(P));
            }
        Scene.Collision.Brushes.push_back(B);
        RadiantStats Stats;
        std::string Error;
        const fs::path Map = Dir / "split.map";
        const bool Ok = WriteRadiant(Scene, Map.string(), Stats, Error);
        const std::string Text = Read(Map);
        Check(Ok && Stats.BrushesSplit == 1 && Stats.BrushPieces > 1 && Stats.BrushesOverLimit == 0, "a brush over 64 faces is split into pieces within the limits",
              std::to_string(Stats.BrushesSplit) + "/" + std::to_string(Stats.BrushPieces) + "/" + std::to_string(Stats.BrushesOverLimit) + " " + Error);
        // every written brush has at most 64 faces
        size_t Worst = 0, Faces = 0;
        for (size_t At = 0; At < Text.size();)
        {
            const size_t Eol = Text.find('\n', At);
            const std::string Line = Text.substr(At, (Eol == std::string::npos ? Text.size() : Eol) - At);
            if (Line == "{") Faces = 0;
            else if (Line.rfind(" ( ", 0) == 0) Worst = (std::max)(Worst, ++Faces);
            At = Eol == std::string::npos ? Text.size() : Eol + 1;
        }
        Check(Worst > 0 && Worst <= 64, "no written brush has more than 64 faces", std::to_string(Worst));
    }
    // a decal is told by the techset's name where it is a real one, and by its renderFlags where the name is a hash (Black Ops 4)
    {
        MapScene Scene;
        Scene.Game = "bo4";
        const auto Add = [&](const char* Name, const char* Techset, int64_t Flags) {
            Material& M = Scene.Materials[Name];
            M.Name = Name, M.Techset = Techset, M.RenderFlags = Flags;
        };
        Add("hash_decal", "1f2e3d4c5b6a798", 0x80000a);
        Add("hash_emissive_decal", "1f2e3d4c5b6a799", 0x800048);
        Add("hash_opaque", "1f2e3d4c5b6a79a", 0x10106);
        Add("hash_unread", "1f2e3d4c5b6a79b", -1);
        Add("hash_stub", "1f2e3d4c5b6a79c", 0);   // a stub techset reads 0: unread, not "not a decal" (SurfaceTypes::ClassOf)
        Add("named_decal", "lit_decal", -1);
        Check(Maps::IsDecalMaterial(Scene, "hash_decal") && Maps::IsDecalMaterial(Scene, "hash_emissive_decal"), "a hashed techset whose renderFlags set decal is a decal (0x80000a, 0x800048)");
        Check(!Maps::IsDecalMaterial(Scene, "hash_opaque") && !Maps::IsDecalMaterial(Scene, "hash_unread") && !Maps::IsDecalMaterial(Scene, "hash_stub"), "opaque flags, unread flags (-1) and a stub (0) are not a decal");
        Check(Maps::IsDecalMaterial(Scene, "named_decal") && !Maps::IsDecalMaterial(Scene, "absent"), "a techset name with decal is a decal; an absent material is not");
        Check(SurfaceTypes::ClassOf("bo4", Scene.Materials["hash_stub"].RenderFlags) == SurfaceTypes::RenderClass::Unread && SurfaceTypes::ClassOf("bo4", Scene.Materials["hash_unread"].RenderFlags) == SurfaceTypes::RenderClass::Unread &&
              SurfaceTypes::ClassOf("bo4", Scene.Materials["hash_opaque"].RenderFlags) == SurfaceTypes::RenderClass::NotDecal, "a stub (0) and an unread word (-1) are Unread, not NotDecal; 0x10106 is NotDecal");
        Scene.Game = "bo3";
        Check(!Maps::IsDecalMaterial(Scene, "hash_decal"), "a game with no render rows gets no decal from flags");
    }
    // prefab separation: every part, a subset, an empty part
    {
        const auto Surf = [](const char* Material, float X) {
            Surface S;
            S.Material = Material;
            for (int K = 0; K < 3; K++)
            {
                Vertex V;
                V.Pos[0] = X + (K == 1 ? 40.0f : 0.0f), V.Pos[1] = 5000.0f + (K == 2 ? 40.0f : 0.0f), V.Pos[2] = 700.0f;
                V.Normal[0] = 0, V.Normal[1] = 0, V.Normal[2] = 1;
                S.Vertices.push_back(V);
                S.Indices.push_back((uint32_t)K);
            }
            return S;
        };
        MapScene Scene;
        Scene.Game = "bo4";
        Scene.Collision.Brushes.push_back(Box(0, 0, 0, 128, 96, 64));
        Scene.Collision.Models.resize(2);
        Scene.Collision.Models[1].push_back(Box(0, 0, 0, 32, 32, 32));
        const float Tri[9] = { 0, 0, 900, 64, 0, 900, 0, 64, 900 };
        Scene.Collision.Vertices.assign(std::begin(Tri), std::end(Tri));
        Scene.Collision.Triangles = { 0, 1, 2 };
        Mesh World;
        World.Name = "world";
        World.Surfaces.push_back(Surf("wall_mat", 3000));
        World.Surfaces.push_back(Surf("decal_mat", 4000));
        Scene.Objects.push_back(World);
        Scene.Materials["wall_mat"].Name = "wall_mat";
        Scene.Materials["wall_mat"].Techset = "lit";
        Scene.Materials["decal_mat"].Name = "decal_mat";
        Scene.Materials["decal_mat"].Techset = "lit_decal";
        Scene.MapEnts = "{\n\"classname\" \"worldspawn\"\n}\n"
                        "{\n\"classname\" \"script_struct\"\n\"targetname\" \"s1\"\n}\n"
                        "{\n\"classname\" \"script_struct\"\n\"targetname\" \"s2\"\n\"rattleEfx\" \"a/b.efx\"\n}\n"
                        "{\n\"classname\" \"fx_marker\"\n}\n"
                        "{\n\"classname\" \"trigger_multiple\"\n\"model\" \"*1\"\n}\n"
                        "{\n\"classname\" \"misc_model\"\n\"model\" \"some_model\"\n}\n"
                        "{\n\"classname\" \"reflection_probe\"\n}\n";
        Light L;
        L.Bo3 = true;
        L.Kind = Light::Type::Point;
        L.Guid = 1;
        Scene.Lights.push_back(L);
        Instance In;
        In.Model = "placed_model";
        Scene.StaticInstances.push_back(In);

        const auto Run = [&](const char* Name, const std::vector<PrefabPart>& Parts, RadiantStats& Stats) {
            const fs::path Map = Dir / Name / (std::string(Name) + ".map");
            fs::create_directories(Map.parent_path(), Ec);
            std::string Error;
            const bool Ok = WriteRadiant(Scene, Map.string(), Stats, Error, nullptr, Parts);
            Check(Ok, "writes the prefab scene", Error);
            return Map;
        };
        // entities besides the worldspawn and the misc_prefab calls, and brushes and meshes, of a map text
        struct Tally
        {
            size_t Entities = 0, Brushes = 0, Meshes = 0, Worlds = 0, Calls = 0, Open = 0, Close = 0;
        };
        const auto Tallied = [&](const std::string& Text) {
            Tally T;
            T.Worlds = Count(Text, "\"classname\" \"worldspawn\"\n");
            T.Calls = Count(Text, "\"classname\" \"misc_prefab\"\n");
            T.Entities = Count(Text, "// entity ") - T.Worlds - T.Calls;
            T.Meshes = Count(Text, "\n  mesh\n");
            T.Brushes = Count(Text, "// brush ") - T.Meshes;
            // braces by line: the lines "{" and "}" with any indent
            for (size_t At = 0; At < Text.size();)
            {
                const size_t Eol = Text.find('\n', At);
                size_t First = At;
                while (First < Text.size() && Text[First] == ' ') First++;
                const std::string Line = Text.substr(First, (Eol == std::string::npos ? Text.size() : Eol) - First);
                T.Open += Line == "{";
                T.Close += Line == "}";
                At = Eol == std::string::npos ? Text.size() : Eol + 1;
            }
            return T;
        };
        RadiantStats Plain, All;
        const fs::path PlainMap = Run("plain", {}, Plain);
        const fs::path AllMap = Run("all", { PrefabPart::Brushes, PrefabPart::Patches, PrefabPart::Decals, PrefabPart::Models, PrefabPart::Effects,
                                             PrefabPart::Entities, PrefabPart::BrushEntities, PrefabPart::Lights }, All);
        const Tally Whole = Tallied(Read(PlainMap));
        Check(Whole.Brushes > 0 && Whole.Meshes > 0 && Whole.Entities > 6, "the unsplit prefab scene has brushes, meshes and entities",
              std::to_string(Whole.Brushes) + "/" + std::to_string(Whole.Meshes) + "/" + std::to_string(Whole.Entities));
        Check(All.PrefabFiles.size() == 8, "every part has something: eight files", std::to_string(All.PrefabFiles.size()));
        // every file is a standalone map; the sums equal the unsplit map
        Tally Sum = Tallied(Read(AllMap));
        const Tally Main = Sum;
        Check(Main.Calls == All.PrefabFiles.size() && Main.Worlds == 1, "the main map has one misc_prefab per file and its worldspawn",
              std::to_string(Main.Calls) + "/" + std::to_string(Main.Worlds));
        for (const RadiantPrefabFile& F : All.PrefabFiles)
        {
            const fs::path File = AllMap.parent_path() / F.File;
            const std::string Text = Read(File);
            const Tally T = Tallied(Text);
            const std::string Name = "all_prefab_" + PrefabPartFileName(F.Part) + ".map";
            Check(F.File == Name, "the file is <map>_prefab_<part>.map", F.File);
            Check(Text.rfind("iwmap 4\n", 0) == 0 && Text.find("// entity 0\n{\n\"classname\" \"worldspawn\"\n") != std::string::npos && T.Worlds == 1 && T.Calls == 0,
                  "a prefab file is a map with a worldspawn", F.File);
            Check(T.Open == T.Close && Text.back() == '\n', "a prefab file's braces balance", F.File);
            Check(T.Entities == F.Entities && T.Brushes == F.Brushes && T.Meshes == F.Meshes, "the counts of a prefab file are what it holds", F.File);
            Check(T.Entities + T.Brushes + T.Meshes > 0, "a prefab file is not empty", F.File);
            Check(Read(AllMap).find("\"model\" \"graywolf_maps/" + F.File + "\"\n\"origin\" \"0 0 0\"\n}\n") != std::string::npos,
                  "the main map calls the file by a path relative to map_source", F.File);
            Sum.Entities += T.Entities, Sum.Brushes += T.Brushes, Sum.Meshes += T.Meshes;
        }
        Check(Sum.Entities == Whole.Entities && Sum.Brushes == Whole.Brushes && Sum.Meshes == Whole.Meshes, "the entities, brushes and meshes of the files add up to the unsplit map",
              std::to_string(Sum.Entities) + "/" + std::to_string(Sum.Brushes) + "/" + std::to_string(Sum.Meshes) + " against " + std::to_string(Whole.Entities) + "/" +
                  std::to_string(Whole.Brushes) + "/" + std::to_string(Whole.Meshes));
        Check(All.MainEntities == Main.Entities && All.MainBrushes == Main.Brushes && All.MainMeshes == Main.Meshes, "the stats of the main map are what it holds",
              std::to_string(All.MainEntities) + "/" + std::to_string(All.MainBrushes) + "/" + std::to_string(All.MainMeshes));
        // what went where
        const auto Held = [&](PrefabPart P, const char* What) {
            return Read(AllMap.parent_path() / ("all_prefab_" + PrefabPartFileName(P) + ".map")).find(What) != std::string::npos;
        };
        Check(Held(PrefabPart::Lights, "\"classname\" \"reflection_probe\"") && Held(PrefabPart::Lights, "\"classname\" \"light\""), "lights and probes are in the lights file");
        Check(Held(PrefabPart::Models, "\"classname\" \"misc_model\"\n\"model\" \"some_model\"") && Held(PrefabPart::Models, "\"model\" \"placed_model\""), "misc_models are in the models file");
        Check(Held(PrefabPart::Effects, "\"classname\" \"fx_marker\"") && Held(PrefabPart::Effects, "\"graywolf_unresolved_rattleEfx\""), "an fx class and an unresolved effect key are in the effects file");
        Check(Held(PrefabPart::BrushEntities, "\"classname\" \"trigger_multiple\""), "an entity with brushes is in the brush-entities file");
        Check(Held(PrefabPart::Entities, "\"targetname\" \"s1\"") && !Held(PrefabPart::Entities, "rattleEfx"), "the others are in the entities file");
        Check(Held(PrefabPart::Decals, "lit_decal") || Held(PrefabPart::Decals, "decal_mat"), "a decal patch is in the decals file");
        Check(!Held(PrefabPart::Patches, "decal_mat") && Held(PrefabPart::Patches, "wall_mat"), "a decal is not in the patches file, a wall patch is");
        Check(Held(PrefabPart::Brushes, "// brush 0\n{\n") && !Held(PrefabPart::Brushes, "func_group"), "the world's brushes are in the brushes file");

        // a subset: the rest stays in the main map, no file for a part not asked for
        RadiantStats Some;
        const fs::path SomeMap = Run("some", { PrefabPart::Models }, Some);
        const std::string SomeText = Read(SomeMap);
        Check(Some.PrefabFiles.size() == 1 && fs::exists(SomeMap.parent_path() / "some_prefab_models.map") && !fs::exists(SomeMap.parent_path() / "some_prefab_lights.map"),
              "only the asked part has a file", std::to_string(Some.PrefabFiles.size()));
        Check(SomeText.find("\"classname\" \"reflection_probe\"") != std::string::npos && SomeText.find("\"classname\" \"misc_model\"") == std::string::npos,
              "the other parts stay in the main map, the asked one leaves it");

        // an empty part writes no file and no call: the map is the one without the option
        Scene.Lights.clear();
        Scene.MapEnts = "{\n\"classname\" \"worldspawn\"\n}\n";
        RadiantStats Empty, Same;
        const fs::path EmptyMap = Run("empty", { PrefabPart::Lights, PrefabPart::Effects }, Empty);
        const fs::path SameMap = Run("empty_plain", {}, Same);
        std::string EmptyText = Read(EmptyMap), SameText = Read(SameMap);
        Check(Empty.PrefabFiles.empty() && !fs::exists(EmptyMap.parent_path() / "empty_prefab_lights.map") && !fs::exists(EmptyMap.parent_path() / "empty_prefab_effects.map"),
              "an empty part writes no file");
        Check(EmptyText.find("misc_prefab") == std::string::npos && EmptyText == SameText, "an empty part leaves the main map as it was", EmptyText.substr(0, 200));
    }
    // --full-map: the placed models are named as the publish step names them and not written, the pieces of the world
    // are XMODEL_BIN in the models folder, and there is no <map>_radiant folder or GDT of its own
    {
        MapScene Scene;
        Scene.Game = "bo4";
        Instance In;
        In.Model = "P7/Some-Model";
        Scene.StaticInstances.push_back(In);
        Mesh Prop;
        Prop.Name = In.Model;
        Surface PropSurface;
        PropSurface.Material = "mtl_prop";
        Scene.Models[In.Model] = Prop;
        // a drawn triangle without a material is a piece of the world (a misc_model), one with is a patch
        Mesh World;
        World.Name = "world";
        Surface Tri;
        Tri.Material = "mtl_floor";
        Vertex A, B, C;
        A.Pos[0] = 0, A.Pos[1] = 0, A.Pos[2] = 0;
        B.Pos[0] = 64, B.Pos[1] = 0, B.Pos[2] = 0;
        C.Pos[0] = 0, C.Pos[1] = 64, C.Pos[2] = 0;
        Tri.Vertices = { A, B, C };
        Tri.Indices = { 0, 1, 2 };
        World.Surfaces.push_back(Tri);
        Surface Loose = Tri;
        Loose.Material.clear();   // no material to give a patch: the triangle is a piece of a world model
        World.Surfaces.push_back(Loose);
        Scene.Objects.push_back(World);
        Scene.MapEnts = "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"script_model\"\n\"model\" \"P7/Other\"\n\"prop_model\" \"#abc\"\n}\n"
                        "{\n\"classname\" \"script_brushmodel\"\n\"model\" \"*3\"\n}\n";
        const fs::path Models = Dir / "full_models";
        const RadiantFullMap Full{ Models };
        RadiantStats Stats;
        RadiantGdt Names;
        std::string Error;
        const fs::path Map = Dir / "full" / "full.map";
        fs::create_directories(Map.parent_path(), Ec);
        const bool Ok = WriteRadiant(Scene, Map.string(), Stats, Error, &Names, {}, &Full);
        const std::string Text = Read(Map);
        Check(Ok, "writes a --full-map scene", Error);
        Check(Text.find("\"model\" \"p7_some_model\"") != std::string::npos && Text.find("\"model\" \"p7_other\"") != std::string::npos,
              "the placed and the entity models are named as the publish step names them", Text);
        Check(Text.find("\"prop_model\" \"#abc\"") == std::string::npos && Text.find("\"graywolf_unresolved_prop_model\" \"#abc\"") != std::string::npos,
              "an unresolved hash model stays an unresolved key, not a name", Text);
        Check(!fs::exists(Map.parent_path() / "full_radiant"), "there is no <map>_radiant folder");
        Check(Stats.ModelsPublished == 1 && Stats.ModelFiles == 0, "the placed model is left to the publish step", std::to_string(Stats.ModelsPublished));
        Check(Stats.WorldModels == 1 && fs::is_regular_file(Models / "full_world_0_0" / "full_world_0_0_LOD0.XMODEL_BIN", Ec) &&
                  Text.find("\"model\" \"full_world_0_0\"") != std::string::npos,
              "a piece of the world is an XMODEL_BIN in the models folder", Text);
        Check(Names.Materials.count("mtl_floor") == 1, "the materials are named for the publish step");
    }
    // a drawn 3 x 2 grid of quads (one surface, shared vertices) is one mesh, not twelve; the counters add up; a removed triangle stays removed; the files list the owner
    {
        MapScene Scene;
        Scene.Game = "bo4";
        Mesh World;
        World.Name = "world";
        Surface Grid;
        Grid.Material = "mtl_floor";
        for (int J = 0; J <= 2; J++)
            for (int I = 0; I <= 3; I++)
            {
                Vertex V;
                V.Pos[0] = I * 64.0f, V.Pos[1] = J * 64.0f, V.Pos[2] = 0;
                V.Normal[2] = 1, V.NormalRead = true;
                V.Uv[0] = static_cast<float>(I) / 3, V.Uv[1] = static_cast<float>(J) / 2;
                Grid.Vertices.push_back(V);
            }
        for (uint32_t J = 0; J < 2; J++)
            for (uint32_t I = 0; I < 3; I++)
            {
                const uint32_t A = J * 4 + I, B = A + 1, C = A + 5, D = A + 4;
                // counter-clockwise from above: cross(b - a, c - a) points up, the vertex normal too (a game whose winding is +1)
                for (const uint32_t K : { A, B, C, A, C, D }) Grid.Indices.push_back(K);
            }
        World.Surfaces.push_back(Grid);
        Scene.Objects.push_back(World);
        Scene.Game = "h1";   // winding +1 is not measured for it, but the vertex normals decide here
        Scene.MapEnts = "{\n\"classname\" \"worldspawn\"\n}\n";
        RadiantStats Stats;
        std::string Error;
        const fs::path Map = Dir / "grid.map";
        const bool Ok = WriteRadiant(Scene, Map.string(), Stats, Error);
        const std::string Text = Read(Map);
        size_t Meshes = 0;
        for (size_t At = Text.find("\n  mesh\n"); At != std::string::npos; At = Text.find("\n  mesh\n", At + 1)) Meshes++;
        Check(Ok && Meshes == 1 && Stats.Patches == 1 && Stats.Mesh.TrianglesIn == 12 && Stats.Mesh.Covered == 12 && Stats.Mesh.CoverageMissing == 0 && Stats.Mesh.CoverageLeftover == 0 && Stats.PatchesFacingAgainst == 0,
              "a drawn grid of quads is one mesh with every triangle covered and no facing against the normals", Error + " " + std::to_string(Meshes) + " " + std::to_string(Stats.Patches));
        Check(Text.find("   lightmap_gray\n   3 4 16 8\n") != std::string::npos || Text.find("   lightmap_gray\n   4 3 16 8\n") != std::string::npos, "the size line is the grid's 4 x 3 points", Text.substr(0, 400));
        Check(fs::exists(fs::path(Map).replace_extension(".meshes.txt")) && !fs::exists(fs::path(Map).replace_extension(".coverage.txt")), "the owner is listed in <map>.meshes.txt and no coverage failure file is made");
    }
    fs::remove_all(Dir, Ec);
    return Failed;
}
}
