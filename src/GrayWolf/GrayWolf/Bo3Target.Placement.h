#pragma once

// Module 3 of the Black Ops III conversion layer: decide where an asset belongs in the BO3 Mod Tools
// layout, for every game. See docs/hydrax-port/bo3-conversion-design.md (module 3),
// bo3-target-hydrax.md 1.1 to 1.4 and references-bo3.md 1.2 (rules P1 to P18).
//
// Pure: no file is read or written, nothing is global. The stock index (Bo3Target.GdtDb.h) is the only
// outside knowledge, and only its stock rows are used. Paths are root-relative with backslashes.
//
// Where this differs from HydraX's PlacementPolicy, on purpose:
//   - the stock-closest match never joins a stock model folder with a material or image file name
//     (58 HydraX files were misplaced that way); orphan image and material roots go to the texture library;
//   - Closest looks at stock rows only (the GdtDb stock rule: not in a manifest, no hydrax_/graywolf_ name);
//   - shelves are named graywolf, and a destination that is a stock gdt path becomes graywolf_<file>.gdt
//     in the same folder;
//   - a model on a shelf gets its own <name>.gdt (a shared shelf file kept 1 of 187 hash-named models).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Bo3Target.GdtDb.h"

namespace Bo3Target::Placement
{
    // ---- game tokens ---------------------------------------------------------------------------

    struct GameEntry
    {
        const char* GameId;     // GrayWolf's game id (DiskGame Id())
        const char* Token;      // the token in BO3 names and folders: t7, t8, t9, ...
        const char* Title;
    };

    // The one table. Every GrayWolf game id has a row (the -oat readers share their game's token).
    const std::vector<GameEntry>& GameTable();

    // The token of a game id (case-insensitive); empty when the id is not in the table.
    std::string GameToken(std::string_view GameId);

    // The game token a name carries, empty when it carries none: a leading p7_/p8_/p9_ of the stem, else
    // a t4..t11 or iw1..iw9 token among the first three '_' tokens. The stem is the name without folder,
    // i_ and mtl_ (HydraX NameGameOfStem, extended to the other games' tokens).
    std::string NameGameOfStem(std::string_view Stem);

    // ---- names ---------------------------------------------------------------------------------

    constexpr size_t MaxEntryNameLength = 63;

    // A name of at most 63 characters is unchanged; a longer one becomes its first 55 characters, '_' and
    // 7 hex digits of the FNV-1a hash of the lower-cased full name (MaterialEntryNames.Shorten). The hash is
    // 32-bit FNV-1a masked to its low 28 bits, which is what 7 hex digits hold (HydraX does the same).
    std::string Shorten(std::string_view Name);

    // The stem of an image's picture file: the name with '\' and '/' replaced by '_' and a leading i_ removed.
    std::string ImageFileStem(std::string_view ImageName);
    // stem + ".tif", or ".exr" for a sky image
    std::string ImageFileName(std::string_view ImageName, bool Sky);
    // FilesFolder\file, root-relative, single backslashes
    std::string ImagePath(std::string_view FilesFolder, std::string_view ImageName, bool Sky);
    // the gdt baseImage value: ImagePath with every backslash doubled
    std::string BaseImageValue(std::string_view FilesFolder, std::string_view ImageName, bool Sky);

    // ---- output slots (ModToolsLayout) -----------------------------------------------------------

    enum class Slot
    {
        ExportRoot, MapEnts, MapGeometry, SourceData, Sound, SoundZone, SoundMusic, AnimationTables,
        AnimationStateMachines, Behavior, XAnimRaw, Rumble, SAnim, XCams,
        ModelGdts,      // placed per asset by Place, no folder here
        GdtSource,      // no folder in the Mod Tools layout
        Placed,         // the placement chose the path; the root itself
    };

    // The root-relative folder of a slot (backslashes, no trailing one); false for ModelGdts and GdtSource.
    bool SlotFolder(Slot Which, std::string& Folder);
    // The sound sub-folder of a file name (.alias.csv aliases, .ambient.csv ambients, .reverb.csv reverb,
    // .musiclist.csv and .mus music); empty for anything else.
    std::string SoundSubFolder(std::string_view FileName);
    // The root-relative path of a file in a slot, with the sound sub-folder for the three sound slots.
    // Nothing when the slot has no folder or the name is unsafe (empty, rooted, a drive, or a ".." segment).
    std::optional<std::string> SlotPath(Slot Which, std::string_view RelativeName);

    // ---- placement -------------------------------------------------------------------------------

    struct Request
    {
        std::string Kind;           // the GDF type or raw kind: xmodel, material, image, bulletweapon, rawfile...
        std::string Name;           // the asset name (for a raw kind, the relative path)
        std::string GameId;         // GrayWolf's game id of the source game
        std::string FamilyRoot;     // optional: the model or weapon that owns this asset
        std::string FamilyRootKind; // its kind; defaults to xmodel for an image or material, weapon for attachmentunique, weaponcamotable for weaponcamo
        int FamilyCount = 0;        // optional: how many families use this image (>= 2: shared)
        std::string LeaveOutGdt;    // backtest only: the stock gdt of this very row, left out of Closest
    };

    struct Options
    {
        bool UseStockClosest = true;    // false: the name rules alone (the backtest's first column)
    };

    struct Result
    {
        std::string GdtPath;        // the gdt to write to (after the stock redirect); for a raw kind, the file itself
        std::string FilesFolder;    // where the family's picture and model files go; empty for a raw kind
        std::string Reason;
        std::string Rule;           // P1 .. P18, with a suffix for a variant ("P6.data", "P18.unresolved")
        std::string NaturalGdtPath; // what the rules chose, before the stock redirect (the backtest compares this)
        bool Raw = false;           // GdtPath is a raw file, not a gdt
        bool Redirected = false;    // NaturalGdtPath was a stock gdt path and became graywolf_<file>.gdt
        bool Rejected = false;      // unsafe name: GdtPath is empty
    };

    // First match wins, in this order: P1 raw; P2 child of the same class; the unresolved shelf (P18); P3 to
    // P11 by kind and name; P12 texture library; P13 follows the family's model; P17 stock closest (the
    // stock folder of the longest token prefix); P15 and P16 props; P18 shelf. P14 (DLC prop folders) needs
    // the zone's DLC and is not decided here: a DLC prop lands in P15/P16 and the report says so.
    Result Place(const Request& Asked, const GdtDb::StockIndex& Stock, const Options& Use = {});

    // True when the name is an unresolved hash name (BO4 and Cold War: <prefix>_<10 to 20 digits> or
    // <prefix>_<8 to 16 hex digits with at least one digit>), for a game id that has them.
    bool IsUnresolvedName(std::string_view GameId, std::string_view Name);

    // Self-tests (one example per rule, the fixes, the shortening, a non-Treyarch game); appends each
    // failure to Failures and returns how many there were.
    int SelfTest(std::string& Failures);
}
