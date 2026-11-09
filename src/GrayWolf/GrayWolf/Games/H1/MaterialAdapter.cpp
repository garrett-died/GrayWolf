#include "stdafx.h"

#include "MaterialDescription.h"
#include "SurfaceTypes.h"
#include "ZoneTableWalker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Modern Warfare Remastered materials (MaterialDescription.h), measured from the game's own zones and shaders (docs/hydrax-port/mwr-materials.md):
//   - colorMap (usage 2): the `_col` image; normalMap (usage 5): the `_nml` image, BC5 x and y (the z is rebuilt by the BO3 target);
//   - specularMap (usage 8): a build-time composite. RGB is the specular colour, stored as sqrt(F0) (the shader squares it: F0 = sp.rgb^2, mwr-materials.md 12.2),
//     and the ALPHA is the gloss, linear 0..1, the shader's specular power being 2^(16 a) (12.1). BO3 computes alpha = sqrt(2 / (2^(17 g) + 2)) with g in stops
//     (s = min + gloss * (max - min)), the same law, so the alpha image is the gloss image unchanged and the range is 0..16 stops;
//   - occlusionMap (usage 16): a BC4 luminance composite.
// The surface word is a bit set (flag_names.txt of the h1 profile), the techset a grammar (techset_rules.txt), the sort key a draw-order class (2 opaque).
// The layer slots of a layered blend ("*49n_50n": colorMap1.. normalMap1.. specularMap1.. occlusionMap1.. revealMap1..) are not read here: the base slots are the first
// layer's, and the others are counted in Description::LayersDropped (a multi-mesh decomposition is a later step).
namespace
{
    using Materials::Role;
    constexpr uint32_t ColorMap = 0xA0AB1041, NormalMap = 0x59D30D0F, SpecularMap = 0x34ECCCB3, OcclusionMap = 0x6001F931, EmissiveMap = 0x34614347;   // emissive: usage 17, 173 materials, _ems (mwr-materials.md 4.1)
    // colorMap1..6 (the colour of a blend layer), and revealMap1..6 and the base reveal (rp): the hashes of the lower-cased name, h = h * 33 ^ c
    const std::set<uint32_t> LayerColors = { 0xB60D1850, 0xB60D1853, 0xB60D1852, 0xB60D1855, 0xB60D1854, 0xB60D1857 };
    constexpr uint32_t BaseReveal = 0x34D849D5;   // "revealmap"

    // one row of techset_rules.txt
    struct Rule
    {
        std::string Id, Techset, Alpha, Label;
        std::vector<std::string> Conditions;
    };

    std::vector<std::string> Split(const std::string& Text, char Sep)
    {
        std::vector<std::string> Out;
        std::stringstream In(Text);
        for (std::string Part; std::getline(In, Part, Sep);) Out.push_back(Part);
        return Out;
    }

    // What the rule file holds, or why it could not be used. A table that failed to load is never kept (the next call reads the file again), and the
    // reason reaches the gate as a material.techset_fallback row with reason=rules_... (a failure there), never as "no_rule".
    struct RuleTable
    {
        std::vector<Rule> Rows;
        std::set<std::string> MetalSurfaces;   // the BO3 surface types whose materials take the advanced (specColorMap) family
        std::string Error;                      // empty when the table is usable
    };

    bool KnownCondition(const std::string& Condition)
    {
        static const std::set<std::string> Plain = { "any", "spec", "nospec", "reveal", "metal", "nonmetal", "plain", "emissive" };
        if (Plain.count(Condition) != 0) return true;
        if (Condition.rfind("sort>", 0) == 0 || Condition.rfind("sort<=", 0) == 0) return true;
        const size_t Eq = Condition.find('=');
        if (Eq == std::string::npos) return false;
        static const std::set<std::string> Keys = { "prefix", "token", "tokenprefix", "has", "surface" };
        return Keys.count(Condition.substr(0, Eq)) != 0;
    }

    // the folder the rule file is read from instead of the profile's (the self-test only)
    std::string& RulesDirOverride()
    {
        static std::string Dir;
        return Dir;
    }

