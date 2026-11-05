#include "stdafx.h"

// See Bo3Target.Placement.h.

#include "Bo3Target.Placement.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>

namespace Bo3Target::Placement
{
    using GdtDb::StockIndex;
    using GdtDb::StockMatch;

    // ---- small helpers -------------------------------------------------------------------------

    namespace
    {
        std::string Lower(std::string_view Text)
        {
            std::string Out(Text);
            for (char& C : Out)
            {
                if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
            }
            return Out;
        }

        bool StartsWithNoCase(std::string_view Text, std::string_view Prefix)
        {
            return Text.size() >= Prefix.size() && Lower(Text.substr(0, Prefix.size())) == Prefix;
        }

        std::vector<std::string> Tokens(std::string_view LowerName)
        {
            std::vector<std::string> Out;
            size_t Start = 0;
            while (Start <= LowerName.size())
            {
                size_t End = LowerName.find('_', Start);
                if (End == std::string_view::npos) End = LowerName.size();
                if (End > Start) Out.emplace_back(LowerName.substr(Start, End - Start));
                Start = End + 1;
            }
            return Out;
        }

        std::string JoinTokens(const std::vector<std::string>& Parts, size_t Count)
        {
            std::string Out;
            for (size_t I = 0; I < Count && I < Parts.size(); ++I)
            {
                if (I != 0) Out += '_';
                Out += Parts[I];
            }
            return Out;
        }

        bool Has(const std::vector<std::string>& Parts, std::string_view Word)
        {
            return std::find(Parts.begin(), Parts.end(), Word) != Parts.end();
        }

        std::string Join(std::string_view Folder, std::string_view File)
        {
            if (Folder.empty()) return std::string(File);
            while (!Folder.empty() && Folder.back() == '\\') Folder.remove_suffix(1);
            return std::string(Folder) + "\\" + std::string(File);
        }

        // a name as one file name: separators become '_'
        std::string SafeFile(std::string_view Name)
        {
            std::string Out(Name);
            for (char& C : Out)
            {
                if (C == '/' || C == '\\') C = '_';
            }
            return Out;
        }

        // A relative path: '/' to '\', no empty, rooted, drive or ".." segment; false when unsafe.
        bool CleanRelative(std::string_view Name, std::string& Out)
        {
            Out.clear();
            std::string Text(Name);
            for (char& C : Text)
            {
                if (C == '/') C = '\\';
            }
            size_t Start = 0;
            while (Start <= Text.size())
            {
                size_t End = Text.find('\\', Start);
                if (End == std::string::npos) End = Text.size();
                const std::string Part = Text.substr(Start, End - Start);
                if (Part == "..") return false;
                if (Part.find(':') != std::string::npos) return false;
                if (!Part.empty() && Part != ".")
                {
                    if (!Out.empty()) Out += '\\';
                    Out += Part;
                }
                else if (Part.empty() && Start == 0 && !Text.empty()) return false;   // rooted
                Start = End + 1;
            }
            return !Out.empty();
        }

        std::string LeafOf(std::string_view Name)
        {
            const size_t Cut = Name.find_last_of("/\\");
            return std::string(Cut == std::string_view::npos ? Name : Name.substr(Cut + 1));
        }

        // ---- kinds ----------------------------------------------------------------------------

        bool IsRawKind(const std::string& Kind)
        {
            static const std::set<std::string> Raw = {
                "rawfile", "rawtextfile", "stringtable", "structuredtable", "localize", "ttf", "fonticon",
                "animselectortable", "animmappingtable", "animstatemachine", "behaviortree", "behaviorstatemachine" };
            return Raw.count(Kind) != 0;
        }

        // data kinds with one gdt per kind (attachment and cosmetic variants have their own rule, P6)
        bool IsDataKind(const std::string& Kind)
        {
            static const std::set<std::string> Data = {
                "fog", "rumble", "physpreset", "physconstraints", "surfacesounddef", "surfacefxtable", "scriptbundle",
                "customizationcolor", "customizationtable", "entitysoundimpacts", "entityfximpacts", "laser", "beam",
                "tracer", "aimtable", "impactsfxtable", "impactsoundstable", "footsteptable", "playersoundstable",
                "playerfxtable", "sharedweaponsounds", "maptable", "shellshock", "flametable", "tagfx" };
            return Data.count(Kind) != 0;
        }

        bool IsWeaponKind(const std::string& Kind) { return Kind.ends_with("weapon"); }   // bulletweapon, meleeweapon, weapon...

        // the family class and rank of a kind (HydraX FamilyClassOf); an empty class never joins a family
        struct ClassRank { std::string Class; int Rank = 5; };
        ClassRank ClassOf(const std::string& Kind)
        {
            if (IsWeaponKind(Kind)) return { "weapon", 0 };
            if (Kind == "attachmentunique") return { "weapon", 1 };
            if (Kind == "xmodel") return { "model", 0 };
            if (Kind == "material") return { "model", 1 };
            if (Kind == "image") return { "model", 2 };
            if (Kind == "weaponcamotable") return { "camo", 0 };
            if (Kind == "weaponcamo") return { "camo", 1 };
            return {};
        }

        // ---- names ----------------------------------------------------------------------------

        // The slot letter of a stock picture name (c n g o m s r e t): i_<material>_<slot> is filed under
        // the material when it has another '_' left.
        std::string StripSlotSuffix(const std::string& Stem)
        {
            const size_t Cut = Stem.rfind('_');
            if (Cut == std::string::npos || Cut == 0 || Stem.size() - Cut != 2) return Stem;
            static const std::string_view Slots = "cngomsret";
            const char Slot = static_cast<char>(Stem.back() >= 'A' && Stem.back() <= 'Z' ? Stem.back() - 'A' + 'a' : Stem.back());
            if (Slots.find(Slot) == std::string_view::npos) return Stem;
            return Stem.substr(0, Cut).find('_') != std::string::npos ? Stem.substr(0, Cut) : Stem;
        }

        // The name the model, material and image rules read: the leaf, without i_ (image) and mtl_.
        std::string ModelStem(const std::string& Kind, std::string_view Name)
        {
            std::string Stem = LeafOf(Name);
            if (Kind == "image" && StartsWithNoCase(Stem, "i_")) Stem = StripSlotSuffix(Stem.substr(2));
            if (StartsWithNoCase(Stem, "mtl_")) Stem = Stem.substr(4);
            return SafeFile(Stem);
        }

        std::string GameOf(const Request& Q, const std::string& Kind)
        {
            std::string Game = NameGameOfStem(ModelStem(Kind, Q.Name));
            if (!Game.empty()) return Game;
            Game = GameToken(Q.GameId);
            return Game.empty() ? Lower(Q.GameId) : Game;
        }

        // The texture library gdt (t7_wood, t7_decal_grunge, t7_micro_materials, t7_graphics_materials) a
        // <game>_<surface>_... name belongs to, or empty when the name is not a library name.
        std::string Library(std::string_view Name, const std::string& Game)
        {
            const std::vector<std::string> Parts = Tokens(Lower(Name));
            const size_t Start = !Parts.empty() && Parts[0] == "i" ? 1 : 0;
            if (Parts.size() < Start + 2 || Parts[Start] != Game) return {};
            const std::string& Surface = Parts[Start + 1];
            if (Surface == "decal" && Parts.size() > Start + 2) return Game + "_decal_" + Parts[Start + 2];
            if (Surface == "micro") return Game + "_micro_materials";
            if (Surface == "graphic" || Surface == "graphics") return Game + "_graphics_materials";
            return Game + "_" + Surface;
        }

