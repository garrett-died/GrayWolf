#include "stdafx.h"

// pool-coverage <game> [--zones a,b|@file] [--out dir] [--export-report file] [--approximations file] [--min]
// Walks the zones with the game's zonekit tables, joins the result with zonekit/profiles/<profile>/pool_map.json and
// a run's report, and writes graywolf_pool_coverage.tsv and .json. The states are decided in PoolCoverage.cpp.

#include "CliOptions.h"
#include "CliSupport.h"
#include "DiskGame.h"
#include "DiskGameCli.h"
#include "GameCommand.h"
#include "Bo3Target.Verify.h"
#include "PoolCoverage.h"
#include "Siren.h"
#include "ZoneTableWalker.h"
#include "ZonekitZone.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    using namespace PoolCoverage;

    bool ReadText(const fs::path& File, std::string& Out)
    {
        std::ifstream In(File, std::ios::binary);
        if (!In) return false;
        std::ostringstream Os;
        Os << In.rdbuf();
        Out = Os.str();
        return true;
    }

    std::vector<std::string> Lines(const std::string& Text)
    {
        std::vector<std::string> Out;
        std::istringstream In(Text);
        std::string Line;
        while (std::getline(In, Line))
        {
            while (!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) Line.pop_back();
            if (!Line.empty()) Out.push_back(Line);
        }
        return Out;
    }

    // The game by its id, or by the zonekit profile name (t7 for bo3).
    const DiskGame::IDiskGame* FindGame(const std::string& Name)
    {
        if (const DiskGame::IDiskGame* G = DiskGame::Find(Name)) return G;
        for (const DiskGame::IDiskGame* G : DiskGame::Games())
            if (const auto Zone = G->Unopened(); Zone)
                if (const auto* Zk = dynamic_cast<const ZonekitZone*>(Zone.get()); Zk != nullptr && Zk->ProfileUsed() == Name) return G;
        return nullptr;
    }

    // Everything a zone's walk says about its pools.
    struct ZoneWalk
    {
        std::string Name, Error, Why;
        bool Opened = false, Complete = false;
        std::map<std::string, Counts> Pools;
    };

    ZoneWalk Summarize(const std::string& Name, const ZoneTables::ZoneResult& R)
    {
        ZoneWalk Z;
        Z.Name = Name;
        Z.Opened = true;
        Z.Why = R.Why;
        Z.Complete = R.Why == "walked to the end";
        const ZoneTables::ZoneResult::Instance* Last = nullptr;
        for (const auto& I : R.Instances)
        {
            if (I.Parent >= 0 || I.Owner >= R.OwnerNames.size()) continue;
            Counts& C = Z.Pools[R.OwnerNames[I.Owner]];
            C.Seen++;
            C.WalkedOk++;
            Last = &I;
        }
        if (Z.Complete || R.Why.rfind("stopped ", 0) == 0) return Z;
        // the walk ended on an asset: one it had no table for (not entered), or one that raised (the last entered)
        if (R.Why.rfind("no table for ", 0) == 0)
        {
            Counts& C = Z.Pools[R.Why.size() > 13 ? R.Why.substr(13) : std::string("(type with no name)")];
            C.Seen++;
            C.WalkWhy = "no table";
        }
        else if (Last != nullptr)
        {
            Counts& C = Z.Pools[R.OwnerNames[Last->Owner]];
            C.WalkedOk--;
            C.WalkWhy = R.Why;
        }
        return Z;
    }

    int RunPoolCoverage(const CliOptions& Options, const std::string& GameName)
    {
        const auto Fail = [&](const std::string& Why) {
            CliReport(Options, "result=error\nreason=" + Why + "\n");
            return 1;
        };
        const DiskGame::IDiskGame* Game = FindGame(GameName);
        if (Game == nullptr) return Fail("no disk reader for " + GameName);
        DiskGame::ExportOptions O;
        std::string Error;
        if (!DiskGame::ReadExportOptions(*Game, Options, O, Error)) return Fail(Error);

        // the profile, and the universe: every pool types.json names
        std::string ProfileName = Options.ZoneWalkProfile;
        if (ProfileName.empty())
        {
            const auto Zone = Game->Unopened();
            const auto* Zk = dynamic_cast<const ZonekitZone*>(Zone.get());
            if (Zk == nullptr) return Fail(std::string(Game->Id()) + " is not read by zonekit tables, so there are no pools to list");
            ProfileName = Zk->ProfileUsed();
        }
        ZoneTables::Profile Profile;
        if (!ZoneTables::LoadProfile(ProfileName, Profile, Error)) return Fail(Error);
        std::vector<std::string> Missing;
        std::set<std::string> Universe;
        {
            std::string Text;
            if (!ReadText(fs::path(Profile.Dir) / "types.json", Text)) return Fail("cannot read types.json of " + ProfileName);
            size_t Dropped = 0;
            const nlohmann::json Types = nlohmann::json::parse(Text);
            for (const auto& [Id, Name] : Types.at("names").items())
            {
                const std::string N = Name.get<std::string>();
                if (N.empty() || N == "?" || N == "<unused>") Dropped++;
                else Universe.insert(N);
            }
            if (Dropped) Missing.push_back(std::to_string(Dropped) + " type ids in types.json are named ? or <unused> and are not pools");
        }

        PoolMap Map;
        {
            std::string Text;
            if (!ReadText(fs::path(Profile.Dir) / "pool_map.json", Text))
                Missing.push_back("zonekit/profiles/" + ProfileName + "/pool_map.json does not exist: every pool is UNMAPPED (tools/bo3target/gen_pool_map.py writes it)");
            else if (!ParsePoolMap(Text, Map, Error))
                return Fail("pool_map.json of " + ProfileName + ": " + Error);
        }
        // pools a person has proven empty (inert_pools.txt, one per line); the file is absent or empty today
        std::set<std::string> InertPools;
        if (std::string Text; ReadText(fs::path(Profile.Dir) / "inert_pools.txt", Text)) InertPools = ParseInertPools(Text);
        else Missing.push_back("zonekit/profiles/" + ProfileName + "/inert_pools.txt does not exist: no pool is INERT by proof");

        // the gate (bo3-verify's graywolf_gate.tsv): converted and gate_pass
        GateCounts Gate;
        if (!Options.PoolGate.empty())
        {
            std::string Text;
            if (!ReadText(Options.PoolGate, Text)) return Fail("cannot read " + Options.PoolGate);
            Gate = ParseGate(Text);
        }

        std::set<std::string> GameKinds;
        for (const DiskGame::Kind K : Game->Kinds()) GameKinds.insert(DiskGame::KindName(K));

        // the zones
        std::vector<std::string> Inputs;
        if (!Options.PoolZones.empty())
        {
            if (Options.PoolZones[0] == '@')
            {
                std::string Text;
                if (!ReadText(Options.PoolZones.substr(1), Text)) return Fail("cannot read " + Options.PoolZones.substr(1));
                Inputs = Lines(Text);
            }
            else
            {
                std::stringstream Ss(Options.PoolZones);
                for (std::string Item; std::getline(Ss, Item, ',');)
                    if (!Item.empty()) Inputs.push_back(Item);
            }
        }
        // the game's own Oodle, when none is beside the exe (as a disk-export run does)
        if (!O.Install.empty()) Siren::AddSearchDir(fs::path(O.Install).wstring());
        std::vector<ZoneWalk> Walks;
        const auto WalkZone = [&](const std::string& Zone) {
            std::string OpenError;
            auto Opened = Game->Open(Zone, O, OpenError);
            const auto* Zk = dynamic_cast<ZonekitZone*>(Opened.get());
            if (Zk == nullptr)
            {
                ZoneWalk Z;
                Z.Name = Zone;
                Z.Error = Opened ? "not a zonekit zone" : OpenError;
                Walks.push_back(std::move(Z));
                return;
            }
            Walks.push_back(Summarize(Zone, Zk->FullWalk()));
        };
        size_t ZonesInGame = 0;     // set only by --all-zones: the zones the install lists
        if (Options.PoolAllZones)
        {
            if (!Inputs.empty()) return Fail("--all-zones walks every zone the install lists; do not give --zones as well");
            if (O.Install.empty()) return Fail("--all-zones needs --install (the zones come from the install's list)");
            std::vector<std::string> Listed;
            if (!Game->ListZones(O.Install, O, Listed, Error)) return Fail("cannot list the zones of " + O.Install + ": " + Error);
            if (Listed.empty()) return Fail("the install lists no zone");
            ZonesInGame = Listed.size();
            std::vector<std::string> Openable;
            std::string Why;
            if (!Game->ResolveZones(O.Install, Listed, O, Openable, Why)) return Fail("cannot resolve the install's zones: " + Why);
            if (Openable.size() < Listed.size())
                Missing.push_back(std::format("the install lists {} zones but only {} resolved to openable zones: INERT stays out of reach", Listed.size(), Openable.size()));
            for (const std::string& Zone : Openable) WalkZone(Zone);
        }
        for (const std::string& Input : Inputs)
        {
            std::vector<std::string> Zones;
            std::string Why;
            if (!Game->Zones(Input, O, Zones, Error))
            {
                Zones.clear();
                if (O.Install.empty() || !Game->ResolveZones(O.Install, { Input }, O, Zones, Why) || Zones.empty())
                {
                    ZoneWalk Z;
                    Z.Name = Input;
                    Z.Error = Error;
                    Walks.push_back(std::move(Z));
                    continue;
                }
            }
            for (const std::string& Zone : Zones) WalkZone(Zone);
        }

        // every zone together
        std::map<std::string, Counts> All;
        size_t Complete = 0;
        for (const ZoneWalk& Z : Walks)
        {
            Complete += Z.Complete ? 1 : 0;
            for (const auto& [Pool, C] : Z.Pools)
            {
                Counts& A = All[Pool];
                A.Seen += C.Seen;
                A.WalkedOk += C.WalkedOk;
                if (A.WalkWhy.empty()) A.WalkWhy = C.WalkWhy;
            }
        }
        for (const ZoneWalk& Z : Walks)
            if (!Z.Opened) Missing.push_back("zone " + Z.Name + " did not open (" + Z.Error + "): its pools are not counted");
        for (const auto& [Pool, C] : All)
            if (!Universe.count(Pool))
            {
                Universe.insert(Pool);
                Missing.push_back("pool " + Pool + " was walked but is not in types.json");
            }

        // the export side
        bool HasReport = !Options.PoolExportReport.empty();
        std::string ReportRejected;
        ExportReport Rep;
        if (HasReport)
        {
            std::string Text;
            if (!ReadText(Options.PoolExportReport, Text)) return Fail("cannot read " + Options.PoolExportReport);
            Rep = ParseExportReport(Text);
            // the report must describe the zones that were walked: a report of other zones would credit this walk with exports it never made
            std::vector<std::string> WalkedNames;
            for (const ZoneWalk& Z : Walks) WalkedNames.push_back(Z.Name);
            std::string Why;
            if (!ReportMatchesWalk(Rep, WalkedNames, Why))
            {
                ReportRejected = Why;
                Rep = ExportReport();
                HasReport = false;
                Missing.push_back("the export report was rejected (" + Why + "): exported is static, every pool with an exporter is STATIC_ONLY");
            }
            else if (Rep.Counters.count("cancelled"))
                Missing.push_back("the export run was cancelled");
        }
        else
            Missing.push_back("exported is static (the game has an exporter for the pool's kind); no run's report was read (--export-report)");
        std::string GateRejectWhy;
        if (Gate.Read)
        {
            // the gate must be this run's and still describe the GDTs on disk: the layout it read is the one the run's report says it
            // published, and re-reading that layout (every .gdt for a scanned gate, the listed files for a listed one) gives the hash the gate
            // recorded. A gate that cannot be shown to hold is rejected, its counts are not used, and the pools stay non-terminal.
            std::string Why;
            if (!ReportRejected.empty()) Why = "the export report was rejected, so the gate cannot be tied to a run";
            else if (GateMatchesRun(Gate, HasReport && Rep.Strings.count("publish_output") ? Rep.Strings.at("publish_output") : std::string(), Why)) Why.clear();
            if (Why.empty())
            {
                std::vector<std::string> RootTexts;
                for (size_t From = 0;;)
                {
                    const size_t Bar = Gate.RunRoots.find('|', From);
                    RootTexts.push_back(Gate.RunRoots.substr(From, Bar == std::string::npos ? std::string::npos : Bar - From));
                    if (Bar == std::string::npos) break;
                    From = Bar + 1;
                }
                if (Bo3Target::Verify::GateStillHolds(Gate.RunMode, RootTexts, Gate.Files, Gate.RunSha, Gate.RunGdts, Why)) Why.clear();
            }
            if (!Why.empty())
            {
                GateRejectWhy = Why;
                Gate = GateCounts();
                Missing.push_back("--gate rejected: " + Why);
            }
        }
        std::map<std::string, size_t> Approximations;
        if (!Options.PoolApprox.empty())
        {
            std::string Text;
            if (!ReadText(Options.PoolApprox, Text)) return Fail("cannot read " + Options.PoolApprox);
            Approximations = CountApproximations(Text);
            if (const auto It = Approximations.find(""); It != Approximations.end())
                Missing.push_back(std::to_string(It->second) + " approximation rows have no pool prefix (name_shortened...) and are in no pool's count");
        }
        else
            Missing.push_back("approximations are counted only from a run with --target (graywolf_approximations.tsv, --approximations)");

        // per pool: the direct pools of a kind share what the run wrote for it
        std::map<std::string, std::map<std::string, size_t>> SeenByKind;
        for (const auto& [Pool, R] : Map.Rows)
            if (R.Route == "direct" && !R.Kind.empty() && GameKinds.count(R.Kind) && All.count(Pool)) SeenByKind[R.Kind][Pool] = All[Pool].Seen;
        std::map<std::string, size_t> Exported, References;
        std::map<std::string, std::string> SharedWith;
        std::set<std::string> KindsNotRun;
        for (const auto& [Kind, Seen] : SeenByKind)
        {
            std::string Folder = Kind;
            for (const DiskGame::KindInfo& K : DiskGame::Kinds)
                if (Kind == K.Name) Folder = K.Folder;
            const auto Count = [&](const std::string& Suffix) {
                const auto It = Rep.Counters.find(Folder + Suffix);
                return It == Rep.Counters.end() ? size_t(0) : It->second;
            };
            size_t Total = 0;
            for (const auto& [Pool, N] : Seen) Total += N;
            const size_t Refs = Count("_references");
            if (HasReport && !Rep.Counters.count(Folder)) KindsNotRun.insert(Kind);
            if (Seen.size() > 1)
            {
                // the run reports one total per kind: no pool of a shared kind gets a number of its own
                std::string Names;
                for (const auto& [Pool, N] : Seen) Names += (Names.empty() ? "" : ", ") + Pool;
                for (const auto& [Pool, N] : Seen)
                {
                    Exported[Pool] = 0;
                    SharedWith[Pool] = Names;
                }
            }
            else if (HasReport)
                for (const auto& [Pool, N] : Seen) Exported[Pool] = std::min(N, Count("_written") + Count("_known") + Count("_linked"));
            else
                for (const auto& [Pool, N] : Seen) Exported[Pool] = N;     // static: the pool's seen; Decide gives STATIC_ONLY
            for (const auto& [Pool, N] : Seen) References[Pool] = Total ? Refs * N / Total : 0;
        }
        if (!SharedWith.empty())
            Missing.push_back("pools that share a kind (rawfile and luafile) have no per-pool export count: the run reports one total for the kind, so they stay WALKED_ONLY");
        if (!HasReport)
            Missing.push_back("no --export-report: every pool with an exporter is STATIC_ONLY, never a terminal state");

        std::vector<Row> Rows;
        const std::string GameId = Game->Id();
        for (const ZoneWalk& Z : Walks)
            for (const auto& [Pool, C] : Z.Pools)
            {
                Row R;
                R.Game = GameId;
                R.Zone = Z.Name;
                R.Pool = Pool;
                R.N = C;
                R.StateText = "-";
                R.Reason = C.WalkWhy.empty() ? "walk counts for this zone; the state is on the ALL row" : "walk: " + C.WalkWhy;
                Rows.push_back(std::move(R));
            }
        std::map<std::string, size_t> StateTotals;
        size_t NotDone = 0;
        for (const std::string& Pool : Universe)
        {
            Row R;
            R.Game = GameId;
            R.Zone = "ALL";
            R.Pool = Pool;
            if (const auto It = All.find(Pool); It != All.end()) R.N = It->second;
            const auto MapIt = Map.Rows.find(Pool);
            Context X;
            X.Row = MapIt == Map.Rows.end() ? nullptr : &MapIt->second;
            X.ZonesTested = Walks.size();
            X.ZonesComplete = Complete;
            X.ZonesInGame = ZonesInGame;
            X.InertProven = InertPools.count(Pool) != 0;
            if (X.InertProven) X.InertProof = "inert_pools.txt";
            if (X.Row != nullptr)
            {
                X.KindSupported = X.Row->Kind.empty() || GameKinds.count(X.Row->Kind) != 0;
                if (X.Row->Route == "direct" && X.KindSupported && !X.Row->Kind.empty() && Exported.count(Pool))
                {
                    R.N.Exported = Exported[Pool];
                    R.N.References = References[Pool];
                    R.N.ExportSource = HasReport ? Counts::Source::Report : Counts::Source::Static;
                    R.N.KindNotRun = HasReport && KindsNotRun.count(X.Row->Kind) != 0;
                    if (const auto Sw = SharedWith.find(Pool); Sw != SharedWith.end())
                    {
                        R.N.SharedKind = true;
                        R.N.SharedWith = Sw->second;
                    }
                    if (HasReport)
                    {
                        R.N.ExportFailed = Rep.FailuresByKind.count(X.Row->Kind) ? Rep.FailuresByKind.at(X.Row->Kind) : 0;
                        R.N.UnresolvedNames = Rep.UnresolvedByKind.count(X.Row->Kind) ? Rep.UnresolvedByKind.at(X.Row->Kind) : 0;
                    }
                }
            }
            if (const auto It = Approximations.find(Pool); It != Approximations.end()) R.N.Approximations = It->second;
            if (!GateRejectWhy.empty() && X.Row != nullptr && !X.Row->Awi.empty()) R.N.GateRejectWhy = GateRejectWhy;
            if (Gate.Read && X.Row != nullptr && !X.Row->Awi.empty())
            {
                R.N.GateRead = true;
                R.N.GateFileFailures = Gate.FileFailures;
                R.N.GateIncomplete = Gate.Incomplete;
                if (const auto It = Gate.Gdfs.find(X.Row->Awi); It != Gate.Gdfs.end())
                {
                    R.N.Converted = It->second.first;
                    R.N.GatePass = It->second.first - std::min(It->second.second, It->second.first);
                }
            }
            const Verdict V = Decide(R.N, X);
            R.StateText = StateName(V.S);
            R.Reason = V.Reason;
            StateTotals[R.StateText]++;
            NotDone += IsDone(V.S) ? 0 : 1;
            Rows.push_back(std::move(R));
        }
        if (!Gate.Read) Missing.push_back("converted and gate_pass are 0: no --gate file (a bo3-verify graywolf_gate.tsv) was given, so no pool with a .awi can be CONVERTED");
        else Missing.push_back("converted and gate_pass come from the gate file: a STATIC gate, not proof that BO3 accepts the assets; entries are GDT entries, not zone assets");
        Missing.push_back("exported counts come from one run's report as a whole (no per-zone counts), and --limit or --name-filter in that run leave pools short");
        Missing.push_back(ZonesInGame ? std::format("INERT here means seen 0 in every one of the install's {} zones (--all-zones), each walked to the end; inert_pools.txt names {} pool(s)", ZonesInGame, InertPools.size())
                                      : std::format("seen 0 is NOT_SEEN, not INERT: INERT needs a pool named in inert_pools.txt ({} named) or --all-zones", InertPools.size()));
        Missing.push_back("a walked_ok pool is read whole by the tables; nothing checks that its bytes decode into a valid asset");

        std::vector<std::string> ZoneNames, KindList(GameKinds.begin(), GameKinds.end());
        for (const ZoneWalk& Z : Walks) ZoneNames.push_back(Z.Name + (Z.Opened ? (Z.Complete ? " (walked to the end)" : " (" + Z.Why + ")") : " (did not open)"));
        const fs::path Dir = Options.Out.empty() ? fs::path(".") : fs::path(Options.Out);
        std::error_code Ec;
        fs::create_directories(Dir, Ec);
        const auto Write = [&](const char* File, const std::string& Text) {
            std::ofstream Out(Dir / File, std::ios::binary);
            return static_cast<bool>(Out << Text);
        };
        if (!Write("graywolf_pool_coverage.tsv", Tsv(Rows)) || !Write("graywolf_pool_coverage.json", Json(GameId, ProfileName, Rows, ZoneNames, KindList, Missing)))
            return Fail("cannot write to " + Dir.string());

        std::string Report = std::format("result=ok\ngame={}\nprofile={}\nzones_walked={}\nzones_complete={}\npools={}\nexport_source={}\n", GameId, ProfileName, Walks.size(),
                                         Complete, Universe.size(), HasReport ? "report" : "static");
        if (!ReportRejected.empty()) Report += "export_report_rejected=" + ReportRejected + "\n";
        if (!GateRejectWhy.empty()) Report += "gate_rejected=" + GateRejectWhy + "\n";
        for (const auto& [State, N] : StateTotals) Report += std::format("state_{}={}\n", State, N);
        Report += std::format("pools_not_terminal={}\nout={}\n", NotDone, Dir.string());
        CliReport(Options, Report);
        return Options.PoolMin && NotDone > 0 ? 3 : 0;
    }

    void AddOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--zones", Options.PoolZones, "Zones to walk: a comma-separated list of zone paths or names, or @file with one per line");
        Sub.add_option("--export-report", Options.PoolExportReport, "The --report file of a disk-export run: exported, failures and unresolved names are read from it (else exported is static)");
        Sub.add_option("--approximations", Options.PoolApprox, "The graywolf_approximations.tsv of a --target run");
        Sub.add_option("--profile", Options.ZoneWalkProfile, "zonekit profile when it is not the game's own");
        Sub.add_option("--gate", Options.PoolGate, "The graywolf_gate.tsv of a bo3-verify run: converted and gate_pass are read from it (a pool is CONVERTED only with its gate failures at zero)");
        Sub.add_flag("--all-zones", Options.PoolAllZones, "Walk every zone the install lists (needs --install; no --zones); only then can a pool seen 0 everywhere be INERT");
        Sub.add_flag("--min", Options.PoolMin, "Exit 3 when any pool is not CONVERTED, EXTRACTED_NEUTRAL or INERT (a CI gate)");
    }

    const GameCommand PoolCoverageCommand{
        "pool-coverage",
        "Per pool, which terminal state it has reached (CONVERTED, EXTRACTED_NEUTRAL, INERT, FAILED, UNMAPPED, WALKED_ONLY): graywolf_pool_coverage.tsv and .json in --out",
        "The game (bo4, bo3, coldwar, ww2, mwr, bo1, waw), or its zonekit profile",
        RunPoolCoverage,
        AddOptions,
        "game",
    };
    const GameCommandRegistration PoolCoverageRegistered(PoolCoverageCommand);
}
