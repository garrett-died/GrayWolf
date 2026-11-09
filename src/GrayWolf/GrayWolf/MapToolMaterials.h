#pragma once

// Tool materials of a Radiant map (clip, caulk, trigger, sky ...) as Black Ops III names them. The names are target data
// (zonekit/profiles/t7/target/tool-materials.json), not code: the list of BO3 names (tested in Treyarch's stock prefabs, or a hypothesis from the tool images),
// and per source lineage the mapping of that game's own tool names onto them (docs/hydrax-port/tools-veg.md). Pure logic; the Radiant writer decides where it applies.

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Maps
{
    enum class ToolKind
    {
        Unchanged,   // the name is a BO3 tool name (tested or hypothesis)
        Mapped,      // the lineage's table names the BO3 tool
        Rule,        // clip_<kind>_<surface> -> <surface>_clip[_<kind>], a BO3 name
        Fallback,    // the surface dropped, or the nearest base tool by prefix: reported (a lineage that defines them only)
        Unlisted,    // not a BO3 name and the game has no lineage rules: kept as it is, reported
        Drop         // the brush or mesh is not written
    };

    struct ToolResult
    {
        std::string Name;   // the BO3 name (empty for a drop)
        ToolKind Kind = ToolKind::Unchanged;
    };

    class ToolMaterials
    {
    public:
        // The target file's text; false (Why) when it does not read or holds no BO3 name
        bool Parse(const std::string& Json, std::string& Why);
        // The file from the exe's folder upward (zonekit/profiles/t7/target/tool-materials.json); loaded once, shared. Null (Why) when it is not found or unreadable.
        static const ToolMaterials* Shared(std::string& Why);

        // The lineage of a game id ("mwr" -> "iw"); empty when the game has none
        std::string LineageOf(const std::string& Game) const;
        // Name (any case) as the BO3 tool it becomes. Lineage may be empty. A name never leaves with a name outside the lists, except a drop.
        ToolResult Resolve(const std::string& Lineage, const std::string& Name) const;
        // Whether Name (any case) is a tool name for certain: a key of the lineage's table, a drop name of it, or a BO3 name. A render surface is a tool only then (a drawn material
        // such as skyline_brick merely starts like one).
        bool Exact(const std::string& Lineage, const std::string& Name) const;
        bool IsBo3(const std::string& Lower) const { return Tested.count(Lower) != 0 || Hypothesis.count(Lower) != 0; }
        bool IsTested(const std::string& Lower) const { return Tested.count(Lower) != 0; }

        std::set<std::string> Tested, Hypothesis;
        std::vector<std::string> Surfaces;

    private:
        struct Lineage
        {
            std::set<std::string> Games, Drop, DropPrefixes;
            std::map<std::string, std::string> Map;
            bool Rule = false;   // the clip_<kind>_<surface> rule and the surface drop
            std::vector<std::pair<std::string, std::string>> Prefixes;
            std::string SuffixClip;
        };
        std::map<std::string, Lineage> Lineages;
    };

    // What one export did with its tool names, and where to apply the table. Table null: names pass through, nothing is counted (the writer's tests build it by hand).
    struct ToolUse
    {
        const ToolMaterials* Table = nullptr;
        std::string Lineage;
        // by "from -> to" (Mapped, Rule, Fallback), and "from" (Unchanged, Dropped)
        std::map<std::string, size_t> Mapped, Rule, Fallback, Unchanged, Unlisted, Dropped;
        size_t BrushesDropped = 0, MeshesDropped = 0, SkyBrushesDropped = 0;

        // The BO3 name of a tool name, not counted
        ToolResult Peek(const std::string& Name) const;
        // Peek, counted
        ToolResult Apply(const std::string& Name);
        // The report: a line per kind, the names with their counts
        std::vector<std::string> Lines() const;
        // The faces, meshes and patches whose name changed or dropped
        size_t Changed() const;
    };

    int ToolMaterialsSelfTest(std::string& Failures);
}
