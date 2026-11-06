#include "stdafx.h"

#include "ImGuiShell.TextView.h"
#include "ImGuiTheme.h"
#include "Strings.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <set>
#include <string_view>

namespace ShellUi
{
    namespace
    {
        constexpr size_t MaxBytes = 4u * 1024 * 1024;   // read; a larger file is cut and says so
        constexpr size_t MaxDrawnChars = 2000;           // a line longer than this is drawn cut

        bool IsIdentStart(char C) { return std::isalpha((unsigned char)C) || C == '_'; }
        bool IsIdent(char C) { return std::isalnum((unsigned char)C) || C == '_'; }

        const std::set<std::string_view>& Keywords(TextViewer::Lang L)
        {
            static const std::set<std::string_view>& Gsc = *new std::set<std::string_view> { "if", "else", "for", "foreach", "while", "switch", "case", "default", "break", "continue",
                "return", "wait", "thread", "function", "class", "self", "level", "game", "anim", "true", "false", "undefined", "in", "do", "waittill",
                "endon", "notify", "waitframe", "new", "var", "const", "private", "autoexec", "event_handler", "namespace" };
            static const std::set<std::string_view>& Lua = *new std::set<std::string_view> { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
                "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };
            static const std::set<std::string_view>& Json = *new std::set<std::string_view> { "true", "false", "null" };
            static const std::set<std::string_view>& None = *new std::set<std::string_view>;   // never destroyed: the worker may still read them at exit
            switch (L)
            {
            case TextViewer::Lang::Gsc: return Gsc;
            case TextViewer::Lang::Lua: return Lua;
            case TextViewer::Lang::Json: return Json;
            default: return None;
            }
        }

        // a new run only when the class changes
        void Push(std::vector<TextViewer::Seg>& Out, size_t At, uint8_t Class)
        {
            if (Out.empty() || Out.back().Class != Class) Out.push_back({ (uint32_t)At, Class });
        }

        // the line trimmed of its line end
        std::string_view Trimmed(std::string_view Text, size_t From, size_t To)
        {
            std::string_view Line = Text.substr(From, To - From);
            while (!Line.empty() && (Line.back() == '\n' || Line.back() == '\r')) Line.remove_suffix(1);
            return Line;
        }
    }

    TextViewer::Lang TextViewer::LangOf(const std::string& Extension)
    {
        std::string E = Extension;
        for (char& C : E) C = (char)std::tolower((unsigned char)C);
        if (E == "gsc" || E == "csc" || E == "gsh") return Lang::Gsc;
        if (E == "lua") return Lang::Lua;
        if (E == "ini" || E == "cfg" || E == "conf") return Lang::Ini;
        if (E == "json") return Lang::Json;
        if (E == "xml" || E == "html" || E == "xsd") return Lang::Xml;
        if (E == "csv" || E == "tsv") return Lang::Csv;
        return Lang::Plain;
    }

    void TextViewer::Tokenise(Lang L, std::string_view Line, bool& InBlock, std::vector<Seg>& Out)
    {
        Out.clear();
        size_t P = 0;
        const size_t N = Line.size();
        const bool Slash = L == Lang::Gsc || L == Lang::Json;   // // and /* */
        const bool Dash = L == Lang::Lua;                       // -- and --[[ ]]
        const bool Xml = L == Lang::Xml;                        // <!-- --> and <tags>
        const auto& Words = Keywords(L);
        const auto At = [&](std::string_view S) { return Line.substr(P, S.size()) == S; };

        if (L == Lang::Ini)
        {
            size_t F = 0;
            while (F < N && (Line[F] == ' ' || Line[F] == '\t')) F++;
            if (F < N && Line[F] == '[') { Push(Out, 0, Key); return; }
            if (F < N && (Line[F] == ';' || Line[F] == '#')) { Push(Out, 0, Comment); return; }
            if (const size_t Eq = Line.find('='); Eq != std::string_view::npos)
            {
                Push(Out, 0, Key);
                Push(Out, Eq, Normal);
                P = Eq;
            }
        }
        while (P < N)
        {
            if (InBlock)
            {
                Push(Out, P, Comment);
                const std::string_view Close = Xml ? "-->" : Dash ? "]]" : "*/";
                const size_t Found = Line.find(Close, P);
                if (Found == std::string_view::npos) return;
                P = Found + Close.size();
                InBlock = false;
                continue;
            }
            if ((Slash && At("/*")) || (Dash && At("--[[")) || (Xml && At("<!--")))
            {
                InBlock = true;
                Push(Out, P, Comment);
                P += Xml || Dash ? 4 : 2;
                continue;
            }
            if ((Slash && At("//")) || (Dash && At("--")))
            {
                Push(Out, P, Comment);
                return;
            }
            const char C = Line[P];
            if (Xml)
            {
                if (C != '<') { Push(Out, P, Normal); P++; continue; }
                Push(Out, P, Keyword);
                while (P < N && Line[P] != '>')
                {
                    if (Line[P] == '"' || Line[P] == '\'')
                    {
                        const char Q = Line[P];
                        Push(Out, P, String);
                        const size_t Close = Line.find(Q, P + 1);
                        P = Close == std::string_view::npos ? N : Close + 1;
                        Push(Out, P, Keyword);
                        continue;
                    }
                    P++;
                }
                if (P < N) P++;
                continue;
            }
            if (C == '"' || C == '\'')
            {
                const size_t Start = P;
                P++;
                while (P < N && Line[P] != C) { if (Line[P] == '\\' && P + 1 < N) P++; P++; }
                if (P < N) P++;
                // a JSON key: a string followed by a colon
                size_t After = P;
                while (After < N && (Line[After] == ' ' || Line[After] == '\t')) After++;
                Push(Out, Start, L == Lang::Json && After < N && Line[After] == ':' ? Key : String);
                continue;
            }
            if (C == '#' && L == Lang::Gsc)
            {
                Push(Out, P, Directive);
                return;
            }
            if (std::isdigit((unsigned char)C) || (C == '-' && L == Lang::Json && P + 1 < N && std::isdigit((unsigned char)Line[P + 1])))
            {
                Push(Out, P, Number);
                P++;
                while (P < N && (std::isalnum((unsigned char)Line[P]) || Line[P] == '.' || Line[P] == '_')) P++;
                continue;
            }
            if (IsIdentStart(C))
            {
                const size_t Start = P;
                while (P < N && IsIdent(Line[P])) P++;
                Push(Out, Start, Words.count(Line.substr(Start, P - Start)) ? Keyword : Normal);
                continue;
            }
            Push(Out, P, Normal);
            P++;
        }
    }


