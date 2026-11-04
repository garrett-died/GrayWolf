// Games/T7/CliCommands.BO3.cpp - the Black Ops III commands: bo3-images, bo3-dump-xpak-entries,
// salsa20-selftest, bo3-decrypt-movie, bo3-gdtdb-info, bo3-placement-backtest.
#include <string>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "WraithX.h"
#include "Strings.h"

#include "Bo3ZoneImages.h"
#include "Bo3XpakDump.h"
#include "Bo3MovieDecrypt.h"
#include "Bo3MovieSalsa20.h"
#include "Bo3Target.GdtDb.h"
#include "Bo3Target.Placement.h"
#include "GameCommand.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <format>


// BO3 images, offline. OAT has no T7 zone loader, so this reads the fastfile itself and
// pulls the pixels out of the install's .xpak packages - see Bo3ZoneImages.h.
int RunBo3Images(const CliOptions& Options)
{
    // No --out means "tell me what is in there". Which families a game keeps its 2D icons
    // under is not guessable.
    if (Options.Out.empty())
    {
        std::string Report;
        const bool Ok = Bo3ListZoneImages(Options.Bo3Images, Options.Bo3Xpaks, Report);
        CliReport(Options, Report);
        return Ok ? 0 : 5;
    }

    WraithX::InitializeAPI();   // WIC, for the DDS -> PNG write
    const std::string OutDir = Options.Out;
    std::string Report;
    const size_t Written = Bo3ExportZoneImages(Options.Bo3Images, Options.Bo3Xpaks, OutDir,
                                               Options.Type, Report);
    CliReport(Options, Report);
    return Written ? 0 : 5;
}


// Dumps large .xpak entries (see Bo3XpakDump.h). --out sets the dump directory.
int RunBo3DumpXpakEntries(const CliOptions& Options)
{
    const auto XpakFiles = Strings::SplitString(Options.Bo3DumpXpakEntries, ',', true);
    const std::string OutDir = Options.Out.empty() ? "bo3_xpak_dump" : Options.Out;
    std::string Report;
    const size_t Written = Bo3DumpXpakEntries(XpakFiles, OutDir,
        (uint64_t)Options.Bo3XpakMinMB * 1024ull * 1024ull, Report);
    CliReport(Options, Report);
    return Written ? 0 : 5;
}


// Verifies the hand-written Salsa20 against a test vector independent of any BO3 file.
int RunSalsa20SelfTest(const CliOptions& Options)
{
    const bool Ok = Salsa20::SelfTest();
    CliReport(Options, std::format("result={}\n", Ok ? "ok" : "FAIL"));
    return Ok ? 0 : 5;
}


// BO3 cinematic decryption - see Bo3MovieDecrypt.h. The key is extracted fresh from the zone's
// own compiled GSC every run. --install sets the game install root, --out the output file.
int RunBo3DecryptMovie(const CliOptions& Options)
{
    std::string Report;
    const bool Ok = Bo3DecryptMovie(Options.Install, Options.Bo3DecryptMovie, Options.ActsExe,
                                    Options.ActsHandler, Options.Out, Report);
    CliReport(Options, Report);
    return Ok ? 0 : 5;
}

// ---- the subcommands (CliCommands.h)

void AddBo3Commands(CliCommandBuilders& B)
{
    B["bo3-images"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo3-images", "BO3 zone images -> PNG, pixels sourced from the .xpaks");
        Sub->add_option("file", Options.Bo3Images, "Fast file (.ff) to read image assets from")->required()->check(CLI::ExistingFile);
        Sub->add_option("--bo3-xpaks", Options.Bo3Xpaks, "Where the packages are (default: beside the ff)");
    };
    B["bo3-dump-xpak-entries"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo3-dump-xpak-entries",
            "Diagnostic: dump every entry >= --bo3-xpak-min-mb to --out (cinematic-encryption brute-force search)");
        Sub->add_option("files", Options.Bo3DumpXpakEntries, "Comma separated .xpak file list")->required();
        Sub->add_option("--bo3-xpak-min-mb", Options.Bo3XpakMinMB, "Size floor in MB (default 2)");
    };
    B["salsa20-selftest"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("salsa20-selftest", "Verify the hand-written Salsa20 cipher vs a test vector")
            ->callback([&Options] { Options.Salsa20SelfTest = true; });
    };
    B["bo3-decrypt-movie"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("bo3-decrypt-movie",
            "Decrypt a BO3 cinematic (Salsa20, key comes from the zone's own GSC, never hardcoded)");
        Sub->add_option("zone", Options.Bo3DecryptMovie, "Zone name whose cinematic to decrypt")->required();
    };
}

