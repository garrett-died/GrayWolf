#pragma once

// Module 5 of the Black Ops III conversion layer: what may be written into a Mod Tools layout and how.
// See docs/hydrax-port/bo3-conversion-design.md (module 5) and bo3-target-hydrax.md section 4.
//
//   - a file that exists is never overwritten unless GrayWolf's manifest lists it with the hash it has now;
//   - the manifest is <root>\graywolf\manifest.json in HydraX's schema (version 1); an unreadable one is an error;
//   - every write is a temp file renamed over the target;
//   - a GDT is merged entry by entry, the entries only the old file holds kept byte for byte;
//   - the install guard refuses an output folder inside a Mod Tools install.
//
// Nothing here knows an asset: it works on paths, bytes and hashes. Paths are root-relative with backslashes.
// No exception crosses this interface.

#include "Bo3Target.GdtDb.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Bo3Target::Output
{
    using Status = GdtDb::Status;

    // ---- SHA-256 (Windows BCrypt) ----------------------------------------------------------------

    // Lower-case hex of the SHA-256 of the bytes; empty when BCrypt fails.
    std::string Sha256Hex(const void* Data, size_t Size);
    inline std::string Sha256Hex(std::string_view Text) { return Sha256Hex(Text.data(), Text.size()); }
    // Of a file's whole content; false when it cannot be read.
    bool Sha256File(const std::filesystem::path& File, std::string& Hex);

    // ---- paths ------------------------------------------------------------------------------------

    // The manifest key of a root-relative path: backslashes, trimmed, lower case.
    std::string PathKey(std::string_view RelativePath);

    // ---- the manifest -----------------------------------------------------------------------------

    struct ManifestEntry
    {
        std::string Path;           // as written, backslashes
        std::string Sha256;         // lower-case hex of the file as written
        std::string Title;          // the source game's title ("BlackOps4")
        std::string AssetType;
        std::string AssetName;
        std::string WrittenAt;      // UTC, ISO 8601 round trip ("O")
    };

    class Manifest
    {
    public:
        // A missing file is an empty manifest. Bad JSON, a version other than 1, a missing files array or an
        // entry without a path is a failure and the manifest stays empty.
        Status Load(const std::filesystem::path& File);
        const ManifestEntry* Find(std::string_view RelativePath) const;
        void Record(ManifestEntry Entry);        // by PathKey; marks the manifest changed
        // Atomic. Entries in ordinal key order, 2-space indent, "+" written + as HydraX does.
        Status Save(const std::filesystem::path& File) const;
        std::string ToText() const;
        size_t Size() const { return Entries.size(); }
        bool Changed() const { return Dirty; }
        const std::map<std::string, ManifestEntry>& All() const { return Entries; }

    private:
        std::map<std::string, ManifestEntry> Entries;
        bool Dirty = false;
    };

    // ---- atomic writes ----------------------------------------------------------------------------

    // Writes <file>.<n>.tmp beside the target, then renames it over (MoveFileEx, replace). Creates the folder.
    // On failure the target is untouched and Error says why.
    // Replace false (a Create verdict): the target must not exist at the move, else it fails and nothing is replaced.
    bool WriteAtomic(const std::filesystem::path& File, const void* Data, size_t Size, std::string& Error, bool Replace = true);
    // <file>.bak, else .bak.1, .bak.2...: the first name that does not exist.
    std::filesystem::path FreeBackupPath(const std::filesystem::path& File);

    // ---- the never-overwrite rule -------------------------------------------------------------------

    enum class Verdict
    {
        Create,         // nothing is there
        Replace,        // GrayWolf's own file, untouched since: its content may change
        Unchanged,      // GrayWolf's own file and already holding these bytes
        NotListed,      // a file that exists and the manifest does not list: foreign, never touched
        Modified,       // listed, but the file's hash is no longer the listed one: someone changed it
        Duplicate,      // another asset of this run wrote different bytes here
    };

    const char* VerdictName(Verdict V);
    inline bool Allowed(Verdict V) { return V == Verdict::Create || V == Verdict::Replace || V == Verdict::Unchanged; }

    // Decides, writes nothing. NewHash: the SHA-256 of what would be written. Claimed: the path keys this
    // run already decided on, with the hash they get; a repeat with the same hash is Unchanged.
    class Gate
    {
    public:
        Gate(std::filesystem::path Root, const Manifest& Listed) : RootFolder(std::move(Root)), Owned(Listed) {}
        Verdict Check(std::string_view RelativePath, const std::string& NewHash);
        // Remember that this run writes these bytes to the path (after a successful Check).
        void Claim(std::string_view RelativePath, const std::string& NewHash);
        const std::filesystem::path& Root() const { return RootFolder; }
        std::filesystem::path Absolute(std::string_view RelativePath) const;

    private:
        std::filesystem::path RootFolder;
        const Manifest& Owned;
        std::map<std::string, std::string> Claimed;     // path key -> hash
    };

    // ---- GDT text ---------------------------------------------------------------------------------

    struct GdtEntryText
    {
        std::string Name;           // as written
        std::string Gdf;            // "material.gdf"
        std::string Text;           // from the opening quote of the name to the closing brace, the bytes as read
    };

    // Reads a GDT: `{` entries `}`. An entry is `"name" ( "gdf" )` or `"name" [ "parent" ]` and a braced body of
    // quoted pairs; text inside quotes is skipped whole. False when the file is not that shape.
    bool ParseGdt(std::string_view Text, std::vector<GdtEntryText>& Entries, std::string& Error);
    // The file: `{`, each entry as "\t" + Text + CRLF, `}`, entries in ordinal name order, UTF-8, no BOM.
    std::string RenderGdt(std::vector<GdtEntryText> Entries);
    // One entry as RenderGdt carries it (Gdt::Entry's text without its leading tab and trailing end of line).
    GdtEntryText EntryOf(const std::string& Name, const std::string& Gdf, const std::vector<std::pair<std::string, std::string>>& Fields);

    // Something MergeGdt did that loses an entry: Code is gdt_duplicate_name (two incoming entries of one name: the
    // first stays, the later is dropped) or gdt_replaced_other_type (an old entry replaced by an incoming one of
    // another gdf type). KeptGdf is the type of the entry that is in the file, OtherGdf the one that is not.
    struct GdtMergeNote
    {
        std::string Code, Name, KeptGdf, OtherGdf;
    };

    // Merge by entry name (case aside, whatever the gdf: a name is unique in one file): an incoming entry replaces the
    // old one of its name; every entry only the old file holds stays. Mode Overwrite keeps only the incoming ones.
    // Notes, when given, gets each loss above.
    std::vector<GdtEntryText> MergeGdt(const std::vector<GdtEntryText>& Old, const std::vector<GdtEntryText>& Incoming, bool Overwrite,
                                       std::vector<GdtMergeNote>* Notes = nullptr);

    // ---- the install guard --------------------------------------------------------------------------

    // BlackOps3.exe, bin\linker_modtools.exe, a share\raw folder, or both map_source and source_data.
    bool IsInstall(const std::filesystem::path& Folder);
    // True when the path, or any ancestor of it, is an install; else the first immediate child folder that is one.
    // Found receives the install folder.
    // A path that cannot be made absolute or canonical counts as touching (refused).
    bool Touches(const std::filesystem::path& Path, std::filesystem::path* Found = nullptr);
    // True when File, normalised (symlinks resolved as far as they exist), is Root or below it. An error is false.
    bool IsUnder(const std::filesystem::path& Root, const std::filesystem::path& File);

    int SelfTest(std::string& Failures);
}