        // The folder of a family's files when the placement names none (HydraX ImageFilePlacement.FolderOf).
        std::string FilesFolderOf(const std::string& Gdt, const std::string& FallbackGame)
        {
            const size_t Slash = Gdt.rfind('\\');
            const std::string Folder = Slash == std::string::npos ? std::string() : Gdt.substr(0, Slash);
            std::string Stem = Slash == std::string::npos ? Gdt : Gdt.substr(Slash + 1);
            if (Stem.ends_with(".gdt")) Stem.resize(Stem.size() - 4);
            if (Folder.empty()) return Stem;
            if (Lower(Folder) == "texture_assets")
            {
                std::string Game = NameGameOfStem(Stem);
                if (Game.empty()) Game = FallbackGame.empty() ? "t7" : FallbackGame;
                return "texture_assets\\" + Game + "_materials";
            }
            return Folder.find('\\') != std::string::npos ? Folder : Folder + "\\" + Stem;
        }

        // The weapon's name without its mode suffix: smg_standard_mp is smg_standard; dualoptic_ is dropped.
        std::string WeaponBase(const std::string& Name)
        {
            std::string Stripped = Name;
            if (StartsWithNoCase(Stripped, "dualoptic_") && Stripped.size() > 10) Stripped = Stripped.substr(10);
            const size_t Cut = Stripped.rfind('_');
            if (Cut != std::string::npos)
            {
                static const std::set<std::string> Modes = { "mp", "zm", "cp", "sp", "wz" };
                if (Modes.count(Lower(Stripped.substr(Cut + 1))) != 0) Stripped.resize(Cut);
            }
            return SafeFile(Stripped);
        }

        bool AllDigits(std::string_view Text)
        {
            return !Text.empty() && std::all_of(Text.begin(), Text.end(), [](char C) { return C >= '0' && C <= '9'; });
        }

        // A trailing variant token of a prop name: digits, one letter, or a letter and digits (01, a, v2).
        bool IsVariantToken(std::string_view Token)
        {
            if (AllDigits(Token) || Token.size() == 1) return true;
            return Token.size() <= 3 && Token[0] >= 'a' && Token[0] <= 'z' && AllDigits(Token.substr(1));
        }

        // What the rules choose before the stock redirect and the files folder default.
        struct Pick
        {
            std::string Gdt;
            std::string Files;      // empty: derive from the gdt
            std::string Rule;
            std::string Why;
            bool Raw = false;
            bool Rejected = false;
        };

        Pick MakePick(std::string Gdt, std::string Files, std::string Rule, std::string Why)
        {
            Pick P;
            P.Gdt = std::move(Gdt);
            P.Files = std::move(Files);
            P.Rule = std::move(Rule);
            P.Why = std::move(Why);
            return P;
        }

        // the texture library placement of an image or material of the given game
        Pick LibraryPick(const std::string& Kind, std::string_view Name, const std::string& Game, std::string Rule, const std::string& Why)
        {
            std::string Library_ = Library(ModelStem(Kind, Name), Game);
            if (Library_.empty()) Library_ = Game + "_materials";
            return MakePick(Join("texture_assets", Library_ + ".gdt"), "texture_assets\\" + Game + "_materials", std::move(Rule), Why);
        }

        Pick Choose(const Request& Q, const StockIndex& Stock, const Options& Use, int Depth);

        // ---- the shelves ---------------------------------------------------------------------------

        Pick ShelfPick(const std::string& Kind, std::string_view Name, const std::string& Title, const std::string& Shelf, std::string Rule, const std::string& Why)
        {
            if (Kind == "xmodel")
            {
                // one gdt per model: a shared shelf file kept 1 of 187 hash-named models
                const std::string Folder = "model_export\\" + Shelf + "\\" + Title;
                return MakePick(Join(Folder, SafeFile(LeafOf(Name)) + ".gdt"), Folder, std::move(Rule), Why + ": the model shelf, one gdt per model");
            }
            if (Kind == "material" || Kind == "image")
            {
                const std::string Folder = "texture_assets\\" + Shelf + "\\" + Title;
                return MakePick(Join(Folder, Kind + ".gdt"), Folder, std::move(Rule), Why + ": the " + Kind + " shelf");
            }
            return MakePick("source_data\\" + Shelf + "\\" + Title + "\\" + Kind + ".gdt", {}, std::move(Rule), Why + ": the data shelf");
        }

        // ---- model naming (P7 to P11, skybox, props) ---------------------------------------------------

        bool FxanimMode(const std::vector<std::string>& Parts, std::string& Mode)
        {
            static const std::set<std::string> Modes = { "gp", "cp", "mp", "zm", "bc" };
            const auto At = std::find(Parts.begin(), Parts.end(), "fxanim");
            if (At == Parts.end() || At + 1 == Parts.end() || Modes.count(*(At + 1)) == 0) return false;
            Mode = *(At + 1);
            return true;
        }

        const std::set<std::string>& WeaponClasses()
        {
            static const std::set<std::string> Classes = { "ar", "smg", "lmg", "sniper", "shotgun", "pistol", "launcher", "knife", "melee", "special", "crossbow" };
            return Classes;
        }

        std::string CharacterGroup(const std::vector<std::string>& Parts, const std::string& Game)
        {
            if (Parts[1] == Game && Parts.size() >= 4 && Parts[2] == "mp") return "mp_" + Parts[3];
            static const std::map<std::string, std::string> Groups = {
                { "civ", "civilian" }, { "hro", "heroes" }, { "spc", "special" }, { "zom", "zombie" },
                { "zsf", "zurich" }, { "ger", "german" }, { "usa", "us_gi" } };
            const auto It = Groups.find(Parts[1]);
            return It == Groups.end() ? Parts[1] : It->second;
        }