// ---- bo3-gdtdb-info (a GameCommand: Bo3Target.GdtDb.h)
//
// bo3-gdtdb-info <label> [--mod-tools-root DIR] [--names a,b:gdf,c] --report FILE: opens the Mod Tools
// gdt.db read-only, reports how long that took, the counts, the stock index (computed from the install,
// never a fixed number) and, for each name, whether it is present by type and by name. The positional
// is only a label ("-"); the root is --mod-tools-root, else --install, else TA_TOOLS_PATH, else the
// positional when it is a folder.
namespace
{
    using Clock = std::chrono::steady_clock;

    int RunGdtDbInfo(const CliOptions& Options, const std::string& Input)
    {
        namespace Db = Bo3Target::GdtDb;
        namespace fs = std::filesystem;
        std::string Out;
        const auto Fail = [&](const std::string& Why)
        {
            CliReport(Options, Out + "result=error\nreason=" + Why + "\n");
            return 1;
        };

        std::string RootText = Options.ModToolsRoot;
        if (RootText.empty()) RootText = Options.Install;
        if (RootText.empty())
        {
            char* Env = nullptr;
            size_t Length = 0;
            if (_dupenv_s(&Env, &Length, "TA_TOOLS_PATH") == 0 && Env != nullptr)
            {
                RootText = Env;
                free(Env);   // NOLINT: _dupenv_s allocates with malloc
            }
        }
        std::error_code Ec;
        if (RootText.empty() && fs::is_directory(Input, Ec)) RootText = Input;
        if (RootText.empty()) return Fail("no Mod Tools root: give --mod-tools-root, --install or set TA_TOOLS_PATH");
        const fs::path Root = fs::absolute(fs::path(RootText), Ec);
        Out += std::format("mod_tools_root={}\n", Root.string());

        const auto Begin = Clock::now();
        const auto Ms = [](Clock::time_point From) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - From).count(); };

        std::unique_ptr<Db::Database> Database;
        Db::Status Status = Db::Database::Open(Root, Database);
        if (!Status.Ok) return Fail(Status.Reason);
        Out += std::format("open_mode={}\nopen_ms={}\n", Database->OpenMode(), Ms(Begin));

        // The schema as the file declares it, and the table sizes
        Database->Select("SELECT name, sql FROM sqlite_master WHERE name IN ('_entity', '_gdf', '_gdt') ORDER BY name", 2,
            [&](const std::vector<Db::Database::Cell>& Cells)
            {
                std::string Sql = Cells[1].Text;
                for (char& C : Sql)
                {
                    if (C == '\r' || C == '\n' || C == '\t') C = ' ';
                }
                Out += std::format("schema_{}={}\n", Cells[0].Text, Sql);
            });
        for (const char* Table : { "_entity", "_gdf", "_gdt" })
        {
            Database->Select(std::string("SELECT COUNT(*) FROM ") + Table, 1,
                [&](const std::vector<Db::Database::Cell>& Cells) { Out += std::format("table_rows{}={}\n", Table, Cells[0].Int); });
        }

        const auto LoadStart = Clock::now();
        std::vector<Db::Row> Rows;
        bool Typed = false, HasStockInfo = false;
        Status = Database->Load(Rows, Typed, HasStockInfo);
        if (!Status.Ok) return Fail(Status.Reason);
        Db::Catalog Catalog;
        int64_t MinId = 0, MaxId = 0;
        for (const Db::Row& Entry : Rows)
        {
            Catalog.Add(Entry);
            if (MinId == 0 || Entry.Id < MinId) MinId = Entry.Id;
            MaxId = (std::max)(MaxId, Entry.Id);
        }
        Out += std::format("load_ms={}\nentities={}\ngdfs={}\ngdts={}\ntyped={}\nentity_id_min={}\nentity_id_max={}\n",
            Ms(LoadStart), Catalog.EntityCount(), Catalog.DistinctGdfs(), Catalog.DistinctGdts(), Typed ? "yes" : "no", MinId, MaxId);

        // Stock, from the install
        std::unique_ptr<Db::StockIndex> Stock;
        if (!HasStockInfo)
        {
            Out += "stock=unavailable (only a fallback query could run: no ids or gdt files)\n";
        }
        else
        {
            const auto StockStart = Clock::now();
            const fs::path Linker = Root / "bin" / "linker_modtools.exe";
            const auto Time = fs::last_write_time(Linker, Ec);
            if (Ec)
            {
                Out += "stock=unavailable (bin\\linker_modtools.exe not found)\n";
            }
            else
            {
                const auto Sys = std::chrono::clock_cast<std::chrono::system_clock>(Time);
                const int64_t Reference = std::chrono::duration_cast<std::chrono::seconds>(Sys.time_since_epoch()).count();
                std::unordered_set<std::string> Manifests;
                Db::Status ManifestStatus = Db::ReadManifestPaths(Root / "hydrax" / "manifest.json", Manifests);
                const size_t HydraxPaths = Manifests.size();
                if (ManifestStatus.Ok) ManifestStatus = Db::ReadManifestPaths(Root / "graywolf" / "manifest.json", Manifests);
                if (!ManifestStatus.Ok)
                {
                    Out += "stock=unavailable (" + ManifestStatus.Reason + ")\n";
                }
                else
                {
                    Db::StockRules Rules;
                    Rules.ReferenceSeconds = Reference;
                    Rules.RootPrefix = Root.string();
                    Rules.Manifests = &Manifests;
                    const auto Files = Db::MakeDiskFileTimes(Root);
                    Stock = std::make_unique<Db::StockIndex>(Rows, Rules, *Files);
                    Out += std::format("linker_modtools_mtime_utc_seconds={}\n", Reference);
                    Out += std::format("manifest_paths_hydrax={}\nmanifest_paths_total={}\n", HydraxPaths, Manifests.size());
                    Out += std::format("stock_ms={}\nstock_count={}\nhighest_stock_id={}\nstock_gdt_files={}\n",
                        Ms(StockStart), Stock->StockCount(), Stock->HighestStockId(), Stock->StockGdts);
                    // For comparison with the HydraX id bound (187,765, measured on one copy), not a rule here
                    size_t StockAtOrBelow = 0, StockAbove = 0, NotStockAtOrBelow = 0;
                    for (const Db::Row& Entry : Rows)
                    {
                        const bool IsStock = Stock->IsStock(Entry.Id);
                        if (IsStock && Entry.Id <= 187765) ++StockAtOrBelow;
                        else if (IsStock) ++StockAbove;
                        else if (Entry.Id <= 187765) ++NotStockAtOrBelow;
                    }
                    Out += std::format("compare_187765: stock_with_id_le={} stock_with_id_gt={} not_stock_with_id_le={}\n",
                        StockAtOrBelow, StockAbove, NotStockAtOrBelow);
                    // Report only: gdts saved more than a day after the linker. They are stock (the age does
                    // not decide); the list shows which stock gdts a naive date rule would have lost.
                    std::map<std::string, size_t> LateGdts;
                    for (const Db::Row& Entry : Rows)
                    {
                        const std::string Why = Stock->Explain(Entry);
                        if (Why.find(" saved_later") != std::string::npos) ++LateGdts[Why];
                    }
                    std::vector<std::pair<size_t, std::string>> Ranked;
                    for (const auto& [Why, Count] : LateGdts) Ranked.emplace_back(Count, Why);
                    std::sort(Ranked.begin(), Ranked.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
                    for (size_t I = 0; I < Ranked.size() && I < 12; ++I)
                        Out += std::format("saved_later_gdt rows={} {}\n", Ranked[I].first, Ranked[I].second);
                    // The gdts the file-name rule left out although no manifest lists them
                    std::map<std::string, size_t> Prefixed;
                    for (const Db::Row& Entry : Rows)
                    {
                        const std::string Why = Stock->Explain(Entry);
                        if (Why.starts_with("tool_prefix")) ++Prefixed[Why];
                    }
                    for (const auto& [Why, Count] : Prefixed) Out += std::format("tool_prefixed_gdt rows={} {}\n", Count, Why);
                    Out += std::format("stock_rule=gdt not in a manifest and file name without hydrax_/graywolf_ prefix\n"
                        "not_stock_no_gdt_path={}\nnot_stock_in_manifest={}\nnot_stock_tool_prefixed={}\n"
                        "report_only_rows_saved_later_than_1d_after_linker={}\nreport_only_rows_gdt_file_missing={}\n",
                        Stock->NoGdtFile, Stock->InManifest, Stock->ToolPrefixed, Stock->SavedLater, Stock->FileMissing);
                }
            }
        }

        // The names asked about: name or name:gdf
        for (const std::string& Item : SplitNameFilter(Options.WhereNames))
        {
            std::string Name = Item, Gdf;
            const size_t Colon = Item.find(':');
            if (Colon != std::string::npos)
            {
                Name = Item.substr(0, Colon);
                Gdf = Item.substr(Colon + 1);
            }
            std::string Types;
            for (const std::string& Type : Catalog.TypesOf(Name)) Types += (Types.empty() ? "" : "|") + Type;
            const bool ByName = Catalog.ContainsName(Name);
            std::string ByType = "n/a (no gdf given)";
            if (!Gdf.empty()) ByType = Catalog.Contains(Gdf, Name) ? "yes" : "no";
            else if (ByName) ByType = "n/a (no gdf given; present as " + Types + ")";

            std::string StockText = "n/a";
            std::string Closest = "none";
            if (Stock)
            {
                const std::string Normal = Db::NormaliseName(Name);
                bool Any = false;
                std::string FirstGdf;
                for (const Db::Row& Entry : Rows)
                {
                    if (Db::NormaliseName(Entry.Name) != Normal) continue;
                    if (FirstGdf.empty()) FirstGdf = Entry.Gdf;
                    if (Stock->IsStock(Entry.Id)) { Any = true; break; }
                }
                StockText = Any ? "yes" : "no";
                for (const Db::Row& Entry : Rows)
                {
                    if (Db::NormaliseName(Entry.Name) == Normal)
                        StockText += std::format(" [id={} {}]", Entry.Id, Stock->Explain(Entry));
                }
                Db::StockMatch Match;
                if (Stock->Closest(Gdf.empty() ? FirstGdf : Gdf, Name, Match))
                    Closest = std::format("{} (entity {}, {} tokens)", Match.Folder, Match.Entity, Match.Tokens);
            }
            Out += std::format("name={} gdf={} by_type={} by_name={} types={} is_stock_row={} closest_stock_folder={}\n",
                Name, Gdf.empty() ? "-" : Gdf, ByType, ByName ? "yes" : "no", Types.empty() ? "-" : Types, StockText, Closest);
        }

        Out += std::format("total_ms={}\nresult=ok\n", Ms(Begin));
        CliReport(Options, Out);
        return 0;
    }

    void AddGdtDbInfoOptions(CLI::App& Sub, CliOptions& Options)
    {
        // --mod-tools-root is a shared option (CliCommands.Shared.cpp)
        Sub.add_option("--names", Options.WhereNames, "Names to look up, comma-separated; name:gdf also tests the type");
    }

    const GameCommand GdtDbInfoCommand{
        "bo3-gdtdb-info",
        "Open the BO3 Mod Tools gdt.db read-only through its own sqlite64r.dll and report counts, the computed "
        "stock index and whether --names are present by type and by name",
        "A label (use -); the root is --mod-tools-root, --install or TA_TOOLS_PATH",
        RunGdtDbInfo,
        AddGdtDbInfoOptions,
    };
    const GameCommandRegistration GdtDbInfoRegistered(GdtDbInfoCommand);
}

