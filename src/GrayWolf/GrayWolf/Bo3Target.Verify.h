#pragma once

// bo3-verify: the static gate over a Black Ops III Mod Tools layout GrayWolf wrote (docs/hydrax-port/plan-materials.md
// section 2, docs/hydrax-port/p1-bo3-verify.md).
//
// STATIC. Every result here is read from the files on disk against the Mod Tools' own deffiles\*.awi and the techset
// catalog. A pass is not proof that APE, the linker or the game accepts the asset; only a link and a run prove that.
//
// Read-only on the layout and on the Mod Tools. The only thing written (by the caller) is the report text this makes.
//
// Checks of this round, in the plan's order:
//   1  gdt.*       GDT text format and the G2/G3 invariants (references-bo3.md)
//   2  awi.*       every key against the GDF's .awi: exists, type, min/max, combo option, reference type, required, name length
//   3  material.*  materialType in the catalog, no camo techset, image keys are slots, image semantic matches the slot,
//                  surfaceType, usage, tint k/255
//   4  rules.*     the hard rules that can be judged from these files
//   5  image.*     baseImage resolves, bit depth, channels, no .dds, colorSRGB, coreSemantic/compression, normal Z, multiple of 4
//   +  model.*     every material a .xmodel_bin names is a material of the output or of the Mod Tools gdt.db (model.material_closure)
//   +  install.*   with a Mod Tools root: output images whose path exists there with other bytes, entity names the gdt.db already holds (warnings)
// Checks 6 (sound), 7 (weapon, anim) and 8 (stock oracle) are listed as not run.

