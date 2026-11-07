#include "stdafx.h"

// The Assets search box's grammar, as HydraX's search has it (docs/uno-ui.md): space-separated terms,
// all of which must match: term (the name contains it), key:term (that column), "term" (exact),
// ! (not), and several exact terms of one key mean any of them. Columns here: name, type, game, format.
#include "ImGuiShell.Internal.h"
#include "ExportNaming.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace ShellUi
{
    namespace Search
    {
        namespace
        {
            std::string Lowered(const std::string& Text) { return ExportNaming::Lower(Text); }

            const char* TypeName(int Kind)
            {
                static const char* Names[] = { "model", "image", "audio", "anim", "map", "fx", "text" };
                return Kind >= 0 && Kind < 7 ? Names[Kind] : "";
            }

            bool IsKey(const std::string& Key) { return Key == "name" || Key == "type" || Key == "game" || Key == "format"; }

            // the text of the column a term names (lowered by the caller)
            std::string Column(const std::string& Key, const Item& It)
            {
                if (Key == "type") return TypeName(It.Kind);
                if (Key == "game") return It.Game;
                if (Key == "format") return It.Format;
                return It.Name;
            }
        }

        Query Parse(const std::string& Text)
        {
            Query Q;
            size_t i = 0;
            while (i < Text.size())
            {
                while (i < Text.size() && std::isspace((unsigned char)Text[i])) i++;
                if (i >= Text.size()) break;
                Term T;
                T.Begin = i;
                if (Text[i] == '!') { T.Negated = true; i++; }
                // key:  (only a known column's name straight before a colon, so "a:b" in a name, or a colon in
                // a later term, still searches for the text)
                size_t Letters = i;
                while (Letters < Text.size() && std::isalpha((unsigned char)Text[Letters])) Letters++;
                if (Letters > i && Letters < Text.size() && Text[Letters] == ':')
                {
                    const std::string Key = Lowered(Text.substr(i, Letters - i));
                    if (IsKey(Key))
                    {
                        T.Key = Key;
                        i = Letters + 1;
                        if (i < Text.size() && Text[i] == '!') { T.Negated = !T.Negated; i++; }   // type:!weapon
                    }
                }
                if (i < Text.size() && Text[i] == '"')
                {
                    T.Exact = true;
                    const size_t End = Text.find('"', i + 1);
                    T.Value = Lowered(Text.substr(i + 1, End == std::string::npos ? std::string::npos : End - i - 1));
                    i = End == std::string::npos ? Text.size() : End + 1;
                }
                else
                {
                    size_t j = i;
                    while (j < Text.size() && !std::isspace((unsigned char)Text[j])) j++;
                    T.Value = Lowered(Text.substr(i, j - i));
                    i = j;
                }
                if (T.Key.empty()) T.Key = "name";
                T.End = i;
                T.Empty = T.Value.empty();
                if (!T.Empty || T.Key != "name" || T.Negated) Q.Terms.push_back(std::move(T));   // "type:" and "!" are kept; blanks are not
            }
            return Q;
        }

        bool Matches(const Query& Q, const Item& It)
        {
            // exact, positive terms of one key are alternatives; everything else must hold
            std::vector<std::string> ExactKeys;
            for (const Term& T : Q.Terms)
            {
                if (T.Empty) continue;
                const std::string Text = Lowered(Column(T.Key, It));
                const bool Hit = T.Exact ? Text == T.Value : Text.find(T.Value) != std::string::npos;
                if (T.Negated) { if (Hit) return false; continue; }
                if (!T.Exact) { if (!Hit) return false; continue; }
                if (std::find(ExactKeys.begin(), ExactKeys.end(), T.Key) != ExactKeys.end()) continue;   // that key's group already judged
                ExactKeys.push_back(T.Key);
                bool Any = false;
                for (const Term& Other : Q.Terms)
                    if (Other.Exact && !Other.Negated && Other.Key == T.Key && Lowered(Column(Other.Key, It)) == Other.Value) Any = true;
                if (!Any) return false;
            }
            return true;
        }

        bool SelfTest(std::string& Report)
        {
            bool Ok = true;
            const auto Check = [&](bool Cond, const char* What) { if (!Cond) { Ok = false; Report += std::string("FAIL ") + What + "\n"; } };
            Item Crate; Crate.Name = "prop_crate_wood_01"; Crate.Kind = 0; Crate.Game = "Sample Game"; Crate.Format = "cast";
            Item Pic; Pic.Name = "prop_crate_wood_c"; Pic.Kind = 1; Pic.Game = "Sample Game"; Pic.Format = "dds";
            Item Gun; Gun.Name = "wpn_rifle_01"; Gun.Kind = 0; Gun.Game = "Other"; Gun.Format = "cast";
            const auto M = [](const char* Q, const Item& I) { return Matches(Parse(Q), I); };
            Check(M("crate", Crate) && !M("rifle", Crate), "a plain term is a contains on the name");
            Check(M("CRATE wood", Crate), "case-insensitive, terms ANDed");
            Check(!M("crate !wood", Crate) && M("crate !metal", Crate), "! negates");
            Check(M("type:image crate", Pic) && !M("type:image crate", Crate), "key:term picks the column");
            Check(M("type:!image crate", Crate) && !M("type:!image crate", Pic), "key:!term negates the column");
            Check(M("\"prop_crate_wood_01\"", Crate) && !M("\"prop_crate\"", Crate), "\"term\" is exact");
            Check(M("type:\"image\" type:\"model\" crate", Crate) && M("type:\"image\" type:\"model\" crate", Pic), "exact terms of one key are alternatives");
            Check(!M("type:\"audio\" type:\"fx\"", Crate), "no alternative holds");
            Check(M("game:sample format:cast", Crate) && !M("game:sample format:cast", Pic), "game and format columns");
            Item Odd; Odd.Name = "xa:by";
            Check(M("a:b", Odd), "an unknown key is searched as text");
            Check(Parse("   ").Terms.empty(), "blank has no terms");
            Check(M("type:", Crate) && Parse("type:").Terms.size() == 1, "a key with nothing after it matches everything and is kept");
            // the terms keep their place in the text, one cutting for the search and the Filters panel
            const std::string Odd2 = "a\"b type:image";
            const Query Cut = Parse(Odd2);
            Check(Cut.Terms.size() == 2 && Odd2.substr(Cut.Terms[0].Begin, Cut.Terms[0].End - Cut.Terms[0].Begin) == "a\"b" && Cut.Terms[1].Key == "type", "a quote inside a word does not open a quoted term");
            const Query Spaced = Parse("x \"two words\" type:\"image\"");
            Check(Spaced.Terms.size() == 3 && Spaced.Terms[1].Value == "two words" && Spaced.Terms[2].Exact, "a quoted term keeps its spaces");
            (void)Gun;
            return Ok;
        }
    }
}
