#pragma once

// Module 2b of the Black Ops III conversion layer: choose a real BO3 techset for a material and know
// what that techset holds (docs/hydrax-port/bo3-conversion-design.md module 2, bo3-target-hydrax.md 5.2).
//
// A port of HydraX's TreyarchTechsetMapper over the trimmed catalog zonekit/profiles/t7/target/techsets.json
// (tools/bo3target/gen_techsets.py). Rules, first match wins:
//   Override, Exact name, Normalized, Sky, Family, Exact slot set, Slot superset, Render class overlap, Default.
// The first five need a usable techset name from the game. The last four work from the set of image slots
// (GDT keys) a material description fills, so every game gets them. Only Exact name, Family and Exact slot
// set are exact; anything else is a fallback and the caller reports it (material.techset_fallback).
//
// Never chosen: special-purpose techsets (names holding base, stub, shaders, script, camo), so never
// lit_weapon_camo (the Mod Tools cannot register it), whatever the game names. Character customisation
// and camo techsets (traits) are not candidates either.
//
// Pure functions over loaded data: no I/O except Data::Load, which reads three optional JSON files and
// never throws. A missing file leaves its part empty; the caller then keeps today's rules and says so.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class WraithNameIndex;

namespace Bo3Target::Techsets
{
    // The hash BO3 stores as an image slot's semantic: each byte from 'A' to '_' moves up by 0x20 (so '_'
    // becomes 0x7F), then h = h * 33 ^ c from a zero seed. colorMap is 0xA0AB1041.
    // Evidence for the '_' case: "Normal_Map" and "Diffuse_Map" hash to 0x942CBFF0 and 0xF039EC2D this way, which
    // are the BO1 (Games/T5/GameBlackOps.cpp) and BO2 (Games/T6/GameBlackOps2.cpp) image semantic hashes of the
    // normal and diffuse maps; plain lower-casing gives other values. See SelfTest.
    uint32_t SlotHash(std::string_view Name);

    // The render class of a techset (HydraX TechsetBlend)
    enum class Blend : uint8_t { Other, Opaque, AlphaTest, Blend, Premul, Add, Multiply, Decal };
    const char* BlendName(Blend B);
    Blend BlendFromName(std::string_view Name);

    // TechsetTraits bits
    namespace Trait
    {
        constexpr uint32_t TwoSided = 1, DecalState = 2, SpecColorMap = 4, GlossMap = 8, AoMap = 16, RevealMap = 32, TintMask = 64,
                           Emissive = 128, Distortion = 256, Effect = 512, Glass = 1024, BaseSpec = 2048, ColorSpec = 4096,
                           NormalMap = 8192, Detail = 16384, Camo = 32768, Customize = 65536, Transition = 131072;
    }

    struct Slot
    {
        std::string Name;       // the shader's texture name (the semantic hash is of this)
        std::string Key;        // the material GDT key; empty: the texture has no material key
        std::string Semantic;   // the image semantic(s) it accepts, comma separated; empty: any
        std::string Default;    // the slot's default image ("$white_gloss"); a material leaves that image out
        uint32_t Hash = 0;
    };

    struct Techset
    {
        std::string Name, Category, RenderFlags;
        Blend Class = Blend::Other;
        uint32_t Traits = 0;
        std::vector<Slot> Slots;
        const Slot* ByKey(const std::string& Key) const;
        size_t KeyedCount() const;
        bool HasTrait(uint32_t T) const { return (Traits & T) == T; }
    };

    // One constant a BO4 techset's block 0 holds: GDT key, byte offset, size, and whether the buffer holds it linear
    // while the GDT holds sRGB.
    struct ConstantAt
    {
        std::string Key;
        uint32_t Offset = 0, Size = 0;
        bool Srgb = false;
    };

    // What a map measured about a material it draws: the area of its render triangles by the way they face (normal z above 0.7 floor, below
    // -0.7 ceiling, else wall), whether the game draws it on a terrain mesh or as the map's sky, and the surface that names it (mesh and
    // surface number, for the gate's message). A material no map surface names has Via empty.
    struct MapUse
    {
        double Floor = 0, Ceiling = 0, Wall = 0;
        bool Terrain = false, Sky = false;
        std::string Via;
        bool Drawn() const { return !Via.empty(); }
        double Total() const { return Floor + Ceiling + Wall; }
        // Another map's use of the same material: the areas add, the roles join, the first surface stays
        void Merge(const MapUse& O);
    };

    struct UsageQuery
    {
        std::string Category, Type;     // the BO3 techset the material got: its catalog category and name ("Decal", "lit_decal")
        std::string Name;               // the material's name, for the tie-break tokens
        MapUse Use;
    };