#include "Bo3Target.GdtDb.h"
#include "Bo3Target.Output.h"
#include "Bo3Target.MaterialImages.h"
#include "Bo3Target.Techsets.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Bo3Target::Verify
{
    enum class Severity { Info, Warning, Failure };
    const char* SeverityName(Severity S);

    struct Finding
    {
        std::string Check;      // "image.channels"
        std::string Asset;      // "<gdf>:<name>", or "file:<path relative to the layout root>"
        Severity Level = Severity::Info;
        std::string Detail;
    };

    struct CheckStat
    {
        size_t Judged = 0, Warnings = 0, Failures = 0;
    };

    struct Inputs
    {
        std::vector<std::filesystem::path> Roots;       // layout roots (see ResolveRoots)
        std::vector<std::filesystem::path> OnlyFiles;   // these .gdt files (absolute) instead of every .gdt under the roots
        bool ListedOnly = false;                        // true: OnlyFiles is the whole input, and an empty list is a failure (never a scan of the roots)
        std::filesystem::path ModToolsRoot;             // deffiles\*.awi; empty: the awi checks are not run
        const Techsets::Data* Catalog = nullptr;        // null or empty: the techset checks are not run
        const GdtDb::Catalog* Held = nullptr;           // the Mod Tools gdt.db; null: the stock-pair and unresolved-reference checks are not run
        std::filesystem::path Approximations;           // the run's graywolf_approximations.tsv: it names why a material's surfaceType is <none> (material.surface_unknown / surface_unmapped); empty: not known
        bool MapSources = false;                        // the output holds map sources: a material's usage cannot be judged
    };

    struct Result
    {
        std::vector<Finding> Findings;
        std::map<std::string, CheckStat> Checks;
        std::vector<std::pair<std::string, std::string>> NotRun;    // check, reason
        std::map<std::string, std::pair<size_t, size_t>> Gdfs;      // gdf -> entries, entries with a failure
        size_t Files = 0, Entries = 0, Images = 0, ImagesDecoded = 0;
        size_t Failures = 0, Warnings = 0;
        // What a check this gate normally runs could not judge (no Mod Tools root, no catalog, no gdt.db, an entry parent known only
        // to the gdt.db, a range the awi gives as an expression, a slot image outside the output). Not the by-design NotRun list.
        std::vector<std::pair<std::string, std::string>> Incomplete;
        std::string RunRoots, Identity, Mode;     // the layout roots (joined with '|'), the SHA-256 over the GDT files read, "scan" or "listed"
        size_t IdentityFiles = 0;
        std::vector<std::pair<size_t, std::string>> GdtFiles;      // (root index, path relative to the root) of every GDT read: the gate.file rows
        // 0 pass: every check ran and found nothing. 1: warnings, or incomplete (something this gate normally judges was not judged).
        // 2: a failure, including an input that gave nothing to judge and an entry whose schema could not be read.
        // (3 is the command's: the reports could not be written.) NotRun (checks 6 to 8 and the rules list) is by design and does not count.
        int ExitCode() const { return Failures > 0 ? 2 : (Warnings > 0 || !Incomplete.empty()) ? 1 : 0; }
        const char* Word() const { return Failures > 0 ? "fail" : !Incomplete.empty() ? "incomplete" : Warnings > 0 ? "warnings" : "pass"; }
        // graywolf_gate.tsv: a "#" line saying the results are static, the header, then one row per finding, one
        // gate.entries row per GDF, one not_run row per check not judged.
        std::string Tsv() const;
        // One line.
        std::string Summary() const;
    };

    // The layout roots under an output folder: Out\bo3 when it exists, else every Out\<game>\bo3, else Out itself.
    std::vector<std::filesystem::path> ResolveRoots(const std::filesystem::path& Out);

    // Never throws. Zero work is a failure: no root, no .gdt, a listed file outside the roots, zero entries.
    Result Run(const Inputs& In);

    // The GDT files under the roots (scan mode) with a SHA-256 over their relative paths and contents: the identity a gate and a
    // later reader can compare. False with Error when a folder could not be read.
    bool LayoutIdentity(const std::vector<std::filesystem::path>& Roots, std::string& Hash, size_t& Files, std::string& Error);

    // Does the layout a gate read still hold the same GDT content? Mode "scan": every .gdt under the roots is listed again and hashed; mode
    // "listed": the files the gate names (gate.file rows) are hashed. False with Why when the layout cannot be re-read (a root that is gone,
    // a file that cannot be read, a listing error, no files named) or the hash or the file count differs: an old gate does not stand
    // for GDTs that have changed since.
    bool GateStillHolds(const std::string& Mode, const std::vector<std::string>& Roots, const std::vector<std::pair<size_t, std::string>>& Files,
                        const std::string& Sha256, size_t Count, std::string& Why);

    // The decision for --target modtools: only the .gdt files GrayWolf's manifest lists, never a scan of the Mod Tools root. False
    // (Why says it) when the manifest did not load or lists no .gdt: the gate is then not run.
    bool ModToolsGdtFiles(const Output::Manifest& Listed, bool Loaded, const std::filesystem::path& Root, std::vector<std::filesystem::path>& Out, std::string& Why);

    // Writes graywolf_gate.tsv and graywolf_gate_summary.txt (the one line) into Dir, atomically, creating Dir. These are the
    // gate's only writes.
    bool WriteReports(const Result& R, const std::filesystem::path& Dir, std::string& Error);

    // ---- pieces the self-test reaches

    struct ImageInfo
    {
        bool Ok = false;
        std::string Why;
        uint32_t Width = 0, Height = 0, BitsPerChannel = 0, Channels = 0;
        bool Float = false;
        std::string Container;      // "tiff" or "png"
    };
    // Reads the header of a TIFF or PNG through Read(offset, bytes, size), which returns false past the end.
    ImageInfo ReadImageHeader(const std::function<bool(uint64_t, void*, size_t)>& Read);
    ImageInfo ReadImageHeaderFile(const std::filesystem::path& File);

    struct Thumb
    {
        uint32_t Width = 0, Height = 0;
        std::vector<uint8_t> Rgba;
    };
    // A fixed-grid nearest-neighbour sample of the picture (kThumb x kThumb), so two pictures of different size can be compared per pixel.
    Thumb MakeThumb(const Rgba8Image& Image);
    struct NormalStat { size_t Pixels = 0; double MeanAbs = 0, Max = 0; };
    NormalStat NormalBlue(const Thumb& T);
    struct RangeStat { size_t Pixels = 0, Below = 0, Above = 0; };
    RangeStat DiffuseRange(const Thumb& T);
    // The most common value of the per-pixel RGB mean among non-zero pixels; Share is that value's share of the non-zero pixels.
    struct SpecStat { size_t NonZero = 0, Pixels = 0; int Mode = -1; double Share = 0; };
    SpecStat SpecularMode(const Thumb& T);
    struct MetalStat { size_t MetalPixels = 0, NotBlack = 0; };
    MetalStat BareMetal(const Thumb& Diffuse, const Thumb& Specular);

    // The material names inside a .xmodel_bin (its MATERIAL records), in file order. False with Why when the file cannot be parsed.
    bool ReadXModelBinMaterials(const std::filesystem::path& File, std::vector<std::string>& Names, std::string& Why);

    int SelfTest(std::string& Failures);
}
