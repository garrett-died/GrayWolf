#pragma once

// The text viewer of the Viewport: a script, a config, a CSV or any text asset shown with coloured
// words, line numbers and horizontal and vertical scrolling; a CSV or TSV as a grid as well. Not an
// editor: there is no caret, and Copy copies the whole text. (HydraX's text viewer, docs/text-viewer.md;
// its hover documentation needs a documentation source GrayWolf does not have, so there is none.)

#include <cstdint>
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

        // Reads Path (the first 4 MB), the language from its extension (without the dot)
        bool LoadFile(const std::string& Path, const std::string& Extension);
        // The same from text already in memory (ui-shot's demo rows)
        void LoadText(const std::string& Extension, std::string Content, bool Truncated = false);
        void Unload();
        bool Loaded() const { return Ready; }
        std::string Summary() const;   // "123 lines, GSC"
        // Fills the free area of the window
        void Draw();

        // The language an extension names
        static Lang LangOf(const std::string& Extension);
        // One line cut into classed runs. InBlock carries a block comment from line to line.
        static void Tokenise(Lang L, std::string_view Line, bool& InBlock, std::vector<Seg>& Out);
        // The cells of a delimited text, quotes undone. Delimiter: the most frequent of , ; tab in the first line.
        static std::vector<std::vector<std::string>> ParseDelimited(const std::string& Text, char& Delimiter, size_t MaxRows = 50000);
        static bool SelfTest(std::string& Report);

    private:
        void Index();
        bool Ready = false, Cut = false, AsGrid = true;
        Lang L = Lang::Plain;
        std::string Text, Ext;
        std::vector<uint32_t> LineStart;      // each line's first byte; one more entry for the end
        std::vector<uint8_t> BlockAtStart;    // is the line inside a block comment
        std::vector<std::vector<std::string>> Cells;
        char Delim = ',';
        float LongestChars = 0;
        std::vector<Seg> Scratch;
    };
}
