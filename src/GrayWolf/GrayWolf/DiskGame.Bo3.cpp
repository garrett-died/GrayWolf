#include "stdafx.h"

#include "DiskGame.Bo3.h"

#include "Bo3Target.Output.h"
#include "Bo3Target.Techsets.h"
#include "Bo3Target.Verify.h"
#include "CliOptions.h"
#include "DiskGuard.h"
#include "GameInstalls.h"
#include "MaterialDescription.h"
#include "SettingsManager.h"
#include "ZonekitZone.h"

#include <algorithm>
#include <future>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <utility>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace DiskGame::Bo3
{
namespace
{
    namespace Pb = Bo3Target::Publish;

    // A setting of graywolf.json that gives a default to a value: the caller's own wins. Never the target.
    std::string SavedDefault(const std::string& Given, const char* Key)
    {
        return Given.empty() ? SettingsManager::GetSetting(Key, "") : Given;
    }

    bool OnOff(const std::string& Text, bool& Out)
    {
        if (Text == "on" || Text == "true" || Text == "yes") { Out = true; return true; }
        if (Text == "off" || Text == "false" || Text == "no") { Out = false; return true; }
        return false;
    }

    std::string TaToolsPath()
    {
        char* Env = nullptr;
        size_t Length = 0;
        std::string Out;
        if (_dupenv_s(&Env, &Length, "TA_TOOLS_PATH") == 0 && Env != nullptr)
        {
            Out = Env;
            free(Env);   // NOLINT: _dupenv_s allocates with malloc
        }
        return Out;
    }

    std::string InstallScanBo3()
    {
        for (const auto& I : GameInstalls::Detect())
            if (I.DiskId == "bo3") return I.Path;
        return {};
    }

    std::vector<std::string> Split(const std::string& Text, char Sep)
    {
        std::vector<std::string> Out;
        std::stringstream Ss(Text);
        for (std::string Item; std::getline(Ss, Item, Sep);) Out.push_back(Item);
        return Out;
    }

    std::string Slurp(const fs::path& File)
    {
        std::ifstream In(File, std::ios::binary);
        std::ostringstream S;
        S << In.rdbuf();
        return S.str();
    }
}

Request FromCli(const CliOptions& Cli)
{
    Request R;
    R.Target = Cli.PublishTarget;
    R.ModToolsRoot = Cli.ModToolsRoot;
    R.CheckGdtDb = Cli.CheckGdtDb;
    R.GdtDbMatch = Cli.GdtDbMatch;
    R.GdtMode = Cli.GdtMode;
    R.Plan = Cli.PublishPlan;
    R.FullMap = Cli.FullMap;
    R.ImageFormatChosen = !Cli.ImageFormat.empty();
    R.ModelFormatChosen = !Cli.ModelFormat.empty();
    return R;
}

// --target and its companions. The command line gives the Mod Tools root of a modtools target with --mod-tools-root alone, so a saved
// setting can never select it; the other values default to graywolf.json (bo3_check_gdtdb, bo3_gdtdb_match, bo3_gdt_mode,
// bo3_mod_tools_root) when the caller does not give them.
bool ReadPublish(const Request& R, const std::string& OutRoot, Pb::Options& P, std::string& Error)
{
    if (!R.Target.empty() && !Pb::ParseTarget(R.Target, P.Which))
    {
        Error = "unknown --target " + R.Target + ": none, folder or modtools";
        return false;
    }
    if (P.Which == Pb::Target::None)
    {
        if (R.Plan || R.FullMap || !R.CheckGdtDb.empty() || !R.GdtDbMatch.empty() || !R.GdtMode.empty())
        {
            Error = "--plan, --full-map, --check-gdtdb, --gdtdb-match and --gdt need --target folder or modtools";
            return false;
        }
        return true;
    }
    P.Plan = R.Plan;
    P.FullMap = R.FullMap;
    const std::string Check = SavedDefault(R.CheckGdtDb, "bo3_check_gdtdb");
    if (!Check.empty() && !OnOff(Check, P.CheckGdtDb))
    {
        Error = "unknown --check-gdtdb " + Check + ": on or off";
        return false;
    }
    const std::string Match = SavedDefault(R.GdtDbMatch, "bo3_gdtdb_match");
    if (!Match.empty() && !Bo3Target::GdtDb::ParseMatchMode(Match, P.Match))
    {
        Error = "unknown --gdtdb-match " + Match + ": type or name";
        return false;
    }
    const std::string Mode = SavedDefault(R.GdtMode, "bo3_gdt_mode");
    if (!Mode.empty() && Mode != "merge" && Mode != "overwrite")
    {
        Error = "unknown --gdt " + Mode + ": merge or overwrite";
        return false;
    }
    P.Merge = Mode != "overwrite";

    // the Mod Tools root. A modtools target writes into it, so only the root the caller names counts and nothing else is tried; for the
    // read-only lookups (gdt.db, stock index) the saved default, the install scan and TA_TOOLS_PATH also count
    std::string Given = R.ModToolsRoot, Scan, Ta;
    if (P.Which != Pb::Target::ModTools)
    {
        Given = SavedDefault(R.ModToolsRoot, "bo3_mod_tools_root");
        if (Given.empty())
        {
            Scan = InstallScanBo3();
            Ta = TaToolsPath();
        }
    }
    std::string How;
    if (!Pb::ChooseRoot(P.Which, Given, Scan, Ta, P.ModToolsRoot, How, Error)) return false;
    if (P.Which == Pb::Target::Folder)
        P.OutputRoot = R.FolderOutputBase.empty() ? fs::path(OutRoot) / "bo3" : fs::path(R.FolderOutputBase) / fs::path(OutRoot).filename() / "bo3";
    else
        P.OutputRoot = P.ModToolsRoot;
    return true;
}

std::string FindModToolsRoot(std::string& How)
{
    std::error_code Ec;
    const auto Try = [&](const std::string& Text, const char* Name) {
        if (Text.empty() || !fs::is_directory(fs::path(Text), Ec)) return false;
        How = Name;
        return true;
    };
    const std::string Setting = SettingsManager::GetSetting("modtools_dir", "");   // SkyLighting::kFolderKey: the Settings page's Mod Tools folder
    if (Try(Setting, "the Mod Tools folder setting")) return Setting;
    const std::string Scan = InstallScanBo3();
    if (Try(Scan, "the BO3 install found in your Steam libraries")) return Scan;
    const std::string Ta = TaToolsPath();
    if (Try(Ta, "TA_TOOLS_PATH")) return Ta;
    How = "none found";
    return {};
}

namespace
{
    constexpr const char* StagingPrefix = "GrayWolf-bo3-staging-";
    constexpr const char* StagingMarker = ".graywolf-staging";

    fs::path StagingParent(const std::string& Parent)
    {
        std::error_code Ec;
        return Parent.empty() ? fs::temp_directory_path(Ec) : fs::path(Parent);
    }

    // A junction or a symlink: never followed by a delete
    bool IsReparse(const fs::path& P)
    {
        const DWORD A = GetFileAttributesW(P.c_str());
        return A != INVALID_FILE_ATTRIBUTES && (A & FILE_ATTRIBUTE_REPARSE_POINT) != 0;   // a path that is not there is no link (IsStaging also wants a directory)
    }

    // Dead only when the system says there is no such process (ERROR_INVALID_PARAMETER); any other failure (access denied) means it exists
    bool ProcessIsGone(unsigned long Pid)
    {
        HANDLE H = OpenProcess(SYNCHRONIZE, FALSE, Pid);
        if (H == nullptr) return GetLastError() == ERROR_INVALID_PARAMETER;
        const bool Gone = WaitForSingleObject(H, 0) == WAIT_OBJECT_0;
        CloseHandle(H);
        return Gone;
    }

    // a folder this program made: its name, directly in some parent, the marker inside it, and not a junction or symlink
    bool IsStaging(const fs::path& Folder)
    {
        std::error_code Ec;
        const std::string Name = Folder.filename().string();
        return Name.rfind(StagingPrefix, 0) == 0 && !IsReparse(Folder) && fs::is_directory(Folder, Ec) && fs::is_regular_file(Folder / StagingMarker, Ec) &&
               !Bo3Target::Output::IsInstall(Folder);
    }

    // The marker with this process's id, written whole: a temporary file, then renamed over the name
    bool WriteMarker(const fs::path& Folder)
    {
        const fs::path Temp = Folder / (std::string(StagingMarker) + ".tmp");
        {
            std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
            Out << GetCurrentProcessId();
            Out.flush();
            if (!Out || MarkerFailureForTest()) return false;   // the temporary file stays: the caller removes the whole folder
        }
        std::error_code Ec;
        fs::rename(Temp, Folder / StagingMarker, Ec);
        if (Ec) fs::remove(Temp, Ec);
        return !Ec;
    }
}

bool& MarkerFailureForTest()
{
    static bool Fail = false;
    return Fail;
}

std::string MakeStaging(const std::string& Folder)
{
    const fs::path P(Folder);
    std::error_code Ec;
    const bool Made = fs::create_directories(P, Ec);
    if (Ec || !Made) return "the staging folder " + Folder + " could not be made" + (Made || Ec ? "" : " (it exists)");
    if (WriteMarker(P)) return {};
    // the folder this call just made, by its exact path (the marker is not there yet, so RemoveStaging would refuse it): only with the
    // staging name and when it is no junction or symlink
    std::string Why = "the staging folder " + Folder + " could not be marked";
    if (P.filename().string().rfind(StagingPrefix, 0) != 0 || IsReparse(P) || !fs::is_directory(P, Ec))
        return Why + " and was left (not a staging folder by name)";
    fs::remove_all(P, Ec);
    if (Ec || fs::exists(P, Ec)) Why += " and could not be removed" + (Ec ? " (" + Ec.message() + ")" : std::string());
    return Why;
}

std::string NewStagingFolder(const std::string& Parent)
{
    return (StagingParent(Parent) / (std::string(StagingPrefix) + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()))).string();
}

