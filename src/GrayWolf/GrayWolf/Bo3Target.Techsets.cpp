#include "stdafx.h"

#include "Bo3Target.Techsets.h"

#include "WraithNameIndex.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <limits>
#include <tuple>

namespace fs = std::filesystem;

namespace Bo3Target::Techsets
{
namespace
{
    std::string Lower(std::string_view S)
    {
        std::string Out(S);
        for (char& C : Out) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
        return Out;
    }

    // HydraX SpecialPurpose: never chosen, whatever the game names
    bool Special(const std::string& N)
    {
        for (const char* Word : { "base", "stub", "shaders", "script", "camo" })
            if (N.find(Word) != std::string::npos) return true;
        return false;
    }
    bool Dotted(const std::string& N) { return N.find('.') != std::string::npos; }
    bool IsLit(const std::string& N) { return (N == "lit" || N.rfind("lit_", 0) == 0) && !Dotted(N) && !Special(N); }
    bool IsVegetation(const std::string& N)
    {
        return (N.rfind("vegetation", 0) == 0 || N.rfind("fxanim_vegetation", 0) == 0) && !Dotted(N) && !Special(N);
    }
    // Techsets a game's material can be written with: lit and vegetation geometry, glass, effect and emissive ones
    bool IsMappable(const std::string& N)
    {
        if (IsLit(N) || IsVegetation(N)) return true;
        return (N.rfind("glass", 0) == 0 || N.rfind("effect_", 0) == 0 || N.rfind("emissive", 0) == 0 || N.rfind("decal_emissive", 0) == 0) &&
               !Dotted(N) && N.find("cloud") == std::string::npos && !Special(N);
    }

    std::vector<std::string> SplitList(const std::string& List)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        while (Start <= List.size())
        {
            size_t End = List.find(',', Start);
            if (End == std::string::npos) End = List.size();
            size_t A = Start, B = End;
            while (A < B && List[A] == ' ') ++A;
            while (B > A && List[B - 1] == ' ') --B;
            if (B > A) Out.push_back(List.substr(A, B - A));
            Start = End + 1;
        }
        return Out;
    }

    bool InList(const std::vector<std::string>& List, const std::string& Item)
    {
        const std::string L = Lower(Item);
        return std::any_of(List.begin(), List.end(), [&](const std::string& S) { return Lower(S) == L; });
    }

    // the family of a BO4 name: no folder, no "#variant"
    std::string FamilyOf(const std::string& Name)
    {
        std::string N = Name.substr(0, Name.find('#'));
        if (const size_t Slash = N.rfind('/'); Slash != std::string::npos) N = N.substr(Slash + 1);
        while (!N.empty() && N.back() == ' ') N.pop_back();
        while (!N.empty() && N.front() == ' ') N.erase(N.begin());
        return N;
    }

    // BO4 names the micro-tile mix a blend (HydraX FamilyAliases)
    std::string FamilyTarget(const Data& D, const std::string& Family)
    {
        if (Family == "lit_micro_tile_mix" && D.Find("lit_micro_tile_blend") != nullptr) return "lit_micro_tile_blend";
        return D.Find(Family) != nullptr ? Family : std::string();
    }

    std::string BareSky(const std::string& Name)
    {
        std::string N = Name.substr(0, Name.find('#'));
        if (const size_t Slash = N.rfind('/'); Slash != std::string::npos) N = N.substr(Slash + 1);
        return N.rfind("sky_", 0) == 0 ? N : std::string();
    }

    bool IsHex(std::string_view S)
    {
        return !S.empty() && std::all_of(S.begin(), S.end(), [](char C) { return std::isxdigit(static_cast<unsigned char>(C)) != 0; });
    }

    std::string KeysText(const std::vector<std::pair<std::string, std::string>>& Wanted)
    {
        std::string Out;
        for (const auto& [K, I] : Wanted) Out += (Out.empty() ? "" : ", ") + K;
        return Out.empty() ? "none" : Out;
    }

    const char* const BlendNames[] = { "Other", "Opaque", "AlphaTest", "Blend", "Premul", "Add", "Multiply", "Decal" };
}

uint32_t SlotHash(std::string_view Name)
{
    uint32_t H = 0;
    for (const char Ch : Name)
    {
        uint32_t C = static_cast<unsigned char>(Ch);
        if (C >= 0x41 && C <= 0x5F) C += 32;
        H = (H * 33u) ^ C;
    }
    return H;
}

const char* BlendName(Blend B)
{
    return BlendNames[static_cast<size_t>(B) < std::size(BlendNames) ? static_cast<size_t>(B) : 0];
}

Blend BlendFromName(std::string_view Name)
{
    for (size_t I = 0; I < std::size(BlendNames); ++I)
        if (Name == BlendNames[I]) return static_cast<Blend>(I);
    return Blend::Other;
}

const Slot* Techset::ByKey(const std::string& Key) const
{
    if (Key.empty()) return nullptr;
    for (const Slot& S : Slots)
        if (S.Key == Key) return &S;
    return nullptr;
}

size_t Techset::KeyedCount() const
{
    return static_cast<size_t>(std::count_if(Slots.begin(), Slots.end(), [](const Slot& S) { return !S.Key.empty(); }));
}

std::string Data::DefaultKeyOfHash(uint32_t Hash) const
{
    const auto It = KeysOfHash.find(Hash);
    if (It == KeysOfHash.end()) return {};
    std::string Best;
    int BestCount = 0;
    for (const auto& [Key, Count] : It->second)
        if (Count > BestCount) { BestCount = Count; Best = Key; }
    return Best;
}

const Techset* Data::Find(const std::string& Name) const
{
    const auto It = ByName.find(Name);
    return It == ByName.end() ? nullptr : &Sets[It->second];
}

const std::string* Data::OverrideFor(const std::string& Source) const
{
    const auto It = Overrides.find(Source);
    return It == Overrides.end() ? nullptr : &It->second;
}

const std::vector<ConstantAt>* Data::LayoutOf(uint64_t TechsetHash) const
{
    const auto It = Layouts.find(TechsetHash & 0x0FFFFFFFFFFFFFFFull);
    return It == Layouts.end() ? nullptr : &It->second;
}

bool Data::IsDefaultImage(const std::string& Key, const std::string& Image) const
{
    return DefaultImages.count({ Key, Lower(Image) }) != 0;
}

void Data::LoadNames(const fs::path& WniFile)
{
    Names.reset();
    std::error_code Ec;
    if (!fs::is_regular_file(WniFile, Ec)) return;
    auto Index = std::make_shared<WraithNameIndex>();
    Index->LoadIndex(WniFile.string());      // keys masked to 60 bits
    if (Index->NameDatabase.size() == 0) return;
    Names = std::move(Index);
}

void Data::SetNameForTest(uint64_t Hash, const std::string& Name)
{
    if (!Names) Names = std::make_shared<WraithNameIndex>();
    Names->NameDatabase.insert_or_assign(Hash & 0x0FFFFFFFFFFFFFFFull, Name);
}

std::string Data::NameOfSource(const std::string& SourceTechset) const
{
    if (!Names) return std::string();
    std::string Hex = SourceTechset;
    if (Hex.rfind("xtechset_", 0) == 0) Hex = Hex.substr(9);
    if (Hex.empty() || Hex.size() > 16 || !IsHex(Hex)) return std::string();
    const uint64_t Key = std::stoull(Hex, nullptr, 16) & 0x0FFFFFFFFFFFFFFFull;
    const auto It = Names->NameDatabase.find(Key);
    return It == Names->NameDatabase.end() ? std::string() : It->second;
}

const std::map<std::string, size_t>* Data::CodeImageSemantics(const std::string& Image) const
{
    const auto It = CodeSemantics.find(Lower(Image));
    return It == CodeSemantics.end() ? nullptr : &It->second;
}

std::string Data::DominantCodeSemantic(const std::string& Image) const
{
    const std::map<std::string, size_t>* All = CodeImageSemantics(Image);
    if (All == nullptr) return std::string();
    std::string Best;
    size_t Most = 0;
    for (const auto& [Semantic, Count] : *All)
        if (Count > Most) { Best = Semantic; Most = Count; }
    return Best;
}

bool Data::LoadCatalogText(const std::string& Json)
{
    Sets.clear();
    ByName.clear();
    Overrides.clear();
    DefaultImages.clear();
    CodeSemantics.clear();
    KnownHashes.clear();
    KeysOfHash.clear();
    const nlohmann::json Root = nlohmann::json::parse(Json, nullptr, false);
    if (Root.is_discarded() || !Root.is_object() || !Root.contains("techsets") || !Root["techsets"].is_array()) return false;
    const auto Str = [](const nlohmann::json& J, const char* Key) {
        return J.contains(Key) && J[Key].is_string() ? J[Key].get<std::string>() : std::string();
    };
    for (const nlohmann::json& J : Root["techsets"])
    {
        if (!J.is_object()) continue;
        Techset T;
        T.Name = Str(J, "n");
        if (T.Name.empty()) continue;
        T.Category = Str(J, "c");
        T.RenderFlags = Str(J, "f");
        T.Class = BlendFromName(Str(J, "b"));
        T.Traits = J.contains("t") && J["t"].is_number_unsigned() ? J["t"].get<uint32_t>() : 0;
        if (J.contains("s") && J["s"].is_array())
        {
            for (const nlohmann::json& A : J["s"])
            {
                if (!A.is_array() || A.size() < 4) continue;
                const auto Cell = [&](size_t I) { return A[I].is_string() ? A[I].get<std::string>() : std::string(); };
                Slot S;
                S.Name = Cell(0);
                S.Key = Cell(1);
                S.Semantic = Cell(2);
                S.Default = Cell(3);
                S.Hash = SlotHash(S.Name);
                if (!S.Key.empty()) { KnownHashes.insert(S.Hash); KeysOfHash[S.Hash][S.Key]++; }     // HydraX _knownSlots: the hashes of ImageSlots, the textures a GDT key can hold
                if (!S.Key.empty() && !S.Default.empty()) DefaultImages.insert({ S.Key, Lower(S.Default) });
                if (!S.Default.empty() && S.Default.front() == '$')
                    for (const std::string& One : SplitList(S.Semantic)) CodeSemantics[Lower(S.Default)][One]++;
                T.Slots.push_back(std::move(S));
            }
        }
        ByName[T.Name] = Sets.size();
        Sets.push_back(std::move(T));
    }
    // HYPOTHESIS: $specular_mask is no slot's default, so the techsetdefs say nothing about it. t7kb gdt_check reads it as a specularMask
    // (mp_frenetic, 2026-11-04) and the name says so; this one line is the single rule the writer and the gate both use.
    if (CodeSemantics.find("$specular_mask") == CodeSemantics.end()) CodeSemantics["$specular_mask"]["specularMask"] = 1;
    if (Root.contains("overrides") && Root["overrides"].is_object())
        for (const auto& [From, To] : Root["overrides"].items())
            if (To.is_string()) Overrides[From] = To.get<std::string>();
    return !Sets.empty();
}