// ---- bo3-placement-backtest (a GameCommand: Bo3Target.Placement.h)
//
// bo3-placement-backtest <label> [--mod-tools-root DIR] --report FILE: loads the real gdt.db read-only and,
// for every stock row, predicts the gdt path from the row's name, kind and game alone (no family: entity
// parents are not in the database) and compares it with where the row really is. Two runs: the name rules
// alone, and the name rules with the stock-closest branch (the row itself left out of Closest). Exact =
// same gdt path; folder = same folder, other file; miss = anything else. Nothing is written but the report;
// no rule is tuned to this data.
namespace
{
    struct BacktestCount
    {
        size_t Total = 0, ExactNames = 0, FolderNames = 0, ExactStock = 0, FolderStock = 0;
    };

    struct MissGroup
    {
        size_t Count = 0;
        std::string Example;
    };

    // the first two path components: model_export\t7_props
    std::string TopOf(const std::string& Path)
    {
        const size_t First = Path.find('\\');
        if (First == std::string::npos) return Path;
        const size_t Second = Path.find('\\', First + 1);
        return Second == std::string::npos ? Path.substr(0, First) : Path.substr(0, Second);
    }

    std::string FolderOfPath(const std::string& Path)
    {
        const size_t Slash = Path.rfind('\\');
        return Slash == std::string::npos ? std::string() : Path.substr(0, Slash);
    }