    RuleTable LoadRules()
    {
        RuleTable T;
        std::filesystem::path Dir = RulesDirOverride();
        if (Dir.empty())
        {
            ZoneTables::Profile P;
            std::string Error;
            if (!ZoneTables::LoadProfile("h1", P, Error))
            {
                T.Error = "rules_missing:profile h1 not loaded (" + Error + ")";
                return T;
            }
            Dir = P.Dir;
        }
        const std::filesystem::path File = Dir / "techset_rules.txt";
        std::ifstream In(File);
        if (!In)
        {
            T.Error = "rules_missing:" + File.string();
            return T;
        }
        int LineNo = 0;
        for (std::string Line; std::getline(In, Line);)
        {
            LineNo++;
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            if (Line.empty() || Line[0] == '#') continue;
            std::vector<std::string> F;
            std::stringstream Fields(Line);
            for (std::string Part; std::getline(Fields, Part, '|');) F.push_back(Part);
            if (F.size() == 3 && F[0] == "set" && F[1] == "metal_surfaces")
            {
                for (const std::string& S : Split(F[2], ';')) T.MetalSurfaces.insert(S);
                continue;
            }
            if (F.size() < 5 || F[0] != "rule")
            {
                T.Error = "rules_malformed:line " + std::to_string(LineNo);
                return T;
            }
            Rule R{ F[1], F[3], F[4], F.size() > 5 ? F[5] : std::string(), Split(F[2], ';') };
            for (const std::string& C : R.Conditions)
                if (!KnownCondition(C))
                {
                    T.Error = "rules_unknown_condition:" + C + " (line " + std::to_string(LineNo) + ")";
                    return T;
                }
            T.Rows.push_back(std::move(R));
        }
        if (T.Rows.empty()) T.Error = "rules_missing:no rule rows in " + File.string();
        return T;
    }

    // The usable table, shared; an unusable one is returned each time it fails and is not cached.
    std::shared_ptr<const RuleTable> Rules(bool Forget = false)
    {
        static std::mutex Lock;
        static std::shared_ptr<const RuleTable> Kept;
        std::lock_guard<std::mutex> Guard(Lock);
        if (Forget) Kept.reset();
        if (Kept) return Kept;
        auto Loaded = std::make_shared<const RuleTable>(LoadRules());
        if (Loaded->Error.empty()) Kept = Loaded;
        return Loaded;
    }

    // what a rule can ask about a material
    struct Facts
    {
        std::string Name;                // the MWR techset name, lower case
        std::vector<std::string> Tokens; // its '_' tokens
        int SortKey = -1;
        std::string Surface;             // the BO3 surfaceType of the description
        bool Spec = false, Reveal = false, Metal = false, Plain = false, Emissive = false;
    };

    bool Holds(const std::string& Condition, const Facts& F)
    {
        const size_t Eq = Condition.find('=');
        const std::string Key = Eq == std::string::npos ? Condition : Condition.substr(0, Eq);
        const std::string Value = Eq == std::string::npos ? std::string() : Condition.substr(Eq + 1);
        if (Condition == "any") return true;
        if (Condition == "spec") return F.Spec;
        if (Condition == "nospec") return !F.Spec;
        if (Condition == "reveal") return F.Reveal;
        if (Condition == "metal") return F.Metal;
        if (Condition == "nonmetal") return !F.Metal;
        if (Condition == "plain") return F.Plain;
        if (Condition == "emissive") return F.Emissive;
        if (Key == "prefix") return F.Name.rfind(Value, 0) == 0;
        if (Key == "token") return std::find(F.Tokens.begin(), F.Tokens.end(), Value) != F.Tokens.end();
        if (Key == "tokenprefix") return std::any_of(F.Tokens.begin(), F.Tokens.end(), [&](const std::string& T) { return T.rfind(Value, 0) == 0; });
        if (Key == "has") return F.Name.find(Value) != std::string::npos;
        if (Key == "surface") return F.Surface == Value;
        if (Condition.rfind("sort>", 0) == 0) return F.SortKey >= 0 && F.SortKey > std::atoi(Condition.substr(5).c_str());
        if (Condition.rfind("sort<=", 0) == 0) return F.SortKey >= 0 && F.SortKey <= std::atoi(Condition.substr(6).c_str());
        return false;   // a condition this build does not know never matches (a typo in the data is a rule that does not fire)
    }

    class MwrMaterials final : public Materials::SemanticAdapter
    {
    public:
        MwrMaterials()
            : SemanticAdapter("mwr", {
                  { ColorMap, Role::Albedo },
                  { NormalMap, Role::Normal },
                  { SpecularMap, Role::Specular },
                  { SpecularMap, Role::Gloss, 3 },        // the alpha of the specular composite
                  { OcclusionMap, Role::Occlusion, 0 },   // BC4, one channel
                  { EmissiveMap, Role::Emissive },        // the emissive colour (_ems), usage 17
              }) {}