    struct UsageChoice
    {
        std::string Usage;
        std::string Rule;               // the rule id ("decal", "facing", "token:carpet", "fallback")
        bool Fallback = false;          // nothing measured or named it: the fallback value
        std::string Why;                // for a fallback: what was missing
    };

    // The rule table zonekit/profiles/t7/target/material-usage.json (see the file and docs/hydrax-port/p2-usage.md). Never throws.
    class UsageRules
    {
    public:
        bool Load(const std::string& JsonText);
        bool Loaded() const { return !Rules.empty(); }
        // Role and techset rules first, then the facing with the largest area (as HydraX does), then the tie tokens of the name when no
        // triangle was measured or two facings tie, then the fallback. Without loaded rules: the facing alone.
        UsageChoice Choose(const UsageQuery& Q) const;
        // The facing's usage: floor, ceiling or wall by the largest area; empty when there is no area or the largest two tie
        std::string FacingUsage(const MapUse& U) const;
        size_t RuleCount() const { return Rules.size(); }

    private:
        struct Rule
        {
            std::string Id, Role, Category, Type, TypePrefix, NameToken, Usage;
        };
        std::vector<Rule> Rules;
        std::vector<std::pair<std::string, std::string>> Tokens;    // token -> usage
        std::string Fallback = "interior wall", FloorUsage = "floor", CeilingUsage = "ceiling", WallUsage = "interior wall";
    };

    class Data
    {
    public:
        // Reads <Profiles>/t7/target/{techsets,material-defaults,material-usage}.json and <Profiles>/bo4/target/constant-layouts.json.
        // A missing or unreadable file leaves its part empty (Problems says which).
        void Load(const std::filesystem::path& Profiles);
        bool HasCatalog() const { return !Sets.empty(); }
        bool HasDefaults() const { return !DefaultValues.empty(); }
        bool HasLayouts() const { return !Layouts.empty(); }
        const std::vector<std::string>& Problems() const { return Missing; }
        // The editor usage rules (material-usage.json); not loaded when the file is missing (Problems says so)
        const UsageRules& Usage() const { return UsageTable; }
        // Test hook: the usage rules from JSON text
        bool LoadUsageText(const std::string& Json) { return UsageTable.Load(Json); }

        const Techset* Find(const std::string& Name) const;
        // the rule table: a techset name the game gives that is forced to a catalog techset (optional "overrides" in
        // techsets.json); null when none
        const std::string* OverrideFor(const std::string& Source) const;
        const std::vector<Techset>& All() const { return Sets; }
        // key -> modal value over the stock materials (only those held by at least half of them)
        const std::map<std::string, std::string>& Defaults() const { return DefaultValues; }
        // The proven constants of a BO4 techset (its name hash, 60 bits); null when the table has none
        const std::vector<ConstantAt>* LayoutOf(uint64_t TechsetHash) const;
        // True when Image is the default image of some catalog slot with this key
        bool IsDefaultImage(const std::string& Key, const std::string& Image) const;
        // Whether some catalog techset has a texture of this slot hash (HydraX's known slots: a BO4-only slot is not known and is ignored)
        bool KnownHash(uint32_t Hash) const { return KnownHashes.count(Hash) != 0; }
        // The material key most catalog slots of this texture hash carry ("colorMap" for the colorMap texture; a few techsets key it otherwise):
        // what a material's image in that texture is, before a techset is chosen. Empty for an unknown or unkeyed hash.
        std::string DefaultKeyOfHash(uint32_t Hash) const;
        // The image semantics a code image ("$white_specular") stands for, read off the catalog: the semantics of the slots that
        // name it as their default (the Mod Tools' techsetdefs), with the number of slots for each. Empty when no slot defaults to it.
        const std::map<std::string, size_t>* CodeImageSemantics(const std::string& Image) const;
        // The semantic most slots give it, else empty
        std::string DominantCodeSemantic(const std::string& Image) const;

        // The BO4 and Cold War techset names (package_index/cod_techsets.wni, filled by update-name-db from echo000/cod-name-db). The
        // hash is the game's techset name hash, masked to 60 bits ('A'..'_' +32 FNV-style rule of TechsetHash is HydraX's; the index
        // keys are the same hash). Empty index: names are unknown and the slot rules decide.
        void LoadNames(const std::filesystem::path& WniFile);
        bool HasNames() const { return Names != nullptr; }
        // Test hook: a name set in place of the index file
        void SetNameForTest(uint64_t Hash, const std::string& Name);
        // The resolved name of a source techset id ("33832cd9511a3ad", "xtechset_<hex>"); empty when unknown or no index
        std::string NameOfSource(const std::string& SourceTechset) const;