    namespace
    {
        constexpr size_t MaxCells = 400000;          // cells kept of a delimited file
        constexpr size_t MaxCellBytes = 2000;        // a cell longer than this is kept cut
        constexpr size_t MaxColumns = 200;
        constexpr size_t MaxHexBytes = 64 * 1024;    // a binary file is shown as hex up to here
        constexpr size_t CacheBytes = 48u * 1024 * 1024;
        constexpr size_t CacheEntries = 4;

        std::string Thousands(size_t N)
        {
            std::string S = std::to_string(N);
            for (int At = (int)S.size() - 3; At > 0; At -= 3) S.insert((size_t)At, ",");
            return S;
        }

        std::string SizeText(size_t Bytes)
        {
            char B[32];
            if (Bytes >= 1024u * 1024) std::snprintf(B, sizeof(B), "%.1f MB", (double)Bytes / (1024.0 * 1024.0));
            else if (Bytes >= 1024) std::snprintf(B, sizeof(B), "%.0f KB", (double)Bytes / 1024.0);
            else std::snprintf(B, sizeof(B), "%zu bytes", Bytes);
            return B;
        }

        void AppendUtf8(std::string& Out, uint32_t Cp)
        {
            if (Cp < 0x80) Out += (char)Cp;
            else if (Cp < 0x800) { Out += (char)(0xC0 | (Cp >> 6)); Out += (char)(0x80 | (Cp & 0x3F)); }
            else if (Cp < 0x10000) { Out += (char)(0xE0 | (Cp >> 12)); Out += (char)(0x80 | ((Cp >> 6) & 0x3F)); Out += (char)(0x80 | (Cp & 0x3F)); }
            else { Out += (char)(0xF0 | (Cp >> 18)); Out += (char)(0x80 | ((Cp >> 12) & 0x3F)); Out += (char)(0x80 | ((Cp >> 6) & 0x3F)); Out += (char)(0x80 | (Cp & 0x3F)); }
        }

        // UTF-16 (after its BOM) to UTF-8; a lone surrogate or a half unit at the end becomes U+FFFD
        std::string Utf16To8(const std::string& In, size_t From, bool BigEndian)
        {
            std::string Out;
            Out.reserve(In.size());
            const auto Unit = [&](size_t At) {
                const unsigned char A = (unsigned char)In[At], B = (unsigned char)In[At + 1];
                return (uint32_t)(BigEndian ? (A << 8 | B) : (B << 8 | A));
            };
            size_t i = From;
            for (; i + 1 < In.size(); i += 2)
            {
                uint32_t U = Unit(i);
                if (U >= 0xD800 && U < 0xDC00)
                {
                    if (i + 3 < In.size() && Unit(i + 2) >= 0xDC00 && Unit(i + 2) < 0xE000) { U = 0x10000 + ((U - 0xD800) << 10) + (Unit(i + 2) - 0xDC00); i += 2; }
                    else U = 0xFFFD;
                }
                else if (U >= 0xDC00 && U < 0xE000) U = 0xFFFD;
                AppendUtf8(Out, U);
            }
            if (i < In.size()) AppendUtf8(Out, 0xFFFD);
            return Out;
        }

        // Does this look like bytes rather than text: a NUL, or many control characters, in the first 8 KB
        bool LooksBinary(const std::string& Data)
        {
            const size_t N = std::min<size_t>(Data.size(), 8192);
            size_t Control = 0;
            for (size_t i = 0; i < N; i++)
            {
                const unsigned char C = (unsigned char)Data[i];
                if (C == 0) return true;
                if (C < 0x20 && C != '\t' && C != '\n' && C != '\r' && C != '\f' && C != 0x1B) Control++;
            }
            return N > 0 && Control * 20 > N;
        }