        // The image naming the game declares (mwr-materials.md 4.4, 4.5): the build-time suffixes become BO3's tags, and the mtl_ the stock names never carry is dropped
        static Materials::ImageNaming Naming()
        {
            Materials::ImageNaming N;
            N.Suffixes = { { "_col", "c" }, { "_nml", "n" }, { "_nrm", "n" }, { "_spc", "s" }, { "_cos", "g" }, { "_occ", "o" }, { "_ems", "e" }, { "_emi", "e" }, { "_rvl", "r" } };
            N.StripPrefixes = { "mtl_" };
            return N;
        }

    protected:
        void Refine(const Materials::Source& M, Materials::Description& D, bool) const override
        {
            D.Naming = Naming();
            // the specular colour is sqrt(F0), the gloss the alpha of the same file in 0..16 stops (12.1, 12.2)
            if (!D.Specular.Empty()) D.Specular.Pack = Materials::PackKind::SpecularSqrtF0;
            if (!D.Roughness.Empty() && D.Roughness.Channel == 3)
            {
                D.Roughness.Pack = Materials::PackKind::SpecularGlossAlpha;
                D.GlossStopsMin = 0.0f;
                D.GlossStopsMax = 16.0f;
            }
            // the base slots are the first layer's: the other layers of a blend are counted, not carried
            for (const auto& Image : M.Images)
                if (LayerColors.count(Image.Semantic)) D.LayersDropped++;

            // the surface type: the bit of the material (a blend: its first layer's, where the reader could tell)
            SurfaceTypes::Apply("h1", M, D);
            // An emissive slot bound to a colour image (`_col`, `_c`: foliage in mp_vacant binds the ivy and the dead grass to their own colour map) is not an emission: the file names its role. Left
            // unbound and said, the material takes the family its other maps say. A real emissive image (`_ems`, `_emi`, `_e`) is untouched.
            if (!D.Emissive.Empty() && Materials::RoleFromName(std::filesystem::path(D.Emissive.Path).stem().string()) == Role::Albedo)
            {
                D.Emissive = Materials::Map();
                D.TexturesUnbound += (D.TexturesUnbound.empty() ? "" : ",") + std::string("emissive(the image is a colour map)");
            }
            D.FoliageVegetation = true;   // a foliage surface takes BO3's vegetation family when its source is alpha tested or blended (MaterialGdt Builder::Add)

            // the BO3 material type, by the techset grammar
            const auto Choose = [&]() {
            Facts F;
            for (const char C : M.Techset) F.Name += (char)std::tolower((unsigned char)C);
            F.Tokens = Split(F.Name, '_');
            F.SortKey = M.SortKey;
            F.Surface = D.SurfaceType;
            F.Spec = !D.Specular.Empty();
            F.Plain = D.Roughness.Empty() && D.Occlusion.Empty();
            // the decal's own reveal map (the base one, not a blend layer's) is bound to alphaRevealMap by the builder; a rule that names a reveal techset needs it
            for (const auto& Image : M.Images)
                if (Image.Semantic == BaseReveal)
                {
                    F.Reveal = true;
                    D.RevealSemantic = BaseReveal;
                    D.RevealChannel = 0;   // the reveal image is a grey picture: its red channel
                    // a blend layer's reveal (a scene material made by Games/H1/BlendLayers: the techset name holds the token `layer`): MWR draws the layer where the map's green
                    // is LOW against w^2, BO3 where it is HIGH, so the image is written as 1 - green; BO3's reveal is alpha = sat((r - lo) / (hi - lo)), lo = 1 - s*a^p,
                    // hi = 1 + s*(1-a)^p, which for ramp p = 0 and soft edge s = 1/(2K) is MWR's sat((w^2 - R) * K) (mwr-materials.md 12.3, K = revealParams.x)
                    if (std::find(F.Tokens.begin(), F.Tokens.end(), "layer") != F.Tokens.end())
                    {
                        D.RevealChannel = 1;
                        D.RevealInvert = true;
                        for (const auto& Block : M.Constants)
                            for (size_t At = 0; At + 32 <= Block.size(); At += 32)
                            {
                                uint32_t Hash;
                                float K;
                                std::memcpy(&Hash, &Block[At], 4);
                                std::memcpy(&K, &Block[At + 16], 4);
                                if (Hash == 0x8C67DED5u && K >= 1.0f && std::isfinite(K))
                                {
                                    D.RevealRamp = 0.0f;
                                    // material.awi limits alphaRevealSoftEdge to 0.01..1: a sharpness above 50 is written as 0.01 (row reveal_soft_edge_clamped)
                                    D.RevealSoftEdge = std::clamp(1.0f / (2.0f * K), 0.01f, 1.0f);
                                    if (1.0f / (2.0f * K) < 0.01f) D.Approximations = "reveal_soft_edge_clamped";   // joined with the layer's others below
                                }
                            }
                    }
                }
            const std::shared_ptr<const RuleTable> Table = Rules();
            // metal is told by the surface type (the rule file's metal_surfaces), and only a metal with a specular map takes the advanced family
            F.Emissive = !D.Emissive.Empty();
            // an ambiguous or unread surface is not known to be a dielectric: it keeps its specular map (a dielectric map on advanced_fullspec costs nothing), with a row
            const bool SurfaceUnknown = D.SurfaceType.empty() && D.SurfaceWhy != "none_in_source";
            F.Metal = F.Spec && (Table->MetalSurfaces.count(F.Surface) != 0 || SurfaceUnknown);
            if (F.Spec && SurfaceUnknown && Table->MetalSurfaces.count(F.Surface) == 0) D.FamilyWhy = "surface_unknown_treated_as_metal";
            if (F.Name.empty())
            {
                D.TechsetWhy = "no_techset";
                return;
            }
            if (!Table->Error.empty())
            {
                D.TechsetWhy = Table->Error;   // reaches the gate as a failure (material.techset_fallback, reason=rules_...)
                return;
            }
            for (const Rule& R : Table->Rows)
            {
                if (!std::all_of(R.Conditions.begin(), R.Conditions.end(), [&](const std::string& C) { return Holds(C, F); })) continue;
                if (R.Techset.rfind("SKIP:", 0) == 0)
                {
                    D.SkipWhy = R.Techset.substr(5) + ":techset=" + F.Name;
                    return;
                }
                D.TechsetName = R.Techset;
                // only the advanced family reads a specular colour map; a non-metal takes BO3's specular simple (the dielectric level the source's 51 is)
                if (R.Techset.find("advanced_fullspec") == std::string::npos)
                {
                    // the pixel check (Description::PixelMetal): what the rules give this material if it were a metal, and its specular image kept
                    if (F.Spec && !F.Metal)
                    {
                        Facts AsMetal = F;
                        AsMetal.Metal = true;
                        for (const Rule& Q : Table->Rows)
                            if (std::all_of(Q.Conditions.begin(), Q.Conditions.end(), [&](const std::string& C) { return Holds(C, AsMetal); }))
                            {
                                if (Q.Techset.find("advanced_fullspec") != std::string::npos)
                                {
                                    D.Metal.Specular = D.Specular;
                                    D.Metal.PromoteTechset = Q.Techset;
                                    D.Metal.P99 = 100.0f;
                                }
                                break;
                            }
                    }
                    D.Specular = Materials::Map();
                    if (!D.FamilyWhy.empty()) D.FamilyWhy = "surface_unknown_specular_dropped_by_rule";   // the rule's family reads no specular map: the row must not claim it was kept
                }
                if (R.Techset.find("reveal") == std::string::npos) D.RevealSemantic = 0;
                // only an emissive techset takes the emissive map; for another family it is reported unbound
                if (!D.Emissive.Empty() && R.Techset.find("emissive") == std::string::npos)
                {
                    D.Emissive = Materials::Map();
                    D.TexturesUnbound += (D.TexturesUnbound.empty() ? "" : ",") + std::string("emissive(no emissive techset in this family)");
                }
                if (R.Alpha == "opaque") D.Alpha = Materials::AlphaMode::Opaque;
                else if (R.Alpha == "cutout") D.Alpha = Materials::AlphaMode::Cutout;
                else if (R.Alpha == "blend") D.Alpha = Materials::AlphaMode::Blend;
                return;
            }
            D.TechsetWhy = "no_rule";   // no row of techset_rules.txt fits this name: the slot rules choose, and the exporter says so
            };
            Choose();
            // a blend layer: its colour image's alpha multiplies the weight per pixel in MWR (no vertex place in BO3), and the layer's normal is rotated by the vertex's
            // rotation word (no BO3 place): both are left out, said in a row
            {
                const std::vector<std::string> Words = Split([&] { std::string Lower; for (const char C : M.Techset) Lower += static_cast<char>(std::tolower(static_cast<unsigned char>(C))); return Lower; }(), '_');
                if (std::find(Words.begin(), Words.end(), "layer") != Words.end())
                {
                    const std::string Earlier = D.Approximations;   // reveal_soft_edge_clamped, from the rule above
                    D.Approximations = "colour_alpha_not_folded";
                    if (!D.Normal.Empty()) D.Approximations += ",normal_rotation_dropped";
                    // MWR squares the interpolated weight per pixel, BO3 interpolates the vertex alpha: only a layer with a reveal differs
                    if (D.RevealInvert) D.Approximations += ",weight_squared_interpolation";
                    if (!Earlier.empty()) D.Approximations += "," + Earlier;

                }
            }
            AccountTextures(M, D);
        }