void Data::Load(const fs::path& Profiles)
{
    Missing.clear();
    DefaultValues.clear();
    Layouts.clear();
    const auto Slurp = [](const fs::path& File, std::string& Text) {
        std::ifstream In(File, std::ios::binary);
        if (!In) return false;
        Text.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
        return true;
    };
    std::string Text;
    const fs::path T7 = Profiles / "t7" / "target", Bo4 = Profiles / "bo4" / "target";
    if (!Slurp(T7 / "techsets.json", Text) || !LoadCatalogText(Text)) Missing.push_back("zonekit/profiles/t7/target/techsets.json");

    if (!Slurp(T7 / "material-defaults.json", Text)) Missing.push_back("zonekit/profiles/t7/target/material-defaults.json");
    else
    {
        const nlohmann::json Root = nlohmann::json::parse(Text, nullptr, false);
        if (Root.is_discarded() || !Root.contains("keys") || !Root["keys"].is_object()) Missing.push_back("zonekit/profiles/t7/target/material-defaults.json (unreadable)");
        else
            for (const auto& [Key, V] : Root["keys"].items())
                if (V.is_object() && V.contains("share") && V["share"].is_number() && V["share"].get<double>() >= 0.5 && V.contains("value") && V["value"].is_string())
                    DefaultValues[Key] = V["value"].get<std::string>();
    }

    if (!Slurp(Bo4 / "constant-layouts.json", Text)) Missing.push_back("zonekit/profiles/bo4/target/constant-layouts.json");
    else
    {
        const nlohmann::json Root = nlohmann::json::parse(Text, nullptr, false);
        if (Root.is_discarded() || !Root.contains("defs") || !Root["defs"].is_array() || !Root.contains("techsets") || !Root["techsets"].is_object())
            Missing.push_back("zonekit/profiles/bo4/target/constant-layouts.json (unreadable)");
        else
        {
            const nlohmann::json& Defs = Root["defs"];
            std::set<uint64_t> Ambiguous;
            for (const auto& [Hex, Rows] : Root["techsets"].items())
            {
                if (!Rows.is_array() || Hex.empty() || Hex.size() > 16 || !IsHex(Hex)) continue;
                std::vector<ConstantAt> List;
                for (const nlohmann::json& Row : Rows)
                {
                    if (!Row.is_array() || Row.size() < 2 || !Row[0].is_number_unsigned() || !Row[1].is_number_unsigned()) continue;
                    const size_t Def = Row[0].get<size_t>();
                    if (Def >= Defs.size() || !Defs[Def].is_object()) continue;
                    ConstantAt C;
                    C.Key = Defs[Def].value("key", std::string());
                    C.Size = Defs[Def].value("size", 0u);
                    C.Srgb = Defs[Def].value("transform", std::string()) == "srgb";
                    C.Offset = Row[1].get<uint32_t>();
                    if (!C.Key.empty()) List.push_back(std::move(C));
                }
                // The table is keyed by the 63-bit name hash; the zone readers hand over the low 60 bits (the pointer's key mask).
                // Keyed by those, a hash that two entries share would be ambiguous and gets no layout.
                const uint64_t Key = std::stoull(Hex, nullptr, 16) & 0x0FFFFFFFFFFFFFFFull;
                if (!Ambiguous.insert(Key).second || Layouts.count(Key) != 0) Layouts.erase(Key);
                else Layouts[Key] = std::move(List);
            }
        }
    }
}

const Data& Shared()
{
    static const Data Loaded = [] {
        Data D;
        wchar_t Buf[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, Buf, static_cast<DWORD>(std::size(Buf)));
        std::error_code Ec;
        // <exe dir>\zonekit\profiles, else a checkout above it
        for (fs::path P = fs::path(Buf).parent_path(); !P.empty(); P = P.parent_path())
        {
            const fs::path Profiles = P / "zonekit" / "profiles";
            if (fs::exists(Profiles / "t7" / "target" / "techsets.json", Ec))
            {
                D.Load(Profiles);
                D.LoadNames(fs::path(Buf).parent_path() / "package_index" / "cod_techsets.wni");
                return D;
            }
            if (P == P.parent_path()) break;
        }
        D.Load(fs::path(Buf).parent_path() / "zonekit" / "profiles");   // nothing found: the problems say which files
        D.LoadNames(fs::path(Buf).parent_path() / "package_index" / "cod_techsets.wni");
        return D;
    }();
    return Loaded;
}

const char* RuleName(Rule R)
{
    switch (R)
    {
    case Rule::Override: return "override";
    case Rule::Name: return "name";
    case Rule::Normalized: return "normalized";
    case Rule::Sky: return "sky";
    case Rule::Family: return "family";
    case Rule::SlotSet: return "slot_set";
    case Rule::SlotSuperset: return "slot_superset";
    case Rule::RenderClass: return "render_class";
    case Rule::Default: return "default";
    }
    return "default";
}

bool IsExact(Rule R)
{
    return R == Rule::Name || R == Rule::Family || R == Rule::SlotSet;
}

bool UsableName(std::string_view Name)
{
    if (Name.empty() || Name.rfind("xtechset_", 0) == 0) return false;
    return !(Name.size() >= 8 && IsHex(Name));   // a bare hash ("2ce22f911a30ce4")
}

bool Accepts(const Slot& S, const std::string& Intent)
{
    if (S.Semantic.empty()) return true;
    const std::vector<std::string> List = SplitList(S.Semantic);
    if (InList(List, Intent)) return true;
    return Intent == "specularMap" && InList(List, "specularMask");
}

std::string SemanticFor(const Slot& S, const std::string& Intent)
{
    if (S.Semantic.empty()) return Intent;
    const std::vector<std::string> List = SplitList(S.Semantic);
    if (InList(List, Intent)) return Intent;
    if (Intent == "specularMap" && InList(List, "specularMask")) return "specularMask";
    return List.empty() ? Intent : List.front();
}

Form FormOf(const std::string& Semantic)
{
    if (Semantic == "diffuseMap") return { "sRGB3chAlpha", "compressed no alpha" };
    if (Semantic == "normalMap") return { "Normal", "compressed" };
    if (Semantic == "specularMap") return { "sRGB3ch", "compressed" };
    if (Semantic == "effectMap") return { "sRGB3chAlpha", "compressed" };
    if (Semantic == "glossMap" || Semantic == "occlusionMap" || Semantic == "thicknessMap" || Semantic == "revealMap" ||
        Semantic == "specularMask" || Semantic == "One Channel")
        return { "Linear1ch", "compressed" };
    return { "sRGB3chAlpha", "compressed" };
}

std::string FloatText(float V)
{
    char B[48];
    std::snprintf(B, sizeof(B), "%g", static_cast<double>(V));
    return B;
}

std::string TintComponentText(float Linear)
{
    double L = std::isfinite(Linear) ? static_cast<double>(Linear) : 1.0;
    L = std::clamp(L, 0.0, 1.0);
    const double S = L <= 0.0031308 ? 12.92 * L : 1.055 * std::pow(L, 1.0 / 2.4) - 0.055;
    const int K = static_cast<int>(std::lround(std::clamp(S, 0.0, 1.0) * 255.0));
    if (K <= 0) return "0";
    if (K >= 255) return "1";
    const double V = K / 255.0;
    const double Scale = std::pow(10.0, 5 - static_cast<int>(std::floor(std::log10(V))));
    char B[48];
    std::snprintf(B, sizeof(B), "%.6g", std::ceil(V * Scale - 1e-7) / Scale);
    return B;
}

bool ClassifyRender(int64_t Flags, Blend& Out, uint32_t& Traits)
{
    // HydraX TreyarchRenderFlags.Classify; the bits were measured on the BO4 and Cold War techsets, not documented
    if (Flags <= 0) return false;
    const uint32_t F = static_cast<uint32_t>(Flags);
    constexpr uint32_t Opaque = 0x4, DecalBit = 0x8, DoubleSided = 0x10, Transparent = 0x20, EmissiveBit = 0x40, GlassBit = 0x1000, EffectBit = 0x4000, DistortionBit = 0x8000, Occluder = 0x10000;
    Traits = 0;
    if (F & DoubleSided) Traits |= Trait::TwoSided;
    if (F & EmissiveBit) Traits |= Trait::Emissive;
    if ((F & DecalBit) != 0 && (F & Opaque) == 0)
    {
        Out = (F & EmissiveBit) != 0 ? Blend::Add : Blend::Decal;
        Traits |= Trait::DecalState;
        return true;
    }
    if (F & Transparent)
    {
        if (F & DistortionBit) { Out = Blend::Premul; Traits |= Trait::Distortion | Trait::Effect; }
        else if (F & EffectBit) { Out = Blend::Premul; Traits |= Trait::Effect; }
        else if (F & GlassBit) { Out = Blend::Premul; Traits |= Trait::Glass; }
        else Out = Blend::Blend;
        return true;
    }
    if (F & Opaque)
    {
        Out = (F & DoubleSided) != 0 && (F & Occluder) == 0 ? Blend::AlphaTest : Blend::Opaque;
        return true;
    }
    return false;
}

bool RenderMatches(Blend WantedBlend, uint32_t WantedTraits, const Techset& T)
{
    if (WantedBlend == Blend::Premul)
    {
        const bool Glass = (WantedTraits & Trait::Glass) != 0;
        return T.Class == Blend::Premul && Glass == T.HasTrait(Trait::Glass) &&
               (Glass || ((WantedTraits & Trait::Distortion) != 0) == T.HasTrait(Trait::Distortion));
    }
    return T.Class == WantedBlend;
}

