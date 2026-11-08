#pragma once

// The Black Ops III conversion around a disk export (--target folder|modtools), the one code the command line (DiskGame.Cli.cpp)
// and the Load Game panel (ImGuiLoadGamePanel.Disk.cpp) both run, so neither has a second copy:
//
//   Request    what a caller asks for (the CLI's flags, or the GUI's settings through FromSettings);
//   ReadPublish  the Request as Bo3Target::Publish::Options;
//   Begin      the checks before anything is written (Touches, Preflight, the old reports removed);
//   Force      the options a target needs (material GDT, TIFF pictures, XMODEL_BIN, the full map);
//   Finish     the publish step and the static gate once the export is on disk, with the optional confirmation of a write
//              into a Mod Tools root.
//
// A Mod Tools root is written to only by Target "modtools". The command line gives its root with --mod-tools-root alone; the GUI
// gives the one its Settings page finds, and only after its confirmation (Hooks::Confirm).

#include "DiskGame.h"

#include "Bo3Target.Publish.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct CliOptions;

namespace DiskGame::Bo3
{
    struct Request
    {
        std::string Target;                // the text of --target: "", none, folder or modtools
        std::string ModToolsRoot;          // the Mod Tools folder given (the command line: --mod-tools-root only)
        std::string CheckGdtDb, GdtDbMatch, GdtMode;   // empty: the saved defaults (bo3_check_gdtdb, bo3_gdtdb_match, bo3_gdt_mode)
        std::string FolderOutputBase;      // the folder target's own place: <this>/<game>/bo3 (the GUI runs the export in a staging folder, the folder target
                                           // is still in the export folder); empty: <run folder>/bo3 as the command line has it
        bool Plan = false;                 // --plan
        bool FullMap = false;              // --full-map
        bool ImageFormatChosen = false;    // the caller chose the picture format: TIFF is not forced
        bool ModelFormatChosen = false;    // the caller chose the model formats: XMODEL_BIN is not forced
    };

    Request FromCli(const CliOptions& Cli);

    // --target and its companions as publish options. OutRoot is <out>/<game>. The root: only Request::ModToolsRoot names the one a
    // modtools target writes to; for the others the saved default, the install scan and TA_TOOLS_PATH are tried (read-only lookups).
    bool ReadPublish(const Request& R, const std::string& OutRoot, Bo3Target::Publish::Options& P, std::string& Error);

    // What a GUI run decided from its settings and its output folder.
    struct Decision
    {
        Request R;
        std::string Staging;            // set with Automatic: the staging folder of this run (OutFolder), removed when the run ends
        std::string OutFolder;          // the folder the export (staging) is written under: the asked one, or a staging folder when that lies in a Mod Tools install
        bool Automatic = false;         // the asked output folder lies in a Mod Tools install: the run is the Mod Tools target whatever the choice said
        std::string Why;                // set (and the run not to be started) when the asked settings cannot be honoured
    };
    // The GUI's choice (bo3_convert: off, folder, modtools; anything else is off) and full_map from the run's settings; AskedOut the output
    // folder as the user set it; ModToolsRoot the one the Settings page finds (FindModToolsRoot), which a modtools choice writes to.
    // An output folder in (or holding) a Mod Tools install makes the run the Mod Tools target on that install, and moves the staging
    // export out of it: the raw layout (<game>\maps, models, images...) is never written into a Mod Tools root.
    Decision FromSettings(const std::map<std::string, std::string>& Settings, const std::string& AskedOut, const std::string& ModToolsRoot,
                          const std::string& StagingBase);
    // The Mod Tools root the Settings page finds: the Mod Tools folder setting, then the BO3 install of the install scan (Steam), then
    // TA_TOOLS_PATH. Empty when none; How says which.
    std::string FindModToolsRoot(std::string& How);
    // Where an automatic run's staging export goes: a folder of its own for this run (GrayWolf-bo3-staging-<process>-<tick>) in Parent, the
    // temporary folder when empty. The path only: Prepare makes it (with a marker file naming the process) and removes it at the end.
    std::string NewStagingFolder(const std::string& Parent = {});
    // Removes a staging folder this program made: only a folder with that name, directly in a parent, holding the marker. Empty, or why not.
    std::string RemoveStaging(const std::string& Folder);
    // At start: removes the staging folders a run left behind (the same name and marker, the process named in the marker no longer
    // running). Nothing else in Parent is touched. Returns how many were removed; Failed gets a line for each that could not be.
    size_t RemoveStaleStaging(const std::string& Parent = {}, std::vector<std::string>* Failed = nullptr);

    // Every run, whatever its target (none included): the directories that already exist under the run folder (and under the folder target's
    // own folder) are scanned, directories only. A junction or symlink whose real place is in a Mod Tools install, or in the game install
    // (GameInstall, may be empty), refuses the run before anything is written; the answer names the link. Empty when none.
    std::string CheckRunFolder(const std::string& RunRoot, const std::string& GameInstall, const Bo3Target::Publish::Options& P);

    // Makes a staging folder (it must not exist), with its marker. When the marker cannot be written the folder just made is removed by its
    // exact path, the marker's temporary file with it, after checking its name and that it is no junction. Empty, or why not.
    std::string MakeStaging(const std::string& Folder);
    bool& MarkerFailureForTest();    // a test makes the marker write fail