    std::string Percent(size_t Part, size_t Whole)
    {
        return Whole == 0 ? "n/a" : std::format("{:.1f}%", 100.0 * static_cast<double>(Part) / static_cast<double>(Whole));
    }

    int RunPlacementBacktest(const CliOptions& Options, const std::string& Input)
    {
        namespace Db = Bo3Target::GdtDb;
        namespace Pl = Bo3Target::Placement;
        namespace fs = std::filesystem;
        std::string Out;
        const auto Fail = [&](const std::string& Why)
        {
            CliReport(Options, Out + "result=error\nreason=" + Why + "\n");
            return 1;
        };

        std::string RootText = Options.ModToolsRoot;
        if (RootText.empty()) RootText = Options.Install;
        if (RootText.empty())
        {
            char* Env = nullptr;
            size_t Length = 0;
            if (_dupenv_s(&Env, &Length, "TA_TOOLS_PATH") == 0 && Env != nullptr)
            {
                RootText = Env;
                free(Env);   // NOLINT: _dupenv_s allocates with malloc
            }
        }
        std::error_code Ec;
        if (RootText.empty() && fs::is_directory(Input, Ec)) RootText = Input;
        if (RootText.empty()) return Fail("no Mod Tools root: give --mod-tools-root, --install or set TA_TOOLS_PATH");
        const fs::path Root = fs::absolute(fs::path(RootText), Ec);
        Out += std::format("mod_tools_root={}\n", Root.string());

        const auto Begin = Clock::now();
        const auto Ms = [&]() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - Begin).count(); };
        std::unique_ptr<Db::Database> Database;
        Db::Status Status = Db::Database::Open(Root, Database);
        if (!Status.Ok) return Fail(Status.Reason);
        std::vector<Db::Row> Rows;
        bool Typed = false, HasStockInfo = false;
        Status = Database->Load(Rows, Typed, HasStockInfo);
        if (!Status.Ok) return Fail(Status.Reason);
        if (!Typed || !HasStockInfo) return Fail("the database gave no types or gdt files; a backtest needs both");

        // the same stock index as bo3-gdtdb-info
        int64_t Reference = 0;
        {
            const auto Time = fs::last_write_time(Root / "bin" / "linker_modtools.exe", Ec);
            if (!Ec) Reference = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(Time).time_since_epoch()).count();
        }
        std::unordered_set<std::string> Manifests;
        Status = Db::ReadManifestPaths(Root / "hydrax" / "manifest.json", Manifests);
        if (Status.Ok) Status = Db::ReadManifestPaths(Root / "graywolf" / "manifest.json", Manifests);
        if (!Status.Ok) return Fail(Status.Reason);
        Db::StockRules Rules;
        Rules.ReferenceSeconds = Reference;
        Rules.RootPrefix = Root.string();
        Rules.Manifests = &Manifests;
        const auto Files = Db::MakeDiskFileTimes(Root);
        const Db::StockIndex Stock(Rows, Rules, *Files);
        Out += std::format("entities={}\nstock_rows={}\nstock_gdt_files={}\n", Rows.size(), Stock.StockCount(), Stock.StockGdts);
        Out += "family_data=none: entity parents are not in the database, so every row is predicted from name, kind and game alone\n"
               "game_id_used=bo3 (a name token such as p8_ or t8 still wins)\n"
               "leave_one_out=yes: the stock-closest run leaves the predicted row out of its own lookup\n"
               "hydrax_published: name_rules_alone_exact=10.2% (74,886 entities), with_family_rules_exact=68.8%; not directly comparable (HydraX counted entities, with its own family data)\n";

        std::map<std::string, BacktestCount> ByKind;
        std::map<std::string, BacktestCount> ByRule;     // the rule of the stock-closest run
        std::map<std::string, MissGroup> Misses;
        BacktestCount All;
        Pl::Options NamesOnly;
        NamesOnly.UseStockClosest = false;
        for (const Db::Row& Entry : Rows)
        {
            if (!Stock.IsStock(Entry.Id)) continue;
            const std::string Actual = Db::StockIndex::RelativeGdtPath(Entry.GdtFile, Root.string());
            if (Actual.empty()) continue;
            Pl::Request Q;
            for (const char C : Entry.Gdf) Q.Kind += static_cast<char>(C >= 'A' && C <= 'Z' ? C - 'A' + 'a' : C);
            Q.Name = Db::NormaliseName(Entry.Name);
            Q.GameId = "bo3";
            Q.LeaveOutGdt = Actual;
            const Pl::Result Names = Pl::Place(Q, Stock, NamesOnly);
            const Pl::Result Closest = Pl::Place(Q, Stock);
            const auto Lower = [](std::string Text) { for (char& C : Text) { if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a'); } return Text; };
            const std::string PredictedNames = Lower(Names.NaturalGdtPath), PredictedStock = Lower(Closest.NaturalGdtPath);
            const auto Add = [&](BacktestCount& Count)
            {
                ++Count.Total;
                if (PredictedNames == Actual) ++Count.ExactNames;
                else if (FolderOfPath(PredictedNames) == FolderOfPath(Actual)) ++Count.FolderNames;
                if (PredictedStock == Actual) ++Count.ExactStock;
                else if (FolderOfPath(PredictedStock) == FolderOfPath(Actual)) ++Count.FolderStock;
            };
            Add(All);
            Add(ByKind[Q.Kind]);
            BacktestCount& RuleCount = ByRule[Closest.Rule];
            ++RuleCount.Total;
            if (PredictedStock == Actual) ++RuleCount.ExactStock;
            else if (FolderOfPath(PredictedStock) == FolderOfPath(Actual)) ++RuleCount.FolderStock;
            if (PredictedStock != Actual && FolderOfPath(PredictedStock) != FolderOfPath(Actual))
            {
                MissGroup& Group = Misses[Q.Kind + " | " + Closest.Rule + " | predicted " + TopOf(PredictedStock) + " | actual " + TopOf(Actual)];
                if (Group.Count++ == 0) Group.Example = std::format("{} predicted {} actual {}", Q.Name, PredictedStock, Actual);
            }
        }

        Out += std::format("scored_rows={}\n", All.Total);
        Out += std::format("TOTAL names_only exact={} ({}) folder_only={} ({}) miss={} ({})\n", All.ExactNames, Percent(All.ExactNames, All.Total),
            All.FolderNames, Percent(All.FolderNames, All.Total), All.Total - All.ExactNames - All.FolderNames, Percent(All.Total - All.ExactNames - All.FolderNames, All.Total));
        Out += std::format("TOTAL with_stock_closest exact={} ({}) folder_only={} ({}) miss={} ({})\n", All.ExactStock, Percent(All.ExactStock, All.Total),
            All.FolderStock, Percent(All.FolderStock, All.Total), All.Total - All.ExactStock - All.FolderStock, Percent(All.Total - All.ExactStock - All.FolderStock, All.Total));

        const auto Emit = [&](const char* Label, const std::map<std::string, BacktestCount>& Map, bool NamesToo)
        {
            std::vector<std::pair<std::string, BacktestCount>> Sorted(Map.begin(), Map.end());
            std::sort(Sorted.begin(), Sorted.end(), [](const auto& A, const auto& B) { return A.second.Total != B.second.Total ? A.second.Total > B.second.Total : A.first < B.first; });
            for (const auto& [Key, C] : Sorted)
            {
                if (NamesToo)
                    Out += std::format("{} {} n={} names_only exact={} folder={} miss={} | stock_closest exact={} folder={} miss={}\n", Label, Key, C.Total,
                        C.ExactNames, C.FolderNames, C.Total - C.ExactNames - C.FolderNames, C.ExactStock, C.FolderStock, C.Total - C.ExactStock - C.FolderStock);
                else
                    Out += std::format("{} {} n={} stock_closest exact={} ({}) folder={} miss={}\n", Label, Key, C.Total,
                        C.ExactStock, Percent(C.ExactStock, C.Total), C.FolderStock, C.Total - C.ExactStock - C.FolderStock);
            }
        };
        Emit("kind", ByKind, true);
        Emit("rule", ByRule, false);

        std::vector<std::pair<size_t, std::string>> Ranked;
        for (const auto& [Key, Group] : Misses) Ranked.emplace_back(Group.Count, Key + "\n    e.g. " + Group.Example);
        std::sort(Ranked.begin(), Ranked.end(), [](const auto& A, const auto& B) { return A.first != B.first ? A.first > B.first : A.second < B.second; });
        Out += std::format("miss_groups_total={} (kind | rule | predicted top folder | actual top folder, stock-closest run)\n", Ranked.size());
        for (size_t I = 0; I < Ranked.size() && I < 15; ++I) Out += std::format("miss_group {} rows={} {}\n", I + 1, Ranked[I].first, Ranked[I].second);

        Out += std::format("total_ms={}\nresult=ok\n", Ms());
        CliReport(Options, Out);
        return 0;
    }

    void AddPlacementBacktestOptions(CLI::App& Sub, CliOptions& Options)
    {
        // --mod-tools-root is a shared option (CliCommands.Shared.cpp)
        (void)Sub;
        (void)Options;
    }

    const GameCommand PlacementBacktestCommand{
        "bo3-placement-backtest",
        "Predict the gdt path of every stock row of the BO3 Mod Tools gdt.db from its name and kind alone and report "
        "exact, folder-only and missed predictions per kind with the largest miss groups (read-only; writes only the report)",
        "A label (use -); the root is --mod-tools-root, --install or TA_TOOLS_PATH",
        RunPlacementBacktest,
        AddPlacementBacktestOptions,
    };
    const GameCommandRegistration PlacementBacktestRegistered(PlacementBacktestCommand);
}