namespace
{
    // One pass of the rules. UseRender false is HydraX's second pass with no render class (only lit techsets are candidates).
    Choice Pick(const Data& D, const Request& R, bool UseRender)
    {
        Choice Out;
        const auto Make = [&](const Techset* T, Rule Used, std::string Why) {
            Out.Chosen = T;
            Out.Used = Used;
            Out.Reason = std::move(Why);
            return Out;
        };
        const bool FromFlags = R.RenderFromFlags;                 // the game states its class in flag words (BO4): HydraX's render class
        const bool HaveRender = FromFlags && R.HasRender && UseRender;
        const auto Compatible = [&](const Techset& T) {
            for (const auto& [Key, Intent] : R.Wanted)
                if (const Slot* S = T.ByKey(Key); S != nullptr && !Accepts(*S, Intent)) return false;
            return true;
        };
        const auto VegetationOk = [&](const std::string& N) {
            return !IsVegetation(N) || (FromFlags ? HaveRender && R.RenderBlend == Blend::AlphaTest : R.Class == Blend::AlphaTest);
        };
        // HydraX Allowed: with a render class the techset must match it, without one only lit techsets are candidates
        const auto ClassOk = [&](const Techset& T) {
            // a material that does not glow never gets a glowing techset (emissive_decal for a plain decal), whether or not its class is known
            // (lit_transition_emissive is a lit_ name); R.Emissive: the material has an emissive image
            const bool Glows = (HaveRender && (R.RenderTraits & Trait::Emissive) != 0) || R.Emissive;
            if (T.HasTrait(Trait::Emissive) && !Glows) return false;
            if (!HaveRender) return IsLit(T.Name);
            return VegetationOk(T.Name) && RenderMatches(R.RenderBlend, R.RenderTraits, T);
        };
        const auto NotCamo = [](const std::string& N) { return N.find("camo") == std::string::npos; };
        const bool Named = UsableName(R.SourceTechset);
        const std::string& Source = R.SourceTechset;
        // the material's slot hashes that some catalog techset knows (HydraX: a BO4-only slot is ignored)
        std::set<uint32_t> KnownMaterial;
        for (const uint32_t H : R.SlotHashes)
            if (D.KnownHash(H)) KnownMaterial.insert(H);
        const bool ByHash = !R.SlotHashes.empty();
        const auto TextureHashes = [](const Techset& T) {
            std::set<uint32_t> Hashes;
            for (const Slot& S : T.Slots) Hashes.insert(S.Hash);
            return Hashes;
        };
        // HydraX candidate Slots / ImageSlots: only the textures that have a material GDT key
        const auto KeyedHashes = [](const Techset& T) {
            std::set<uint32_t> Hashes;
            for (const Slot& S : T.Slots)
                if (!S.Key.empty()) Hashes.insert(S.Hash);
            return Hashes;
        };
        // HydraX CompatibleCount: of the material's known slots, how many the techset's slot accepts (a slot the material has no wish for, or the techset has no key for, counts)
        const auto CompatibleCount = [&](const Techset& T) {
            int Count = 0;
            for (const uint32_t H : KnownMaterial)
            {
                const Slot* Own = nullptr;
                for (const Slot& S : T.Slots)
                    if (S.Hash == H && !S.Key.empty()) { Own = &S; break; }
                bool Ok = true;
                // the material's image in texture H is what the texture's usual key says it is (colorMap: the colour image), not what this
                // techset happens to key the texture as: HydraX compares the image's semantic with what the techset expects at that hash
                if (Own != nullptr)
                {
                    const std::string Usual = D.DefaultKeyOfHash(H);
                    for (const auto& [Key, Intent] : R.Wanted)
                        if (Key == Usual && !Accepts(*Own, Intent)) { Ok = false; break; }
                }
                if (Ok) ++Count;
            }
            return Count;
        };
        const std::string Family = Named ? FamilyOf(Source) : std::string();
        const std::string Target = Family.empty() ? std::string() : FamilyTarget(D, Family);

        // 1. Override
        if (Named)
            if (const std::string* To = D.OverrideFor(Source))
                if (const Techset* T = D.Find(*To); T != nullptr && NotCamo(T->Name) && VegetationOk(T->Name) && Compatible(*T))
                    return Make(T, Rule::Override, std::format("The rule table maps {} to {}.", Source, T->Name));

        if (Named)
        {
            // 2. Exact name (HydraX keeps out the camo techsets only)
            if (const Techset* T = D.Find(Source); T != nullptr && NotCamo(Source) && VegetationOk(Source) && Compatible(*T))
                return Make(T, Rule::Name, std::format("Techset {} is in the Black Ops III catalog.", Source));
            // 3. Normalized
            if (Source.rfind("t8_", 0) == 0)
            {
                const std::string Bare = Source.substr(3);
                if (const Techset* T = D.Find(Bare); T != nullptr && NotCamo(Bare) && VegetationOk(Bare) && Compatible(*T))
                    return Make(T, Rule::Normalized, std::format("Techset {} is not in the Black Ops III catalog; {} is.", Source, Bare));
            }
            // 4. Sky
            if (const std::string Sky = BareSky(Source); !Sky.empty())
                if (const Techset* T = D.Find(Sky); T != nullptr && NotCamo(Sky) && Compatible(*T))
                    return Make(T, Rule::Normalized, std::format("Techset {} is the Black Ops III sky techset {}.", Source, Sky));
            // 5. Family (a BO4 name with its folder and variant hash): the type has a slot for every slot the material fills, and its class
            if (!Target.empty())
            {
                const Techset* T = D.Find(Target);
                // with the slot hashes (BO4, Cold War) the type needs a texture for every KNOWN hash of the material, as HydraX; else for every wanted key
                const auto Covers = [&](const Techset& Type) {
                    if (!KnownMaterial.empty())
                        return std::all_of(KnownMaterial.begin(), KnownMaterial.end(), [&](uint32_t H) { return std::any_of(Type.Slots.begin(), Type.Slots.end(), [&](const Slot& S) { return S.Hash == H && !S.Key.empty(); }); });     // HydraX: the keyed ImageSlots
                    return std::all_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return Type.ByKey(W.first) != nullptr; });
                };
                if (T != nullptr && IsMappable(Target) && VegetationOk(Target) && (!HaveRender || RenderMatches(R.RenderBlend, R.RenderTraits, *T)) && Compatible(*T) && Covers(*T))
                    return Make(T, Rule::Family, std::format("bo4 techset family: {} is of family {}, the Black Ops III type {}; it has a slot for each of the {} image slots.",
                                                              Source, Family, Target, R.Wanted.size()));
            }
        }

        // 6 to 8 work from the slots the description fills.
        const auto Base = [](const std::string& Key) { return Key == "heatmap" || Key == "colorMap" || Key == "normalMap"; };
        const auto Supports = [&](const Techset& T, const std::pair<std::string, std::string>& W) {
            const Slot* S = T.ByKey(W.first);
            return S != nullptr && Accepts(*S, W.second);
        };
        const auto Mismatch = [&](const Techset& T) {
            // a colour specular map on a scalar slot (or the reverse) is the worse of two candidates
            return R.HasColorSpec && T.HasTrait(Trait::SpecColorMap) && T.HasTrait(Trait::ColorSpec) != R.ColorSpec ? 1 : 0;
        };
        // 0 for the family's type, 1 for a variant of it, 2 otherwise (HydraX FamilyRank)
        const auto FamilyRank = [&](const std::string& Name) {
            if (Target.empty()) return 2;
            if (Name == Target) return 0;
            const std::string Stem = Family == "lit_micro_tile_mix" ? Target : Family;
            return Name.rfind(Stem + "_", 0) == 0 ? 1 : 2;
        };
        const char* const LevelNote[] = { "", " The emissive trait was let go: no techset of the class has it with these slots.",
                                          " The render class was let go: no techset of that class has these slots (lit techsets of any class)." };
        const int Levels = FromFlags ? 1 : 3;
        for (int Level = 0; Level < Levels; ++Level)
        {
            std::vector<const Techset*> Pool;
            for (const Techset& T : D.All())
            {
                if (T.Traits & Trait::Camo) continue;
                if (FromFlags)
                {
                    // HydraX keeps the _cc (Customize) techsets and so does this; camo ones stay out (the Mod Tools cannot register them).
                    // Deviation from HydraX, which has no category test: techsets of the Weapons category (lit_paintshop) and distortion techsets
                    // (lit_emissive_clone, Specialty) are weapon or special-effect only, so a material needs to ask for them.
                    if (T.Category == "Weapons") continue;
                    if (T.HasTrait(Trait::Distortion) && !(HaveRender && (R.RenderTraits & Trait::Distortion))) continue;
                    if (!IsMappable(T.Name) || !ClassOk(T)) continue;
                }
                else
                {
                    // a surface techset: colour and normal slots (not emissive_passthrough or lit_emissive_clone, which a material with few images would reach)
                    if (T.ByKey("colorMap") == nullptr || T.ByKey("normalMap") == nullptr) continue;
                    if (Level < 2)
                    {
                        if (!IsMappable(T.Name) || T.Class != R.Class || !VegetationOk(T.Name)) continue;
                        if (Level == 0 && T.HasTrait(Trait::Emissive) != R.Emissive) continue;
                    }
                    else if (!IsLit(T.Name)) continue;
                }
                Pool.push_back(&T);
            }
            const size_t Need = R.Wanted.size();
            // 6. exact slot set. With the material's slot hashes (BO4): the hashes are exactly the texture names of the techset.
            // Without them: every slot is there and the techset holds nothing else but the base slots.
            {
                std::vector<std::tuple<int, int, size_t, size_t, std::string, const Techset*>> Hit;
                const std::set<uint32_t> Material(R.SlotHashes.begin(), R.SlotHashes.end());
                for (const Techset* T : Pool)
                {
                    if (Material.empty() && R.Wanted.empty()) break;      // nothing is asked for: nothing is an exact set (Default)
                    if (!Compatible(*T)) continue;
                    if (!Material.empty())
                    {
                        std::set<uint32_t> Texture;
                        for (const Slot& S : T->Slots) Texture.insert(S.Hash);
                        if (Texture != Material) continue;
                    }
                    else
                    {
                        if (!std::all_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return Supports(*T, W); })) continue;
                        bool Extra = false;
                        for (const Slot& S : T->Slots)
                            if (!S.Key.empty() && !Base(S.Key) && std::none_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return W.first == S.Key; })) Extra = true;
                        if (Extra) continue;
                    }
                    Hit.emplace_back(Mismatch(*T), FamilyRank(T->Name), std::numeric_limits<size_t>::max() - T->KeyedCount(), T->Name.size(), T->Name, T);
                }
                if (!Hit.empty())
                {
                    std::sort(Hit.begin(), Hit.end());
                    const Techset* T = std::get<5>(Hit.front());
                    return Make(T, Rule::SlotSet, std::format("The material's image slots ({}) are exactly the slots of {}.{}{}", KeysText(R.Wanted), T->Name,
                                                             Hit.size() > 1 ? std::format(" {} techsets share this set; the one with the most image slots, then the shortest name is taken.", Hit.size()) : "",
                                                             LevelNote[Level]));
                }
            }
            // 7. slot superset: every slot is there, the fewest extras
            {
                std::vector<std::tuple<int, int, size_t, size_t, std::string, const Techset*>> Hit;
                for (const Techset* T : Pool)
                {
                    if (ByHash)
                    {
                        // every known hash is a texture of the techset; the extras are the textures beyond them (HydraX)
                        if (KnownMaterial.empty() || !Compatible(*T)) continue;
                        const std::set<uint32_t> Has = KeyedHashes(*T);
                        if (!std::all_of(KnownMaterial.begin(), KnownMaterial.end(), [&](uint32_t H) { return Has.count(H) != 0; })) continue;
                        Hit.emplace_back(Mismatch(*T), FamilyRank(T->Name), Has.size() - KnownMaterial.size(), T->Name.size(), T->Name, T);
                    }
                    else if (!R.Wanted.empty() && std::all_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return Supports(*T, W); }))
                        Hit.emplace_back(Mismatch(*T), FamilyRank(T->Name), T->KeyedCount() - Need, T->Name.size(), T->Name, T);
                }
                if (!Hit.empty())
                {
                    std::sort(Hit.begin(), Hit.end());
                    const Techset* T = std::get<5>(Hit.front());
                    return Make(T, Rule::SlotSuperset, std::format("{} covers the material's {} image slots ({}) with {} more.{}", T->Name, Need, KeysText(R.Wanted),
                                                                   T->KeyedCount() - Need, LevelNote[Level]));
                }
            }
            // 8. render-class overlap: the candidate that keeps the most of the slots (the rest are reported as unmapped).
            // With a game-stated class this is only for a material that has one (HydraX).
            if (!FromFlags || HaveRender)
            {
                std::vector<std::tuple<int, int, size_t, size_t, size_t, std::string, const Techset*>> Hit;
                for (const Techset* T : Pool)
                {
                    if (ByHash)
                    {
                        const std::set<uint32_t> Has = KeyedHashes(*T);
                        const size_t Shared = static_cast<size_t>(std::count_if(KnownMaterial.begin(), KnownMaterial.end(), [&](uint32_t H) { return Has.count(H) != 0; }));
                        if (Shared == 0) continue;
                        Hit.emplace_back(Mismatch(*T), -CompatibleCount(*T), KnownMaterial.size() - Shared, Has.size() - Shared, T->Name.size(), T->Name, T);
                        continue;
                    }
                    const size_t Shared = static_cast<size_t>(std::count_if(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return Supports(*T, W); }));
                    if (Shared == 0) continue;
                    Hit.emplace_back(Mismatch(*T), 0, Need - Shared, T->KeyedCount() - Shared, T->Name.size(), T->Name, T);
                }
                if (!Hit.empty())
                {
                    std::sort(Hit.begin(), Hit.end());
                    const Techset* T = std::get<6>(Hit.front());
                    return Make(T, Rule::RenderClass, std::format("No techset of the class has all of the {} image slots ({}); {} keeps {} of them.{}", Need,
                                                                  KeysText(R.Wanted), T->Name, Need - std::get<2>(Hit.front()), LevelNote[Level]));
                }
            }
        }

        // 9. Default: lit, which holds colour and normal only. Reached only when no catalog techset shares any of the slots.
        if (const Techset* T = D.Find("lit"))
            return Make(T, Rule::Default, std::format("No catalog techset has any of the image slots ({}); lit is the default.", KeysText(R.Wanted)));
        return Out;
    }
}