        // Every semantic of the source the description does not carry is named (mwr-materials.md 4.1): the exporter writes one material.texture_unbound row and the
        // gate warns. Not named: the base slots (bound), the layer slots of a blend (counted as layers_dropped), the environment BRDF lookup (the engine supplies it).
        static void AccountTextures(const Materials::Source& M, Materials::Description& D)
        {
            const auto InRange = [](uint32_t V, uint32_t Lo, uint32_t Hi) { return V >= Lo && V <= Hi; };
            std::set<std::string> Names;
            for (const auto& Image : M.Images)
            {
                const uint32_t S = Image.Semantic;
                if (S == ColorMap || S == NormalMap || S == SpecularMap || S == OcclusionMap) continue;
                if (S == EmissiveMap) continue;   // bound, or reported by the family rule above
                if (S == BaseReveal && D.RevealSemantic != 0) continue;
                if (LayerColors.count(S) || S == 0xCD7DF742 || InRange(S, 0x9434AED8, 0x9434AEDF) || InRange(S, 0xD2866320, 0xD2866327) || InRange(S, 0x60411F60, 0x60411F67) ||
                    InRange(S, 0x7D3CDFB0, 0x7D3CDFB7) || InRange(S, 0xCFE18440, 0xCFE18447))
                    continue;
                if (S == BaseReveal) Names.insert("reveal(no reveal techset)");
                else if (S == 0xEB529B4D || S == 0x55A604DC || S == 0x5A7D8971) Names.insert("detail");
                else if (InRange(S, 0x62F1F090, 0x62F1F09F)) Names.insert("decalLayer");
                else if (S == 0x21CF6E54 || S == 0x546D65C7) Names.insert("parallax");
                else Names.insert(std::format("unknown_0x{:08X}", S));
            }
            for (const std::string& N : Names) D.TexturesUnbound += (D.TexturesUnbound.empty() ? "" : ",") + N;
        }
    };
    const MwrMaterials Adapter;
    const Materials::AdapterRegistration Registered("H1", Adapter);
}

