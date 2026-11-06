#include "stdafx.h"

// See PoolCoverage.h.

#include "PoolCoverage.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace PoolCoverage
{
    const char* StateName(State S)
    {
        switch (S)
        {
        case State::Converted: return "CONVERTED";
        case State::ExtractedNeutral: return "EXTRACTED_NEUTRAL";
        case State::Inert: return "INERT";
        case State::Failed: return "FAILED";
        case State::Unmapped: return "UNMAPPED";
        case State::WalkedOnly: return "WALKED_ONLY";
        case State::NotSeen: return "NOT_SEEN";
        case State::StaticOnly: return "STATIC_ONLY";
        }
        return "?";
    }

    bool IsDone(State S)
    {
        return S == State::Converted || S == State::ExtractedNeutral || S == State::Inert;
    }

    bool ParsePoolMap(const std::string& Text, PoolMap& Out, std::string& Error)
    {
        Out = PoolMap();
        try
        {
            const nlohmann::json J = nlohmann::json::parse(Text);
            Out.Game = J.value("game", std::string());
            if (!J.contains("pools") || !J.at("pools").is_array())
            {
                Error = "no \"pools\" array";
                return false;
            }
            const auto Field = [](const nlohmann::json& R, const char* Key) {
                if (!R.contains(Key) || R.at(Key).is_null()) return std::string();
                if (!R.at(Key).is_string()) throw std::runtime_error(std::string("\"") + Key + "\" is not a string or null");
                return R.at(Key).get<std::string>();
            };
            for (const nlohmann::json& R : J.at("pools"))
            {
                MapRow Row;
                Row.Pool = Field(R, "pool");
                if (Row.Pool.empty()) throw std::runtime_error("a row has no pool");
                Row.Kind = Field(R, "kind");
                Row.Awi = Field(R, "bo3_awi");
                Row.Neutral = Field(R, "neutral_form");
                Row.Route = Field(R, "route");
                if (Row.Route.empty()) Row.Route = "none";
                if (!Out.Rows.emplace(Row.Pool, Row).second) throw std::runtime_error("pool " + Row.Pool + " has two rows");
            }
        }
        catch (const std::exception& E)
        {
            Error = E.what();
            Out = PoolMap();
            return false;
        }
        return true;
    }

    namespace
    {
        std::string Plural(size_t N, const char* Word)
        {
            return std::format("{} {}{}", N, Word, N == 1 ? "" : "s");
        }
    }

    // The order matters: evidence against a listing, then the listing, emptiness, what the row allows, how much was exported.
    Verdict Decide(const Counts& C, const Context& X)
    {
        if (X.InertProven)
        {
            // a listing is a person's claim; evidence beats it, and the conflict is a failure to be looked at
            if (C.Seen > 0)
                return { State::Failed, std::format("conflict: named in {} but seen {} in the zones tested: the list says inert, the data does not", X.InertProof, C.Seen) };
            if (C.WalkedOk < C.Seen || !C.WalkWhy.empty())
                return { State::Failed, std::format("conflict: named in {} but the walk failed ({})", X.InertProof, C.WalkWhy) };
            if (X.ZonesComplete < X.ZonesTested)
                return { State::Failed, std::format("conflict: named in {} but {} of {} did not walk to the end, so the walk cannot confirm it", X.InertProof, Plural(X.ZonesTested - X.ZonesComplete, "zone"), X.ZonesTested) };
            return { State::Inert, "named in " + X.InertProof };
        }
        if (C.Seen == 0)
        {
            if (X.ZonesTested == 0) return { X.Row ? State::WalkedOnly : State::Unmapped, "no zone was walked, so nothing is known about the pool" };
            if (X.ZonesComplete < X.ZonesTested)
                return { State::WalkedOnly, std::format("seen 0, but {} of {} did not walk to the end, so the pool cannot be shown empty",
                                                        Plural(X.ZonesTested - X.ZonesComplete, "zone"), X.ZonesTested) };
            if (X.ZonesInGame > 0 && X.ZonesTested == X.ZonesInGame)
                return { State::Inert, std::format("seen 0 in every one of the game's {} (each walked to the end); not named in inert_pools.txt", Plural(X.ZonesInGame, "zone")) };
            return { State::NotSeen, std::format("seen 0 in the {} tested (each walked to the end); no proof: not named in inert_pools.txt, and not every zone of the game was walked (--all-zones)",
                                                 Plural(X.ZonesTested, "zone")) };
        }
        if (X.Row == nullptr) return { State::Unmapped, "no row in pool_map.json" };
        if (C.WalkedOk < C.Seen)
            return { State::Failed, std::format("walk: {} of {} did not walk{}", C.Seen - C.WalkedOk, C.Seen, C.WalkWhy.empty() ? "" : " (" + C.WalkWhy + ")") };
        if (C.ExportFailed > 0) return { State::Failed, std::format("export: {} (see failure lines of the run's report)", Plural(C.ExportFailed, "failure")) };

        const MapRow& R = *X.Row;
        if (R.Kind.empty())
            return { State::WalkedOnly, R.Neutral.empty() ? "no exporter writes this pool" : "a neutral form is named but no exporter kind writes it" };
        if (!X.KindSupported) return { State::WalkedOnly, "this game has no exporter for kind " + R.Kind };
        if (R.Route != "direct") return { State::WalkedOnly, "written as part of " + R.Route + "; no per-pool count" };
        // no per-pool number exists for a kind several pools share: the run's report gives one total, and a total split by guess proves nothing
        if (C.SharedKind) return { State::WalkedOnly, std::format("kind {} is shared by {}: the run's report gives one total for the kind, so this pool's own export count is not available", R.Kind, C.SharedWith) };
        // no run's report: an exporter exists, which says nothing about what a run wrote
        if (C.ExportSource == Counts::Source::Static)
            return { State::StaticOnly, std::format("static: an exporter for kind {} exists, but no run's report was read (--export-report), so nothing says this pool was exported", R.Kind) };

        if (C.Exported == 0 && C.KindNotRun) return { State::WalkedOnly, std::format("0 of {} exported: the run did not ask for kind {}", C.Seen, R.Kind) };
        if (C.Exported == 0) return { State::WalkedOnly, std::format("0 of {} exported", C.Seen) };
        const size_t Need = C.Seen > C.References ? C.Seen - C.References : 0;
        if (C.Exported < Need)
            return { State::WalkedOnly, std::format("exported {} of {}{}", C.Exported, C.Seen,
                                                    C.References ? std::format(" ({} left to the zones that define them)", Plural(C.References, "reference")) : std::string()) };
        if (!R.Awi.empty())
        {
            if (!C.GateRead)
                return { State::WalkedOnly, std::format("exported {}; maps to {}.awi, awaiting bo3-verify ({})", C.Exported, R.Awi,
                                                        C.GateRejectWhy.empty() ? std::string("no --gate file was given") : "the --gate file was rejected: " + C.GateRejectWhy) };
            if (C.GateFileFailures > 0)
                return { State::Failed, std::format("gate: {} on files (not on a GDF) hold back every pool", Plural(C.GateFileFailures, "failure")) };
            if (C.Converted == 0) return { State::WalkedOnly, std::format("exported {}; maps to {}.awi, but the gate file holds no {} entry", C.Exported, R.Awi, R.Awi) };
            if (C.GatePass < C.Converted)
                return { State::Failed, std::format("gate: {} of {} {} entries fail (see graywolf_gate.tsv)", C.Converted - C.GatePass, C.Converted, R.Awi) };
            if (C.GateIncomplete > 0)
                return { State::WalkedOnly, std::format("gate incomplete: {} not judged (incomplete or not_run rows of a required check in graywolf_gate.tsv), so {} entries cannot be called converted", Plural(C.GateIncomplete, "check"), R.Awi) };
            if (C.Converted < C.Exported)
                return { State::WalkedOnly, std::format("exported {}, but the gate holds only {} {} entries", C.Exported, C.Converted, R.Awi) };
            return { State::Converted, "exported and passed the gate as " + R.Awi + std::format(" ({} entries, 0 failing; static gate, not proof BO3 accepts them)", C.Converted) };
        }
        if (!R.Neutral.empty()) return { State::ExtractedNeutral, "exported as " + R.Neutral };
        return { State::WalkedOnly, "exported, but the row names no .awi and no neutral form" };
    }

    ExportReport ParseExportReport(const std::string& Text)
    {
        ExportReport R;
        std::istringstream In(Text);
        std::string Line;
        while (std::getline(In, Line))
        {
            while (!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) Line.pop_back();
            if (Line.rfind("failure ", 0) == 0)
            {
                // "<zone>: <kind> <name> <why>"
                const size_t Colon = Line.find(": ", 8);
                if (Colon == std::string::npos) continue;
                const size_t From = Colon + 2, To = Line.find(' ', From);
                R.FailuresByKind[Line.substr(From, To == std::string::npos ? std::string::npos : To - From)]++;
                continue;
            }
            if (Line.rfind("unresolved ", 0) == 0)
            {
                const size_t To = Line.find(' ', 11);
                R.UnresolvedByKind[Line.substr(11, To == std::string::npos ? std::string::npos : To - 11)]++;
                continue;
            }
            const size_t Eq = Line.find('=');
            if (Eq != std::string::npos && Line.compare(0, Eq, "publish_output") == 0) R.Strings["publish_output"] = Line.substr(Eq + 1);
            if (Eq != std::string::npos && Line.compare(0, Eq, "zone_names") == 0)
            {
                R.HasZoneNames = true;
                std::string Names = Line.substr(Eq + 1);
                for (size_t From = 0; From <= Names.size();)
                {
                    const size_t Bar = Names.find('|', From);
                    const std::string One = Names.substr(From, Bar == std::string::npos ? std::string::npos : Bar - From);
                    if (!One.empty()) R.ZoneNames.insert(One);
                    if (Bar == std::string::npos) break;
                    From = Bar + 1;
                }
            }
            if (Eq == std::string::npos || Eq == 0 || Eq + 1 >= Line.size() || Line[Eq + 1] < '0' || Line[Eq + 1] > '9') continue;
            R.Counters[Line.substr(0, Eq)] = static_cast<size_t>(std::strtoull(&Line[Eq + 1], nullptr, 10));
        }
        if (const auto It = R.Counters.find("zones"); It != R.Counters.end()) R.Zones = It->second;
        return R;
    }

    std::map<std::string, size_t> CountApproximations(const std::string& Text)
    {
        std::map<std::string, size_t> Out;
        std::istringstream In(Text);
        std::string Line;
        bool Header = true;
        while (std::getline(In, Line))
        {
            if (Header || Line.empty())
            {
                Header = false;
                continue;
            }
            const std::string Code = Line.substr(0, Line.find('\t'));
            const size_t Dot = Code.find('.');
            Out[Dot == std::string::npos ? std::string() : Code.substr(0, Dot)]++;
        }
        return Out;
    }

    namespace
    {
        // a TSV cell: no tab or line break
        std::string Cell(std::string S)
        {
            for (char& C : S)
                if (C == '\t' || C == '\r' || C == '\n') C = ' ';
            return S;
        }
        const char* SourceName(Counts::Source S)
        {
            return S == Counts::Source::Report ? "report" : S == Counts::Source::Static ? "static" : "none";
        }
    }

    std::string Tsv(const std::vector<Row>& Rows)
    {
        std::string Out = "game\tzone\tpool\tseen\twalked_ok\texported\texport_src\tconverted\tgate_pass\tstate\treason\tapproximations\tunresolved_names\n";
        for (const Row& R : Rows)
            Out += std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", Cell(R.Game), Cell(R.Zone), Cell(R.Pool), R.N.Seen, R.N.WalkedOk, R.N.Exported,
                               SourceName(R.N.ExportSource), R.N.Converted, R.N.GatePass, Cell(R.StateText), Cell(R.Reason), R.N.Approximations, R.N.UnresolvedNames);
        return Out;
    }

    std::string Json(const std::string& Game, const std::string& Profile, const std::vector<Row>& Rows, const std::vector<std::string>& Zones,
                     const std::vector<std::string>& GameKinds, const std::vector<std::string>& Missing)
    {
        nlohmann::ordered_json J;
        J["game"] = Game;
        J["profile"] = Profile;
        J["zones"] = Zones;
        J["game_kinds"] = GameKinds;
        std::map<std::string, size_t> Totals;
        nlohmann::ordered_json Out = nlohmann::ordered_json::array();
        for (const Row& R : Rows)
        {
            if (R.Zone == "ALL") Totals[R.StateText]++;
            Out.push_back({ { "game", R.Game }, { "zone", R.Zone }, { "pool", R.Pool }, { "seen", R.N.Seen }, { "walked_ok", R.N.WalkedOk },
                            { "exported", R.N.Exported }, { "export_src", SourceName(R.N.ExportSource) }, { "converted", R.N.Converted },
                            { "gate_pass", R.N.GatePass }, { "state", R.StateText }, { "reason", R.Reason },
                            { "approximations", R.N.Approximations }, { "unresolved_names", R.N.UnresolvedNames } });
        }
        J["totals_by_state"] = Totals;
        J["not_checked"] = Missing;
        J["rows"] = Out;
        return J.dump(1) + "\n";
    }

    // ---- self-test: the state logic on synthetic rows

    GateCounts ParseGate(const std::string& Text)
    {
        GateCounts G;
        G.Read = true;
        std::istringstream In(Text);
        std::string Line;
        while (std::getline(In, Line))
        {
            while (!Line.empty() && (Line.back() == '\r' || Line.back() == ' ')) Line.pop_back();
            if (Line.empty() || Line[0] == '#') continue;
            std::vector<std::string> Cell;
            size_t From = 0;
            for (size_t Tab; (Tab = Line.find('	', From)) != std::string::npos; From = Tab + 1) Cell.push_back(Line.substr(From, Tab - From));
            Cell.push_back(Line.substr(From));
            if (Cell.size() < 4) continue;
            if (Cell[0] == "gate.entries")
            {
                size_t Entries = 0, Failed = 0;
                const auto Number = [&](const char* Key, size_t& Into) {
                    const size_t At = Cell[3].find(Key);
                    if (At != std::string::npos) Into = static_cast<size_t>(std::strtoull(&Cell[3][At + std::strlen(Key)], nullptr, 10));
                };
                Number("entries=", Entries);
                Number("failed=", Failed);
                G.Gdfs[Cell[1]] = { Entries, Failed };
            }
            else if (Cell[0] == "gate.run")
            {
                G.HasRun = true;
                const auto Field = [&](const char* Key) {
                    const size_t At = Cell[3].find(Key);
                    if (At == std::string::npos) return std::string();
                    const size_t From = At + std::strlen(Key);
                    return Cell[3].substr(From, Cell[3].find(' ', From) - From);
                };
                G.RunMode = Field("mode=");
                G.RunSha = Field("sha256=");
                G.RunGdts = static_cast<size_t>(std::strtoull(Field("gdts=").c_str(), nullptr, 10));
                if (const size_t At = Cell[3].find("roots="); At != std::string::npos) G.RunRoots = Cell[3].substr(At + 6);
            }
            else if (Cell[0] == "gate.file")
            {
                size_t Root = 0;
                if (const size_t At = Cell[3].find("root="); At != std::string::npos) Root = static_cast<size_t>(std::strtoull(&Cell[3][At + 5], nullptr, 10));
                G.Files.emplace_back(Root, Cell[1]);
            }
            else if (Cell[2] == "incomplete") G.Incomplete++;
            else if (Cell[2] == "not_run" && (Cell[0].rfind("awi.", 0) == 0 || Cell[0].rfind("gdt.", 0) == 0 || Cell[0].rfind("material.", 0) == 0 || Cell[0].rfind("image.", 0) == 0)) G.Incomplete++;
            else if (Cell[2] == "failure" && (Cell[1].rfind("file:", 0) == 0 || Cell[1].rfind("root:", 0) == 0 || Cell[1].rfind("path:", 0) == 0 || Cell[1].rfind("unresolved:", 0) == 0 || Cell[1] == "gate")) G.FileFailures++;
        }
        return G;
    }

    namespace
    {
        std::string PathKey(std::string S)
        {
            for (char& C : S) C = C == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            while (S.size() > 1 && S.back() == '/') S.pop_back();
            return S;
        }
    }

    bool ReportMatchesWalk(const ExportReport& R, const std::vector<std::string>& Walked, std::string& Why)
    {
        if (!R.HasZoneNames)
        {
            Why = "the export report does not record its zones (no zone_names line; a report from a disk-export build before that line, or not a disk-export report), so it cannot be tied to the zones walked";
            return false;
        }
        const auto Key = [](std::string S) {
            const size_t Cut = S.find_last_of("/\\:");
            if (Cut != std::string::npos) S.erase(0, Cut + 1);
            for (char& C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            return S;
        };
        // a zone is the same zone with or without its .ff / .zone extension (a run asked for mp_crash, its report names mp_crash.ff)
        const auto Bare = [](std::string S) {
            for (const char* Ext : { ".ff", ".zone" })
            {
                const size_t N = std::strlen(Ext);
                if (S.size() > N && S.compare(S.size() - N, N, Ext) == 0) return S.substr(0, S.size() - N);
            }
            return S;
        };
        // bare name -> the exact names (extension kept) given for it. One spelling on each side matches by the bare name (a run asked for mp_crash, its report
        // names mp_crash.ff); a side that holds both x.ff and x.zone walked them as two zones, so then the exact spellings must be equal
        std::map<std::string, std::set<std::string>> A, B;
        for (const std::string& Z : R.ZoneNames) A[Bare(Key(Z))].insert(Key(Z));
        for (const std::string& Z : Walked) B[Bare(Key(Z))].insert(Key(Z));
        const auto SameKeys = [](const std::map<std::string, std::set<std::string>>& X, const std::map<std::string, std::set<std::string>>& Y) {
            if (X.size() != Y.size()) return false;
            for (const auto& [Name, Spellings] : X)
            {
                const auto It = Y.find(Name);
                if (It == Y.end()) return false;
                if (Spellings.size() == 1 && It->second.size() == 1) continue;
                if (Spellings != It->second) return false;
            }
            return true;
        };
        if (SameKeys(A, B)) return true;
        std::string OnlyReport, OnlyWalk;
        for (const auto& [Bare2, Spellings] : A)
            for (const std::string& Z : Spellings)
                if (!B.count(Bare2) || (Spellings.size() != 1 || B.at(Bare2).size() != 1) && !B.at(Bare2).count(Z)) OnlyReport += (OnlyReport.empty() ? "" : ", ") + Z;
        for (const auto& [Bare2, Spellings] : B)
            for (const std::string& Z : Spellings)
                if (!A.count(Bare2) || (Spellings.size() != 1 || A.at(Bare2).size() != 1) && !A.at(Bare2).count(Z)) OnlyWalk += (OnlyWalk.empty() ? "" : ", ") + Z;
        Why = "the export report's zones differ from the zones walked" + (OnlyReport.empty() ? std::string() : "; only in the report: " + OnlyReport) + (OnlyWalk.empty() ? std::string() : "; only walked: " + OnlyWalk);
        return false;
    }

    bool GateMatchesRun(const GateCounts& G, const std::string& PublishOutput, std::string& Why)
    {
        if (!G.Read) { Why = "no gate file was read"; return false; }
        if (!G.HasRun) { Why = "the gate file has no gate.run row (written by an older bo3-verify): it cannot be tied to a run"; return false; }
        if (PublishOutput.empty())
        {
            Why = "the run's report has no publish_output line (no --export-report, or the run published no layout), so the gate cannot be tied to the run";
            return false;
        }
        std::string Roots = G.RunRoots;
        for (size_t From = 0;;)
        {
            const size_t Bar = Roots.find('|', From);
            if (PathKey(Roots.substr(From, Bar == std::string::npos ? std::string::npos : Bar - From)) == PathKey(PublishOutput)) return true;
            if (Bar == std::string::npos) break;
            From = Bar + 1;
        }
        Why = "the gate read the layout " + G.RunRoots + " but the run published " + PublishOutput + ": it is not this run's gate";
        return false;
    }

    std::set<std::string> ParseInertPools(const std::string& Text)
    {
        std::set<std::string> Out;
        std::istringstream In(Text);
        std::string Line;
        while (std::getline(In, Line))
        {
            if (const size_t Hash = Line.find('#'); Hash != std::string::npos) Line.resize(Hash);
            while (!Line.empty() && std::isspace(static_cast<unsigned char>(Line.back()))) Line.pop_back();
            size_t B = 0;
            while (B < Line.size() && std::isspace(static_cast<unsigned char>(Line[B]))) B++;
            if (B < Line.size()) Out.insert(Line.substr(B));
        }
        return Out;
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL PoolCoverage {} (got '{}')\n", Name, Got);
        };
        const auto Is = [&](const Verdict& V, State S, const char* Name, const char* Reason = nullptr) {
            Check(V.S == S && (Reason == nullptr || V.Reason.find(Reason) != std::string::npos), Name,
                  std::string(StateName(V.S)) + ": " + V.Reason);
        };

        const MapRow Raw{ "rawfile", "rawfile", "", "raw files", "direct" };
        const MapRow Xmodel{ "xmodel", "model", "xmodel", "", "direct" };
        const MapRow Mesh{ "xmodelmesh", "model", "", "", "carried:xmodel" };
        const MapRow Bare{ "ddl", "", "", "", "none" };
        const MapRow NoForm{ "sound", "sound", "", "", "direct" };
        Context X;
        X.ZonesTested = 2;
        X.ZonesComplete = 2;
        const auto Seen = [](size_t N, size_t Exported = 0, size_t Ok = SIZE_MAX) {
            Counts C;
            C.Seen = N;
            C.WalkedOk = Ok == SIZE_MAX ? N : Ok;
            C.Exported = Exported;
            return C;
        };

        // emptiness
        Is(Decide(Seen(0), X), State::NotSeen, "seen 0 in the zones tested, all walked to the end, no proof: NOT_SEEN, not INERT", "no proof");
        Check(!IsDone(State::NotSeen), "NOT_SEEN is not terminal", "");
        Context Every = X;
        Every.ZonesInGame = 2;
        Is(Decide(Seen(0), Every), State::Inert, "seen 0 in every zone of the game: INERT, the zone count in the reason", "every one of the game's 2 zones");
        Every.ZonesInGame = 450;
        Is(Decide(Seen(0), Every), State::NotSeen, "2 of 450 zones walked: still NOT_SEEN", "not every zone of the game");
        Every.ZonesInGame = 2;
        Every.ZonesComplete = 1;
        Is(Decide(Seen(0), Every), State::WalkedOnly, "every zone named but one stopped early: not INERT", "did not walk to the end");
        Is(Decide(Seen(3), Context{}), State::Unmapped, "seen 3, no row: UNMAPPED");
        Context Partial = X;
        Partial.ZonesComplete = 1;
        Is(Decide(Seen(0), Partial), State::WalkedOnly, "empty, but a zone stopped early: not INERT", "did not walk to the end");
        Context None;
        Is(Decide(Seen(0), None), State::Unmapped, "no zone walked, no row: UNMAPPED");
        None.Row = &Raw;
        Is(Decide(Seen(0), None), State::WalkedOnly, "no zone walked, a row: WALKED_ONLY", "no zone was walked");
        Context Proven = X;
        Proven.InertProven = true;
        Proven.InertProof = "inert_pools.txt";
        Is(Decide(Seen(0), Proven), State::Inert, "a pool named in inert_pools.txt is INERT", "named in inert_pools.txt");
        Is(Decide(Seen(7), Proven), State::Failed, "a listing does not override evidence: seen 7 is a FAILED conflict", "seen 7");
        Counts Broken = Seen(0);
        Broken.WalkedOk = 0;
        Broken.WalkWhy = "no table";
        Is(Decide(Broken, Proven), State::Failed, "a walk failure beats the listing", "walk failed");
        Context Listed2 = X;
        Listed2.InertProven = true;
        Listed2.InertProof = "inert_pools.txt";
        Listed2.ZonesComplete = 1;
        Is(Decide(Seen(0), Listed2), State::Failed, "a zone that did not walk to the end beats the listing", "did not walk to the end");

        // the row
        Is(Decide(Seen(3), X), State::Unmapped, "seen but no row: UNMAPPED");
        X.Row = &Bare;
        Is(Decide(Seen(3), X), State::WalkedOnly, "a row with nothing exporting it: WALKED_ONLY", "no exporter");
        X.Row = &Raw;
        X.KindSupported = false;
        Is(Decide(Seen(3, 3), X), State::WalkedOnly, "the game lacks the kind: WALKED_ONLY", "no exporter for kind");
        X.KindSupported = true;
        X.Row = &Mesh;
        Is(Decide(Seen(3, 3), X), State::WalkedOnly, "a carried pool has no count of its own", "carried:xmodel");

        // failures come before export
        X.Row = &Raw;
        Is(Decide(Seen(4, 4, 3), X), State::Failed, "a pool that did not walk whole: FAILED", "1 of 4");
        Counts Bad = Seen(4, 4);
        Bad.ExportFailed = 2;
        Is(Decide(Bad, X), State::Failed, "export failures: FAILED", "2 failures");

        // how much was exported
        Is(Decide(Seen(4, 0), X), State::WalkedOnly, "nothing exported: WALKED_ONLY", "0 of 4");
        Is(Decide(Seen(4, 3), X), State::WalkedOnly, "part exported: WALKED_ONLY", "exported 3 of 4");
        Counts Refs = Seen(4, 3);
        Refs.References = 1;
        Is(Decide(Refs, X), State::ExtractedNeutral, "references left to other zones do not count against it", "raw files");
        Is(Decide(Seen(4, 4), X), State::ExtractedNeutral, "all exported, a neutral form: EXTRACTED_NEUTRAL", "exported as raw files");
        Counts Stat = Seen(4, 4);
        Stat.ExportSource = Counts::Source::Static;
        Is(Decide(Stat, X), State::StaticOnly, "no run's report: STATIC_ONLY, never a terminal state", "no run's report was read");
        Check(!IsDone(State::StaticOnly), "STATIC_ONLY is not done", "");
        X.Row = &NoForm;
        Is(Decide(Seen(4, 4), X), State::WalkedOnly, "exported, no awi, no neutral form: not terminal", "no .awi and no neutral form");

        // an awi needs bo3-verify
        X.Row = &Xmodel;
        Is(Decide(Seen(5, 5), X), State::WalkedOnly, "exported with an awi but converted 0: WALKED_ONLY", "awaiting bo3-verify");
        Counts Gated = Seen(5, 5);
        Gated.GateRead = true;
        Is(Decide(Gated, X), State::WalkedOnly, "a gate file with no entry of the awi: WALKED_ONLY", "no xmodel entry");
        Gated.Converted = 5;
        Gated.GatePass = 5;
        Is(Decide(Gated, X), State::Converted, "all converted and through the gate: CONVERTED", "xmodel");
        Gated.GatePass = 4;
        Is(Decide(Gated, X), State::Failed, "a gate failure keeps it from CONVERTED: FAILED", "1 of 5 xmodel entries fail");
        Gated.GatePass = 5;
        Gated.GateFileFailures = 2;
        Is(Decide(Gated, X), State::Failed, "a file-level gate failure holds back every pool", "2 failures on files");
        Gated.GateFileFailures = 0;
        Gated.GateIncomplete = 2;
        Is(Decide(Gated, X), State::WalkedOnly, "a gate with checks it did not judge cannot make CONVERTED", "gate incomplete");
        Gated.GateIncomplete = 0;
        Gated.Converted = Gated.GatePass = 3;
        Is(Decide(Gated, X), State::WalkedOnly, "gated fewer entries than exported: not CONVERTED", "only 3 xmodel entries");

        // the done states
        Check(IsDone(State::Converted) && IsDone(State::ExtractedNeutral) && IsDone(State::Inert), "done states", "");
        Check(!IsDone(State::Failed) && !IsDone(State::Unmapped) && !IsDone(State::WalkedOnly), "FAILED and the non-terminal states are not done", "");

        // a kind several pools share has no per-pool count: never terminal
        Counts Sh = Seen(6, 6);
        Sh.SharedKind = true;
        Sh.SharedWith = "rawfile, luafile";
        Is(Decide(Sh, X), State::WalkedOnly, "a shared kind gives no terminal state", "shared by rawfile, luafile");
        Counts NotAsked = Seen(4, 0);
        NotAsked.KindNotRun = true;
        Is(Decide(NotAsked, X), State::WalkedOnly, "a kind the run did not ask for says so", "did not ask for kind");

        // the pool map file
        PoolMap Map;
        std::string Error;
        Check(ParsePoolMap(R"({"game":"x","pools":[{"pool":"rawfile","kind":"rawfile","bo3_awi":null,"neutral_form":"raw files","route":"direct"},{"pool":"ddl"}]})", Map, Error)
                  && Map.Rows.size() == 2 && Map.Rows.at("ddl").Route == "none" && Map.Rows.at("ddl").Kind.empty() && Map.Rows.at("rawfile").Neutral == "raw files",
              "pool map: rows, null fields, route defaults to none", Error);
        Check(!ParsePoolMap(R"({"pools":[{"kind":"model"}]})", Map, Error), "pool map: a row without a pool is refused", "");
        Check(!ParsePoolMap(R"({"pools":[{"pool":"a"},{"pool":"a"}]})", Map, Error), "pool map: two rows for a pool are refused", "");
        Check(!ParsePoolMap(R"({"pools":[{"pool":"a","kind":3}]})", Map, Error), "pool map: a non-string field is refused", "");
        Check(!ParsePoolMap("{ broken", Map, Error), "pool map: broken JSON is refused", "");

        // the run's report and the approximations
        const ExportReport Rep = ParseExportReport("result=ok\nzones=2\nmodels=25\nmodels_written=24\ndeps_images=88 from 3 zones\n"
                                                   "failure mp_x: model p7_crate bad\nfailure mp_x: image i_a bad\nfailure mp_x: image i_b bad\n"
                                                   "unresolved image ximage_1: no zone\nfailures=3\n");
        Check(Rep.Zones == 2 && Rep.Counters.at("models_written") == 24 && Rep.Counters.at("deps_images") == 88 && Rep.Counters.count("result") == 0,
              "report: numeric counters only", std::to_string(Rep.Counters.size()));
        Check(Rep.FailuresByKind.at("image") == 2 && Rep.FailuresByKind.at("model") == 1 && Rep.UnresolvedByKind.at("image") == 1 && Rep.Counters.at("failures") == 3,
              "report: failures and unresolved counted by kind", "");
        const auto Ap = CountApproximations("code\tasset\tdetail\tmessage\nmaterial.techset_fallback\ta\t\tm\nmaterial.techset_fallback\tb\t\tm\nname_shortened\tc\t\tm\n");
        Check(Ap.at("material") == 2 && Ap.at("") == 1 && Ap.size() == 2, "approximations: counted by code prefix", std::to_string(Ap.size()));

        // the gate file and the inert list
        const GateCounts G = ParseGate("# static gate: x\ncheck\tasset\tseverity\tdetail\nmaterial.surface_type\tmaterial:a\tfailure\td\ngdt.syntax\tfile:x.gdt\tfailure\td\n"
                                       "gate.entries\timage\tinfo\tentries=70 failed=2 warned=31\ngate.entries\tmaterial\tinfo\tentries=54 failed=54 warned=0\nsound (check 6)\t-\tnot_run\tw\n");
        Check(G.Read && G.Gdfs.at("image") == std::make_pair<size_t, size_t>(70, 2) && G.Gdfs.at("material").second == 54 && G.FileFailures == 1 && G.Gdfs.size() == 2, "gate file: per-GDF entries and failures, file failures", std::to_string(G.FileFailures));
        const std::set<std::string> Inert = ParseInertPools("# none yet\nrawfile  # proven in zm_x\n\n  ddl\n");
        Check(Inert.size() == 2 && Inert.count("rawfile") && Inert.count("ddl"), "inert_pools.txt: one pool per line, comments cut", std::to_string(Inert.size()));
        Check(ParseInertPools("").empty() && ParseInertPools("# only a comment\n").empty(), "an empty inert_pools.txt names nothing", "");

        // run identity
        const GateCounts Run = ParseGate("# s\ncheck\tasset\tseverity\tdetail\ngate.run\tscan\tinfo\tmode=scan gdts=29 sha256=abc roots=C:/out/bo4/bo3\n"
                                         "gate.entries\timage\tinfo\tentries=1 failed=0 warned=0\nawi.*\t-\tincomplete\tno root\nsound (check 6)\t-\tnot_run\tby design\nawi.range\t-\tnot_run\tx\n");
        Check(Run.HasRun && Run.RunMode == "scan" && Run.RunSha == "abc" && Run.RunGdts == 29 && Run.RunRoots == "C:/out/bo4/bo3", "gate.run row parsed", Run.RunRoots);
        Check(Run.Incomplete == 2, "an incomplete row and a not_run row of a required check count; a by-design not_run does not", std::to_string(Run.Incomplete));
        std::string Why;
        Check(GateMatchesRun(Run, "C:\\out\\bo4\\bo3", Why), "the same layout (slashes and case aside) matches", Why);
        Check(!GateMatchesRun(Run, "C:\\other\\bo3", Why) && Why.find("not this run's gate") != std::string::npos, "another layout is rejected", Why);
        Check(!GateMatchesRun(Run, "", Why) && Why.find("publish_output") != std::string::npos, "no publish_output: rejected", Why);
        Check(!GateMatchesRun(ParseGate("gate.entries\timage\tinfo\tentries=1 failed=0 warned=0\n"), "C:/out/bo4/bo3", Why) && Why.find("gate.run") != std::string::npos, "a gate without a run row is rejected", Why);
        const ExportReport Pub = ParseExportReport("result=ok\npublish_output=C:\\out\\bo4\\bo3\nzones=1\n");
        Check(Pub.Strings.count("publish_output") && Pub.Strings.at("publish_output") == "C:\\out\\bo4\\bo3" && Pub.Zones == 1, "publish_output is read from a report", "");

        // the export report must describe the zones that were walked
        {
            ExportReport Old = ParseExportReport("result=ok\nzones=1\nrawfiles=3\n");
            std::string W;
            Check(!ReportMatchesWalk(Old, { "casc:mp_frenetic.ff" }, W) && W.find("does not record its zones") != std::string::npos, "a report with no zone_names is rejected", W);
            ExportReport Other = ParseExportReport("result=ok\nzone_names=zm_factory.ff|mp_x.ff\nzones=2\n");
            Check(Other.HasZoneNames && Other.ZoneNames.size() == 2, "zone_names is read", std::to_string(Other.ZoneNames.size()));
            Check(!ReportMatchesWalk(Other, { "casc:mp_frenetic.ff" }, W) && W.find("only in the report: ") != std::string::npos && W.find("only walked: mp_frenetic.ff") != std::string::npos, "a report from other zones is rejected, both differences named", W);
            ExportReport Same = ParseExportReport("zone_names=casc:MP_Frenetic.ff\n");
            Check(ReportMatchesWalk(Same, { "F:\\bo4\\zone\\mp_frenetic.ff" }, W), "the same zone set matches (file name, no case)", W);
            Check(ReportMatchesWalk(Same, { "mp_frenetic" }, W) && ReportMatchesWalk(Same, { "MP_FRENETIC.zone" }, W), "a zone named with or without its .ff / .zone extension is the same zone", W);
            // D9: x.ff and x.zone are two walks: a report that names only x.ff does not match a run that walked both
            ExportReport XOnly = ParseExportReport("zone_names=x.ff\n");
            ExportReport XBoth = ParseExportReport("zone_names=x.ff|x.zone\n");
            Check(!ReportMatchesWalk(XOnly, { "x.ff", "x.zone" }, W) && ReportMatchesWalk(XBoth, { "x.ff", "x.zone" }, W) && !ReportMatchesWalk(XBoth, { "x.ff" }, W),
                  "D9: x.ff and x.zone stay two zones: a report of x.ff does not match walks {x.ff, x.zone}, a report of both does", W);
            Check(!ReportMatchesWalk(Same, { "mp_frenetic.ff", "mp_extra.ff" }, W) && W.find("only walked: mp_extra.ff") != std::string::npos, "a walked zone the report lacks is rejected", W);
            const GateCounts Gf = ParseGate("gate.run\tscan\tinfo\tmode=listed gdts=2 sha256=ab roots=C:/m\ngate.file\tsource_data/a.gdt\tinfo\troot=0\ngate.file\tb.gdt\tinfo\troot=1\n");
            Check(Gf.Files.size() == 2 && Gf.Files[0].first == 0 && Gf.Files[0].second == "source_data/a.gdt" && Gf.Files[1].first == 1, "gate.file rows are read", std::to_string(Gf.Files.size()));
            Counts Rej = Seen(5, 5);
            Rej.GateRejectWhy = "the GDT files are not the ones the gate read";
            X.Row = &Xmodel;
            Is(Decide(Rej, X), State::WalkedOnly, "a rejected gate leaves the pool non-terminal and says why", "the --gate file was rejected: the GDT files are not the ones the gate read");
        }

        // the writers
        Row One;
        One.Game = "g";
        One.Zone = "ALL";
        One.Pool = "p\tq";
        One.N = Seen(2, 2);
        One.StateText = "EXTRACTED_NEUTRAL";
        One.Reason = "a\nb";
        const std::string T = Tsv({ One });
        Check(T.find("\np\tq") == std::string::npos && T.find("p q\t2\t2\t2\tnone\t0\t0\tEXTRACTED_NEUTRAL\ta b\t0\t0\n") != std::string::npos, "tsv: cells carry no tab or line break", T);
        const nlohmann::json J = nlohmann::json::parse(Json("g", "p", { One }, { "z" }, { "rawfile" }, { "x" }));
        Check(J.at("totals_by_state").at("EXTRACTED_NEUTRAL") == 1 && J.at("rows").size() == 1 && J.at("rows")[0].at("converted") == 0, "json: totals and rows", "");
        return Failed;
    }
}