        std::string HexDump(const std::string& Data)
        {
            std::string Out;
            const size_t N = std::min(Data.size(), MaxHexBytes);
            char B[24];
            for (size_t At = 0; At < N; At += 16)
            {
                std::snprintf(B, sizeof(B), "%08zx ", At);
                Out += B;
                std::string Ascii;
                for (size_t k = 0; k < 16; k++)
                {
                    if (At + k < N)
                    {
                        std::snprintf(B, sizeof(B), " %02x", (unsigned char)Data[At + k]);
                        Out += B;
                        const unsigned char C = (unsigned char)Data[At + k];
                        Ascii += C >= 0x20 && C < 0x7F ? (char)C : '.';
                    }
                    else Out += "   ";
                }
                Out += "  " + Ascii + "\n";
            }
            return Out;
        }

        // The delimiter of a delimited text: the most frequent of , ; tab in its first record, counting
        // only outside quotes ("a;b;c",d is a comma file)
        char DetectDelimiter(const std::string& Text)
        {
            size_t Commas = 0, Semis = 0, Tabs = 0;
            bool Quoted = false;
            const size_t N = std::min<size_t>(Text.size(), 64 * 1024);
            for (size_t i = 0; i < N; i++)
            {
                const char C = Text[i];
                if (C == '"') Quoted = !Quoted;
                else if (Quoted) continue;
                else if (C == '\n' || C == '\r') break;
                else if (C == ',') Commas++;
                else if (C == ';') Semis++;
                else if (C == '\t') Tabs++;
            }
            return Tabs > Commas && Tabs >= Semis ? '\t' : Semis > Commas ? ';' : ',';
        }
    }

    std::vector<std::vector<std::string>> TextViewer::ParseDelimited(const std::string& Text, char& Delimiter, size_t MaxRows, ParseStats* Stats)
    {
        ParseStats Local;
        ParseStats& St = Stats ? *Stats : Local;
        St = ParseStats{};
        Delimiter = DetectDelimiter(Text);
        std::vector<std::vector<std::string>> Rows;
        std::vector<std::string> Row;
        std::string Cell;
        bool Quoted = false, Any = false;
        size_t Line = 1, Cells = 0, NoCloseFrom = std::string::npos;
        // is there a closing quote after Open: a quote not doubled, followed by the delimiter, a line end or the end
        const auto Closes = [&](size_t Open) {
            if (NoCloseFrom != std::string::npos && Open >= NoCloseFrom) return false;
            for (size_t k = Open + 1; k < Text.size(); k++)
            {
                if (Text[k] != '"') continue;
                if (k + 1 < Text.size() && Text[k + 1] == '"') { k++; continue; }
                if (k + 1 == Text.size() || Text[k + 1] == Delimiter || Text[k + 1] == '\n' || Text[k + 1] == '\r') return true;
            }
            NoCloseFrom = Open;
            return false;
        };
        size_t RowCells = 0;   // the cells of this row, kept or not
        const auto EndCell = [&] {
            if (Cell.size() > MaxCellBytes) { Cell.resize(MaxCellBytes); St.CellsCut++; }
            if (Row.size() < MaxColumns) Row.push_back(std::move(Cell));   // the columns beyond the shown ones are not kept
            Cell.clear();
            RowCells++;
            Cells++;
        };
        const auto EndRow = [&] {
            EndCell();
            St.Columns = std::max(St.Columns, RowCells);
            Rows.push_back(std::move(Row));
            Row.clear();
            RowCells = 0;
            Any = false;
        };
        size_t i = 0;
        for (; i < Text.size() && Rows.size() < MaxRows && Cells < MaxCells; i++)
        {
            const char C = Text[i];
            Any = true;
            if (Quoted)
            {
                if (C == '"' && i + 1 < Text.size() && Text[i + 1] == '"') { Cell += '"'; i++; }
                else if (C == '"') Quoted = false;
                else
                {
                    if (C == '\n') Line++;
                    if (Cell.size() <= MaxCellBytes) Cell += C;   // beyond that the cell is cut anyway
                }
            }
            else if (C == '"' && Cell.empty())
            {
                // an opening quote with no closing one is an ordinary character, so the rest is not swallowed
                if (Closes(i)) Quoted = true;
                else { Cell += C; if (St.BadQuoteLine == 0) St.BadQuoteLine = Line; St.BadQuotes++; }
            }
            else if (C == Delimiter) EndCell();
            else if (C == '\n') { Line++; EndRow(); }
            else if (C == '\r') { if (i + 1 < Text.size() && Text[i + 1] == '\n') i++; Line++; EndRow(); }   // CRLF, and CR alone
            else if (Cell.size() <= MaxCellBytes) Cell += C;
        }
        // stopped at a limit: what was read of the row under way is kept, and the cut is said
        St.RowsCut = i < Text.size();
        if (St.RowsCut) { if (!Row.empty() || !Cell.empty()) EndRow(); }
        else if (Any) EndRow();
        return Rows;
    }

