#pragma once

// The text viewer of the Viewport: a script, a config, a CSV or any text asset shown with coloured
// words, line numbers and horizontal and vertical scrolling; a CSV or TSV as a grid as well. Not an
// editor: there is no caret, and Copy copies the whole text. (HydraX's text viewer, docs/text-viewer.md;
// its hover documentation needs a documentation source GrayWolf does not have, so there is none.)
// A file is read and indexed on a worker (Request) and shown when it is built; the last few built are
// kept. Encodings: UTF-8 with or without a BOM, UTF-16 with a BOM (shown as UTF-8), anything with NULs
// as a hex dump. Whatever was cut (file, long lines, rows, columns, cells, quotes) is said in Summary.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ShellUi
{
    class TextViewer
    {
    public:
        enum class Lang { Plain, Gsc, Lua, Ini, Json, Xml, Csv };
        // the word classes a line is cut into
        enum Cls : uint8_t { Normal, Keyword, String, Comment, Number, Directive, Key };
        struct Seg { uint32_t Start; uint8_t Class; };
        // what a delimited text lost or got wrong
        struct ParseStats
        {
            size_t Columns = 0;      // the widest row, kept or not
            size_t CellsCut = 0;     // cells kept shorter than they are
            size_t BadQuotes = 0;    // opening quotes never closed (read as plain characters)
            size_t BadQuoteLine = 0; // the first of them
            bool RowsCut = false;    // the row or cell limit stopped the read
        };

        // Reads Path (the first 4 MB) on a worker; the language from its extension (without the dot)
        void Request(const std::string& Path, const std::string& Extension);
        // The same from text already in memory (ui-shot's demo rows)
        void LoadText(const std::string& Extension, std::string Content, bool Truncated = false);
        void Unload();
        bool Loaded() const;    // a document is shown
        bool Reading() const;   // a file is being read
        std::string Summary() const;   // "123 lines, GSC", and what was cut
        // Fills the free area of the window ("Reading", or why not, while there is no document)
        void Draw();

        // The language an extension names
        static Lang LangOf(const std::string& Extension);
        // One line cut into classed runs. InBlock carries a block comment from line to line.
        static void Tokenise(Lang L, std::string_view Line, bool& InBlock, std::vector<Seg>& Out);
        // The cells of a delimited text, quotes undone. Delimiter: the most frequent of , ; tab in the first
        // record, outside quotes. Stats (optional) says what was cut.
        static std::vector<std::vector<std::string>> ParseDelimited(const std::string& Text, char& Delimiter, size_t MaxRows = 50000, ParseStats* Stats = nullptr);
        static bool SelfTest(std::string& Report);

    private:
        // One built document: immutable once shown, shared with the cache
        struct Doc
        {
            Lang L = Lang::Plain;
            std::string Text, Ext, Encoding;
            std::vector<uint32_t> LineStart;      // each line's first byte; one more entry for the end
            std::vector<uint8_t> BlockAtStart;    // is the line inside a block comment
            std::vector<std::vector<std::string>> Cells;
            ParseStats Parse;
            char Delim = ',';
            float LongestChars = 0;
            size_t FileBytes = 0, LinesCut = 0, Approx = 0;
            bool FileCut = false, Binary = false, HexCut = false;
            std::uintmax_t Size = 0;                 // the file's size and write time when read (the cache's key)
            std::filesystem::file_time_type Time{};
            void Index();
            std::string Summary() const;
        };
        struct Pending
        {
            std::atomic<bool> Done{ false }, Cancel{ false };
            std::shared_ptr<const Doc> Result;
        };
        struct Kept { std::string Path; std::shared_ptr<const Doc> D; };
        static std::shared_ptr<const Doc> Build(const std::string& Extension, std::string Bytes, size_t FileBytes, bool FileCut);
        void Poll() const;

        mutable std::shared_ptr<const Doc> Cur;
        mutable std::shared_ptr<Pending> Wait;
        mutable std::list<Kept> Cache;
        mutable bool Failed = false;
        mutable std::string Asked;
        bool AsGrid = true;
    };
}