    // The checks before the export: with a target, an output inside a Mod Tools install is refused (AskedOut is what the user asked for), the
    // target's Preflight, and the reports an earlier run left in RunRoot removed. Empty, or the reason the run is refused.
    std::string Begin(const Bo3Target::Publish::Options& P, const std::string& AskedOut, const std::string& RunRoot);

    // The settings a target needs: the material GDT on, the Radiant .map with it when maps are exported, the full map as asked, pictures converted, the constants kept for the techsets, TIFF
    // pictures and XMODEL_BIN unless the Request says the caller chose others. No target: the conversion's keys are taken out of the options.
    // Unforce undoes the process-wide parts (run settings, the sidecar constants) for a caller that lives on (the GUI).
    void Force(const Bo3Target::Publish::Options& P, const Request& R, ExportOptions& O, Context& C);
    void Unforce();

    // What the confirmation of a write into a Mod Tools root is shown, from the plan (a run of the publish step that writes nothing).
    struct PlanSummary
    {
        std::filesystem::path Root;
        size_t FilesCreate = 0, FilesMerge = 0, FilesReplace = 0, GdtBackups = 0;
        size_t Unchanged = 0, InModTools = 0, NameConflicts = 0, ImageClashes = 0, Conflicts = 0;
        bool GdtDbChecked = false;       // the gdt.db of the root was read for the clashes
        std::string Text;                // the lines above, in words
    };

    // The question put to the user before a write into a Mod Tools root: the worker asks and waits, the UI thread shows it and answers.
    // Cancel (a run stopped, the window closing) is No: Wake makes a waiting Ask return false at once.
    class Question
    {
    public:
        // Worker: puts Text up and waits for Answer (true on Yes), or for Cancel to be set and Wake called (false).
        bool Ask(const std::string& Text, const std::atomic<bool>& Cancel);
        // UI thread: the text while a question is waiting; false when none.
        bool Pending(std::string& Text) const;
        void Answer(bool Yes);
        void Wake();                         // after setting the cancel flag
        void ShowForTest(const std::string& Text);   // ui-shot: a question with no worker behind it
        void DropForTest();

    private:
        mutable std::mutex Lock;
        std::condition_variable Changed;
        int State = 0;                       // 0 none, 1 asked, 2 yes, 3 no
        std::string Shown;
    };

    struct Hooks
    {
        // Before a write into a Mod Tools root: false declines (nothing is written there). Not set: no question (the command line).
        std::function<bool(const PlanSummary&)> Confirm;
    };

    struct Outcome
    {
        std::string Lines;               // the report lines (publish_*, gate_*)
        std::string Refused;             // the publish step refused or failed
        bool Declined = false;           // the confirmation said no
        bool GateRun = false;
        std::string OutputRoot;          // where the layout was written
        std::string GateWord, GateSummary, GateReport;
        int GateFailures = 0, GateWarnings = 0, GateExit = 0;
        size_t Backups = 0;              // .bak files made (publish_gdt_backups)
        size_t FilesWritten = 0;
        std::string PlanText;            // the confirmation's text, when one was asked
    };

    // The publish step and the gate over what the export wrote under RunRoot (<out>/<game>); counters into Into.
    // Without H.Confirm a modtools target is written with no question: that is the command line's (its --mod-tools-root is the consent).
    // A GUI-shaped run goes through Complete, which refuses a modtools target when no hook is set.
    Outcome Finish(const IDiskGame& Game, const Bo3Target::Publish::Options& P, const std::string& RunRoot, Totals& Into, const Hooks& H = {});

    // A GUI-shaped run (the Load Game panel's, and gui-export-check's): Prepare before the export, Complete after it.
    struct Prepared
    {
        Bo3Target::Publish::Options Publish;
        bool Converting = false;         // a target is set
        double SavedFloor = -1.0;        // the free space floor before the run (the GUI has none unless converting)
        std::string Error;               // set: the run is not to start (nothing was forced)
        std::string Staging;             // the staging folder this run made (removed by Complete / Abandon)
        std::string CleanupError;        // Prepare removed the staging folder after a refusal and could not
        // what the run really does, which may differ from the Decision asked (a folder that resolves into an install makes it automatic)
        bool Automatic = false;
        std::string OutFolder;
    };
    // The run's folders and options from the Decision: C.OutRoot (<OutFolder>/<game>), ReadPublish, Begin, Force and, when converting,
    // the free space floor of the command line (8 GB unless set), checked on the drive of the export and the drive of the target: a run
    // that would start under the floor is refused (Error names the drive). Makes the staging folder of an automatic run.
    Prepared Prepare(const IDiskGame& Game, const Decision& D, ExportOptions& O, Context& C);
    struct Completed
    {
        Outcome Out;
        bool FloorReached = false;       // the free space floor stopped the run (its row is written beside the reports)
        std::string CleanupError;        // the staging folder could not be removed (it stays where it is)
    };
    // After the export: the publish step and gate (a cancelled export is not converted), then the process-wide forced settings and the floor given back.
    Completed Complete(const IDiskGame& Game, Prepared& Run, const Context& C, Totals& T, const Hooks& H);
    // The run ended before an export: gives back what Prepare set and removes the staging folder. Empty, or why that failed.
    std::string Abandon(Prepared& Run);

    // Self-test: the decisions above on a fake Mod Tools root made in a temporary folder (never a real install).
    int SelfTest(std::string& Failures);
}