std::string RemoveStaging(const std::string& Folder)
{
    if (Folder.empty()) return {};
    const fs::path P(Folder);
    std::error_code Ec;
    if (!fs::exists(P, Ec) && !IsReparse(P)) return {};
    if (!IsStaging(P)) return Folder + " is not a staging folder of this program: not removed";
    fs::remove_all(P, Ec);
    if (Ec || fs::exists(P, Ec)) return "the staging folder " + Folder + " could not be removed" + (Ec ? " (" + Ec.message() + ")" : std::string());
    return {};
}

size_t RemoveStaleStaging(const std::string& Parent, std::vector<std::string>* Failed)
{
    size_t Removed = 0;
    std::error_code Ec;
    for (fs::directory_iterator It(StagingParent(Parent), Ec), End; !Ec && It != End; It.increment(Ec))
    {
        const fs::path P = It->path();
        if (!IsStaging(P)) continue;     // another name, no marker, or a junction or symlink
        // an unreadable or empty marker, or process 0, is a live run: only a readable id of a process that is gone makes it stale
        const unsigned long Pid = std::strtoul(Slurp(P / StagingMarker).c_str(), nullptr, 10);
        if (Pid == 0 || Pid == GetCurrentProcessId() || !ProcessIsGone(Pid)) continue;
        if (const std::string Why = RemoveStaging(P.string()); Why.empty()) Removed++;
        else if (Failed != nullptr) Failed->push_back(Why);
    }
    return Removed;
}

bool Question::Ask(const std::string& Text, const std::atomic<bool>& Cancel)
{
    std::unique_lock<std::mutex> Lk(Lock);
    Shown = Text;
    State = 1;
    while (State == 1 && !Cancel.load()) Changed.wait_for(Lk, std::chrono::milliseconds(100));
    const bool Yes = State == 2;
    State = 0;
    return Yes;
}

bool Question::Pending(std::string& Text) const
{
    std::lock_guard<std::mutex> Lk(Lock);
    if (State != 1) return false;
    Text = Shown;
    return true;
}

void Question::Answer(bool Yes)
{
    {
        std::lock_guard<std::mutex> Lk(Lock);
        if (State == 1) State = Yes ? 2 : 3;
    }
    Changed.notify_all();
}

void Question::Wake()
{
    Changed.notify_all();
}

void Question::ShowForTest(const std::string& Text)
{
    std::lock_guard<std::mutex> Lk(Lock);
    Shown = Text;
    State = 1;
}

void Question::DropForTest()
{
    std::lock_guard<std::mutex> Lk(Lock);
    State = 0;
}

Decision FromSettings(const std::map<std::string, std::string>& Settings, const std::string& AskedOut, const std::string& ModToolsRoot,
                      const std::string& StagingBase)
{
    Decision D;
    D.OutFolder = AskedOut;
    const auto Get = [&](const char* Key) {
        const auto It = Settings.find(Key);
        return It == Settings.end() ? std::string() : It->second;
    };
    std::string Choice = Get("bo3_convert");
    if (Choice != "folder" && Choice != "modtools") Choice.clear();   // anything else is off
    // the output folder in (or holding) a Mod Tools install: the Mod Tools target on that install, whatever the choice says
    fs::path Found;
    const bool Inside = !AskedOut.empty() && Bo3Target::Output::Touches(AskedOut, &Found);
    if (Inside)
    {
        D.Automatic = true;
        if (Found.empty())
        {
            D.Why = "GrayWolf cannot tell where the export folder " + AskedOut + " is, and it may lie in the Black Ops III Mod Tools: choose another folder in Settings";
            return D;
        }
        // an install found as a link inside the folder (a junction or symlink to it) is no place to write: the run is refused
        if (IsReparse(Found))
        {
            D.Why = "The export folder " + AskedOut + " holds " + Found.string() + ", a junction or symlink into a Mod Tools install: the run is refused. Remove the link or choose another folder in Settings";
            return D;
        }
        Choice = "modtools";
        D.R.ModToolsRoot = Found.string();
        D.OutFolder = StagingBase;
        D.Staging = StagingBase;
    }
    else if (Choice == "modtools")
    {
        if (ModToolsRoot.empty() || !Bo3Target::Output::IsInstall(ModToolsRoot))
        {
            D.Why = "The Black Ops III Mod Tools folder was not found: set it in Settings, under Black Ops III Mod Tools";
            return D;
        }
        D.R.ModToolsRoot = ModToolsRoot;
    }
    else if (Choice == "folder")
        D.R.ModToolsRoot = ModToolsRoot;   // a read-only lookup (gdt.db, the stock index)
    D.R.Target = Choice;
    // a run with a target writes its raw export into a folder it makes itself (a new folder holds no junction); the folder target
    // still ends in the export folder the user chose
    if (!Choice.empty() && !D.Automatic)
    {
        D.OutFolder = StagingBase;
        D.Staging = StagingBase;
        if (Choice == "folder") D.R.FolderOutputBase = AskedOut;
    }
    if (!Choice.empty()) D.R.FullMap = Get("full_map") == "on";
    D.R.ImageFormatChosen = true;          // the Image format setting is the user's choice
    D.R.ModelFormatChosen = false;         // XMODEL_BIN is what a published xmodel entry references
    return D;
}

