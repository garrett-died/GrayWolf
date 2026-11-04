#include "stdafx.h"

// bo3-verify <out> [--mod-tools-root <root>] [--report-out <dir>]
// The static gate over a Mod Tools layout (Bo3Target.Verify.h). Read-only on <out> and the Mod Tools; writes only
// graywolf_gate.tsv and its summary line into --report-out (default: the current folder).
// Exit 0: pass, 1: warnings, 2: failures; 3: the gate could not run.

#include "Bo3Target.Publish.h"
#include "Bo3Target.Verify.h"
#include "CliOptions.h"
#include "CliSupport.h"
#include "GameCommand.h"
#include "GameInstalls.h"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    int RunVerify(const CliOptions& Options, const std::string& Input)
    {
        namespace Pb = Bo3Target::Publish;
        namespace Vf = Bo3Target::Verify;
        const auto Fail = [&](const std::string& Why) {
            CliReport(Options, "result=error\nreason=" + Why + "\n");
            return 3;
        };
        std::error_code Ec;

        // the Mod Tools root: read-only lookups only (deffiles, gdt.db)
        fs::path Root;
        std::string How, Scan, Ta;
        if (Options.ModToolsRoot.empty())
        {
            for (const auto& I : GameInstalls::Detect())
                if (I.DiskId == "bo3") { Scan = I.Path; break; }
            char* Env = nullptr;
            size_t Length = 0;
            if (_dupenv_s(&Env, &Length, "TA_TOOLS_PATH") == 0 && Env != nullptr)
            {
                Ta = Env;
                free(Env);   // NOLINT: _dupenv_s allocates with malloc
            }
        }
        const bool HaveRoot = Pb::ResolveRoot(Options.ModToolsRoot, Scan, Ta, Root, How);

        Vf::Inputs In;
        In.Roots = Vf::ResolveRoots(Input);
        if (HaveRoot) In.ModToolsRoot = Root;
        In.Catalog = &Bo3Target::Techsets::Shared();
        for (const fs::path& R : In.Roots)
            if (fs::is_directory(R / "map_source", Ec)) In.MapSources = true;
        Pb::Loaded Mod;
        std::vector<std::string> Warnings;
        if (HaveRoot) Mod.Open(Root, true, Warnings);
        In.Held = Mod.View().Held;

        const Vf::Result R = Vf::Run(In);
        std::string Error;
        const fs::path Dir = Options.ReportOut.empty() ? fs::current_path(Ec) : fs::path(Options.ReportOut);
        if (!Vf::WriteReports(R, Dir, Error)) return Fail("cannot write the reports to " + Dir.string() + ": " + Error);
        std::string Text = std::string("result=") + R.Word() + "\nmode=static\nroots=" + std::to_string(In.Roots.size()) + "\nmod_tools_root=" + (HaveRoot ? Root.string() + " (" + How + ")" : "none") +
                           "\ngdtdb=" + (In.Held ? "read" : "not read") + "\ngate_exit=" + std::to_string(R.ExitCode()) + "\ngate_report=" + (Dir / "graywolf_gate.tsv").string() + "\nsummary=" + R.Summary() + "\n";
        for (const auto& W : Warnings) Text += "warning " + W + "\n";
        CliReport(Options, Text);
        return R.ExitCode();
    }

    void AddOptions(CLI::App& Sub, CliOptions& Options)
    {
        Sub.add_option("--report-out", Options.ReportOut, "The folder graywolf_gate.tsv and graywolf_gate_summary.txt are written to (default: the current folder)");
    }

    const GameCommand VerifyCommand{
        "bo3-verify",
        "STATIC gate over a Black Ops III Mod Tools layout (what --target wrote): GDT syntax, .awi schema, materials, techsets, images. Read-only. Exit 0 pass, 1 warnings, 2 failures. Not proof BO3 accepts the assets",
        "The output folder: the layout root (.../bo3), or a folder holding <game>/bo3",
        RunVerify,
        AddOptions,
        "folder",
    };
    const GameCommandRegistration VerifyRegistered(VerifyCommand);
}
