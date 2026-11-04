#pragma once

// A reader for the Mod Tools' deffiles\*.awi (APE's asset-definition scripts), for the bo3-verify gate.
// The files are AngelScript: GenerateUI() calls Asset.AddEntry_<Kind>( key, ... ) for each field. This reads those calls
// as data and nothing else: no script is run. What it learns per key: the kind, min/max, the combo's options, the
// referenced asset type and the relative path of a file field.
//
// Limits (by design, stated so a caller never reads "unknown" as "valid"):
//   - variables are scoped per function (a name reused in another function does not union its options); the file's top-level
//     statements are the shared scope;
//   - an entry declared inside a branch or a loop is taken as declared (a key seen anywhere counts as existing); two
//     declarations of one key are merged (the widest min/max, the union of the options);
//   - a key built from a loop variable ("cg" + Digit + "_x") becomes a pattern, '*' standing for the unknown part;
//   - a combo whose options come from a function call (GetTechsetdefTypes) has OptionsKnown false: nothing can be said
//     about its values from the file.
// Nothing is copied into the source: the schema is whatever the install's files say at run time.

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Bo3Target::Awi
{
    enum class Kind { Float, Int, CheckBox, Combo, AssetCombo, Texture, String, Path, Text, Color, XModel, FileCombo, BoneCombo, Other };
    const char* KindName(Kind K);

    struct Entry
    {
        std::string Key;                // exact key, or a pattern with '*' when Pattern
        bool Pattern = false;
        Kind K = Kind::Other;
        bool HasMin = false, HasMax = false;
        double Min = 0, Max = 0;
        bool RangeUnknown = false;       // a min or max is given as an expression this reader cannot resolve: the range is NOT known (not "no range")
        bool HasDefault = false;         // Float/Int/Vector components: the default the declaration gives
        double Default = 0;
        bool OptionsKnown = false;
        std::vector<std::string> Options;   // the values a combo may hold (a "name{value}" option gives both), "*" markers removed
        std::string Target;             // AssetCombo: the asset type (GDF name) the value names
        std::string RelativePath;       // XModel/Path/Texture: SetRelativePath(...), "" if none
        int Declarations = 0;
    };

    class Schema
    {
    public:
        // Reads the text of one .awi. Returns false only when it holds no AddEntry call at all (not an awi).
        bool Parse(std::string_view Text, std::string& Error);
        // Exact key first, then the patterns. Null when the schema does not declare the key.
        const Entry* Find(const std::string& Key) const;
        size_t Size() const { return Exact.size() + Patterns.size(); }
        size_t PatternCount() const { return Patterns.size(); }
        // true when the option list of the key is known and holds Value (a trailing '*' on either side is ignored)
        static bool HasOption(const Entry& E, std::string_view Value);

    private:
        std::map<std::string, Entry> Exact;
        std::vector<Entry> Patterns;
    };

    // <Root>\deffiles\<gdf>.awi. False with Error when the file is not there or not an awi.
    bool Load(const std::filesystem::path& Root, const std::string& Gdf, Schema& Out, std::string& Error);

    int SelfTest(std::string& Failures);
}