bool ClassFromName(const Data& D, const std::string& Name, Blend& Out, uint32_t& Traits)
{
    if (Name.empty()) return false;
    const Techset* T = D.Find(Name);
    if (T == nullptr && Name.rfind("t8_", 0) == 0) T = D.Find(Name.substr(3));
    if (T == nullptr)
        if (const std::string Sky = BareSky(Name); !Sky.empty()) T = D.Find(Sky);
    if (T == nullptr)
        if (const std::string Family = FamilyOf(Name); !Family.empty())
            if (const std::string Target = FamilyTarget(D, Family); !Target.empty()) T = D.Find(Target);
    if (T == nullptr || T->Class == Blend::Other) return false;
    Out = T->Class;
    constexpr uint32_t Kept = Trait::TwoSided | Trait::DecalState | Trait::Emissive | Trait::Distortion | Trait::Effect | Trait::Glass;
    Traits = T->Traits & Kept;
    return true;
}

Choice Choose(const Data& D, const Request& R)
{
    if (!D.HasCatalog()) return Choice();
    Choice C = Pick(D, R, true);
    // HydraX: the render class narrows the slot rules; when it leaves nothing the lit rules of old apply, so a misread flag word never costs a material its techset
    if (R.RenderFromFlags && R.HasRender && C.Used == Rule::Default) return Pick(D, R, false);
    return C;
}

int SelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Condition, const char* Name, const std::string& Got) {
        if (Condition) return;
        ++Failed;
        Failures += std::format("FAIL Techsets {} (got '{}')\n", Name, Got);
    };

    // the slot hash: the oracles of the BO3 catalog, and the underscore case
    Check(SlotHash("colorMap") == 0xA0AB1041u && SlotHash("normalMap") == 0x59D30D0Fu && SlotHash("glossMap") == 0x6D0A6C98u &&
          SlotHash("specColorMap") == 0xEC443804u && SlotHash("aoMap") == 0x07176BF2u, "hash: the catalog oracles", std::format("{:08X}", SlotHash("colorMap")));
    // BO1 and BO2 name their normal and diffuse maps by the same hash of "Normal_Map" and "Diffuse_Map" (Games/T5, Games/T6): '_' moves to 0x7F
    Check(SlotHash("Normal_Map") == 0x942CBFF0u && SlotHash("Diffuse_Map") == 0xF039EC2Du, "hash: an underscore becomes 0x7F",
          std::format("{:08X} {:08X}", SlotHash("Normal_Map"), SlotHash("Diffuse_Map")));

    // colour text: k/255 rounded up to 6 digits so APE shows k
    Check(TintComponentText(1.0f) == "1" && TintComponentText(0.0f) == "0", "tint: the ends", TintComponentText(1.0f));
    {
        const auto ToLinear = [](int K) {
            const double S = K / 255.0;
            return static_cast<float>(S <= 0.04045 ? S / 12.92 : std::pow((S + 0.055) / 1.055, 2.4));
        };
        bool Ok = true;
        std::string Bad;
        for (int K = 1; K < 255; ++K)
        {
            const double V = std::stod(TintComponentText(ToLinear(K)));
            if (static_cast<int>(std::floor(V * 255.0)) != K || V < K / 255.0)
            {
                Ok = false;
                Bad = std::format("k={} text={}", K, TintComponentText(ToLinear(K)));
                break;
            }
        }
        Check(Ok, "tint: APE shows k for every k", Bad);
        Check(TintComponentText(ToLinear(51)) == "0.2", "tint: 51/255 is 0.2", TintComponentText(ToLinear(51)));
    }

    // a small catalog for the rules that need none of the shipped data
    const std::string Mini = R"({"version":1,"techsets":[
        {"n":"lit","c":"Geometry","f":"","b":"Opaque","t":8192,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["normalMap","normalMap","normalMap","$identitynormalmap"],["thermalHeatmap","heatmap","One Channel","$gray_32_one_channel"]]},
        {"n":"lit_plus","c":"Geometry Plus","f":"","b":"Opaque","t":8192,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["normalMap","normalMap","normalMap","$identitynormalmap"],["thermalHeatmap","heatmap","One Channel","$gray_32_one_channel"],["aoMap","occMap","occlusionMap","$white_ao"],["glossMap","cosinePowerMap","glossMap","$white_gloss"]]},
        {"n":"lit_weapon_camo","c":"Geometry","f":"","b":"Opaque","t":8192,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["normalMap","normalMap","normalMap","$identitynormalmap"],["aoMap","occMap","occlusionMap","$white_ao"],["glossMap","cosinePowerMap","glossMap","$white_gloss"]]}]})";
    {
        Data M;
        Check(M.LoadCatalogText(Mini) && M.HasCatalog(), "mini catalog loads", "");
        Request Rq;
        Rq.SourceTechset = "lit_weapon_camo";   // the game names it
        Rq.Wanted = { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" }, { "occMap", "occlusionMap" }, { "cosinePowerMap", "glossMap" } };
        const Choice C = Choose(M, Rq);
        Check(C.Chosen != nullptr && C.Chosen->Name == "lit_plus" && !C.Exact() == false, "camo: never chosen, even when named (mini catalog)", C.Chosen ? C.Chosen->Name : "(null)");
        Rq.SourceTechset = "lit_plus";
        const Choice N = Choose(M, Rq);
        Check(N.Chosen != nullptr && N.Chosen->Name == "lit_plus" && N.Used == Rule::Name, "an exact name is taken", N.Chosen ? N.Chosen->Name : "(null)");
        Rq.SourceTechset = "2ce22f911a30ce4";   // a hash is no name: slots decide
        const Choice H = Choose(M, Rq);
        Check(H.Chosen != nullptr && H.Chosen->Name == "lit_plus" && H.Used == Rule::SlotSet, "a hash goes straight to slot matching", H.Chosen ? std::string(RuleName(H.Used)) : "(null)");
    }

    // the rules of the mapper, one case each plus the near miss that must not take the rule (a small catalog, so the answers are known)
    {
        const auto Sl = [](const char* Name, const char* Key, const char* Semantic, const char* Default) {
            return std::string("[\"") + Name + "\",\"" + Key + "\",\"" + Semantic + "\",\"" + Default + "\"]";
        };
        const std::string Col = Sl("colorMap", "colorMap", "diffuseMap", "$white_diffuse"), Nor = Sl("normalMap", "normalMap", "normalMap", "$identitynormalmap"),
                          Heat = Sl("thermalHeatmap", "heatmap", "One Channel", "$gray_32_one_channel"), Ao = Sl("aoMap", "occMap", "occlusionMap", "$white_ao"),
                          Gl = Sl("glossMap", "cosinePowerMap", "glossMap", "$white_gloss"), Sp = Sl("specColorMap", "specColorMap", "specularMap", "$specular");
        const auto T = [&](const char* Name, const char* Blend, uint32_t Traits, std::initializer_list<const std::string*> Slots) {
            std::string Out = std::string("{\"n\":\"") + Name + "\",\"c\":\"Geometry\",\"f\":\"\",\"b\":\"" + Blend + "\",\"t\":" + std::to_string(Traits) + ",\"s\":[";
            bool First = true;
            for (const std::string* S : Slots) { Out += (First ? "" : ",") + *S; First = false; }
            return Out + "]}";
        };
        const std::string Json = "{\"version\":1,\"overrides\":{\"forcedname\":\"lit_plus\",\"forcedcamo\":\"lit_camo_x\"},\"techsets\":[" +
            T("lit", "Opaque", 8192, { &Col, &Nor, &Heat }) + "," +
            T("lit_plus", "Opaque", 8192, { &Col, &Nor, &Heat, &Ao, &Gl }) + "," +
            T("lit_plus_extra", "Opaque", 8192, { &Col, &Nor, &Heat, &Ao, &Gl, &Sp }) + "," +
            T("lit_zz", "Opaque", 8192, { &Col, &Nor, &Heat, &Ao, &Gl, &Sp }) + "," +
            T("lit_decal", "Decal", 8194, { &Col, &Nor }) + "," +
            T("lit_decal_plus", "Decal", 8194, { &Col, &Nor, &Ao }) + "," +
            T("lit_alphatest", "AlphaTest", 8192, { &Col, &Nor }) + "," +
            T("vegetation_a", "AlphaTest", 8192, { &Col, &Nor }) + "," +
            T("glass_x", "Premul", 9216, { &Col, &Nor }) + "," +
            T("lit_micro_tile_blend", "Opaque", 8192, { &Col, &Nor, &Ao }) + "," +
            T("lit_camo_x", "Opaque", 8192 + 32768, { &Col, &Nor, &Ao }) + "," +
            "{\"n\":\"sky_latlong_hdr\",\"c\":\"Sky\",\"f\":\"\",\"b\":\"Other\",\"t\":0,\"s\":[" + Col + "]}]}";
        Data M;
        Check(M.LoadCatalogText(Json) && M.HasCatalog(), "rules catalog loads", "");
        const auto Pair = [](const char* K, const char* I) { return std::pair<std::string, std::string>(K, I); };
        const auto Ask = [&](const char* Source, std::vector<std::pair<std::string, std::string>> Wanted) {
            Request Rq;
            Rq.SourceTechset = Source;
            Rq.Wanted = std::move(Wanted);
            return Rq;
        };
        const auto Nm = [](const Choice& C) { return C.Chosen ? C.Chosen->Name : std::string("(null)"); };
        const auto Got = [&](const Choice& C) { return Nm(C) + "/" + RuleName(C.Used); };
        const std::vector<std::pair<std::string, std::string>> CN = { Pair("colorMap", "diffuseMap"), Pair("normalMap", "normalMap") };
        const auto With = [&](std::initializer_list<std::pair<std::string, std::string>> More) {
            std::vector<std::pair<std::string, std::string>> W = CN;
            W.insert(W.end(), More.begin(), More.end());
            return W;
        };
        // 1. Override: first, and a camo target is refused
        {
            const Choice C = Choose(M, Ask("forcedname", CN));
            Check(Nm(C) == "lit_plus" && C.Used == Rule::Override && !C.Exact(), "rule 1 override: the table wins over the slots, and is a fallback", Got(C));
            const Choice K = Choose(M, Ask("forcedcamo", CN));
            Check(K.Chosen != nullptr && Nm(K).find("camo") == std::string::npos && K.Used != Rule::Override, "rule 1 override: a camo target is never taken", Got(K));
            Check(Choose(M, Ask("lit_plus", CN)).Used == Rule::Name, "rule 1 override: no table row, no override", "");
        }
        // 2. Exact name: taken whatever the slots, unless an image's meaning is refused; exact
        {
            const Choice C = Choose(M, Ask("lit_plus", CN));
            Check(Nm(C) == "lit_plus" && C.Used == Rule::Name && C.Exact(), "rule 2 exact name: taken, counts as exact", Got(C));
            const Choice R = Choose(M, Ask("lit_plus", { Pair("colorMap", "normalMap"), Pair("normalMap", "normalMap") }));
            Check(R.Used != Rule::Name, "rule 2 exact name: an image of the wrong semantic in a slot refuses the name", Got(R));
            const Choice Cam = Choose(M, Ask("lit_camo_x", CN));
            Check(Nm(Cam).find("camo") == std::string::npos, "rule 2 exact name: a camo name is refused", Got(Cam));
        }
        // 3. Normalized: t8_ prefix
        {
            const Choice C = Choose(M, Ask("t8_lit_plus", CN));
            Check(Nm(C) == "lit_plus" && C.Used == Rule::Normalized && !C.Exact(), "rule 3 normalized: t8_ prefix cut, a fallback", Got(C));
            Check(Choose(M, Ask("t8_lit_nothing", CN)).Used != Rule::Normalized, "rule 3 normalized: a bare name the catalog lacks is not taken", "");
        }
        // 4. Sky: folder and variant hash cut, only sky_ names
        {
            const Choice C = Choose(M, Ask("mc/sky_latlong_hdr#99fe632f", { Pair("colorMap", "diffuseMap") }));
            Check(Nm(C) == "sky_latlong_hdr" && C.Used == Rule::Normalized, "rule 4 sky: the bare sky techset", Got(C));
            const Choice L = Choose(M, Ask("mc/lit_nothing#99fe632f", { Pair("colorMap", "diffuseMap") }));
            Check(Nm(L) != "sky_latlong_hdr", "rule 4 sky: a lit name is never read as sky", Got(L));
        }
        // 5. Family: alias target, with a slot for every wanted key; exact
        {
            const Choice C = Choose(M, Ask("mc/lit_micro_tile_mix#4cce5c8c", With({ Pair("occMap", "occlusionMap") })));
            Check(Nm(C) == "lit_micro_tile_blend" && C.Used == Rule::Family && C.Exact(), "rule 5 family: lit_micro_tile_mix is the blend, exact", Got(C));
            const Choice L = Choose(M, Ask("mc/lit_micro_tile_mix#4cce5c8c", With({ Pair("occMap", "occlusionMap"), Pair("cosinePowerMap", "glossMap") })));
            Check(L.Used != Rule::Family, "rule 5 family: a wanted key the type lacks refuses the family", Got(L));
        }
        // 6. Exact slot set by slot hash (BO4 and Cold War): the hashes are exactly the texture names
        {
            Request Rq = Ask("", With({ Pair("occMap", "occlusionMap"), Pair("cosinePowerMap", "glossMap") }));
            Rq.RenderFromFlags = true;
            Rq.HasRender = true;
            Rq.RenderBlend = Blend::Opaque;
            Rq.SlotHashes = { SlotHash("colorMap"), SlotHash("normalMap"), SlotHash("thermalHeatmap"), SlotHash("aoMap"), SlotHash("glossMap") };
            const Choice C = Choose(M, Rq);
            Check(Nm(C) == "lit_plus" && C.Used == Rule::SlotSet && C.Exact(), "rule 6 slot set: the hashes are the techset's texture names", Got(C));
            Rq.SlotHashes.push_back(SlotHash("someBo4OnlyMap"));
            const Choice X = Choose(M, Rq);
            Check(X.Used != Rule::SlotSet, "rule 6 slot set: one hash the techset does not have is no exact set", Got(X));
            Rq.SlotHashes.pop_back();
            Rq.SlotHashes.pop_back();
            const Choice Y = Choose(M, Rq);
            Check(Y.Used != Rule::SlotSet || Nm(Y) != "lit_plus", "rule 6 slot set: a hash short of the techset's textures is no exact set", Got(Y));
            // the tie goes to the family type, then the techset with more slots, then the shorter name
            Request Tie = Ask("mc/lit_plus#1", With({ Pair("occMap", "occlusionMap"), Pair("cosinePowerMap", "glossMap"), Pair("specColorMap", "specularMap") }));
            Tie.RenderFromFlags = true;
            Tie.HasRender = true;
            Tie.RenderBlend = Blend::Opaque;
            Tie.SlotHashes = { SlotHash("colorMap"), SlotHash("normalMap"), SlotHash("thermalHeatmap"), SlotHash("aoMap"), SlotHash("glossMap"), SlotHash("specColorMap") };
            const Choice T2 = Choose(M, Tie);
            Check(Nm(T2) == "lit_plus_extra" && T2.Used == Rule::SlotSet, "rule 6 slot set: of two techsets with the set, the family's variant wins over the shorter name", Got(T2));
            Tie.SourceTechset = "";
            const Choice T3 = Choose(M, Tie);
            Check(Nm(T3) == "lit_zz" && T3.Used == Rule::SlotSet, "rule 6 slot set: no family, the shorter name wins", Got(T3));
        }
        // 7. Superset: every slot, the fewest extras (no hashes)
        {
            const Choice C = Choose(M, Ask("", With({ Pair("cosinePowerMap", "glossMap") })));
            Check(Nm(C) == "lit_plus" && C.Used == Rule::SlotSuperset && !C.Exact(), "rule 7 superset: the fewest extra slots, a fallback", Got(C));
        }
        // 8. Render class: nothing has every slot, the class keeps the most of them
        {
            Request Rq = Ask("", With({ Pair("occMap", "occlusionMap"), Pair("cosinePowerMap", "glossMap") }));
            Rq.RenderFromFlags = true;
            Rq.HasRender = true;
            Rq.RenderBlend = Blend::Decal;
            Rq.RenderTraits = Trait::DecalState;
            const Choice C = Choose(M, Rq);
            Check(Nm(C) == "lit_decal_plus" && C.Used == Rule::RenderClass && !C.Exact(), "rule 8 render class: a decal keeps the most slots among the decal techsets", Got(C));
        }
        // 9. Default
        {
            const Choice C = Choose(M, Ask("", { Pair("noSuchKey", "diffuseMap") }));
            Check(Nm(C) == "lit" && C.Used == Rule::Default && !C.Exact(), "rule 9 default: lit when nothing shares a slot", Got(C));
        }
        // the order: a name beats the slots; the render class is a filter on the slot rules, and unread means lit techsets only
        {
            Request Rq = Ask("lit_plus", CN);
            Rq.RenderFromFlags = true;
            Rq.HasRender = true;
            Rq.RenderBlend = Blend::Decal;
            Rq.SlotHashes = { SlotHash("colorMap"), SlotHash("normalMap") };
            Check(Choose(M, Rq).Used == Rule::Name, "order: the exact name beats the slot set", Got(Choose(M, Rq)));
            Request Dec = Ask("", CN);
            Dec.RenderFromFlags = true;
            Dec.HasRender = true;
            Dec.RenderBlend = Blend::Decal;
            Dec.SlotHashes = { SlotHash("colorMap"), SlotHash("normalMap") };
            Check(Nm(Choose(M, Dec)) == "lit_decal", "render class: a decal gets the decal techset with its slot set", Got(Choose(M, Dec)));
            Request Cut = Dec;
            Cut.RenderBlend = Blend::AlphaTest;
            Check(Nm(Choose(M, Cut)) == "lit_alphatest" || Nm(Choose(M, Cut)) == "vegetation_a", "render class: alpha test gets an alpha-test techset", Got(Choose(M, Cut)));
            Request Veg = Dec;
            Veg.RenderBlend = Blend::Opaque;
            Check(Nm(Choose(M, Veg)).rfind("vegetation", 0) != 0 && Nm(Choose(M, Veg)) != "lit_decal", "render class: an opaque material never gets vegetation or a decal", Got(Choose(M, Veg)));
            Request Gla = Dec;
            Gla.RenderBlend = Blend::Premul;
            Gla.RenderTraits = Trait::Glass;
            Check(Nm(Choose(M, Gla)) == "glass_x", "render class: glass gets the glass techset", Got(Choose(M, Gla)));
            Request Unread = Dec;
            Unread.HasRender = false;
            const Choice U = Choose(M, Unread);
            Check(U.Chosen != nullptr && IsLit(U.Chosen->Name) && Nm(U) != "vegetation_a" && Nm(U) != "glass_x" && Nm(U) != "lit_alphatest" && U.Used != Rule::RenderClass,
                  "unread class is no class: lit techsets only, never glass or vegetation, and no render-class rule", Got(U));
        }
        // ClassifyRender: HydraX's table
        {
            Blend B = Blend::Other;
            uint32_t Tr = 0;
            Check(!ClassifyRender(0, B, Tr) && !ClassifyRender(-1, B, Tr) && !ClassifyRender(0x2, B, Tr), "classify: 0, -1 and a word with no class bit say nothing", "");
            Check(ClassifyRender(0x80000a, B, Tr) && B == Blend::Decal && (Tr & Trait::DecalState), "classify: 0x80000a is a decal", BlendName(B));
            Check(ClassifyRender(0x800048, B, Tr) && B == Blend::Add && (Tr & Trait::Emissive), "classify: 0x800048 is an emissive decal (add)", BlendName(B));
            Check(ClassifyRender(0x4, B, Tr) && B == Blend::Opaque, "classify: opaque", BlendName(B));
            Check(ClassifyRender(0x14, B, Tr) && B == Blend::AlphaTest, "classify: opaque and double sided, no occluder, is alpha test", BlendName(B));
            Check(ClassifyRender(0x10014, B, Tr) && B == Blend::Opaque, "classify: with the occluder bit it is opaque", BlendName(B));
            Check(ClassifyRender(0x20, B, Tr) && B == Blend::Blend, "classify: transparent is blend", BlendName(B));
            Check(ClassifyRender(0x1020, B, Tr) && B == Blend::Premul && (Tr & Trait::Glass), "classify: transparent glass is premul glass", BlendName(B));
            Check(ClassifyRender(0x4020, B, Tr) && B == Blend::Premul && (Tr & Trait::Effect) && !(Tr & Trait::Glass), "classify: transparent effect is premul effect", BlendName(B));
            Check(ClassifyRender(0x8020, B, Tr) && (Tr & Trait::Distortion), "classify: transparent distortion", BlendName(B));
            Techset G;
            G.Class = Blend::Premul;
            G.Traits = Trait::Glass;
            Techset E;
            E.Class = Blend::Premul;
            Check(RenderMatches(Blend::Premul, Trait::Glass, G) && !RenderMatches(Blend::Premul, Trait::Glass, E) && RenderMatches(Blend::Premul, 0, E) && !RenderMatches(Blend::Premul, 0, G) &&
                  !RenderMatches(Blend::Premul, Trait::Distortion, E) && RenderMatches(Blend::Decal, 0, [] { Techset D; D.Class = Blend::Decal; return D; }()),
                  "matches: glass wants glass, effect wants effect, distortion is told apart, a decal wants a decal", "");
        }
    }


        // a code image is what the catalog slots that default to it say it is
        {
            Data CodeData;
            CodeData.LoadCatalogText(R"({"techsets":[
                {"n":"a","c":"G","f":"","b":"Opaque","t":0,"s":[["specColorMap","specColorMap","specularMask","$white_specular"]]},
                {"n":"b","c":"G","f":"","b":"Opaque","t":0,"s":[["specColorMap","specColorMap","specularMask","$white_specular"]]},
                {"n":"c","c":"G","f":"","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap","$white_specular"]]},
                {"n":"d","c":"G","f":"","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"]]}]})");
            const auto* Own = CodeData.CodeImageSemantics("$White_Specular");
            Check(Own != nullptr && Own->size() == 2 && Own->at("specularMask") == 2 && Own->at("diffuseMap") == 1 && CodeData.DominantCodeSemantic("$white_specular") == "specularMask",
                  "code image: semantics and counts come from the slots that default to it; the dominant one wins", Own ? std::to_string(Own->size()) : "(none)");
            Check(CodeData.CodeImageSemantics("$never_a_default") == nullptr && CodeData.DominantCodeSemantic("$never_a_default").empty() && CodeData.CodeImageSemantics("i_stock") == nullptr,
                  "code image: one no slot defaults to has no semantic here (never guessed); a stock name is no code image", "");
        }


    // names, the candidate pool, emissive, an empty request (B1 follow-up)
    {
        const auto Sl = [](const char* Name, const char* Key, const char* Semantic, const char* Default) {
            return std::string("[\"") + Name + "\",\"" + Key + "\",\"" + Semantic + "\",\"" + Default + "\"]";
        };
        const std::string Col = Sl("colorMap", "colorMap", "diffuseMap", "$white_diffuse"), Nor = Sl("normalMap", "normalMap", "normalMap", "$identitynormalmap"),
                          Ao = Sl("aoMap", "occMap", "occlusionMap", "$white_ao");
        const auto T = [&](const char* Name, const char* Category, const char* Blend, uint32_t Traits, std::initializer_list<const std::string*> Slots) {
            std::string Out = std::string("{\"n\":\"") + Name + "\",\"c\":\"" + Category + "\",\"f\":\"\",\"b\":\"" + Blend + "\",\"t\":" + std::to_string(Traits) + ",\"s\":[";
            bool First = true;
            for (const std::string* S : Slots) { Out += (First ? "" : ",") + *S; First = false; }
            return Out + "]}";
        };
        const std::string Json = "{\"version\":1,\"techsets\":[" +
            T("lit_decal", "Decal", "Decal", 2, { &Col, &Nor }) + "," +
            T("lit_pd", "Weapons", "Decal", 2, { &Col, &Nor }) + "," +                                    // shorter name, Weapons category
            T("emissive_decal", "Geometry", "Decal", 130, { &Col, &Nor }) + "," +                         // emissive decal, same textures
            T("lit_a", "Geometry", "Opaque", 8192, { &Col, &Nor, &Ao }) + "," +
            T("lit_emissive_clone", "Specialty", "Opaque", 8192 + 256, { &Col, &Nor, &Ao }) + "," +      // distortion, shorter than lit_a_cc
            T("lit_a_cc", "Character Customization", "Opaque", 8192 + 65536, { &Col, &Nor, &Ao }) + "," + // _cc: HydraX keeps it
            T("lit_a_camo", "Geometry", "Opaque", 8192 + 32768, { &Col, &Nor, &Ao }) + "," +
            T("lit", "Geometry", "Opaque", 8192, { &Col, &Nor }) + "]}";
        Data M;
        Check(M.LoadCatalogText(Json), "pool catalog loads", "");
        const auto Nm = [](const Choice& C) { return C.Chosen ? C.Chosen->Name : std::string("(null)"); };
        const auto Got = [&](const Choice& C) { return Nm(C) + "/" + RuleName(C.Used); };
        const auto Hashes = [](std::initializer_list<const char*> Names) {
            std::vector<uint32_t> H;
            for (const char* N : Names) H.push_back(SlotHash(N));
            return H;
        };
        const auto Base = [&](Blend B, uint32_t Traits) {
            Request Rq;
            Rq.RenderFromFlags = true;
            Rq.HasRender = true;
            Rq.RenderBlend = B;
            Rq.RenderTraits = Traits;
            Rq.Wanted = { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" } };
            Rq.SlotHashes = Hashes({ "colorMap", "normalMap" });
            return Rq;
        };
        // the pool: a Weapons-category techset is not taken for a decal that fits it exactly; the plain decal is
        Check(Nm(Choose(M, Base(Blend::Decal, Trait::DecalState))) == "lit_decal", "pool: lit_pd (Weapons) is never taken although shorter", Got(Choose(M, Base(Blend::Decal, Trait::DecalState))));
        // emissive: a plain decal does not get the emissive decal; one that glows may
        Check(Nm(Choose(M, Base(Blend::Decal, Trait::DecalState))) != "emissive_decal", "emissive: a non-emissive decal never gets emissive_decal", "");
        {
            Request Glow = Base(Blend::Decal, Trait::DecalState | Trait::Emissive);
            Glow.RenderBlend = Blend::Decal;
            const Choice C = Choose(M, Glow);
            Check(Nm(C) == "emissive_decal" || Nm(C) == "lit_decal", "emissive: a glowing decal may take the emissive decal", Got(C));
            Request Image = Base(Blend::Decal, Trait::DecalState);
            Image.Emissive = true;       // flags say no glow but the material has an emissive image: an emissive techset is not refused
            Image.SlotHashes = Hashes({ "colorMap", "normalMap" });
            Check(Choose(M, Image).Chosen != nullptr, "emissive: a material with an emissive image still gets a techset", Got(Choose(M, Image)));
        }
        // distortion: a distortion techset needs a distortion request; _cc stays a candidate (HydraX), camo does not
        {
            Request Rq = Base(Blend::Opaque, 0);
            Rq.Wanted.emplace_back("occMap", "occlusionMap");
            Rq.SlotHashes = Hashes({ "colorMap", "normalMap", "aoMap" });
            const Choice C = Choose(M, Rq);
            Check(Nm(C) == "lit_a" && C.Used == Rule::SlotSet, "pool: lit_emissive_clone (distortion) is not taken for an opaque material", Got(C));
            Request Cc = Rq;
            Cc.HasRender = false;      // an unread class: every lit_ techset is a candidate, the shortest name wins among equals
            const Choice D = Choose(M, Cc);
            Check(Nm(D) != "lit_emissive_clone" && Nm(D) != "lit_a_camo" && !D.Chosen->HasTrait(Trait::Camo), "pool: neither distortion nor camo is taken without a class", Got(D));
            // lit_a_cc is a candidate: remove the plain one and it is taken
            Data Only;
            std::string Cut = Json;
            const size_t At = Cut.find("{\"n\":\"lit_a\"");
            Cut.erase(At, Cut.find("},", At) - At + 2);
            Only.LoadCatalogText(Cut);
            const Choice E = Choose(Only, Rq);
            Check(Nm(E) == "lit_a_cc", "pool: a _cc (Customize) techset is a candidate as in HydraX", Got(E));
        }
        // no class: an emissive lit_ techset is still refused for a material that does not glow
        {
            std::string Cut = Json;
            const size_t At = Cut.find("{\"n\":\"lit\"");
            Cut.insert(At, "{\"n\":\"lit_em\",\"c\":\"Geometry\",\"f\":\"\",\"b\":\"Opaque\",\"t\":8320,\"s\":[" + Col + "," + Nor + "]},");
            const size_t Plain = Cut.find("{\"n\":\"lit\"", At + 10);
            Cut.replace(Plain + 6, 3, "other");      // the plain lit is no candidate any more (shorter than anything else with these textures): only the guard keeps lit_em away
            Data WithEm;
            WithEm.LoadCatalogText(Cut);
            Request Rq;
            Rq.RenderFromFlags = true;
            Rq.HasRender = false;
            Rq.Wanted = { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" } };
            Rq.SlotHashes = Hashes({ "colorMap", "normalMap" });
            const Choice C = Choose(WithEm, Rq);
            Check(C.Chosen != nullptr && C.Chosen->Name == "lit_decal", "emissive: with no class an emissive lit_ techset is still refused for a plain material", Got(C));
        }
        // an empty request is Default, not "every techset has all of nothing"
        {
            Request Nothing;
            Nothing.RenderFromFlags = true;
            const Choice C = Choose(M, Nothing);
            Check(C.Used == Rule::Default && Nm(C) == "lit", "empty request: Default", Got(C));
            Request Legacy;
            const Choice L = Choose(M, Legacy);
            Check(L.Used == Rule::Default, "empty request (no hashes, no keys): Default", Got(L));
            Request Unknown = Nothing;
            Unknown.SlotHashes = { SlotHash("aBo4OnlySlot") };
            Check(Choose(M, Unknown).Used == Rule::Default, "a material whose hashes no techset knows: Default", Got(Choose(M, Unknown)));
        }
        // keyed slots, as HydraX ImageSlots: a texture with no material key is not a known slot, adds no extras, and the overlap prefers the techset whose slots accept the images
        {
            const std::string UnZ = Sl("zzMap", "", "", ""), Un = Sl("xUnkeyed", "", "", ""), Zz = Sl("zzMap", "specColorMap", "specularMap", ""),
                              G1 = Sl("gMap", "cosinePowerMap", "glossMap", ""), G2 = Sl("gMap", "cosinePowerMap", "occlusionMap", "");
            const std::string KJson = "{\"version\":1,\"techsets\":[" +
                T("lit_kz_b", "Geometry", "Opaque", 0, { &Col, &Nor, &Ao }) + "," +
                T("lit_kz_long_a", "Geometry", "Opaque", 0, { &Col, &Nor, &Ao, &Un }) + "," +          // one more texture, no key for it
                T("lit_kx_p", "Geometry", "Opaque", 0, { &Col, &Nor, &Un }) + "," +
                T("lit_kx_qqqq", "Geometry", "Opaque", 0, { &Col, &Nor }) + "," +
                T("lit_ov_x", "Geometry", "Opaque", 0, { &Col, &Nor, &G1 }) + "," +
                T("lit_ov_yyyyy", "Geometry", "Opaque", 0, { &Col, &Nor, &G2 }) + "," +
                T("lit_other_blend", "Geometry", "Blend", 0, { &Col, &Nor, &Zz }) + "," +
                T("lit_famly_long", "Geometry", "Opaque", 0, { &Col, &Nor, &UnZ }) + "]}";
            Data K;
            Check(K.LoadCatalogText(KJson), "keyed-slot catalog loads", "");
            Check(!K.KnownHash(SlotHash("xUnkeyed")) && K.KnownHash(SlotHash("zzMap")) && K.KnownHash(SlotHash("colorMap")), "a texture with no material key is not a known slot", "");
            const auto Opaque = [&](std::initializer_list<const char*> Names, std::vector<std::pair<std::string, std::string>> Wanted) {
                Request Rq;
                Rq.RenderFromFlags = true;
                Rq.HasRender = true;
                Rq.RenderBlend = Blend::Opaque;
                Rq.Wanted = std::move(Wanted);
                Rq.SlotHashes = Hashes(Names);
                return Rq;
            };
            const std::vector<std::pair<std::string, std::string>> Plain = { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" }, { "occMap", "occlusionMap" } };
            // a BO4-only hash keeps the exact slot set from matching; the unkeyed texture is not asked for, so the shorter name wins
            const Choice A = Choose(K, Opaque({ "colorMap", "normalMap", "aoMap", "xUnkeyed", "bo4Only" }, Plain));
            Check(Nm(A) == "lit_kz_b" && A.Used == Rule::SlotSuperset, "superset: an unkeyed texture of the material is no known slot, so it does not narrow the candidates", Got(A));
            // both cover the material; the unkeyed texture of one is no extra
            const Choice B = Choose(K, Opaque({ "colorMap", "normalMap", "bo4Only" }, { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" } }));
            Check(Nm(B) == "lit_kx_p" && B.Used == Rule::SlotSuperset, "superset: the extras count keyed slots only", Got(B));
            // overlap: zzMap is known (keyed in a blend techset) but no opaque techset has it
            const Choice C = Choose(K, Opaque({ "colorMap", "normalMap", "zzMap", "bo4Only" }, { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" } }));
            Check(Nm(C) == "lit_kx_p" && C.Used == Rule::RenderClass, "overlap: the extras count keyed slots only", Got(C));
            // the compatible count is by texture hash: a techset that keys the colorMap texture as a normal map does not accept a colour image there
            {
                const std::string NormalKeyed = Sl("colorMap", "normalMap", "normalMap", "");
                const std::string LJson = "{\"version\":1,\"techsets\":[" +
                    T("lit_cx_a", "Geometry", "Opaque", 0, { &NormalKeyed }) + "," +
                    T("lit_cx_bbbbb", "Geometry", "Opaque", 0, { &Col }) + "," +
                    T("lit_cx_c_other", "Geometry", "Opaque", 0, { &Col }) + "," +
                    T("lit_cx_zz", "Geometry", "Blend", 0, { &Col, &Zz }) + "]}";
                Data L;
                Check(L.LoadCatalogText(LJson), "compat catalog loads", "");
                Check(L.DefaultKeyOfHash(SlotHash("colorMap")) == "colorMap" && L.DefaultKeyOfHash(SlotHash("nothing")).empty(), "the usual key of a texture hash is its most common key", L.DefaultKeyOfHash(SlotHash("colorMap")));
                const Choice X = Choose(L, Opaque({ "colorMap", "zzMap" }, { { "colorMap", "diffuseMap" } }));
                Check(Nm(X) == "lit_cx_bbbbb" && X.Used == Rule::RenderClass, "overlap: a colour image is not compatible with a techset that keys the colorMap texture as a normal map", Got(X));
            }
            // family: the type must have a keyed slot for every known hash of the material; a texture it holds without a key does not count
            {
                Request Fam = Opaque({ "colorMap", "normalMap", "zzMap" }, { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" } });
                Fam.SourceTechset = "mc/lit_famly_long#9";
                const Choice F = Choose(K, Fam);
                Check(F.Used != Rule::Family, "family: a texture the type holds with no key is no slot for the material's known hash", Got(F));
                Request Ok = Opaque({ "colorMap", "normalMap" }, { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" } });
                Ok.SourceTechset = "mc/lit_famly_long#9";
                const Choice G = Choose(K, Ok);
                Check(Nm(G) == "lit_famly_long" && G.Used == Rule::Family, "family: the same type is taken when the material's slots are all keyed there", Got(G));
            }
            // overlap tie on shared slots, broken by how many of the images the slots accept (HydraX CompatibleCount), before the name length
            const Choice D = Choose(K, Opaque({ "colorMap", "normalMap", "gMap", "zzMap" }, { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" }, { "cosinePowerMap", "occlusionMap" } }));
            Check(Nm(D) == "lit_ov_yyyyy" && D.Used == Rule::RenderClass, "overlap: the techset whose slots accept the images beats a shorter name", Got(D));
        }
        // the name list: 60-bit key, xtechset_ form, no index
        {
            Data Names;
            Check(Names.NameOfSource("33832cd9511a3ad").empty() && !Names.HasNames(), "names: no index, no name", "");
            Names.SetNameForTest(0x7C74CCEC2904CEE4ull, "mc/lit#f840f400");      // a 63-bit key of the name list
            Check(Names.NameOfSource("c74ccec2904cee4") == "mc/lit#f840f400", "names: the key is masked to 60 bits", Names.NameOfSource("c74ccec2904cee4"));
            Check(Names.NameOfSource("xtechset_7c74ccec2904cee4") == "mc/lit#f840f400" && Names.NameOfSource("xtechset_c74ccec2904cee4") == "mc/lit#f840f400", "names: the Cold War xtechset_ form resolves", "");
            Check(Names.NameOfSource("1234").empty() && Names.NameOfSource("lit").empty() && Names.NameOfSource("").empty() && Names.NameOfSource("zz").empty(), "names: an unknown hash or a plain name resolves to nothing", "");
            Blend B = Blend::Other;
            uint32_t Tr = 0;
            Check(ClassFromName(M, "mc/lit_decal#12345678", B, Tr) && B == Blend::Decal && (Tr & Trait::DecalState), "class from name: the family's catalog techset gives the class", BlendName(B));
            Check(ClassFromName(M, "t8_lit_a", B, Tr) && B == Blend::Opaque && !ClassFromName(M, "mc/not_a_techset#1", B, Tr) && !ClassFromName(M, "", B, Tr), "class from name: t8_ cut; an unknown name has no class", "");
            Request Named;
            Named.SourceTechset = "mc/lit_a#99";
            Named.Wanted = { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" }, { "occMap", "occlusionMap" } };
            Named.RenderFromFlags = true;
            Named.SlotHashes = Hashes({ "colorMap", "normalMap", "aoMap" });
            ClassFromName(M, Named.SourceTechset, Named.RenderBlend, Named.RenderTraits);
            Named.HasRender = true;
            const Choice C = Choose(M, Named);
            Check(Nm(C) == "lit_a" && C.Used == Rule::Family && C.Exact(), "names: a resolved family name runs the family rule (exact)", Got(C));
        }
        // $specular_mask: one rule in the catalog data, a hypothesis
        {
            Check(M.DominantCodeSemantic("$specular_mask") == "specularMask" && M.CodeImageSemantics("$specular_mask") != nullptr, "code image: $specular_mask is a specularMask entry of the catalog data (hypothesis), for the writer and the gate alike", M.DominantCodeSemantic("$specular_mask"));
        }
    }

    // missing data: nothing loads, nothing is chosen, the caller falls back
    {
        Data Missing;
        Missing.Load(fs::temp_directory_path() / "graywolf_no_such_profiles_dir");
        Check(!Missing.HasCatalog() && !Missing.HasDefaults() && !Missing.HasLayouts() && Missing.Problems().size() == 3, "missing files: three problems, no data", std::to_string(Missing.Problems().size()));
        Check(Choose(Missing, Request{}).Chosen == nullptr, "missing files: nothing is chosen", "");
    }

    // the shipped data
    const Data& S = Shared();
    Check(S.HasCatalog() && S.HasDefaults() && S.HasLayouts(), "shipped data loads (techsets, defaults, layouts)", S.Problems().empty() ? "" : S.Problems().front());
    if (!S.HasCatalog()) return Failed;
    Check(S.All().size() > 1000, "shipped catalog size", std::to_string(S.All().size()));
    const Techset* Lit = S.Find("lit");
    Check(Lit != nullptr && Lit->ByKey("colorMap") != nullptr && Lit->ByKey("cosinePowerMap") == nullptr && Lit->ByKey("occMap") == nullptr && Lit->ByKey("specColorMap") == nullptr,
          "lit has colour and normal slots only", "");
    const Techset* Full = S.Find("lit_advanced_fullspec");
    Check(Full != nullptr && Full->ByKey("specColorMap") != nullptr && Full->ByKey("specColorMap")->Semantic == "specularMap", "fullspec: specColorMap takes specularMap", "");

    const auto Run = [&](std::initializer_list<std::pair<const char*, const char*>> Wanted, Blend Class = Blend::Opaque, bool Emissive = false, bool Spec = false,
                         const char* Source = "") {
        Request Rq;
        Rq.SourceTechset = Source;
        for (const auto& [K, I] : Wanted) Rq.Wanted.emplace_back(K, I);
        Rq.Class = Class;
        Rq.Emissive = Emissive;
        Rq.HasColorSpec = Spec;
        return Choose(S, Rq);
    };
    const auto Name = [](const Choice& C) { return C.Chosen ? C.Chosen->Name : std::string("(null)"); };
    const std::pair<const char*, const char*> Color{ "colorMap", "diffuseMap" }, Normal{ "normalMap", "normalMap" }, Gloss{ "cosinePowerMap", "glossMap" },
        Occ{ "occMap", "occlusionMap" }, Spec{ "specColorMap", "specularMap" };
    {
        const Choice C = Run({ Color, Normal });
        Check(Name(C) == "lit" && C.Exact(), "colour and normal give lit", Name(C));
    }
    {
        const Choice C = Run({ Color, Normal, Gloss, Occ });
        Check(C.Chosen != nullptr && C.Chosen->ByKey("cosinePowerMap") && C.Chosen->ByKey("occMap") && Name(C) != "lit", "gloss and occlusion give a techset that has those slots", Name(C));
    }
    {
        const Choice C = Run({ Color, Normal, Gloss, Occ, Spec }, Blend::Opaque, false, true);
        const Slot* Sp = C.Chosen ? C.Chosen->ByKey("specColorMap") : nullptr;
        Check(Name(C) == "lit_advanced_fullspec" && Sp != nullptr && SemanticFor(*Sp, "specularMap") == "specularMap" && C.Exact(), "a colour spec map gets a fullspec slot, semantic specularMap", Name(C));
    }
    {
        const Choice C = Run({ Color, Normal, Gloss, Occ }, Blend::Opaque, false, false, "lit_weapon_camo");
        Check(C.Chosen != nullptr && Name(C).find("camo") == std::string::npos && Name(C) != "lit", "lit_weapon_camo is never chosen when named", Name(C));
        const Choice D = Run({ Color, Normal }, Blend::Opaque, false, false, "lit_weapon_camo_detail");
        Check(D.Chosen != nullptr && Name(D).find("camo") == std::string::npos, "lit_weapon_camo_detail is never chosen either", Name(D));
    }
    {
        const Choice C = Run({ Color, Normal }, Blend::AlphaTest);
        Check(C.Chosen != nullptr && C.Chosen->Class == Blend::AlphaTest && !C.Exact(), "a cutout material gets an alpha-test techset and the fallback is flagged", Name(C));
        const Choice B = Run({ Color, Normal, Gloss, Occ }, Blend::Blend);
        Check(B.Chosen != nullptr && B.Chosen->Class == Blend::Blend && B.Chosen->ByKey("occMap") != nullptr, "a blended material with gloss and occlusion keeps both", Name(B));
        const Choice E = Run({ Color, Normal, { "colorMap00", "diffuseMap" } }, Blend::Opaque, true);
        Check(Name(E) == "lit_emissive", "an emissive material gets lit_emissive", Name(E));
    }
    {
        // every slot of every candidate the rules can pick is a slot of that techset (no image is written to a missing slot)
        int Bad = 0;
        std::string First;
        const char* const Classes[] = { "Opaque", "AlphaTest", "Blend", "Decal" };
        for (const char* Cl : Classes)
            for (int Mask = 0; Mask < 32; ++Mask)
            {
                std::vector<std::pair<std::string, std::string>> W = { { "colorMap", "diffuseMap" }, { "normalMap", "normalMap" } };
                if (Mask & 1) W.emplace_back("cosinePowerMap", "glossMap");
                if (Mask & 2) W.emplace_back("occMap", "occlusionMap");
                if (Mask & 4) W.emplace_back("specColorMap", "specularMap");
                if (Mask & 8) W.emplace_back("colorMap00", "diffuseMap");
                Request Rq;
                Rq.Wanted = W;
                Rq.Class = BlendFromName(Cl);
                Rq.Emissive = (Mask & 8) != 0;
                Rq.HasColorSpec = (Mask & 4) != 0;
                const Choice C = Choose(S, Rq);
                if (C.Chosen == nullptr) { ++Bad; First = "null"; continue; }
                if (Special(C.Chosen->Name)) { ++Bad; First = C.Chosen->Name; }
                // exact rules promise every slot; "lit" only ever with just the base slots, or when nothing shares any
                if (C.Chosen->Name == "lit" && (Mask & 15) != 0 && C.Used != Rule::Default && std::string(Cl) == "Opaque") { ++Bad; First = "lit with extras"; }
                if (C.Used == Rule::SlotSet || C.Used == Rule::SlotSuperset)
                    for (const auto& [K, I] : W)
                        if (const Slot* Sl = C.Chosen->ByKey(K); Sl == nullptr || !Accepts(*Sl, I)) { ++Bad; First = C.Chosen->Name + " lacks " + K; }
            }
        Check(Bad == 0, "every wanted slot exists on a techset chosen by a slot rule; no special-purpose or bare lit", First);
    }
    return Failed;
}
}