    // One document built from the bytes of a file: decoded to UTF-8, indexed, a delimited file parsed.
    // Pure: runs on a worker.
    std::shared_ptr<const TextViewer::Doc> TextViewer::Build(const std::string& Extension, std::string Bytes, size_t FileBytes, bool FileCut)
    {
        auto D = std::make_shared<Doc>();
        D->Ext = Extension;
        for (char& C : D->Ext) C = (char)std::toupper((unsigned char)C);
        D->L = LangOf(Extension);
        D->FileBytes = FileBytes;
        D->FileCut = FileCut;
        // the encoding: a BOM says it; none is UTF-8 (or bytes)
        std::string Data;
        if (Bytes.size() >= 3 && (unsigned char)Bytes[0] == 0xEF && (unsigned char)Bytes[1] == 0xBB && (unsigned char)Bytes[2] == 0xBF)
        {
            Data = Bytes.substr(3);
            D->Encoding = "UTF-8 (BOM)";
        }
        else if (Bytes.size() >= 2 && (unsigned char)Bytes[0] == 0xFF && (unsigned char)Bytes[1] == 0xFE)
        {
            Data = Utf16To8(Bytes, 2, false);
            D->Encoding = "UTF-16 LE";
        }
        else if (Bytes.size() >= 2 && (unsigned char)Bytes[0] == 0xFE && (unsigned char)Bytes[1] == 0xFF)
        {
            Data = Utf16To8(Bytes, 2, true);
            D->Encoding = "UTF-16 BE";
        }
        else if (LooksBinary(Bytes))
        {
            D->Binary = true;
            D->HexCut = Bytes.size() > MaxHexBytes;
            Data = HexDump(Bytes);
            D->L = Lang::Plain;
        }
        else Data = std::move(Bytes);
        D->Text = std::move(Data);
        D->Index();
        if (D->L == Lang::Csv)
        {
            D->Cells = ParseDelimited(D->Text, D->Delim, 50000, &D->Parse);
            D->Approx += D->Cells.size() * 48;
            for (const auto& R : D->Cells) D->Approx += R.size() * 40;
        }
        D->Approx += D->Text.size() + D->LineStart.size() * 5;
        return D;
    }

    void TextViewer::Doc::Index()
    {
        // a line ends at LF, CRLF or a CR alone
        LineStart.assign(1, 0);
        for (size_t i = 0; i < Text.size(); i++)
        {
            if (Text[i] == '\n') LineStart.push_back((uint32_t)i + 1);
            else if (Text[i] == '\r')
            {
                if (i + 1 < Text.size() && Text[i + 1] == '\n') i++;
                LineStart.push_back((uint32_t)i + 1);
            }
        }
        if (LineStart.back() != Text.size()) LineStart.push_back((uint32_t)Text.size());
        else if (LineStart.size() == 1) LineStart.push_back(0);
        const size_t Lines = LineStart.size() - 1;
        BlockAtStart.assign(Lines, 0);
        bool Block = false;
        LongestChars = 0;
        std::vector<Seg> Scratch;
        for (size_t i = 0; i < Lines; i++)
        {
            BlockAtStart[i] = Block;
            const std::string_view Line = Trimmed(Text, LineStart[i], LineStart[i + 1]);
            if (Line.size() > MaxDrawnChars) LinesCut++;
            LongestChars = std::max(LongestChars, (float)std::min<size_t>(Line.size(), MaxDrawnChars));
            if (L != Lang::Plain && L != Lang::Csv && L != Lang::Ini) Tokenise(L, Line, Block, Scratch);
        }
    }

    std::string TextViewer::Doc::Summary() const
    {
        std::string S = Thousands(LineStart.size() - 1) + " lines  \xC2\xB7  " + (Ext.empty() ? "TEXT" : Ext);
        if (!Encoding.empty()) S += "  \xC2\xB7  " + Encoding;
        if (Binary) S += "  \xC2\xB7  binary, shown as hex" + std::string(HexCut ? " (first 64 KB)" : "");
        if (FileCut) S += "  \xC2\xB7  first " + SizeText(MaxBytes) + " of " + SizeText(FileBytes) + " shown";
        if (LinesCut) S += "  \xC2\xB7  " + Thousands(LinesCut) + (LinesCut == 1 ? " line" : " lines") + " drawn cut at " + Thousands(MaxDrawnChars) + " characters";
        if (L == Lang::Csv)
        {
            if (Parse.RowsCut) S += "  \xC2\xB7  first " + Thousands(Cells.size()) + " rows read";
            if (Parse.Columns > MaxColumns) S += "  \xC2\xB7  first " + Thousands(MaxColumns) + " of " + Thousands(Parse.Columns) + " columns shown";
            if (Parse.CellsCut) S += "  \xC2\xB7  " + Thousands(Parse.CellsCut) + " cells cut at " + Thousands(MaxCellBytes) + " bytes";
            if (Parse.BadQuotes) S += "  \xC2\xB7  " + Thousands(Parse.BadQuotes) + " unterminated quote" + (Parse.BadQuotes == 1 ? "" : "s") + " (first on line " + Thousands(Parse.BadQuoteLine) + "), read as plain characters";
        }
        return S;
    }

    void TextViewer::Unload()
    {
        if (Wait) Wait->Cancel = true;
        Wait.reset();
        Cur.reset();
        Failed = false;
        Asked.clear();
    }

    void TextViewer::LoadText(const std::string& Extension, std::string Content, bool Truncated)
    {
        Unload();
        const size_t Bytes = Content.size();
        Cur = Build(Extension, std::move(Content), Bytes, Truncated);
        AsGrid = true;
    }

