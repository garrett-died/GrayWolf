#pragma once

// The publish step of the Black Ops III conversion layer: after a disk-export run, turn the run's
// <model>.materials.json sidecars and exported files into the Mod Tools layout (--target folder|modtools).
// See docs/hydrax-port/bo3-conversion-design.md (modules 3 to 5, CLI) and bo3-target-hydrax.md.
//
// Generic on purpose: a game's exporter is not touched. Any game that writes sidecars gets the step.
//
//   - each model is its own family: its materials and images are placed with it, except an image used
//     by two or more models (the texture library); Placement (module 3) decides every path;
//   - the gdt.db (module 4) says what the Mod Tools already hold: such an asset is not written
//     (already_in_mod_tools, or name_conflict for a name of another game) but its references are
//     followed: a held material's images are each checked on their own;
//   - Output (module 5) decides what may be written: never over a file the manifest does not list.
//
// Reports under the run's output: graywolf_placement.tsv, graywolf_approximations.tsv, and the counters.

#include "Bo3Target.GdtDb.h"
#include "Bo3Target.Output.h"
#include "Bo3Target.Placement.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Bo3Target::Publish
{
    enum class Target
    {
        None,       // nothing is published: the export is what it always was
        Folder,     // the Mod Tools layout mirrored under <out>/<game>/bo3/
        ModTools,   // written into the Mod Tools root; only ever chosen on the command line, each run
    };
    bool ParseTarget(std::string_view Text, Target& Out);

    struct Options
    {
        Target Which = Target::None;
        std::filesystem::path ModToolsRoot;     // gdt.db, bin and the stock index; empty when none was found
        std::filesystem::path OutputRoot;       // where the layout is written (Folder: <out>/<game>/bo3, ModTools: the root)
        bool CheckGdtDb = true;                 // false: nothing is skipped for being in the gdt.db
        GdtDb::MatchMode Match = GdtDb::MatchMode::Type;
        bool Merge = true;                      // --gdt merge; false: overwrite (a file GrayWolf owns is replaced whole)
        bool Plan = false;                      // decide and report, write nothing but the reports
        bool FullMap = false;                   // --full-map: the maps' dependency lists (<run>/maps/*.full_map.tsv) are walked and reported
    };

    // The Mod Tools root: the value given, else the BO3 install the install scan finds (InstallScan, which
    // may be empty), else TA_TOOLS_PATH. How says which one. False when none is a folder.
    bool ResolveRoot(const std::string& Given, const std::string& InstallScan, const std::string& TaToolsPath, std::filesystem::path& Out,
                     std::string& How);

    // The root for a target. ModTools writes into it, so only Given (--mod-tools-root on the command line) counts and
    // nothing else is tried; an empty Given is refused with Error. For the others Given may be the saved default, and
    // InstallScan and TaToolsPath follow as in ResolveRoot (read-only lookups). Error is set when false is returned.
    bool ChooseRoot(Target Which, const std::string& Given, const std::string& InstallScan, const std::string& TaToolsPath,
                    std::filesystem::path& Out, std::string& How, std::string& Error);

    // What the Mod Tools hold, as the publish step needs it.
    struct Knowledge
    {
        const GdtDb::Catalog* Held = nullptr;           // null: no check (off, or the database is unreadable)
        const GdtDb::StockIndex* Stock = nullptr;       // required
        size_t OwnRows = 0;                             // gdt.db rows left out of Held: their gdt is in the graywolf manifest (GrayWolf rewrites it)
        size_t StaleRows = 0;                           // gdt.db rows left out of Held: their gdt file is not on disk (the database is behind)
    };

    // Loads the gdt.db of Root read-only and the stock index. Warnings (one for an unreadable database) are
    // appended to Warnings; the result then holds no catalog and an empty stock index.
    class Loaded
    {
    public:
        Loaded();
        ~Loaded();
        Loaded(const Loaded&) = delete;
        Loaded& operator=(const Loaded&) = delete;
        void Open(const std::filesystem::path& Root, bool Check, std::vector<std::string>& Warnings);
        Knowledge View() const;

    private:
        struct State;
        std::unique_ptr<State> Data;
    };

    struct Result
    {
        std::map<std::string, size_t> Counters;     // publish_<what>
        std::vector<std::string> Warnings;
        std::string Error;                          // set: nothing was written, the step was refused
        // --full-map: the GDT asset name of every material the maps' own geometry draws, with the first surface that names it ("world#3",
        // "terrain_0#1", "skybox#0"). The gate fails one of these left <not in editor>, and names the surface.
        std::map<std::string, std::string> MapMaterials;
        // --full-map: a map-drawn material the Mod Tools already hold (no entry is written for it): asset name (lower case) -> { the gdt that
        // holds it, its usage there or "unavailable: why" }. The held entry is never rewritten; the gate warns for each.
        std::map<std::string, std::pair<std::string, std::string>> MapHeld;
        bool MapDataComplete = false;               // --full-map: every list was read and every material row understood; else MapMaterials is not the whole picture
        std::string MapDataWhy;
    };

    // Checks what can be checked before the export runs: the target's output folder is not inside an install
    // (Folder), is one (ModTools), and the manifest there reads. An error aborts before any write.
    std::string Preflight(const Options& O);

    // The reports one run leaves in its run folder: the placement, approximations and techset-rule reports and the gate result.
    // ClearRunReports removes them (nothing else is touched), and returns an error text when one that exists cannot be removed (read-only
    // and locked files included): an old report that survives could be read as this run's result, so the run must stop or ignore it.
    std::string ClearRunReports(const std::filesystem::path& RunRoot);
    // The same for the gate result alone (graywolf_gate.tsv, graywolf_gate_summary.txt)
    std::string ClearGateReports(const std::filesystem::path& RunRoot);

    // The step: sidecars and files under RunRoot (<out>/<game>), the game's id and title.
    Result Run(const Options& O, const Knowledge& Know, const std::filesystem::path& RunRoot, const std::string& GameId);

    // Self-tests on a temporary folder (the rules of module 5 end to end); never touches a real install.
    int SelfTest(std::string& Failures);
}
