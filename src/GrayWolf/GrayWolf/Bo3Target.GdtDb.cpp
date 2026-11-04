#include "stdafx.h"

// See Bo3Target.GdtDb.h.

#include "Bo3Target.GdtDb.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <format>

namespace fs = std::filesystem;

namespace Bo3Target::GdtDb
{
    // ---- names -------------------------------------------------------------------------------

    std::string NormaliseName(std::string_view Name)
    {
        const size_t Slash = Name.rfind('/');
        if (Slash != std::string_view::npos) Name.remove_prefix(Slash + 1);
        std::string Out(Name);
        for (char& C : Out)
        {
            if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
        }
        return Out;
    }

    bool ParseMatchMode(std::string_view Text, MatchMode& Out)
    {
        const std::string Low = NormaliseName(Text);
        if (Low == "type") { Out = MatchMode::Type; return true; }
        if (Low == "name") { Out = MatchMode::Name; return true; }
        return false;
    }

    namespace
    {
        std::string LowerAscii(std::string_view Text)
        {
            std::string Out(Text);
            for (char& C : Out)
            {
                if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
            }
            return Out;
        }

        // lower case, backslashes, no trailing backslash on a folder prefix
        std::string PathKey(std::string_view Path)
        {
            std::string Out = LowerAscii(Path);
            for (char& C : Out)
            {
                if (C == '/') C = '\\';
            }
            return Out;
        }

        std::vector<std::string> Tokens(std::string_view LowerName)
        {
            std::vector<std::string> Out;
            size_t Start = 0;
            while (Start <= LowerName.size())
            {
                size_t End = LowerName.find('_', Start);
                if (End == std::string_view::npos) End = LowerName.size();
                if (End > Start) Out.emplace_back(LowerName.substr(Start, End - Start));
                Start = End + 1;
            }
            return Out;
        }
    }

    // ---- catalog -----------------------------------------------------------------------------

    void Catalog::Add(const Row& Entry)
    {
        const std::string Name = NormaliseName(Entry.Name);
        if (Name.empty()) return;
        ++Entities;
        const std::string Gdf = LowerAscii(Entry.Gdf);
        TypesByName[Name].insert(Gdf);
        if (!Gdf.empty())
        {
            Typed = true;
            Pairs.emplace(Gdf, Name);
            Gdfs.insert(Gdf);
        }
        if (!Entry.GdtFile.empty()) Gdts.insert(PathKey(Entry.GdtFile));
    }

    bool Catalog::Contains(std::string_view Gdf, std::string_view Name) const
    {
        if (!Typed) return ContainsName(Name);
        return Pairs.count({ LowerAscii(Gdf), NormaliseName(Name) }) != 0;
    }

    bool Catalog::ContainsName(std::string_view Name) const
    {
        return TypesByName.count(NormaliseName(Name)) != 0;
    }

    bool Catalog::Matches(MatchMode Mode, std::string_view Gdf, std::string_view Name) const
    {
        return Mode == MatchMode::Type ? Contains(Gdf, Name) : ContainsName(Name);
    }

    std::vector<std::string> Catalog::TypesOf(std::string_view Name) const
    {
        std::vector<std::string> Out;
        const auto It = TypesByName.find(NormaliseName(Name));
        if (It == TypesByName.end()) return Out;
        for (const std::string& Type : It->second)
        {
            if (!Type.empty()) Out.push_back(Type);
        }
        return Out;
    }

    // ---- stock index -------------------------------------------------------------------------

    std::string StockIndex::FamilyOf(std::string_view Gdf)
    {
        const std::string G = LowerAscii(Gdf);
        if (G.ends_with("weapon") || G == "attachmentunique") return "weapon";
        return (G == "xmodel" || G == "material" || G == "image") ? "model" : G;
    }

    std::string StockIndex::RelativeGdtPath(std::string_view Path, std::string_view RootPrefix)
    {
        static const char* const Anchors[] = { "source_data", "model_export", "xanim_export", "texture_assets", "art_assets", "_custom", "archetypes" };
        std::string P = PathKey(Path);
        while (!P.empty() && (P.front() == ' ' || P.front() == '\t')) P.erase(P.begin());
        while (!P.empty() && (P.back() == ' ' || P.back() == '\t')) P.pop_back();
        if (P.empty()) return {};
        if (!RootPrefix.empty())
        {
            std::string R = PathKey(RootPrefix);
            while (!R.empty() && R.back() == '\\') R.pop_back();
            R += '\\';
            if (P.starts_with(R)) return P.substr(R.size());
        }
        const bool Rooted = P[0] == '\\' || (P.size() > 1 && P[1] == ':');
        if (!Rooted) return P;
        size_t Best = std::string::npos;
        for (const char* Anchor : Anchors)
        {
            const size_t At = P.find(std::string("\\") + Anchor + "\\");
            if (At != std::string::npos && (Best == std::string::npos || At < Best)) Best = At;
        }
        return Best == std::string::npos ? std::string() : P.substr(Best + 1);
    }

