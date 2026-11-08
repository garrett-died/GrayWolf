#include "stdafx.h"

#include "DiskGame.Bo3.h"

#include "CliOptions.h"
#include "CliSupport.h"
#include "GameCommand.h"
#include "Languages.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

// gui-export-check <zone|folder> --game <id> --out <dir> [--gui-setting key=value]... [--gui-mod-tools-root <dir>] [--gui-staging <dir>]
// [--gui-answer yes|no]
// The Load Game panel's export without the window: the settings map as the panel builds it (every setting of the game at its default,
// then the --gui-setting overrides, as DiskOptions would give them), the same Decision, Prepare, Export and Complete the panel runs, the
// confirmation answered by --gui-answer. It never looks for a Mod Tools root on its own: only --gui-mod-tools-root names one.
namespace DiskGame::Bo3
{
namespace
{
    // what the options of this command fill (CLI11 binds them by reference); one object, made on first use
    struct GuiArgs
    {
        std::vector<std::string> Settings;
        std::string ModToolsRoot, Staging, Answer = "yes";
    };
    GuiArgs& Args()
    {
        static GuiArgs A;
        return A;
    }

    int RunGuiExportCheck(const CliOptions& Options, const std::string& Input)
    {
        const auto Fail = [&](const std::string& Why) {
            CliReport(Options, "result=error\nreason=" + Why + "\n");
            return 1;
        };
        if (Options.Out.empty()) return Fail("gui-export-check needs --out");
        const IDiskGame* Game = Options.DiskGameId.empty() ? Detect(Input) : Find(Options.DiskGameId);
        if (Game == nullptr) return Fail("no disk reader for " + Input);
        ExportOptions O;
        O.Install = Options.Install;
        O.Cache = Options.Cache;
        for (const Setting* S : Game->AllSettings()) O.Settings[S->Key] = S->Default;
        for (const std::string& Item : Args().Settings)
        {
            const size_t Eq = Item.find('=');
            if (Eq == std::string::npos) return Fail("--gui-setting wants key=value, not " + Item);
            O.Settings[Item.substr(0, Eq)] = Item.substr(Eq + 1);
        }
        O.Languages = Languages::GuiDefault;
        const Decision D = FromSettings(O.Settings, fs::absolute(Options.Out).string(), Args().ModToolsRoot, NewStagingFolder(Args().Staging));
        if (!D.Why.empty()) return Fail(D.Why);
        Context C;
        std::string Error;
        if (!ParseKinds(Options.Type, C.Kinds, Error)) return Fail(Error);
        C.Limit = Options.Limit;
        std::vector<std::string> StaleFailed;
        const size_t Stale = RemoveStaleStaging(Args().Staging, &StaleFailed);
        Prepared Prep = Prepare(*Game, D, O, C);
        if (!Prep.Error.empty()) return Fail(Prep.Error + (Prep.CleanupError.empty() ? "" : " | " + Prep.CleanupError));
        std::vector<std::string> Zones;
        if (!Game->Zones(Input, O, Zones, Error))
        {
            std::string Why;
            Zones.clear();
            if (O.Install.empty() || !Game->ResolveZones(O.Install, { Input }, O, Zones, Why) || Zones.empty())
            {
                Abandon(Prep);
                return Fail(Error);
            }
        }
        Totals T = Export(*Game, O, Zones, C);
        Hooks Ask;
        size_t Asked = 0;
        std::string Shown;
        Ask.Confirm = [&](const PlanSummary& Plan) {
            Asked++;
            Shown = Plan.Text;
            return Args().Answer == "yes";
        };
        const std::string Staged = Prep.Staging;
        const bool WasAutomatic = Prep.Automatic;
        const Completed Done = Complete(*Game, Prep, C, T, Ask);
        std::string Lines = std::string("result=") + (Done.Out.Refused.empty() ? "ok" : "error") + "\ngame=" + Game->Id() +
                            "\ngui_stale_removed=" + std::to_string(Stale) + "\ngui_staging_left=" + (fs::exists(Staged.empty() ? fs::path("-") : fs::path(Staged)) ? "1" : "0") + "\ngui_cleanup_error=" + Done.CleanupError +
                            "\ngui_automatic=" + (WasAutomatic ? "1" : "0") + "\ngui_target=" + (Done.Out.OutputRoot.empty() ? "off" : "converted") +
                            "\ngui_out_root=" + C.OutRoot + "\ngui_asked=" + std::to_string(Asked) + "\ngui_declined=" + (Done.Out.Declined ? "1" : "0") +
                            "\ngui_backups=" + std::to_string(Done.Out.Backups) + "\ngui_files_written=" + std::to_string(Done.Out.FilesWritten) +
                            "\ngui_gate=" + (Done.Out.GateRun ? Done.Out.GateWord + " failures=" + std::to_string(Done.Out.GateFailures) : std::string("not run")) +
                            "\n" + Done.Out.Lines;
        if (!Done.Out.Refused.empty()) Lines += "reason=" + Done.Out.Refused + "\n";
        std::string Row;
        for (std::stringstream Ss(Shown); std::getline(Ss, Row);) Lines += "gui_plan " + Row + "\n";
        CliReport(Options, Lines + Report(T));
        return Done.Out.Refused.empty() ? 0 : 1;
    }

    void AddGuiExportCheckOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--game", Options.DiskGameId, "The game; detected from the zones when omitted");
        Sub.add_option("--gui-setting", Args().Settings, "A setting as the panel keeps it, key=value (bo3_convert=modtools, full_map=on, map_radiant=on...)");
        Sub.add_option("--gui-mod-tools-root", Args().ModToolsRoot, "The Mod Tools folder the Settings page would find (a test gives a fake one)");
        Sub.add_option("--gui-staging", Args().Staging, "The folder an automatic run makes its own staging folder in (default: the temporary folder)");
        Sub.add_option("--gui-answer", Args().Answer, "The answer to the confirmation of a write into a Mod Tools root: yes or no");
    }

    const GameCommand GuiExportCheckCommand{
        "gui-export-check",
        "The Load Game panel's export with the Black Ops III conversion, without the window (the panel's Decision, Prepare, Export and Complete)",
        "A zone, or a folder of zones",
        RunGuiExportCheck,
        AddGuiExportCheckOptions,
    };
    const GameCommandRegistration GuiExportCheckRegistered(GuiExportCheckCommand);
}
}