        // P7 to P10 and the skybox rule for a model-like kind; false when none applies
        bool ModelRule(const Request& Q, const std::string& Kind, const std::string& Game, const StockIndex& Stock, const Options& Use, Pick& Out)
        {
            const std::string Stem = ModelStem(Kind, Q.Name);
            const std::vector<std::string> Parts = Tokens(Lower(Stem));
            std::string Mode;
            if (FxanimMode(Parts, Mode))
                { Out = MakePick("model_export\\" + Game + "_fxanim_" + Mode + ".gdt", {}, "P7", "fxanim " + Mode); return true; }
            if (Kind == "scriptbundle") return false;
            const std::string Low = Lower(Stem);
            if (Parts.size() >= 2 && (StartsWithNoCase(Low, "c_") || StartsWithNoCase(Low, "pbt_")))
            {
                const std::string Group = CharacterGroup(Parts, Game);
                Out = MakePick("model_export\\" + Game + "_characters_" + Group + ".gdt", "model_export\\" + Game + "_characters\\" + Group, "P8", "character group " + Group);
                return true;
            }
            if (StartsWithNoCase(Low, "veh_" + Game + "_"))
            {
                std::string Gdt = "model_export\\" + JoinTokens(Parts, 4) + ".gdt";
                std::string Why = "vehicle name (first four tokens)";
                if (Use.UseStockClosest)
                {
                    // the longest stock token prefix whose busiest gdt sits directly in model_export and is named like the name's start
                    StockMatch Match;
                    GdtDb::StockExclusion Left{ {}, Lower(Q.LeaveOutGdt) };
                    const size_t Slash = Left.Gdt.rfind('\\');
                    if (Slash != std::string::npos) Left.Folder = Left.Gdt.substr(0, Slash);
                    if (Stock.Closest(Kind, Q.Name, Match, Q.LeaveOutGdt.empty() ? nullptr : &Left) && Match.Folder == "model_export"
                        && Match.Gdt.size() > 14 && Match.Gdt.ends_with(".gdt") && Match.Gdt.starts_with("model_export\\veh_"))
                    {
                        const std::string Stock_Stem = Match.Gdt.substr(13, Match.Gdt.size() - 13 - 4);
                        if (Low == Stock_Stem || Low.starts_with(Stock_Stem + "_"))
                        {
                            Gdt = Match.Gdt;
                            Why = "vehicle: the stock gdt " + Match.Gdt + " of the longest token prefix";
                        }
                    }
                }
                Out = MakePick(std::move(Gdt), {}, "P9", std::move(Why));
                return true;
            }
            if (StartsWithNoCase(Low, "wpn_" + Game + "_") || StartsWithNoCase(Low, "attach_" + Game + "_"))
            {
                const std::vector<std::string> Rest(Parts.begin() + (std::min<size_t>)(2, Parts.size()), Parts.end());
                const bool Zombie = !Rest.empty() && Rest[0] == "zmb";
                const size_t Take = Rest.size() >= 2 && WeaponClasses().count(Rest[0]) != 0 ? 2 : (std::min<size_t>)(1, Rest.size());
                std::string Gdt = Zombie ? "wpn_" + Game + "_zmb_weapons" : "wpn_" + Game + "_" + JoinTokens(Rest, Take);
                while (!Gdt.empty() && Gdt.back() == '_') Gdt.pop_back();
                const std::string Weapon = Take == 2 ? Rest[1] : (Zombie && Rest.size() >= 2 ? Rest[1] : (!Rest.empty() ? Rest[0] : "weapon"));
                Out = MakePick("model_export\\" + Gdt + ".gdt", "model_export\\weapons_" + Game + "\\" + Weapon, "P10", "weapon model " + Weapon);
                return true;
            }
            if (Has(Parts, "skybox"))
            {
                Out = MakePick("model_export\\" + Game + "_skybox.gdt", {}, "P16", "skybox");
                return true;
            }
            return false;
        }

        // P15 and P16: the prop set of a model (named after the name without its trailing variant tokens)
        Pick PropPick(const std::string& Kind, std::string_view Name, const std::string& Game)
        {
            const std::string Stem = ModelStem(Kind, Name);
            std::vector<std::string> Parts = Tokens(Lower(Stem));
            bool Zombie = false;
            static const std::set<std::string> ZombieTokens = { "zm", "zmb", "zom", "zombie", "zod" };
            for (size_t I = 0; I < Parts.size() && I < 4; ++I) Zombie = Zombie || ZombieTokens.count(Parts[I]) != 0;
            while (Parts.size() > 2 && IsVariantToken(Parts.back())) Parts.pop_back();
            const std::string Set = Parts.empty() ? Lower(Stem) : JoinTokens(Parts, Parts.size());
            const std::string Folder = "model_export\\" + Game + (Zombie ? "_props_zombie" : "_props") + "\\" + Set;
            return MakePick(Join(Folder, Set + ".gdt"), Folder, Zombie ? "P15" : "P16", std::string(Zombie ? "zombie prop set " : "prop set ") + Set);
        }

        // P11
        Pick AnimPick(std::string_view Name, const std::string& Game)
        {
            const std::vector<std::string> Parts = Tokens(Lower(LeafOf(Name)));
            std::string Mode;
            if (FxanimMode(Parts, Mode)) return MakePick("model_export\\" + Game + "_fxanim_" + Mode + ".gdt", {}, "P7", "fxanim " + Mode);
            if (Parts.size() >= 3 && Parts[0] == "vm")
                return MakePick("xanim_export\\" + Game + "_xanim_viewmodel.gdt", "xanim_export\\" + Game + "_viewmodels\\" + Parts[1], "P11", "viewmodel animation");
            if (Parts.size() >= 3 && Parts[0] == "ai")
                return MakePick("xanim_export\\" + Game + "_ai_" + Parts[1] + ".gdt", "xanim_export\\ai\\" + Parts[1], "P11", "ai animation");
            return MakePick("xanim_export\\" + Game + "_xanim_" + (Parts.empty() ? "misc" : Parts[0]) + ".gdt", {}, "P11", "animation group");
        }

        // ---- Choose --------------------------------------------------------------------------------