std::string CheckRunFolder(const std::string& RunRoot, const std::string& GameInstall, const Pb::Options& P)
{
    std::error_code Ec;
    const auto Judge = [&](const fs::path& Link) -> std::string {
        if (!IsReparse(Link)) return {};
        const fs::path Real = fs::weakly_canonical(Link, Ec);
        if (Ec) return "The folder " + Link.string() + " is a junction or symlink that cannot be resolved: the run is refused";
        fs::path Install;
        if (Bo3Target::Output::Touches(Real, &Install))
            return "The folder " + Link.string() + " is a junction or symlink that leads into the Mod Tools install " + Install.string() + " (" + Real.string() +
                   "): the run is refused, nothing was written. Remove the link or use another export folder.";
        if (!GameInstall.empty())
        {
            const fs::path Game = fs::weakly_canonical(fs::path(GameInstall), Ec);
            if (!Ec && (Real == Game || Bo3Target::Output::IsUnder(Game, Real)))
                return "The folder " + Link.string() + " is a junction or symlink that leads into the game install " + Game.string() + " (" + Real.string() +
                       "): the run is refused, nothing was written. Remove the link or use another export folder.";
        }
        return {};
    };
    std::vector<fs::path> Roots = { fs::path(RunRoot) };
    if (P.Which == Pb::Target::Folder && !P.OutputRoot.empty()) Roots.push_back(P.OutputRoot);
    for (const fs::path& Root : Roots)
    {
        if (!fs::exists(Root, Ec) && !IsReparse(Root)) continue;
        if (const std::string Bad = Judge(Root); !Bad.empty()) return Bad;
        // directories only; a link is judged and not entered
        for (fs::recursive_directory_iterator It(Root, fs::directory_options::skip_permission_denied, Ec), End; !Ec && It != End; It.increment(Ec))
        {
            std::error_code Ec2;
            if (!It->is_directory(Ec2)) continue;
            if (IsReparse(It->path()))
            {
                if (const std::string Bad = Judge(It->path()); !Bad.empty()) return Bad;
                It.disable_recursion_pending();
            }
        }
    }
    return {};
}

std::string Begin(const Pb::Options& P, const std::string& AskedOut, const std::string& RunRoot)
{
    // with a target set, an export never lands inside a Mod Tools install (HydraX guarded --map-name and zpkg only);
    // with none the run is what it always was
    fs::path Install;
    if (P.Which != Pb::Target::None && Bo3Target::Output::Touches(AskedOut, &Install))
        return "--out lies in (or cannot be told from) the Mod Tools install " + Install.string() + "; use a folder outside it (--target modtools writes there on purpose)";
    // the folder the run writes is <out>/<game>: where it really is (junctions and symlinks followed), not the name it was given
    if (P.Which != Pb::Target::None && Bo3Target::Output::Touches(RunRoot, &Install))
        return "the export folder " + RunRoot + " lies in (or cannot be told from) the Mod Tools install " + Install.string() + " (a junction or symlink may lead there); use a folder outside it";
    if (const std::string Refused = Pb::Preflight(P); !Refused.empty()) return Refused;
    // As soon as the run folder is known and before anything can fail: the reports an earlier run left at this run's paths go, so a refused,
    // failed or crashed rerun never leaves an old result. Only the five report files are removed; with --target none (no report is written
    // or read) nothing is touched; a --plan rewrites the reports anyway, and its export and sidecars are not among them.
    if (P.Which != Pb::Target::None)
        if (const std::string Stuck = Pb::ClearRunReports(RunRoot); !Stuck.empty()) return Stuck;
    return {};
}

void Force(const Pb::Options& P, const Request& R, ExportOptions& O, Context& C)
{
    if (P.Which == Pb::Target::None) return;
    // the target needs the material GDT's model files (assets named as the GDT names them), TIFF pictures
    // (stock's format) and XMODEL_BIN (what an xmodel entry references), unless the caller chose others
    O.Settings["material_gdt"] = "on";
    // the maps' Radiant source is what the conversion publishes of a map (map_source), so it goes with the target
    if (C.Kinds.count(Kind::Map)) O.Settings["map_radiant"] = "on";
    if (R.FullMap) O.Settings["full_map"] = "on";
    C.ConvertImages = true;
    Materials::KeepConstantsInSidecar(true);    // the BO3 techset step reads colorTint and glossRange from the kept constant block
    if (!R.ImageFormatChosen) SettingsManager::SetRunSetting("exportimg", "TIFF");
    if (!R.ModelFormatChosen) SettingsManager::SetRunSetting("export_xmbin", "true");
}

void Unforce()
{
    Materials::KeepConstantsInSidecar(false);
    SettingsManager::ClearRunSetting("exportimg");
    SettingsManager::ClearRunSetting("export_xmbin");
}

namespace
{
    // The first drive among the paths with less free space than the floor, as a sentence; empty when none (or the floor is off).
    std::string DriveUnderFloor(const std::vector<std::pair<fs::path, const char*>>& Places, double FloorGb)
    {
        for (const auto& [Path, What] : Places)
        {
            uint64_t Free = 0;
            if (FloorGb <= 0.0 || !DiskGuard::Available(Path, Free) || !DiskGuard::IsBelowFloor(Free, FloorGb)) continue;
            std::string Drive = Path.root_name().string();
            if (Drive.empty()) Drive = Path.root_path().string();
            return std::format("The drive {} has only {:.1f} GB free, under the {:.0f} GB that stays free during a Black Ops III conversion (--min-free-gb). It holds {}. Free some space there or choose another folder.",
                               Drive, (double)Free / 1073741824.0, FloorGb, What);
        }
        return {};
    }
}

