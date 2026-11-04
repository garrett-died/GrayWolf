#pragma once

// Module 4 of the Black Ops III conversion layer: read-only access to the Mod Tools' gdt.db and an
// index of which rows are stock. See docs/hydrax-port/bo3-conversion-design.md (module 4) and
// bo3-target-hydrax.md section 3.
//
// The database is opened through the Mod Tools' own bin\sqlite64r.dll (an ordinary SQLite build fails
// on it with "too many columns"), read-only; nothing is ever created, written or journalled under the
// Mod Tools root. Every failure comes back as a Status with a reason: the caller warns once and
// exports everything. No exception crosses this interface.
//
// Everything that touches the disk sits behind two small interfaces (IGdtRowSource, IFileTimes), so
// the name rules, the match modes, the stock rule and Closest are tested on synthetic rows.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Bo3Target::GdtDb
{
    struct Status
    {
        bool Ok = true;
        std::string Reason;     // short, human readable; empty when Ok
        int Code = 0;           // the SQLite result code when one was involved (1 = SQLITE_ERROR: no such table or column)

        static Status Good() { return {}; }
        static Status Fail(std::string Why, int Code = 0) { return { false, std::move(Why), Code }; }
    };

    // The one name rule, for stored and queried names alike: lower case, then the part after the last '/'.
    std::string NormaliseName(std::string_view Name);

    // How a lookup matches: the GDF and the name, or the name alone. The same for every game.
    enum class MatchMode { Type, Name };

    // file:///F:/dir/gdt.db?immutable=1 for a Windows path: opened that way SQLite takes no lock and
    // writes no file at all. Empty when the path cannot be written as a URI (a UNC path).
    std::string ImmutableUri(const std::filesystem::path& DatabaseFile);

    // A private copy of gdt.db and its -wal (when there is one) in a new temp folder, so a database in WAL mode can be
    // opened read-only without a -shm appearing beside the original (in a Mod Tools install). Nothing is created
    // beside the original. On success Copy is the copy's gdt.db and Folder the folder to remove afterwards with RemoveCopy; the folder is created exclusively (an existing one is never used);
    // on failure nothing is left and Error says why.
    bool CopyForReading(const std::filesystem::path& DatabaseFile, std::filesystem::path& Copy, std::filesystem::path& Folder, std::string& Error);

    // Removes the copy and the files SQLite may have added beside it, then the folder only if it is empty.
    void RemoveCopy(const std::filesystem::path& Folder, const std::filesystem::path& Copy);
    bool ParseMatchMode(std::string_view Text, MatchMode& Out);

    // One entity of the database. Id and GdtFile are 0 and empty when only a fallback query could run.
    struct Row
    {
        int64_t Id = 0;
        std::string Name;       // as stored
        std::string Gdf;        // as stored; empty when the database gave no type
        std::string GdtFile;    // the gdt path as stored (absolute, lower case, backslashes) or relative
    };

    // Where rows come from.
    class IGdtRowSource
    {
    public:
        virtual ~IGdtRowSource() = default;
        // Fills Rows. Typed: some row carries a GDF. HasStockInfo: rows carry an id and a gdt path.
        virtual Status Load(std::vector<Row>& Rows, bool& Typed, bool& HasStockInfo) = 0;
    };

    // Every entity of the database, for the exists and conflict checks.
    class Catalog
    {
    public:
        void Add(const Row& Entry);
        // True when an entity of that GDF and name exists. With no types in the database: the name alone.
        bool Contains(std::string_view Gdf, std::string_view Name) const;
        // True when an entity of that name exists, whatever its GDF.
        bool ContainsName(std::string_view Name) const;
        // Contains or ContainsName by the mode; the mode applies to every game.
        bool Matches(MatchMode Mode, std::string_view Gdf, std::string_view Name) const;

        size_t EntityCount() const { return Entities; }
        size_t DistinctGdfs() const { return Gdfs.size(); }
        size_t DistinctGdts() const { return Gdts.size(); }
        bool HasTypes() const { return Typed; }
        // The GDFs (lower case) an entity of this name has, in order; empty when there is none.
        std::vector<std::string> TypesOf(std::string_view Name) const;

    private:
        std::set<std::pair<std::string, std::string>> Pairs;   // (lower gdf, name)
        std::unordered_map<std::string, std::set<std::string>> TypesByName;
        std::unordered_set<std::string> Gdfs, Gdts;
        size_t Entities = 0;
        bool Typed = false;
    };

    // Opens <root>\gdtdb\gdt.db through <root>\bin\sqlite64r.dll and reads it. Not copyable.
    class Database : public IGdtRowSource
    {
    public:
        struct Impl;
        ~Database() override;
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;

        // Never throws. On failure Out stays empty and the Status says why (missing database, missing
        // or unloadable DLL, a function the DLL lacks, a database that will not open).
        static Status Open(const std::filesystem::path& ModToolsRoot, std::unique_ptr<Database>& Out);

        Status Load(std::vector<Row>& Rows, bool& Typed, bool& HasStockInfo) override;

        // A cell of a result row: the text and the integer reading of the same column.
        struct Cell
        {
            std::string Text;
            int64_t Int = 0;
            bool Null = true;
        };
        // Runs one read-only SELECT. OnRow must not throw.
        Status Select(const std::string& Sql, size_t Columns, const std::function<void(const std::vector<Cell>&)>& OnRow);

        // "immutable" (nothing written or locked; used while no write-ahead log has content) or "readonly".
        const std::string& OpenMode() const;

    private:
        Database();
        std::unique_ptr<Impl> State;
    };

    // The age test of the stock rule, behind an interface.
    class IFileTimes
    {
    public:
        virtual ~IFileTimes() = default;
        // Seconds since the Unix epoch of the file's modification time, false when it is not there.
        virtual bool ModifiedSeconds(const std::string& RootRelativePath, int64_t& Seconds) = 0;
    };

    // The real one: files under the Mod Tools root.
    std::unique_ptr<IFileTimes> MakeDiskFileTimes(const std::filesystem::path& ModToolsRoot);

    // Reads the paths of a HydraX-format manifest ({"files":[{"path":...}]}) into a lower-case,
    // backslash set. A missing file is an empty set and Ok; an unreadable or malformed one is a failure.
    Status ReadManifestPaths(const std::filesystem::path& ManifestFile, std::unordered_set<std::string>& Paths);

    struct StockRules
    {
        int64_t ReferenceSeconds = 0;       // modification time of bin\linker_modtools.exe
        int64_t WindowSeconds = 86400;      // "saved late" = a gdt modified more than this after the reference; reported, never decisive
        std::string RootPrefix;             // the Mod Tools root, to cut it off absolute gdt paths (may be empty)
        const std::unordered_set<std::string>* Manifests = nullptr;   // paths written by HydraX or GrayWolf
    };

    struct StockMatch
    {
        std::string Folder;     // root-relative, lower case, backslashes
        std::string Entity;     // a stock entity with that token prefix
        int Tokens = 0;         // leading tokens matched
        std::string Gdt;        // the stock gdt (root-relative) holding most entities of that prefix, ties ordinal
    };

    // One stock row left out of a Closest lookup, so a row can be predicted from all the others
    // (the placement backtest). Both are root-relative, lower case, backslashes.
    struct StockExclusion
    {
        std::string Folder;
        std::string Gdt;
    };

    // Which rows are stock, computed from the install: the row's gdt is not in a manifest and its file
    // name has no hydrax_ or graywolf_ prefix. The gdt's age no longer decides (a stock gdt saved again
    // later, such as texture_assets\t7_metal.gdt, is still stock); rows whose gdt was saved more than
    // WindowSeconds after the reference are only counted, in SavedLater.
    class StockIndex
    {
    public:
        StockIndex(const std::vector<Row>& Rows, const StockRules& Rules, IFileTimes& Files);

        // The GDF family: weapon gdfs and attachmentunique are "weapon"; xmodel, material, image "model".
        static std::string FamilyOf(std::string_view Gdf);
        static std::string RelativeGdtPath(std::string_view Path, std::string_view RootPrefix);

        // The stock folder whose entities share the longest leading-token prefix with the name among
        // entities of the same GDF family; one shared token is not enough unless the name is one token.
        // The folder holding most entities of that prefix wins, a tie goes to the ordinally first.
        // Stock rows only. File names are not combined here.
        bool Closest(std::string_view Gdf, std::string_view Name, StockMatch& Out, const StockExclusion* Exclude = nullptr) const;

        // True when the root-relative gdt path holds stock rows (a path the exporter must not write to).
        bool IsStockGdtPath(std::string_view RelativePath) const;

        bool IsStock(int64_t EntityId) const { return StockIds.count(EntityId) != 0; }
        // Why a row is or is not stock: "stock", "in_manifest", "tool_prefix" or "no_gdt_path", with the
        // relative path, the file's age against the reference in seconds, and "saved_later" /
        // "gdt_file_missing" when those hold.
        std::string Explain(const Row& Entry) const;
        size_t StockCount() const { return Stock; }
        int64_t HighestStockId() const { return HighestId; }

        // Where the other rows went.
        size_t NoGdtFile = 0;       // no usable gdt path (not stock)
        size_t InManifest = 0;      // the gdt is in a HydraX or GrayWolf manifest (not stock)
        size_t ToolPrefixed = 0;    // the gdt file is named hydrax_* or graywolf_* (not stock)
        size_t SavedLater = 0;      // report only: the gdt was saved more than WindowSeconds after the reference (any verdict)
        size_t FileMissing = 0;     // report only: the gdt is not on disk (any verdict)
        size_t StockGdts = 0;       // distinct gdt files of stock rows

    private:
        struct Bucket
        {
            std::map<std::string, int> Folders;
            std::map<std::string, int> Gdts;        // root-relative gdt path -> stock entities
            std::string Entity;
        };
        std::map<std::string, std::map<std::string, Bucket>> Prefixes;   // family -> token prefix -> bucket
        struct GdtVerdict
        {
            int Code = 0;               // 0 stock, 3 in a manifest, 4 tool-prefixed file name
            int64_t AgeSeconds = 0;     // file time minus the reference (when the file exists)
            bool Late = false;
            bool Missing = false;
        };
        std::unordered_map<std::string, GdtVerdict> Verdicts;   // by root-relative gdt path
        std::string RootPrefix;
        std::unordered_set<int64_t> StockIds;
        std::unordered_set<std::string> StockGdtPaths;
        size_t Stock = 0;
        int64_t HighestId = 0;
    };

    // Self-tests on synthetic rows (name rule, match modes, stock rule, Closest). Appends each failure
    // to Failures and returns how many there were.
    int SelfTest(std::string& Failures);
}