    // Reads the file on a worker; the viewer shows it when it is built (Poll). The last few built are kept
    // (while the file's size and write time are the same), so going back to one is at once.
    void TextViewer::Request(const std::string& Path, const std::string& Extension)
    {
        Unload();
        Asked = Path;
        AsGrid = true;
        std::shared_ptr<const Doc> Kept;
        for (auto It = Cache.begin(); It != Cache.end(); ++It)
            if (It->Path == Path)
            {
                Kept = It->D;
                Cache.splice(Cache.begin(), Cache, It);   // most recent first
                break;
            }
        Cur = Kept;   // shown at once while the file's size and time are looked at
        auto P = std::make_shared<Pending>();
        Wait = P;
        Submit({ Path, Extension, Kept, P });
    }

    // ---- the one reading worker. Its state is shared with the thread, so it lives as long as either holds it; Shutdown waits a little and lets it go.
    struct TextViewer::WorkerState
    {
        std::mutex M;
        std::condition_variable Cv;
        bool Has = false, Stop = false;
        Job Latest;
        std::atomic<bool> Started{ false }, Done{ false };
        std::thread T;
        ~WorkerState()
        {
            // the last holder may be the worker itself: it cannot join itself
            if (!T.joinable()) return;
            if (T.get_id() != std::this_thread::get_id() && Done.load()) T.join();
            else T.detach();
        }
    };

    std::shared_ptr<TextViewer::WorkerState> TextViewer::Worker()
    {
        static const std::shared_ptr<WorkerState> W = std::make_shared<WorkerState>();
        return W;
    }

    void TextViewer::RunJob(const Job& J)
    {
        const std::shared_ptr<Pending>& P = J.P;
        const std::filesystem::path W(Strings::ToUnicodeString(J.Path));   // wide: a folder with non-ANSI letters opens
        std::error_code Ec;
        const auto Size = std::filesystem::file_size(W, Ec);
        const auto Time = Ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(W, Ec);
        if (Ec) { P->Done = true; return; }
        if (J.Kept && J.Kept->Size == Size && J.Kept->Time == Time) { P->Result = J.Kept; P->Done = true; return; }
        std::ifstream In(W, std::ios::binary);
        if (!In) { P->Done = true; return; }
        std::string Data(MaxBytes + 1, '\0');
        In.read(Data.data(), (std::streamsize)Data.size());
        Data.resize((size_t)In.gcount());
        const bool Cut = Data.size() > MaxBytes;
        if (Cut) Data.resize(MaxBytes);
        if (P->Cancel.load()) { P->Done = true; return; }
        auto D = std::const_pointer_cast<Doc>(Build(J.Extension, std::move(Data), (size_t)Size, Cut));
        D->Size = Size;
        D->Time = Time;
        P->Result = D;
        P->Done = true;
    }

    void TextViewer::Submit(Job J)
    {
        const std::shared_ptr<WorkerState> S = Worker();
        std::lock_guard<std::mutex> Lock(S->M);
        if (S->Stop) { J.P->Done = true; return; }
        if (S->Has) S->Latest.P->Done = true;   // replaced before it was started: nobody waits for it (its viewer asked again)
        S->Latest = std::move(J);
        S->Has = true;
        if (!S->Started.exchange(true))
            S->T = std::thread([S] {
                for (;;)
                {
                    Job Mine;
                    {
                        std::unique_lock<std::mutex> Lk(S->M);
                        S->Cv.wait(Lk, [&] { return S->Stop || S->Has; });
                        if (S->Stop) break;
                        Mine = std::move(S->Latest);
                        S->Has = false;
                    }
                    try { RunJob(Mine); }
                    catch (...) { Mine.P->Done = true; }   // a bad file or no memory: the viewer says it could not read it
                }
                S->Done = true;
            });
        S->Cv.notify_one();
    }

    void TextViewer::Shutdown()
    {
        const std::shared_ptr<WorkerState> S = Worker();
        {
            std::lock_guard<std::mutex> Lock(S->M);
            S->Stop = true;
            if (S->Has) S->Latest.P->Done = true;
            S->Has = false;
        }
        S->Cv.notify_all();
        if (!S->Started.load()) return;
        for (int Waited = 0; Waited < 100 && !S->Done.load(); Waited++) Sleep(20);
        if (S->Done.load() && S->T.joinable()) S->T.join();
        else if (S->T.joinable()) S->T.detach();
    }

    void TextViewer::Poll() const
    {
        if (!Wait || !Wait->Done.load()) return;
        const std::shared_ptr<Pending> P = std::move(Wait);
        Wait.reset();
        if (P->Cancel.load()) return;
        if (!P->Result) { Failed = true; return; }
        Cur = P->Result;
        // kept: the last few, within a size
        for (auto It = Cache.begin(); It != Cache.end(); ++It)
            if (It->Path == Asked) { Cache.erase(It); break; }
        Cache.push_front({ Asked, P->Result });
        size_t Total = 0, Kept = 0;
        for (auto It = Cache.begin(); It != Cache.end();)
        {
            Total += It->D->Approx;
            if (Kept >= CacheEntries || (Kept > 0 && Total > CacheBytes)) It = Cache.erase(It);
            else { Kept++; ++It; }
        }
    }