        Pick Choose(const Request& Q, const StockIndex& Stock, const Options& Use, int Depth)
        {
            const std::string Kind = Lower(Q.Kind);
            const std::string Game = GameOf(Q, Kind);
            std::string Title = Lower(Q.GameId);
            if (Title.empty()) Title = Game;

            // P1 raw kinds: share\raw\<name>
            if (IsRawKind(Kind))
            {
                std::string Relative;
                if (!CleanRelative(Q.Name, Relative))
                {
                    Pick Bad = MakePick({}, {}, "P1", "unsafe raw path " + Q.Name);
                    Bad.Raw = Bad.Rejected = true;
                    return Bad;
                }
                const bool Unresolved = IsUnresolvedName(Q.GameId, LeafOf(Q.Name));
                Pick P = Unresolved ? MakePick("share\\raw\\graywolf_unresolved\\" + Title + "\\" + Relative, {}, "P1", "raw output with an unresolved hash name " + Q.Name)
                                    : MakePick("share\\raw\\" + Relative, {}, "P1", "raw output at its share\\raw path (the name carries the path)");
                P.Raw = true;
                return P;
            }

            const ClassRank Mine = ClassOf(Kind);
            std::string RootKind = Lower(Q.FamilyRootKind);
            const bool HasRoot = Depth == 0 && !Q.FamilyRoot.empty();
            if (HasRoot && RootKind.empty())
            {
                RootKind = Kind == "attachmentunique" ? "weapon" : Kind == "weaponcamo" ? "weaponcamotable" : "xmodel";
            }
            const auto FollowRoot = [&](const char* Rule, const std::string& Why) -> Pick
            {
                Request RootQ;
                RootQ.Kind = RootKind;
                RootQ.Name = Q.FamilyRoot;
                RootQ.GameId = Q.GameId;
                RootQ.LeaveOutGdt = Q.LeaveOutGdt;
                Pick P = Choose(RootQ, Stock, Use, Depth + 1);
                P.Why = Why + " (" + P.Rule + ": " + P.Why + ")";
                P.Rule = Rule;
                return P;
            };

            // P2 a child of its family's root in the same class takes the root's gdt (weapon and camo families)
            if (HasRoot && !Mine.Class.empty() && Mine.Class != "model" && ClassOf(RootKind).Class == Mine.Class)
                return FollowRoot("P2", "child " + Kind + " " + Q.Name + " follows its root " + RootKind + " " + Q.FamilyRoot);

            const bool Texture = Kind == "image" || Kind == "material";
            const bool ModelRoot = HasRoot && Texture && ClassOf(RootKind).Class == "model" && !IsUnresolvedName(Q.GameId, Q.FamilyRoot);

            // P18 unresolved hash names go to the unresolved shelf, unless the asset joins a resolved model
            if (!ModelRoot && IsUnresolvedName(Q.GameId, LeafOf(Q.Name)))
                return ShelfPick(Kind, Q.Name, Title, "graywolf_unresolved", "P18.unresolved", "unresolved hash name " + Q.Name);

            // P3 to P6 weapons, attachments, camos
            if (IsWeaponKind(Kind))
                return MakePick("source_data\\" + WeaponBase(Q.Name) + ".gdt", {}, "P3", "weapon base name");
            if (Kind == "attachmentunique")
            {
                std::string Stripped = Q.Name;
                if (StartsWithNoCase(Stripped, "au_")) Stripped = Stripped.substr(3);
                const size_t Underscores = static_cast<size_t>(std::count(Stripped.begin(), Stripped.end(), '_'));
                const std::string Weapon = Underscores >= 2 ? Stripped.substr(0, Stripped.rfind('_')) : Stripped;
                return MakePick("source_data\\" + WeaponBase(Weapon) + ".gdt", {}, "P4", "weapon base name of the attachment's weapon");
            }
            if (Kind == "weaponcamo" || Kind == "weaponcamotable")
            {
                if (Kind == "weaponcamotable") return MakePick("source_data\\wpn_" + Game + "_camo.gdt", {}, "P5", "camo table");
                const std::string Prefix = "camo_" + Game + "_";
                std::string Rest = StartsWithNoCase(Q.Name, Prefix) ? Q.Name.substr(Prefix.size()) : StartsWithNoCase(Q.Name, "camo_") ? Q.Name.substr(5) : Q.Name;
                return MakePick("source_data\\wpn_" + Game + "_camo_" + SafeFile(Rest) + ".gdt", {}, "P5", "camo name");
            }
            if (Kind == "attachment" || Kind == "attachmentcosmeticvariant")
                return MakePick("source_data\\wpn_" + Game + "_attachments.gdt", {}, "P6", "weapon attachments of the game");
            if (IsDataKind(Kind))
            {
                std::string Mode;
                if (Kind == "scriptbundle" && FxanimMode(Tokens(Lower(Q.Name)), Mode))
                    return MakePick("model_export\\" + Game + "_fxanim_" + Mode + ".gdt", {}, "P7", "fxanim " + Mode);
                return MakePick("source_data\\" + Kind + ".gdt", {}, "P6.data", "one gdt per data kind");
            }

            // P7 to P10 models; P11 animations
            if (Kind == "xanim") return AnimPick(Q.Name, Game);
            const bool ModelLike = Kind == "xmodel" || Kind == "sanim";
            if (ModelLike)
            {
                Pick P;
                if (ModelRule(Q, Kind, Game, Stock, Use, P)) return P;
            }

            // P12 texture library: a library name, or an image used by several families
            if (Texture)
            {
                const std::string Stem = ModelStem(Kind, Q.Name);
                const bool Shared = Kind == "image" && Q.FamilyCount >= 2;
                if (!Library(Stem, Game).empty())
                    return LibraryPick(Kind, Q.Name, Game, "P12", "library name " + Library(Stem, Game));
                if (Shared)
                    return LibraryPick(Kind, Q.Name, Game, "P12", "image used by " + std::to_string(Q.FamilyCount) + " families: the texture library");

                // P13 follows the family's model: mtl_ materials and their images share the model's gdt
                if (ModelRoot)
                {
                    const std::string ImageGame = Kind == "image" ? NameGameOfStem(Stem) : std::string();
                    Request RootQ;
                    RootQ.Kind = RootKind;
                    RootQ.Name = Q.FamilyRoot;
                    RootQ.GameId = Q.GameId;
                    const std::string RootGame = GameOf(RootQ, RootKind);
                    if (!ImageGame.empty() && ImageGame != RootGame)
                        return LibraryPick(Kind, Q.Name, ImageGame, "P12", "image of game " + ImageGame + " in a " + RootGame + " family: its own game's texture library");
                    return FollowRoot("P13", Kind + " " + Q.Name + " follows its model " + Q.FamilyRoot);
                }

                // No model holds it: a texture of the game's library. The stock match is looked up for the reason, but a
                // stock model folder is never joined with a texture gdt name (HydraX put 58 files in model folders that way).
                StockMatch Match;
                GdtDb::StockExclusion Left{ {}, Lower(Q.LeaveOutGdt) };
                const size_t Slash = Left.Gdt.rfind('\\');
                if (Slash != std::string::npos) Left.Folder = Left.Gdt.substr(0, Slash);
                if (Use.UseStockClosest && Stock.Closest(Kind, Q.Name, Match, Q.LeaveOutGdt.empty() ? nullptr : &Left))
                {
                    const bool InTextures = Match.Folder == "texture_assets" || Match.Folder.starts_with("texture_assets\\");
                    Pick P = LibraryPick(Kind, Q.Name, Game, InTextures ? "P17" : "P12", "texture not tied to a model");
                    P.Why += InTextures ? ": stock match " + Match.Entity + " (" + std::to_string(Match.Tokens) + " tokens) in " + Match.Folder
                                        : ": the stock match " + Match.Entity + " lies in " + Match.Folder + ", not a texture folder, so the texture library is used";
                    return P;
                }
                return LibraryPick(Kind, Q.Name, Game, "P12", "texture not tied to a model: the game's material library");
            }

            // P17 the stock folder of the longest token prefix (stock rows only), then the prop sets
            const std::string Stem = ModelStem(Kind, Q.Name);
            const std::string File = ModelLike ? Lower(Stem) + ".gdt" : SafeFile(Q.Name) + ".gdt";
            if (Use.UseStockClosest)
            {
                StockMatch Match;
                GdtDb::StockExclusion Left{ {}, Lower(Q.LeaveOutGdt) };
                const size_t Slash = Left.Gdt.rfind('\\');
                if (Slash != std::string::npos) Left.Folder = Left.Gdt.substr(0, Slash);
                if (Stock.Closest(Kind, Q.Name, Match, Q.LeaveOutGdt.empty() ? nullptr : &Left))
                {
                    // a model's gdt in a stock folder is named after the folder (the stock <set>\<set>.gdt), not the model
                    const std::string Leaf = LeafOf(Match.Folder);
                    const std::string Path = Join(Match.Folder, ModelLike && !Leaf.empty() ? Lower(Leaf) + ".gdt" : File);
                    return MakePick(Path, {}, "P17", "stock match " + Match.Entity + " (" + std::to_string(Match.Tokens) + " tokens) in " + (Match.Folder.empty() ? "the root" : Match.Folder));
                }
            }
            if (ModelLike) return PropPick(Kind, Q.Name, Game);

            // P18 nothing inferable
            return ShelfPick(Kind, Q.Name, Title, "graywolf", "P18", "no rule for kind " + Kind);
        }
    }