    StockIndex::StockIndex(const std::vector<Row>& Rows, const StockRules& Rules, IFileTimes& Files)
    {
        RootPrefix = Rules.RootPrefix;
        std::unordered_set<std::string> UsedGdts;
        for (const Row& Entry : Rows)
        {
            const std::string Name = NormaliseName(Entry.Name);
            const std::string Relative = RelativeGdtPath(Entry.GdtFile, Rules.RootPrefix);
            if (Name.empty() || Relative.empty())
            {
                ++NoGdtFile;
                continue;
            }
            auto Found = Verdicts.find(Relative);
            if (Found == Verdicts.end())
            {
                // Stock = not in a manifest and not a file this toolchain names (hydrax_ or graywolf_ prefix).
                // The file's age no longer decides; it is only measured, for the report.
                GdtVerdict Result;
                const size_t Slash = Relative.rfind('\\');
                const std::string_view Leaf = std::string_view(Relative).substr(Slash == std::string::npos ? 0 : Slash + 1);
                if (Rules.Manifests != nullptr && Rules.Manifests->count(Relative) != 0) Result.Code = 3;
                else if (Leaf.starts_with("hydrax_") || Leaf.starts_with("graywolf_")) Result.Code = 4;
                int64_t Seconds = 0;
                if (!Files.ModifiedSeconds(Relative, Seconds)) Result.Missing = true;
                else
                {
                    Result.AgeSeconds = Seconds - Rules.ReferenceSeconds;
                    Result.Late = Result.AgeSeconds > Rules.WindowSeconds;
                }
                Found = Verdicts.emplace(Relative, Result).first;
            }
            if (Found->second.Missing) ++FileMissing;
            if (Found->second.Late) ++SavedLater;
            switch (Found->second.Code)
            {
            case 3: ++InManifest; continue;
            case 4: ++ToolPrefixed; continue;
            default: break;
            }

            ++Stock;
            StockIds.insert(Entry.Id);
            HighestId = (std::max)(HighestId, Entry.Id);
            UsedGdts.insert(Relative);
            StockGdtPaths.insert(Relative);

            const size_t Slash = Relative.rfind('\\');
            const std::string Folder = Slash == std::string::npos ? std::string() : Relative.substr(0, Slash);
            auto& ByPrefix = Prefixes[FamilyOf(Entry.Gdf)];
            const std::vector<std::string> Parts = Tokens(Name);
            std::string Prefix;
            for (size_t N = 0; N < Parts.size(); ++N)
            {
                if (N != 0) Prefix += '_';
                Prefix += Parts[N];
                Bucket& B = ByPrefix[Prefix];
                ++B.Folders[Folder];
                ++B.Gdts[Relative];
                if (B.Entity.empty() || Name < B.Entity) B.Entity = Name;
            }
        }
        StockGdts = UsedGdts.size();
    }

    std::string StockIndex::Explain(const Row& Entry) const
    {
        const std::string Relative = RelativeGdtPath(Entry.GdtFile, RootPrefix);
        if (Relative.empty()) return "no_gdt_path";
        const auto Found = Verdicts.find(Relative);
        if (Found == Verdicts.end()) return "unknown gdt " + Relative;
        static const char* const Words[] = { "stock", "", "", "in_manifest", "tool_prefix" };
        return std::format("{} gdt={} age_vs_linker_seconds={}{}{}", Words[Found->second.Code], Relative, Found->second.AgeSeconds,
            Found->second.Late ? " saved_later" : "", Found->second.Missing ? " gdt_file_missing" : "");
    }

    bool StockIndex::IsStockGdtPath(std::string_view RelativePath) const
    {
        return StockGdtPaths.count(PathKey(RelativePath)) != 0;
    }

    bool StockIndex::Closest(std::string_view Gdf, std::string_view Name, StockMatch& Out, const StockExclusion* Exclude) const
    {
        const auto Family = Prefixes.find(FamilyOf(Gdf));
        if (Family == Prefixes.end()) return false;
        const std::vector<std::string> Parts = Tokens(NormaliseName(Name));
        const size_t Minimum = Parts.size() == 1 ? 1 : 2;
        // most entities wins; the maps are ordinal, so the first of a tie is kept
        const auto Best = [](const std::map<std::string, int>& Counts, const std::string& Skip, std::string& Winner)
        {
            int BestCount = 0;
            int Total = 0;
            for (const auto& [Key, Raw] : Counts)
            {
                const int Count = Raw - (!Skip.empty() && Key == Skip ? 1 : 0);
                if (Count <= 0) continue;
                Total += Count;
                if (Count > BestCount) { BestCount = Count; Winner = Key; }
            }
            return Total;
        };
        const std::string NoSkip;
        for (size_t N = Parts.size(); N >= Minimum && N >= 1; --N)
        {
            std::string Prefix;
            for (size_t I = 0; I < N; ++I)
            {
                if (I != 0) Prefix += '_';
                Prefix += Parts[I];
            }
            const auto Hit = Family->second.find(Prefix);
            if (Hit == Family->second.end()) continue;
            std::string Folder, Gdt;
            if (Best(Hit->second.Folders, Exclude != nullptr ? Exclude->Folder : NoSkip, Folder) == 0) continue;   // only the row itself: a shorter prefix
            Best(Hit->second.Gdts, Exclude != nullptr ? Exclude->Gdt : NoSkip, Gdt);
            Out.Folder = Folder;
            Out.Gdt = Gdt;
            Out.Entity = Hit->second.Entity;
            Out.Tokens = static_cast<int>(N);
            return true;
        }
        return false;
    }

    // ---- file side ---------------------------------------------------------------------------

