#pragma once

// A game's own extra subcommand (a diagnosis tool, say: Games/T8/ZoneDiag.cpp), `<name> <input>`,
// declared in its Games/<game>/ folder with a static GameCommandRegistration; Main.cpp builds the
// subcommands from this registry, so a game adds one without editing a shared file. Exports go
// through disk-export (DiskGame.h), not here.

#include <string>
#include <vector>

namespace CLI
{
    class App;
}
struct CliOptions;

struct GameCommand
{
    const char* Name;         // the subcommand, e.g. "bo4-zone-diag"
    const char* Help;
    const char* InputHelp;    // the one positional: a zone, a folder, a list...
    int (*Run)(const CliOptions& Options, const std::string& Input);
    // options of its own beyond the shared ones (--out, --type, --name-filter...), or nullptr
    void (*AddOptions)(CLI::App& Sub, CliOptions& Options) = nullptr;
    // the positional's name, when AddOptions has an option of the name "zones"
    const char* InputName = "zones";
};

const std::vector<const GameCommand*>& GameCommands();

struct GameCommandRegistration
{
    explicit GameCommandRegistration(const GameCommand& Command);
};
