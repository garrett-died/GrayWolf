#include "stdafx.h"

#include "MapToolMaterials.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace Maps
{
    namespace
    {
        std::string LowerOf(std::string S)
        {
            for (char& C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            return S;
        }

        void ReadSet(const nlohmann::json& J, const char* Key, std::set<std::string>& Out)
        {
            if (J.contains(Key) && J[Key].is_array())
                for (const auto& V : J[Key])
                    if (V.is_string()) Out.insert(LowerOf(V.get<std::string>()));
        }
    }

    bool ToolMaterials::Parse(const std::string& Json, std::string& Why)
    {
        *this = ToolMaterials();
        const nlohmann::json J = nlohmann::json::parse(Json, nullptr, false);
        if (J.is_discarded() || !J.is_object())
        {
            Why = "tool-materials.json is not readable";
            return false;
        }
        ReadSet(J, "tested", Tested);
        ReadSet(J, "hypothesis", Hypothesis);
        if (Tested.empty())
        {
            Why = "tool-materials.json lists no tested BO3 tool name";
            return false;
        }
        if (J.contains("surfaces") && J["surfaces"].is_array())
            for (const auto& V : J["surfaces"])
                if (V.is_string()) Surfaces.push_back(LowerOf(V.get<std::string>()));
        if (J.contains("lineages") && J["lineages"].is_object())
            for (const auto& [Name, L] : J["lineages"].items())
            {
                Lineage Out;
                ReadSet(L, "games", Out.Games);
                ReadSet(L, "drop", Out.Drop);
                ReadSet(L, "fallback_drop_prefixes", Out.DropPrefixes);
                Out.Rule = L.value("rule", false);
                if (L.contains("fallback_prefixes") && L["fallback_prefixes"].is_array())
                    for (const auto& P : L["fallback_prefixes"])
                        if (P.is_array() && P.size() == 2 && P[0].is_string() && P[1].is_string()) Out.Prefixes.emplace_back(LowerOf(P[0].get<std::string>()), LowerOf(P[1].get<std::string>()));
                if (L.contains("fallback_suffix_clip") && L["fallback_suffix_clip"].is_string()) Out.SuffixClip = LowerOf(L["fallback_suffix_clip"].get<std::string>());
                if (L.contains("map") && L["map"].is_object())
                    for (const auto& [From, To] : L["map"].items())
                        if (To.is_string()) Out.Map[LowerOf(From)] = LowerOf(To.get<std::string>());
                Lineages[Name] = std::move(Out);
            }
        return true;
    }

    const ToolMaterials* ToolMaterials::Shared(std::string& Why)
    {
        static ToolMaterials Table;
        static bool Tried = false, Ok = false;
        static std::string Reason;
        if (!Tried)
        {
            Tried = true;
            wchar_t Buf[MAX_PATH * 2] = {};
            GetModuleFileNameW(nullptr, Buf, static_cast<DWORD>(std::size(Buf)));
            std::error_code Ec;
            Reason = "zonekit/profiles/t7/target/tool-materials.json was not found";
            for (fs::path P = fs::path(Buf).parent_path(); !P.empty(); P = P.parent_path())
            {
                const fs::path File = P / "zonekit" / "profiles" / "t7" / "target" / "tool-materials.json";
                if (fs::exists(File, Ec))
                {
                    std::ifstream In(File, std::ios::binary);
                    std::stringstream Text;
                    Text << In.rdbuf();
                    Ok = Table.Parse(Text.str(), Reason);
                    break;
                }
                if (P == P.parent_path()) break;
            }
        }
        if (!Ok) Why = Reason;
        return Ok ? &Table : nullptr;
    }

    std::string ToolMaterials::LineageOf(const std::string& Game) const
    {
        const std::string G = LowerOf(Game);
        for (const auto& [Name, L] : Lineages)
            if (L.Games.count(G) != 0) return Name;
        return std::string();
    }

    bool ToolMaterials::Exact(const std::string& LineageName, const std::string& Name) const
    {
        const std::string N = LowerOf(Name);
        if (IsBo3(N)) return true;
        const auto Lin = Lineages.find(LineageName);
        return Lin != Lineages.end() && (Lin->second.Drop.count(N) != 0 || Lin->second.Map.count(N) != 0);
    }

    ToolResult ToolMaterials::Resolve(const std::string& LineageName, const std::string& Name) const
    {
        const std::string N = LowerOf(Name);
        const auto Lin = Lineages.find(LineageName);
        if (Lin != Lineages.end())
        {
            if (Lin->second.Drop.count(N) != 0) return { std::string(), ToolKind::Drop };
            if (const auto M = Lin->second.Map.find(N); M != Lin->second.Map.end()) return { M->second, ToolKind::Mapped };
        }
        if (IsBo3(N)) return { N, ToolKind::Unchanged };
        // a game with no lineage (Black Ops 3, 4, Cold War) keeps a name it does not know: no rule, no fallback, no drop
        if (Lin == Lineages.end() || !Lin->second.Rule) return { N, ToolKind::Unlisted };
        const Lineage& L = Lin->second;
        // clip_<kind>_<surface> -> <surface>_clip[_<kind>]
        std::string Kind;
        bool Surface = false;
        if (N.rfind("clip_", 0) == 0)
        {
            const size_t Cut = N.rfind('_');
            const std::string Tail = N.substr(Cut + 1);
            if (Cut >= 4 && std::find(Surfaces.begin(), Surfaces.end(), Tail) != Surfaces.end())
            {
                Surface = true;
                Kind = Cut > 5 ? N.substr(5, Cut - 5) : std::string();
                const std::string Candidate = Tail + "_clip" + (Kind.empty() ? std::string() : "_" + Kind);
                if (IsBo3(Candidate)) return { Candidate, ToolKind::Rule };
            }
        }
        // the surface dropped
        if (Surface)
        {
            const std::string Plain = Kind.empty() ? std::string("clip") : "clip_" + Kind;
            if (IsBo3(Plain)) return { Plain, ToolKind::Fallback };
        }
        for (const std::string& D : L.DropPrefixes)
            if (N.rfind(D, 0) == 0) return { std::string(), ToolKind::Drop };
        for (const auto& [Prefix, To] : L.Prefixes)
            if (N.rfind(Prefix, 0) == 0) return { To, ToolKind::Fallback };
        if (!L.SuffixClip.empty() && N.size() > 5 && N.compare(N.size() - 5, 5, "_clip") == 0) return { L.SuffixClip, ToolKind::Fallback };
        return { std::string("clip"), ToolKind::Fallback };
    }

    ToolResult ToolUse::Peek(const std::string& Name) const
    {
        if (Table == nullptr) return { Name, ToolKind::Unchanged };
        ToolResult R = Table->Resolve(Lineage, Name);
        // a name that is a BO3 name keeps the spelling it came with, so nothing changes unless the name does
        if (R.Kind == ToolKind::Unchanged || R.Kind == ToolKind::Unlisted) R.Name = Name;
        return R;
    }

    ToolResult ToolUse::Apply(const std::string& Name)
    {
        const ToolResult R = Peek(Name);
        if (Table == nullptr) return R;
        const std::string Key = Name + " -> " + R.Name;
        switch (R.Kind)
        {
        case ToolKind::Unchanged: Unchanged[Name]++; break;
        case ToolKind::Mapped: Mapped[Key]++; break;
        case ToolKind::Rule: Rule[Key]++; break;
        case ToolKind::Fallback: Fallback[Key]++; break;
        case ToolKind::Unlisted: Unlisted[Name]++; break;
        case ToolKind::Drop: Dropped[Name]++; break;
        }
        return R;
    }

    size_t ToolUse::Changed() const
    {
        size_t N = 0;
        for (const auto* M : { &Mapped, &Rule, &Fallback, &Dropped })
            for (const auto& [K, C] : *M) N += C;
        return N;
    }

    std::vector<std::string> ToolUse::Lines() const
    {
        std::vector<std::string> Out;
        if (Table == nullptr) return Out;
        const auto Put = [&](const char* Title, const std::map<std::string, size_t>& M) {
            if (M.empty()) return;
            size_t Total = 0;
            std::string Text;
            for (const auto& [K, C] : M) Total += C, Text += (Text.empty() ? "" : "; ") + K + " x" + std::to_string(C);
            Out.push_back(std::format("tool materials ({}): {} ({}): {}", Lineage.empty() ? std::string("no lineage") : Lineage, Title, Total, Text));
        };
        Put("mapped by the lineage's table", Mapped);
        Put("mapped by the clip_<kind>_<surface> rule", Rule);
        Put("FALLBACK (no table row, no BO3 name: surface dropped or nearest base tool)", Fallback);
        Put("dropped (the face's brush or mesh is not written)", Dropped);
        Put("NOT a BO3 name and the game has no rules for it: kept as it is", Unlisted);
        Put("already a BO3 name, unchanged", Unchanged);
        if (BrushesDropped + MeshesDropped + SkyBrushesDropped > 0)
            Out.push_back(std::format("tool materials: {} brushes dropped for a face that drops, {} meshes dropped for a material that drops, {} all-sky world brushes dropped (the sky shell and the volume_sun replace them)", BrushesDropped, MeshesDropped, SkyBrushesDropped));
        return Out;
    }
}

namespace Maps
{
    int ToolMaterialsSelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("tool materials: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        std::string Why;
        const ToolMaterials* Table = ToolMaterials::Shared(Why);
        Check(Table != nullptr, "the real tool-materials.json loads", Why);
        if (Table == nullptr) return Failed;
        const std::string Iw = Table->LineageOf("mwr");
        Check(Iw == "iw" && Table->LineageOf("MWR") == "iw" && Table->LineageOf("bo3").empty() && Table->LineageOf("bo4").empty(), "mwr is the iw lineage; bo3 and bo4 have none", Iw);

        // every MWR name of mp_vacant that needed a decision, as the brief lists it
        const std::pair<const char*, const char*> Wanted[] = {
            { "clip_ai_player", "clip" }, { "clip_ai_player_plastic", "plastic_clip" }, { "clip_ai_player_wood", "wood_clip" },
            { "clip_full_concrete", "concrete_clip_full" }, { "clip_full_metal", "metal_clip_full" }, { "clip_ground_concrete", "concrete_clip" },
            { "clip_nosight_brick", "brick_clip_nosight" }, { "clip_nosight_cloth", "cloth_clip_nosight" }, { "clip_nosight_concrete", "concrete_clip_nosight" },
            { "clip_nosight_dirt", "dirt_clip_nosight" }, { "clip_nosight_metal", "metal_clip_nosight" }, { "clip_nosight_plastic", "plastic_clip_nosight" },
            { "clip_nosight_rock", "rock_clip_nosight" }, { "clip_nosight_wood", "wood_clip_nosight" },
            { "clip_nosight_cushion", "cloth_clip_nosight" }, { "clip_nosight_soft_metal", "metal_clip_nosight" }, { "clip_nosight_nothing", "clip_nosight" },
            { "clip_player_metal", "clip_player" }, { "clip_missile_nomark", "clip_missile" }, { "clip_weap", "clip_weapon" }, { "clip_item", "clip_physics" },
            { "clip_stairs", "clip_player" }, { "clip_stairs_default", "clip_player" }, { "clip_water_player", "clip_player" }, { "portal_caulk", "caulk" },
            { "shadowcaster", "shadowcaster" }, { "sky", "sky" }, { "sun_volume", "sun_volume" }, { "hint", "hint" }, { "trigger", "trigger" }, { "mantle_on", "mantle_on" },
            { "nosight_noclip", "nosight_noclip" }, { "caulk", "caulk" }, { "clip", "clip" }, { "caulk_shadow", "caulk_shadow" }, { "mantle_over", "mantle_over" },
        };
        std::string Wrong;
        for (const auto& [From, To] : Wanted)
        {
            const ToolResult R = Table->Resolve(Iw, From);
            if (R.Name != To || R.Kind == ToolKind::Drop || R.Kind == ToolKind::Fallback) Wrong += std::string(From) + " -> " + R.Name + "; ";
        }
        Check(Wrong.empty(), "every MWR name maps as listed", Wrong);
        // portals have no BO3 tool
        for (const char* Name : { "portal", "portal_nodraw", "clip_block_spawn_traces" })
            Check(Table->Resolve(Iw, Name).Kind == ToolKind::Drop, "an IW portal drops", Name);
        // every BO3 name is unchanged, with or without the iw lineage; the names here are written out from the stock prefabs, not read back from the table
        std::string Changed;
        for (const char* Name : { "caulk", "caulk_sun_shadow", "caulk_transparent", "clip", "clip_full", "clip_player", "clip_ai", "clip_missile", "clip_physics", "clip_weapon", "clip_weapon_noimpact", "clip_vehicle",
                                  "clip_nosight", "nosight_noclip", "clip_slick", "clip_out_of_bounds", "clip_weap_glass", "hint", "mantle_on", "trigger", "sound_trigger", "nodraw", "sky", "sun_volume",
                                  "shadowcaster", "volume", "exposure_volume", "umbra_volume", "litfog_volume", "outdoorbounds_volume", "metal_clip", "metal_clip_nosight", "metal_clip_full", "wood_clip",
                                  "wood_clip_nosight", "concrete_clip", "plastic_clip_nosight", "cloth_clip_nosight", "glass_clip", "rock_clip" })
            for (const char* Lin : { "", "iw" })
            {
                const ToolResult R = Table->Resolve(Lin, Name);
                if (R.Kind != ToolKind::Unchanged || R.Name != Name || !Table->IsTested(Name)) Changed += std::string(Name) + " (" + Lin + ") -> " + R.Name + "; ";
            }
        for (const auto* List : { &Table->Tested, &Table->Hypothesis })
            for (const std::string& Name : *List)
                for (const char* Lin : { "", "iw" })
                {
                    const ToolResult R = Table->Resolve(Lin, Name);
                    if (R.Kind != ToolKind::Unchanged || R.Name != Name) Changed += Name + " (" + Lin + ") -> " + R.Name + "; ";
                }
        Check(Changed.empty() && Table->Tested.size() > 40 && Table->Hypothesis.size() > 20, "every name of the BO3 table is unchanged", Changed);
        // the tested table holds what was measured in the stock prefabs
        for (const char* Name : { "caulk", "clip_full", "nosight_noclip", "metal_clip", "wood_clip_nosight", "sun_volume", "outdoorbounds_volume", "metal_clip_full", "clip_weap_glass" })
            Check(Table->IsTested(Name), "a stock tool name is tested", Name);
        // an unknown name: the rule, then the surface dropped, then the prefix; always a BO3 name, and a fallback is a report
        const auto Is = [&](const char* Name, const char* To, ToolKind Kind) {
            const ToolResult R = Table->Resolve(Iw, Name);
            Check(R.Name == To && R.Kind == Kind, "the fallback chain", std::string(Name) + " -> " + R.Name + " kind " + std::to_string(static_cast<int>(R.Kind)));
        };
        Is("clip_nosight_glass", "glass_clip_nosight", ToolKind::Rule);
        Is("clip_nosight_foo", "clip_nosight", ToolKind::Fallback);
        Is("clip_missile_zzz", "clip_missile", ToolKind::Fallback);
        Is("clip_ai_sand", "clip_ai", ToolKind::Fallback);   // sand_clip_ai is no BO3 name: the surface drops to clip_ai
        Is("clip_nosight_grass", "grass_clip_nosight", ToolKind::Rule);
        Is("nosight_xyz", "clip_nosight", ToolKind::Fallback);
        Is("weird_clip", "clip", ToolKind::Fallback);
        Is("portal_extra", "", ToolKind::Drop);
        for (const char* Name : { "clip_nosight_foo", "clip_xyz_metal", "clip_weap_extra", "caulk_other", "nosight_other", "mantle_other", "hint_x", "sky_other", "trigger_other", "nodraw_other", "ladder_x", "clip_zzz" })
        {
            const ToolResult R = Table->Resolve(Iw, Name);
            Check(R.Kind == ToolKind::Drop || Table->IsBo3(R.Name), "a name never leaves outside the BO3 list", std::string(Name) + " -> " + R.Name);
        }
        // a game with no lineage (Black Ops 3, 4, Cold War) keeps a name it does not know: no rule, no fallback, no drop
        for (const char* Lin : { "", "bo4", "bo3" })
            for (const char* Name : { "clip_monster", "portal", "portal_nodraw", "clip_nosight_metal", "clip_zzz", "weird_clip", "nosight_xyz" })
            {
                const ToolResult R = Table->Resolve(Lin, Name);
                Check(R.Kind == ToolKind::Unlisted && R.Name == Name, "a game with no lineage keeps a name outside the BO3 lists", std::string(Lin) + ":" + Name + " -> " + R.Name);
            }
        // an exact tool name is a key of the lineage table, a drop name or a BO3 name; a drawn material that starts like one is not
        Check(Table->Exact("iw", "portal") && Table->Exact("iw", "clip_nosight_metal") && Table->Exact("", "sun_volume") && Table->Exact("iw", "Metal_Clip_Nosight") && !Table->Exact("", "portal") && !Table->Exact("", "clip_nosight_metal"),
              "exact tool names");
        for (const char* Name : { "skyline_brick", "ladder_metal_rust", "portal_frame", "foo_clip", "skyscraper_concrete", "hint_sign", "triggerbox" })
            Check(!Table->Exact("iw", Name) && !Table->Exact("", Name), "a drawn material that starts like a tool is not an exact tool name", Name);
        // the counts: a use object counts what it resolves, and a table-less one passes names through
        ToolUse Use;
        Use.Table = Table;
        Use.Lineage = Iw;
        Use.Apply("clip_nosight_brick");
        Use.Apply("clip_nosight_brick");
        Use.Apply("caulk");
        Use.Apply("portal");
        Use.Apply("clip_nosight_foo");
        Check(Use.Mapped["clip_nosight_brick -> brick_clip_nosight"] == 2 && Use.Unchanged["caulk"] == 1 && Use.Dropped["portal"] == 1 && Use.Fallback["clip_nosight_foo -> clip_nosight"] == 1 && Use.Changed() == 4,
              "the use counts each kind by name");
        Check(Use.Lines().size() == 4, "the report has a line per kind", std::to_string(Use.Lines().size()));
        ToolUse Bare;
        Check(Bare.Apply("clip_nosight_foo").Name == "clip_nosight_foo" && Bare.Changed() == 0 && Bare.Lines().empty(), "without a table the names pass through, nothing counted");
        // a name keeps its spelling when it is already a BO3 name
        Check(Use.Peek("Caulk").Name == "Caulk", "an unchanged name keeps its spelling");

        // a small table of the same shape (the parser): an unreadable one and one without BO3 names are refused, a lineage from another game works
        ToolMaterials Mini;
        Check(!Mini.Parse("{", Why) && !Mini.Parse("{\"tested\": []}", Why), "an unreadable table and one with no BO3 name are refused");
        Check(Mini.Parse(R"({"tested":["clip","metal_clip_nosight"],"surfaces":["metal"],"lineages":{"cw":{"games":["cw"],"rule":true,"map":{"clip_player_cw":"clip"},"drop":["zone"],"fallback_prefixes":[["clip","clip"]]}}})", Why), "a small table parses", Why);
        Check(Mini.LineageOf("CW") == "cw" && Mini.Resolve("cw", "clip_player_cw").Name == "clip" && Mini.Resolve("cw", "zone").Kind == ToolKind::Drop && Mini.Resolve("cw", "clip_nosight_metal").Name == "metal_clip_nosight",
              "a lineage of another game maps its own names, and the rule needs no table row");
        return Failed;
    }
}
