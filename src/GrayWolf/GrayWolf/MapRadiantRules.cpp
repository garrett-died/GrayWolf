#include "stdafx.h"

#include "MapRadiantRules.h"

#include "MapFullExport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace Maps
{
using namespace RadiantGeometry;

std::string RadiantNumber(double V)
{
    if (std::fabs(V - std::round(V)) < 1e-9) return std::to_string((long long)std::llround(V));
    char Buf[64];
    std::snprintf(Buf, sizeof(Buf), "%.4f", V);
    std::string S = Buf;
    while (!S.empty() && S.back() == '0') S.pop_back();
    if (!S.empty() && S.back() == '.') S.pop_back();
    return S == "-0" ? "0" : S;
}

// ---- BO4 static models

StaticBoundsResult StaticModelBounds(const std::vector<StaticPlacementBox>& Placements)
{
    constexpr double Tolerance = 0.05;   // the stored bounds are floats of world coordinates
    struct Local
    {
        double Box[6];
    };
    std::map<uint32_t, std::vector<Local>> ByModel;
    for (const StaticPlacementBox& P : Placements)
    {
        const bool Upright = std::fabs(P.Rotation[0]) < 1e-4 && std::fabs(P.Rotation[1]) < 1e-4 && std::fabs(P.Rotation[2]) < 1e-4 &&
                             std::fabs(std::fabs(P.Rotation[3]) - 1.0) < 1e-4;
        if (!Upright || std::fabs(P.Scale - 1.0f) > 1e-3f) continue;
        Local L;
        for (int K = 0; K < 6; K++) L.Box[K] = (double)P.Bounds[K] - (double)P.Origin[K % 3];
        ByModel[P.Model].push_back(L);
    }
    StaticBoundsResult R;
    for (const auto& [Model, List] : ByModel)
    {
        if (List.size() < 2) continue;
        bool One = true;
        for (const Local& L : List)
            for (int K = 0; K < 6; K++) One = One && std::fabs(L.Box[K] - List[0].Box[K]) <= Tolerance;
        (One ? R.Hold : R.Fail)++;
    }
    return R;
}

// ---- patches

std::string PatchTexture(double U, double V) { return "t " + RadiantNumber(U * 1024.0) + " " + RadiantNumber(V * 1024.0); }
std::string PatchRunningTexture(double RunU, double RunV) { return "t " + RadiantNumber(RunU) + " " + RadiantNumber(-RunV); }
std::string PatchColour(const uint8_t (&C)[4])
{
    if (C[0] == 255 && C[1] == 255 && C[2] == 255 && C[3] == 255) return std::string();
    return " c " + std::to_string(C[0]) + " " + std::to_string(C[1]) + " " + std::to_string(C[2]) + " " + std::to_string(C[3]);
}

// ---- brush hulls

std::vector<HullFace> HullFaces(const std::vector<V3>& P)
{
    const size_t N = P.size();
    if (N < 4 || N > MaxHullVertices) return {};
    // the planes found, each with every vertex on it: three vertices of a near-coplanar set give the same plane a
    // hair apart, and the plane (normal within 2e-3, distance within 0.05, as MatchSide) is one face, not one
    // per triple. The vertex sets of the triples that agree are merged.
    struct Candidate
    {
        V3 Normal;
        double Dist = 0;
        std::set<size_t> On;
    };
    std::vector<Candidate> Planes;
    for (size_t A = 0; A < N; A++)
        for (size_t B = A + 1; B < N; B++)
        {
            const V3 AB = Sub(P[B], P[A]);
            for (size_t C = B + 1; C < N; C++)
            {
                V3 Normal = Cross(AB, Sub(P[C], P[A]));
                const double L = Length(Normal);
                if (L < 1e-6) continue;
                Normal = Mul(Normal, 1.0 / L);
                double Dist = Dot(Normal, P[A]);
                bool Above = false, Below = false;
                std::vector<size_t> On;
                for (size_t K = 0; K < N && !(Above && Below); K++)
                {
                    const double S = Dot(Normal, P[K]) - Dist;
                    if (S > 0.02) Above = true;
                    else if (S < -0.02) Below = true;
                    else On.push_back(K);
                }
                if (Above && Below) continue;
                if (Above) Normal = Mul(Normal, -1), Dist = -Dist;
                if (On.size() < 3) continue;
                Candidate* Known = nullptr;
                for (Candidate& Plane : Planes)
                    if (std::fabs(Plane.Normal[0] - Normal[0]) < 2e-3 && std::fabs(Plane.Normal[1] - Normal[1]) < 2e-3 &&
                        std::fabs(Plane.Normal[2] - Normal[2]) < 2e-3 && std::fabs(Plane.Dist - Dist) < 0.05)
                    {
                        Known = &Plane;
                        break;
                    }
                if (Known == nullptr)
                {
                    Planes.push_back({ Normal, Dist, {} });
                    Known = &Planes.back();
                }
                Known->On.insert(On.begin(), On.end());
            }
        }
    std::vector<HullFace> Out;
    for (const Candidate& Plane : Planes)
    {
        const std::vector<size_t> On(Plane.On.begin(), Plane.On.end());
        HullFace F;
        F.Normal = Plane.Normal;
        F.Dist = Plane.Dist;
        // counter-clockwise about the normal (seen from outside), points that coincide once
        V3 Centre = { 0, 0, 0 };
        for (const size_t K : On) Centre = Add(Centre, Mul(P[K], 1.0 / (double)On.size()));
        V3 U = Sub(P[On[0]], Centre);
        U = Sub(U, Mul(F.Normal, Dot(U, F.Normal)));
        if (Length(U) < 1e-9) continue;
        U = Mul(U, 1.0 / Length(U));
        const V3 W = Cross(F.Normal, U);
        std::vector<std::pair<double, size_t>> Order;
        for (const size_t K : On) Order.emplace_back(std::atan2(Dot(Sub(P[K], Centre), W), Dot(Sub(P[K], Centre), U)), K);
        std::sort(Order.begin(), Order.end());
        for (const auto& [Angle, K] : Order)
        {
            bool Same = false;
            for (const size_t J : F.Index) Same = Same || Length(Sub(P[J], P[K])) < 1e-4;
            if (!Same) F.Index.push_back(K);
        }
        if (F.Index.size() >= 3) Out.push_back(std::move(F));
    }
    return Out.size() >= 4 ? Out : std::vector<HullFace>();
}

bool WithinRadiantLimits(const std::vector<HullFace>& Faces)
{
    if (Faces.size() > MaxBrushFaces) return false;
    for (const HullFace& F : Faces)
        if (F.Index.size() > MaxFacePoints) return false;
    return true;
}

namespace
{
    constexpr size_t MaxPieces = 64;
    constexpr int MaxDepth = 12;
    constexpr double Away = 0.02;       // a vertex this near a cutting plane, not on it, spoils it
    constexpr double OnCut = 1e-6;      // on it

    struct Cut
    {
        V3 Normal;
        double Dist = 0;
        bool Found = false;
    };

    // The plane through three of the vertices with some strictly on both sides and none within Away
    // but not on it, the most balanced
    Cut ChoosePlane(const std::vector<V3>& P)
    {
        Cut Best;
        size_t BestScore = 0;
        const size_t N = P.size();
        for (size_t A = 0; A < N; A++)
            for (size_t B = A + 1; B < N; B++)
                for (size_t C = B + 1; C < N; C++)
                {
                    V3 Normal = Cross(Sub(P[B], P[A]), Sub(P[C], P[A]));
                    const double L = Length(Normal);
                    if (L < 1e-6) continue;
                    Normal = Mul(Normal, 1.0 / L);
                    const double Dist = Dot(Normal, P[A]);
                    size_t Front = 0, Back = 0;
                    bool Spoiled = false;
                    for (size_t K = 0; K < N && !Spoiled; K++)
                    {
                        const double S = Dot(Normal, P[K]) - Dist;
                        if (std::fabs(S) <= OnCut) continue;
                        if (std::fabs(S) <= Away) Spoiled = true;
                        else (S > 0 ? Front : Back)++;
                    }
                    if (Spoiled || !Front || !Back) continue;
                    if (const size_t Score = (std::min)(Front, Back); Score > BestScore)
                    {
                        BestScore = Score;
                        Best = { Normal, Dist, true };
                    }
                }
        return Best;
    }

    void Add1(std::vector<V3>& Into, const V3& P)
    {
        for (const V3& Q : Into)
            if (Length(Sub(P, Q)) < 1e-5) return;
        Into.push_back(P);
    }

    // The hull's two halves by the plane: the vertices on each side (the plane's on both), and where
    // every edge two faces share crosses it
    void Halves(const std::vector<V3>& P, const std::vector<HullFace>& Faces, const Cut& C, std::vector<V3>& Front, std::vector<V3>& Back)
    {
        std::vector<double> S(P.size());
        for (size_t K = 0; K < P.size(); K++)
        {
            S[K] = Dot(C.Normal, P[K]) - C.Dist;
            if (S[K] >= -OnCut) Add1(Front, P[K]);
            if (S[K] <= OnCut) Add1(Back, P[K]);
        }
        std::map<std::pair<size_t, size_t>, int> Edges;
        for (const HullFace& F : Faces)
            for (size_t K = 0; K < F.Index.size(); K++)
            {
                const size_t A = F.Index[K], B = F.Index[(K + 1) % F.Index.size()];
                Edges[{ (std::min)(A, B), (std::max)(A, B) }]++;
            }
        for (const auto& [Edge, Count] : Edges)
        {
            if (Count < 2) continue;
            const double Sa = S[Edge.first], Sb = S[Edge.second];
            if ((Sa > OnCut && Sb < -OnCut) || (Sa < -OnCut && Sb > OnCut))
            {
                const V3 Crossing = Add(P[Edge.first], Mul(Sub(P[Edge.second], P[Edge.first]), Sa / (Sa - Sb)));
                Add1(Front, Crossing);
                Add1(Back, Crossing);
            }
        }
    }

    // Hull: the faces of P when the caller has them (the first level only); Nodes counts the calls, which end at
    // MaxSplitNodes: a brush that needs more is not worth the hull each node costs (96 vertices: millions of
    // triples a node).
    bool TrySplit(const std::vector<V3>& P, int Depth, std::vector<std::vector<V3>>& Out, std::string& Why, size_t& Nodes, size_t MaxNodes,
                  const std::vector<HullFace>* Hull = nullptr)
    {
        if (++Nodes > MaxNodes)
        {
            Why = "more than " + std::to_string(MaxNodes) + " split nodes";
            return false;
        }
        const std::vector<HullFace> Faces = Hull != nullptr ? *Hull : HullFaces(P);
        if (Faces.empty())
        {
            Why = "a piece is not a solid";
            return false;
        }
        if (WithinRadiantLimits(Faces))
        {
            if (Out.size() >= MaxPieces)
            {
                Why = "more than 64 pieces";
                return false;
            }
            Out.push_back(P);
            return true;
        }
        if (Depth >= MaxDepth)
        {
            Why = "still over a limit after 12 splits";
            return false;
        }
        const Cut C = ChoosePlane(P);
        if (!C.Found)
        {
            Why = "no plane through three vertices has vertices strictly on both sides and none within 0.02 of it";
            return false;
        }
        std::vector<V3> Front, Back;
        Halves(P, Faces, C, Front, Back);
        return TrySplit(Front, Depth + 1, Out, Why, Nodes, MaxNodes) && TrySplit(Back, Depth + 1, Out, Why, Nodes, MaxNodes);
    }
}

Split SplitForRadiant(const std::vector<V3>& Points, const std::vector<HullFace>* Hull, size_t MaxNodes)
{
    Split R;
    size_t Nodes = 0;
    if (!TrySplit(Points, 0, R.Pieces, R.Failure, Nodes, MaxNodes, Hull)) R.Pieces.clear();
    R.Nodes = Nodes;
    return R;
}

int MatchSide(const HullFace& Face, const std::vector<std::pair<V3, double>>& Planes)
{
    for (size_t K = 0; K < Planes.size(); K++)
    {
        const auto& [N, D] = Planes[K];
        if (std::fabs(N[0] - Face.Normal[0]) < 2e-3 && std::fabs(N[1] - Face.Normal[1]) < 2e-3 && std::fabs(N[2] - Face.Normal[2]) < 2e-3 &&
            std::fabs(D - Face.Dist) < 0.05)
            return (int)K;
    }
    return -1;
}

bool IsTinyFace(const V3& A, const V3& B, const V3& C)
{
    const V3 U = Sub(B, A), V = Sub(C, A);
    const double Span = Length(Cross(U, V)), Product = Length(U) * Length(V);
    return Span < TinyFaceSpan || Product <= 0 || Span / Product < SkinnyFaceSine;
}

// ---- brush contents

std::string ContentsKeywords(uint32_t Contents)
{
    std::string S;
    for (const auto& [Bit, Word] : { std::pair<uint32_t, const char*>{ 0x80, "weaponClip" }, { 0x2000, "detail" }, { 0x80000, "umbraTarget" } })
        if (Contents & Bit) S += (S.empty() ? "" : " ") + std::string(Word);
    return S;
}

// ---- entities

namespace
{
    std::string Pair(const std::string& Key, const std::string& Value) { return "\"" + Key + "\" \"" + Value + "\"\n"; }
    std::string Three(const float (&V)[3]) { return RadiantNumber(V[0]) + " " + RadiantNumber(V[1]) + " " + RadiantNumber(V[2]); }
}

namespace
{
    // Black Ops III: the keys proven against the stock lights (docs/LIGHT-CONVERSION.md); the rest unresolved
    std::string Bo3LightKeys(const Light& L, const std::string& Prefix)
    {
        constexpr double Deg = 180.0 / 3.14159265358979323846;
        const double Fx = L.Forward[0], Fy = L.Forward[1], Fz = L.Forward[2];
        // the stock lights' angles run 0 to 360 (a light straight up has pitch 270)
        const auto Wrap = [](double A) { return A < 0 ? A + 360.0 : A; };
        const double Pitch = Wrap(std::asin(std::max(-1.0, std::min(1.0, -Fz))) * Deg);
        const double Yaw = std::hypot(Fx, Fy) < 1e-4 ? 0.0 : Wrap(std::atan2(Fy, Fx) * Deg);
        char Name[32];
        std::snprintf(Name, sizeof(Name), "%08x", L.Guid);
        const bool Spot = L.Kind == Light::Type::Spot;
        std::string S = Pair("classname", "light");
        S += Pair("origin", Three(L.Origin));
        // the roll is not in the record (docs/LIGHT-CONVERSION.md): written 0
        S += Pair("angles", RadiantNumber(Pitch) + " " + RadiantNumber(Yaw) + " 0");
        S += Pair("PRIMARY_TYPE", Spot ? "PRIMARY_SPOT" : "PRIMARY_OMNI");
        S += Pair("name", Prefix + "light_" + Name);
        S += Pair("radius", RadiantNumber(L.Radius));
        S += Pair("fov_outer", RadiantNumber(2.0 * std::acos(std::max(-1.0, std::min(1.0, (double)L.CosHalfFovOuter))) * Deg));
        S += Pair("near_edge", RadiantNumber(L.NearEdge));
        S += Pair("far_edge", RadiantNumber(L.FarEdge));
        // a light with its falloff off keeps twice its radius there; Radiant's default distance stays
        if (std::fabs((double)L.FalloffDistance - 2.0 * L.Radius) < 1e-3 * std::max(1.0, (double)L.Radius))
            S += Pair("ENABLE_FALLOFF", "0");
        else
            S += Pair("falloffdistance", RadiantNumber(L.FalloffDistance));
        if (L.SpecScale > 0) S += Pair("spec_comp", RadiantNumber(std::log2((double)L.SpecScale)));
        S += Pair("cut_on", RadiantNumber(L.CutOn));
        S += Pair("penumbraRadius", RadiantNumber(L.Penumbra));
        S += Pair("bulbRadius", RadiantNumber(L.BulbRadius));
        if (Spot)
            S += Pair("superellipse", RadiantNumber(L.SuperEllipse[0]) + " " + RadiantNumber(L.SuperEllipse[1]) + " " + RadiantNumber(L.SuperEllipse[2]) + " " +
                                          RadiantNumber(L.SuperEllipse[3]));
        S += Pair(Prefix + "unresolved_radiance", Three(L.Color));
        return S;
    }
}

std::string LightEntityKeys(const Light& L, const std::string& Prefix)
{
    if (L.Bo3) return L.Kind == Light::Type::Point || L.Kind == Light::Type::Spot ? Bo3LightKeys(L, Prefix) : std::string();
    if (!L.Compiled || (L.Kind != Light::Type::Point && L.Kind != Light::Type::Spot)) return std::string();
    constexpr double Deg = 180.0 / 3.14159265358979323846;
    const double Pitch = std::atan2(-(double)L.Dir[2], std::sqrt((double)L.Dir[0] * L.Dir[0] + (double)L.Dir[1] * L.Dir[1])) * Deg;
    const double Yaw = std::atan2((double)L.Dir[1], (double)L.Dir[0]) * Deg;
    const double Roll = std::atan2(-(double)L.Right[2], (double)L.Up[2]) * Deg;
    char Name[32];
    std::snprintf(Name, sizeof(Name), "%08x", L.Guid);
    std::string S = Pair("classname", "light");
    S += Pair("origin", Three(L.Origin));
    S += Pair("angles", RadiantNumber(Pitch) + " " + RadiantNumber(Yaw) + " " + RadiantNumber(Roll));
    S += Pair("PRIMARY_TYPE", L.Kind == Light::Type::Point ? "PRIMARY_OMNI" : "PRIMARY_SPOT");
    S += Pair("name", Prefix + "light_" + Name);
    const std::string Un = Prefix + "unresolved_";
    S += Pair(Un + (L.ColorKey.empty() ? std::string("rgb_at_168") : L.ColorKey), Three(L.Color));
    if (!L.OnlyColor)
    {
        S += Pair(Un + "float_at_204", RadiantNumber(L.Unresolved204));
        S += Pair(Un + "float_at_448", RadiantNumber(L.Unresolved448));
    }
    if (!L.Def.empty()) S += Pair(Un + "def", L.Def);
    return S;
}

std::string StaticModelAngles(const float (&Axis)[9])
{
    constexpr double Deg = 180.0 / 3.14159265358979323846;
    const double Fx = Axis[0], Fy = Axis[1], Fz = Axis[2], Lx = Axis[3], Ly = Axis[4], Lz = Axis[5], Uz = Axis[8];
    const double Cp = std::hypot(Fx, Fy);
    const double Pitch = std::atan2(-Fz, Cp) * Deg;
    double Yaw, Roll;
    if (Cp >= 1e-4)
    {
        Yaw = std::atan2(Fy, Fx) * Deg;
        Roll = std::atan2(Lz, Uz) * Deg;
    }
    else
    {
        Yaw = std::atan2(-Lx, Ly) * Deg;
        Roll = 0;
    }
    const auto Round = [](double V) { return std::fabs(V - std::round(V)) < 1e-3 ? std::round(V) : V; };
    return RadiantNumber(Round(Pitch)) + " " + RadiantNumber(Round(Yaw)) + " " + RadiantNumber(Round(Roll));
}

std::string UnresolvedKey(const std::string& Key) { return "graywolf_unresolved_" + Key; }

// ---- prefab separation

namespace
{
    constexpr const char* PartNames[PrefabPartCount] = { "brushes", "patches", "decals", "models", "effects", "entities", "brush-entities", "lights" };
    std::string LowerOf(std::string S)
    {
        for (char& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    }
    bool EndsWith(const std::string& S, const char* Tail)
    {
        const size_t N = std::strlen(Tail);
        return S.size() >= N && S.compare(S.size() - N, N, Tail) == 0;
    }
}

const char* PrefabPartName(PrefabPart Part) { return PartNames[(int)Part]; }

std::string PrefabPartFileName(PrefabPart Part)
{
    std::string S = PartNames[(int)Part];
    std::replace(S.begin(), S.end(), '-', '_');
    return S;
}

std::vector<PrefabPart> ParsePrefabParts(const std::string& List, std::vector<std::string>& Unknown)
{
    std::set<int> Got;
    std::string Word;
    const auto Take = [&]() {
        if (Word.empty()) return;
        std::string W = LowerOf(Word);
        Word.clear();
        std::replace(W.begin(), W.end(), '_', '-');
        if (W == "none") return;
        if (W == "all")
        {
            for (int K = 0; K < PrefabPartCount; K++) Got.insert(K);
            return;
        }
        for (int K = 0; K < PrefabPartCount; K++)
            if (W == PartNames[K])
            {
                Got.insert(K);
                return;
            }
        Unknown.push_back(W);
    };
    for (const char C : List)
    {
        if (C == ',' || C == ';' || C == ' ' || C == '\t') Take();
        else Word += C;
    }
    Take();
    std::vector<PrefabPart> Out;
    for (const int K : Got) Out.push_back((PrefabPart)K);
    return Out;
}

std::string PrefabPartList(const std::vector<PrefabPart>& Parts)
{
    if ((int)Parts.size() == PrefabPartCount) return "all";
    std::string S;
    for (const PrefabPart P : Parts) S += std::string(S.empty() ? "" : ",") + PrefabPartName(P);
    return S;
}

PrefabPart ClassifyEntity(const std::map<std::string, std::string>& Keys, bool HasBrushes)
{
    const auto Class = Keys.find("classname");
    const std::string C = Class == Keys.end() ? std::string() : LowerOf(Class->second);
    if (C.rfind("light", 0) == 0 || C.rfind("reflection_probe", 0) == 0) return PrefabPart::Lights;
    if (C == "misc_model") return PrefabPart::Models;
    if (HasBrushes) return PrefabPart::BrushEntities;
    if (C.rfind("fx", 0) == 0) return PrefabPart::Effects;
    // only an effect-named key sends an entity to the effects: a graywolf_unresolved_ key alone does not (a hashed
    // script_sound is no effect); graywolf_unresolved_rattleEfx still ends in "efx"
    for (const auto& [K, V] : Keys)
        if (EndsWith(LowerOf(K), "efx") || LowerOf(K) == "fxdef" || EndsWith(LowerOf(V), ".efx")) return PrefabPart::Effects;
    return PrefabPart::Entities;
}

bool IsUnresolvedReference(const std::string& Game, const std::string& Key, const std::string& Value)
{
    const auto Lower = [](std::string S) {
        for (char& C : S) C = (char)std::tolower((unsigned char)C);
        return S;
    };
    const auto Ends = [](const std::string& S, const char* Tail) {
        const size_t N = std::strlen(Tail);
        return S.size() >= N && S.compare(S.size() - N, N, Tail) == 0;
    };
    if (Key == "classname" || Key.rfind("graywolf_", 0) == 0) return false;
    const std::string K = Lower(Key);
    if (Value.size() > 1 && Value[0] == '#' && std::all_of(Value.begin() + 1, Value.end(), [](char C) { return std::isxdigit((unsigned char)C) != 0; }))
    {
        // a hash with no name: renamed only where the key names an asset Radiant would look up. A script's links
        // (targetname, target, script_noteworthy, script_string, script_label...) are read by the game's scripts
        // as they are, hashed or not: renaming them breaks the links.
        const bool ScriptLink = K == "targetname" || K == "target" || K == "script_noteworthy" || K == "script_string" || K == "script_label" ||
                                K == "script_flag" || K == "script_int" || K == "script_float" || K == "script_vector" || K == "script_trigger" ||
                                K == "script_parameters" || K == "script_linkto" || K == "linkto" || K == "script_group" || K == "script_wait";
        if (ScriptLink) return false;
        // the keys the full-map vocabulary treats as assets (models, the lut material, xanims, effects), and the
        // sound and effect keys it does not list
        return !EntityAssetType(Key, "x").empty() || K == "script_sound" || Ends(K, "_sound") || K == "soundalias" || K == "fxdef" || Ends(K, "efx") || Ends(K, "_fx");
    }
    if (Game == "bo4" || Game == "coldwar") return Ends(K, "efx") || Ends(Lower(Value), ".efx");
    return false;
}

// ---- materials

void FacingAreas::Add(const V3& A, const V3& B, const V3& C, const V3* Normal, int Winding)
{
    const V3 Cr = Cross(Sub(B, A), Sub(C, A));
    const double L = Length(Cr);
    if (L < 1e-9) return;
    double Z = Winding * Cr[2] / L;
    if (Normal != nullptr)
    {
        Z = (*Normal)[2];
        WithNormal++;
        const double Dot = (Cr[0] * (*Normal)[0] + Cr[1] * (*Normal)[1] + Cr[2] * (*Normal)[2]) / L;
        (Dot >= 0 ? CrossAgrees : CrossAgainst)++;
    }
    const double Area = L / 2;
    (Z > 0.7 ? Floor : Z < -0.7 ? Ceiling : Wall) += Area;
}

// ---- self test

namespace
{
    std::vector<V3> Box(double X0, double Y0, double Z0, double X1, double Y1, double Z1)
    {
        return { { X0, Y0, Z0 }, { X1, Y0, Z0 }, { X1, Y1, Z0 }, { X0, Y1, Z0 }, { X0, Y0, Z1 }, { X1, Y0, Z1 }, { X1, Y1, Z1 }, { X0, Y1, Z1 } };
    }
}

int RadiantRulesSelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::string("radiant rules: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
    };

    // static models: by index the identity placements of a model share a local box, by position they do not
    {
        std::vector<StaticPlacementBox> Right(4), Wrong(4);
        for (int K = 0; K < 4; K++)
        {
            StaticPlacementBox P;
            P.Origin[0] = 100.0f * K;
            const float Half = K < 2 ? 10.0f : 20.0f;   // two different models: boxes of 10 and 20
            P.Bounds[0] = P.Origin[0] - Half, P.Bounds[1] = -Half, P.Bounds[2] = -Half;
            P.Bounds[3] = P.Origin[0] + Half, P.Bounds[4] = Half, P.Bounds[5] = Half;
            P.Model = K < 2 ? 0 : 1;
            Right[K] = P;
            Wrong[K] = P;
            Wrong[K].Model = K % 2;   // the placement's own number: mixes the two
        }
        const StaticBoundsResult A = StaticModelBounds(Right), B = StaticModelBounds(Wrong);
        Check(A.Hold == 2 && A.Fail == 0, "bounds test holds for the right pairing", std::to_string(A.Hold) + "/" + std::to_string(A.Fail));
        Check(B.Hold == 0 && B.Fail == 2, "bounds test fails for the wrong pairing", std::to_string(B.Hold) + "/" + std::to_string(B.Fail));
        Right[0].Rotation[2] = 0.7f;   // a turned placement is not counted
        Right[0].Rotation[3] = 0.7f;
        Check(StaticModelBounds(Right).Hold == 1, "a turned placement is left out of the bounds test");
    }

    // patch texture: x 1024, V kept; the fallback 16 per unit with V negated; colour only when not white
    Check(PatchTexture(0.5, 0.25) == "t 512 256", "patch texture is UV x 1024, V not mirrored", PatchTexture(0.5, 0.25));
    Check(PatchTexture(-1.5, 2) == "t -1536 2048", "patch texture keeps signs");
    Check(PatchRunningTexture(32, 48) == "t 32 -48", "running patch texture negates V", PatchRunningTexture(32, 48));
    {
        const uint8_t White[4] = { 255, 255, 255, 255 }, Grey[4] = { 255, 255, 255, 254 }, Red[4] = { 255, 0, 0, 255 };
        Check(PatchColour(White).empty(), "a white vertex has no c");
        Check(PatchColour(Grey) == " c 255 255 255 254", "an alpha below 255 is not white", PatchColour(Grey));
        Check(PatchColour(Red) == " c 255 0 0 255", "a coloured vertex has c", PatchColour(Red));
    }

    // hull: a box has six faces of four points; a vertex inside the box adds none
    {
        const std::vector<HullFace> F = HullFaces(Box(0, 0, 0, 64, 32, 16));
        Check(F.size() == 6, "a box hull has six faces", std::to_string(F.size()));
        bool Four = true, Outward = true;
        const std::vector<V3> P = Box(0, 0, 0, 64, 32, 16);
        for (const HullFace& Face : F)
        {
            Four = Four && Face.Index.size() == 4;
            for (const V3& Q : P) Outward = Outward && Dot(Face.Normal, Q) - Face.Dist <= 0.02;
            V3 A = { 0, 0, 0 };
            for (size_t K = 1; K + 1 < Face.Index.size(); K++)
                A = Add(A, Cross(Sub(P[Face.Index[K]], P[Face.Index[0]]), Sub(P[Face.Index[K + 1]], P[Face.Index[0]])));
            Outward = Outward && Dot(A, Face.Normal) > 0;
        }
        Check(Four, "box faces have four points");
        Check(Outward, "hull normals point out and faces wind counter-clockwise seen from outside");
        std::vector<V3> WithInside = P;
        WithInside.push_back({ 10, 10, 5 });
        Check(HullFaces(WithInside).size() == 6, "an interior vertex adds no face");
        Check(HullFaces({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }).empty(), "three points are no hull");
        Check(HullFaces({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 } }).empty(), "four flat points are no hull");
        // a wedge (a box cut by a diagonal): five faces
        const std::vector<V3> Wedge = { { 0, 0, 0 }, { 64, 0, 0 }, { 0, 64, 0 }, { 0, 0, 16 }, { 64, 0, 16 }, { 0, 64, 16 } };
        Check(HullFaces(Wedge).size() == 5, "a wedge has five faces", std::to_string(HullFaces(Wedge).size()));
    }

    // near-coplanar triples are one face: a top ring of five points, one lifted 0.022 off the plane (past the 0.02 on-plane
    // band, so the triples through it have other point sets than the rest), is one top face, not one per triple, and its points are all on it
    {
        const std::vector<V3> Slab = { { 0, 0, 0 }, { 64, 0, 0 }, { 64, 64, 0 }, { 0, 64, 0 },
                                       { 0, 0, 16 }, { 32, 0, 16.022 }, { 64, 0, 16 }, { 64, 64, 16 }, { 0, 64, 16 } };
        const std::vector<HullFace> F = HullFaces(Slab);
        size_t Tops = 0, TopPoints = 0;
        for (const HullFace& Face : F)
            if (Face.Normal[2] > 0.99)
            {
                Tops++;
                TopPoints = Face.Index.size();
            }
        Check(Tops == 1 && F.size() == 6, "near-coplanar triples are one face by plane, their points merged",
              std::to_string(Tops) + " tops, " + std::to_string(TopPoints) + " points, " + std::to_string(F.size()) + " faces");
    }

    // sides: a face takes the side whose plane it lies on; none when none does
    {
        const std::vector<HullFace> F = HullFaces(Box(0, 0, 0, 64, 32, 16));
        std::vector<std::pair<V3, double>> Planes = { { { 0, 0, 1 }, 16 }, { { 1, 0, 0 }, 64 } };
        int Matched = 0, Not = 0;
        for (const HullFace& Face : F) (MatchSide(Face, Planes) >= 0 ? Matched : Not)++;
        Check(Matched == 2 && Not == 4, "a hull face matches a side by plane only", std::to_string(Matched) + "/" + std::to_string(Not));
        HullFace Near = F[0];
        Near.Dist += 0.04;
        Check(MatchSide(Near, { { Near.Normal, F[0].Dist } }) == 0, "a distance 0.04 off still matches");
        Near.Dist += 0.04;
        Check(MatchSide(Near, { { Near.Normal, F[0].Dist } }) == -1, "a distance 0.08 off does not match");
    }

    // limits: a prism of 40 sides is 42 faces (within); three rings of 32 with the middle one pushed out
    // are over 64 faces and split
    {
        std::vector<V3> Prism;
        for (int Z = 0; Z < 2; Z++)
            for (int K = 0; K < 40; K++)
            {
                const double A = 2 * 3.14159265358979323846 * K / 40;
                Prism.push_back({ 256 * std::cos(A), 256 * std::sin(A), Z * 64.0 });
            }
        Check(WithinRadiantLimits(HullFaces(Prism)), "a prism of 40 sides (42 faces) is within the limits");
        Check(SplitForRadiant(Prism).Pieces.size() == 1, "a brush within the limits is one piece");
        std::vector<V3> Bulge;
        for (int Z = 0; Z < 3; Z++)
            for (int K = 0; K < 32; K++)
            {
                const double A = 2 * 3.14159265358979323846 * K / 32, R = Z == 1 ? 300.0 : 256.0;
                Bulge.push_back({ R * std::cos(A), R * std::sin(A), Z * 64.0 });
            }
        const std::vector<HullFace> F = HullFaces(Bulge);
        Check(F.size() > MaxBrushFaces, "a bulged ring of 32 is over 64 faces", std::to_string(F.size()));
        Check(!WithinRadiantLimits(F), "a hull over 64 faces is over the limit");
        const Split S = SplitForRadiant(Bulge);
        bool All = !S.Pieces.empty();
        for (const auto& Piece : S.Pieces) All = All && WithinRadiantLimits(HullFaces(Piece));
        Check(All && S.Failure.empty(), "every piece of a split is within the limits", S.Failure + " " + std::to_string(S.Pieces.size()));
        Check(S.Pieces.size() > 1 && S.Pieces.size() <= 64, "a brush over the limit is split into at most 64 pieces", std::to_string(S.Pieces.size()));
        // the node cap: the same brush, given one node, fails with its reason and counts what it tried; the hull passed in is the one built
        {
            const Split Capped = SplitForRadiant(Bulge, &F, 1);
            Check(Capped.Pieces.empty() && Capped.Failure.find("split nodes") != std::string::npos && Capped.Nodes == 2, "a split past its node cap fails with a reason",
                  Capped.Failure + " nodes " + std::to_string(Capped.Nodes));
            const Split Given = SplitForRadiant(Bulge, &F);
            Check(Given.Pieces.size() == S.Pieces.size() && Given.Nodes <= MaxSplitNodes, "the hull passed in gives the split the hull it would build", std::to_string(Given.Pieces.size()));
        }
        // a hull of fewer than four faces cannot be split: reported, not written
        Check(!SplitForRadiant({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }).Failure.empty(), "a non-solid is a failure with a reason");
    }

    // tiny and skinny faces
    Check(IsTinyFace({ 0, 0, 0 }, { 3, 0, 0 }, { 0, 3, 0 }), "a 3 x 3 triangle is tiny (cross 9)");
    Check(!IsTinyFace({ 0, 0, 0 }, { 64, 0, 0 }, { 0, 64, 0 }), "a 64 x 64 triangle is not tiny");
    Check(IsTinyFace({ 0, 0, 0 }, { 1000, 0, 0 }, { 500, 5, 0 }), "a triangle 1000 long and 5 high is skinny (sine 0.01)");
    Check(!IsTinyFace({ 0, 0, 0 }, { 100, 0, 0 }, { 0, 20, 0 }), "a 100 x 20 triangle is neither");
    Check(IsTinyFace({ 0, 0, 0 }, { 0, 0, 0 }, { 5, 5, 5 }), "a repeated corner is tiny");

    // contents line
    Check(ContentsKeywords(0x2080) == "weaponClip detail", "0x2080 is weaponClip detail", ContentsKeywords(0x2080));
    Check(ContentsKeywords(0x82080) == "weaponClip detail umbraTarget", "0x82080 adds umbraTarget", ContentsKeywords(0x82080));
    Check(ContentsKeywords(0x80000) == "umbraTarget", "0x80000 is umbraTarget alone");
    Check(ContentsKeywords(0x1 | 0x10000) == "", "other bits give no keyword");

    // light entity: omni with forward +x, up +z
    {
        Light L;
        L.Compiled = true;
        L.Kind = Light::Type::Point;
        L.Origin[0] = 1, L.Origin[1] = 2.5f, L.Origin[2] = -3;
        L.Dir[0] = 1, L.Dir[1] = 0, L.Dir[2] = 0;
        L.Right[0] = 0, L.Right[1] = -1, L.Right[2] = 0;
        L.Up[0] = 0, L.Up[1] = 0, L.Up[2] = 1;
        L.Guid = 0xA1B2C3D;
        L.Color[0] = 1, L.Color[1] = 0.5f, L.Color[2] = 0.25f;
        L.Unresolved204 = 7, L.Unresolved448 = 0.5f;
        L.Def = "light_def";
        const std::string S = LightEntityKeys(L, "graywolf_");
        Check(S.find("\"classname\" \"light\"\n\"origin\" \"1 2.5 -3\"\n\"angles\" \"0 0 0\"\n\"PRIMARY_TYPE\" \"PRIMARY_OMNI\"\n\"name\" \"graywolf_light_0a1b2c3d\"\n") == 0,
              "a light entity has classname, origin, angles, PRIMARY_TYPE, name", S);
        Check(S.find("\"graywolf_unresolved_rgb_at_168\" \"1 0.5 0.25\"") != std::string::npos, "colour is under an unresolved key", S);
        Check(S.find("_color") == std::string::npos && S.find("stops") == std::string::npos, "no _color and no stops");
        Check(S.find("\"graywolf_unresolved_def\" \"light_def\"") != std::string::npos, "the definition name is kept unresolved");
        L.Kind = Light::Type::Sun;
        Check(LightEntityKeys(L, "graywolf_").empty(), "a sun is not written");
        L.Kind = Light::Type::Spot;
        L.Dir[0] = 0, L.Dir[1] = 0, L.Dir[2] = -1;
        L.Right[0] = 0, L.Right[1] = 1, L.Right[2] = 0;
        L.Up[0] = 1, L.Up[1] = 0, L.Up[2] = 0;
        Check(LightEntityKeys(L, "graywolf_").find("\"angles\" \"90 0 0\"") != std::string::npos, "a spot straight down has pitch 90", LightEntityKeys(L, "graywolf_"));
        L.Compiled = false;
        Check(LightEntityKeys(L, "graywolf_").empty(), "a light not read whole is not written");
        // a game that keeps only the colour among the unresolved fields (Modern Warfare Remastered, colour already divided by its unit): its own key, no BO4 floats
        Light W;
        W.Compiled = true;
        W.Kind = Light::Type::Spot;
        W.Dir[0] = 1, W.Dir[1] = 0, W.Dir[2] = 0;
        W.Up[0] = 0, W.Up[1] = 0, W.Up[2] = 1;
        W.Right[0] = 0, W.Right[1] = -1, W.Right[2] = 0;
        W.Color[0] = 2, W.Color[1] = 1, W.Color[2] = 0.5f;
        W.ColorKey = "color_div_15500_03";
        W.OnlyColor = true;
        W.Def = "light_cuc_probe";
        const std::string M = LightEntityKeys(W, "graywolf_");
        Check(M.find("\"graywolf_unresolved_color_div_15500_03\" \"2 1 0.5\"") != std::string::npos && M.find("float_at_204") == std::string::npos && M.find("rgb_at_168") == std::string::npos &&
                  M.find("\"graywolf_unresolved_def\" \"light_cuc_probe\"") != std::string::npos,
              "a compiled light with its own colour key writes that key and no Black Ops 4 floats", M);
    }

    // a Black Ops III light: the keys fitted against the stock lights, colour and stops left as the radiance
    {
        Light L;
        L.Bo3 = true;
        L.Kind = Light::Type::Spot;
        L.Guid = 0x0351dcd6;
        L.Origin[0] = 552.98f, L.Origin[1] = -501.692f, L.Origin[2] = 138.25f;
        L.Forward[0] = -0.579296f, L.Forward[1] = -0.815117f, L.Forward[2] = 0;
        L.Color[0] = 47975.0f, L.Color[1] = 32641.5f, L.Color[2] = 13787.5f;
        L.Radius = 62, L.CosHalfFovOuter = 0.9063078f;   // 25 degrees: fov_outer 50
        L.NearEdge = 0.2f, L.FarEdge = 0.75f, L.FalloffDistance = 1.5f, L.SpecScale = 0.25f, L.CutOn = 19, L.Penumbra = 3, L.BulbRadius = 0.01f;
        L.SuperEllipse[0] = 0.05f, L.SuperEllipse[1] = 1, L.SuperEllipse[2] = 0.05f, L.SuperEllipse[3] = 1;
        const std::string S = LightEntityKeys(L, "graywolf_");
        Check(S.find("\"classname\" \"light\"\n\"origin\" \"552.98 -501.692 138.25\"\n\"angles\" \"0 234.59") == 0 &&
                  S.find(" 0\"\n\"PRIMARY_TYPE\" \"PRIMARY_SPOT\"\n\"name\" \"graywolf_light_0351dcd6\"\n") != std::string::npos,
              "a BO3 light has classname, origin, angles from its forward, type, name", S);
        Check(S.find("\"radius\" \"62\"\n\"fov_outer\" \"50\"\n\"near_edge\" \"0.2\"\n\"far_edge\" \"0.75\"\n\"falloffdistance\" \"1.5\"\n\"spec_comp\" \"-2\"\n\"cut_on\" \"19\"\n") != std::string::npos,
              "radius, fov_outer, near_edge, far_edge, falloffdistance, spec_comp and cut_on are the fitted keys", S);
        Check(S.find("\"superellipse\" \"0.05 1 0.05 1\"") != std::string::npos && S.find("\"penumbraRadius\" \"3\"") != std::string::npos && S.find("\"bulbRadius\" \"0.01\"") != std::string::npos,
              "superellipse, penumbraRadius and bulbRadius are written", S);
        Check(S.find("\"graywolf_unresolved_radiance\" \"47975 32641.5 13787.5\"") != std::string::npos && S.find("_color") == std::string::npos && S.find("stops") == std::string::npos,
              "the radiance is unresolved, no _color and no stops", S);
        L.FalloffDistance = 124;   // twice the radius: the light's falloff is off
        const std::string Off = LightEntityKeys(L, "graywolf_");
        Check(Off.find("\"ENABLE_FALLOFF\" \"0\"") != std::string::npos && Off.find("falloffdistance") == std::string::npos, "twice the radius is ENABLE_FALLOFF 0 and no distance", Off);
        L.Kind = Light::Type::Point;
        Check(LightEntityKeys(L, "graywolf_").find("superellipse") == std::string::npos && LightEntityKeys(L, "graywolf_").find("PRIMARY_OMNI") != std::string::npos, "an omni light has no superellipse");
        L.Forward[0] = 0, L.Forward[1] = 0, L.Forward[2] = 1;
        Check(LightEntityKeys(L, "graywolf_").find("\"angles\" \"270 0 0\"") != std::string::npos, "a light pointing up has pitch 270 (-90) and yaw 0");
        L.Kind = Light::Type::Sun;
        Check(LightEntityKeys(L, "graywolf_").empty(), "a BO3 sun is not written");
    }

    // prefab parts: the names, the list, the classes
    {
        std::vector<std::string> Unknown;
        const std::vector<PrefabPart> A = ParsePrefabParts("Brush_Entities; lights decals,bogus", Unknown);
        Check(A.size() == 3 && A[0] == PrefabPart::Decals && A[1] == PrefabPart::BrushEntities && A[2] == PrefabPart::Lights && Unknown.size() == 1 && Unknown[0] == "bogus",
              "the part list takes _ for -, any separator, case aside, and names the unknown", std::to_string(A.size()));
        Unknown.clear();
        Check(ParsePrefabParts("all", Unknown).size() == (size_t)PrefabPartCount && PrefabPartList(ParsePrefabParts("all", Unknown)) == "all", "all is every part");
        Check(ParsePrefabParts("", Unknown).empty() && ParsePrefabParts("none", Unknown).empty() && Unknown.empty(), "empty and none are no part");
        Check(PrefabPartList(ParsePrefabParts("lights,brushes", Unknown)) == "brushes,lights", "the list is kept in part order");
        Check(PrefabPartFileName(PrefabPart::BrushEntities) == "brush_entities" && std::string(PrefabPartName(PrefabPart::BrushEntities)) == "brush-entities", "file name and part name");
        const auto Class = [](const char* C, std::initializer_list<std::pair<const char*, const char*>> More, bool Brushes) {
            std::map<std::string, std::string> E;
            E["classname"] = C;
            for (const auto& [K, V] : More) E[K] = V;
            return ClassifyEntity(E, Brushes);
        };
        Check(Class("light_spot", {}, false) == PrefabPart::Lights && Class("light", {}, true) == PrefabPart::Lights && Class("reflection_probe", {}, false) == PrefabPart::Lights, "light* and reflection_probe are lights, brushes or not");
        Check(Class("misc_model", {}, false) == PrefabPart::Models && Class("misc_model", { { "x.efx", "y" } }, false) == PrefabPart::Models, "misc_model is a model before the effect test");
        Check(Class("trigger_use", {}, true) == PrefabPart::BrushEntities && Class("fx_x", {}, true) == PrefabPart::BrushEntities, "an entity with brushes is a brush entity before the effect test");
        Check(Class("fx_ambient", {}, false) == PrefabPart::Effects && Class("script_struct", { { "fxdef", "a/b.efx" } }, false) == PrefabPart::Effects &&
                  Class("script_struct", { { "rattleEfx", "x" } }, false) == PrefabPart::Effects && Class("script_struct", { { "graywolf_unresolved_rattleEfx", "1" } }, false) == PrefabPart::Effects,
              "an fx class, a .efx value, an efx key and an unresolved efx key are effects");
        Check(Class("script_struct", { { "graywolf_unresolved_script_sound", "#1a" } }, false) == PrefabPart::Entities && Class("script_struct", { { "graywolf_unresolved_x", "1" } }, false) == PrefabPart::Entities,
              "an unresolved key alone is no effect");
        Check(Class("script_struct", { { "targetname", "a" } }, false) == PrefabPart::Entities && Class("info_volume", {}, false) == PrefabPart::Entities, "everything else is an entity");
    }

    // static model angles
    {
        const float Level[9] = { 0, 1, 0, -1, 0, 0, 0, 0, 1 };   // yaw 90
        Check(StaticModelAngles(Level) == "0 90 0", "level yaw 90", StaticModelAngles(Level));
        // forward straight up with noise below 1e-4: pitch -90, roll 0, yaw from the left vector
        const float Up[9] = { 1e-5f, 1e-5f, 1, 0, 1, 0, -1, 0, 0 };   // forward +z, left +y
        Check(StaticModelAngles(Up) == "-90 -0 0" || StaticModelAngles(Up) == "-90 0 0", "pitch -90 gives roll 0 and yaw from left", StaticModelAngles(Up));
        const float Down[9] = { 0, 0, -1, -1, 0, 0, 0, 1, 0 };   // forward -z, left -x: yaw = atan2(1, 0) = 90
        Check(StaticModelAngles(Down) == "90 90 0", "pitch +90 with left -x gives yaw 90", StaticModelAngles(Down));
        const float Noise[9] = { 1e-5f, 0, -1, 0, 1, 0, 1, 0, 0 };   // forward almost -z, left +y: yaw 0
        Check(StaticModelAngles(Noise) == "90 0 0", "horizontal part 1e-5 is polar: roll 0", StaticModelAngles(Noise));
    }

    // unresolved keys
    Check(UnresolvedKey("rattleEfx") == "graywolf_unresolved_rattleEfx", "unresolved key name");
    Check(IsUnresolvedReference("bo4", "rattleEfx", "debris/fx8_paper_rattle.efx"), "a BO4 rattleEfx is unresolved");
    Check(IsUnresolvedReference("coldwar", "script_fx", "x/y.efx"), "a Cold War .efx value is unresolved");
    Check(!IsUnresolvedReference("bo3", "rattleEfx", "debris/fx8_paper_rattle.efx"), "a BO3 effect key is kept: its game has the effects");
    Check(IsUnresolvedReference("bo3", "model", "#cd36b9dc6e798fd"), "an unresolved hash value is unresolved in any game");
    Check(!IsUnresolvedReference("bo4", "model", "p7_chair") && !IsUnresolvedReference("bo4", "targetname", "door_1") && !IsUnresolvedReference("bo4", "classname", "#abc"),
          "named values and classname are kept");
    Check(!IsUnresolvedReference("bo4", "targetname", "#1a2b3c") && !IsUnresolvedReference("bo4", "target", "#1a2b3c") &&
              !IsUnresolvedReference("coldwar", "script_noteworthy", "#1a2b3c") && !IsUnresolvedReference("bo4", "script_string", "#1a2b3c"),
          "a hashed targetname, target, script_noteworthy or script_string is kept: script links break otherwise");
    Check(IsUnresolvedReference("bo4", "script_sound", "#1a2b3c") && IsUnresolvedReference("bo4", "zbarrierboardModel1", "#1a2b3c") && IsUnresolvedReference("bo4", "lutmaterial", "#1a2b3c"),
          "a hashed asset key (sound, model, material) is renamed");

    // usage
    {
        FacingAreas Floor, Ceiling, Wall;
        Floor.Add({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 });         // normal +z
        Ceiling.Add({ 0, 0, 0 }, { 0, 10, 0 }, { 10, 0, 0 });       // normal -z
        Wall.Add({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 0, 10 });          // normal -y
        // a triangle is placed by its vertex normal when given, and by the winding times the cross product otherwise
        {
            FacingAreas Up, Down, ByNormal;
            const V3 N = { 0, 0, 1 }, Dn = { 0, 0, -1 };
            Up.Add({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 }, nullptr, 1);       // cross +z, winding +1: up
            Down.Add({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 }, nullptr, -1);    // the same triangle where the front is clockwise: down
            Check(Up.Floor > 0 && Down.Ceiling > 0 && Down.Floor == 0, "the winding sign turns the cross product");
            ByNormal.Add({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 }, &Dn, 1);     // the vertex normal says down whatever the winding
            ByNormal.Add({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 }, &N, 1);
            Check(ByNormal.Ceiling > 0 && ByNormal.Floor > 0 && ByNormal.WithNormal == 2 && ByNormal.CrossAgrees == 1 && ByNormal.CrossAgainst == 1, "the vertex normal decides, and the cross product is compared with it");
        }
        // a real Black Ops 4 triangle: mp_frenetic, t8_concrete_floor_rough_02, vertices (986,1696,182.5) (806,1764,182.5) (986,1764,182.5), every vertex
        // normal (0,0,1). cross(b - a, c - a) points down (z = -12240): the winding is opposite to the normal, as on 98,064 of 98,133 triangles measured
        {
            const V3 A = { 986, 1696, 182.5 }, B = { 806, 1764, 182.5 }, C = { 986, 1764, 182.5 }, Up = { 0, 0, 1 };
            FacingAreas ByNormal, ByWinding, Unsigned;
            ByNormal.Add(A, B, C, &Up, 1);
            ByWinding.Add(A, B, C, nullptr, -1);
            Unsigned.Add(A, B, C, nullptr, 1);   // the old behaviour: the floor comes out a ceiling
            Check(ByNormal.Floor > 0 && ByNormal.Ceiling == 0 && ByNormal.CrossAgainst == 1 && ByNormal.CrossAgrees == 0, "a real BO4 floor triangle: the vertex normal says floor, the cross product is against it");
            Check(ByWinding.Floor > 0 && ByWinding.Ceiling == 0, "a real BO4 floor triangle: winding -1 gives floor with no normal");
            Check(Unsigned.Ceiling > 0 && Unsigned.Floor == 0, "a real BO4 floor triangle: the unsigned cross product would call it a ceiling (the defect)");
        }
        // the areas only; the usage is chosen from them and the BO3 techset (Techsets::UsageRules, tested in Bo3Target.Techsets.cpp)
        Check(Floor.Floor > 0 && Floor.Ceiling == 0 && Floor.Wall == 0, "up facing is floor area");
        Check(Ceiling.Ceiling > 0 && Ceiling.Floor == 0 && Ceiling.Wall == 0, "down facing is ceiling area");
        Check(Wall.Wall > 0 && Wall.Floor == 0 && Wall.Ceiling == 0, "side facing is wall area");
        FacingAreas Mixed = Wall;
        Mixed.Add({ 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 });
        Check(Mixed.Wall > Mixed.Floor && Mixed.Floor > 0, "a mixed surface keeps both areas");
    }
    Check(RadiantNumber(1.00004) == "1" && RadiantNumber(-0.00001) == "0" && RadiantNumber(2.5) == "2.5", "numbers are four decimals trimmed");
    return Failed;
}
}