    namespace
    {
        bool SecondsOf(const fs::path& File, int64_t& Seconds)
        {
            std::error_code Ec;
            const auto Time = fs::last_write_time(File, Ec);
            if (Ec) return false;
            const auto Sys = std::chrono::clock_cast<std::chrono::system_clock>(Time);
            Seconds = std::chrono::duration_cast<std::chrono::seconds>(Sys.time_since_epoch()).count();
            return true;
        }

        class DiskFileTimes final : public IFileTimes
        {
        public:
            explicit DiskFileTimes(fs::path Root) : Root(std::move(Root)) {}
            bool ModifiedSeconds(const std::string& RootRelativePath, int64_t& Seconds) override
            {
                return SecondsOf(Root / fs::path(RootRelativePath), Seconds);
            }

        private:
            fs::path Root;
        };
    }

    std::unique_ptr<IFileTimes> MakeDiskFileTimes(const fs::path& ModToolsRoot)
    {
        return std::make_unique<DiskFileTimes>(ModToolsRoot);
    }

    Status ReadManifestPaths(const fs::path& ManifestFile, std::unordered_set<std::string>& Paths)
    {
        std::error_code Ec;
        if (!fs::exists(ManifestFile, Ec)) return Status::Good();
        try
        {
            std::ifstream In(ManifestFile, std::ios::binary);
            if (!In) return Status::Fail("manifest cannot be opened: " + ManifestFile.string());
            const nlohmann::json Doc = nlohmann::json::parse(In);
            const auto Files = Doc.find("files");
            if (Files == Doc.end() || !Files->is_array()) return Status::Fail("manifest has no files array: " + ManifestFile.string());
            for (const auto& Entry : *Files)
            {
                const auto Path = Entry.find("path");
                if (Path != Entry.end() && Path->is_string()) Paths.insert(PathKey(Path->get<std::string>()));
            }
            return Status::Good();
        }
        catch (const std::exception& Error)   // boundary: nothing may escape this module
        {
            return Status::Fail(std::string("manifest unreadable: ") + ManifestFile.string() + ": " + Error.what());
        }
    }

    // ---- SQLite through the Mod Tools' own DLL ----------------------------------------------

    namespace
    {
        std::string Utf8Of(const fs::path& Path)
        {
            const std::u8string Text = Path.generic_u8string();
            return std::string(reinterpret_cast<const char*>(Text.data()), Text.size());
        }
    }

    std::string ImmutableUri(const fs::path& DatabaseFile)
    {
        const std::string Utf8 = Utf8Of(DatabaseFile);
        if (Utf8.starts_with("//")) return {};
        std::string Out = "file:///";
        for (const unsigned char C : Utf8)
        {
            const bool Plain = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9')
                || C == '/' || C == ':' || C == '-' || C == '_' || C == '.' || C == '~';
            if (Plain) Out += static_cast<char>(C);
            else Out += std::format("%{:02X}", static_cast<unsigned>(C));
        }
        return Out + "?immutable=1";
    }

    namespace
    {
        struct sqlite3;
        struct sqlite3_stmt;

        constexpr int SqliteOk = 0, SqliteRow = 100, SqliteDone = 101;
        constexpr int OpenReadOnly = 0x00000001, OpenUri = 0x00000040;

        struct Api
        {
            int (__cdecl* OpenV2)(const char*, sqlite3**, int, const char*) = nullptr;
            int (__cdecl* Close)(sqlite3*) = nullptr;
            int (__cdecl* BusyTimeout)(sqlite3*, int) = nullptr;
            int (__cdecl* PrepareV2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
            int (__cdecl* Step)(sqlite3_stmt*) = nullptr;
            int (__cdecl* Finalize)(sqlite3_stmt*) = nullptr;
            const unsigned char* (__cdecl* ColumnText)(sqlite3_stmt*, int) = nullptr;
            int (__cdecl* ColumnBytes)(sqlite3_stmt*, int) = nullptr;
            long long (__cdecl* ColumnInt64)(sqlite3_stmt*, int) = nullptr;
            int (__cdecl* ColumnType)(sqlite3_stmt*, int) = nullptr;
            const char* (__cdecl* ErrMsg)(sqlite3*) = nullptr;
        };

        struct ModuleFree
        {
            void operator()(HINSTANCE__* Module) const noexcept { FreeLibrary(Module); }
        };
        using ModuleHandle = std::unique_ptr<HINSTANCE__, ModuleFree>;

        struct DbClose
        {
            int (__cdecl* Close)(sqlite3*) = nullptr;
            void operator()(sqlite3* Db) const noexcept
            {
                if (Close != nullptr) Close(Db);
            }
        };
        using DbHandle = std::unique_ptr<sqlite3, DbClose>;

        struct StmtFinalize
        {
            int (__cdecl* Finalize)(sqlite3_stmt*) = nullptr;
            void operator()(sqlite3_stmt* Stmt) const noexcept
            {
                if (Finalize != nullptr) Finalize(Stmt);
            }
        };
        using StmtHandle = std::unique_ptr<sqlite3_stmt, StmtFinalize>;

        template <typename Fn>
        bool Resolve(HMODULE Module, const char* Name, Fn& Out)
        {
            Out = reinterpret_cast<Fn>(GetProcAddress(Module, Name));
            return Out != nullptr;
        }
    }

