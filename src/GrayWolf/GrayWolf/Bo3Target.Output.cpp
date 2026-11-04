#include "stdafx.h"

// See Bo3Target.Output.h.

#include "Bo3Target.Output.h"

#include "GdtWriter.h"

#include <nlohmann/json.hpp>

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <iterator>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace Bo3Target::Output
{
    // A path as UTF-8 text for a message. path::string() throws on characters outside the ANSI code page.
    static std::string Narrow(const fs::path& Path)
    {
        const std::wstring& Wide = Path.native();
        if (Wide.empty()) return {};
        const int Size = WideCharToMultiByte(CP_UTF8, 0, Wide.data(), static_cast<int>(Wide.size()), nullptr, 0, nullptr, nullptr);
        if (Size <= 0) return {};
        std::string Text(static_cast<size_t>(Size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, Wide.data(), static_cast<int>(Wide.size()), Text.data(), Size, nullptr, nullptr);
        return Text;
    }

    // ---- SHA-256 -------------------------------------------------------------------------------------

    namespace
    {
        std::string Hex(const uint8_t* Bytes, size_t Count)
        {
            static const char Digits[] = "0123456789abcdef";
            std::string Out;
            Out.reserve(Count * 2);
            for (size_t I = 0; I < Count; ++I)
            {
                Out += Digits[Bytes[I] >> 4];
                Out += Digits[Bytes[I] & 15];
            }
            return Out;
        }

        // One hash, fed in pieces.
        class Sha256
        {
        public:
            Sha256()
            {
                if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
                {
                    Algorithm = nullptr;
                    return;
                }
                if (BCryptCreateHash(Algorithm, &Handle, nullptr, 0, nullptr, 0, 0) != 0) Handle = nullptr;
            }
            ~Sha256()
            {
                if (Handle != nullptr) BCryptDestroyHash(Handle);
                if (Algorithm != nullptr) BCryptCloseAlgorithmProvider(Algorithm, 0);
            }
            Sha256(const Sha256&) = delete;
            Sha256& operator=(const Sha256&) = delete;
            bool Ok() const { return Handle != nullptr; }
            void Add(const void* Data, size_t Size)
            {
                // BCryptHashData takes a ULONG
                const uint8_t* Bytes = static_cast<const uint8_t*>(Data);
                while (Ok() && Size > 0)
                {
                    const ULONG Piece = static_cast<ULONG>((std::min<size_t>)(Size, 1u << 30));
                    if (BCryptHashData(Handle, const_cast<PUCHAR>(Bytes), Piece, 0) != 0) Failed = true;
                    Bytes += Piece;
                    Size -= Piece;
                }
            }
            std::string Finish()
            {
                if (!Ok() || Failed) return {};
                uint8_t Digest[32];
                if (BCryptFinishHash(Handle, Digest, sizeof(Digest), 0) != 0) return {};
                return Hex(Digest, sizeof(Digest));
            }

        private:
            BCRYPT_ALG_HANDLE Algorithm = nullptr;
            BCRYPT_HASH_HANDLE Handle = nullptr;
            bool Failed = false;
        };
    }

    std::string Sha256Hex(const void* Data, size_t Size)
    {
        Sha256 Hash;
        Hash.Add(Data, Size);
        return Hash.Finish();
    }

    bool Sha256File(const fs::path& File, std::string& Out)
    {
        Out.clear();
        std::ifstream In(File, std::ios::binary);
        if (!In) return false;
        Sha256 Hash;
        std::vector<char> Buffer(1 << 20);
        while (In)
        {
            In.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
            const std::streamsize Got = In.gcount();
            if (Got > 0) Hash.Add(Buffer.data(), static_cast<size_t>(Got));
        }
        if (!In.eof()) return false;
        Out = Hash.Finish();
        return !Out.empty();
    }

    // ---- paths ---------------------------------------------------------------------------------------

    std::string PathKey(std::string_view RelativePath)
    {
        std::string Out(RelativePath);
        for (char& C : Out)
        {
            if (C == '/') C = '\\';
            else if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
        }
        while (!Out.empty() && (Out.front() == '\\' || Out.front() == ' ')) Out.erase(Out.begin());
        while (!Out.empty() && (Out.back() == '\\' || Out.back() == ' ')) Out.pop_back();
        return Out;
    }

    // ---- the manifest ----------------------------------------------------------------------------------

    Status Manifest::Load(const fs::path& File)
    {
        Entries.clear();
        Dirty = false;
        std::error_code Ec;
        if (!fs::exists(File, Ec)) return Status::Good();
        try
        {
            std::ifstream In(File, std::ios::binary);
            if (!In) return Status::Fail("manifest cannot be opened: " + Narrow(File));
            const nlohmann::json Doc = nlohmann::json::parse(In);
            const auto Version = Doc.find("version");
            if (Version == Doc.end() || !Version->is_number_integer() || Version->get<int>() != 1)
                return Status::Fail("manifest version is not 1: " + Narrow(File));
            const auto Files = Doc.find("files");
            if (Files == Doc.end() || !Files->is_array()) return Status::Fail("manifest has no files array: " + Narrow(File));
            std::map<std::string, ManifestEntry> Read;
            for (const auto& Item : *Files)
            {
                const auto Path = Item.find("path");
                if (!Item.is_object() || Path == Item.end() || !Path->is_string() || Path->get<std::string>().empty())
                    return Status::Fail("manifest has an entry without a path: " + Narrow(File));
                const auto Text = [&](const char* Key) {
                    const auto It = Item.find(Key);
                    return It != Item.end() && It->is_string() ? It->get<std::string>() : std::string();
                };
                ManifestEntry E;
                E.Path = Path->get<std::string>();
                E.Sha256 = Text("sha256");
                E.Title = Text("title");
                E.AssetType = Text("assetType");
                E.AssetName = Text("assetName");
                E.WrittenAt = Text("writtenAt");
                Read[PathKey(E.Path)] = std::move(E);
            }
            Entries = std::move(Read);
            return Status::Good();
        }
        catch (const std::exception& Error)   // boundary: nothing may escape this module
        {
            return Status::Fail(std::string("manifest unreadable: ") + Narrow(File) + ": " + Error.what());
        }
    }

    const ManifestEntry* Manifest::Find(std::string_view RelativePath) const
    {
        const auto It = Entries.find(PathKey(RelativePath));
        return It == Entries.end() ? nullptr : &It->second;
    }

    void Manifest::Record(ManifestEntry Entry)
    {
        while (!Entry.Path.empty() && Entry.Path.front() == '\\') Entry.Path.erase(Entry.Path.begin());
        for (char& C : Entry.Path)
        {
            if (C == '/') C = '\\';
        }
        Entries[PathKey(Entry.Path)] = std::move(Entry);
        Dirty = true;
    }

    namespace
    {
        // System.Text.Json's escaping as HydraX writes it: quote, backslash, control characters, and "+" as +
        std::string JsonText(std::string_view Text)
        {
            std::string Out = "\"";
            for (const unsigned char C : Text)
            {
                switch (C)
                {
                case '"': Out += "\\\""; break;
                case '\\': Out += "\\\\"; break;
                case '+': Out += "\\u002B"; break;
                case '\n': Out += "\\n"; break;
                case '\r': Out += "\\r"; break;
                case '\t': Out += "\\t"; break;
                default:
                    if (C < 0x20) Out += std::format("\\u{:04X}", static_cast<unsigned>(C));
                    else Out += static_cast<char>(C);
                }
            }
            return Out + "\"";
        }
    }

    std::string Manifest::ToText() const
    {
        std::string Out = "{\n  \"version\": 1,\n  \"files\": [";
        bool First = true;
        for (const auto& [Key, E] : Entries)
        {
            Out += First ? "\n" : ",\n";
            First = false;
            Out += "    {\n";
            Out += "      \"path\": " + JsonText(E.Path) + ",\n";
            Out += "      \"sha256\": " + JsonText(E.Sha256) + ",\n";
            Out += "      \"title\": " + JsonText(E.Title) + ",\n";
            Out += "      \"assetType\": " + JsonText(E.AssetType) + ",\n";
            Out += "      \"assetName\": " + JsonText(E.AssetName) + ",\n";
            Out += "      \"writtenAt\": " + JsonText(E.WrittenAt) + "\n";
            Out += "    }";
        }
        Out += First ? "]\n}" : "\n  ]\n}";
        return Out;
    }

    Status Manifest::Save(const fs::path& File) const
    {
        const std::string Text = ToText();
        std::string Error;
        if (!WriteAtomic(File, Text.data(), Text.size(), Error)) return Status::Fail("manifest not written: " + Error);
        return Status::Good();
    }

    // ---- atomic writes ----------------------------------------------------------------------------------

    bool WriteAtomic(const fs::path& File, const void* Data, size_t Size, std::string& Error, bool Replace)
    {
        static std::atomic<unsigned> Counter{ 0 };
        std::error_code Ec;
        fs::create_directories(File.parent_path(), Ec);
        if (Ec)
        {
            Error = "cannot create " + Narrow(File.parent_path()) + ": " + Ec.message();
            return false;
        }
        // wide all the way: a path with characters outside the ANSI code page must not be narrowed
        const fs::path Temp = File.native() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(Counter.fetch_add(1)) + L".tmp";
        {
            std::ofstream Out(Temp, std::ios::binary | std::ios::trunc);
            if (!Out)
            {
                Error = "cannot open " + Narrow(Temp);
                return false;
            }
            Out.write(static_cast<const char*>(Data), static_cast<std::streamsize>(Size));
            Out.flush();
            if (!Out)
            {
                Out.close();
                fs::remove(Temp, Ec);
                Error = "cannot write " + Narrow(Temp);
                return false;
            }
        }
        // Create (Replace false): no MOVEFILE_REPLACE_EXISTING, so a file that appeared since the check wins
        const DWORD Flags = MOVEFILE_WRITE_THROUGH | (Replace ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(Temp.c_str(), File.c_str(), Flags))
        {
            const DWORD Code = GetLastError();
            Error = std::format("cannot rename {} over {} (error {}{})", Narrow(Temp), Narrow(File), Code,
                                !Replace && (Code == ERROR_ALREADY_EXISTS || Code == ERROR_FILE_EXISTS) ? ", the file appeared since it was checked" : "");
            fs::remove(Temp, Ec);
            return false;
        }
        return true;
    }

    fs::path FreeBackupPath(const fs::path& File)
    {
        std::error_code Ec;
        fs::path Candidate = File.native() + L".bak";
        for (int N = 1; fs::exists(Candidate, Ec); ++N) Candidate = File.native() + L".bak." + std::to_wstring(N);
        return Candidate;
    }

    // ---- the never-overwrite rule ------------------------------------------------------------------------------

    const char* VerdictName(Verdict V)
    {
        switch (V)
        {
        case Verdict::Create: return "create";
        case Verdict::Replace: return "replace";
        case Verdict::Unchanged: return "unchanged";
        case Verdict::NotListed: return "foreign_file";
        case Verdict::Modified: return "modified_since_write";
        case Verdict::Duplicate: return "duplicate_output";
        }
        return "unknown";
    }

    fs::path Gate::Absolute(std::string_view RelativePath) const
    {
        std::string Text(RelativePath);
        for (char& C : Text)
        {
            if (C == '\\') C = '/';
        }
        return RootFolder / fs::path(Text);
    }

    Verdict Gate::Check(std::string_view RelativePath, const std::string& NewHash)
    {
        const std::string Key = PathKey(RelativePath);
        if (const auto It = Claimed.find(Key); It != Claimed.end()) return It->second == NewHash ? Verdict::Unchanged : Verdict::Duplicate;
        const fs::path File = Absolute(RelativePath);
        std::error_code Ec;
        if (!fs::exists(File, Ec)) return Verdict::Create;
        const ManifestEntry* Listed = Owned.Find(RelativePath);
        if (Listed == nullptr || !fs::is_regular_file(File, Ec)) return Verdict::NotListed;
        std::string OnDisk;
        if (!Sha256File(File, OnDisk)) return Verdict::NotListed;
        if (OnDisk != Listed->Sha256) return Verdict::Modified;
        return OnDisk == NewHash ? Verdict::Unchanged : Verdict::Replace;
    }

    void Gate::Claim(std::string_view RelativePath, const std::string& NewHash)
    {
        Claimed[PathKey(RelativePath)] = NewHash;
    }

    // ---- GDT text -------------------------------------------------------------------------------------------

    namespace
    {
        struct Cursor
        {
            std::string_view Text;
            size_t Pos = 0;
            void Skip()
            {
                while (Pos < Text.size() && (Text[Pos] == ' ' || Text[Pos] == '\t' || Text[Pos] == '\r' || Text[Pos] == '\n')) ++Pos;
                // a UTF-8 byte order mark before the first brace
                if (Pos == 0 && Text.size() >= 3 && Text.substr(0, 3) == "\xEF\xBB\xBF")
                {
                    Pos = 3;
                    Skip();
                }
            }
            bool At(char C) const { return Pos < Text.size() && Text[Pos] == C; }
            bool Take(char C)
            {
                if (!At(C)) return false;
                ++Pos;
                return true;
            }
            // a quoted string, no escapes (the Mod Tools write none); the text between the quotes
            bool Quoted(std::string& Out)
            {
                if (!Take('"')) return false;
                const size_t End = Text.find('"', Pos);
                if (End == std::string_view::npos) return false;
                Out.assign(Text.substr(Pos, End - Pos));
                Pos = End + 1;
                return true;
            }
        };
    }

    bool ParseGdt(std::string_view Text, std::vector<GdtEntryText>& Entries, std::string& Error)
    {
        Entries.clear();
        Cursor C{ Text };
        C.Skip();
        const auto Fail = [&](const char* Why) {
            Error = std::format("{} at byte {}", Why, C.Pos);
            Entries.clear();
            return false;
        };
        if (!C.Take('{')) return Fail("no opening brace");
        for (;;)
        {
            C.Skip();
            if (C.Take('}')) break;
            GdtEntryText E;
            const size_t Start = C.Pos;
            if (!C.Quoted(E.Name)) return Fail("an entry has no quoted name");
            C.Skip();
            char Close = ')';
            std::string Prefix;
            if (C.Take('(')) Close = ')';
            else if (C.Take('[')) { Close = ']'; Prefix = "["; }
            else return Fail("an entry has no ( or [");
            C.Skip();
            if (!C.Quoted(E.Gdf)) return Fail("an entry has no quoted gdf");
            E.Gdf = Prefix + E.Gdf;
            C.Skip();
            if (!C.Take(Close)) return Fail("an entry's gdf is not closed");
            C.Skip();
            if (!C.Take('{')) return Fail("an entry has no body");
            for (;;)
            {
                C.Skip();
                if (C.Take('}')) break;
                std::string Key, Value;
                if (!C.Quoted(Key)) return Fail("a property has no quoted key");
                C.Skip();
                if (!C.Quoted(Value)) return Fail("a property has no quoted value");
            }
            E.Text.assign(Text.substr(Start, C.Pos - Start));
            Entries.push_back(std::move(E));
        }
        C.Skip();
        if (C.Pos != Text.size()) return Fail("text after the closing brace");
        return true;
    }

    std::string RenderGdt(std::vector<GdtEntryText> Entries)
    {
        std::stable_sort(Entries.begin(), Entries.end(), [](const GdtEntryText& A, const GdtEntryText& B) { return A.Name < B.Name; });
        std::string Out = "{\r\n";
        for (const GdtEntryText& E : Entries) Out += "\t" + E.Text + "\r\n";
        return Out + "}\r\n";
    }

    GdtEntryText EntryOf(const std::string& Name, const std::string& Gdf, const std::vector<std::pair<std::string, std::string>>& Fields)
    {
        std::string Text = Gdt::Entry(Name, Gdf, Fields, "\r\n");
        // Gdt::Entry is "\t" + entry + "\r\n"
        if (Text.size() >= 3) Text = Text.substr(1, Text.size() - 3);
        return { Name, Gdf, std::move(Text) };
    }

    std::vector<GdtEntryText> MergeGdt(const std::vector<GdtEntryText>& Old, const std::vector<GdtEntryText>& Incoming, bool Overwrite,
                                       std::vector<GdtMergeNote>* Notes)
    {
        const auto Lower = [](std::string Text) {
            for (char& C : Text)
            {
                if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
            }
            return Text;
        };
        // A name is unique in one file, case aside, whatever its gdf: the first incoming entry of a name stays,
        // a later one of the same name is dropped and reported (never both, never silently the last)
        std::map<std::string, GdtEntryText> New;
        for (const GdtEntryText& E : Incoming)
        {
            const auto [It, Fresh] = New.emplace(Lower(E.Name), E);
            if (!Fresh && Notes != nullptr)
                Notes->push_back({ "gdt_duplicate_name", E.Name, It->second.Gdf, E.Gdf });
        }
        std::vector<GdtEntryText> Out;
        if (!Overwrite)
        {
            for (const GdtEntryText& E : Old)
            {
                const auto Hit = New.find(Lower(E.Name));
                if (Hit == New.end())
                {
                    Out.push_back(E);
                    continue;
                }
                // replaced by the incoming entry; one of another gdf type is a loss to say out loud
                if (Hit->second.Gdf != E.Gdf && Notes != nullptr)
                    Notes->push_back({ "gdt_replaced_other_type", E.Name, Hit->second.Gdf, E.Gdf });
            }
        }
        for (auto& [Key, E] : New) Out.push_back(std::move(E));
        return Out;
    }

    // ---- the install guard -------------------------------------------------------------------------------------

    bool IsInstall(const fs::path& Folder)
    {
        std::error_code Ec;
        if (fs::exists(Folder / "BlackOps3.exe", Ec)) return true;
        if (fs::exists(Folder / "bin" / "linker_modtools.exe", Ec)) return true;
        if (fs::is_directory(Folder / "share" / "raw", Ec)) return true;
        return fs::is_directory(Folder / "map_source", Ec) && fs::is_directory(Folder / "source_data", Ec);
    }

    bool Touches(const fs::path& Path, fs::path* Found)
    {
        std::error_code Ec;
        const fs::path Absolute = fs::absolute(Path, Ec);
        if (Ec) return true;    // cannot tell where it is: refuse
        const fs::path Here = fs::weakly_canonical(Absolute, Ec);
        if (Ec) return true;
        for (fs::path Up = Here; !Up.empty(); Up = Up.parent_path())
        {
            if (IsInstall(Up))
            {
                if (Found != nullptr) *Found = Up;
                return true;
            }
            if (Up == Up.parent_path()) break;
        }
        if (fs::is_directory(Here, Ec))
        {
            for (fs::directory_iterator It(Here, Ec), End; !Ec && It != End; It.increment(Ec))
            {
                std::error_code Ec2;
                if (It->is_directory(Ec2) && IsInstall(It->path()))
                {
                    if (Found != nullptr) *Found = It->path();
                    return true;
                }
            }
        }
        return false;
    }

    bool IsUnder(const fs::path& Root, const fs::path& File)
    {
        std::error_code Ec;
        const fs::path R = fs::weakly_canonical(fs::absolute(Root, Ec), Ec);
        if (Ec) return false;
        const fs::path F = fs::weakly_canonical(fs::absolute(File, Ec), Ec);
        if (Ec) return false;
        const fs::path Rel = F.lexically_relative(R);
        if (Rel.empty()) return false;
        const fs::path First = *Rel.begin();
        return First != L".." && !Rel.is_absolute();
    }

    // ---- self-test ----------------------------------------------------------------------------------------------

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL Output {} (got '{}')\n", Name, Got);
        };

        // SHA-256 known answers
        Check(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc", Sha256Hex("abc"));
        Check(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing", Sha256Hex(""));

        std::error_code Ec;
        const fs::path Dir = fs::temp_directory_path() / std::format("graywolf_selftest_output_{}", static_cast<unsigned long long>(GetCurrentProcessId()));
        fs::remove_all(Dir, Ec);
        fs::create_directories(Dir, Ec);

        // paths
        Check(PathKey("Model_Export/Foo\\Bar.GDT ") == "model_export\\foo\\bar.gdt", "path key", PathKey("Model_Export/Foo\\Bar.GDT "));

        // atomic write: content in place, no temp left, replacing works
        {
            std::string Error;
            const fs::path File = Dir / "a" / "b.txt";
            Check(WriteAtomic(File, "one", 3, Error) && WriteAtomic(File, "three", 5, Error), "atomic write twice", Error);
            std::string Hash;
            Check(Sha256File(File, Hash) && Hash == Sha256Hex("three"), "atomic write replaces", Hash);
            size_t Files = 0;
            for (const auto& E : fs::directory_iterator(Dir / "a", Ec)) { (void)E; ++Files; }
            Check(Files == 1, "atomic write leaves no temp file", std::to_string(Files));
            Check(FreeBackupPath(File) == File.string() + ".bak", "backup name", FreeBackupPath(File).string());
            { std::ofstream(File.string() + ".bak") << "x"; }
            Check(FreeBackupPath(File) == File.string() + ".bak.1", "second backup name", FreeBackupPath(File).string());
        }

        // manifest: save, load, the HydraX shape, an unreadable one fails
        {
            Manifest M;
            ManifestEntry E{ "Art_Assets\\X\\A.gdt", "ab", "BlackOps4", "material", "mtl_a", "2026-11-04T10:00:00.0000000+00:00" };
            M.Record(E);
            const fs::path File = Dir / "manifest.json";
            Check(M.Save(File).Ok, "manifest saves", "");
            std::ifstream In(File, std::ios::binary);
            const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
            Check(Text.find("\"version\": 1") != std::string::npos && Text.find("\\u002B00:00") != std::string::npos &&
                  Text.find("\"path\": \"Art_Assets\\\\X\\\\A.gdt\"") != std::string::npos, "manifest uses HydraX's schema and escapes", Text);
            Manifest Back;
            const Status S = Back.Load(File);
            Check(S.Ok && Back.Size() == 1 && Back.Find("art_assets/x/a.GDT") != nullptr && Back.Find("art_assets\\x\\a.gdt")->WrittenAt == E.WrittenAt,
                  "manifest round trip, lower-cased keys", S.Reason);
            { std::ofstream(Dir / "bad.json") << "{ not json"; }
            Check(!Manifest().Load(Dir / "bad.json").Ok, "an unreadable manifest is a failure", "");
            { std::ofstream(Dir / "v2.json") << "{\"version\":2,\"files\":[]}"; }
            Check(!Manifest().Load(Dir / "v2.json").Ok, "a manifest of another version is a failure", "");
            { std::ofstream(Dir / "nop.json") << "{\"version\":1,\"files\":[{\"sha256\":\"x\"}]}"; }
            Check(!Manifest().Load(Dir / "nop.json").Ok, "a manifest entry without a path is a failure", "");
            Check(Manifest().Load(Dir / "missing.json").Ok, "a missing manifest is empty", "");
        }

        // the gate: create, foreign, replace, unchanged, modified, duplicate
        {
            Manifest M;
            Gate G(Dir / "root", M);
            std::error_code E2;
            fs::create_directories(Dir / "root" / "x", E2);
            const std::string H1 = Sha256Hex("one"), H2 = Sha256Hex("two");
            Check(G.Check("x\\new.tif", H1) == Verdict::Create, "gate: a new file is created", "");
            { std::ofstream(Dir / "root" / "x" / "mine.tif", std::ios::binary) << "one"; }
            Check(G.Check("x\\mine.tif", H1) == Verdict::NotListed, "gate: an unlisted file is foreign even with the same bytes", "");
            M.Record({ "x\\mine.tif", H1, "T", "image", "i", "t" });
            Check(G.Check("x\\mine.tif", H1) == Verdict::Unchanged, "gate: a listed file with the same bytes is unchanged", "");
            Check(G.Check("x\\mine.tif", H2) == Verdict::Replace, "gate: a listed, untouched file may change", "");
            { std::ofstream(Dir / "root" / "x" / "mine.tif", std::ios::binary) << "edited"; }
            Check(G.Check("x\\mine.tif", H2) == Verdict::Modified, "gate: a hash mismatch is modified_since_write", "");
            G.Claim("x\\new.tif", H1);
            Check(G.Check("X/NEW.tif", H1) == Verdict::Unchanged && G.Check("x\\new.tif", H2) == Verdict::Duplicate, "gate: two assets, one path", "");
        }

        // GDT text: parse, byte-exact carry, merge, ordering, unparsable
        {
            const std::string Old = "{\r\n\t\"b_only_old\" ( \"material.gdf\" )\r\n\t{\r\n\t\t\"k\" \"v } \\\\ {\"\r\n\t}\r\n"
                                    "\t\"a_both\" ( \"image.gdf\" )\r\n\t{\r\n\t\t\"baseImage\" \"old\"\r\n\t}\r\n"
                                    "\t\"child\" [ \"b_only_old\" ]\r\n\t{\r\n\t}\r\n}\r\n";
            std::vector<GdtEntryText> Entries;
            std::string Error;
            Check(ParseGdt(Old, Entries, Error) && Entries.size() == 3, "gdt parse", Error);
            Check(Entries.size() == 3 && Entries[2].Gdf == "[b_only_old" && Entries[0].Text.find("v } ") != std::string::npos, "gdt parse: a bracket parent, braces inside quotes", "");
            const std::vector<GdtEntryText> Incoming = { EntryOf("A_Both", "image.gdf", { { "baseImage", "new" } }), EntryOf("c_new", "image.gdf", {}) };
            const std::vector<GdtEntryText> Merged = MergeGdt(Entries, Incoming, false);
            const std::string Text = RenderGdt(Merged);
            std::vector<GdtEntryText> Back;
            Check(ParseGdt(Text, Back, Error) && Back.size() == 4, "merge: parses back with four entries", Error + Text);
            Check(Text.find("\"k\" \"v } \\\\ {\"") != std::string::npos && Text.find("\"baseImage\" \"new\"") != std::string::npos &&
                  Text.find("\"baseImage\" \"old\"") == std::string::npos, "merge: the old-only entry kept byte for byte, the shared one replaced", Text);
            Check(Back.size() == 4 && Back[0].Name == "A_Both" && Back[1].Name == "b_only_old" && Back[2].Name == "c_new" && Back[3].Name == "child", "merge: ordinal name order", Back.empty() ? "" : Back[0].Name);
            const std::vector<GdtEntryText> Over = MergeGdt(Entries, Incoming, true);
            Check(Over.size() == 2, "overwrite keeps only the incoming entries", std::to_string(Over.size()));
            Check(RenderGdt(Merged) == RenderGdt(MergeGdt(Back, Incoming, false)), "merge twice gives the same bytes", "");
            // one name, two gdf types: the first incoming one stays and the other is reported, whatever the case
            {
                std::vector<GdtMergeNote> Notes;
                const std::vector<GdtEntryText> Twice = { EntryOf("Same_Name", "xmodel.gdf", {}), EntryOf("same_name", "material.gdf", {}), EntryOf("other", "image.gdf", {}) };
                const std::vector<GdtEntryText> Kept = MergeGdt({}, Twice, false, &Notes);
                Check(Kept.size() == 2 && Kept[0].Name == "other" && Kept[1].Name == "Same_Name" && Kept[1].Gdf == "xmodel.gdf", "merge: one name of two types keeps the first", std::to_string(Kept.size()));
                Check(Notes.size() == 1 && Notes[0].Code == "gdt_duplicate_name" && Notes[0].KeptGdf == "xmodel.gdf" && Notes[0].OtherGdf == "material.gdf", "merge: the duplicate is reported naming both types", Notes.empty() ? "" : Notes[0].Code);
                Check(RenderGdt(Kept) == RenderGdt(MergeGdt({}, Twice, false, nullptr)), "merge: the same result without notes", "");
                // an old entry of another gdf type replaced by an incoming one of the name: reported
                Notes.clear();
                const std::vector<GdtEntryText> OldMaterial = { EntryOf("Thing", "material.gdf", {}), EntryOf("kept_old", "image.gdf", {}) };
                const std::vector<GdtEntryText> NewModel = { EntryOf("thing", "xmodel.gdf", {}) };
                const std::vector<GdtEntryText> Swapped = MergeGdt(OldMaterial, NewModel, false, &Notes);
                Check(Swapped.size() == 2 && Notes.size() == 1 && Notes[0].Code == "gdt_replaced_other_type" && Notes[0].KeptGdf == "xmodel.gdf" && Notes[0].OtherGdf == "material.gdf",
                      "merge: replacing an entry of another gdf type is reported", Notes.empty() ? "" : Notes[0].Code);
                Notes.clear();
                MergeGdt({ EntryOf("a", "image.gdf", {}) }, { EntryOf("A", "image.gdf", {}) }, false, &Notes);
                Check(Notes.empty(), "merge: replacing an entry of the same type is quiet", std::to_string(Notes.size()));
            }
            Check(!ParseGdt("{ \"a\" ( \"x.gdf\" ) { \"k\" ", Back, Error), "an unterminated gdt does not parse", "");
            Check(!ParseGdt("garbage", Back, Error), "garbage does not parse", "");
            Check(ParseGdt("{\r\n}\r\n", Back, Error) && Back.empty(), "an empty gdt parses", Error);
        }

        // the install guard
        {
            const fs::path Fake = Dir / "fake_install";
            fs::create_directories(Fake / "bin", Ec);
            Check(!IsInstall(Fake), "guard: an empty folder is no install", "");
            { std::ofstream(Fake / "bin" / "linker_modtools.exe") << "x"; }
            Check(IsInstall(Fake), "guard: linker_modtools.exe marks an install", "");
            fs::create_directories(Fake / "out" / "deeper", Ec);
            fs::path Found;
            Check(Touches(Fake / "out" / "deeper", &Found) && Found == fs::weakly_canonical(Fake), "guard: an output below an install is refused", Found.string());
            Check(Touches(Dir, &Found), "guard: a parent of an install is refused (a child that is one)", Found.string());
            const fs::path Plain = Dir / "plain" / "sub";
            fs::create_directories(Plain, Ec);
            Check(!Touches(Plain), "guard: a plain folder is fine", "");
            fs::create_directories(Dir / "pair" / "map_source", Ec);
            fs::create_directories(Dir / "pair" / "source_data", Ec);
            Check(IsInstall(Dir / "pair"), "guard: map_source with source_data marks an install", "");
            fs::create_directories(Dir / "raw" / "share" / "raw", Ec);
            Check(IsInstall(Dir / "raw"), "guard: share\\raw marks an install", "");
        }

        // wide paths: a folder name outside the ANSI code page, a backup beside it
        {
            std::string Error;
            const fs::path Wide = Dir / (std::wstring(L"w\u00e9ird_\u4e2d\u6587_\u0416") ) / "f.txt";
            Check(WriteAtomic(Wide, "wide", 4, Error) && fs::exists(Wide, Ec), "wide path: written", Error);
            std::string Hash;
            Check(Sha256File(Wide, Hash) && Hash == Sha256Hex("wide"), "wide path: content in place", Hash);
            size_t Files = 0;
            for (const auto& E : fs::directory_iterator(Wide.parent_path(), Ec)) { (void)E; ++Files; }
            Check(Files == 1, "wide path: no temp file left", std::to_string(Files));
            const fs::path Backup = FreeBackupPath(Wide);
            Check(Backup.native() == Wide.native() + L".bak", "wide path: backup name keeps the characters", Narrow(Backup));
            { std::ofstream(Backup, std::ios::binary) << "x"; }
            Check(FreeBackupPath(Wide).native() == Wide.native() + L".bak.1", "wide path: second backup name", Narrow(FreeBackupPath(Wide)));
        }

        // Create: a file that is there at the move is not replaced
        {
            std::string Error;
            const auto Slurp = [](const fs::path& F) { std::ifstream In(F, std::ios::binary); return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>()); };
            const fs::path File = Dir / "create" / "c.txt";
            Check(WriteAtomic(File, "first", 5, Error, false), "create: a new file is written", Error);
            Check(!WriteAtomic(File, "second", 6, Error, false) && Slurp(File) == "first", "create: an existing file is not replaced", Error);
            size_t Files = 0;
            for (const auto& E : fs::directory_iterator(File.parent_path(), Ec)) { (void)E; ++Files; }
            Check(Files == 1, "create: a refused write leaves no temp file", std::to_string(Files));
            Check(WriteAtomic(File, "third", 5, Error, true) && Slurp(File) == "third", "create: Replace still replaces", Error);
        }

        // IsUnder, and Touches refusing what it cannot tell
        {
            const fs::path Root = Dir / "run";
            fs::create_directories(Root / "a", Ec);
            Check(IsUnder(Root, Root / "a" / "x.png") && IsUnder(Root, Root / "a" / ".." / "b" / "x.png"), "under: files inside", "");
            Check(!IsUnder(Root, Root / ".." / "x.png") && !IsUnder(Root, Dir / "x.png") && !IsUnder(Root, Root / "a" / ".." / ".." / "x.png"), "under: ../ out of the root is outside", "");
            Check(!IsUnder(Root, Root.root_path() / "elsewhere" / "x.png") && !IsUnder(Root, Root.parent_path() / "run2" / "x.png"), "under: an absolute path and a sibling with the same prefix are outside", "");
            Check(!IsUnder(Root, fs::path()), "under: an empty path is outside", "");
        }

        fs::remove_all(Dir, Ec);
        return Failed;
    }
}