Prepared Prepare(const IDiskGame& Game, const Decision& Asked, ExportOptions& O, Context& C)
{
    Prepared Run;
    Decision D = Asked;
    Run.SavedFloor = DiskGuard::FloorGb();
    const auto Refuse = [&](const std::string& Why) {
        Run.Error = Why;
        Run.CleanupError = RemoveStaging(Run.Staging);
        Run.Staging.clear();
        return Run;
    };
    // links already under the folder the run writes, into an install: the run is refused before anything else is decided or written
    if (!D.Automatic && D.Staging.empty() && !D.OutFolder.empty())
        if (const std::string Linked = CheckRunFolder((fs::path(D.OutFolder) / Game.Id()).string(), O.Install, Pb::Options{}); !Linked.empty()) return Refuse(Linked);
    // the folder the run writes is <out>/<game>: judged by where it really is (junctions and symlinks followed). One that resolves into
    // a Mod Tools install makes the run the Mod Tools target with a staging export, never a raw export into the install.
    if (!D.Automatic && !D.OutFolder.empty())
    {
        fs::path Found;
        if (Bo3Target::Output::Touches(fs::path(D.OutFolder) / Game.Id(), &Found))
        {
            if (Found.empty()) return Refuse("GrayWolf cannot tell where the export folder " + D.OutFolder + " leads, and it may lie in the Black Ops III Mod Tools");
            D.Automatic = true;
            D.R.Target = "modtools";
            D.R.ModToolsRoot = Found.string();
            const auto Full = O.Settings.find("full_map");
            D.R.FullMap = Full != O.Settings.end() && Full->second == "on";
            D.OutFolder = D.Staging = NewStagingFolder();
        }
    }
    C.OutRoot = (fs::path(D.OutFolder) / Game.Id()).string();
    Run.Converting = !D.R.Target.empty();
    Run.Automatic = D.Automatic;
    Run.OutFolder = D.OutFolder;
    if (!D.Staging.empty())
    {
        // made and marked here, or removed here by its exact path: Refuse (which needs the marker) is not used for it
        if (const std::string Why = MakeStaging(D.Staging); !Why.empty())
        {
            Run.Error = Why;
            return Run;
        }
        Run.Staging = D.Staging;
    }
    if (Run.Converting)
    {
        if (!ReadPublish(D.R, C.OutRoot, Run.Publish, Run.Error)) return Refuse(Run.Error);
        if (Run.Error = Begin(Run.Publish, D.OutFolder, C.OutRoot); !Run.Error.empty()) return Refuse(Run.Error);
    }
    // links already under the folders the run writes: into an install, never (the staging folder is new, so this is for a run with no target)
    if (const std::string Linked = CheckRunFolder(C.OutRoot, O.Install, Run.Publish); !Linked.empty()) return Refuse(Linked);
    Force(Run.Publish, D.R, O, C);
    // the panel keeps full_map as a setting: without a target it must not start a full-map write
    if (!Run.Converting) O.Settings.erase("full_map");
    if (Run.Converting)
    {
        // the free space floor of the command line (8 GB unless set): a converted map writes many GB. Both drives are checked: the one the
        // export is written to (staging, when the output folder is in a Mod Tools install) and the one of the target
        if (DiskGuard::FloorGb() < 0.0) DiskGuard::FloorGb() = 8.0;
        const std::string Short = DriveUnderFloor({ { fs::path(C.OutRoot), Run.Staging.empty() ? "the export" : "the staging export" },
                                                    { Run.Publish.OutputRoot, Run.Publish.Which == Pb::Target::ModTools ? "the Mod Tools folder" : "the BO3 folder" } },
                                                  DiskGuard::FloorGb());
        if (!Short.empty())
        {
            Unforce();
            DiskGuard::FloorGb() = Run.SavedFloor;
            return Refuse(Short);
        }
        DiskGuard::ReportDir() = C.OutRoot;
        DiskGuard::Reset();
    }
    return Run;
}

std::string Abandon(Prepared& Run)
{
    Unforce();
    DiskGuard::FloorGb() = Run.SavedFloor;
    const std::string Why = RemoveStaging(Run.Staging);
    Run.Staging.clear();
    return Why;
}

namespace
{
    // The plan's placement report in words: what a write into the root would create, merge, replace or back up.
    PlanSummary Summarise(const Pb::Result& Planned, const fs::path& RunRoot, const Pb::Options& P)
    {
        PlanSummary S;
        S.Root = P.OutputRoot;
        S.GdtDbChecked = P.CheckGdtDb;
        const auto Count = [&](const char* Key) {
            const auto It = Planned.Counters.find(Key);
            return It == Planned.Counters.end() ? size_t(0) : It->second;
        };
        S.Unchanged = Count("publish_unchanged");
        S.InModTools = Count("publish_already_in_mod_tools");
        S.NameConflicts = Count("publish_name_conflict");
        S.ImageClashes = Count("publish_gdtdb_image_file_conflicts");
        S.Conflicts = Count("publish_placement_conflict") + Count("publish_map_conflicts");
        std::error_code Ec;
        std::set<std::string> Seen;
        bool First = true;
        std::istringstream Rows(Slurp(RunRoot / "graywolf_placement.tsv"));
        for (std::string Line; std::getline(Rows, Line);)
        {
            if (First) { First = false; continue; }   // the header
            std::vector<std::string> F = Split(Line, '\t');
            F.resize(6);
            if (F[2] != "would_write") continue;
            std::vector<std::string> Files = Split(F[5], ';');
            if (F[0] == "gdt") Files = { F[1] };
            for (const std::string& Rel : Files)
            {
                if (Rel.empty() || !Seen.insert(Rel).second) continue;
                const fs::path Where = P.OutputRoot / fs::path(Rel);
                if (!fs::is_regular_file(Where, Ec)) { S.FilesCreate++; continue; }
                if (F[0] == "gdt")
                {
                    S.FilesMerge++;
                    std::vector<Bo3Target::Output::GdtEntryText> Entries;
                    std::string Why;
                    if (!Bo3Target::Output::ParseGdt(Slurp(Where), Entries, Why)) S.GdtBackups++;   // an old GDT that does not read is copied to a .bak
                }
                else
                    S.FilesReplace++;
            }
        }
        std::string T = "Mod Tools folder: " + S.Root.string() + "\n";
        T += std::format("Files to create: {}\nFiles to replace (GrayWolf's own, unchanged since it wrote them): {}\nGDT files to merge into: {}\n", S.FilesCreate, S.FilesReplace, S.FilesMerge);
        T += std::format("GDT backups (.bak, an old GDT that does not read): {}\n", S.GdtBackups);
        if (S.GdtDbChecked)
            T += std::format("gdt.db check: {} assets are already in the Mod Tools (kept, not written), {} names clash with another game's, {} image files clash\n", S.InModTools, S.NameConflicts, S.ImageClashes);
        else
            T += "gdt.db check: off, nothing is skipped for being in the Mod Tools\n";
        if (S.Conflicts) T += std::format("Files that are not GrayWolf's own and stay untouched: {}\n", S.Conflicts);
        S.Text = T;
        return S;
    }
}