    // ---- public: game tokens -------------------------------------------------------------------------

    const std::vector<GameEntry>& GameTable()
    {
        static const std::vector<GameEntry> Table = {
            { "waw", "t4", "Call of Duty: World at War" },
            { "waw-oat", "t4", "Call of Duty: World at War (OpenAssetTools)" },
            { "bo1", "t5", "Call of Duty: Black Ops" },
            { "bo1-oat", "t5", "Call of Duty: Black Ops (OpenAssetTools)" },
            { "bo2", "t6", "Call of Duty: Black Ops II" },
            { "bo2-oat", "t6", "Call of Duty: Black Ops II (OpenAssetTools)" },
            { "bo3", "t7", "Call of Duty: Black Ops III" },
            { "bo4", "t8", "Call of Duty: Black Ops 4" },
            { "coldwar", "t9", "Call of Duty: Black Ops Cold War" },
            { "bo6", "t10", "Call of Duty: Black Ops 6" },
            { "bo6sp", "t10", "Call of Duty: Black Ops 6 (campaign)" },
            { "bo7", "t11", "Call of Duty: Black Ops 7" },
            { "cod1", "iw1", "Call of Duty" },
            { "cod2", "iw2", "Call of Duty 2" },
            { "cod4", "iw3", "Call of Duty 4: Modern Warfare" },
            { "cod4-oat", "iw3", "Call of Duty 4: Modern Warfare (OpenAssetTools)" },
            { "mw2", "iw4", "Call of Duty: Modern Warfare 2" },
            { "mw2-oat", "iw4", "Call of Duty: Modern Warfare 2 (OpenAssetTools)" },
            { "mw3", "iw5", "Call of Duty: Modern Warfare 3" },
            { "mw3-oat", "iw5", "Call of Duty: Modern Warfare 3 (OpenAssetTools)" },
            { "ghosts", "iw6", "Call of Duty: Ghosts" },
            { "iw", "iw7", "Call of Duty: Infinite Warfare" },
            { "mw2019", "iw8", "Call of Duty: Modern Warfare (2019)" },
            { "mwiii", "iw9", "Call of Duty: Modern Warfare III" },
            { "mwiii_sp", "iw9", "Call of Duty: Modern Warfare III (campaign)" },
            { "aw", "s1", "Call of Duty: Advanced Warfare" },
            { "ww2", "s2", "Call of Duty: WWII" },
            { "mwr", "h1", "Call of Duty: Modern Warfare Remastered" },
            { "codol", "ol", "Call of Duty Online" },
        };
        return Table;
    }

    std::string GameToken(std::string_view GameId)
    {
        const std::string Id = Lower(GameId);
        for (const GameEntry& Entry : GameTable())
        {
            if (Id == Entry.GameId) return Entry.Token;
        }
        return {};
    }

    std::string NameGameOfStem(std::string_view Stem)
    {
        if (Stem.size() > 3 && (Stem[0] == 'p' || Stem[0] == 'P') && (Stem[1] == '7' || Stem[1] == '8' || Stem[1] == '9') && Stem[2] == '_')
            return std::string("t") + Stem[1];
        // the first three '_' tokens (empty ones skipped)
        size_t Seen = 0, Start = 0;
        while (Start <= Stem.size() && Seen < 3)
        {
            size_t End = Stem.find('_', Start);
            if (End == std::string_view::npos) End = Stem.size();
            if (End > Start)
            {
                ++Seen;
                const std::string Token = Lower(Stem.substr(Start, End - Start));
                const bool T = Token.size() >= 2 && Token.size() <= 3 && Token[0] == 't' && AllDigits(std::string_view(Token).substr(1))
                    && std::stoi(Token.substr(1)) >= 4 && std::stoi(Token.substr(1)) <= 11;
                const bool Iw = Token.size() == 3 && Token[0] == 'i' && Token[1] == 'w' && Token[2] >= '1' && Token[2] <= '9';
                if (T || Iw) return Token;
            }
            Start = End + 1;
        }
        return {};
    }