        // Test hook: the catalog from JSON text (the techsets.json format)
        bool LoadCatalogText(const std::string& Json);

    private:
        std::vector<Techset> Sets;
        std::map<std::string, size_t> ByName;
        std::map<std::string, std::string> DefaultValues, Overrides;
        std::map<uint64_t, std::vector<ConstantAt>> Layouts;
        std::set<std::pair<std::string, std::string>> DefaultImages;
        std::set<uint32_t> KnownHashes;
        std::map<uint32_t, std::map<std::string, int>> KeysOfHash;
        std::map<std::string, std::map<std::string, size_t>> CodeSemantics;   // lower code image -> semantic -> slot count
        std::vector<std::string> Missing;
        UsageRules UsageTable;
        std::shared_ptr<WraithNameIndex> Names;
    };

    // The data shipped next to the exe (zonekit/profiles), loaded once. Empty parts when absent.
    const Data& Shared();

    enum class Rule { Override, Name, Normalized, Sky, Family, SlotSet, SlotSuperset, RenderClass, Default };
    const char* RuleName(Rule R);
    // Name, Family and SlotSet are exact; every other rule is a fallback to be reported
    bool IsExact(Rule R);

    struct Request
    {
        std::string SourceTechset;      // the game's own techset name; "" or a hash gives no usable name
        // the image slots the description fills: GDT key and the semantic the image has (diffuseMap, glossMap, ...)
        std::vector<std::pair<std::string, std::string>> Wanted;
        Blend Class = Blend::Opaque;    // how it draws: Opaque, AlphaTest, Blend or Decal
        bool Emissive = false;
        bool HasColorSpec = false;      // the material has a specular map ...
        bool ColorSpec = true;          // ... which is an RGB colour (else a scalar mask)
        // BO4: the class comes from the techset's renderFlags (HydraX's render class), not from the description. RenderFromFlags
        // switches the slot rules to HydraX's: HasRender false (flags unread) means no class, never a guess, so only lit techsets
        // are candidates; Class and Emissive are then unused. RenderTraits are Trait bits.
        bool RenderFromFlags = false;
        bool HasRender = false;
        Blend RenderBlend = Blend::Other;
        uint32_t RenderTraits = 0;
        // The material's image slot hashes (the game's semantic hashes). When given, the exact slot set rule compares them with the
        // texture names of each techset (HydraX exactSlotSet); empty: the rule works from Wanted.
        std::vector<uint32_t> SlotHashes;
    };

    struct Choice
    {
        const Techset* Chosen = nullptr;
        Rule Used = Rule::Default;
        std::string Reason;
        bool Exact() const { return IsExact(Used); }
    };

    // HydraX TreyarchRenderFlags.Classify: the render class of a techset renderFlags word; false when it says nothing (zero or unread)
    bool ClassifyRender(int64_t Flags, Blend& Out, uint32_t& Traits);
    // HydraX TreyarchRenderFlags.Matches: whether a catalog techset has the wanted class (glass and distortion told apart for premul)
    bool RenderMatches(Blend WantedBlend, uint32_t WantedTraits, const Techset& T);

    // The render class a known name stands for (HydraX would read the techset record; for a record that is not readable the name's own
    // catalog techset, by the Exact, Normalized, Sky and Family routes, says the same). False when the name is no catalog techset.
    bool ClassFromName(const Data& D, const std::string& Name, Blend& Out, uint32_t& Traits);

    // A name the game gave that can be looked up: not empty, not a bare hash ("2ce22f911a30ce4"), not "xtechset_<hash>".
    bool UsableName(std::string_view Name);
    // Null Chosen only when the catalog is empty
    Choice Choose(const Data& D, const Request& R);

    // Whether a slot takes an image of this semantic: the slot's list holds it, or it is a colour specular map offered
    // to a scalar specular slot (taken, but lossy: the caller counts it)
    bool Accepts(const Slot& S, const std::string& Intent);
    // The core semantic and compression an image semantic has in a stock image entry (bo3 references 1.3 I3)
    struct Form { const char* Core; const char* Compression; };
    Form FormOf(const std::string& Semantic);
    // The semantic an image of Intent gets on a slot: Intent when the slot takes it, else the slot's first
    std::string SemanticFor(const Slot& S, const std::string& Intent);

    // Linear to sRGB (BO4 stores colorTint linear, the GDT holds sRGB), then k/255 as APE shows it: rounded up
    // to 6 significant digits so APE shows k (references-bo3 M4). Returns the text of the 0..1 value.
    std::string TintComponentText(float Linear);
    std::string FloatText(float V);

    int SelfTest(std::string& Failures);
}