    void RemoveCopy(const fs::path& Folder, const fs::path& Copy)
    {
        if (Folder.empty()) return;
        // only the files a read of the copy makes (the copy, its log and the files SQLite may add), then the folder
        // only when nothing else is in it
        std::error_code Ec;
        if (!Copy.empty())
        {
            for (const wchar_t* Suffix : { L"", L"-wal", L"-shm", L"-journal" })
                fs::remove(fs::path(Copy.native() + Suffix), Ec);
        }
        fs::remove(Folder, Ec);
    }

    bool CopyForReading(const fs::path& DatabaseFile, fs::path& Copy, fs::path& Folder, std::string& Error)
    {
        static std::atomic<unsigned> Counter{ 0 };
        std::error_code Ec;
        Copy.clear();
        Folder.clear();
        const fs::path Temp = fs::temp_directory_path(Ec);
        if (Ec)
        {
            Error = "no temp folder: " + Ec.message();
            return false;
        }
        // CreateDirectoryW fails on a folder that exists, so the folder is ours alone: a leftover or a reused PID's
        // folder is skipped for the next name, never written into
        fs::path Made;
        for (int Try = 0; Try < 1000; ++Try)
        {
            const fs::path Candidate = Temp / (L"graywolf_gdtdb_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(Counter.fetch_add(1)));
            if (CreateDirectoryW(Candidate.c_str(), nullptr))
            {
                Made = Candidate;
                break;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
            {
                Error = std::format("cannot create {} (Windows error {})", Candidate.string(), GetLastError());
                return false;
            }
        }
        if (Made.empty())
        {
            Error = "no free graywolf_gdtdb_ folder name in " + Temp.string();
            return false;
        }
        const fs::path Wal = fs::path(DatabaseFile.native() + L"-wal");
        const fs::path Target = Made / DatabaseFile.filename();
        bool Ok = fs::copy_file(DatabaseFile, Target, fs::copy_options::overwrite_existing, Ec) && !Ec;
        if (Ok && fs::exists(Wal, Ec))
        {
            Ok = fs::copy_file(Wal, fs::path(Target.native() + L"-wal"), fs::copy_options::overwrite_existing, Ec) && !Ec;
        }
        if (!Ok)
        {
            Error = "cannot copy " + DatabaseFile.string() + " to " + Made.string() + ": " + Ec.message();
            RemoveCopy(Made, Target);
            return false;
        }
        Copy = Target;
        Folder = Made;
        return true;
    }

    namespace
    {
        // removes a temp folder when the database that reads it is closed
        struct TempFolder
        {
            fs::path Path;
            fs::path File;      // the copy of the database inside it
            TempFolder() = default;
            TempFolder(const TempFolder&) = delete;
            TempFolder& operator=(const TempFolder&) = delete;
            ~TempFolder()
            {
                RemoveCopy(Path, File);
            }
        };
    }

    struct Database::Impl
    {
        ModuleHandle Module;    // declared first: destroyed last, after the database handle
        Api Calls;
        TempFolder Copy;        // before Db: destroyed after the handle is closed
        DbHandle Db;
        std::string Mode;
    };

    Database::Database() : State(std::make_unique<Impl>()) {}
    Database::~Database() = default;

    const std::string& Database::OpenMode() const { return State->Mode; }

    Status Database::Open(const fs::path& ModToolsRoot, std::unique_ptr<Database>& Out)
    {
        Out.reset();
        try
        {
            std::error_code Ec;
            const fs::path Root = fs::absolute(ModToolsRoot, Ec);
            const fs::path DbFile = Root / "gdtdb" / "gdt.db";
            fs::path Dll = Root / "bin" / "sqlite64r.dll";
            Dll.make_preferred();   // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs backslashes
            if (!fs::is_regular_file(DbFile, Ec)) return Status::Fail("gdt.db not found: " + DbFile.string());
            if (!fs::is_regular_file(Dll, Ec)) return Status::Fail("sqlite64r.dll not found: " + Dll.string());

            std::unique_ptr<Database> Result(new Database());
            Impl& S = *Result->State;
            // Full path, and only this DLL's folder plus the system folders: no search of the current
            // directory or PATH, so no other sqlite DLL is picked up by accident.
            S.Module.reset(LoadLibraryExW(Dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
            if (!S.Module) return Status::Fail(std::format("cannot load {} (Windows error {})", Dll.string(), GetLastError()));

            const HMODULE M = S.Module.get();
            Api& A = S.Calls;
            const bool All = Resolve(M, "sqlite3_open_v2", A.OpenV2) && Resolve(M, "sqlite3_close", A.Close)
                && Resolve(M, "sqlite3_busy_timeout", A.BusyTimeout) && Resolve(M, "sqlite3_prepare_v2", A.PrepareV2)
                && Resolve(M, "sqlite3_step", A.Step) && Resolve(M, "sqlite3_finalize", A.Finalize)
                && Resolve(M, "sqlite3_column_text", A.ColumnText) && Resolve(M, "sqlite3_column_bytes", A.ColumnBytes)
                && Resolve(M, "sqlite3_column_int64", A.ColumnInt64) && Resolve(M, "sqlite3_column_type", A.ColumnType)
                && Resolve(M, "sqlite3_errmsg", A.ErrMsg);
            if (!All) return Status::Fail("sqlite64r.dll lacks a sqlite3 function this reader needs");

            // With no log content the file is opened immutable: no lock, no -shm, no -wal, nothing written anywhere.
            // A write-ahead log with content (or an immutable open that did not work) would make a plain read-only
            // connection create a -shm file in the Mod Tools folder: then a private copy in the temp folder is read.
            const fs::path Wal = fs::path(DbFile.native() + L"-wal");
            const bool WalEmpty = !fs::exists(Wal, Ec) || fs::file_size(Wal, Ec) == 0;
            const std::string Uri = WalEmpty ? ImmutableUri(DbFile) : std::string();

            sqlite3* Raw = nullptr;
            int Rc = SqliteOk;
            if (!Uri.empty())
            {
                Rc = A.OpenV2(Uri.c_str(), &Raw, OpenReadOnly | OpenUri, nullptr);
                if (Rc == SqliteOk) S.Mode = "immutable";
                else
                {
                    S.Db = DbHandle(Raw, DbClose{ A.Close });
                    S.Db.reset();
                    Raw = nullptr;
                }
            }
            if (S.Mode.empty())
            {
                fs::path Copy, Folder;
                std::string CopyError;
                if (!CopyForReading(DbFile, Copy, Folder, CopyError))
                    return Status::Fail("gdt.db cannot be read without touching the install (" + CopyError + ")");
                S.Copy.Path = Folder;
                S.Copy.File = Copy;
                Rc = A.OpenV2(Utf8Of(Copy).c_str(), &Raw, OpenReadOnly, nullptr);
                if (Rc != SqliteOk)
                {
                    const std::string Why = Raw != nullptr && A.ErrMsg != nullptr ? A.ErrMsg(Raw) : "open failed";
                    S.Db = DbHandle(Raw, DbClose{ A.Close });
                    return Status::Fail(std::format("cannot open a copy of {} read-only: {} (code {})", DbFile.string(), Why, Rc), Rc);
                }
                S.Mode = "copy";
            }
            S.Db = DbHandle(Raw, DbClose{ A.Close });
            A.BusyTimeout(S.Db.get(), 3000);
            Out = std::move(Result);
            return Status::Good();
        }
        catch (const std::exception& Error)   // boundary: nothing may escape this module
        {
            Out.reset();
            return Status::Fail(std::string("gdt.db open failed: ") + Error.what());
        }
    }

    Status Database::Select(const std::string& Sql, size_t Columns, const std::function<void(const std::vector<Cell>&)>& OnRow)
    {
        try
        {
            Impl& S = *State;
            const Api& A = S.Calls;
            sqlite3_stmt* Raw = nullptr;
            const int Rc = A.PrepareV2(S.Db.get(), Sql.c_str(), -1, &Raw, nullptr);
            StmtHandle Stmt(Raw, StmtFinalize{ A.Finalize });
            if (Rc != SqliteOk) return Status::Fail(std::format("{} (code {})", A.ErrMsg(S.Db.get()), Rc), Rc);
            std::vector<Cell> Cells(Columns);
            for (;;)
            {
                const int Step = A.Step(Stmt.get());
                if (Step == SqliteDone) return Status::Good();
                if (Step != SqliteRow) return Status::Fail(std::format("{} (code {})", A.ErrMsg(S.Db.get()), Step), Step);
                for (size_t C = 0; C < Columns; ++C)
                {
                    const int Col = static_cast<int>(C);
                    Cell& Out = Cells[C];
                    Out.Null = A.ColumnType(Stmt.get(), Col) == 5;
                    Out.Int = A.ColumnInt64(Stmt.get(), Col);
                    const unsigned char* Text = A.ColumnText(Stmt.get(), Col);
                    const int Bytes = A.ColumnBytes(Stmt.get(), Col);
                    Out.Text.assign(Text != nullptr ? reinterpret_cast<const char*>(Text) : "", Text != nullptr && Bytes > 0 ? static_cast<size_t>(Bytes) : 0);
                }
                OnRow(Cells);
            }
        }
        catch (const std::exception& Error)   // boundary: nothing may escape this module
        {
            return Status::Fail(std::string("query failed: ") + Error.what());
        }
    }

    Status Database::Load(std::vector<Row>& Rows, bool& Typed, bool& HasStockInfo)
    {
        Rows.clear();
        Typed = false;
        HasStockInfo = false;
        // Tried in order; only SQLITE_ERROR (a table or column is missing) falls through.
        static const char* const Full = "SELECT e.PK_id, e.name, g.name, t.name FROM _entity e JOIN _gdf g ON g.PK_id = e.FK_gdf JOIN _gdt t ON t.PK_id = e.FK_gdt";
        static const char* const TypedOnly = "SELECT e.name, g.name FROM _entity e JOIN _gdf g ON g.PK_id = e.FK_gdf";
        static const char* const NameOnly = "SELECT name FROM _entity";

        const auto Collect = [&](const char* Sql, size_t Columns, bool Full4)
        {
            Rows.clear();
            return Select(Sql, Columns, [&](const std::vector<Cell>& Cells)
            {
                Row R;
                if (Full4)
                {
                    R.Id = Cells[0].Int;
                    R.Name = Cells[1].Text;
                    R.Gdf = Cells[2].Text;
                    R.GdtFile = Cells[3].Text;
                }
                else
                {
                    R.Name = Cells[0].Text;
                    if (Columns > 1) R.Gdf = Cells[1].Text;
                }
                Rows.push_back(std::move(R));
            });
        };

        Status Result = Collect(Full, 4, true);
        if (Result.Ok) HasStockInfo = true;
        else if (Result.Code == 1) Result = Collect(TypedOnly, 2, false);
        if (!Result.Ok && Result.Code == 1) Result = Collect(NameOnly, 1, false);
        if (!Result.Ok)
        {
            Rows.clear();
            return Status::Fail("gdt.db query failed: " + Result.Reason, Result.Code);
        }
        for (const Row& R : Rows)
        {
            if (!R.Gdf.empty()) { Typed = true; break; }
        }
        return Status::Good();
    }

    // ---- self-test ---------------------------------------------------------------------------

    namespace
    {
        class FakeFileTimes final : public IFileTimes
        {
        public:
            std::map<std::string, int64_t> Times;
            bool ModifiedSeconds(const std::string& Path, int64_t& Seconds) override
            {
                const auto It = Times.find(Path);
                if (It == Times.end()) return false;
                Seconds = It->second;
                return true;
            }
        };
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got)
        {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL GdtDb {} (got '{}')\n", Name, Got);
        };

        // name rule: lower case, after the last '/'
        Check(NormaliseName("T7_Decal_Grunge") == "t7_decal_grunge", "name lower-cased", NormaliseName("T7_Decal_Grunge"));
        Check(NormaliseName("Dir/Sub/Name_01") == "name_01", "name after last slash", NormaliseName("Dir/Sub/Name_01"));
        Check(NormaliseName("trailing/").empty(), "name ending in slash is empty", NormaliseName("trailing/"));
        Check(NormaliseName("a\\b") == "a\\b", "backslash is not a separator", NormaliseName("a\\b"));

        MatchMode Mode = MatchMode::Name;
        Check(ParseMatchMode("TYPE", Mode) && Mode == MatchMode::Type, "match mode type", "");
        Check(ParseMatchMode("name", Mode) && Mode == MatchMode::Name, "match mode name", "");
        Check(!ParseMatchMode("v01", Mode), "match mode rejects other words", "");

        // catalog: one rule for stored and queried names, both lookups agree on a name with '/'
        Catalog Typed;
        Typed.Add({ 1, "Props/Crate_A", "xmodel", "" });
        Typed.Add({ 2, "t7_wall", "material", "" });
        Typed.Add({ 3, "t7_wall", "image", "" });
        Check(Typed.Contains("xmodel", "crate_a"), "typed contains by stored path name", "");
        Check(Typed.Contains("XMODEL", "x/y/CRATE_A"), "typed contains by queried path name", "");
        Check(Typed.ContainsName("crate_a") && Typed.ContainsName("a/b/Crate_A"), "name contains agrees with contains on a slash name", "");
        Check(!Typed.Contains("material", "crate_a"), "typed contains is by type", "");
        Check(Typed.ContainsName("crate_a"), "name mode ignores the type", "");
        Check(Typed.Matches(MatchMode::Type, "image", "t7_wall") && !Typed.Matches(MatchMode::Type, "xmodel", "t7_wall"), "type mode needs the type", "");
        Check(Typed.Matches(MatchMode::Name, "xmodel", "t7_wall"), "name mode matches any type", "");
        Check(!Typed.ContainsName("invented_name_zzz"), "absent name", "");
        Check(Typed.TypesOf("T7_WALL").size() == 2 && Typed.EntityCount() == 3 && Typed.DistinctGdfs() == 3, "types of a name and counts",
            std::to_string(Typed.TypesOf("T7_WALL").size()));
        Catalog Untyped;
        Untyped.Add({ 0, "abc", "", "" });
        Check(Untyped.Contains("anything", "abc") && !Untyped.HasTypes(), "untyped database falls back to the name", "");

        // a WAL database is read through a private copy: the original's folder gets nothing, the copy goes away
        {
            std::error_code Ec;
            const fs::path Src = fs::temp_directory_path(Ec) / (L"graywolf_selftest_gdtdb_src_" + std::to_wstring(GetCurrentProcessId()));
            fs::remove_all(Src, Ec);
            fs::create_directories(Src, Ec);
            { std::ofstream(Src / "gdt.db", std::ios::binary) << "dbbytes"; }
            { std::ofstream(Src / "gdt.db-wal", std::ios::binary) << "walbytes"; }
            fs::path Copy, Folder;
            std::string Error;
            const bool Made = CopyForReading(Src / "gdt.db", Copy, Folder, Error);
            Check(Made && fs::file_size(Copy, Ec) == 7 && fs::file_size(fs::path(Copy.native() + L"-wal"), Ec) == 8, "wal copy: db and log copied", Error);
            size_t InSource = 0;
            for (const auto& E : fs::directory_iterator(Src, Ec)) { (void)E; ++InSource; }
            Check(InSource == 2 && Made && Copy.parent_path() == Folder && Folder != Src, "wal copy: nothing new beside the original", std::to_string(InSource));
            if (Made) RemoveCopy(Folder, Copy);
            Check(!Made || !fs::exists(Folder, Ec), "wal copy: the folder is removed once its files are", Folder.string());
            // a folder name that is taken (a reused PID) is skipped: the sentinel in it survives the copy and its cleanup
            {
                fs::path C1, F1, C3, F3;
                const bool First = CopyForReading(Src / "gdt.db", C1, F1, Error);
                const std::wstring Name = F1.filename().wstring();
                const size_t Cut = Name.rfind(L'_');
                const unsigned long N = First && Cut != std::wstring::npos ? std::wcstoul(Name.c_str() + Cut + 1, nullptr, 10) : 0;
                const fs::path Taken = F1.parent_path() / (Name.substr(0, Cut + 1) + std::to_wstring(N + 1));
                const fs::path Taken2 = F1.parent_path() / (Name.substr(0, Cut + 1) + std::to_wstring(N + 2));
                fs::create_directories(Taken, Ec);
                fs::create_directories(Taken2, Ec);
                { std::ofstream(Taken / "keep.txt") << "mine"; }
                { std::ofstream(Taken2 / "gdt.db") << "mine"; }
                const bool Second = First && CopyForReading(Src / "gdt.db", C3, F3, Error);
                Check(Second && F3 != Taken && F3 != Taken2 && F3 != F1, "wal copy: an existing folder is skipped, not reused", F3.string());
                if (Second) RemoveCopy(F3, C3);
                Check(fs::exists(Taken / "keep.txt", Ec) && fs::file_size(Taken2 / "gdt.db", Ec) == 4, "wal copy: files in a taken folder are untouched", "");
                if (First) RemoveCopy(F1, C1);
                fs::remove_all(Taken, Ec);
                fs::remove_all(Taken2, Ec);
                // a folder with a foreign file in it is not removed along with the copy
                fs::path C4, F4;
                if (CopyForReading(Src / "gdt.db", C4, F4, Error))
                {
                    { std::ofstream(F4 / "foreign.txt") << "x"; }
                    RemoveCopy(F4, C4);
                    Check(fs::exists(F4 / "foreign.txt", Ec) && !fs::exists(C4, Ec), "wal copy: cleanup removes its own files and leaves a folder with others", F4.string());
                    fs::remove_all(F4, Ec);
                }
            }
            Check(!fs::exists(Src / "gdt.db-shm", Ec), "wal copy: no -shm beside the original", "");
            fs::path C2, F2;
            Check(!CopyForReading(Src / "absent.db", C2, F2, Error) && F2.empty(), "wal copy: a failed copy reports and leaves nothing", Error);
            fs::remove_all(Src, Ec);
        }

        Check(ImmutableUri("F:\\Steam Library\\gdtdb\\gdt.db") == "file:///F:/Steam%20Library/gdtdb/gdt.db?immutable=1", "immutable uri", ImmutableUri("F:\\Steam Library\\gdtdb\\gdt.db"));

        // relative gdt paths
        Check(StockIndex::RelativeGdtPath("F:\\Root\\Model_Export\\A\\b.gdt", "f:\\root") == "model_export\\a\\b.gdt", "path under the root is cut", StockIndex::RelativeGdtPath("F:\\Root\\Model_Export\\A\\b.gdt", "f:\\root"));
        Check(StockIndex::RelativeGdtPath("c:\\other\\machine\\source_data\\w.gdt", "f:\\root") == "source_data\\w.gdt", "foreign path cut at an anchor", "");
        Check(StockIndex::RelativeGdtPath("c:\\nowhere\\w.gdt", "f:\\root").empty(), "foreign path with no anchor is unplaced", "");
        Check(StockIndex::RelativeGdtPath("Texture_Assets/x.gdt", "") == "texture_assets\\x.gdt", "relative path kept", "");

        // stock rule: the gdt is not in a manifest and its file name has no hydrax_ / graywolf_ prefix;
        // its age does not decide, it is only counted (SavedLater)
        constexpr int64_t Day = 86400, Ref = 1'755'000'000;
        FakeFileTimes Files;
        Files.Times["model_export\\a\\stock.gdt"] = Ref - 3600;
        Files.Times["model_export\\a\\edge.gdt"] = Ref + Day;
        Files.Times["model_export\\a\\late.gdt"] = Ref + Day + 1;
        Files.Times["model_export\\a\\hydrax.gdt"] = Ref;
        Files.Times["model_export\\a\\hydrax_prefixed.gdt"] = Ref;
        Files.Times["model_export\\a\\graywolf_prefixed.gdt"] = Ref;
        Files.Times["texture_assets\\t7_wall.gdt"] = Ref + 5;
        Files.Times["texture_assets\\t7_metal.gdt"] = Ref + 40 * Day;   // a stock gdt saved weeks later
        std::unordered_set<std::string> Manifest{ "model_export\\a\\hydrax.gdt" };
        StockRules Rules;
        Rules.ReferenceSeconds = Ref;
        Rules.RootPrefix = "f:\\root";
        Rules.Manifests = &Manifest;
        const std::vector<Row> Rows = {
            { 10, "t7_prop_crate_a", "xmodel", "F:\\Root\\model_export\\a\\stock.gdt" },
            { 11, "t7_prop_crate_b", "xmodel", "F:\\Root\\model_export\\a\\stock.gdt" },
            { 12, "t7_prop_crate_c", "xmodel", "F:\\Root\\model_export\\a\\edge.gdt" },
            { 13, "t7_prop_crate_d", "xmodel", "F:\\Root\\model_export\\a\\late.gdt" },
            { 14, "t7_prop_crate_e", "xmodel", "F:\\Root\\model_export\\a\\hydrax.gdt" },
            { 15, "t7_prop_crate_f", "xmodel", "F:\\Root\\model_export\\a\\absent.gdt" },
            { 16, "t7_prop_crate_g", "xmodel", "" },
            { 17, "T7_Wall_Dirty", "material", "f:\\root\\texture_assets\\t7_wall.gdt" },
            { 18, "t7_prop_crate_h", "xmodel", "F:\\Root\\model_export\\a\\hydrax_prefixed.gdt" },
            { 19, "t7_prop_crate_i", "xmodel", "F:\\Root\\model_export\\a\\GrayWolf_prefixed.gdt" },
            { 20, "t7_decal_grunge_rust_offset_01", "material", "F:\\Root\\texture_assets\\t7_metal.gdt" },
            { 900, "t7_prop_crate_z", "xmodel", "F:\\Root\\model_export\\a\\stock.gdt" },
        };
        const StockIndex Stock(Rows, Rules, Files);
        Check(Stock.StockCount() == 8, "stock count", std::to_string(Stock.StockCount()));
        Check(Stock.IsStock(10) && Stock.IsStock(12) && Stock.IsStock(17) && Stock.IsStock(900), "inside the window is stock (edge included)", "");
        Check(Stock.IsStock(13), "a gdt saved a second past the old window is still stock", "");
        Check(Stock.IsStock(20), "t7_decal_grunge_rust_offset_01 in a stock gdt saved weeks later is stock", Stock.Explain(Rows[10]));
        Check(!Stock.IsStock(14), "a manifest gdt is not stock", "");
        Check(!Stock.IsStock(18) && !Stock.IsStock(19), "a hydrax_ or graywolf_ file name is not stock (case-insensitive)", "");
        Check(Stock.IsStock(15), "a gdt missing from the disk is still stock (not in a manifest, no tool prefix)", "");
        Check(!Stock.IsStock(16), "a row with no gdt path is not stock", "");
        Check(Stock.HighestStockId() == 900, "highest stock id", std::to_string(Stock.HighestStockId()));
        Check(Stock.InManifest == 1 && Stock.ToolPrefixed == 2 && Stock.NoGdtFile == 1 && Stock.StockGdts == 6,
            "stock rejection counts", std::format("{} {} {} {}", Stock.InManifest, Stock.ToolPrefixed, Stock.NoGdtFile, Stock.StockGdts));
        Check(Stock.SavedLater == 2 && Stock.FileMissing == 1, "report-only counts: saved later, file missing", std::format("{} {}", Stock.SavedLater, Stock.FileMissing));
        Check(Stock.IsStockGdtPath("model_export\\a\\stock.gdt") && Stock.IsStockGdtPath("Model_Export/A/Stock.gdt"), "stock gdt path lookup", "");
        Check(!Stock.IsStockGdtPath("model_export\\a\\graywolf_stock.gdt") && !Stock.IsStockGdtPath("model_export\\a\\hydrax.gdt"), "a tool gdt path is not a stock path", "");

        // Closest: stock rows only, longest token prefix, family, minimum two tokens
        StockMatch Match;
        Check(Stock.Closest("xmodel", "t7_prop_crate_q", Match) && Match.Folder == "model_export\\a" && Match.Tokens == 3 && Match.Entity == "t7_prop_crate_a",
            "closest by the longest prefix", Match.Folder + "/" + Match.Entity + "/" + std::to_string(Match.Tokens));
        Check(Match.Gdt == "model_export\\a\\stock.gdt", "closest names the busiest stock gdt", Match.Gdt);
        Check(Stock.Closest("material", "t7_wall_clean", Match) && Match.Folder == "texture_assets" && Match.Tokens == 2, "closest in the model family across gdfs", Match.Folder);
        Check(!Stock.Closest("xmodel", "t7_other", Match), "one shared token is not enough", "");
        Check(Stock.Closest("xmodel", "t7_prop_crate_d_only", Match) && Match.Tokens == 4, "closest past extra tokens takes the longest shared prefix (a gdt saved late is stock)", std::to_string(Match.Tokens));
        Check(Stock.Closest("xmodel", "t7_prop_crate_q_only", Match) && Match.Tokens == 3, "closest past extra tokens falls back to the shared prefix", std::to_string(Match.Tokens));
        Check(!Stock.Closest("bulletweapon", "t7_prop_crate_a", Match), "other family has no match", "");
        Check(!Stock.Closest("xmodel", "t7_prop_crate_h", Match) || Match.Gdt != "model_export\\a\\hydrax_prefixed.gdt", "a tool-prefixed gdt never feeds Closest", "");
        // leave one out: the only stock row of a prefix cannot predict itself
        const StockExclusion Self{ "texture_assets", "texture_assets\\t7_wall.gdt" };
        Check(!Stock.Closest("material", "t7_wall_dirty", Match, &Self), "leave-one-out: a prefix held only by the row itself finds nothing", "");
        Check(Stock.Closest("xmodel", "t7_prop_crate_a", Match, &Self) && Match.Tokens == 4, "leave-one-out elsewhere keeps the full-prefix match", std::to_string(Match.Tokens));
        return Failed;
    }
}
