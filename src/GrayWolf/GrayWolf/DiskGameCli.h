#pragma once

// The CLI adapter of DiskGame::Export (disk-export and the games' aliases): the command line as
// ExportOptions and a Context, the run, the report through CliReport.

#include "DiskGame.h"

struct CliOptions;

namespace DiskGame
{
    // The exit code.
    int Run(const IDiskGame& Game, const CliOptions& Options, const std::string& Input);
    // The command line's --install, --cache, --languages... as ExportOptions, as Run reads them. False with Error.
    bool ReadExportOptions(const IDiskGame& Game, const CliOptions& Cli, ExportOptions& Out, std::string& Error);
}