    bool TextViewer::Loaded() const { Poll(); return Cur != nullptr; }
    bool TextViewer::Reading() const { Poll(); return Wait != nullptr; }

    std::string TextViewer::Summary() const
    {
        Poll();
        return Cur ? Cur->Summary() : std::string();
    }

    void TextViewer::Draw()
    {
        Poll();
        if (!Cur)
        {
            if (Wait) ImGui::TextDisabled("Reading the file\xE2\x80\xA6");
            else ImGui::TextDisabled("Could not read this file");
            return;
        }
        const Doc& D = *Cur;
        const ImVec4 Colours[] = { Theme::Text, ImVec4(0.55f, 0.72f, 1.0f, 1.0f), ImVec4(0.62f, 0.86f, 0.56f, 1.0f), ImVec4(0.55f, 0.65f, 0.60f, 1.0f),
                                   ImVec4(0.98f, 0.72f, 0.45f, 1.0f), ImVec4(0.85f, 0.62f, 0.95f, 1.0f), ImVec4(0.45f, 0.82f, 0.88f, 1.0f) };
        // the bar: what it is and what was cut, then grid or text for a delimited file, Copy
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(D.Summary().c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (D.L == Lang::Csv)
        {
            if (Theme::Pill("Grid", AsGrid)) AsGrid = true;
            ImGui::SameLine(0, 2);
            if (Theme::Pill("Text", !AsGrid)) AsGrid = false;
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(D.Text.c_str());
        if (ImGui::IsItemHovered())
        {
            if (D.Binary) ImGui::SetTooltip("Copy the hex dump shown%s", D.HexCut ? " (the first 64 KB of the file)" : "");
            else if (D.FileCut) ImGui::SetTooltip("Copy the text shown: only the first %s of this %s file was read", SizeText(MaxBytes).c_str(), SizeText(D.FileBytes).c_str());
            else if (!D.Encoding.empty() && D.Encoding.rfind("UTF-16", 0) == 0) ImGui::SetTooltip("Copy the whole text, as UTF-8");
            else ImGui::SetTooltip("Copy the whole text");
        }

        if (D.L == Lang::Csv && AsGrid && !D.Cells.empty())
        {
            const size_t Cols = std::min<size_t>(D.Parse.Columns, MaxColumns);
            if (Cols > 0 && ImGui::BeginTable("##csvgrid", (int)Cols, ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit))
            {
                // what is drawn of a cell: its first characters with line breaks shown as " / " (a taller
                // row would break the clipper's fixed row height); the cell is clipped to its column
                constexpr size_t Shown = 240;
                const auto Flat = [](const std::string& V, std::string& Out) {
                    Out.assign(V, 0, Shown);
                    for (size_t k = 0; k < Out.size(); k++)
                        if (Out[k] == '\n' || Out[k] == '\r')
                        {
                            const size_t Len = (Out[k] == '\r' && k + 1 < Out.size() && Out[k + 1] == '\n') ? 2 : 1;
                            Out.replace(k, Len, " / ");
                            k += 2;
                        }
                };
                std::string Draw;
                ImGui::TableSetupScrollFreeze(0, 1);
                for (size_t c = 0; c < Cols; c++)
                {
                    Flat(c < D.Cells.front().size() && !D.Cells.front()[c].empty() ? D.Cells.front()[c] : "col " + std::to_string(c + 1), Draw);
                    ImGui::TableSetupColumn(Draw.c_str(), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
                }
                ImGui::TableHeadersRow();
                ImGuiListClipper Clip;
                Clip.Begin((int)D.Cells.size() - 1);
                while (Clip.Step())
                    for (int r = Clip.DisplayStart; r < Clip.DisplayEnd; r++)
                    {
                        const auto& Row = D.Cells[(size_t)r + 1];
                        ImGui::TableNextRow();
                        for (size_t c = 0; c < Cols && c < Row.size(); c++)
                        {
                            ImGui::TableSetColumnIndex((int)c);
                            const std::string& V = Row[c];
                            Flat(V, Draw);
                            char* End = nullptr;
                            std::strtod(V.c_str(), &End);
                            const bool Numeric = !V.empty() && V.size() < 40 && End != nullptr && *End == 0;   // numbers in their own colour
                            ImGui::PushStyleColor(ImGuiCol_Text, Colours[Numeric ? Number : Normal]);
                            ImGui::TextUnformatted(Draw.c_str());
                            ImGui::PopStyleColor();
                            if (V.size() > Shown && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", V.substr(0, 1200).c_str());
                        }
                    }
                ImGui::EndTable();
            }
            return;
        }

        // the text: a gutter of line numbers, the words in their colours; only the lines in view are cut
        const float LineH = ImGui::GetTextLineHeight();
        const size_t Lines = D.LineStart.size() - 1;
        const float GutterW = ImGui::CalcTextSize(std::to_string(Lines).c_str()).x + ImGui::GetFontSize() * 1.2f;
        const float CharW = ImGui::CalcTextSize("0").x;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Raised);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));   // lines touch
        ImGui::SetNextWindowContentSize(ImVec2(GutterW + D.LongestChars * CharW + CharW * 4, (float)Lines * LineH));
        if (ImGui::BeginChild("##textview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
        {
            ImDrawList* DL = ImGui::GetWindowDrawList();
            ImGuiListClipper Clip;
            Clip.Begin((int)Lines, LineH);
            const ImU32 Gutter = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            std::vector<Seg> Segs;
            while (Clip.Step())
                for (int i = Clip.DisplayStart; i < Clip.DisplayEnd; i++)
                {
                    const ImVec2 At = ImGui::GetCursorScreenPos();
                    const std::string_view Raw = Trimmed(D.Text, D.LineStart[(size_t)i], D.LineStart[(size_t)i + 1]);
                    char Num[16];
                    std::snprintf(Num, sizeof(Num), "%d", i + 1);
                    DL->AddText(ImVec2(At.x + GutterW - ImGui::CalcTextSize(Num).x - CharW * 1.5f, At.y), Gutter, Num);
                    std::string Line(Raw.substr(0, MaxDrawnChars));
                    for (size_t t = 0; (t = Line.find('\t', t)) != std::string::npos;) Line.replace(t, 1, "    ");
                    bool Block = D.BlockAtStart[(size_t)i] != 0;
                    if (D.L == Lang::Plain || D.L == Lang::Csv) Segs.assign(1, Seg{ 0, Normal });
                    else Tokenise(D.L, Line, Block, Segs);
                    if (Segs.empty()) Segs.push_back({ 0, Normal });
                    float X = At.x + GutterW;
                    for (size_t s = 0; s < Segs.size(); s++)
                    {
                        const size_t From = Segs[s].Start, To = s + 1 < Segs.size() ? Segs[s + 1].Start : Line.size();
                        if (To <= From) continue;
                        const std::string Piece = Line.substr(From, To - From);
                        DL->AddText(ImVec2(X, At.y), ImGui::GetColorU32(Colours[Segs[s].Class]), Piece.c_str());
                        X += ImGui::CalcTextSize(Piece.c_str()).x;
                    }
                    ImGui::Dummy(ImVec2(0, LineH));
                }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    bool TextViewer::SelfTest(std::string& Report)
    {
        bool Ok = true;
        const auto Check = [&](bool Cond, const char* What) { if (!Cond) { Ok = false; Report += std::string("FAIL ") + What + "\n"; } };
        const auto Classes = [](Lang L, const std::string& Line, bool& Block) {
            std::vector<Seg> Out;
            Tokenise(L, Line, Block, Out);
            std::string C;
            for (const Seg& S : Out) C += (char)('0' + S.Class);
            return C;
        };
        bool B = false;
        Check(LangOf("GSC") == Lang::Gsc && LangOf("csv") == Lang::Csv && LangOf("xyz") == Lang::Plain, "language by extension");
        Check(Classes(Lang::Gsc, "if (x) // note", B) == "103", "gsc: keyword, text, comment");
        B = false;
        const std::string Mixed = Classes(Lang::Gsc, "a = \"s\" + 12;", B);
        Check(Mixed.find('2') != std::string::npos && Mixed.find('4') != std::string::npos, "gsc: string and number");
        B = false;
        Classes(Lang::Gsc, "x /* open", B);
        Check(B, "a block comment opened carries to the next line");
        Check(Classes(Lang::Gsc, "still */ y", B).front() == '3' && !B, "a block comment closes");
        B = false;
        Check(Classes(Lang::Gsc, "#using scripts\\shared\\util_shared;", B) == "5", "gsc: directive");
        const std::string LuaRun = Classes(Lang::Lua, "local a = nil -- c", B);
        Check(LuaRun.find('1') != std::string::npos && LuaRun.back() == '3', "lua: keywords and comment");
        Check(Classes(Lang::Json, "{\"k\": \"v\", \"n\": -3}", B).find('6') != std::string::npos, "json: key");
        Check(Classes(Lang::Ini, "[Section]", B) == "6" && Classes(Lang::Ini, "key=1", B).find('6') == 0, "ini: section and key");
        char D = 0;
        const auto Rows = ParseDelimited("a,b,c\n1,\"x,y\",3\n\"q\"\"r\",,\n", D);
        Check(D == ',' && Rows.size() == 3 && Rows[1].size() == 3 && Rows[1][1] == "x,y" && Rows[2][0] == "q\"r" && Rows[2][1].empty(), "csv: quotes, commas, empties");
        const auto Tabs = ParseDelimited("a\tb\n1\t2", D);
        Check(D == '\t' && Tabs.size() == 2 && Tabs[1][1] == "2", "tsv: delimiter detected, no trailing newline");
        // the delimiter ignores quoted fields
        ParseDelimited("\"a;b;c\",d\nx,y\n", D);
        Check(D == ',', "csv: a quoted semicolon is not the delimiter");
        // an unterminated quote is reported and does not swallow the rest
        ParseStats St;
        const auto Bad = ParseDelimited("a,b\n\"oops,c\nd,e\nf,g\n", D, 50000, &St);
        Check(St.BadQuotes == 1 && St.BadQuoteLine == 2 && Bad.size() == 4 && Bad[3][1] == "g", "csv: an unterminated quote is reported, the rows after it kept");
        // a multi-line cell stays in its row, CR alone ends a row
        const auto Multi = ParseDelimited("a,b\n\"l1\nl2\",c\nd,e\n", D);
        Check(Multi.size() == 3 && Multi[1][0] == "l1\nl2" && Multi[2][0] == "d", "csv: a multi-line cell");
        const auto Cr = ParseDelimited("a,b\r1,2\r3,4", D);
        Check(Cr.size() == 3 && Cr[2][1] == "4", "csv: CR alone ends a row");
        // limits: rows and cells say they were cut
        {
            std::string Many;
            for (int r = 0; r < 20; r++) Many += "a,b\n";
            ParseStats Cut;
            const auto Few = ParseDelimited(Many, D, 5, &Cut);
            Check(Few.size() == 5 && Cut.RowsCut, "csv: rows cut at the limit and said so");
            std::string Long = "h\n" + std::string(MaxCellBytes * 3, 'z') + "\n";
            ParseStats LongSt;
            const auto One = ParseDelimited(Long, D, 50000, &LongSt);
            Check(One.size() == 2 && One[1][0].size() == MaxCellBytes && LongSt.CellsCut == 1, "csv: a long cell is cut and said so");
            // 4 MB of "a," makes 2M cells: capped
            std::string Wide(MaxCells * 2 * 2, 'a');
            for (size_t k = 1; k < Wide.size(); k += 2) Wide[k] = ',';
            ParseStats WideSt;
            const auto W = ParseDelimited(Wide, D, 50000, &WideSt);
            size_t Total = 0;
            for (const auto& R : W) Total += R.size();
            Check(W.size() == 1 && W[0].size() == MaxColumns && Total <= MaxCells && WideSt.RowsCut && WideSt.Columns > MaxColumns, "csv: cells and columns are capped as well as rows");
        }
        // lines: LF, CRLF, CR alone, no final newline
        {
            const auto Lines = [](const std::string& T) { auto Doc = TextViewer::Build("txt", T, T.size(), false); return Doc->LineStart.size() - 1; };
            Check(Lines("one\ntwo\r\nthree") == 3, "line count without a final newline");
            Check(Lines("a\rb\rc") == 3, "CR alone ends a line");
            Check(Lines("a\r\nb\r\n") == 2, "CRLF is one line end");
        }
        // encodings
        {
            const auto Of = [](const std::string& Bytes) { return TextViewer::Build("txt", Bytes, Bytes.size(), false); };
            Check(Of("\xEF\xBB\xBFhello")->Text == "hello", "a UTF-8 BOM is stripped");
            const std::string Le("\xFF\xFE" "h\0i\0\x3D\xD8\x00\xDE", 10);   // "hi" and U+1F600 as a surrogate pair
            Check(Of(Le)->Text == "hi\xF0\x9F\x98\x80", "UTF-16 LE with a BOM is decoded");
            const std::string Be("\xFE\xFF\0h\0i", 6);
            Check(Of(Be)->Text == "hi" && Of(Be)->Encoding == "UTF-16 BE", "UTF-16 BE with a BOM is decoded");
            const auto Bin = Of(std::string("\x00\x01\x02" "ABCD" "\xFF\xFE\x00", 10));
            Check(Bin->Binary && Bin->Text.find("00 01 02 41") != std::string::npos && Bin->Summary().find("binary") != std::string::npos, "binary is shown as hex and says so");
            const auto Cut = TextViewer::Build("txt", "abc", 9 * 1024 * 1024, true);
            Check(Cut->Summary().find("first 4.0 MB of 9.0 MB") != std::string::npos, "a cut file says how much was read");
            std::string LongLine(MaxDrawnChars + 10, 'x');
            Check(Of(LongLine + "\nshort\n")->Summary().find("1 line drawn cut at 2,000") != std::string::npos, "a long line says it is drawn cut");
        }
        TextViewer V;
        V.LoadText("gsc", "one\ntwo\r\nthree");
        Check(V.Summary().find("3 lines") == 0, "line count without a final newline (viewer)");
        {
            // the one worker: a file in a folder with non-ANSI letters opens, and requests made one after the other end with the last one shown
            const std::filesystem::path Dir = std::filesystem::temp_directory_path() / std::filesystem::path(L"graywolf-textview-тест-é");
            std::error_code Ec;
            std::filesystem::create_directories(Dir, Ec);
            for (int N = 0; N < 3; N++) { std::ofstream(Dir / (L"f" + std::to_wstring(N) + L".gsc")) << "// file " << N << "\nmain() {}\n"; }
            TextViewer V;
            for (int N = 0; N < 3; N++)
            {
                const std::u8string U8 = (Dir / (L"f" + std::to_wstring(N) + L".gsc")).u8string();   // UTF-8: .string() cannot hold these letters
                V.Request(std::string(reinterpret_cast<const char*>(U8.data()), U8.size()), "gsc");
            }
            for (int I = 0; I < 300 && (V.Reading() || !V.Loaded()); I++) Sleep(10);
            Check(V.Loaded() && V.Summary().find("2 lines") != std::string::npos, "the text worker reads the last request in a folder with non-ANSI letters");
            V.Unload();
            std::filesystem::remove_all(Dir, Ec);
            Shutdown();   // the worker ends here, as the shell ends it
        }
        return Ok;
    }
}