    bool IsUnresolvedName(std::string_view GameId, std::string_view Name)
    {
        const std::string Id = Lower(GameId);
        if (Id != "bo4" && Id != "coldwar") return false;
        const size_t Cut = Name.find('_');
        if (Cut == std::string_view::npos || Cut == 0) return false;
        const std::string_view Prefix = Name.substr(0, Cut), Tail = Name.substr(Cut + 1);
        if (Prefix[0] < 'a' || Prefix[0] > 'z') return false;
        for (const char C : Prefix)
        {
            if (!((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'))) return false;
        }
        if (AllDigits(Tail) && Tail.size() >= 10 && Tail.size() <= 20) return true;
        if (Tail.size() < 8 || Tail.size() > 16) return false;
        bool Digit = false;
        for (const char C : Tail)
        {
            if (C >= '0' && C <= '9') Digit = true;
            else if (!(C >= 'a' && C <= 'f')) return false;
        }
        return Digit;
    }

    // ---- public: names -------------------------------------------------------------------------------

    std::string Shorten(std::string_view Name)
    {
        if (Name.size() <= MaxEntryNameLength) return std::string(Name);
        uint32_t Hash = 0x811C9DC5u;
        for (const char C : Name)
        {
            const unsigned char B = static_cast<unsigned char>(C >= 'A' && C <= 'Z' ? C - 'A' + 'a' : C);
            Hash = (Hash ^ B) * 0x01000193u;
        }
        return std::string(Name.substr(0, 55)) + "_" + std::format("{:07x}", Hash & 0x0FFFFFFFu);
    }

    std::string ImageFileStem(std::string_view ImageName)
    {
        std::string Stem = SafeFile(ImageName);
        if (Stem.size() > 2 && StartsWithNoCase(Stem, "i_")) Stem = Stem.substr(2);
        return Stem;
    }

    std::string ImageFileName(std::string_view ImageName, bool Sky)
    {
        return ImageFileStem(ImageName) + (Sky ? ".exr" : ".tif");
    }

    std::string ImagePath(std::string_view FilesFolder, std::string_view ImageName, bool Sky)
    {
        return Join(FilesFolder, ImageFileName(ImageName, Sky));
    }

    std::string BaseImageValue(std::string_view FilesFolder, std::string_view ImageName, bool Sky)
    {
        std::string Out;
        for (const char C : ImagePath(FilesFolder, ImageName, Sky))
        {
            Out += C;
            if (C == '\\') Out += '\\';
        }
        return Out;
    }

    // ---- public: slots ---------------------------------------------------------------------------------

    bool SlotFolder(Slot Which, std::string& Folder)
    {
        switch (Which)
        {
        case Slot::ExportRoot: Folder = "share\\raw"; return true;
        case Slot::MapEnts: Folder = "map_source\\map_ents"; return true;
        case Slot::MapGeometry: Folder = "map_source\\graywolf_maps"; return true;
        case Slot::SourceData: Folder = "source_data"; return true;
        case Slot::Sound:
        case Slot::SoundZone:
        case Slot::SoundMusic: Folder = "share\\raw\\sound"; return true;
        case Slot::AnimationTables: Folder = "share\\raw\\animtables"; return true;
        case Slot::AnimationStateMachines: Folder = "share\\raw\\animstatemachines"; return true;
        case Slot::Behavior: Folder = "share\\raw\\behavior"; return true;
        case Slot::XAnimRaw: Folder = "share\\raw\\xanim_raw"; return true;
        case Slot::Rumble: Folder = "share\\raw\\rumble"; return true;
        case Slot::SAnim: Folder = "xanim_export\\graywolf\\sanim"; return true;
        case Slot::XCams: Folder = "xanim_export\\graywolf_xcams"; return true;
        case Slot::Placed: Folder.clear(); return true;
        case Slot::ModelGdts:
        case Slot::GdtSource: break;
        }
        Folder.clear();
        return false;
    }

    std::string SoundSubFolder(std::string_view FileName)
    {
        const std::string Low = Lower(FileName);
        if (Low.ends_with(".alias.csv")) return "aliases";
        if (Low.ends_with(".ambient.csv")) return "ambients";
        if (Low.ends_with(".reverb.csv")) return "reverb";
        if (Low.ends_with(".musiclist.csv") || Low.ends_with(".mus")) return "music";
        return {};
    }

    std::optional<std::string> SlotPath(Slot Which, std::string_view RelativeName)
    {
        std::string Folder;
        if (!SlotFolder(Which, Folder)) return std::nullopt;
        std::string Relative;
        if (!CleanRelative(RelativeName, Relative)) return std::nullopt;
        if (Which == Slot::Sound || Which == Slot::SoundZone || Which == Slot::SoundMusic)
        {
            const std::string Sub = SoundSubFolder(Relative);
            if (!Sub.empty()) Folder += "\\" + Sub;
        }
        return Join(Folder, Relative);
    }

    // ---- public: Place ----------------------------------------------------------------------------------

    Result Place(const Request& Asked, const StockIndex& Stock, const Options& Use)
    {
        const Pick P = Choose(Asked, Stock, Use, 0);
        Result Out;
        Out.Rule = P.Rule;
        Out.Reason = P.Rule + ": " + P.Why;
        Out.Raw = P.Raw;
        Out.Rejected = P.Rejected;
        Out.NaturalGdtPath = P.Gdt;
        Out.GdtPath = P.Gdt;
        if (P.Rejected || P.Raw) return Out;

        const std::string Kind = Lower(Asked.Kind);
        Out.FilesFolder = !P.Files.empty() ? P.Files : FilesFolderOf(P.Gdt, GameOf(Asked, Kind));
        // a destination that is a gdt this tool does not own (stock, a re-saved library gdt, the user's own: the unfiltered set, not the layout
        // filter) becomes graywolf_<file>.gdt in the same folder
        if (Stock.IsHeldGdtPath(P.Gdt))
        {
            const size_t Slash = P.Gdt.rfind('\\');
            Out.GdtPath = Slash == std::string::npos ? "graywolf_" + P.Gdt : P.Gdt.substr(0, Slash + 1) + "graywolf_" + P.Gdt.substr(Slash + 1);
            Out.Redirected = true;
            Out.Reason += " [gdt path held by the install: redirected to graywolf_" + Out.GdtPath.substr(Out.GdtPath.rfind('\\') + 1 + 9) + "]";
        }
        return Out;
    }

    // ---- self-test --------------------------------------------------------------------------------------

    namespace
    {
        class NoFileTimes final : public GdtDb::IFileTimes
        {
        public:
            bool ModifiedSeconds(const std::string&, int64_t&) override { return false; }
        };
        class MapFileTimes final : public GdtDb::IFileTimes
        {
        public:
            std::map<std::string, int64_t> Times;
            bool ModifiedSeconds(const std::string& Path, int64_t& Seconds) override
            {
                const auto It = Times.find(Path);
                if (It == Times.end()) return false;
                Seconds = It->second;
                return true;
            }
        };
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got)
        {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL Placement {} (got '{}')\n", Name, Got);
        };

        // A synthetic install: stock rows, plus rows that are not stock.
        const std::vector<GdtDb::Row> Rows = {
            { 1, "p7_crate_a", "xmodel", "model_export\\t7_props\\p7_crate\\p7_crate.gdt" },
            { 2, "p7_crate_b", "xmodel", "model_export\\t7_props\\p7_crate\\p7_crate.gdt" },
            { 3, "t7_metal_rust", "material", "texture_assets\\t7_metal.gdt" },
            { 4, "i_t7_metal_rust_c", "image", "texture_assets\\t7_metal.gdt" },
            { 5, "p7_onlyhydrax_a", "xmodel", "model_export\\t7_props\\p7_onlyhydrax\\hydrax_p7_onlyhydrax.gdt" },
            { 6, "veh_t7_tank_main", "xmodel", "model_export\\veh_t7_tank.gdt" },
            { 7, "i_p7_crate_a_c", "image", "model_export\\t7_props\\p7_crate\\p7_crate.gdt" },
            { 8, "i_wall_paint_c", "image", "texture_assets\\t7_walls.gdt" },
            { 9, "i_p7_crate_a_n", "image", "model_export\\t7_props\\p7_crate\\p7_crate.gdt" },
        };
        NoFileTimes Times;
        GdtDb::StockRules Rules;
        const GdtDb::StockIndex Stock(Rows, Rules, Times);
        const auto Do = [&](const char* Kind, const char* Name, const char* Game = "bo3", const char* Root = "", const char* RootKind = "", int Count = 0)
        {
            Request Q;
            Q.Kind = Kind;
            Q.Name = Name;
            Q.GameId = Game;
            Q.FamilyRoot = Root;
            Q.FamilyRootKind = RootKind;
            Q.FamilyCount = Count;
            return Place(Q, Stock);
        };
        const auto Is = [&](const Result& R, const char* Rule, const char* Natural, const char* Name)
        {
            Check(R.Rule == Rule && R.NaturalGdtPath == Natural, Name, R.Rule + " " + R.NaturalGdtPath + " | " + R.Reason);
        };

        // one example per rule
        Result R = Do("rawfile", "scripts/zm/foo.gsc");
        Check(R.Rule == "P1" && R.Raw && R.GdtPath == "share\\raw\\scripts\\zm\\foo.gsc", "P1 raw kind", R.GdtPath);
        R = Do("rawfile", "../escape.gsc");
        Check(R.Rejected && R.GdtPath.empty(), "P1 raw path outside the root is rejected", R.GdtPath);
        R = Do("weaponcamo", "camo_t7_gold", "bo3", "camotable_main", "weaponcamotable");
        Is(R, "P2", "source_data\\wpn_t7_camo.gdt", "P2 child takes the root's gdt");
        Is(Do("bulletweapon", "smg_standard_mp"), "P3", "source_data\\smg_standard.gdt", "P3 weapon base name");
        Is(Do("bulletweapon", "dualoptic_smg_standard_mp"), "P3", "source_data\\smg_standard.gdt", "P3 dualoptic is dropped");
        Is(Do("attachmentunique", "au_smg_standard_mp_silencer"), "P4", "source_data\\smg_standard.gdt", "P4 attachmentunique joins its weapon");
        Is(Do("weaponcamo", "camo_t7_gold"), "P5", "source_data\\wpn_t7_camo_gold.gdt", "P5 camo variant");
        Is(Do("weaponcamotable", "camotable_main"), "P5", "source_data\\wpn_t7_camo.gdt", "P5 camo table");
        Is(Do("attachment", "silencer"), "P6", "source_data\\wpn_t7_attachments.gdt", "P6 attachments");
        Is(Do("fog", "zm_fog_a"), "P6.data", "source_data\\fog.gdt", "P6 data kind");
        Is(Do("xmodel", "p7_fxanim_zm_gate_01"), "P7", "model_export\\t7_fxanim_zm.gdt", "P7 fxanim mode");
        R = Do("xmodel", "c_zom_player_x");
        Check(R.Rule == "P8" && R.NaturalGdtPath == "model_export\\t7_characters_zombie.gdt" && R.FilesFolder == "model_export\\t7_characters\\zombie", "P8 character group", R.NaturalGdtPath + " " + R.FilesFolder);
        Is(Do("xmodel", "veh_t7_tank_gun"), "P9", "model_export\\veh_t7_tank.gdt", "P9 vehicle: the stock gdt of the longest prefix");
        Is(Do("xmodel", "veh_t7_newcar_door"), "P9", "model_export\\veh_t7_newcar_door.gdt", "P9 vehicle: first four tokens without a stock match");
        R = Do("xmodel", "wpn_t7_smg_ap9_mag");
        Check(R.Rule == "P10" && R.NaturalGdtPath == "model_export\\wpn_t7_smg_ap9.gdt" && R.FilesFolder == "model_export\\weapons_t7\\ap9", "P10 weapon model", R.NaturalGdtPath + " " + R.FilesFolder);
        R = Do("xanim", "vm_ar_standard_idle");
        Check(R.Rule == "P11" && R.NaturalGdtPath == "xanim_export\\t7_xanim_viewmodel.gdt" && R.FilesFolder == "xanim_export\\t7_viewmodels\\ar", "P11 viewmodel animation", R.NaturalGdtPath + " " + R.FilesFolder);
        R = Do("image", "i_t7_decal_grunge_wood_scratches_c");
        Check(R.Rule == "P12" && R.GdtPath == "texture_assets\\t7_decal_grunge.gdt" && R.FilesFolder == "texture_assets\\t7_materials", "P12 library decal image", R.GdtPath + " " + R.FilesFolder);
        R = Do("image", "i_mtl_p7_odd_c", "bo3", "", "", 3);
        Check(R.Rule == "P12" && R.GdtPath == "texture_assets\\t7_materials.gdt", "P12 image shared by several families", R.GdtPath);
        R = Do("material", "mtl_p7_crate_x", "bo3", "p7_crate_x", "xmodel");
        Check(R.Rule == "P13" && R.NaturalGdtPath == "model_export\\t7_props\\p7_crate\\p7_crate.gdt", "P13 material follows its model", R.NaturalGdtPath + " | " + R.Reason);
        Is(Do("xmodel", "p7_zm_tra_bench_01"), "P15", "model_export\\t7_props_zombie\\p7_zm_tra_bench\\p7_zm_tra_bench.gdt", "P15 zombie prop set");
        Is(Do("xmodel", "p7_widget_big_02"), "P16", "model_export\\t7_props\\p7_widget_big\\p7_widget_big.gdt", "P16 prop set drops the variant token");
        Is(Do("xmodel", "p7_crate_c"), "P17", "model_export\\t7_props\\p7_crate\\p7_crate.gdt", "P17 stock closest: gdt named after the stock folder");
        R = Do("zzzkind", "foo_bar");
        Check(R.Rule == "P18" && R.GdtPath == "source_data\\graywolf\\bo3\\zzzkind.gdt", "P18 last resort shelf is graywolf", R.GdtPath);

        // fix (a): the stock-closest branch never joins a stock model folder with a texture gdt name
        R = Do("image", "i_p7_crate_z_c");
        Check(R.GdtPath == "texture_assets\\t7_materials.gdt" && R.GdtPath.find("model_export") == std::string::npos, "fix a: orphan image whose stock match is a model folder goes to the texture library", R.GdtPath + " | " + R.Reason);
        R = Do("material", "mtl_p7_crate_zz");
        Check(R.GdtPath == "texture_assets\\t7_materials.gdt", "fix a: orphan material whose stock match is a model folder goes to the texture library", R.GdtPath);
        R = Do("image", "i_wall_paint_new_c");
        Check(R.Rule == "P17" && R.GdtPath == "texture_assets\\t7_materials.gdt", "fix a: a stock match inside texture_assets is used", R.Rule + " " + R.GdtPath);
        // fix (b): Closest uses stock rows only (the hydrax_ gdt row is not stock)
        R = Do("xmodel", "p7_onlyhydrax_b");
        Check(R.Rule == "P16", "fix b: a non-stock sibling does not feed the stock match", R.Rule + " " + R.NaturalGdtPath);
        // fix (c): a stock-path destination redirects, shelves are graywolf
        R = Do("image", "i_t7_metal_rust_c");
        Check(R.Redirected && R.NaturalGdtPath == "texture_assets\\t7_metal.gdt" && R.GdtPath == "texture_assets\\graywolf_t7_metal.gdt", "fix c: a stock gdt path redirects to graywolf_<file>", R.GdtPath);
        R = Do("bulletweapon", "smg_new_mp");
        Check(!R.Redirected && R.GdtPath == "source_data\\smg_new.gdt", "fix c: a path that is not stock is kept", R.GdtPath);
        // fix (e): a re-saved library gdt and the user's own are not stock (the layout filter) but are not ours either: the redirect still applies
        {
            constexpr int64_t Ref = 1'755'000'000, Day = 86400;
            const std::vector<GdtDb::Row> Held = {
                { 10, "t7_wall_clean", "material", "texture_assets\\t7_wall.gdt" },                      // in the install window: sets the bound
                { 9000, "t7_metal_rust", "material", "texture_assets\\t7_metal.gdt" },                   // re-saved weeks later: renumbered, not stock
                { 9001, "i_t7_metal_rust_c", "image", "texture_assets\\t7_metal.gdt" },
                { 9500, "t8_custom_thing", "material", "texture_assets\\t8_materials\\t8.gdt" },       // the user's own
            };
            MapFileTimes HeldTimes;
            HeldTimes.Times["texture_assets\\t7_wall.gdt"] = Ref;
            HeldTimes.Times["texture_assets\\t7_metal.gdt"] = Ref + 40 * Day;
            HeldTimes.Times["texture_assets\\t8_materials\\t8.gdt"] = Ref + 60 * Day;
            GdtDb::StockRules HeldRules;
            HeldRules.ReferenceSeconds = Ref;
            const GdtDb::StockIndex HeldStock(Held, HeldRules, HeldTimes);
            Check(!HeldStock.IsStockGdtPath("texture_assets\\t7_metal.gdt") && HeldStock.IsHeldGdtPath("texture_assets\\t7_metal.gdt") && HeldStock.IsHeldGdtPath("texture_assets\\t8_materials\\t8.gdt"),
                  "fix e: the re-saved and the user's gdt are not stock but are held", "");
            Request Q;
            Q.Kind = "image";
            Q.Name = "i_t7_metal_rust_d";
            Q.GameId = "bo3";
            const Result Re = Place(Q, HeldStock);
            Check(Re.NaturalGdtPath == "texture_assets\\t7_metal.gdt" && Re.Redirected && Re.GdtPath == "texture_assets\\graywolf_t7_metal.gdt",
                  "fix e: a new image aimed at a re-saved library gdt goes to graywolf_<file>.gdt, not into the owner's file", Re.NaturalGdtPath + " -> " + Re.GdtPath);
            Request U = Q;
            U.Name = "i_t7_metal_rust_c";
            Check(Place(U, HeldStock).GdtPath.find("graywolf_") != std::string::npos || Place(U, HeldStock).GdtPath != "texture_assets\\t7_metal.gdt",
                  "fix e: no output is ever aimed at the re-saved file itself", Place(U, HeldStock).GdtPath);
        }
        // fix (d): each hash-named model gets its own gdt on the graywolf unresolved shelf
        const Result A = Do("xmodel", "xmodel_1a2b3c4d5e6f", "bo4"), B = Do("xmodel", "xmodel_ffee00112233", "bo4");
        Check(A.Rule == "P18.unresolved" && A.GdtPath == "model_export\\graywolf_unresolved\\bo4\\xmodel_1a2b3c4d5e6f.gdt" && A.GdtPath != B.GdtPath, "fix d: hash-named models each have their own gdt", A.GdtPath + " | " + B.GdtPath);
        R = Do("rawfile", "gsc_1a2b3c4d5e6f.gsc", "bo4");
        Check(R.Raw && R.GdtPath == "share\\raw\\gsc_1a2b3c4d5e6f.gsc", "a raw name with an extension is not an unresolved hash name", R.GdtPath);
        Check(IsUnresolvedName("coldwar", "weapon_12345678901") && !IsUnresolvedName("bo3", "weapon_12345678901") && !IsUnresolvedName("bo4", "weapon_abcdefab"), "unresolved hash names are BO4/CW only and need a digit", "");

        // name shortening: first 55, '_', 7 hex of the lower-cased FNV-1a (checked against an independent computation)
        const std::string Long = "Mtl_P7_Zm_Tra_Some_Really_Long_Material_Name_For_A_Dirty_Decal_Overlay_Version_Two";
        Check(Shorten(Long) == "Mtl_P7_Zm_Tra_Some_Really_Long_Material_Name_For_A_Dirt_e821834" && Shorten(Long).size() == 63, "shorten over 63 characters", Shorten(Long));
        Check(Shorten("short_name") == "short_name" && Shorten(std::string(63, 'a')) == std::string(63, 'a'), "63 characters or fewer are kept", "");

        // image file rules
        Check(ImageFileStem("i_t7_decal_grunge_wood_scratches_c") == "t7_decal_grunge_wood_scratches_c", "image stem drops i_", ImageFileStem("i_t7_decal_grunge_wood_scratches_c"));
        Check(ImageFileStem("mc\\i_a_b") == "mc_i_a_b" && ImageFileStem("i_") == "i_", "image stem replaces separators; a bare i_ stays", ImageFileStem("mc\\i_a_b"));
        Check(ImageFileName("i_sky_x", false) == "sky_x.tif" && ImageFileName("i_sky_x", true) == "sky_x.exr", "image extension tif, exr for sky", "");
        Check(BaseImageValue("model_export\\t7_props\\p7_crate", "i_p7_crate_a_c", false) == "model_export\\\\t7_props\\\\p7_crate\\\\p7_crate_a_c.tif", "baseImage doubles the backslashes", BaseImageValue("model_export\\t7_props\\p7_crate", "i_p7_crate_a_c", false));

        // slots
        Check(SlotPath(Slot::Sound, "zm_x.alias.csv") == std::optional<std::string>("share\\raw\\sound\\aliases\\zm_x.alias.csv"), "slot: sound alias sub-folder", "");
        Check(SlotPath(Slot::SoundMusic, "a.musiclist.csv") == std::optional<std::string>("share\\raw\\sound\\music\\a.musiclist.csv"), "slot: music sub-folder", "");
        Check(SlotPath(Slot::AnimationTables, "x/y.csv") == std::optional<std::string>("share\\raw\\animtables\\x\\y.csv"), "slot: animtables", "");
        Check(SlotPath(Slot::SourceData, "ssi_zm_x.gdt") == std::optional<std::string>("source_data\\ssi_zm_x.gdt"), "slot: source data", "");
        Check(!SlotPath(Slot::GdtSource, "a.gdt") && !SlotPath(Slot::ModelGdts, "a.gdt") && !SlotPath(Slot::ExportRoot, "..\\a"), "slot: no folder or unsafe name", "");

        // game tokens: Treyarch and the others, names win
        Check(GameToken("bo3") == "t7" && GameToken("BO4") == "t8" && GameToken("coldwar") == "t9" && GameToken("bo6") == "t10" && GameToken("bo7") == "t11", "game tokens, Treyarch", "");
        Check(GameToken("ww2") == "s2" && GameToken("mwr") == "h1" && GameToken("iw") == "iw7" && GameToken("mw2019") == "iw8" && GameToken("nosuchgame").empty(), "game tokens, the others", "");
        Check(NameGameOfStem("p8_foo_bar") == "t8" && NameGameOfStem("veh_t9_glass_x") == "t9" && NameGameOfStem("wpn_iw8_x") == "iw8" && NameGameOfStem("plain_name_here").empty(), "a game token in the name wins", "");
        R = Do("xmodel", "c_ger_soldier_head", "ww2");
        Check(R.Rule == "P8" && R.NaturalGdtPath == "model_export\\s2_characters_german.gdt", "WWII character uses the s2 token", R.NaturalGdtPath);
        R = Do("xmodel", "wpn_s2_ppsh_body", "ww2");
        Check(R.Rule == "P10" && R.NaturalGdtPath == "model_export\\wpn_s2_ppsh.gdt", "WWII weapon model uses the s2 token", R.NaturalGdtPath);
        R = Do("xmodel", "p7_zm_tra_bench_01", "bo4");
        Check(R.NaturalGdtPath.starts_with("model_export\\t7_props_zombie\\"), "a p7_ prop in a BO4 export is a t7 prop", R.NaturalGdtPath);
        R = Do("xmodel", "rock_big_03", "mw2019");
        Check(R.Rule == "P16" && R.NaturalGdtPath == "model_export\\iw8_props\\rock_big\\rock_big.gdt", "mw2019 prop uses the iw8 token", R.NaturalGdtPath);
        R = Do("image", "i_iw8_stone_wall_c", "bo3");
        Check(R.Rule == "P12" && R.GdtPath == "texture_assets\\iw8_stone.gdt" && R.FilesFolder == "texture_assets\\iw8_materials", "an iw8 image in a BO3 export goes to the iw8 library", R.GdtPath + " " + R.FilesFolder);
        return Failed;
    }
}
