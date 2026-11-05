#pragma once

// The rules of the Radiant source export that stand alone (no scene, no file), ported from HydraX's
// map export (docs/hydrax-port/phase1-spec.md): brush hulls from the clip map's vertices and
// Radiant's limits, the three points of a tiny face, the contents line, a light entity, a static
// model's angles, a patch vertex's texture, a material's usage, the unresolved-key name and the
// bounds test that tells a placement's model. Each is tested by SelfTest (RunSelfTest).

#include "MapRadiantGeometry.h"
#include "MapScene.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Maps
{
    using RadiantGeometry::V3;

    // A number as the .map writes it: four decimals, trailing zeros dropped, no "-0"
    std::string RadiantNumber(double V);

    // ---- BO4 static models

    // A placement as the bounds test sees it: rotation quaternion (x, y, z, w), origin, scale, world
    // bounds (mins, maxs) and the model the pairing gives it.
    struct StaticPlacementBox
    {
        float Rotation[4] = { 0, 0, 0, 1 };
        float Origin[3] = { 0, 0, 0 };
        float Scale = 1.0f;
        float Bounds[6] = { 0, 0, 0, 0, 0, 0 };
        uint32_t Model = 0;
    };
    struct StaticBoundsResult
    {
        size_t Hold = 0, Fail = 0;   // models with several upright scale 1 placements: one local box, or not
    };
    // HydraX's check of which model a placement names: among the models with several identity
    // rotation, scale 1 placements, the stored world bounds minus the origin must be one local box.
    StaticBoundsResult StaticModelBounds(const std::vector<StaticPlacementBox>& Placements);

    // ---- patches

    // The "t" of a patch vertex that has render data: the UV x 1024 (a compiler constant, not the
    // image size), V not mirrored; "t U V" without the lightmap pair.
    std::string PatchTexture(double U, double V);
    // Without render data: the running coordinate, 16 per unit along the grid, V negated.
    std::string PatchRunningTexture(double RunU, double RunV);
    // A patch vertex line ("\tv X Y Z[ c R G B A] ..."), the colour only when it is not white
    std::string PatchColour(const uint8_t (&Colour)[4]);

    // ---- brush hulls

    constexpr size_t MaxHullVertices = 96;   // more: no hull is built
    constexpr size_t MaxBrushFaces = 64, MaxFacePoints = 64;   // Radiant's limits (brush.cpp)

    struct HullFace
    {
        V3 Normal = { 0, 0, 1 };
        double Dist = 0;                // Normal . p == Dist on the face, the solid behind
        std::vector<size_t> Index;      // its vertices, counter-clockwise seen from outside
    };
    // The faces of the convex hull of Points (any three whose plane has none above or none below
    // them, vertices within 0.02 on it): empty for fewer than four points, more than MaxHullVertices
    // or fewer than four faces.
    std::vector<HullFace> HullFaces(const std::vector<V3>& Points);
    // Whether a hull is within Radiant's limits
    bool WithinRadiantLimits(const std::vector<HullFace>& Faces);
    // The brush as pieces within the limits: split by the most balanced plane through three of its own
    // vertices with none within 0.02 of it, depth 12, at most 64 pieces. Empty Pieces with Failure set
    // when it cannot be. One piece (Points) when it is within the limits already.
    constexpr size_t MaxSplitNodes = 64;
    // At most MaxNodes (64) nodes (calls that build a hull) are tried: Nodes says how many were. Hull: the faces of Points when
    // the caller has built them (HullFaces(Points)), so the first level does not build them again.
    struct Split
    {
        std::vector<std::vector<V3>> Pieces;
        std::string Failure;
        size_t Nodes = 0;
    };
    Split SplitForRadiant(const std::vector<V3>& Points, const std::vector<HullFace>* Hull = nullptr, size_t MaxNodes = MaxSplitNodes);
    // The side (index into Planes: unit normal, distance) a hull face lies on: normals within 2e-3,
    // distances within 0.05; -1 for none.
    int MatchSide(const HullFace& Face, const std::vector<std::pair<V3, double>>& Planes);

    // A face of three corners is tiny when they span a cross product under 16, skinny when the sine of
    // the angle at the first is under 0.1: it is then written with three points of its plane 64 units
    // apart around its centre.
    constexpr double TinyFaceSpan = 16.0, SkinnyFaceSine = 0.1, TinyFacePointSpacing = 64.0;
    bool IsTinyFace(const V3& A, const V3& B, const V3& C);

    // ---- brush contents

    // "weaponClip detail umbraTarget" for the bits 0x80, 0x2000, 0x80000 (in that order), "" for none
    std::string ContentsKeywords(uint32_t Contents);

    // ---- entities

    // A light entity's keys (one "key" "value" per line) for a compiled primary light; "" for one
    // Radiant has no entity for (a sun) or one that was not read whole. Colour and the other
    // unconverted fields are under <Prefix>unresolved_ keys; Prefix "graywolf_". Black Ops III's
    // (Light::Bo3) also has the keys fitted against the stock zm_giant lights (radius, fov_outer, near_edge,
    // far_edge, falloffdistance and ENABLE_FALLOFF, spec_comp, cut_on, penumbraRadius, bulbRadius, a spot's
    // superellipse, angles with roll 0); _color and stops stay one <Prefix>unresolved_radiance (the linear
    // product: the compiled light does not keep the two apart). docs/LIGHT-CONVERSION.md.
    std::string LightEntityKeys(const Light& L, const std::string& Prefix);
    // A static model's "angles" (pitch yaw roll) from the rows of its axis: at pitch +-90 (the
    // forward vector's horizontal part under 1e-4) roll is 0 and yaw comes from the left vector.
    std::string StaticModelAngles(const float (&Axis)[9]);
    // A key Radiant cannot take (it names an effect or asset the Mod Tools lack): the key it is kept
    // under so the data stays and Radiant ignores it
    std::string UnresolvedKey(const std::string& Key);
    // Whether an entity's key and value name something the Mod Tools lack and Radiant must not read:
    // for Black Ops 4 and Cold War an effect (a key ending "efx", or a value ending ".efx": their effects
    // are not BO3's; an unknown effect crashes Radiant's guard, eclass.cpp:425, on rattleEfx), and for any
    // game a value that is an unresolved hash ("#" and hex digits: no name for it) under a key that names an
    // asset (model keys, the lut material, xanims, effects, sounds). Never classname, and never a script's
    // links (targetname, target, script_noteworthy, script_string...), hashed or not.
    bool IsUnresolvedReference(const std::string& Game, const std::string& Key, const std::string& Value);

    // ---- prefab separation (--map-prefabs)

    // The parts of a map that go to a standalone map each, as HydraX's MapPrefabParts (full-export-spec.md 1)
    enum class PrefabPart : int
    {
        Brushes, Patches, Decals, Models, Effects, Entities, BrushEntities, Lights
    };
    constexpr int PrefabPartCount = 8;
    // "brushes", "brush-entities", ...
    const char* PrefabPartName(PrefabPart Part);
    // The file name part: "brush_entities"
    std::string PrefabPartFileName(PrefabPart Part);
    // A comma, semicolon or space separated list of the names (case-insensitive, "_" for "-", or "all"):
    // the parts in enum order; Unknown gets each word that is none. "" and "none" give no part.
    std::vector<PrefabPart> ParsePrefabParts(const std::string& List, std::vector<std::string>& Unknown);
    // The list as the setting keeps it: the names, comma separated, in enum order ("all" for every part)
    std::string PrefabPartList(const std::vector<PrefabPart>& Parts);
    // Where an entity goes (first match wins): a class starting "light" or "reflection_probe" is Lights;
    // "misc_model" Models; one with brushes or meshes BrushEntities; one with a key naming an effect (a key
    // a key ending "efx", fxdef, a value ending ".efx"; a graywolf_unresolved_ key alone is none) or a class starting "fx" Effects;
    // everything else Entities.
    PrefabPart ClassifyEntity(const std::map<std::string, std::string>& Keys, bool HasBrushes);
    // The "model" a misc_prefab names for a prefab file (relative to map_source: PrefabFolder + file name)
    constexpr const char* PrefabFolder = "graywolf_maps/";

    // ---- materials

    // The area of a material's render triangles by the way they face
    struct FacingAreas
    {
        double Floor = 0, Ceiling = 0, Wall = 0;
        void Add(const V3& A, const V3& B, const V3& C);
    };
    // The usage Radiant lists a map's material under: a decal category "decal", a glass techset
    // "window", else the facing with the most area: normal z above 0.7 "floor", below -0.7 "ceiling",
    // "interior wall" otherwise
    std::string MaterialUsage(bool Decal, const std::string& Techset, const FacingAreas& Areas);
    constexpr const char* NotInEditor = "<not in editor>";

    // The rules above on synthetic input; the number failed (each failure appended to Failures)
    int RadiantRulesSelfTest(std::string& Failures);
}
