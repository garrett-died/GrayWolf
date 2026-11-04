#pragma once

// pool-coverage: per game and pool, which terminal state the pool has reached (docs/hydrax-port/plan-materials.md
// section 1, docs/hydrax-port/p1-pool-coverage.md). This file is the pure part: the pool map, the state
// decision and the report writers. Walking zones and reading the export run is PoolCoverageCli.cpp.
//
// Nothing here is faked: converted and gate_pass come from a graywolf_gate.tsv (bo3-verify) given with --gate and stay 0
// without one, and a pool whose row names no exporter, awi or neutral form is reported as it is (WALKED_ONLY), never given
// a form to pass. "Seen 0" is not "inert": it is NOT_SEEN until a list or every zone of the game says more.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace PoolCoverage
{
    enum class State { Converted, ExtractedNeutral, Inert, Failed, Unmapped, WalkedOnly, NotSeen, StaticOnly };

    const char* StateName(State S);
    // CONVERTED, EXTRACTED_NEUTRAL and INERT. FAILED is a terminal state in the plan but a bug, so it is not "done" here either.
    // NOT_SEEN (seen 0 in the zones tested, no proof) and STATIC_ONLY (no run's report was read) are not done.
    bool IsDone(State S);

    // One row of zonekit/profiles/<game>/pool_map.json. An empty string is null in the file.
    //   route: "direct"          the pool's assets are counted by its kind (rawfile, image...)
    //          "carried:<pool>"  written inside another pool's output (xmodelmesh in xmodel)
    //          "map"             part of the map export, not counted per pool
    //          "none"            nothing exports it
    struct MapRow
    {
        std::string Pool, Kind, Awi, Neutral, Route;
    };

    struct PoolMap
    {
        std::string Game;
        std::map<std::string, MapRow> Rows;
    };

    // False with Error when the text is not a pool map (a row without a pool, a non-string field).
    bool ParsePoolMap(const std::string& Text, PoolMap& Out, std::string& Error);

    // What was measured for one pool, in one zone or over every zone tested.
    struct Counts
    {
        size_t Seen = 0;         // zone-level assets of the pool the walk entered (plus one it had no table for)
        size_t WalkedOk = 0;     // ... read whole
        std::string WalkWhy;     // the first walk failure's reason
        size_t Exported = 0;     // written + known + linked, for a run's report; the pool's seen for a static count
        enum class Source { None, Report, Static } ExportSource = Source::None;
        size_t References = 0;   // name-only headers the run left to the zone that defines them
        bool KindNotRun = false; // a run's report has no line for the pool's kind: the run did not ask for it
        size_t ExportFailed = 0;
        size_t Converted = 0, GatePass = 0;   // from a graywolf_gate.tsv (--gate): the pool's awi entries, and those without a gate failure
        bool GateRead = false;                // a gate file was given
        size_t GateFileFailures = 0;          // failures of the gate that belong to a file, not a GDF: they hold back every pool
        size_t GateIncomplete = 0;            // checks the gate could not judge (incomplete rows, not_run rows of a required check): no CONVERTED
        std::string GateRejectWhy;            // a --gate file was given and rejected: why (the pool stays non-terminal)
        bool SharedKind = false;              // the pool's kind is shared with other pools: the run reports one total for the kind
        std::string SharedWith;               // ... those pools
        size_t Approximations = 0, UnresolvedNames = 0;
    };

    // What is known beside the counts.
    struct Context
    {
        const MapRow* Row = nullptr;   // nullptr: the pool has no row (or the file is missing)
        bool KindSupported = true;     // the game has an exporter for Row->Kind
        bool InertProven = false;      // inert_pools.txt names the pool
        std::string InertProof;
        size_t ZonesTested = 0;        // zones the game walked
        size_t ZonesComplete = 0;      // ... of which to the end of the zone
        size_t ZonesInGame = 0;        // --all-zones: the zones the install lists; every one of them was walked when this equals ZonesTested; else 0
    };

    struct Verdict
    {
        State S = State::Unmapped;
        std::string Reason;
    };

    // The state of a pool over every zone tested. See the order of rules in PoolCoverage.cpp.
    Verdict Decide(const Counts& C, const Context& X);

    // ---- the report

    struct Row
    {
        std::string Game, Zone, Pool;
        Counts N;
        std::string StateText;    // StateName(), or "-" for a per-zone row (the state is on the ALL row)
        std::string Reason;
    };

    std::string Tsv(const std::vector<Row>& Rows);
    std::string Json(const std::string& Game, const std::string& Profile, const std::vector<Row>& Rows,
                     const std::vector<std::string>& Zones, const std::vector<std::string>& GameKinds,
                     const std::vector<std::string>& Missing);

    // The text of the export run's report (key=value lines): counters, failures and unresolved by kind name.
    struct ExportReport
    {
        std::map<std::string, size_t> Counters;
        std::map<std::string, size_t> FailuresByKind, UnresolvedByKind;
        std::map<std::string, std::string> Strings;     // publish_output: the layout a --target run wrote
        bool HasZoneNames = false;                      // the report records its zones (zone_names=a|b, written by disk-export)
        std::set<std::string> ZoneNames;                // ... as written
        size_t Zones = 0;
    };
    ExportReport ParseExportReport(const std::string& Text);

    // The run's graywolf_approximations.tsv: rows per code prefix ("material" of material.techset_fallback);
    // codes without a dot (name_shortened) go under "".
    std::map<std::string, size_t> CountApproximations(const std::string& Text);

    // The run's graywolf_gate.tsv (bo3-verify): per GDF the entries and those with a failure (the gate.entries rows), and the
    // failures that belong to a file (asset "file:...") or to the gate itself.
    struct GateCounts
    {
        bool Read = false;
        std::map<std::string, std::pair<size_t, size_t>> Gdfs;    // gdf -> entries, entries with a failure
        size_t FileFailures = 0;
        size_t Incomplete = 0;      // "incomplete" rows, and "not_run" rows of a check that is required (awi., gdt., material., image.)
        bool HasRun = false;        // the gate.run row: which layout the gate read
        std::string RunMode, RunSha, RunRoots;
        std::vector<std::pair<size_t, std::string>> Files;      // gate.file rows: (root index, path relative to it)
        size_t RunGdts = 0;
    };
    GateCounts ParseGate(const std::string& Text);
    // An export report describes the zones that were walked only when it records the same set (compared by file name, no case).
    bool ReportMatchesWalk(const ExportReport& R, const std::vector<std::string>& Walked, std::string& Why);

    // A gate belongs to a run only when it names the layout the run's report says it published (publish_output). False with Why.
    bool GateMatchesRun(const GateCounts& G, const std::string& PublishOutput, std::string& Why);

    // One path per line of inert_pools.txt: the pools a person has proven empty. '#' starts a comment.
    std::set<std::string> ParseInertPools(const std::string& Text);

    int SelfTest(std::string& Failures);
}