Outcome Finish(const IDiskGame& Game, const Pb::Options& Publish, const std::string& RunRoot, Totals& Into, const Hooks& H)
{
    Outcome Out;
    Pb::Loaded Mod;
    std::vector<std::string> Warnings;
    Mod.Open(Publish.ModToolsRoot, Publish.CheckGdtDb, Warnings);
    // a write into a Mod Tools root: the plan first (it writes only the run's reports), and the question on it
    if (Publish.Which == Pb::Target::ModTools && !Publish.Plan && H.Confirm)
    {
        Pb::Options Plan = Publish;
        Plan.Plan = true;
        const Pb::Result Planned = Pb::Run(Plan, Mod.View(), RunRoot, Game.Id());
        if (!Planned.Error.empty())
        {
            Out.Refused = Planned.Error;
            return Out;
        }
        const PlanSummary S = Summarise(Planned, RunRoot, Publish);
        Out.PlanText = S.Text;
        if (!H.Confirm(S))
        {
            Out.Declined = true;
            Out.Lines = "publish_target=modtools\npublish_declined=1\npublish_output=" + Publish.OutputRoot.string() + "\n";
            return Out;
        }
    }
    Pb::Result Done = Pb::Run(Publish, Mod.View(), RunRoot, Game.Id());
    for (const auto& [Name, N] : Done.Counters) Into.Counters[Name] += N;
    Warnings.insert(Warnings.end(), Done.Warnings.begin(), Done.Warnings.end());
    Out.OutputRoot = Publish.OutputRoot.string();
    Out.Lines = std::string("publish_target=") + (Publish.Which == Pb::Target::Folder ? "folder" : "modtools") + "\npublish_plan=" +
                (Publish.Plan ? "1" : "0") + "\npublish_output=" + Publish.OutputRoot.string() + "\npublish_mod_tools_root=" +
                Publish.ModToolsRoot.string() + "\n";
    for (const auto& W : Warnings) Out.Lines += "publish_warning " + W + "\n";
    Out.Refused = Done.Error;
    if (const auto It = Done.Counters.find("publish_gdt_backups"); It != Done.Counters.end()) Out.Backups = It->second;
    if (const auto It = Done.Counters.find("publish_files_written"); It != Done.Counters.end()) Out.FilesWritten = It->second;

    // the static gate (bo3-verify) over what was just written: read-only, and its report goes beside the run's other reports
    // (graywolf_gate.tsv in the run folder). It changes no exported file. A plan wrote nothing to the target, so it has nothing to check.
    if (Out.Refused.empty() && !Publish.Plan)
    {
        namespace Vf = Bo3Target::Verify;
        Vf::Inputs In;
        In.Roots = { Publish.OutputRoot };
        In.ModToolsRoot = Publish.ModToolsRoot;
        In.Catalog = &Bo3Target::Techsets::Shared();
        In.Held = Mod.View().Held;
        In.Stock = Mod.View().Stock;
        In.Approximations = fs::path(RunRoot) / "graywolf_approximations.tsv";
        std::error_code GateEc;
        In.MapSources = fs::is_directory(Publish.OutputRoot / "map_source", GateEc) || Publish.FullMap;
        // --full-map says which materials the maps draw on their own geometry: the gate fails one of them left <not in editor>
        if (Publish.FullMap)
        {
            In.MapDrawn = Done.MapMaterials;
            In.MapDrawnKnown = Done.MapDataComplete;
            In.MapDataWhy = Done.MapDataWhy;
            In.MapHeld = Done.MapHeld;
        }
        bool RunGate = true;
        std::string NotRunWhy;
        if (Publish.Which == Pb::Target::ModTools)
        {
            // the Mod Tools root holds far more than this run wrote: only the GDTs GrayWolf's manifest lists. A manifest that does not
            // load, or lists none, means the gate is not run: the root is never scanned instead.
            Bo3Target::Output::Manifest Listed;
            const bool Loaded = Listed.Load(Publish.OutputRoot / "graywolf" / "manifest.json").Ok;
            RunGate = Vf::ModToolsGdtFiles(Listed, Loaded, Publish.OutputRoot, In.OnlyFiles, NotRunWhy);
            In.ListedOnly = true;
        }
        if (!RunGate)
        {
            Out.Lines += "gate_mode=not run (" + NotRunWhy + ")\n";
            Into.Counters["gate_not_run"] += 1;
        }
        else
        {
            // no result of an earlier run survives a crash in the gate; one that cannot be removed means the gate is not run
            if (const std::string Stuck = Pb::ClearGateReports(RunRoot); !Stuck.empty())
            {
                Out.Lines += "gate_mode=not run (" + Stuck + ")" + char(10);
                Into.Counters["gate_not_run"] += 1;
                return Out;
            }
            const Vf::Result Gate = Vf::Run(In);
            std::string GateError;
            if (Vf::WriteReports(Gate, RunRoot, GateError))
            {
                Out.GateReport = (fs::path(RunRoot) / "graywolf_gate.tsv").string();
                Out.Lines += "gate_report=" + Out.GateReport + "\n";
            }
            else
                Out.Lines += "gate_warning cannot write the gate report: " + GateError + "\n";
            Out.Lines += "gate_mode=static (not proof BO3 accepts the assets)\ngate_result=" + std::string(Gate.Word()) + "\ngate_exit=" + std::to_string(Gate.ExitCode()) + "\ngate_summary=" + Gate.Summary() + "\n";
            Into.Counters["gate_failures"] += Gate.Failures;
            Into.Counters["gate_warnings"] += Gate.Warnings;
            Out.GateRun = true;
            Out.GateWord = Gate.Word();
            Out.GateSummary = Gate.Summary();
            Out.GateFailures = (int)Gate.Failures;
            Out.GateWarnings = (int)Gate.Warnings;
            Out.GateExit = Gate.ExitCode();
        }
    }
    return Out;
}

Completed Complete(const IDiskGame& Game, Prepared& Run, const Context& C, Totals& T, const Hooks& H)
{
    Completed Done;
    // a GUI-shaped run always asks before a write into a Mod Tools root: no hook is a programming error, and nothing is written
    if (Run.Converting && !T.Cancelled && Run.Publish.Which == Pb::Target::ModTools && !H.Confirm)
        Done.Out.Refused = "internal: a write into a Mod Tools root needs the confirmation hook; nothing was written";
    else if (Run.Converting && !T.Cancelled)
        Done.Out = Finish(Game, Run.Publish, C.OutRoot, T, H);
    Done.FloorReached = Run.Converting && DiskGuard::WriteReport(fs::path(C.OutRoot));
    // an automatic run's reports are in the staging folder that goes now: the gate report is kept in a folder of its own to open
    if (!Done.Out.GateReport.empty() && !Run.Staging.empty())
    {
        std::error_code Ec;
        const fs::path Keep = StagingParent({}) / "GrayWolf-bo3-last-check";
        fs::create_directories(Keep, Ec);
        for (const char* Name : { "graywolf_gate.tsv", "graywolf_gate_summary.txt", "graywolf_placement.tsv", "graywolf_approximations.tsv" })
            fs::copy_file(fs::path(C.OutRoot) / Name, Keep / Name, fs::copy_options::overwrite_existing, Ec);
        Done.Out.GateReport = (Keep / "graywolf_gate.tsv").string();
    }
    Done.CleanupError = Abandon(Run);
    return Done;
}

int SelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Condition, const char* Name, const std::string& Got) {
        if (Condition) return;
        Failed++;
        Failures += std::string("FAIL bo3-run: ") + Name + (Got.empty() ? "" : " (" + Got + ")") + "\n";
    };
    std::error_code Ec;
    // a directory of this run alone: made new (a name taken is a failure, never a folder to clear), and the only one removed at the end
    const fs::path Dir = fs::temp_directory_path(Ec) / ("graywolf-bo3run-selftest-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!fs::create_directory(Dir, Ec))
    {
        Failures += "FAIL bo3-run: the test folder " + Dir.string() + " could not be made new\n";
        return 1;
    }
    const fs::path Fake = Dir / "fake_root", Plain = Dir / "elsewhere", Staging = Dir / "staging";
    fs::create_directories(Fake / "bin", Ec);
    fs::create_directories(Plain, Ec);
    { std::ofstream(Fake / "bin" / "linker_modtools.exe") << ""; }   // the marker of an install, empty

    const std::map<std::string, std::string> Off = {}, Folder = { { "bo3_convert", "folder" }, { "full_map", "on" } },
                                             ModTools = { { "bo3_convert", "modtools" }, { "full_map", "on" } }, Odd = { { "bo3_convert", "yes please" } };
    // off, and a value that is not one of the choices, are off
    {
        const Decision A = FromSettings(Off, Plain.string(), Fake.string(), Staging.string());
        const Decision B = FromSettings(Odd, Plain.string(), Fake.string(), Staging.string());
        Check(A.R.Target.empty() && !A.Automatic && A.Why.empty() && A.OutFolder == Plain.string(), "off is no target", A.R.Target);
        Check(B.R.Target.empty() && !B.Automatic, "an unknown value reads as off", B.R.Target);
        Pb::Options P;
        std::string Why;
        Check(ReadPublish(A.R, Plain.string(), P, Why) && P.Which == Pb::Target::None, "off reads as no target", Why);
    }
    // the folder choice: a folder target, the full map as asked, the root a read-only lookup
    {
        const Decision D = FromSettings(Folder, Plain.string(), Fake.string(), Staging.string());
        Check(D.R.Target == "folder" && D.R.FullMap && !D.Automatic && D.OutFolder == Staging.string() && D.Staging == Staging.string() && D.R.FolderOutputBase == Plain.string(),
              "the folder choice stages its raw export and ends in the export folder", D.OutFolder);
        Pb::Options PF;
        std::string WhyF;
        Check(ReadPublish(D.R, (Staging / "mwr").string(), PF, WhyF) && PF.OutputRoot == Plain / "mwr" / "bo3", "the folder target's output is in the export folder, not the staging folder", PF.OutputRoot.string() + WhyF);
        Check(D.R.ImageFormatChosen && !D.R.ModelFormatChosen, "the GUI keeps its picture format and forces XMODEL_BIN", "");
    }
    // the Mod Tools choice: needs the root the Settings page finds
    {
        const Decision D = FromSettings(ModTools, Plain.string(), Fake.string(), Staging.string());
        Check(D.R.Target == "modtools" && D.R.ModToolsRoot == Fake.string() && D.Why.empty() && D.OutFolder == Staging.string() && D.Staging == Staging.string(), "the Mod Tools choice stages its raw export and writes to the found root", D.Why);
        const Decision None = FromSettings(ModTools, Plain.string(), "", Staging.string());
        Check(!None.Why.empty(), "the Mod Tools choice without a root is refused", None.Why);
        const Decision NotRoot = FromSettings(ModTools, Plain.string(), Plain.string(), Staging.string());
        Check(!NotRoot.Why.empty(), "the Mod Tools choice on a folder that is no install is refused", NotRoot.Why);
    }
    // the output folder in an install: the Mod Tools target on it whatever the choice, the staging export moved out
    for (const auto* Choice : { &Off, &Folder, &ModTools, &Odd })
    {
        const Decision D = FromSettings(*Choice, (Fake / "exported_files").string(), "", Staging.string());
        Check(D.Automatic && D.R.Target == "modtools" && fs::weakly_canonical(D.R.ModToolsRoot, Ec) == fs::weakly_canonical(Fake, Ec), "an output in an install is the Mod Tools target", D.R.Target + " " + D.R.ModToolsRoot);
        Check(D.OutFolder == Staging.string() && !Bo3Target::Output::Touches(D.OutFolder), "the staging export is outside the install", D.OutFolder);
    }
    // Begin: a folder target with an output in the install is refused, with no target nothing is checked
    {
        Pb::Options P;
        P.Which = Pb::Target::Folder;
        P.OutputRoot = Fake / "bo3";
        Check(!Begin(P, (Fake / "x").string(), (Fake / "x" / "mwr").string()).empty(), "a target with an output in an install is refused", "");
        const Pb::Options None;
        Check(Begin(None, (Fake / "x").string(), (Fake / "x" / "mwr").string()).empty(), "no target: nothing is checked", "");
    }
    // Force: no target takes the conversion's keys out; a target sets what it needs and Unforce restores the run settings
    {
        ExportOptions O;
        Context C;
        C.Kinds = { Kind::Map };
        O.Settings["full_map"] = "on";
        Force(Pb::Options{}, Request{}, O, C);
        Check(O.Settings.count("full_map") == 1 && O.Settings["full_map"] == "on" && !C.ConvertImages && O.Settings.count("map_radiant") == 0, "no target: nothing is forced and a saved full_map is not erased", "");
        Pb::Options P;
        P.Which = Pb::Target::Folder;
        Request R;
        R.FullMap = true;
        Force(P, R, O, C);
        Check(O.Settings["material_gdt"] == "on" && O.Settings["full_map"] == "on" && C.ConvertImages, "a target forces the material GDT and the full map", "");
        Check(O.Settings["map_radiant"] == "on", "a target with maps forces the Radiant map", "");
        ExportOptions Models;
        Context ModelsOnly;
        ModelsOnly.Kinds = { Kind::Model };
        Force(P, Request{}, Models, ModelsOnly);
        Check(Models.Settings.count("map_radiant") == 0, "a target without maps does not force the Radiant map", "");
        Check(SettingsManager::GetSetting("exportimg", "") == "TIFF" && SettingsManager::GetSetting("export_xmbin", "") == "true", "a target forces TIFF and XMODEL_BIN", "");
        Unforce();
        Check(SettingsManager::GetSetting("export_xmbin", "") != "true", "Unforce gives the run settings back", "");
    }
    // staging folders: removed only by their own name and marker; a stale one (its process gone) at start, never a live one or a lookalike
    {
        const fs::path Parent = Dir / "tmp";
        fs::create_directories(Parent, Ec);
        const auto Make = [&](const char* Name, const char* Pid, bool Marker) {
            fs::create_directories(Parent / Name / "mwr", Ec);
            { std::ofstream(Parent / Name / "mwr" / "x.txt") << "x"; }
            if (Marker) { std::ofstream(Parent / Name / StagingMarker) << Pid; }
        };
        const std::string Mine = std::to_string(GetCurrentProcessId());
        Make("GrayWolf-bo3-staging-dead", "4294967", true);       // a run that ended without cleaning up
        Make("GrayWolf-bo3-staging-live", Mine.c_str(), true);    // a run under way in this process
        Make("GrayWolf-bo3-staging-nomarker", "4294967", false);  // a lookalike without the marker
        Make("other-folder", "4294967", true);                    // another name
        std::vector<std::string> Failures2;
        const size_t N = RemoveStaleStaging(Parent.string(), &Failures2);
        Check(N == 1 && Failures2.empty() && !fs::exists(Parent / "GrayWolf-bo3-staging-dead", Ec), "a stale staging folder is removed at start", std::to_string(N));
        Check(fs::exists(Parent / "GrayWolf-bo3-staging-live", Ec), "a staging folder of a live process stays", "");
        Check(fs::exists(Parent / "GrayWolf-bo3-staging-nomarker", Ec) && fs::exists(Parent / "other-folder", Ec), "a lookalike without the marker and another folder stay", "");
        Check(!RemoveStaging((Parent / "other-folder").string()).empty() && fs::exists(Parent / "other-folder", Ec), "RemoveStaging refuses a folder that is not a staging folder", "");
        Check(RemoveStaging((Parent / "GrayWolf-bo3-staging-live").string()).empty() && !fs::exists(Parent / "GrayWolf-bo3-staging-live", Ec), "RemoveStaging removes this run's folder", "");
        Check(RemoveStaging((Parent / "GrayWolf-bo3-staging-gone").string()).empty(), "a staging folder already gone is no error", "");
        const std::string One = NewStagingFolder(Parent.string());
        Check(One.find(StagingPrefix) != std::string::npos && fs::path(One).parent_path() == Parent, "a staging folder is named for its run, in its parent", One);
    }
    // Question: a cancel while it waits answers No at once (the window closing must not hang on it); an answer is returned
    {
        Question Q;
        std::atomic<bool> Cancel{ false };
        auto Waiting = std::async(std::launch::async, [&] { return Q.Ask("plan", Cancel); });
        std::string Text;
        for (int K = 0; K < 100 && !Q.Pending(Text); K++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check(Text == "plan", "a question is pending for the UI", Text);
        Cancel.store(true);
        Q.Wake();
        const bool Returned = Waiting.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        Check(Returned && !Waiting.get() && !Q.Pending(Text), "a cancel answers No and ends the wait", "");
        Cancel.store(false);
        auto Yes = std::async(std::launch::async, [&] { return Q.Ask("again", Cancel); });
        for (int K = 0; K < 100 && !Q.Pending(Text); K++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Q.Answer(true);
        Check(Yes.wait_for(std::chrono::seconds(5)) == std::future_status::ready && Yes.get(), "Yes is returned", "");
    }
    // a junction: the real place counts. Made with mklink /J inside this test's own folder, removed with RemoveDirectoryW (never a recursive delete)
    {
        const fs::path Junction = Dir / "junction_to_install";
        const std::string Cmd = "cmd /c mklink /J \"" + Junction.string() + "\" \"" + Fake.string() + "\" > nul 2>&1";
        const bool Made = std::system(Cmd.c_str()) == 0 && fs::exists(Junction / "bin", Ec);
        Check(Made, "a junction could be made for the test", Cmd);
        if (Made)
        {
            fs::path Found;
            Check(Bo3Target::Output::Touches(Junction / "exported_files", &Found) && fs::weakly_canonical(Found, Ec) == fs::weakly_canonical(Fake, Ec), "Touches follows a junction into an install", Found.string());
            Check(Bo3Target::Output::IsUnder(Fake, Junction / "x" / "y"), "IsUnder follows a junction", "");
            const Decision D = FromSettings(Off, (Junction / "out").string(), "", Staging.string());
            Check(D.Automatic && D.R.Target == "modtools", "an output folder that is a junction into an install is automatic", D.R.Target);
            // <out>/<game> a junction into the install, out itself plain
            fs::create_directories(Plain / "viajunction", Ec);
            const fs::path Game = Plain / "viajunction" / "mwr";
            const std::string Cmd2 = "cmd /c mklink /J \"" + Game.string() + "\" \"" + Fake.string() + "\" > nul 2>&1";
            if (std::system(Cmd2.c_str()) == 0)
            {
                Pb::Options P;
                P.Which = Pb::Target::Folder;
                P.OutputRoot = Game / "bo3";
                Check(!Begin(P, (Plain / "viajunction").string(), Game.string()).empty(), "Begin refuses a run folder that is a junction into an install", "");
                Check(Bo3Target::Output::Touches(Game, &Found), "Touches sees <out>/<game> leading into an install", "");
                RemoveDirectoryW(Game.c_str());
            }
            // a staging folder name that is a junction: never removed through
            const fs::path Parent = Dir / "tmp2";
            fs::create_directories(Parent / "victim" / "keep", Ec);
            { std::ofstream(Parent / "victim" / "keep" / "x.txt") << "x"; }
            { std::ofstream(Parent / "victim" / StagingMarker) << "4294967"; }
            const fs::path Link = Parent / "GrayWolf-bo3-staging-link";
            const std::string Cmd3 = "cmd /c mklink /J \"" + Link.string() + "\" \"" + (Parent / "victim").string() + "\" > nul 2>&1";
            if (std::system(Cmd3.c_str()) == 0)
            {
                std::vector<std::string> Failed;
                const size_t N = RemoveStaleStaging(Parent.string(), &Failed);
                Check(N == 0 && fs::exists(Parent / "victim" / "keep" / "x.txt", Ec), "a staging name that is a junction is not removed through", std::to_string(N));
                Check(!RemoveStaging(Link.string()).empty() && fs::exists(Parent / "victim" / "keep" / "x.txt", Ec), "RemoveStaging refuses a junction", "");
                RemoveDirectoryW(Link.c_str());
            }
            RemoveDirectoryW(Junction.c_str());
            Check(fs::exists(Fake / "bin" / "linker_modtools.exe", Ec), "the junction's target is intact after its removal", "");
        }
    }
    // a marker that is empty, zero or unreadable is a live run, never stale
    {
        const fs::path Parent = Dir / "tmp3";
        const auto Make = [&](const char* Name, const char* Text) {
            fs::create_directories(Parent / Name, Ec);
            { std::ofstream(Parent / Name / StagingMarker, std::ios::binary) << Text; }
        };
        Make("GrayWolf-bo3-staging-empty", "");
        Make("GrayWolf-bo3-staging-zero", "0");
        Make("GrayWolf-bo3-staging-text", "not a number");
        Make("GrayWolf-bo3-staging-gone", "4294967");
        std::vector<std::string> Failed;
        const size_t N = RemoveStaleStaging(Parent.string(), &Failed);
        Check(N == 1 && !fs::exists(Parent / "GrayWolf-bo3-staging-gone", Ec) && fs::exists(Parent / "GrayWolf-bo3-staging-empty", Ec) &&
              fs::exists(Parent / "GrayWolf-bo3-staging-zero", Ec) && fs::exists(Parent / "GrayWolf-bo3-staging-text", Ec),
              "an empty, zero or unreadable marker is live; a readable id of a gone process is stale", std::to_string(N));
        Check(ProcessIsGone(4294967) && !ProcessIsGone(GetCurrentProcessId()), "a process that does not exist is gone, this one is not", "");
        Check(!ProcessIsGone(4), "a process that cannot be opened (System, access denied) is not gone", "");
    }
    // a junction nested under the run folder (<out>/<game>/images) into an install: refused before anything is written, the link named
    {
        const fs::path Run = Dir / "nested_run" / "mwr";
        fs::create_directories(Run / "maps", Ec);
        fs::create_directories(Dir / "game_install" / "zone", Ec);
        const Pb::Options None;
        Check(CheckRunFolder(Run.string(), "", None).empty(), "a run folder with no link passes", CheckRunFolder(Run.string(), "", None));
        const fs::path Link = Run / "images";
        const std::string Cmd = "cmd /c mklink /J \"" + Link.string() + "\" \"" + Fake.string() + "\" > nul 2>&1";
        if (std::system(Cmd.c_str()) == 0)
        {
            const std::string Why = CheckRunFolder(Run.string(), "", None);
            Check(!Why.empty() && Why.find("images") != std::string::npos && Why.find("Mod Tools") != std::string::npos, "a nested junction into an install refuses the run and names the link", Why);
            RemoveDirectoryW(Link.c_str());
            Check(CheckRunFolder(Run.string(), "", None).empty(), "the run passes again once the link is gone", "");
        }
        else
            Check(false, "a nested junction could be made for the test", Cmd);
        const fs::path Link2 = Run / "models";
        const std::string Cmd2 = "cmd /c mklink /J \"" + Link2.string() + "\" \"" + (Dir / "game_install" / "zone").string() + "\" > nul 2>&1";
        if (std::system(Cmd2.c_str()) == 0)
        {
            const std::string Why = CheckRunFolder(Run.string(), (Dir / "game_install").string(), None);
            Check(!Why.empty() && Why.find("models") != std::string::npos && Why.find("game install") != std::string::npos, "a nested junction into the game install refuses the run", Why);
            Check(CheckRunFolder(Run.string(), "", None).empty(), "a link to a plain folder is fine when the folder is no install", "");
            RemoveDirectoryW(Link2.c_str());
        }
        // files are not scanned: a file that is not a directory is skipped, and the folder target's own folder is scanned too
        { std::ofstream(Run / "maps" / "a.txt") << "x"; }
        Pb::Options Folder2;
        Folder2.Which = Pb::Target::Folder;
        Folder2.OutputRoot = Dir / "nested_out" / "bo3";
        fs::create_directories(Folder2.OutputRoot / "texture_assets", Ec);
        const fs::path Link3 = Folder2.OutputRoot / "texture_assets" / "sneaky";
        const std::string Cmd3 = "cmd /c mklink /J \"" + Link3.string() + "\" \"" + Fake.string() + "\" > nul 2>&1";
        if (std::system(Cmd3.c_str()) == 0)
        {
            Check(!CheckRunFolder(Run.string(), "", Folder2).empty(), "a link under the folder target's own folder refuses the run", "");
            RemoveDirectoryW(Link3.c_str());
        }
    }
    // a marker that cannot be written: the folder just made is removed by its exact path, its temporary file with it
    {
        const fs::path Parent = Dir / "tmp5";
        fs::create_directories(Parent, Ec);
        const std::string Mine = (Parent / "GrayWolf-bo3-staging-made").string();
        MarkerFailureForTest() = true;
        const std::string Why = MakeStaging(Mine);
        MarkerFailureForTest() = false;
        Check(!Why.empty() && !fs::exists(Mine, Ec), "a failed marker removes the folder just made", Why);
        const std::string Other = (Parent / "not-a-staging-name").string();
        MarkerFailureForTest() = true;
        const std::string Why2 = MakeStaging(Other);
        MarkerFailureForTest() = false;
        Check(!Why2.empty() && fs::exists(Other, Ec), "a folder without the staging name is not removed", Why2);
        Check(MakeStaging(Mine).empty() && fs::is_regular_file(fs::path(Mine) / StagingMarker, Ec) && !fs::exists(fs::path(Mine) / (std::string(StagingMarker) + ".tmp"), Ec),
              "a staging folder is made with its marker and no temporary file left", "");
        Check(!MakeStaging(Mine).empty(), "a staging folder that exists is not taken over", "");
        // a junction at the staging name is left alone by the failure path
        const fs::path Victim = Parent / "victim2";
        fs::create_directories(Victim, Ec);
        { std::ofstream(Victim / "keep.txt") << "x"; }
        const fs::path JLink = Parent / "GrayWolf-bo3-staging-jl";
        const std::string CmdJ = "cmd /c mklink /J \"" + JLink.string() + "\" \"" + Victim.string() + "\" > nul 2>&1";
        if (std::system(CmdJ.c_str()) == 0)
        {
            Check(!MakeStaging(JLink.string()).empty() && fs::exists(Victim / "keep.txt", Ec), "an existing junction is not taken for a new staging folder", "");
            RemoveDirectoryW(JLink.c_str());
        }
    }
    // raw zones: a fast file's first bytes, however short, are never a raw zone
    {
        const fs::path Zones = Dir / "zones";
        fs::create_directories(Zones, Ec);
        const auto Write = [&](const char* Name, const std::string& Bytes) {
            std::ofstream(Zones / Name, std::ios::binary) << Bytes;
            return (Zones / Name).string();
        };
        Check(!RawZoneRefusal(Write("a.zone", "IWffu100....")).empty(), "a file starting IWff is not a raw zone", "");
        Check(!RawZoneRefusal(Write("b.zone", "IWff")).empty(), "a fast file cut to its magic is not a raw zone", "");
        Check(!RawZoneRefusal(Write("c.zone", "TAf")).empty() && !RawZoneRefusal(Write("d.zone", "")).empty(), "a file shorter than 4 bytes is not a raw zone", "");
        Check(RawZoneRefusal(Write("e.zone", std::string("\x71\x9a\x00\x29rest of a zone", 18))).empty(), "a file with other first bytes may be a raw zone", RawZoneRefusal((Zones / "e.zone").string()));
    }
    fs::remove_all(Dir, Ec);
    return Failed;
}
}
