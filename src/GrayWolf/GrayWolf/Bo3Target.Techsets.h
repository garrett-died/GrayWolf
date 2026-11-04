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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    class Data
    {
    public:
        // Reads <Profiles>/t7/target/{techsets,material-defaults}.json and <Profiles>/bo4/target/constant-layouts.json.
        // A missing or unreadable file leaves its part empty (Problems says which).
        void Load(const std::filesystem::path& Profiles);
        bool HasCatalog() const { return !Sets.empty(); }
        bool HasDefaults() const { return !DefaultValues.empty(); }
        bool HasLayouts() const { return !Layouts.empty(); }
        const std::vector<std::string>& Problems() const { return Missing; }

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

        // Test hook: the catalog from JSON text (the techsets.json format)
        bool LoadCatalogText(const std::string& Json);

    private:
        std::vector<Techset> Sets;
        std::map<std::string, size_t> ByName;
        std::map<std::string, std::string> DefaultValues, Overrides;
        std::map<uint64_t, std::vector<ConstantAt>> Layouts;
        std::set<std::pair<std::string, std::string>> DefaultImages;
        std::vector<std::string> Missing;
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
    };

    struct Choice
    {
        const Techset* Chosen = nullptr;
        Rule Used = Rule::Default;
        std::string Reason;
        bool Exact() const { return IsExact(Used); }
    };

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
