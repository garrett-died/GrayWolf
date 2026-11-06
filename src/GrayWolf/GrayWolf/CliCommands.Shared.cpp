// CliCommands.Shared.cpp - the options every subcommand takes (--install/--out/--cache/--threads/
// --limit/--type/--report/--name-filter/format overrides/--acts-*/--oat-*), on the parent app with
// fallthrough().

#include "CliOptions.h"
#include "DiskGuard.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>

void AddSharedOptions(CLI::App& App, CliOptions& Options)
{
    App.add_option("--install", Options.Install, "Game install / asset source directory")
        ->check(CLI::ExistingDirectory)->group("Shared options");
    App.add_option("--out", Options.Out, "Output file or directory (meaning depends on the command)")->group("Shared options");
    App.add_option("--cache", Options.Cache, "Persistent decompression cache directory")->group("Shared options");
    App.add_option("--threads", Options.Threads, "Worker thread count")->group("Shared options");
    App.add_option("--limit", Options.Limit, "Cap the number of assets exported (disk-export: per kind, what they pull from other zones not counted)")->group("Shared options");
    App.add_option("--type", Options.Type, "Asset type filter (model, image, sound, ...)")->group("Shared options");
    App.add_option("--min-free-gb", DiskGuard::FloorGb(), "Stop the export (exit code 75, a row in graywolf_disk_floor.tsv) when the output drive has less than this many GB free, checked before image writes (default 8; 0 turns it off)")->group("Shared options");
    App.add_option("--report", Options.Report, "Write the result report to this file")->group("Shared options");
    App.add_flag("--export-all", Options.ExportAll, "Export every asset matching --type/--limit/--name-filter")->group("Shared options");
    App.add_option("--name-filter", Options.NameFilter,
        "Comma separated substrings, matches any (case-insensitive); =name matches that name only")->group("Shared options");
    App.add_option("--model-format", Options.ModelFormat,
        "cast|semodel|obj|ma|xna|xmodel_export|xmodel_bin|smd|gltf|glb|none, or a comma list (cast,semodel)")->group("Shared options");
    App.add_option("--anim-format", Options.AnimFormat,
        "cast|seanim|xanim_export|none, or a comma list (cast,seanim)")->group("Shared options");
    App.add_option("--image-format", Options.ImageFormat, "png|tga|dds")->group("Shared options");
    App.add_option("--ui-res", Options.UiRes,
                   "4k|1080|all: BO4 UI texture zones to read, 4K (default) or 1080p (the same images at half size)")
        ->group("Shared options");
    App.add_option("--map-textures", Options.MapTextures,
                   "full|2k|1k: map textures larger than that copied at that size with mips (maps/textures/), "
                   "which the import script reads (full: none)")
        ->group("Shared options");
    App.add_option("--map-png", Options.MapPng,
                   "on|off: also write PNG copies of map colour maps (maps/textures/) for glTF tools other than "
                   "Blender (off: the .glb names the DDS files, Blender's import script reads them)")
        ->group("Shared options");
    App.add_option("--map-cast", Options.MapCast,
                   "on|off: also write the map as a Cast scene (<map>.cast placing maps/models/<model>.cast), "
                   "for Maya and the Cast plugins (off: the .glb only)")
        ->group("Shared options");
    App.add_option("--map-radiant", Options.MapRadiant,
                   "on|off: also write the map as a Radiant source (<map>.map: the clip map's brushes, the entities, "
                   "misc_model placements; <map>_radiant/: the world and models as XMODEL_EXPORT, a GDT), for the mod tools")
        ->group("Shared options");
    App.add_option("--map-prefabs", Options.MapPrefabs,
                   "LIST: with --map-radiant on, each listed part of the map as a standalone map beside it "
                   "(<map>_prefab_<part>.map: a worldspawn and the part), the main map holding a misc_prefab for each "
                   "(\"model\" graywolf_maps/<file>, relative to map_source; --target places the files there). "
                   "brushes, patches, decals, models, effects, entities, brush-entities, lights, or all; none (default)")
        ->group("Shared options");
    App.add_option("--material-gdt", Options.MaterialGdt,
                   "on|off: also write Black Ops III Mod Tools material and image GDT entries for the exported materials "
                   "(models: source_data/graywolf_<out folder>.gdt, their XMODEL_EXPORT files naming them; maps: in the "
                   "Radiant GDT with --map-radiant on)")
        ->group("Shared options");
    App.add_option("--target", Options.PublishTarget,
                   "none|folder|modtools: after a disk-export, place the models, materials and images in the BO3 Mod Tools "
                   "layout. folder: a copy of that layout under <out>/<game>/bo3/; modtools: written into the Mod Tools root "
                   "(--mod-tools-root; only ever taken from the command line, never from the saved settings). none (default): "
                   "the export is unchanged. Implies --material-gdt on, TIFF images and xmodel_bin when those are not given")
        ->group("Shared options");
    App.add_option("--mod-tools-root", Options.ModToolsRoot,
                   "The BO3 Mod Tools folder for --target (else the BO3 install found, else TA_TOOLS_PATH)")
        ->group("Shared options");
    App.add_option("--check-gdtdb", Options.CheckGdtDb,
                   "on|off: with --target, an asset the Mod Tools gdt.db already holds is not written (already_in_mod_tools); "
                   "its materials' and models' references are still followed (on)")
        ->group("Shared options");
    App.add_option("--gdtdb-match", Options.GdtDbMatch,
                   "type|name: the gdt.db check matches the GDF and the name, or the name alone (type)")
        ->group("Shared options");
    App.add_option("--gdt", Options.GdtMode,
                   "merge|overwrite: with --target, a GDT GrayWolf wrote earlier is merged entry by entry (merge) or replaced (overwrite)")
        ->group("Shared options");
    App.add_flag("--plan", Options.PublishPlan,
                 "With --target: decide and report (graywolf_placement.tsv, graywolf_approximations.tsv), write no file in the target")
        ->group("Shared options");
    App.add_flag("--full-map", Options.FullMap,
                 "With --target: a map also brings everything it names into the Mod Tools layout through the publish step: its models "
                 "(placed, entity model keys, the sky) as XMODEL_BIN with their materials and images, the render surfaces' materials, "
                 "worldspawn skyboxmodel and lutmaterial; with --map-radiant, no <map>_radiant folder or GDT of its own. "
                 "graywolf_full_map_report.tsv lists unresolved, in_mod_tools, exported and not_exported_yet (effects, xanims)")
        ->group("Shared options");
    App.add_option("--sound-aliases", Options.SoundAliases,
                   "on|off: the sound kind also writes each zone's sound aliases as a Black Ops III mod tools alias CSV "
                   "(sounds/<zone>.csv, FileSpec relative to sounds/), to port sounds with their behaviour (off: audio only)")
        ->group("Shared options");
    App.add_option("--map-collision", Options.MapCollision,
                   "on|off: also write the map's collision as <map>_collision.json (the clip map's brushes, brush "
                   "models, triggers and triangles), for navigation and top-down tools")
        ->group("Shared options");
    App.add_option("--map-content", Options.MapContent,
                   "all|terrain: the whole map, or its terrain alone (meshes and their materials; no models, "
                   "lights or entities)")
        ->group("Shared options");
    App.add_flag("--decompile{on}", Options.Decompile,
                 "--type script: also decompile the compiled scripts (acts or gsc-tool, by the game's VM); "
                 "--type rawfile: the compiled Lua files (cod-luajit-decompiler or CoDLuaDecompiler, by the bytecode); "
                 "a file the tool does not decompile keeps its bytecode file (--decompile=off: bytecode only)")
        ->group("Shared options");
    App.add_option("--language", Options.Language,
                   "english|french|german|italian|spanish|russian|polish: CoD1/CoD2, the localized archives that "
                   "replace the base ones' files (english: none, the base wins; auto: as --languages)")
        ->group("Shared options");
    App.add_option("--languages", Options.Languages,
                   "all|auto|<lang>,...: the languages exported (default all). auto: the install's, as Battle.net's "
                   ".product.db or Steam's app manifest says (all when neither does); en,fr...: those, auto among them "
                   "(auto,en). Content of no language is always exported; a zone named alone whatever its language")
        ->group("Shared options");
    App.add_option("--audio-format", Options.AudioFormat,
                   "opus|wav|flac: sounds (opus, the default: small and lossy, the game's own Opus packets kept as "
                   "they are, any other sound encoded; wav: lossless, as the game keeps them, or decoded; flac: "
                   "lossless, compressed)")
        ->transform(CLI::IsMember({ "wav", "flac", "opus" }, CLI::ignore_case))
        ->group("Shared options");
    App.add_option("--unit", Options.Unit, "Centimeter|Meter|Inch (model export scale)")->group("Shared options");
    App.add_flag("--lods", Options.Lods, "Export every available model LOD")->group("Shared options");
    App.add_option("--normal-map", Options.NormalMap, "none|opengl (flip normal-map green for Blender/DCC)")->group("Shared options");
    App.add_option("--acts-exe", Options.ActsExe, "Path to acts.exe (else acts.exe on PATH)")->group("Shared options");
    App.add_option("--gsc-tool-exe", Options.GscToolExe, "Path to gsc-tool.exe (else gsc-tool.exe on PATH)")->group("Shared options");
    App.add_option("--cod-luajit-decompiler-exe", Options.CodLuaJitExe,
                   "Path to cod-luajit-decompiler (else cod-luajit-decompiler[-windows-x86_64].exe on PATH)")->group("Shared options");
    App.add_option("--cod-lua-decompiler-exe", Options.CodLuaExe,
                   "Path to CoDLuaDecompiler.CLI.exe (else on PATH)")->group("Shared options");
    App.add_option("--ffmpeg-exe", Options.FfmpegExe,
                   "Path to ffmpeg.exe, the WMA sounds' decoder (else beside GrayWolf.exe, on PATH, or from WinGet, Chocolatey, Scoop; none: WMA sounds kept as they are)")->group("Shared options");
    App.add_option("--game-exe", Options.GameExe,
                   "--decompile: the game's dumped (unpacked) exe acts decrypts the scripts' encrypted strings with, "
                   "for a game that encrypts them (default: the game's dump in acts' deps folder; not found: strings left encrypted)")
        ->group("Shared options");
    App.add_option("--acts-handler", Options.ActsHandler, "Force a game handler for acts (bo4/cw/mwiii/bo6/...)")->group("Shared options");
    App.add_option("--oat-model-format", Options.OatModelFmt, "gltf|glb|obj|xmodel_export|xmodel_bin")->group("Shared options");
    App.add_option("--oat-image-format", Options.OatImageFmt, "dds|iwi")->group("Shared options");
    App.add_flag("--oat-gdt", Options.OatGdt, "Also emit a source_data/<zone>.gdt")->group("Shared options");
    App.add_flag("--with-sounds", Options.WithSounds, "Also extract sounds OAT can't dump (T5/BO1)")->group("Shared options");
}
