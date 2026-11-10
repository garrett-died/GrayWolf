#pragma once

// The sun, the sun volume and the reflection probes of a Radiant map (M4b, M4c; docs/hydrax-port/m4.md). Pure text builders: the Radiant writer decides what to write and
// where, the game's reader fills MapScene (Sun, Probes, Effects). Formats are the stock BO3 forms read from the Mod Tools install (zm_gridlock.map: reflection_probe;
// the ZM Advanced Level template: volume_sun with its info_null target; source_data/ssi.gdt) and HydraX's MapRenderers.cs (ssi entry keys). Nothing here was loaded in Radiant:
// every key is labelled in the m4 doc.

#include "GdtWriter.h"
#include "MapScene.h"

#include <array>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace Maps
{
    // BO3's ssi pitch and yaw (degrees) of a sun that lies in the direction Toward (a unit vector from the world to the sun, game space). The Mod Tools' own convention is the one
    // LightingPreset::SunDirection (ModToolsSky.h) states: toward = (-cos p cos y, -cos p sin y, sin p), with the stock pitches between 90 and 180. The elevation is then 180 - pitch;
    // a sun below the horizon gives a pitch over 180, written wrapped into -180..180 (the same direction). Returns false for a vector that is not a direction.
    bool SunAngles(std::span<const float, 3> Toward, double& Pitch, double& Yaw);

    // A value that may go into an entity key or a GDT field as it is: letters, digits and . _ / - only. Anything else (a quote, a backslash, a newline, a space) is refused,
    // never rewritten.
    bool SafeText(const std::string& S);

    struct SsiEntry
    {
        std::string Name, SkyboxModel;   // SkyboxModel empty: no skyboxmodel field is written
        double Pitch = 0, Yaw = 0;
        bool HasColor = false;
        float ColorSRGB[3] = { 1, 1, 1 };
        Gdt::Fields Stock;               // brightness and exposure fields taken from the stock median (LoadSsiStock), written as they are
    };
    // One ssi.gdf entry: the stock fields, skyboxmodel, pitch, yaw, colorSRGB (when given), enablesun 1 and type ssi. Empty when the name or the sky name is not SafeText.
    std::string SsiGdtEntry(const SsiEntry& E);

    // The stock BO3 ssi.gdt's median of stops, evmin, evmax, evcmp, bounceCount, dynamicShadow and penumbra_inches, with the names of the stock entries, read from
    // zonekit/profiles/t7/target/ssi-stock-median.json (target data, not code). False (Why filled) when the file is not found or holds no value.
    struct SsiStock
    {
        Gdt::Fields Values;
        std::vector<std::string> StockNames;
        size_t Entries = 0;
        std::string Source;
    };
    bool LoadSsiStock(SsiStock& Out, std::string& Why);
    // The same from the text of that file (for the tests)
    bool ParseSsiStock(const std::string& Json, SsiStock& Out, std::string& Why);

    // The bounds of the map's collision (the triangle soup's vertices and the world brushes' corners, all-sky brushes left out when the scene seals with the shell). The drawn world is not used: a sky mesh is far larger than the map.
    // False when the scene has no collision vertex.
    // Skip: world brushes left out (the writer's all-sky brushes of an ssi skybox game); Why, when given, gets what set each extreme as WrittenBounds does.
    bool CollisionBounds(const MapScene& Scene, std::array<float, 3>& Lo, std::array<float, 3>& Hi, const std::set<const Brush*>* Skip = nullptr, std::array<std::string, 6>* Why = nullptr);
    // The bounds of everything the map writes: the collision above, the drawn world's vertices (a sky material's surfaces, drawn far out, left out) and the static instances' origins
    // Why, when given, gets what set each extreme: x min, x max, y min, y max, z min, z max, each as "collision" or "render vertex of <mesh> (<material>) at (x, y, z)" or "static instance <name> of
    // <model> at (x, y, z)". SkipOriginBeyond > 0 leaves out the static instances with an origin coordinate of that size or more (M4f: the instances outside the shell's reach).
    bool WrittenBounds(const MapScene& Scene, std::array<float, 3>& Lo, std::array<float, 3>& Hi, std::array<std::string, 6>* Why = nullptr, const char* SkipModelContaining = nullptr, float SkipOriginBeyond = 0.0f,
                       const std::set<const Brush*>* SkipBrushes = nullptr, const std::set<std::string>* SkipMaterials = nullptr);

    // The box of a sun volume: the bounds grown by Pad, every side clamped to +-Range.
    void SunBox(const std::array<float, 3>& Lo, const std::array<float, 3>& Hi, float Pad, float Range, std::array<double, 3>& A, std::array<double, 3>& B);

    // The two entities of a sun volume, numbered Entity and Entity + 1: an info_null at the box centre (targetname Target) and the volume_sun with "target" Target, six
    // sun_volume faces over A..B and ssi1..ssi4 = Ssi, as the stock template has them.
    std::string SunVolumeEntities(size_t Entity, const std::array<double, 3>& A, const std::array<double, 3>& B, const std::string& Target, const std::string& Ssi);

    // The reflection_probe entity of a probe: name probeN, origin, and size_min / size_max only when the probe's size was read
    std::string ReflectionProbeEntity(size_t Entity, size_t Index, const Probe& P);

    // A script-placed effect or sound as a script_struct: targetname graywolf_createfx (the writer sends it to the effects prefab), script_noteworthy the kind, script_string the name
    // of the effect or sound alias as the game's script names it, origin and angles. BO3 has no entity that plays a stock effect by itself: a GSC loop over the targetname spawns them.
    // Empty when the kind or the name is not SafeText (the writer counts it refused).
    std::string EffectPlacementEntity(size_t Entity, const EffectPlacement& E);

    // A closed shell of six slabs in the stock `sky` material around the box A..B, Thickness thick, as a func_group of six brushes (the Mod Tools' own maps seal with
    // sky brushes: zm_giant's geo prefab holds five brush shells of `sky`). Entity is the entity number. The slabs overlap nowhere and leave no gap: the x slabs span the
    // outer y and z, the y slabs the inner x and outer z, the z slabs the inner x and y.
    struct SkySlab
    {
        std::array<double, 3> Lo, Hi;
    };
    std::array<SkySlab, 6> SkyShell(const std::array<double, 3>& A, const std::array<double, 3>& B, double Thickness);
    std::string SkyShellEntity(size_t Entity, const std::array<double, 3>& A, const std::array<double, 3>& B, double Thickness);

    int SunProbesSelfTest(std::string& Failures);
}