// Self-test of the rule table's states (called from MaterialGdt's self-test): a missing folder, a malformed row and an unknown condition are distinct errors, and a
// failed load is not kept (the next load, with a good file, works).
bool H1TechsetRulesSelfTest(const std::string& TempDir, std::string& Got)
{
    namespace fs = std::filesystem;
    const auto Put = [&](const fs::path& Dir, const std::string& Text) {
        std::error_code Ec;
        fs::create_directories(Dir, Ec);
        std::ofstream(Dir / "techset_rules.txt", std::ios::binary) << Text;
    };
    const fs::path Base = fs::path(TempDir) / "h1_rules_test";
    const auto State = [&](const fs::path& Dir, bool Forget) {
        RulesDirOverride() = Dir.string();
        return Rules(Forget)->Error;
    };
    // a failed load is never kept: after the missing folder, each next state is read from its own folder without forgetting anything
    const std::string Missing = State(Base / "none", true);
    Put(Base / "bad_row", "rule|a|any|lit\n");
    const std::string Bad = State(Base / "bad_row", false);
    Put(Base / "bad_cond", "rule|a|colour=red|lit|opaque|x\n");
    const std::string Cond = State(Base / "bad_cond", false);
    Put(Base / "good", "set|metal_surfaces|metal\nrule|a|any|lit|opaque|x\n");
    const std::string Good = State(Base / "good", false);
    const std::string Kept = State(Base / "none", false);   // a good table is kept: the folder is not read again
    // the adapter's Describe with no usable table: the reason reaches the description (the gate fails it), it is not no_rule
    RulesDirOverride() = (Base / "none").string();
    Rules(true);
    Materials::Source Src;
    Src.Name = "mtl_h1_probe";
    Src.Techset = "m_l_sm_lmpb_probe";
    Src.SortKey = 2;
    Src.SurfaceFlags = 1ll << 4;
    const Materials::Description Described = Adapter.Describe(Src);
    RulesDirOverride().clear();
    Rules(true);
    Got = Missing + " | " + Bad + " | " + Cond + " | [" + Good + "] | [" + Kept + "] | " + Described.TechsetWhy;
    return Missing.rfind("rules_missing:", 0) == 0 && Bad.rfind("rules_malformed:", 0) == 0 && Cond.rfind("rules_unknown_condition:", 0) == 0 && Good.empty() && Kept.empty() &&
           Described.TechsetName.empty() && Described.TechsetWhy.rfind("rules_missing:", 0) == 0;
}
