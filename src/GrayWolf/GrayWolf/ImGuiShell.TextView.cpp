#include "stdafx.h"

#include "ImGuiShell.TextView.h"
#include "ImGuiTheme.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
            static const std::set<std::string_view> Gsc = { "if", "else", "for", "foreach", "while", "switch", "case", "default", "break", "continue",
                "return", "wait", "thread", "function", "class", "self", "level", "game", "anim", "true", "false", "undefined", "in", "do", "waittill",
                "endon", "notify", "waitframe", "new", "var", "const", "private", "autoexec", "event_handler", "namespace" };
            static const std::set<std::string_view> Lua = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
                "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };
            static const std::set<std::string_view> Json = { "true", "false", "null" };
            static const std::set<std::string_view> None;
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

    std::vector<std::vector<std::string>> TextViewer::ParseDelimited(const std::string& Text, char& Delimiter, size_t MaxRows)
    {
        const size_t FirstEnd = Text.find('\n');
        const std::string First = Text.substr(0, FirstEnd);
        const size_t Commas = (size_t)std::count(First.begin(), First.end(), ','), Semis = (size_t)std::count(First.begin(), First.end(), ';'),
                     Tabs = (size_t)std::count(First.begin(), First.end(), '\t');
        Delimiter = Tabs > Commas && Tabs >= Semis ? '\t' : Semis > Commas ? ';' : ',';
        std::vector<std::vector<std::string>> Rows;
        std::vector<std::string> Row;
        std::string Cell;
        bool Quoted = false, Any = false;
        const auto EndRow = [&] {
            Row.push_back(std::move(Cell));
            Cell.clear();
            Rows.push_back(std::move(Row));
            Row.clear();
            Any = false;
        };
        for (size_t i = 0; i < Text.size() && Rows.size() < MaxRows; i++)
        {
            const char C = Text[i];
            Any = true;
            if (Quoted)
            {
                if (C == '"' && i + 1 < Text.size() && Text[i + 1] == '"') { Cell += '"'; i++; }
                else if (C == '"') Quoted = false;
                else Cell += C;
            }
            else if (C == '"' && Cell.empty()) Quoted = true;
            else if (C == Delimiter) { Row.push_back(std::move(Cell)); Cell.clear(); }
            else if (C == '\n') { if (!Cell.empty() && Cell.back() == '\r') Cell.pop_back(); EndRow(); }
            else Cell += C;
        }
        if (Any && Rows.size() < MaxRows) EndRow();
        return Rows;
    }

    void TextViewer::Unload()
    {
        Ready = false;
        Text.clear(); LineStart.clear(); BlockAtStart.clear(); Cells.clear();
    }

    bool TextViewer::LoadFile(const std::string& Path, const std::string& Extension)
    {
        Unload();
        std::ifstream In(Path, std::ios::binary);
        if (!In) return false;
        std::string Data(MaxBytes + 1, '\0');
        In.read(Data.data(), (std::streamsize)Data.size());
        Data.resize((size_t)In.gcount());
        const bool Truncated = Data.size() > MaxBytes;
        if (Truncated) Data.resize(MaxBytes);
        LoadText(Extension, std::move(Data), Truncated);
        return true;
    }

    void TextViewer::LoadText(const std::string& Extension, std::string Content, bool Truncated)
    {
        Unload();
        Text = std::move(Content);
        Ext = Extension;
        for (char& C : Ext) C = (char)std::toupper((unsigned char)C);
        Cut = Truncated;
        L = LangOf(Extension);
        AsGrid = true;
        Index();
        if (L == Lang::Csv) Cells = ParseDelimited(Text, Delim);
        Ready = true;
    }

    void TextViewer::Index()
    {
        LineStart.assign(1, 0);
        for (size_t i = 0; i < Text.size(); i++)
            if (Text[i] == '\n') LineStart.push_back((uint32_t)i + 1);
        if (LineStart.back() != Text.size()) LineStart.push_back((uint32_t)Text.size());
        else if (LineStart.size() == 1) LineStart.push_back(0);
        const size_t Lines = LineStart.size() - 1;
        BlockAtStart.assign(Lines, 0);
        bool Block = false;
        LongestChars = 0;
        for (size_t i = 0; i < Lines; i++)
        {
            BlockAtStart[i] = Block;
            const std::string_view Line = Trimmed(Text, LineStart[i], LineStart[i + 1]);
            LongestChars = std::max(LongestChars, (float)std::min<size_t>(Line.size(), MaxDrawnChars));
            if (L != Lang::Plain && L != Lang::Csv && L != Lang::Ini) Tokenise(L, Line, Block, Scratch);
        }
    }

    std::string TextViewer::Summary() const
    {
        if (!Ready) return {};
        char B[96];
        std::snprintf(B, sizeof(B), "%zu lines  \xC2\xB7  %s%s", LineStart.size() - 1, Ext.empty() ? "TEXT" : Ext.c_str(), Cut ? "  \xC2\xB7  first 4 MB" : "");
        return B;
    }

    void TextViewer::Draw()
    {
        if (!Ready) return;
        const ImVec4 Colours[] = { Theme::Text, ImVec4(0.55f, 0.72f, 1.0f, 1.0f), ImVec4(0.62f, 0.86f, 0.56f, 1.0f), ImVec4(0.55f, 0.65f, 0.60f, 1.0f),
                                   ImVec4(0.98f, 0.72f, 0.45f, 1.0f), ImVec4(0.85f, 0.62f, 0.95f, 1.0f), ImVec4(0.45f, 0.82f, 0.88f, 1.0f) };
        // the bar: what it is, grid or text for a delimited file, Copy
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Summary().c_str());
        if (L == Lang::Csv)
        {
            ImGui::SameLine();
            if (Theme::Pill("Grid", AsGrid)) AsGrid = true;
            ImGui::SameLine(0, 2);
            if (Theme::Pill("Text", !AsGrid)) AsGrid = false;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(Text.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the whole text");

        if (L == Lang::Csv && AsGrid && !Cells.empty())
        {
            size_t Cols = 0;
            for (const auto& R : Cells) Cols = std::max(Cols, R.size());
            Cols = std::min<size_t>(Cols, 200);
            if (ImGui::BeginTable("##csvgrid", (int)Cols, ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                for (size_t c = 0; c < Cols; c++)
                {
                    const std::string Head = c < Cells.front().size() && !Cells.front()[c].empty() ? Cells.front()[c] : "col " + std::to_string(c + 1);
                    ImGui::TableSetupColumn(Head.c_str(), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
                }
                ImGui::TableHeadersRow();
                ImGuiListClipper Clip;
                Clip.Begin((int)Cells.size() - 1);
                while (Clip.Step())
                    for (int r = Clip.DisplayStart; r < Clip.DisplayEnd; r++)
                    {
                        const auto& Row = Cells[(size_t)r + 1];
                        ImGui::TableNextRow();
                        for (size_t c = 0; c < Cols && c < Row.size(); c++)
                        {
                            ImGui::TableSetColumnIndex((int)c);
                            const std::string& V = Row[c];
                            char* End = nullptr;
                            std::strtod(V.c_str(), &End);
                            const bool Numeric = !V.empty() && End != nullptr && *End == 0;   // numbers in their own colour
                            ImGui::PushStyleColor(ImGuiCol_Text, Colours[Numeric ? Number : Normal]);
                            ImGui::TextUnformatted(V.c_str());
                            ImGui::PopStyleColor();
                        }
                    }
                ImGui::EndTable();
            }
            return;
        }

        // the text: a gutter of line numbers, the words in their colours; only the lines in view are cut
        const float LineH = ImGui::GetTextLineHeight();
        const size_t Lines = LineStart.size() - 1;
        const float GutterW = ImGui::CalcTextSize(std::to_string(Lines).c_str()).x + ImGui::GetFontSize() * 1.2f;
        const float CharW = ImGui::CalcTextSize("0").x;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Raised);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));   // lines touch
        ImGui::SetNextWindowContentSize(ImVec2(GutterW + LongestChars * CharW + CharW * 4, (float)Lines * LineH));
        if (ImGui::BeginChild("##textview", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
        {
            ImDrawList* DL = ImGui::GetWindowDrawList();
            ImGuiListClipper Clip;
            Clip.Begin((int)Lines, LineH);
            const ImU32 Gutter = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            while (Clip.Step())
                for (int i = Clip.DisplayStart; i < Clip.DisplayEnd; i++)
                {
                    const ImVec2 At = ImGui::GetCursorScreenPos();
                    const std::string_view Raw = Trimmed(Text, LineStart[(size_t)i], LineStart[(size_t)i + 1]);
                    char Num[16];
                    std::snprintf(Num, sizeof(Num), "%d", i + 1);
                    DL->AddText(ImVec2(At.x + GutterW - ImGui::CalcTextSize(Num).x - CharW * 1.5f, At.y), Gutter, Num);
                    std::string Line(Raw.substr(0, MaxDrawnChars));
                    for (size_t t = 0; (t = Line.find('\t', t)) != std::string::npos;) Line.replace(t, 1, "    ");
                    bool Block = BlockAtStart[(size_t)i] != 0;
                    if (L == Lang::Plain || L == Lang::Csv) Scratch.assign(1, Seg{ 0, Normal });
                    else Tokenise(L, Line, Block, Scratch);
                    if (Scratch.empty()) Scratch.push_back({ 0, Normal });
                    float X = At.x + GutterW;
                    for (size_t s = 0; s < Scratch.size(); s++)
                    {
                        const size_t From = Scratch[s].Start, To = s + 1 < Scratch.size() ? Scratch[s + 1].Start : Line.size();
                        if (To <= From) continue;
                        const std::string Piece = Line.substr(From, To - From);
                        DL->AddText(ImVec2(X, At.y), ImGui::GetColorU32(Colours[Scratch[s].Class]), Piece.c_str());
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
        TextViewer V;
        V.LoadText("gsc", "one\ntwo\r\nthree");
        Check(V.Summary().find("3 lines") == 0, "line count without a final newline");
        return Ok;
    }
}
