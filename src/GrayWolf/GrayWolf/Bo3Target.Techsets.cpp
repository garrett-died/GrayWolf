#include "stdafx.h"

#include "Bo3Target.Techsets.h"

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

bool Data::LoadCatalogText(const std::string& Json)
{
    Sets.clear();
    ByName.clear();
    Overrides.clear();
    DefaultImages.clear();
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
                if (!S.Key.empty() && !S.Default.empty()) DefaultImages.insert({ S.Key, Lower(S.Default) });
                T.Slots.push_back(std::move(S));
            }
        }
        ByName[T.Name] = Sets.size();
        Sets.push_back(std::move(T));
    }
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
                return D;
            }
            if (P == P.parent_path()) break;
        }
        D.Load(fs::path(Buf).parent_path() / "zonekit" / "profiles");   // nothing found: the problems say which files
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

Choice Choose(const Data& D, const Request& R)
{
    Choice Out;
    if (!D.HasCatalog()) return Out;
    const auto Make = [&](const Techset* T, Rule Used, std::string Why) {
        Out.Chosen = T;
        Out.Used = Used;
        Out.Reason = std::move(Why);
        return Out;
    };
    const auto Compatible = [&](const Techset& T) {
        for (const auto& [Key, Intent] : R.Wanted)
            if (const Slot* S = T.ByKey(Key); S != nullptr && !Accepts(*S, Intent)) return false;
        return true;
    };
    const auto VegetationOk = [&](const std::string& N) { return !IsVegetation(N) || R.Class == Blend::AlphaTest; };
    const bool Named = UsableName(R.SourceTechset);
    const std::string& Source = R.SourceTechset;

    // 1. Override
    if (Named)
        if (const std::string* To = D.OverrideFor(Source))
            if (const Techset* T = D.Find(*To); T != nullptr && !Special(T->Name) && VegetationOk(T->Name) && Compatible(*T))
                return Make(T, Rule::Override, std::format("The rule table maps {} to {}.", Source, T->Name));

    if (Named)
    {
        // 2. Exact name
        if (const Techset* T = D.Find(Source); T != nullptr && !Special(Source) && VegetationOk(Source) && Compatible(*T))
            return Make(T, Rule::Name, std::format("Techset {} is in the Black Ops III catalog.", Source));
        // 3. Normalized
        if (Source.rfind("t8_", 0) == 0)
        {
            const std::string Bare = Source.substr(3);
            if (const Techset* T = D.Find(Bare); T != nullptr && !Special(Bare) && VegetationOk(Bare) && Compatible(*T))
                return Make(T, Rule::Normalized, std::format("Techset {} is not in the Black Ops III catalog; {} is.", Source, Bare));
        }
        // 4. Sky
        if (const std::string Sky = BareSky(Source); !Sky.empty())
            if (const Techset* T = D.Find(Sky); T != nullptr && !Special(Sky) && Compatible(*T))
                return Make(T, Rule::Normalized, std::format("Techset {} is the Black Ops III sky techset {}.", Source, Sky));
        // 5. Family (a BO4 name with its folder and variant hash)
        if (const std::string Family = FamilyOf(Source); !Family.empty())
        {
            const std::string Target = FamilyTarget(D, Family);
            const Techset* T = Target.empty() ? nullptr : D.Find(Target);
            if (T != nullptr && IsMappable(Target) && VegetationOk(Target) && Compatible(*T) &&
                std::all_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return T->ByKey(W.first) != nullptr; }))
                return Make(T, Rule::Family, std::format("bo4 techset family: {} is of family {}, the Black Ops III type {}; it has a slot for each of the {} image slots.",
                                                          Source, Family, Target, R.Wanted.size()));
        }
    }

    // 6 to 8 work from the slots the description fills. The class (alpha, decal, emissive) narrows the candidates first; when
    // nothing of the class has the slots the emissive trait, then the class itself is let go.
    const auto Base = [](const std::string& Key) { return Key == "heatmap" || Key == "colorMap" || Key == "normalMap"; };
    const auto Supports = [&](const Techset& T, const std::pair<std::string, std::string>& W) {
        const Slot* S = T.ByKey(W.first);
        return S != nullptr && Accepts(*S, W.second);
    };
    const auto Mismatch = [&](const Techset& T) {
        // a colour specular map on a scalar slot (or the reverse) is the worse of two candidates
        return R.HasColorSpec && T.HasTrait(Trait::SpecColorMap) && T.HasTrait(Trait::ColorSpec) != R.ColorSpec ? 1 : 0;
    };
    const char* const LevelNote[] = { "", " The emissive trait was let go: no techset of the class has it with these slots.",
                                      " The render class was let go: no techset of that class has these slots (lit techsets of any class)." };
    for (int Level = 0; Level < 3; ++Level)
    {
        std::vector<const Techset*> Pool;
        for (const Techset& T : D.All())
        {
            if (T.Traits & (Trait::Camo | Trait::Customize)) continue;
            // a surface techset: colour and normal slots (not emissive_passthrough or lit_emissive_clone, which a material with few images would reach)
            if (T.ByKey("colorMap") == nullptr || T.ByKey("normalMap") == nullptr) continue;
            if (Level < 2)
            {
                if (!IsMappable(T.Name) || T.Class != R.Class || !VegetationOk(T.Name)) continue;
                if (Level == 0 && T.HasTrait(Trait::Emissive) != R.Emissive) continue;
            }
            else if (!IsLit(T.Name)) continue;
            Pool.push_back(&T);
        }
        const size_t Need = R.Wanted.size();
        // 6. exact slot set: every slot is there and the techset holds nothing else but the base slots
        {
            std::vector<std::tuple<int, size_t, size_t, std::string, const Techset*>> Hit;
            for (const Techset* T : Pool)
            {
                if (!std::all_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return Supports(*T, W); })) continue;
                bool Extra = false;
                for (const Slot& S : T->Slots)
                    if (!S.Key.empty() && !Base(S.Key) && std::none_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return W.first == S.Key; })) Extra = true;
                if (!Extra) Hit.emplace_back(Mismatch(*T), std::numeric_limits<size_t>::max() - T->KeyedCount(), T->Name.size(), T->Name, T);
            }
            if (!Hit.empty())
            {
                std::sort(Hit.begin(), Hit.end());
                const Techset* T = std::get<4>(Hit.front());
                return Make(T, Rule::SlotSet, std::format("The material's image slots ({}) are exactly the slots of {}.{}{}", KeysText(R.Wanted), T->Name,
                                                         Hit.size() > 1 ? std::format(" {} techsets share this set; the one with the most image slots, then the shortest name is taken.", Hit.size()) : "",
                                                         LevelNote[Level]));
            }
        }
        // 7. slot superset: every slot is there, the fewest extras
        {
            std::vector<std::tuple<int, size_t, size_t, std::string, const Techset*>> Hit;
            for (const Techset* T : Pool)
                if (std::all_of(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return Supports(*T, W); }))
                    Hit.emplace_back(Mismatch(*T), T->KeyedCount() - Need, T->Name.size(), T->Name, T);
            if (!Hit.empty())
            {
                std::sort(Hit.begin(), Hit.end());
                const Techset* T = std::get<4>(Hit.front());
                return Make(T, Rule::SlotSuperset, std::format("{} covers the material's {} image slots ({}) with {} more.{}", T->Name, Need, KeysText(R.Wanted),
                                                               T->KeyedCount() - Need, LevelNote[Level]));
            }
        }
        // 8. render-class overlap: the candidate that keeps the most of the slots (the rest are reported as unmapped)
        {
            std::vector<std::tuple<int, size_t, size_t, size_t, std::string, const Techset*>> Hit;
            for (const Techset* T : Pool)
            {
                const size_t Shared = static_cast<size_t>(std::count_if(R.Wanted.begin(), R.Wanted.end(), [&](const auto& W) { return Supports(*T, W); }));
                if (Shared == 0) continue;
                Hit.emplace_back(Mismatch(*T), Need - Shared, T->KeyedCount() - Shared, T->Name.size(), T->Name, T);
            }
            if (!Hit.empty())
            {
                std::sort(Hit.begin(), Hit.end());
                const Techset* T = std::get<5>(Hit.front());
                return Make(T, Rule::RenderClass, std::format("No techset of the class has all of the {} image slots ({}); {} keeps {} of them.{}", Need,
                                                              KeysText(R.Wanted), T->Name, Need - std::get<1>(Hit.front()), LevelNote[Level]));
            }
        }
    }

    // 9. Default: lit, which holds colour and normal only. Reached only when no catalog techset shares any of the slots.
    if (const Techset* T = D.Find("lit"))
        return Make(T, Rule::Default, std::format("No catalog techset has any of the image slots ({}); lit is the default.", KeysText(R.Wanted)));
    return Out;
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
