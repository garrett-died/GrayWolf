#include "stdafx.h"

// See Bo3Target.Verify.h.

#include "Bo3Target.Verify.h"

#include "Bo3Target.Awi.h"
#include "Bo3Target.GdtDb.h"
#include "Bo3Target.Output.h"
#include "XAnimBinReader.h"
#include "ExrWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <sstream>

namespace fs = std::filesystem;
namespace Tc = Bo3Target::Techsets;

namespace Bo3Target::Verify
{
    namespace
    {
        // ---- thresholds. Each is a judgement stated in docs/hydrax-port/p1-bo3-verify.md, not a measured BO3 limit.
        constexpr uint32_t kThumb = 128;                  // the sample grid
        constexpr size_t kNameLimit = 63;                 // hypothesis (plan-materials.md conflict 17): a warning only
        constexpr double kNormalFail = 4.0;               // mean |blue - blue implied by red and green|, on 0..255: above this is a failure
        constexpr double kNormalWarn = 2.0;               // ... above this a warning (8-bit rounding of the rebuild is about 1)
        constexpr double kDiffuseOutShare = 0.05;         // more than this share of pixels outside 40..238 is flagged (a warning, never a clamp)
        constexpr int kDiffuseLow = 40, kDiffuseHigh = 238;
        constexpr int kSpecDielectric = 56, kSpecTolerance = 2, kSpecDielectricMax = 90;
        constexpr double kSpecMinNonZero = 0.10;          // below this share of non-zero pixels the map carries no level to judge
        constexpr int kMetalSpecMin = 150, kBareMetalBlack = 24;
        // rules.specular_level and rules.bare_metal are warnings, never failures: measured on stock BO3 sources, stock breaks both
        // guidelines on a large share of its own assets (painted metal, plastic, ice), so a failure would fail Treyarch's own assets. See
        // docs/hydrax-port/p2-specular-gloss.md.
        constexpr double kMetalWarnShare = 0.10;
        constexpr double kScrollMax = 10.0, kRotationMax = 3600.0;   // the awi ranges, restated so a stale awi cannot widen the rule

        const char* const kStatic = "static gate: read from the files against the Mod Tools' deffiles and the techset catalog; "
                                    "not proof that APE, the linker or the game accepts the asset";

        std::string Lower(std::string S)
        {
            for (char& C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            return S;
        }

        bool KeyLess(const std::string& A, const std::string& B)
        {
            return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end(), [](char X, char Y) {
                return std::tolower(static_cast<unsigned char>(X)) < std::tolower(static_cast<unsigned char>(Y));
            });
        }

        std::string Unescape(const std::string& V)
        {
            std::string Out;
            for (size_t I = 0; I < V.size(); I++)
            {
                if (V[I] == '\\' && I + 1 < V.size() && V[I + 1] == '\\') I++;
                Out += V[I];
            }
            return Out;
        }

        bool ParseDouble(const std::string& S, double& Out)
        {
            if (S.empty()) return false;
            char* End = nullptr;
            Out = std::strtod(S.c_str(), &End);
            return End != S.c_str() && *End == '\0' && std::isfinite(Out);
        }

        bool ParseFloats(const std::string& S, std::vector<double>& Out)
        {
            Out.clear();
            std::istringstream In(S);
            std::string Tok;
            while (In >> Tok)
            {
                double V;
                if (!ParseDouble(Tok, V)) return false;
                Out.push_back(V);
            }
            return !Out.empty();
        }

        struct Entry
        {
            std::string Gdf, Name, File, Parent;
            size_t Root = 0, Line = 0;
            std::vector<std::pair<std::string, std::string>> Fields;
            const std::string* Get(const char* Key) const
            {
                for (const auto& F : Fields)
                    if (F.first == Key) return &F.second;
                return nullptr;
            }
            std::string Value(const char* Key) const
            {
                const std::string* V = Get(Key);
                return V ? *V : std::string();
            }
            std::string Id() const { return (Gdf.empty() ? std::string("unresolved") : Gdf) + ":" + Name; }
        };

        // ---- the context a run keeps

        struct Context
        {
            const Inputs& In;
            Result R;
            std::vector<Entry> Entries;
            std::map<std::pair<std::string, std::string>, std::vector<size_t>> ByGdfName;   // (lower gdf, lower name)
            std::map<std::string, std::vector<size_t>> ByName;                              // lower name
            std::map<std::string, std::unique_ptr<Awi::Schema>> Schemas;                    // by gdf; null: not available
            std::unique_ptr<GdtDb::StockImages> Stock;                                      // the semantic of a stock image, read from the Mod Tools' own GDTs
            std::vector<std::string> SlotImageUnread;                                       // "image (why)" of slot images whose semantic could not be read
            std::set<std::string> SchemaWhy;
            std::set<std::string> FailedEntries;
            std::map<std::string, std::shared_ptr<Thumb>> Thumbs;
            std::set<std::string> Unjudged;     // counters for the notes
            size_t UnresolvedNoDb = 0, SlotImageNotInOutput = 0, UnjudgedRange = 0, SlotImageRead = 0;
            size_t ClosureBins = 0, ClosureUnparsed = 0, ClosureNoDb = 0;      // model.material_closure: files read, files not parsed, names that could not be looked up
            std::vector<std::string> ClosureUnparsedExamples, ClosureNoDbExamples;
            std::set<std::string> SkyImages;   // lower-case names of the images a sky_* material uses as its colorMap (only those may be EXR)
            bool HaveAwi = false;
            // material asset (lower case) -> the reason its surfaceType is <none>, from graywolf_approximations.tsv
            std::map<std::string, std::string> SurfaceWhy;
            size_t StockLateRows = 0;                          // material.stock_semantic_late rows: the image is held by an installed GDT, or stock
            std::map<std::string, std::string> Unbound;         // asset (lower case) -> detail of a material.texture_unbound row
            std::map<std::string, std::string> FamilyAssumed;   // asset (lower case) -> what a material.family_* row says was assumed
            std::map<std::string, std::string> F0Failed;        // asset (lower case) -> detail of a material.f0_conversion_failed row
            std::map<std::string, std::string> GlossFailed, RevealFailed;   // likewise for material.gloss_conversion_failed and material.reveal_conversion_failed
            std::map<std::string, std::string> LayersDroppedWhy;  // asset -> detail of a material.layers_dropped row: a blend whose layers are not (all) carried, or whose weights were not read
            std::map<std::string, std::string> FallbackWhy;     // asset (lower case) -> detail of a material.techset_fallback row: the techset was not the game's rule choice
            std::map<std::string, std::string> SkippedWhy;      // asset (lower case) -> "code detail" of a material.skipped_* row: the exporter left this material out on purpose
            bool HaveApproximations = false;
            // asset (lower case) -> (gdt file, held usage) from the material.usage_held rows of graywolf_approximations.tsv: the stand-alone gate has no run behind it, only these rows
            std::map<std::string, std::pair<std::string, std::string>> HeldRows;

            explicit Context(const Inputs& I) : In(I)
            {
                if (I.Approximations.empty()) return;
                std::ifstream File(I.Approximations, std::ios::binary);
                if (!File) return;
                HaveApproximations = true;
                std::string Line;
                while (std::getline(File, Line))
                {
                    if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                    std::vector<std::string> Cells;
                    std::istringstream Row(Line);
                    for (std::string Cell; std::getline(Row, Cell, '\t');) Cells.push_back(Cell);
                    if (Cells.size() >= 3 && Cells[0] == "material.usage_held")
                    {
                        // detail: gdt=<file>, held_usage=<usage>, this_export_would_choose=<usage>, surface=<via>
                        const std::string& D = Cells[2];
                        const size_t G = D.find("gdt="), U = D.find(", held_usage="), W = D.find(", this_export_would_choose=");
                        if (G == 0 && U != std::string::npos && W != std::string::npos && W > U)
                        {
                            std::string Asset = Cells[1];
                            for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                            std::string Gdt = D.substr(4, U - 4);
                            if (Gdt == "(unknown)") Gdt.clear();
                            HeldRows[Asset] = { Gdt, D.substr(U + 13, W - U - 13) };
                        }
                        continue;
                    }
                    if (Cells.size() >= 3 && Cells[0] == "material.stock_semantic_late") StockLateRows++;
                    if (Cells.size() >= 3 && Cells[0] == "material.texture_unbound")
                    {
                        std::string Asset = Cells[1];
                        for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                        Unbound[Asset] = Cells[2];
                        continue;
                    }
                    if (Cells.size() >= 3 && Cells[0].rfind("material.family_", 0) == 0)
                    {
                        std::string Asset = Cells[1];
                        for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                        FamilyAssumed[Asset] = Cells[0].substr(16);
                        continue;
                    }
                    if (Cells.size() >= 3 && (Cells[0] == "material.gloss_conversion_failed" || Cells[0] == "material.reveal_conversion_failed"))
                    {
                        std::string Asset = Cells[1];
                        for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                        (Cells[0] == "material.gloss_conversion_failed" ? GlossFailed : RevealFailed)[Asset] = Cells[2];
                        continue;
                    }
                    if (Cells.size() >= 3 && Cells[0] == "material.f0_conversion_failed")
                    {
                        std::string Asset = Cells[1];
                        for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                        F0Failed[Asset] = Cells[2];
                        continue;
                    }
                    if (Cells.size() >= 3 && Cells[0] == "material.layers_dropped")
                    {
                        std::string Asset = Cells[1];
                        for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                        LayersDroppedWhy[Asset] = Cells[2];
                        continue;
                    }
                    if (Cells.size() >= 3 && Cells[0] == "material.techset_fallback")
                    {
                        std::string Asset = Cells[1];
                        for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                        FallbackWhy[Asset] = Cells[2];
                        continue;
                    }
                    if (Cells.size() < 3 || (Cells[0].rfind("material.surface_", 0) != 0 && Cells[0].rfind("material.skipped_", 0) != 0)) continue;
                    std::string Asset = Cells[1];
                    for (char& Ch : Asset) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                    (Cells[0].rfind("material.skipped_", 0) == 0 ? SkippedWhy : SurfaceWhy)[Asset] = Cells[0] + " " + Cells[2];
                }
            }

            void Judged(const char* Check) { R.Checks[Check].Judged++; }
            void Incomplete(const std::string& Check, const std::string& Why)
            {
                for (const auto& I : R.Incomplete)
                    if (I.first == Check) return;
                R.Incomplete.emplace_back(Check, Why);
            }
            void Add(const char* Check, const std::string& Asset, Severity S, const std::string& Detail)
            {
                R.Findings.push_back({ Check, Asset, S, Detail });
                CheckStat& C = R.Checks[Check];
                if (S == Severity::Failure)
                {
                    C.Failures++;
                    R.Failures++;
                    FailedEntries.insert(Asset);
                }
                else if (S == Severity::Warning)
                {
                    C.Warnings++;
                    R.Warnings++;
                }
            }
            void Fail(const char* Check, const Entry& E, const std::string& Detail) { Add(Check, E.Id(), Severity::Failure, Detail); }
            void Warn(const char* Check, const Entry& E, const std::string& Detail) { Add(Check, E.Id(), Severity::Warning, Detail); }

            // The semantic(s) of an image that is not in this output, from the Mod Tools' own data: a code image ($white_specular) is what the
            // catalog slots that default to it declare; any other name is a stock image, read from the stock GDT the gdt.db names for it.
            // False with Why when it cannot be read: the caller must not count that as a pass.
            bool OutsideSemantics(const std::string& Image, std::vector<std::string>& Semantics, std::string& Why)
            {
                if (!Image.empty() && Image.front() == '$')
                {
                    if (In.Catalog != nullptr)
                        if (const auto* All = In.Catalog->CodeImageSemantics(Image))
                        {
                            for (const auto& [Semantic, Count] : *All) Semantics.push_back(Semantic);
                            return true;
                        }
                    Why = "a code image no catalog slot defaults to: the Mod Tools data names no semantic for it";
                    return false;
                }
                if (In.Held == nullptr || In.ModToolsRoot.empty())
                {
                    Why = "no gdt.db or Mod Tools root to read the stock image from";
                    return false;
                }
                if (!Stock) Stock = std::make_unique<GdtDb::StockImages>(*In.Held, In.ModToolsRoot);
                const GdtDb::StockSemantic Read = Stock->Semantic(Image);
                if (!Read.Known)
                {
                    Why = Read.Why;
                    return false;
                }
                Semantics.push_back(Read.Semantic);
                return true;
            }

            const Awi::Schema* SchemaFor(const std::string& Gdf)
            {
                const auto It = Schemas.find(Gdf);
                if (It != Schemas.end()) return It->second.get();
                std::unique_ptr<Awi::Schema> S;
                if (!In.ModToolsRoot.empty())
                {
                    auto Fresh = std::make_unique<Awi::Schema>();
                    std::string Why;
                    if (Awi::Load(In.ModToolsRoot, Gdf, *Fresh, Why)) S = std::move(Fresh);
                    else SchemaWhy.insert(Gdf + ": " + Why);
                }
                const Awi::Schema* Raw = S.get();
                Schemas[Gdf] = std::move(S);
                if (Raw != nullptr) HaveAwi = true;
                return Raw;
            }

            const Entry* Find(const std::string& Gdf, const std::string& Name) const
            {
                const auto It = ByGdfName.find({ Lower(Gdf), Lower(Name) });
                return It == ByGdfName.end() || It->second.empty() ? nullptr : &Entries[It->second.front()];
            }
        };

        // ---- check 1: the GDT text

        void ReadGdt(Context& C, size_t RootIndex, const fs::path& File, const std::string& Rel)
        {
            const std::string Asset = "file:" + Rel;
            std::ifstream In(File, std::ios::binary);
            if (!In)
            {
                C.Add("gdt.syntax", Asset, Severity::Failure, "cannot read the file");
                return;
            }
            std::ostringstream Os;
            Os << In.rdbuf();
            std::string Text = Os.str();
            C.Judged("gdt.syntax");
            C.Judged("gdt.bom");
            if (Text.size() >= 3 && static_cast<unsigned char>(Text[0]) == 0xEF && static_cast<unsigned char>(Text[1]) == 0xBB && static_cast<unsigned char>(Text[2]) == 0xBF)
            {
                C.Add("gdt.bom", Asset, Severity::Failure, "starts with a UTF-8 BOM (the format is UTF-8 without one)");
                Text.erase(0, 3);
            }
            C.Judged("gdt.empty");
            if (Text.empty())
            {
                C.Add("gdt.empty", Asset, Severity::Failure, "the file holds nothing (0 bytes): there is nothing to judge, and a file that gives nothing is not a pass");
                return;
            }
            const size_t EntriesBefore = C.Entries.size();

            std::vector<std::string> Lines;
            size_t BareLf = 0;
            {
                size_t From = 0;
                while (From < Text.size())
                {
                    size_t To = Text.find('\n', From);
                    const bool Last = To == std::string::npos;
                    if (Last) To = Text.size();
                    std::string Line = Text.substr(From, To - From);
                    if (!Last)
                    {
                        if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                        else BareLf++;
                    }
                    Lines.push_back(std::move(Line));
                    From = To + 1;
                }
            }
            if (BareLf > 0) C.Add("gdt.syntax", Asset, Severity::Failure, std::format("{} line(s) end in LF alone (the format is CRLF)", BareLf));
            if (!Text.empty() && Text.back() != '\n') C.Add("gdt.syntax", Asset, Severity::Warning, "the file does not end with a line break");

            enum class St { Top, Header, Open, Body } State = St::Top;
            bool Opened = false, Closed = false;
            Entry Cur;
            size_t Errors = 0;
            std::string PrevName, PrevKey;
            bool OrderWarnedEntries = false;
            std::set<std::string> NamesInFile;
            const auto SyntaxError = [&](size_t N, const std::string& What) {
                if (++Errors <= 5) C.Add("gdt.syntax", Asset, Severity::Failure, std::format("line {}: {}", N, What));
            };
            for (size_t I = 0; I < Lines.size(); I++)
            {
                const std::string& L = Lines[I];
                const size_t N = I + 1;
                if (L.empty() && I + 1 == Lines.size()) continue;      // the text after the last line break
                if (State == St::Top)
                {
                    if (!Opened)
                    {
                        if (L != "{") SyntaxError(N, "the file must open with a line holding only {");
                        Opened = true;
                        continue;
                    }
                    if (L == "}")
                    {
                        Closed = true;
                        continue;
                    }
                    if (Closed)
                    {
                        SyntaxError(N, "text after the closing }");
                        continue;
                    }
                    // \t"name" ( "gdf.gdf" )   or   \t"name" [ "parent" ]
                    bool Good = false;
                    if (L.size() > 3 && L[0] == '\t' && L[1] == '"')
                    {
                        const size_t Q = L.find('"', 2);
                        if (Q != std::string::npos && Q > 2)
                        {
                            const std::string Rest = L.substr(Q + 1);
                            const size_t Open = Rest.compare(0, 4, " ( \"") == 0 ? 4 : Rest.compare(0, 4, " [ \"") == 0 ? 4 : 0;
                            if (Open != 0)
                            {
                                const bool Paren = Rest[1] == '(';
                                const std::string Tail = Paren ? "\" )" : "\" ]";
                                if (Rest.size() > Open + Tail.size() && Rest.compare(Rest.size() - Tail.size(), Tail.size(), Tail) == 0)
                                {
                                    const std::string Inner = Rest.substr(Open, Rest.size() - Open - Tail.size());
                                    if (Inner.find('"') == std::string::npos && !Inner.empty())
                                    {
                                        Cur = Entry();
                                        Cur.Name = L.substr(2, Q - 2);
                                        Cur.File = Rel;
                                        Cur.Root = RootIndex;
                                        Cur.Line = N;
                                        if (Paren)
                                        {
                                            Cur.Gdf = Inner;
                                            if (Cur.Gdf.size() > 4 && Cur.Gdf.compare(Cur.Gdf.size() - 4, 4, ".gdf") == 0) Cur.Gdf.resize(Cur.Gdf.size() - 4);
                                            else SyntaxError(N, "the type must end in .gdf: " + Inner);
                                        }
                                        else Cur.Parent = Inner;
                                        Good = true;
                                    }
                                }
                            }
                        }
                    }
                    if (!Good)
                    {
                        SyntaxError(N, "expected a tab, the quoted name, then ( \"x.gdf\" ) or [ \"parent\" ]");
                        continue;
                    }
                    State = St::Open;
                    PrevKey.clear();
                    continue;
                }
                if (State == St::Open)
                {
                    if (L != "\t{") SyntaxError(N, "expected a tab and { after the entry header");
                    State = St::Body;
                    continue;
                }
                // body
                if (L == "\t}")
                {
                    // entry done
                    if (!Cur.Gdf.empty() || !Cur.Parent.empty())
                    {
                        if (!PrevName.empty() && Cur.Name < PrevName && !OrderWarnedEntries)
                        {
                            OrderWarnedEntries = true;
                            C.Add("gdt.order", Asset, Severity::Warning, std::format("entry \"{}\" (line {}) comes after \"{}\": entries are written in ordinal name order", Cur.Name, Cur.Line, PrevName));
                        }
                        C.Judged("gdt.order");
                        PrevName = Cur.Name;
                        // an image and an xmodel may share a name in one file: the stock t7_skybox.gdt has skybox_<map> as both, so the name is unique per GDF
                        const std::string Key = Lower(Cur.Gdf) + ":" + Lower(Cur.Name);
                        if (!NamesInFile.insert(Key).second)
                            C.Add("gdt.duplicate_name", Asset, Severity::Failure, std::format("name \"{}\" is defined twice in this file (names are unique case-insensitively, G3)", Cur.Name));
                        C.Judged("gdt.duplicate_name");
                        C.Entries.push_back(Cur);
                    }
                    State = St::Top;
                    continue;
                }
                // \t\t"key" "value"
                bool Good = false;
                if (L.size() > 6 && L[0] == '\t' && L[1] == '\t' && L[2] == '"')
                {
                    const size_t KEnd = L.find('"', 3);
                    if (KEnd != std::string::npos && L.compare(KEnd + 1, 2, " \"") == 0 && L.back() == '"' && L.size() >= KEnd + 4)
                    {
                        const std::string Key = L.substr(3, KEnd - 3);
                        const std::string Value = L.substr(KEnd + 3, L.size() - KEnd - 4);
                        if (Value.find('"') != std::string::npos)
                            C.Add("gdt.unsafe_value", Cur.Id(), Severity::Failure, std::format("line {}: the value of {} holds a double quote (nothing is escaped, G2)", N, Key));
                        C.Judged("gdt.unsafe_value");
                        if (!PrevKey.empty() && KeyLess(Key, PrevKey))
                            C.Add("gdt.order", Cur.Id(), Severity::Warning, std::format("line {}: key {} comes after {} (keys are written sorted, case aside)", N, Key, PrevKey));
                        PrevKey = Key;
                        Cur.Fields.emplace_back(Key, Value);
                        Good = true;
                    }
                }
                if (!Good) SyntaxError(N, "expected two tabs, a quoted key and a quoted value (a value with a line break breaks the file)");
            }
            if (State != St::Top) SyntaxError(Lines.size(), "the file ends inside an entry");
            if (Opened && !Closed) SyntaxError(Lines.size(), "the file does not end with a line holding only }");
            if (Errors > 5) C.Add("gdt.syntax", Asset, Severity::Failure, std::format("{} further syntax errors not listed", Errors - 5));
            if (C.Entries.size() == EntriesBefore) C.Add("gdt.empty", Asset, Severity::Failure, "the file holds no entries: there is nothing to judge, and a file that gives nothing is not a pass");
        }

        // ---- image headers

        uint32_t U16(std::span<const uint8_t> P, bool Big) { return Big ? (uint32_t(P[0]) << 8 | P[1]) : (uint32_t(P[1]) << 8 | P[0]); }
        uint32_t U32(std::span<const uint8_t> P, bool Big)
        {
            return Big ? (uint32_t(P[0]) << 24 | uint32_t(P[1]) << 16 | uint32_t(P[2]) << 8 | P[3]) : (uint32_t(P[3]) << 24 | uint32_t(P[2]) << 16 | uint32_t(P[1]) << 8 | P[0]);
        }
    }

    const char* SeverityName(Severity S)
    {
        return S == Severity::Failure ? "failure" : S == Severity::Warning ? "warning" : "info";
    }

    ImageInfo ReadImageHeader(const std::function<bool(uint64_t, void*, size_t)>& Read)
    {
        ImageInfo Out;
        uint8_t Head[32] = {};
        if (!Read(0, Head, 8))
        {
            Out.Why = "the file is shorter than a header";
            return Out;
        }
        if (std::memcmp(Head, "\x89PNG\r\n\x1a\n", 8) == 0)
        {
            Out.Container = "png";
            if (!Read(8, Head, 8) || std::memcmp(&Head[4], "IHDR", 4) != 0)
            {
                Out.Why = "PNG without an IHDR chunk";
                return Out;
            }
            if (U32(Head, true) != 13)
            {
                Out.Why = std::format("PNG IHDR chunk length is {}, not 13", U32(Head, true));
                return Out;
            }
            uint8_t Ihdr[13] = {};
            if (!Read(16, Ihdr, 13))
            {
                Out.Why = "PNG IHDR is cut short";
                return Out;
            }
            Out.Width = U32(Ihdr, true);
            Out.Height = U32(std::span<const uint8_t>(Ihdr).subspan(4), true);
            if (Out.Width == 0 || Out.Height == 0)
            {
                Out.Why = "PNG with a zero width or height";
                return Out;
            }
            Out.BitsPerChannel = Ihdr[8];
            switch (Ihdr[9])
            {
            case 0: Out.Channels = 1; break;
            case 2: Out.Channels = 3; break;
            case 4: Out.Channels = 2; break;
            case 6: Out.Channels = 4; break;
            default: Out.Why = "an indexed PNG (colour type 3) has no channel count of its own"; return Out;
            }
            Out.Ok = true;
            return Out;
        }
        const bool Little = Head[0] == 'I' && Head[1] == 'I', Big = Head[0] == 'M' && Head[1] == 'M';
        if (!Little && !Big)
        {
            Out.Why = "neither a TIFF nor a PNG";
            return Out;
        }
        Out.Container = "tiff";
        const uint32_t Magic = U16(std::span<const uint8_t>(Head).subspan(2), Big);
        if (Magic != 42)
        {
            Out.Why = Magic == 43 ? "BigTIFF is not read" : "bad TIFF magic";
            return Out;
        }
        const uint64_t Ifd = U32(std::span<const uint8_t>(Head).subspan(4), Big);
        uint8_t Count[2];
        if (!Read(Ifd, Count, 2))
        {
            Out.Why = "the TIFF directory is past the end of the file";
            return Out;
        }
        const uint32_t N = U16(Count, Big);
        std::vector<uint8_t> Tags(static_cast<size_t>(N) * 12);
        if (N == 0 || !Read(Ifd + 2, Tags.data(), Tags.size()))     // 64-bit: Ifd + 2 cannot wrap
        {
            Out.Why = "the TIFF directory is cut short";
            return Out;
        }
        uint32_t Samples = 1;
        bool HaveW = false, HaveH = false;
        // BitsPerSample and SampleFormat hold one value per sample: inline in the entry when they fit in 4 bytes, else through a pointer
        struct Array { bool Present = false; uint32_t Count = 0; std::span<const uint8_t> Cell; };
        Array BitsTag, FormatTag;
        for (uint32_t I = 0; I < N; I++)
        {
            const std::span<const uint8_t> T = std::span<const uint8_t>(Tags).subspan(static_cast<size_t>(I) * 12, 12);
            const uint32_t Tag = U16(T, Big), Type = U16(T.subspan(2), Big), Cnt = U32(T.subspan(4), Big);
            const auto Scalar = [&]() { return Type == 3 ? U16(T.subspan(8), Big) : U32(T.subspan(8), Big); };
            switch (Tag)
            {
            case 256: Out.Width = Scalar(); HaveW = true; break;
            case 257: Out.Height = Scalar(); HaveH = true; break;
            case 258: BitsTag = { true, Cnt, T }; break;
            case 277: Samples = Scalar(); break;
            case 339: FormatTag = { true, Cnt, T }; break;
            default: break;
            }
        }
        if (!HaveW || !HaveH)
        {
            Out.Why = "the TIFF has no width or height tag";
            return Out;
        }
        if (Out.Width == 0 || Out.Height == 0)
        {
            Out.Why = "TIFF with a zero width or height";
            return Out;
        }
        if (Samples == 0 || Samples > 16)
        {
            Out.Why = std::format("TIFF with {} samples per pixel", Samples);
            return Out;
        }
        const auto Values = [&](const Array& A, uint32_t Default, std::vector<uint32_t>& Into) {
            Into.clear();
            if (!A.Present)
            {
                Into.assign(Samples, Default);
                return true;
            }
            if (A.Count == 0 || A.Count > 64) return false;
            if (A.Count * 2 <= 4)
                for (uint32_t K = 0; K < A.Count; K++) Into.push_back(U16(A.Cell.subspan(8 + 2 * static_cast<size_t>(K)), Big));
            else
            {
                std::vector<uint8_t> Raw(static_cast<size_t>(A.Count) * 2);
                if (!Read(U32(A.Cell.subspan(8), Big), Raw.data(), Raw.size())) return false;
                for (uint32_t K = 0; K < A.Count; K++) Into.push_back(U16(std::span<const uint8_t>(Raw).subspan(2 * static_cast<size_t>(K)), Big));
            }
            if (Into.size() == 1) Into.assign(Samples, Into[0]);
            return Into.size() == Samples;
        };
        std::vector<uint32_t> Bits, Format;
        if (!Values(BitsTag, 1, Bits))
        {
            Out.Why = "BitsPerSample could not be read for every sample";
            return Out;
        }
        if (!Values(FormatTag, 1, Format))
        {
            Out.Why = "SampleFormat could not be read for every sample";
            return Out;
        }
        for (uint32_t B : Bits)
            if (B != Bits[0])
            {
                Out.Why = "the samples have different bit depths";
                return Out;
            }
        for (uint32_t F : Format)
        {
            if (F != Format[0])
            {
                Out.Why = "the samples have different sample formats";
                return Out;
            }
            if (F != 1 && F != 3)
            {
                Out.Why = std::format("sample format {} (signed or undefined data); the sources are unsigned integer or float", F);
                return Out;
            }
        }
        Out.BitsPerChannel = Bits[0];
        Out.Channels = Samples;
        Out.Float = Format[0] == 3;
        Out.Ok = true;
        return Out;
    }

    ImageInfo ReadImageHeaderFile(const fs::path& File)
    {
        std::ifstream In(File, std::ios::binary);
        if (!In)
        {
            ImageInfo Out;
            Out.Why = "cannot open the file";
            return Out;
        }
        return ReadImageHeader([&](uint64_t At, void* Into, size_t Size) {
            In.clear();
            In.seekg(static_cast<std::streamoff>(At));
            In.read(static_cast<char*>(Into), static_cast<std::streamsize>(Size));
            return static_cast<size_t>(In.gcount()) == Size;
        });
    }

    // ---- pixel statistics

    Thumb MakeThumb(const Rgba8Image& Image)
    {
        Thumb T;
        if (!Image.IsValid()) return T;
        T.Width = T.Height = kThumb;
        T.Rgba.resize(static_cast<size_t>(kThumb) * kThumb * 4);
        for (uint32_t Y = 0; Y < kThumb; Y++)
            for (uint32_t X = 0; X < kThumb; X++)
            {
                const size_t SX = std::min<size_t>(Image.Width - 1, (static_cast<uint64_t>(X) * 2 + 1) * Image.Width / (2 * kThumb));
                const size_t SY = std::min<size_t>(Image.Height - 1, (static_cast<uint64_t>(Y) * 2 + 1) * Image.Height / (2 * kThumb));
                std::memcpy(&T.Rgba[(static_cast<size_t>(Y) * kThumb + X) * 4], &Image.Pixels[(SY * Image.Width + SX) * 4], 4);
            }
        return T;
    }

    NormalStat NormalBlue(const Thumb& T)
    {
        NormalStat S;
        double Sum = 0;
        for (size_t I = 0; I + 3 < T.Rgba.size(); I += 4)
        {
            const double X = T.Rgba[I] / 255.0 * 2 - 1, Y = T.Rgba[I + 1] / 255.0 * 2 - 1;
            const double Z = std::sqrt(std::max(0.0, 1 - X * X - Y * Y));
            const double Expect = (Z + 1) / 2 * 255;
            const double Diff = std::fabs(T.Rgba[I + 2] - Expect);
            Sum += Diff;
            S.Max = std::max(S.Max, Diff);
            S.Pixels++;
        }
        S.MeanAbs = S.Pixels ? Sum / static_cast<double>(S.Pixels) : 0;
        return S;
    }

    RangeStat DiffuseRange(const Thumb& T)
    {
        RangeStat S;
        for (size_t I = 0; I + 3 < T.Rgba.size(); I += 4)
        {
            const int Mean = (T.Rgba[I] + T.Rgba[I + 1] + T.Rgba[I + 2] + 1) / 3;
            S.Pixels++;
            if (Mean < kDiffuseLow) S.Below++;
            else if (Mean > kDiffuseHigh) S.Above++;
        }
        return S;
    }

    SpecStat SpecularMode(const Thumb& T)
    {
        SpecStat S;
        size_t Hist[256] = {};
        for (size_t I = 0; I + 3 < T.Rgba.size(); I += 4)
        {
            const int Mean = (T.Rgba[I] + T.Rgba[I + 1] + T.Rgba[I + 2] + 1) / 3;
            S.Pixels++;
            if (Mean > 0)
            {
                Hist[Mean]++;
                S.NonZero++;
            }
        }
        size_t Best = 0;
        for (int V = 1; V < 256; V++)
            if (Hist[V] > Best)
            {
                Best = Hist[V];
                S.Mode = V;
            }
        S.Share = S.NonZero ? static_cast<double>(Best) / static_cast<double>(S.NonZero) : 0;
        return S;
    }

    MetalStat BareMetal(const Thumb& Diffuse, const Thumb& Specular)
    {
        MetalStat S;
        if (Diffuse.Rgba.size() != Specular.Rgba.size()) return S;
        for (size_t I = 0; I + 3 < Specular.Rgba.size(); I += 4)
        {
            const int Spec = (Specular.Rgba[I] + Specular.Rgba[I + 1] + Specular.Rgba[I + 2] + 1) / 3;
            if (Spec < kMetalSpecMin) continue;
            S.MetalPixels++;
            const int Diff = (Diffuse.Rgba[I] + Diffuse.Rgba[I + 1] + Diffuse.Rgba[I + 2] + 1) / 3;
            if (Diff > kBareMetalBlack) S.NotBlack++;
        }
        return S;
    }

    namespace
    {
        // ---- check 2: the .awi schema

        bool IsNumberText(const std::string& V, double& Out) { return ParseDouble(V, Out); }

        // Root / Dir / Value, only when Value is a relative path that stays under Root: an absolute path, a drive, or enough ".." to
        // leave the layout does not "resolve under the layout".
        bool UnderRoot(const fs::path& Root, const std::string& Dir, const std::string& Value, fs::path& Out)
        {
            const fs::path V(Value);
            if (Value.empty() || V.has_root_name() || V.has_root_directory() || V.is_absolute()) return false;
            Out = Root / fs::path(Dir) / V;
            return Output::IsUnder(Root, Out);
        }

        // Do two files hold the same bytes? Sizes first, then a block-wise compare. False with Why when either cannot be read.
        bool SameBytes(const fs::path& A, const fs::path& B, bool& Same, std::string& Why)
        {
            std::error_code Ec;
            const auto SizeA = fs::file_size(A, Ec);
            if (Ec) { Why = "cannot size " + A.filename().string(); return false; }
            const auto SizeB = fs::file_size(B, Ec);
            if (Ec) { Why = "cannot size " + B.filename().string(); return false; }
            if (SizeA != SizeB) { Same = false; return true; }
            std::ifstream FA(A, std::ios::binary), FB(B, std::ios::binary);
            if (!FA || !FB) { Why = "cannot open " + (FA ? B : A).filename().string(); return false; }
            std::vector<char> BA(1 << 16), BB(1 << 16);
            std::uintmax_t Total = 0;
            for (;;)
            {
                FA.read(BA.data(), static_cast<std::streamsize>(BA.size()));
                FB.read(BB.data(), static_cast<std::streamsize>(BB.size()));
                const auto NA = static_cast<size_t>(FA.gcount()), NB = static_cast<size_t>(FB.gcount());
                if (FA.bad() || FB.bad()) { Why = "a read failed"; return false; }
                // the sizes are equal, so a short read on one side only is a failed read, not a difference
                if (NA != NB) { Why = "a read stopped early on one file"; return false; }
                if (std::memcmp(BA.data(), BB.data(), NA) != 0) { Same = false; return true; }
                Total += NA;
                if (NA == 0 || !FA || !FB) break;
            }
            if (Total != SizeA) { Why = std::format("only {} of {} bytes were read", Total, SizeA); return false; }
            Same = true;
            return true;
        }

        void CheckSchema(Context& C, const Entry& E, const std::string& RootText)
        {
            if (E.Name.size() > kNameLimit)
                C.Warn("awi.name_length", E, std::format("the name is {} characters; 63 is a hypothesis (one proven case), not a measured limit", E.Name.size()));
            C.Judged("awi.name_length");

            C.Judged("awi.schema");
            const Awi::Schema* S = C.SchemaFor(E.Gdf);
            if (S == nullptr)
            {
                // with no Mod Tools root at all the run is incomplete (Run says so); with one, an entry whose GDF has no readable awi
                // could not be judged, which is a failure
                if (!C.In.ModToolsRoot.empty())
                    C.Fail("awi.schema", E, std::format("no readable deffiles\\{}.awi under the Mod Tools root: this entry's keys cannot be judged", E.Gdf));
                return;
            }
            for (const auto& [Key, Value] : E.Fields)
            {
                const Awi::Entry* D = S->Find(Key);
                C.Judged("awi.unknown_key");
                if (D == nullptr)
                {
                    C.Fail("awi.unknown_key", E, std::format("{}.awi does not declare the key \"{}\"", E.Gdf, Key));
                    continue;
                }
                double V = 0;
                switch (D->K)
                {
                case Awi::Kind::Float:
                case Awi::Kind::Int:
                {
                    if (D->RangeUnknown) C.UnjudgedRange++;
                    else C.Judged("awi.range");
                    if (!IsNumberText(Value, V) || (D->K == Awi::Kind::Int && V != std::floor(V)))
                    {
                        C.Fail("awi.type", E, std::format("{} = \"{}\" is not {}", Key, Value, D->K == Awi::Kind::Int ? "an integer" : "a number"));
                        break;
                    }
                    if ((D->HasMin && V < D->Min) || (D->HasMax && V > D->Max))
                        C.Fail("awi.range", E, std::format("{} = {} is outside {}..{}", Key, Value, D->HasMin ? std::format("{}", D->Min) : "-", D->HasMax ? std::format("{}", D->Max) : "-"));
                    break;
                }
                case Awi::Kind::CheckBox:
                    if (Value != "0" && Value != "1") C.Fail("awi.type", E, std::format("{} = \"{}\" is a checkbox: 0 or 1", Key, Value));
                    break;
                case Awi::Kind::Color:
                {
                    std::vector<double> Parts;
                    if (!ParseFloats(Value, Parts) || (Parts.size() != 3 && Parts.size() != 4))
                        C.Fail("awi.type", E, std::format("{} = \"{}\" is a colour: three or four numbers", Key, Value));
                    break;
                }
                case Awi::Kind::Combo:
                    if (D->OptionsKnown) C.Judged("awi.combo");
                    if (D->OptionsKnown && !Awi::Schema::HasOption(*D, Value))
                        C.Fail("awi.combo", E, std::format("{} = \"{}\" is not one of the {} options {}.awi gives", Key, Value, D->Options.size(), E.Gdf));
                    break;
                case Awi::Kind::AssetCombo:
                {
                    if (Value.empty() || Value[0] == '$' || D->Target.empty()) break;
                    C.Judged("awi.reference");
                    if (const Entry* T = C.Find(D->Target, Value); T != nullptr) break;
                    // defined in the output with another type?
                    const auto Other = C.ByName.find(Lower(Value));
                    if (Other != C.ByName.end() && !Other->second.empty())
                    {
                        bool Right = false;
                        for (size_t I : Other->second) Right = Right || Lower(C.Entries[I].Gdf) == Lower(D->Target);
                        if (!Right)
                        {
                            C.Fail("awi.reference_type", E, std::format("{} names \"{}\", which is a {} in this output; {}.awi wants a {}", Key, Value, C.Entries[Other->second.front()].Gdf, E.Gdf, D->Target));
                            break;
                        }
                    }
                    if (C.In.Held == nullptr) { C.UnresolvedNoDb++; break; }
                    if (C.In.Held->Contains(D->Target, Value)) break;
                    const std::vector<std::string> Types = C.In.Held->TypesOf(Value);
                    if (!Types.empty() && C.In.Held->HasTypes())
                        C.Fail("awi.reference_type", E, std::format("{} names \"{}\", which the gdt.db holds as {}; {}.awi wants a {}", Key, Value, Types.front(), E.Gdf, D->Target));
                    else
                        C.Warn("awi.reference_unresolved", E, std::format("{} names {} \"{}\", which is in no GDT of this output and not in the gdt.db", Key, D->Target, Value));
                    break;
                }
                case Awi::Kind::XModel:
                case Awi::Kind::Path:
                {
                    if (Value.empty()) break;
                    C.Judged("awi.file");
                    std::string Rel = D->RelativePath;
                    std::replace(Rel.begin(), Rel.end(), '/', '\\');
                    fs::path File;
                    std::error_code Ec;
                    bool Found = UnderRoot(fs::path(RootText), Rel, Unescape(Value), File) && fs::is_regular_file(File, Ec);
                    // a model the output only names (a sky xmodel names the stock sky mesh, never copied) resolves under the Mod Tools root
                    if (!Found && D->K == Awi::Kind::XModel && !C.In.ModToolsRoot.empty() && Lower(Unescape(Value)) == "t6_props\\vista\\skybox\\t6_skybox.xmodel_bin")
                        Found = UnderRoot(C.In.ModToolsRoot, Rel, Unescape(Value), File) && fs::is_regular_file(File, Ec);
                    if (!Found)
                        C.Fail("awi.file", E, std::format("{} = \"{}\" does not resolve to a file under the layout root ({}{}); an absolute path or a .. that leaves the root does not count", Key, Value, Rel, Unescape(Value)));
                    break;
                }
                default: break;
                }
            }
            // required: the keys the later checks and the Mod Tools cannot do without. APE's .awi has no required marker.
            const auto Required = [&](const char* Key) {
                C.Judged("awi.required");
                const std::string* V = E.Get(Key);
                if (V == nullptr || V->empty()) C.Fail("awi.required", E, std::format("{} has no \"{}\"", E.Gdf, Key));
            };
            if (Lower(E.Gdf) == "image")
            {
                Required("semantic");
                if (E.Value("type") != "composite") Required("baseImage");
            }
            else if (Lower(E.Gdf) == "xmodel") Required("filename");
            else if (Lower(E.Gdf) == "material") Required("materialType");
        }

        // ---- images

        std::shared_ptr<Thumb> ThumbOf(Context& C, const fs::path& File, const Entry& E)
        {
            const std::string Key = Lower(File.generic_string());
            if (const auto It = C.Thumbs.find(Key); It != C.Thumbs.end()) return It->second;
            Rgba8Image Image;
            std::string Why;
            std::shared_ptr<Thumb> T;
            if (LoadRgba8(File, Image, Why))
            {
                T = std::make_shared<Thumb>(MakeThumb(Image));
                C.R.ImagesDecoded++;
            }
            else C.Fail("image.decode", E, std::format("the pixels of {} could not be read ({}); the pixel rules cannot be judged", File.filename().string(), Why));
            C.Thumbs[Key] = T;
            return T;
        }

        std::vector<int> ChannelsFor(const std::string& Semantic)
        {
            // stock headers: specularMask is 1 channel on 51 of 150 and 3 on 98, so both are accepted; gloss (126/150), occlusion (142/150),
            // reveal (136/150) and thickness (32/36) are one channel
            if (Semantic == "specularMask") return { 1, 3 };
            if (Semantic == "glossMap" || Semantic == "occlusionMap" || Semantic == "revealMap" || Semantic == "thicknessMap" || Semantic == "One Channel") return { 1 };
            if (Semantic == "diffuseMap" || Semantic == "effectMap" || Semantic == "specularMap" || Semantic == "normalMap") return { 3, 4 };
            if (Semantic == "Two Channel") return { 2 };
            if (Semantic == "camoMap") return { 3 };
            if (Semantic == "multipleMask" || Semantic == "2d") return { 3, 4 };
            return {};
        }

        bool IsCompressed(const std::string& Method)
        {
            return !Method.empty() && Method != "uncompressed" && Method.rfind("R", 0) != 0;   // "compressed...", "BC7", "shared exponent"; the R8/R16 formats are not block compressed
        }

        std::string ExrProblem(const fs::path& File);

        void CheckImage(Context& C, const Entry& E)
        {
            C.R.Images++;
            const std::string Semantic = E.Value("semantic");
            const std::string Core = E.Value("coreSemantic"), Method = E.Value("compressionMethod");
            const bool Composite = E.Value("type") == "composite";

            // semantic table (I3): coreSemantic and compression
            if (!Semantic.empty())
            {
                static const std::set<std::string> Known = { "diffuseMap", "effectMap", "specularMap", "normalMap", "glossMap", "occlusionMap", "revealMap", "thicknessMap", "specularMask", "One Channel" };
                if (Known.count(Semantic))
                {
                    C.Judged("image.semantic_table");
                    const Tc::Form F = Tc::FormOf(Semantic);
                    if (Core != F.Core) C.Fail("image.semantic_table", E, std::format("coreSemantic \"{}\" for semantic {}; the stock table has {}", Core, Semantic, F.Core));
                    const bool Colour = Semantic == "diffuseMap" || Semantic == "effectMap";
                    const bool Ok = Method == F.Compression || Method == "uncompressed" || (Colour && Method.rfind("compressed ", 0) == 0);
                    if (!Ok) C.Fail("image.semantic_table", E, std::format("compressionMethod \"{}\" for semantic {}; the stock table has \"{}\" (or uncompressed)", Method, Semantic, F.Compression));
                }
            }
            if (const std::string* Srgb = E.Get("colorSRGB"); Srgb != nullptr && *Srgb != "0")
                C.Fail("image.srgb", E, "colorSRGB is " + *Srgb + "; stock images are 0 (the file holds the display-encoded value)");
            C.Judged("image.srgb");

            if (Composite) return;
            const std::string Base = Unescape(E.Value("baseImage"));
            if (Base.empty()) return;     // awi.required said so

            if (Lower(fs::path(Base).extension().string()) == ".dds") C.Fail("image.dds", E, "baseImage is a .dds; stock has no .dds sources (TIFF or PNG)");
            C.Judged("image.dds");

            const fs::path Root = C.In.Roots[E.Root];
            fs::path File;
            std::error_code Ec;
            C.Judged("image.source");
            if (!UnderRoot(Root, "", Base, File) || !fs::is_regular_file(File, Ec))
            {
                C.Fail("image.source", E, "baseImage does not resolve under the layout root (an absolute path or a .. that leaves it does not count): " + Base);
                return;
            }
            // a copy of this output into the Mod Tools root would overwrite an installed file of the same path with other bytes (read-only here)
            if (!C.In.ModToolsRoot.empty())
            {
                // absent (no such file) is told from could-not-inspect (a path that cannot be placed, an error, not a regular file, an unreadable file): the second is incomplete
                const auto Unknown = [&](const std::string& Why) { C.Incomplete("install.image_overwrite (part)", "an output baseImage could not be checked against the Mod Tools root (e.g. " + Base + "): " + Why); };
                fs::path Installed;
                if (!UnderRoot(C.In.ModToolsRoot, "", Base, Installed)) Unknown("the path cannot be placed under the Mod Tools root");
                else
                {
                    std::error_code E2;
                    const fs::file_status St = fs::status(Installed, E2);
                    if (St.type() == fs::file_type::not_found) {}      // nothing installed there: no collision
                    else if (E2 || St.type() == fs::file_type::none) Unknown("the installed path could not be inspected: " + E2.message());
                    else if (St.type() != fs::file_type::regular) Unknown("the installed path exists and is not a regular file");
                    else
                    {
                        const bool Equivalent = fs::equivalent(Installed, File, E2);
                        if (E2) Unknown("the installed file could not be compared with the output file: " + E2.message());
                        else if (!Equivalent)
                        {
                            C.Judged("install.image_overwrite");
                            bool Same = true;
                            std::string Why;
                            if (!SameBytes(Installed, File, Same, Why)) Unknown("the bytes could not be compared (" + Why + ")");
                            else if (!Same) C.Warn("install.image_overwrite", E, "baseImage " + Base + " already exists in the Mod Tools root with different bytes: copying this output there would replace it");
                        }
                    }
                }
            }
            // the lat-long EXR of a sky material: its structure is checked (the pixel rules are for TIFF and PNG); any other image, whatever its extension, goes on to those rules
            if (C.SkyImages.count(Lower(E.Name)) != 0 && Lower(File.extension().string()) == ".exr")
            {
                C.Judged("image.format");
                if (const std::string Problem = ExrProblem(File); !Problem.empty()) C.Fail("image.format", E, "the sky EXR is not a readable OpenEXR file: " + Problem);
                return;
            }
            const ImageInfo Info = ReadImageHeaderFile(File);
            C.Judged("image.format");
            if (!Info.Ok)
            {
                C.Fail("image.format", E, "header not read: " + Info.Why);
                return;
            }
            if (Info.Float || (Info.BitsPerChannel != 8 && Info.BitsPerChannel != 16))
                C.Fail("image.bit_depth", E, std::format("{}-bit {} {}; the sources are 8 or 16 bit integer", Info.BitsPerChannel, Info.Float ? "float" : "integer", Info.Container));
            C.Judged("image.bit_depth");
            const std::vector<int> Want = ChannelsFor(Semantic);
            if (!Want.empty())
            {
                C.Judged("image.channels");
                if (std::find(Want.begin(), Want.end(), static_cast<int>(Info.Channels)) == Want.end())
                {
                    std::string List;
                    for (int W : Want) List += (List.empty() ? "" : " or ") + std::to_string(W);
                    // a one-channel map stored in more channels is acceptable to read only when it is grey (R = G = B)
                    bool Grey = false, Judged = false;
                    if (std::find(Want.begin(), Want.end(), 1) != Want.end() && Info.Channels > 1)
                    {
                        if (const auto T = ThumbOf(C, File, E); T && !T->Rgba.empty())
                        {
                            Judged = true;
                            size_t Off = 0;
                            for (size_t I = 0; I + 3 < T->Rgba.size(); I += 4)
                                if (T->Rgba[I] != T->Rgba[I + 1] || T->Rgba[I] != T->Rgba[I + 2]) Off++;
                            Grey = Off * 1000 <= T->Rgba.size() / 4;
                        }
                    }
                    if (Grey)
                        C.Warn("image.channels", E, std::format("{} channels for the one-channel semantic {} (expected {}); the pixels are grey (R = G = B), so a reader that takes one channel gets the same data", Info.Channels, Semantic, List));
                    else
                        C.Fail("image.channels", E, std::format("{} channel(s) for semantic {}; expected {}{}", Info.Channels, Semantic, List, Judged ? " (and the pixels are not grey)" : ""));
                }
            }
            if ((Info.Width % 4 != 0 || Info.Height % 4 != 0) && IsCompressed(Method))
                C.Fail("image.block_size", E, std::format("{}x{} with compressionMethod \"{}\": compressed images need width and height that are multiples of 4 (or mark it uncompressed)", Info.Width, Info.Height, Method));
            C.Judged("image.block_size");

            // pixel rules
            if (Semantic == "normalMap" || Semantic == "diffuseMap" || Semantic == "specularMap")
            {
                const auto T = ThumbOf(C, File, E);
                if (!T || T->Rgba.empty()) return;
                if (Semantic == "normalMap")
                {
                    const NormalStat N = NormalBlue(*T);
                    C.Judged("image.normal_z");
                    if (N.MeanAbs > kNormalFail)
                        C.Fail("image.normal_z", E, std::format("blue differs from the Z implied by red and green by {:.1f} on average (max {:.0f}); a normal map carries Z in blue (tolerance {:.0f})", N.MeanAbs, N.Max, kNormalFail));
                    else if (N.MeanAbs > kNormalWarn)
                        C.Warn("image.normal_z", E, std::format("blue is {:.1f} from the implied Z on average (max {:.0f}); above the {:.0f} of an 8-bit rebuild", N.MeanAbs, N.Max, kNormalWarn));
                }
                else if (Semantic == "diffuseMap")
                {
                    const RangeStat R = DiffuseRange(*T);
                    C.Judged("rules.diffuse_range");
                    const double Out = R.Pixels ? static_cast<double>(R.Below + R.Above) / static_cast<double>(R.Pixels) : 0;
                    if (Out > kDiffuseOutShare)
                        C.Warn("rules.diffuse_range", E, std::format("{:.1f}% of the pixels are below {} and {:.1f}% above {} (8-bit encoded mean); nothing was clamped. Pure metal is exempt by the rule and cannot be told apart here",
                                                                     100.0 * static_cast<double>(R.Below) / static_cast<double>(R.Pixels), kDiffuseLow, 100.0 * static_cast<double>(R.Above) / static_cast<double>(R.Pixels), kDiffuseHigh));
                }
                else
                {
                    const SpecStat S = SpecularMode(*T);
                    C.Judged("rules.specular_level");
                    if (S.Pixels && static_cast<double>(S.NonZero) / static_cast<double>(S.Pixels) >= kSpecMinNonZero && S.Mode > 0 && S.Mode <= kSpecDielectricMax && std::abs(S.Mode - kSpecDielectric) > kSpecTolerance)
                        C.Warn("rules.specular_level", E, std::format("the most common non-zero level is {} ({:.0f}% of them); a non-metal's specular is usually {}/255 (the stock mode; stock maps differ on purpose for gold, copper, brass, ice and plastic, so this is a guideline; nothing was changed)", S.Mode, 100 * S.Share, kSpecDielectric));
                }
            }
        }

        // The structure of an OpenEXR scanline file: magic, version 2 without flags, the attributes the sky needs (channels R G B, compression, dataWindow), the offset table and the
        // last chunk inside the file. "" when sound.
        std::string ExrProblem(const fs::path& File)
        {
            std::error_code Ec;
            const std::uintmax_t Size = fs::file_size(File, Ec);
            std::ifstream In(File, std::ios::binary);
            if (Ec || !In || Size < 16 || Size > (std::uintmax_t(1) << 30)) return "unreadable or too small";
            std::string Head(static_cast<size_t>(std::min<std::uintmax_t>(Size, 65536)), '\0');
            In.read(Head.data(), static_cast<std::streamsize>(Head.size()));
            const auto U8 = [&](size_t At) { return At < Head.size() ? static_cast<uint8_t>(Head[At]) : 0u; };
            const auto I32 = [&](size_t At) { return static_cast<int32_t>(U8(At) | (U8(At + 1) << 8) | (U8(At + 2) << 16) | (static_cast<uint32_t>(U8(At + 3)) << 24)); };
            if (U8(0) != 0x76 || U8(1) != 0x2f || U8(2) != 0x31 || U8(3) != 0x01) return "no OpenEXR magic number";
            if (U8(4) != 2 || U8(5) != 0 || U8(6) != 0 || U8(7) != 0) return "not version 2 without flags (tiled, deep or multipart)";
            size_t At = 8;
            int Compression = -1, X0 = 0, Y0 = 0, X1 = -1, Y1 = -1;
            bool Channels = false, R = false, G = false, B = false;
            while (At < Head.size() && U8(At) != 0)
            {
                const size_t NameEnd = Head.find('\0', At), TypeEnd = NameEnd == std::string::npos ? NameEnd : Head.find('\0', NameEnd + 1);
                if (TypeEnd == std::string::npos || TypeEnd + 5 > Head.size()) return "the header is cut short or over 64 KiB";
                const std::string Name = Head.substr(At, NameEnd - At);
                const int32_t Len = I32(TypeEnd + 1);
                const size_t Body = TypeEnd + 5;
                if (Len < 0 || Body + static_cast<size_t>(Len) > Head.size()) return "an attribute runs past the header";
                if (Name == "compression" && Len == 1) Compression = static_cast<int>(U8(Body));
                else if (Name == "dataWindow" && Len == 16) X0 = I32(Body), Y0 = I32(Body + 4), X1 = I32(Body + 8), Y1 = I32(Body + 12);
                else if (Name == "channels")
                {
                    Channels = true;
                    size_t P = Body;
                    while (P < Body + static_cast<size_t>(Len) && U8(P) != 0)
                    {
                        const size_t E = Head.find('\0', P);
                        if (E == std::string::npos || E + 17 > Body + static_cast<size_t>(Len)) return "the channel list is cut short";
                        const std::string C = Head.substr(P, E - P);
                        R = R || C == "R", G = G || C == "G", B = B || C == "B";
                        P = E + 17;
                    }
                }
                At = Body + static_cast<size_t>(Len);
            }
            if (At >= Head.size()) return "no end of header";
            if (!Channels || !R || !G || !B) return "no R, G and B channels";
            if (Compression < 0 || Compression > 9) return "no valid compression attribute";
            if (X1 < X0 || Y1 < Y0 || X1 - X0 >= 16384 || Y1 - Y0 >= 16384) return "the dataWindow is empty or over 16384";
            const int Lines[10] = { 1, 1, 1, 16, 32, 16, 32, 32, 32, 256 };
            const int Rows = Y1 - Y0 + 1, Chunks = (Rows + Lines[Compression] - 1) / Lines[Compression];
            const std::uintmax_t TableAt = At + 1, TableEnd = TableAt + static_cast<std::uintmax_t>(Chunks) * 8;
            if (TableEnd > Size) return "the offset table runs past the end of the file";
            In.clear();
            In.seekg(static_cast<std::streamoff>(TableEnd - 8));
            std::array<uint8_t, 8> Last{};
            In.read(reinterpret_cast<char*>(Last.data()), 8);
            std::uint64_t Offset = 0;
            for (size_t K = 0; K < 8; K++) Offset |= static_cast<std::uint64_t>(Last[K]) << (8 * K);
            if (Offset < TableEnd || Offset + 8 > Size) return "the last chunk's offset is outside the file";
            In.seekg(static_cast<std::streamoff>(Offset + 4));
            std::array<uint8_t, 4> Len{};
            In.read(reinterpret_cast<char*>(Len.data()), 4);
            const std::uint64_t ChunkSize = static_cast<std::uint64_t>(Len[0]) | (static_cast<std::uint64_t>(Len[1]) << 8) | (static_cast<std::uint64_t>(Len[2]) << 16) | (static_cast<std::uint64_t>(Len[3]) << 24);
            if (Offset + 8 + ChunkSize > Size) return "the file ends inside the last chunk";
            return std::string();
        }

        // ---- check 3 and the rest of 4: a material

        void CheckMaterial(Context& C, const Entry& E)
        {
            const std::string Type = E.Value("materialType");
            const Tc::Techset* T = nullptr;
            const bool HaveCatalog = C.In.Catalog != nullptr && C.In.Catalog->HasCatalog();
            if (HaveCatalog)
            {
                C.Judged("material.type");
                T = C.In.Catalog->Find(Type);
                if (T == nullptr) C.Fail("material.type", E, std::format("materialType \"{}\" is not in the techset catalog", Type));
                else
                {
                    C.Judged("material.camo");
                    if (Type == "lit_weapon_camo" || T->HasTrait(Tc::Trait::Camo) || Lower(Type).find("camo") != std::string::npos)
                        C.Fail("material.camo", E, std::format("materialType \"{}\" is a camo techset; the Mod Tools cannot register it (T-rule: never emit lit_weapon_camo or camoMap)", Type));
                    if (Type == "effect_cloud_emissive_add") C.Fail("rules.forbidden_techset", E, "effect_cloud_emissive_add is not allowed");
                    C.Judged("rules.forbidden_techset");
                    if (Type.find("nocull") != std::string::npos || Type.find("transparent") != std::string::npos)
                        C.Add("rules.costly_techset", E.Id(), Severity::Info, std::format("materialType \"{}\" is nocull or transparent: costly to draw", Type));
                }
            }

            // a sky material: its colour image is an EXR (the lat-long the sky shader reads; M4e)
            if (Lower(Type).rfind("sky_", 0) == 0)
            {
                C.Judged("sky.chain");
                const std::string Color = E.Value("colorMap");
                const Entry* SkyImage = Color.empty() ? nullptr : C.Find("image", Color);
                if (SkyImage == nullptr)
                    C.Fail("sky.chain", E, std::format("the sky material's colorMap \"{}\" is not an image entry of this output", Color));
                else
                {
                    const std::string Base = Lower(SkyImage->Value("baseImage"));
                    if (Base.size() < 4 || Base.compare(Base.size() - 4, 4, ".exr") != 0)
                        C.Fail("sky.chain", E, std::format("the sky image \"{}\" has baseImage \"{}\", not an EXR: the sky shader reads a lat-long EXR", Color, SkyImage->Value("baseImage")));
                }
            }

            // image keys against the techset's slots
            if (T != nullptr)
            {
                std::set<std::string> SlotKeys;
                for (const Tc::Slot& S : T->Slots)
                    if (!S.Key.empty()) SlotKeys.insert(S.Key);
                // every key the catalog calls an image slot anywhere, and every awi asset-combo of type image
                std::set<std::string> ImageKeys;
                for (const Tc::Techset& O : C.In.Catalog->All())
                    for (const Tc::Slot& S : O.Slots)
                        if (!S.Key.empty()) ImageKeys.insert(S.Key);
                const Awi::Schema* Schema = C.SchemaFor("material");
                for (const auto& [Key, Value] : E.Fields)
                {
                    bool IsImageKey = ImageKeys.count(Key) != 0;
                    if (!IsImageKey && Schema != nullptr)
                        if (const Awi::Entry* D = Schema->Find(Key); D != nullptr && D->K == Awi::Kind::AssetCombo && Lower(D->Target) == "image") IsImageKey = true;
                    // a slot's name used where the key differs (the M8 renames)
                    if (!IsImageKey)
                        for (const Tc::Slot& S : T->Slots)
                            if (S.Name == Key && S.Key != Key && !S.Key.empty() && !Value.empty())
                                C.Fail("material.slot_key", E, std::format("\"{}\" is the slot's name; the GDT key of that slot is \"{}\"", Key, S.Key));
                    if (!IsImageKey || Value.empty()) continue;
                    C.Judged("material.slot_key");
                    const Tc::Slot* Slot = T->ByKey(Key);
                    if (Slot == nullptr)
                    {
                        C.Fail("material.slot_key", E, std::format("{} = \"{}\" fills a key that is not a slot of {} (its slots: {})", Key, Value, Type,
                                                                   [&] { std::string L; for (const std::string& K : SlotKeys) L += (L.empty() ? "" : ", ") + K; return L; }()));
                        continue;
                    }
                    if (Slot->Default == Value) continue;
                    const Entry* Img = C.Find("image", Value);
                    if (Img == nullptr)
                    {
                        // a stock or code image: judged by the semantic the Mod Tools give it; one that cannot be read stays incomplete, never a pass
                        std::vector<std::string> Semantics;
                        std::string Why;
                        if (!C.OutsideSemantics(Value, Semantics, Why))
                        {
                            C.SlotImageNotInOutput++;
                            if (C.SlotImageUnread.size() < 5) C.SlotImageUnread.push_back(Value + " (" + Why + ")");
                            continue;
                        }
                        C.SlotImageRead++;
                        C.Judged("material.slot_semantic");
                        const bool Taken = std::any_of(Semantics.begin(), Semantics.end(), [&](const std::string& S) { return Tc::Accepts(*Slot, S); });
                        if (!Taken)
                        {
                            std::string List;
                            for (const std::string& S : Semantics) List += (List.empty() ? "" : "/") + S;
                            C.Fail("material.slot_semantic", E, std::format("{} (linker slot {}) takes {} but the Mod Tools' image \"{}\" has semantic {}; the linker rejects this as a mismatched usage", Key, Slot->Name, Slot->Semantic, Value, List));
                        }
                        continue;
                    }
                    C.Judged("material.slot_semantic");
                    const std::string Semantic = Img->Value("semantic");
                    if (!Tc::Accepts(*Slot, Semantic))
                        C.Fail("material.slot_semantic", E, std::format("{} (linker slot {}) takes {} but image \"{}\" has semantic {}; the linker rejects this as a mismatched usage", Key, Slot->Name, Slot->Semantic, Value, Semantic));
                }
            }

            // surface type
            {
                const Awi::Schema* Schema = C.SchemaFor("material");
                const std::string* Surface = E.Get("surfaceType");
                if (Surface != nullptr)
                {
                    if (*Surface == "<none>" && Lower(Type).rfind("sky_", 0) == 0)
                    {
                        // a sky material has no surface: every stock sky material writes <none>
                        C.Judged("material.surface_type");
                        C.Add("material.surface_type", E.Id(), Severity::Info, "a sky material has no surface type (the stock sky materials all write <none>)");
                    }
                    else if (*Surface == "<none>")
                    {
                        C.Judged("material.surface_type");
                        std::string Key = E.Name;
                        for (char& Ch : Key) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                        const auto Why = C.SurfaceWhy.find(Key);
                        // positive evidence only: a <none> passes (Info) only with a material.surface_none_in_source row, written when the word was read and names none.
                        // A reported unknown or unmapped is a failure with its reason; no row (no file, a truncated file, a file of an older build) is a failure.
                        if (Why != C.SurfaceWhy.end() && (Why->second.rfind("material.surface_none_in_source", 0) == 0 || Why->second.rfind("material.surface_no_bo3_use", 0) == 0))
                            C.Add("material.surface_type", E.Id(), Severity::Info, "surfaceType is <none> and the export states why: " + Why->second);
                        else if (Why != C.SurfaceWhy.end())
                            C.Fail("material.surface_type", E, "surfaceType is <none> and the export reports why: " + Why->second);
                        else if (C.HaveApproximations)
                            C.Fail("material.surface_type", E, "surfaceType is <none> and graywolf_approximations.tsv has no row for it: no evidence that the source's word was read (a truncated file, or one from an older build?)");
                        else
                            C.Fail("material.surface_type", E, "surfaceType is <none> and there is no graywolf_approximations.tsv (Inputs::Approximations): no evidence that the source's word was read");
                    }
                    else if (Schema != nullptr)
                        if (const Awi::Entry* D = Schema->Find("surfaceType"); D != nullptr && D->OptionsKnown)
                        {
                            C.Judged("material.surface_type");
                            if (!Awi::Schema::HasOption(*D, *Surface)) C.Fail("material.surface_type", E, std::format("surfaceType \"{}\" is not a combo value of material.awi", *Surface));
                        }
                }
            }

            // techset fallback: a material whose techset was not the game's rule choice. A rule table that could not be used (reason=rules_...) is a failure, any other is a warning
            if (C.HaveApproximations)
            {
                std::string Key = E.Name;
                for (char& Ch : Key) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
                if (const auto U = C.Unbound.find(Key); U != C.Unbound.end())
                {
                    C.Judged("material.texture_unbound");
                    C.Warn("material.texture_unbound", E, "the source binds textures this export does not write: " + U->second);
                }
                if (const auto A = C.FamilyAssumed.find(Key); A != C.FamilyAssumed.end())
                {
                    C.Judged("material.family_assumed");
                    C.Warn("material.family_assumed", E, "the techset family rests on an assumption, the surface being unknown: " + A->second);
                }
                if (const auto X = C.GlossFailed.find(Key); X != C.GlossFailed.end())
                {
                    C.Judged("material.gloss_conversion");
                    C.Fail("material.gloss_conversion", E, "the gloss could not be extracted from the specular composite: " + X->second);
                }
                if (const auto X = C.RevealFailed.find(Key); X != C.RevealFailed.end())
                {
                    C.Judged("material.reveal_conversion");
                    C.Fail("material.reveal_conversion", E, "the reveal image could not be reduced to one channel: " + X->second);
                }
                if (const auto X = C.F0Failed.find(Key); X != C.F0Failed.end())
                {
                    C.Judged("material.f0_conversion");
                    C.Fail("material.f0_conversion", E, "the specular colour could not be converted from sqrt(F0): " + X->second);
                }
                if (const auto X = C.LayersDroppedWhy.find(Key); X != C.LayersDroppedWhy.end())
                {
                    C.Judged("material.layers_dropped");
                    C.Warn("material.layers_dropped", E, "a layered material's layers are not carried here (a slice of the game's layer data did not fit its shape, or the layers were not read): " + X->second);
                }
                if (const auto F = C.FallbackWhy.find(Key); F != C.FallbackWhy.end())
                {
                    C.Judged("material.techset_fallback");
                    if (F->second.rfind("reason=rules_", 0) == 0) C.Fail("material.techset_fallback", E, "the techset rule table could not be used: " + F->second);
                    else C.Warn("material.techset_fallback", E, "the techset is not the game's rule choice: " + F->second);
                }
            }

            // usage
            if (const std::string* Usage = E.Get("usage"); Usage != nullptr)
            {
                C.Judged("material.usage");
                if (C.In.MapSources && !C.In.MapDrawnKnown)
                    C.Incomplete("material.usage", "the materials the maps draw are not known (" + (C.In.MapDataWhy.empty() ? std::string("no map list given") : C.In.MapDataWhy) + "): a drawn material left <not in editor> would pass unseen");
                const auto Drawn = C.In.MapDrawnKnown ? C.In.MapDrawn.find(Lower(E.Name)) : C.In.MapDrawn.end();
                if (Drawn != C.In.MapDrawn.end())
                {
                    // a material the map's own geometry draws must be listed in Radiant: <not in editor> hides it
                    if (*Usage == "<not in editor>")
                        C.Fail("material.usage", E, "the map draws this material (surface " + Drawn->second + ") and its usage is <not in editor>: Radiant would not list it");
                }
                else if (*Usage != "<not in editor>")
                {
                    if (C.In.MapSources && !C.In.MapDrawnKnown) C.Add("material.usage", E.Id(), Severity::Info, "usage \"" + *Usage + "\" with map sources in the output: the gate cannot tell whether a map draws this material");
                    else C.Fail("material.usage", E, "usage \"" + *Usage + "\" on a material " + (C.In.MapDrawnKnown ? "that none of the run's maps draws on its geometry" : "no map draws") + " (usage is for map geometry only)");
                }
            }

            // tint k/255
            for (const auto& [Key, Value] : E.Fields)
            {
                if (Key.rfind("colorTint", 0) != 0 && Key != "transColorTint") continue;
                std::vector<double> V;
                if (!ParseFloats(Value, V) || V.size() < 3) continue;
                C.Judged("material.tint");
                for (size_t I = 0; I < 3; I++)
                {
                    const double K = std::round(V[I] * 255.0);
                    const double Exact = K / 255.0;
                    const double Scale = Exact > 0 ? std::pow(10.0, 5 - std::floor(std::log10(Exact))) : 1.0;
                    const double Up = Exact > 0 ? std::ceil(Exact * Scale - 1e-9) / Scale : 0.0;
                    if (std::fabs(V[I] * 255.0 - K) > 0.01 || std::fabs(V[I] - Up) > 1.01 / Scale)
                    {
                        C.Warn("material.tint", E, std::format("{} component {} = {} is not a k/255 step rounded up to 6 digits (nearest k = {:.0f}, {})", Key, I, V[I], K, Up));
                        break;
                    }
                }
            }

            // levels and desaturation away from the default cost extra
            if (const Awi::Schema* Schema = C.SchemaFor("material"); Schema != nullptr)
                for (const auto& [Key, Value] : E.Fields)
                {
                    if (Key.rfind("levels", 0) != 0 && Key != "desaturationAmount") continue;
                    const Awi::Entry* D = Schema->Find(Key);
                    double V;
                    if (D == nullptr || !D->HasDefault || !ParseDouble(Value, V)) continue;
                    C.Judged("rules.costly_levels");
                    if (V != D->Default) C.Warn("rules.costly_levels", E, std::format("{} = {} differs from the default {}: non-default levels or desaturation cost about 10% extra", Key, Value, D->Default));
                }

            // scroll and rotation rates (the u/v scroll amounts and rates and the rotation rate; other keys that hold the word
            // are scales or speeds of one effect and are covered by the awi range check)
            for (const auto& [Key, Value] : E.Fields)
            {
                const std::string K = Lower(Key);
                const bool Scroll = K.rfind("uscroll", 0) == 0 || K.rfind("vscroll", 0) == 0 || K.rfind("uvscrollrate", 0) == 0;
                const bool Rotation = K.rfind("uvrotationrate", 0) == 0;
                if (!Scroll && !Rotation) continue;
                double V;
                if (!ParseDouble(Value, V)) continue;
                C.Judged("rules.scroll_range");
                const double Limit = Rotation ? kRotationMax : kScrollMax;
                if (std::fabs(V) > Limit) C.Fail("rules.scroll_range", E, std::format("{} = {} is outside -{}..{}", Key, Value, Limit, Limit));
            }
        }

        // diffuse and specular of one material: bare metal is black
        // a sky xmodel (it swaps the stock sky material, mtl_skybox_default, for its own): the skinOverride's last name is a material entry of this output
        void CheckSkyXmodel(Context& C, const Entry& E)
        {
            if (E.Value("materials").find("mtl_skybox_default") == std::string::npos) return;
            C.Judged("sky.chain");
            std::string Skin = E.Value("skinOverride");
            for (size_t At = Skin.find("\\r\\n"); At != std::string::npos; At = Skin.find("\\r\\n")) Skin.replace(At, 4, " ");
            const size_t End = Skin.find_last_not_of(" \t\r\n");
            const std::string Last = End == std::string::npos ? std::string() : Skin.substr(Skin.find_last_of(" \t", End) == std::string::npos ? 0 : Skin.find_last_of(" \t", End) + 1, End + 1 - (Skin.find_last_of(" \t", End) == std::string::npos ? 0 : Skin.find_last_of(" \t", End) + 1));
            if (Last.empty() || Last == "mtl_skybox_default")
                C.Fail("sky.chain", E, "the sky xmodel's skinOverride names no sky material of its own");
            else if (C.Find("material", Last) == nullptr)
                C.Fail("sky.chain", E, std::format("the sky xmodel's skinOverride names \"{}\", which is not a material entry of this output", Last));
        }

        void CheckBareMetal(Context& C, const Entry& E)
        {
            const std::string DiffName = E.Value("colorMap"), SpecName = E.Value("specColorMap");
            if (DiffName.empty() || SpecName.empty()) return;
            const Entry* D = C.Find("image", DiffName);
            const Entry* S = C.Find("image", SpecName);
            if (D == nullptr || S == nullptr || D->Value("semantic") != "diffuseMap" || S->Value("semantic") != "specularMap") return;
            fs::path FD, FS;
            std::error_code Ec;
            if (!UnderRoot(C.In.Roots[D->Root], "", Unescape(D->Value("baseImage")), FD) || !UnderRoot(C.In.Roots[S->Root], "", Unescape(S->Value("baseImage")), FS) ||
                !fs::is_regular_file(FD, Ec) || !fs::is_regular_file(FS, Ec))
                return;     // image.source says so
            const auto TD = ThumbOf(C, FD, *D), TS = ThumbOf(C, FS, *S);
            if (!TD || !TS || TD->Rgba.empty() || TS->Rgba.empty()) return;
            const MetalStat M = BareMetal(*TD, *TS);
            C.Judged("rules.bare_metal");
            if (M.MetalPixels < 64) return;
            const double Share = static_cast<double>(M.NotBlack) / static_cast<double>(M.MetalPixels);
            if (Share > kMetalWarnShare)
                C.Warn("rules.bare_metal", E, std::format("{:.0f}% of the {} metal-bright specular pixels (mean >= {}) have a diffuse above {}; bare metal's diffuse is usually black, but stock materials with both maps break that (painted metal, plastic), so this is a guideline; nothing was changed", 100 * Share, M.MetalPixels, kMetalSpecMin, kBareMetalBlack));
        }
    }

    namespace
    {
        // [ "parent" ] entries: follow the chain (the same file first, then any file of the layout, then the gdt.db), merge the fields
        // (a child overrides its parent) and give the entry the root ancestor's GDF, so it is validated like any other. A parent that is
        // nowhere, a cycle or a chain deeper than 256 is a failure on every entry of the chain.
        void ResolveParents(Context& C)
        {
            std::map<std::string, std::vector<size_t>> Names;
            for (size_t I = 0; I < C.Entries.size(); I++) Names[Lower(C.Entries[I].Name)].push_back(I);
            enum : int { Open = 0, Done = 2, Broken = 3 };
            std::vector<int> State(C.Entries.size(), Open);
            const auto Parent = [&](size_t I) -> size_t {
                const Entry& E = C.Entries[I];
                const auto It = Names.find(Lower(E.Parent));
                if (It == Names.end()) return SIZE_MAX;
                size_t Best = SIZE_MAX;
                for (size_t P : It->second)
                {
                    if (P == I && It->second.size() > 1) continue;
                    if (Best == SIZE_MAX) Best = P;
                    if (C.Entries[P].File == E.File && C.Entries[P].Root == E.Root) { Best = P; break; }
                }
                return Best;
            };
            for (size_t I = 0; I < C.Entries.size(); I++)
            {
                if (C.Entries[I].Parent.empty() || State[I] != Open) continue;
                C.Judged("gdt.parent");
                std::vector<size_t> Chain{ I };
                std::set<size_t> Seen{ I };
                std::string Problem;
                size_t Base = SIZE_MAX;        // an entry with its fields and GDF settled
                std::string ExternalGdf;       // the last chain entry's parent exists only in the gdt.db
                for (size_t Cur = I;;)
                {
                    const size_t P = Parent(Cur);
                    if (P == SIZE_MAX)
                    {
                        const std::string& Name = C.Entries[Cur].Parent;
                        std::vector<std::string> Types;
                        if (C.In.Held != nullptr) Types = C.In.Held->TypesOf(Name);
                        if (Types.size() == 1)
                        {
                            ExternalGdf = Types[0];
                            if (ExternalGdf.size() > 4 && ExternalGdf.compare(ExternalGdf.size() - 4, 4, ".gdf") == 0) ExternalGdf.resize(ExternalGdf.size() - 4);
                        }
                        else
                            Problem = Types.size() > 1 ? std::format("parent \"{}\" is in the gdt.db under several types", Name)
                                                       : std::format("parent \"{}\" is in no GDT of this layout{}", Name, C.In.Held == nullptr ? " (the gdt.db was not read, so a stock parent cannot be looked up)" : " and not in the gdt.db");
                        break;
                    }
                    if (Seen.count(P)) { Problem = std::format("the parent chain loops back to \"{}\"", C.Entries[P].Name); break; }
                    if (State[P] == Broken) { Problem = std::format("parent \"{}\" is itself unresolvable", C.Entries[P].Name); break; }
                    if (C.Entries[P].Parent.empty() || State[P] == Done) { Base = P; break; }
                    if (Chain.size() >= 256) { Problem = "the parent chain is deeper than 256"; break; }
                    Chain.push_back(P);
                    Seen.insert(P);
                    Cur = P;
                }
                if (!Problem.empty())
                {
                    for (size_t K : Chain)
                    {
                        State[K] = Broken;
                        C.Fail("gdt.parent", C.Entries[K], Problem + "; the entry cannot be validated");
                    }
                    continue;
                }
                std::vector<std::pair<std::string, std::string>> Fields;
                std::string Gdf;
                if (Base != SIZE_MAX) { Fields = C.Entries[Base].Fields; Gdf = C.Entries[Base].Gdf; }
                else
                {
                    Gdf = ExternalGdf;
                    C.Incomplete("gdt.parent", std::format("a parent exists only in the gdt.db (as {}): its fields are not readable here, so the entries derived from it are validated without them", Gdf));
                }
                for (size_t K = Chain.size(); K-- > 0;)
                {
                    Entry& E = C.Entries[Chain[K]];
                    for (const auto& F : E.Fields)
                    {
                        const auto At = std::find_if(Fields.begin(), Fields.end(), [&](const auto& X) { return X.first == F.first; });
                        if (At != Fields.end()) At->second = F.second;
                        else Fields.push_back(F);
                    }
                    E.Fields = Fields;
                    E.Gdf = Gdf;
                    State[Chain[K]] = Done;
                }
            }
        }

        struct Scan
        {
            std::vector<std::pair<size_t, fs::path>> Files;
            std::vector<std::pair<std::string, std::string>> Errors;    // path, reason
        };

        // Every .gdt under a root. A folder that cannot be listed is recorded with its path and the walk goes on with the others.
        void ScanRoot(size_t RootIndex, const fs::path& Root, Scan& Out)
        {
            std::vector<fs::path> Pending{ Root };
            while (!Pending.empty())
            {
                const fs::path Dir = Pending.back();
                Pending.pop_back();
                std::error_code Ec;
                fs::directory_iterator It(Dir, Ec), End;
                if (Ec)
                {
                    Out.Errors.emplace_back(Dir.generic_string(), Ec.message());
                    continue;
                }
                for (; It != End; It.increment(Ec))
                {
                    if (Ec) break;
                    std::error_code Ec2;
                    if (It->is_directory(Ec2)) Pending.push_back(It->path());
                    else if (It->is_regular_file(Ec2) && Lower(It->path().extension().string()) == ".gdt") Out.Files.emplace_back(RootIndex, It->path());
                    if (Ec2) Out.Errors.emplace_back(It->path().generic_string(), Ec2.message());
                }
                if (Ec) Out.Errors.emplace_back(Dir.generic_string(), "the listing stopped: " + Ec.message());
            }
        }

        // The identity of a set of GDT files: SHA-256 over "<root index>/<relative path>", a NUL, the file's SHA-256 and a newline, in
        // (root index, path) order, so the same files give the same value whichever way they were found.
        bool HashRel(const std::vector<fs::path>& Roots, std::vector<std::pair<size_t, std::string>> Rels, std::string& Hash, std::string& Error)
        {
            std::sort(Rels.begin(), Rels.end());
            std::string All;
            for (const auto& [R, Rel] : Rels)
            {
                std::string Sha;
                if (R >= Roots.size() || !Output::Sha256File(Roots[R] / fs::path(Rel), Sha))
                {
                    Error = "cannot read " + Rel;
                    return false;
                }
                All += std::format("{}/{}", R, Rel) + '\0' + Sha + '\n';
            }
            Hash = Output::Sha256Hex(All);
            return !Hash.empty();
        }

        bool HashFiles(const std::vector<fs::path>& Roots, const std::vector<std::pair<size_t, fs::path>>& Files, std::string& Hash, std::string& Error)
        {
            std::vector<std::pair<size_t, std::string>> Rels;
            for (const auto& [R, F] : Files)
            {
                std::error_code Ec;
                Rels.emplace_back(R, fs::relative(F, Roots[R], Ec).generic_string());
            }
            return HashRel(Roots, std::move(Rels), Hash, Error);
        }
    }

    std::vector<fs::path> ResolveRoots(const fs::path& Out)
    {
        std::error_code Ec;
        if (!fs::is_directory(Out, Ec)) return {};     // a typo'd folder is not a layout and is not silently read as one
        if (fs::is_directory(Out / "bo3", Ec)) return { Out / "bo3" };
        std::vector<fs::path> Found;
        for (const auto& D : fs::directory_iterator(Out, Ec))
            if (D.is_directory(Ec) && fs::is_directory(D.path() / "bo3", Ec)) Found.push_back(D.path() / "bo3");
        std::sort(Found.begin(), Found.end());
        if (!Found.empty()) return Found;
        return { Out };
    }

    bool LayoutIdentity(const std::vector<fs::path>& Roots, std::string& Hash, size_t& Files, std::string& Error)
    {
        Scan S;
        for (size_t R = 0; R < Roots.size(); R++) ScanRoot(R, Roots[R], S);
        if (!S.Errors.empty())
        {
            Error = S.Errors.front().first + ": " + S.Errors.front().second;
            return false;
        }
        std::sort(S.Files.begin(), S.Files.end());
        Files = S.Files.size();
        return HashFiles(Roots, S.Files, Hash, Error);
    }

    bool GateStillHolds(const std::string& Mode, const std::vector<std::string>& RootTexts, const std::vector<std::pair<size_t, std::string>>& Files,
                        const std::string& Sha256, size_t Count, std::string& Why)
    {
        if (RootTexts.empty())
        {
            Why = "the gate names no layout root";
            return false;
        }
        std::vector<fs::path> Roots;
        for (const std::string& R : RootTexts)
        {
            std::error_code Ec;
            if (!fs::is_directory(R, Ec))
            {
                Why = "the layout folder " + R + " cannot be re-read (it is not a folder now): the gate cannot be shown to describe the GDTs";
                return false;
            }
            Roots.emplace_back(R);
        }
        std::vector<std::pair<size_t, std::string>> Rels;
        if (Mode == "scan")
        {
            Scan S;
            for (size_t R = 0; R < Roots.size(); R++) ScanRoot(R, Roots[R], S);
            if (!S.Errors.empty())
            {
                Why = "the layout cannot be re-read: " + S.Errors.front().first + ": " + S.Errors.front().second;
                return false;
            }
            for (const auto& [R, F] : S.Files)
            {
                std::error_code Ec;
                Rels.emplace_back(R, fs::relative(F, Roots[R], Ec).generic_string());
            }
        }
        else if (Mode == "listed")
        {
            if (Files.empty())
            {
                Why = "the gate names no GDT files (no gate.file rows), so its content cannot be re-verified";
                return false;
            }
            Rels = Files;
        }
        else
        {
            Why = "the gate's mode \"" + Mode + "\" is not scan or listed";
            return false;
        }
        std::string Hash, Error;
        if (!HashRel(Roots, Rels, Hash, Error))
        {
            Why = "the layout cannot be re-read: " + Error;
            return false;
        }
        if (Hash != Sha256 || Rels.size() != Count)
        {
            Why = std::format("the GDT files are not the ones the gate read ({} files now, {} then; the layout changed after the gate ran)", Rels.size(), Count);
            return false;
        }
        return true;
    }

    bool ModToolsGdtFiles(const Output::Manifest& Listed, bool Loaded, const fs::path& Root, std::vector<fs::path>& Out, std::string& Why)
    {
        Out.clear();
        if (!Loaded)
        {
            Why = "GrayWolf's manifest in the Mod Tools root did not load, so which GDTs this run wrote is unknown";
            return false;
        }
        for (const auto& [Key, Entry] : Listed.All())
        {
            std::string Ext = fs::path(Entry.Path).extension().string();
            for (char& Ch : Ext) Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
            if (Ext == ".gdt") Out.push_back(Root / Entry.Path);
        }
        if (Out.empty())
        {
            Why = "GrayWolf's manifest lists no .gdt file; the Mod Tools root is never scanned for them";
            return false;
        }
        return true;
    }

    bool ReadXModelBinMaterials(const fs::path& File, std::vector<std::string>& Names, std::string& Why)
    {
        Names.clear();
        std::vector<XAnimBinRecord> Records;
        if (!ReadXAnimBinRecords(File.string(), Records))
        {
            Why = "the file is not a readable .xmodel_bin (bad container, LZ4 block or an unknown token)";
            return false;
        }
        // Only a whole model is judged: a MODEL marker and a VERSION (an animation stream has neither), and a NUMMATERIALS that agrees with the
        // MATERIAL records read (a stream cut short, or one that was never a model, must not read as "no materials").
        bool Model = false, Version = false, Declared = false;
        uint16_t Count = 0;
        for (const XAnimBinRecord& R : Records)
        {
            if (R.Keyword == "MODEL") Model = true;
            else if (R.Keyword == "VERSION") Version = true;
            else if (R.Keyword == "NUMMATERIALS" && !Declared && R.Raw.size() >= 2)
            {
                Declared = true;
                std::memcpy(&Count, R.Raw.data(), 2);
            }
            else if (R.Keyword == "MATERIAL" && !R.Strings.empty()) Names.push_back(R.Strings.front());
        }
        if (!Model || !Version)
        {
            Names.clear();
            Why = "the stream has no MODEL and VERSION header, so it is not a model";
            return false;
        }
        if (!Declared)
        {
            Names.clear();
            Why = "the model declares no NUMMATERIALS";
            return false;
        }
        if (Names.size() != Count)
        {
            Why = std::format("NUMMATERIALS declares {} but {} MATERIAL record(s) were read", Count, Names.size());
            Names.clear();
            return false;
        }
        return true;
    }

    namespace
    {
        // model.material_closure: the linker looks every material a model names up by name in the gdt.db and in the GDTs of the build
        // ("Material X was not found in gdtDB"). A model's own GDT does not hold them all, so the names are read from each .xmodel_bin.
        void CheckModelClosure(Context& C)
        {
            if (C.R.Mode == "listed")
            {
                C.Incomplete("model.material_closure", "the gate was given a list of GDT files (not a folder): the .xmodel_bin files of the output are not enumerated, so no model's materials were checked");
                return;
            }
            for (size_t R = 0; R < C.In.Roots.size(); R++)
            {
                std::error_code Ec;
                if (!fs::is_directory(C.In.Roots[R], Ec)) continue;
                std::vector<fs::path> Bins;
                fs::recursive_directory_iterator It(C.In.Roots[R], fs::directory_options::skip_permission_denied, Ec), End;
                for (; !Ec && It != End; It.increment(Ec))
                {
                    std::error_code E2;
                    if (It->is_regular_file(E2) && Lower(It->path().extension().string()) == ".xmodel_bin") Bins.push_back(It->path());
                }
                if (Ec) C.Incomplete("model.material_closure (part)", "a folder under the layout could not be listed, so some .xmodel_bin files may be missing: " + Ec.message());
                std::sort(Bins.begin(), Bins.end());
                for (const fs::path& Bin : Bins)
                {
                    const std::string Asset = "xmodel_bin:" + fs::relative(Bin, C.In.Roots[R], Ec).generic_string();
                    C.ClosureBins++;
                    std::vector<std::string> Names;
                    std::string Why;
                    if (!ReadXModelBinMaterials(Bin, Names, Why))
                    {
                        C.ClosureUnparsed++;
                        if (C.ClosureUnparsedExamples.size() < 3) C.ClosureUnparsedExamples.push_back(Asset + " (" + Why + ")");
                        continue;
                    }
                    C.Judged("model.material_closure");
                    std::set<std::string> Seen;
                    for (const std::string& N : Names)
                    {
                        if (N.empty() || !Seen.insert(Lower(N)).second) continue;
                        std::string Key = Lower(N);
                        if (const size_t Slash = Key.rfind('/'); Slash != std::string::npos) Key = Key.substr(Slash + 1);
                        if (C.ByGdfName.count({ "material", Key })) continue;
                        if (C.In.Held == nullptr)
                        {
                            C.ClosureNoDb++;
                            if (C.ClosureNoDbExamples.size() < 3) C.ClosureNoDbExamples.push_back(N + " in " + Asset);
                            continue;
                        }
                        if (C.In.Held->Contains("material", Key) || C.In.Held->Contains("material.gdf", Key)) continue;
                        if (const auto Left = C.SkippedWhy.find(Key); Left != C.SkippedWhy.end())
                            C.Add("model.material_closure", Asset, Severity::Failure,
                                  "the model names material \"" + N + "\", which the exporter skipped on purpose (" + Left->second + ") and which is in no GDT of this output and not in the Mod Tools gdt.db: the linker stops with \"Material " + N + " was not found in gdtDB\"");
                        else
                            C.Add("model.material_closure", Asset, Severity::Failure,
                                  "the model names material \"" + N + "\", which is in no GDT of this output and not in the Mod Tools gdt.db: the linker stops with \"Material " + N + " was not found in gdtDB\"");
                    }
                }
            }
        }
    }

    Result Run(const Inputs& In)
    {
        Context C(In);
        try
        {
            std::string RootsText;
            for (const fs::path& R : In.Roots) RootsText += (RootsText.empty() ? "" : "|") + fs::absolute(R).lexically_normal().generic_string();
            C.R.RunRoots = RootsText;
            C.R.Mode = (In.ListedOnly || !In.OnlyFiles.empty()) ? "listed" : "scan";
            if (C.StockLateRows != 0)
                C.Add("material.stock_semantic_late", "gate", Severity::Info,
                      std::format("{} material.stock_semantic_late rows in graywolf_approximations.tsv: an image whose final name is held by an installed GDT, or stock, is judged again with that name (not necessarily a stock image)", C.StockLateRows));

            // the GDT files; any input that gives nothing is a failure
            if (In.Roots.empty()) C.Add("gate.input", "gate", Severity::Failure, "no layout root: the input folder does not exist or none was given");
            for (const fs::path& R : In.Roots)
            {
                std::error_code Ec;
                if (!fs::is_directory(R, Ec)) C.Add("gate.input", "root:" + R.generic_string(), Severity::Failure, "the layout root is not a folder");
            }
            Scan S;
            if (In.ListedOnly || !In.OnlyFiles.empty())
            {
                if (In.OnlyFiles.empty()) C.Add("gate.no_input", "gate", Severity::Failure, "the list of GDT files to check is empty");
                for (const fs::path& F : In.OnlyFiles)
                {
                    bool Placed = false;
                    for (size_t R = 0; R < In.Roots.size() && !Placed; R++)
                        if (Output::IsUnder(In.Roots[R], F)) { S.Files.emplace_back(R, F); Placed = true; }
                    if (!Placed) C.Add("gate.input", "file:" + F.generic_string(), Severity::Failure, "a listed GDT lies outside the layout roots and was not read");
                }
            }
            else
                for (size_t R = 0; R < In.Roots.size(); R++)
                {
                    std::error_code Ec;
                    if (fs::is_directory(In.Roots[R], Ec)) ScanRoot(R, In.Roots[R], S);
                }
            for (const auto& [Path, Why] : S.Errors) C.Add("gate.input", "path:" + Path, Severity::Failure, "a folder could not be read, so GDTs under it may be missing: " + Why);
            std::sort(S.Files.begin(), S.Files.end());
            if (S.Files.empty() && !(In.ListedOnly || !In.OnlyFiles.empty()))
                C.Add("gate.no_input", "gate", Severity::Failure, "no .gdt file was found under the layout root(s): nothing was judged");
            C.R.Files = S.Files.size();
            for (const auto& [R, F] : S.Files)
            {
                std::error_code Ec;
                ReadGdt(C, R, F, fs::relative(F, In.Roots[R], Ec).generic_string());
            }
            {
                std::string Hash, Why;
                if (HashFiles(In.Roots, S.Files, Hash, Why))
                {
                    C.R.Identity = Hash;
                    C.R.IdentityFiles = S.Files.size();
                    for (const auto& [R, F] : S.Files)
                    {
                        std::error_code Ec;
                        C.R.GdtFiles.emplace_back(R, fs::relative(F, In.Roots[R], Ec).generic_string());
                    }
                }
            }
            C.R.Entries = C.Entries.size();
            // a map-drawn material the Mod Tools hold is not ours to rewrite: warn where its usage there hides it (or cannot be read)
            // (the run's own list, else the rows the run wrote to its approximations file, so the stand-alone gate and the export's gate agree)
            std::map<std::string, std::pair<std::string, std::string>> AllHeld = C.HeldRows;
            for (const auto& [Asset, Held] : In.MapHeld) AllHeld[Asset] = Held;
            for (const auto& [Asset, Held] : AllHeld)
            {
                C.Judged("material.usage_held");
                if (Held.second == "<not in editor>" || Held.second.rfind("unavailable", 0) == 0)
                    C.Add("material.usage_held", "material:" + Asset, Severity::Warning,
                          "the map draws this material, the Mod Tools hold it in " + (Held.first.empty() ? std::string("(unknown gdt)") : Held.first) + " with usage " + Held.second +
                              (Held.second == "<not in editor>" ? ": Radiant will not list it" : ": its usage could not be read"));
            }
            if (C.Entries.empty()) C.Add("gate.no_entries", "gate", Severity::Failure, "zero entries were read: nothing was judged, so this is not a pass");

            ResolveParents(C);
            for (size_t I = 0; I < C.Entries.size(); I++)
            {
                const Entry& E = C.Entries[I];
                if (!E.Gdf.empty()) C.ByGdfName[{ Lower(E.Gdf), Lower(E.Name) }].push_back(I);
                C.ByName[Lower(E.Name)].push_back(I);
            }

            // G3: each (name, GDF) in exactly one file; a name has one GDF per file is covered above
            for (const auto& [Key, Idx] : C.ByGdfName)
            {
                C.Judged("gdt.single_file");
                std::set<std::string> FilesOf;
                for (size_t I : Idx) FilesOf.insert(C.Entries[I].File);
                if (FilesOf.size() > 1)
                {
                    std::string L;
                    for (const std::string& F : FilesOf) L += (L.empty() ? "" : "; ") + F;
                    C.Fail("gdt.single_file", C.Entries[Idx.front()], "defined in " + std::to_string(FilesOf.size()) + " files (each name and GDF belongs in exactly one, G3): " + L);
                }
            }
            if (In.Held != nullptr)
            {
                for (const auto& [Key, Idx] : C.ByGdfName)
                {
                    C.Judged("gdt.stock_pair");
                    const Entry& E = C.Entries[Idx.front()];
                    if (In.Held->Contains(E.Gdf + ".gdf", E.Name) || In.Held->Contains(E.Gdf, E.Name))
                    {
                        // The row blocks the name either way (the unfiltered catalog: a user's asset blocks a name too); only a stock row is called a stock pair.
                        const bool Stock = In.Stock == nullptr || In.Stock->HoldsStockPair(E.Gdf, E.Name);
                        C.Fail("gdt.stock_pair", E, Stock ? "the Mod Tools gdt.db already holds this name as " + E.Gdf + " (a stock pair is never written)"
                                                          : "the Mod Tools gdt.db already holds this name as " + E.Gdf + " in a GDT that is not Treyarch's own (the owner's content or a re-saved library GDT): the name is taken, a second one is a duplicate asset");
                    }
                    else if (In.Held->ContainsName(E.Name))
                    {
                        C.Judged("install.name_collision");
                        const std::vector<std::string> Types = In.Held->TypesOf(E.Name);
                        C.Warn("install.name_collision", E, "the Mod Tools gdt.db already holds an entity of this name" + (Types.empty() ? std::string() : " as " + Types.front()) + ": a link that sees both reports a duplicate asset");
                    }
                }
            }

            for (const Entry& E : C.Entries)
                if (Lower(E.Gdf) == "material" && Lower(E.Value("materialType")).rfind("sky_", 0) == 0) C.SkyImages.insert(Lower(E.Value("colorMap")));
            // 2, 3, 5 and the file-reading part of 4
            for (const Entry& E : C.Entries)
            {
                if (E.Gdf.empty()) continue;     // gdt.parent said why
                CheckSchema(C, E, In.Roots[E.Root].string());
                if (Lower(E.Gdf) == "material") CheckMaterial(C, E);
                else if (Lower(E.Gdf) == "image") CheckImage(C, E);
                else if (Lower(E.Gdf) == "xmodel") CheckSkyXmodel(C, E);
            }
            for (const Entry& E : C.Entries)
                if (Lower(E.Gdf) == "material") CheckBareMetal(C, E);
            CheckModelClosure(C);
        }
        catch (const std::exception& Ex)
        {
            C.Add("gate.internal", "gate", Severity::Failure, std::string("the gate stopped on an exception: ") + Ex.what());
        }

        Result& R = C.R;
        for (const Entry& E : C.Entries)
        {
            if (E.Gdf.empty()) continue;
            auto& G = R.Gdfs[E.Gdf];
            G.first++;
            if (C.FailedEntries.count(E.Id())) G.second++;
        }
        for (const std::string& W : C.SchemaWhy) R.Findings.push_back({ "awi.schema", "gdf", Severity::Info, "no readable awi: " + W });
        const bool Judgeable = !C.Entries.empty();
        if (Judgeable && In.ModToolsRoot.empty()) C.Incomplete("awi.*", "no Mod Tools root given: deffiles\\*.awi not read, so no key, type, range or combo was checked");
        if (Judgeable && (In.Catalog == nullptr || !In.Catalog->HasCatalog()))
            C.Incomplete("material.type, material.camo, material.slot_key, material.slot_semantic", "the techset catalog (zonekit/profiles/t7/target/techsets.json) is not loaded");
        if (Judgeable && In.Held == nullptr)
            C.Incomplete("gdt.stock_pair, awi.reference_unresolved", std::format("no gdt.db was read (no Mod Tools root, or the database is unreadable): stock pairs and references to assets outside this output cannot be judged ({} reference(s) seen)", C.UnresolvedNoDb));
        if (C.SlotImageNotInOutput > 0)
        {
            std::string Examples;
            for (const std::string& X : C.SlotImageUnread) Examples += (Examples.empty() ? "" : "; ") + X;
            C.Incomplete("material.slot_semantic (part)", std::format("{} filled slot(s) name an image outside this output whose semantic the Mod Tools data could not give ({} others were read and judged); e.g. {}", C.SlotImageNotInOutput, C.SlotImageRead, Examples));
        }
        if (C.ClosureUnparsed > 0)
        {
            std::string Examples;
            for (const std::string& X : C.ClosureUnparsedExamples) Examples += (Examples.empty() ? "" : "; ") + X;
            C.Incomplete("model.material_closure (unparsed)", std::format("{} of {} .xmodel_bin file(s) could not be parsed, so the materials they name were not checked; e.g. {}", C.ClosureUnparsed, C.ClosureBins, Examples));
        }
        if (C.ClosureNoDb > 0)
        {
            std::string Examples;
            for (const std::string& X : C.ClosureNoDbExamples) Examples += (Examples.empty() ? "" : "; ") + X;
            C.Incomplete("model.material_closure (no gdt.db)", std::format("{} material name(s) in .xmodel_bin files are in no GDT of this output and the Mod Tools gdt.db was not read (no Mod Tools root, or unreadable), so they could not be looked up; e.g. {}", C.ClosureNoDb, Examples));
        }
        if (C.UnjudgedRange > 0) C.Incomplete("awi.range (part)", std::format("{} value(s) of keys whose min or max the awi gives as an expression this reader cannot resolve were not range-checked", C.UnjudgedRange));
        R.NotRun.emplace_back("sound (check 6)", "not built this round");
        R.NotRun.emplace_back("weapon and anim (check 7)", "not built this round");
        R.NotRun.emplace_back("stock oracle comparison (check 8)", "not built this round");
        R.NotRun.emplace_back("rules: clean glass 40, effect maps never uncompressed, decal category, emissive intensity 0..12000 (the sky EXR and skinOverride rule is sky.chain)",
                              "need the material's intent, the sky/decal classification or a key material.awi does not declare; nothing here judges them");
        return std::move(C.R);
    }

    bool WriteReports(const Result& R, const fs::path& Dir, std::string& Error)
    {
        std::error_code Ec;
        fs::create_directories(Dir, Ec);
        const std::string Tsv = R.Tsv(), One = R.Summary() + '\n';
        return Output::WriteAtomic(Dir / "graywolf_gate.tsv", Tsv.data(), Tsv.size(), Error) &&
               Output::WriteAtomic(Dir / "graywolf_gate_summary.txt", One.data(), One.size(), Error);
    }

    std::string Result::Tsv() const
    {
        const auto Cell = [](std::string S) {
            for (char& Ch : S)
                if (Ch == '\t' || Ch == '\r' || Ch == '\n') Ch = ' ';
            return S;
        };
        std::string Out = std::string("# ") + kStatic + "\n";
        Out += "check\tasset\tseverity\tdetail\n";
        for (const Finding& F : Findings) Out += std::format("{}\t{}\t{}\t{}\n", Cell(F.Check), Cell(F.Asset), SeverityName(F.Level), Cell(F.Detail));
        for (const auto& [Gdf, N] : Gdfs)
        {
            size_t Warned = 0;
            for (const Finding& F : Findings)
                if (F.Level == Severity::Warning && F.Asset.rfind(Gdf + ":", 0) == 0) Warned++;
            Out += std::format("gate.entries\t{}\tinfo\tentries={} failed={} warned={}\n", Cell(Gdf), N.first, N.second, Warned);
        }
        for (const auto& [Check, S] : Checks) Out += std::format("gate.check\t{}\tinfo\tjudged={} failures={} warnings={}\n", Cell(Check), S.Judged, S.Failures, S.Warnings);
        Out += std::format("gate.run\t{}\tinfo\tmode={} gdts={} sha256={} roots={}\n", Cell(Mode), Cell(Mode), IdentityFiles, Identity, Cell(RunRoots));
        for (const auto& [Root, Rel] : GdtFiles) Out += std::format("gate.file\t{}\tinfo\troot={}\n", Cell(Rel), Root);
        for (const auto& [Check, Why] : Incomplete) Out += std::format("{}\t-\tincomplete\t{}\n", Cell(Check), Cell(Why));
        for (const auto& [Check, Why] : NotRun) Out += std::format("{}\t-\tnot_run\t{}\n", Cell(Check), Cell(Why));
        return Out;
    }

    std::string Result::Summary() const
    {
        std::string Out = std::format("bo3-verify STATIC (not proof BO3 accepts the assets): files={} entries={} images={} decoded={} failures={} warnings={} exit={} result={}", Files, Entries, Images, ImagesDecoded, Failures, Warnings, ExitCode(), Word());
        std::string Per;
        for (const auto& [Check, S] : Checks)
            if (S.Failures || S.Warnings) Per += std::format(" {}[fail={} warn={} of {}]", Check, S.Failures, S.Warnings, S.Judged);
        if (!Per.empty()) Out += " |" + Per;
        if (!Incomplete.empty())
        {
            std::string Inc;
            for (const auto& [Check, Why] : Incomplete) Inc += (Inc.empty() ? "" : "; ") + Check;
            Out += " | incomplete (not judged): " + Inc;
        }
        std::string Not;
        for (const auto& [Check, Why] : NotRun) Not += (Not.empty() ? "" : "; ") + Check;
        Out += " | not run by design: " + Not;
        return Out;
    }

    // ---- self-test: synthetic GDT text, a synthetic .awi, tiny images

    namespace
    {
        void Put(const fs::path& File, const std::string& Text)
        {
            std::error_code Ec;
            fs::create_directories(File.parent_path(), Ec);
            std::ofstream(File, std::ios::binary) << Text;
        }

        // An uncompressed little-endian TIFF, W x H, Samples 8-bit channels, every pixel the same.
        std::string Tiff(uint32_t W, uint32_t H, uint32_t Samples, const std::vector<uint8_t>& Pixel)
        {
            std::string B;
            const auto P16 = [&](uint32_t V) { B += static_cast<char>(V & 255); B += static_cast<char>((V >> 8) & 255); };
            const auto P32 = [&](uint32_t V) { P16(V & 0xFFFF); P16(V >> 16); };
            const uint32_t Entries = 9, Ifd = 8, BitsAt = Ifd + 2 + Entries * 12 + 4, DataAt = BitsAt + 8;
            B += "II";
            P16(42);
            P32(Ifd);
            P16(Entries);
            const auto Tag = [&](uint32_t T, uint32_t Type, uint32_t Count, uint32_t Value) {
                P16(T);
                P16(Type);
                P32(Count);
                if (Type == 3 && Count == 1) { P16(Value); P16(0); }
                else P32(Value);
            };
            Tag(256, 3, 1, W);
            Tag(257, 3, 1, H);
            Tag(258, 3, Samples, Samples == 1 ? 8 : BitsAt);
            Tag(259, 3, 1, 1);
            Tag(262, 3, 1, Samples == 1 ? 1 : 2);
            Tag(273, 4, 1, DataAt);
            Tag(277, 3, 1, Samples);
            Tag(278, 3, 1, H);
            Tag(279, 4, 1, W * H * Samples);
            P32(0);
            for (int I = 0; I < 4; I++) P16(8);
            for (uint32_t I = 0; I < W * H; I++)
                for (uint32_t S = 0; S < Samples; S++) B += static_cast<char>(Pixel[S % Pixel.size()]);
            return B;
        }

        std::string Gdt(const std::vector<std::string>& Entries, const char* Eol = "\r\n")
        {
            std::string Out = std::string("{") + Eol;
            for (const std::string& E : Entries) Out += E;
            return Out + "}" + Eol;
        }
        std::string Ent(const std::string& Name, const std::string& Gdf, const std::vector<std::pair<std::string, std::string>>& F, const char* Eol = "\r\n")
        {
            std::string Out = "\t\"" + Name + "\" ( \"" + Gdf + ".gdf\" )" + Eol + "\t{" + Eol;
            for (const auto& [K, V] : F) Out += "\t\t\"" + K + "\" \"" + V + "\"" + Eol;
            return Out + "\t}" + Eol;
        }
        using Fields = std::vector<std::pair<std::string, std::string>>;
        Fields Img(const char* File, const char* Sem, const char* Core, const char* Comp)
        {
            return { { "baseImage", std::string("texture_assets\\\\t\\\\") + File }, { "compressionMethod", Comp }, { "coreSemantic", Core }, { "semantic", Sem }, { "type", "image" } };
        }
    }

    // ---- self-test: the review findings (each expectation failed before its fix)

    namespace
    {
        std::string Esc(const std::string& Path)
        {
            std::string Out;
            for (char C : Path) Out += C == '\\' ? std::string("\\\\") : std::string(1, C);
            return Out;
        }

        // A TIFF with per-sample bit depths and sample formats written as arrays (count > 2 goes through a pointer).
        std::string TiffRaw(uint32_t W, uint32_t H, const std::vector<uint16_t>& Bits, const std::vector<uint16_t>& Format)
        {
            std::string B;
            const auto P16 = [&](uint32_t V) { B += static_cast<char>(V & 255); B += static_cast<char>((V >> 8) & 255); };
            const auto P32 = [&](uint32_t V) { P16(V & 0xFFFF); P16(V >> 16); };
            const uint32_t Samples = static_cast<uint32_t>(Bits.size()), Entries = 10, Ifd = 8;
            const uint32_t BitsAt = Ifd + 2 + Entries * 12 + 4, FormatAt = BitsAt + 2 * Samples, DataAt = FormatAt + 2 * Samples;
            B += "II";
            P16(42);
            P32(Ifd);
            P16(Entries);
            const auto Tag = [&](uint32_t T, uint32_t Type, uint32_t Count, uint32_t Value) {
                P16(T);
                P16(Type);
                P32(Count);
                if (Type == 3 && Count == 1) { P16(Value); P16(0); }
                else P32(Value);
            };
            Tag(256, 3, 1, W);
            Tag(257, 3, 1, H);
            Tag(258, 3, Samples, BitsAt);
            Tag(259, 3, 1, 1);
            Tag(262, 3, 1, 2);
            Tag(273, 4, 1, DataAt);
            Tag(277, 3, 1, Samples);
            Tag(278, 3, 1, H);
            Tag(279, 4, 1, W * H * Samples);
            Tag(339, 3, Samples, FormatAt);
            P32(0);
            for (uint16_t V : Bits) P16(V);
            for (uint16_t V : Format) P16(V);
            B.append(static_cast<size_t>(W) * H * Samples, '\x40');
            return B;
        }

        std::string PngHeader(uint32_t Length, uint32_t W, uint32_t H)
        {
            std::string B = "\x89PNG\r\n\x1a\n";
            const auto P32 = [&](uint32_t V) { for (int S = 24; S >= 0; S -= 8) B += static_cast<char>((V >> S) & 255); };
            P32(Length);
            B += "IHDR";
            P32(W);
            P32(H);
            B += std::string("\x08\x02\0\0\0", 5);
            return B;
        }
    }

    int ReviewTests(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got = std::string()) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL Verify(review) {} (got '{}')\n", Name, Got);
        };
        const auto Reader = [](const std::string& Bytes) {
            return [&Bytes](uint64_t At, void* To, size_t N) {
                if (At + N > Bytes.size()) return false;
                std::memcpy(To, &Bytes[At], N);
                return true;
            };
        };
        const auto Has = [](const Result& R, const char* Check1, Severity S = Severity::Failure, const char* Asset = nullptr) {
            size_t N = 0;
            for (const Finding& F : R.Findings)
                if (F.Check == Check1 && F.Level == S && (Asset == nullptr || F.Asset == Asset)) N++;
            return N;
        };
        const auto Describe = [](const Result& R) {
            std::string L;
            for (const Finding& F : R.Findings) L += F.Check + "@" + F.Asset + "; ";
            return L + R.Summary();
        };

        std::error_code Ec;
        const fs::path Base = fs::temp_directory_path(Ec) / std::format("gw_verify_review_{}", static_cast<unsigned long>(::GetCurrentProcessId()));
        fs::remove_all(Base, Ec);
        const fs::path Mod = Base / "modtools";
        Put(Mod / "deffiles" / "image.awi", R"AWI(
void GenerateUI( asset Asset )
{
    Asset.AddEntry_Combo( "semantic", "diffuseMap | normalMap | glossMap | specularMap" );
    Asset.AddEntry_Combo( "compressionMethod", "compressed | uncompressed | compressed high color{compressed high color}" );
    Asset.AddEntry_Texture( "baseImage", "" );
    Asset.AddEntry_String( "coreSemantic", "" );
    Asset.AddEntry_Combo( "type", "image | composite" );
}
)AWI");
        Put(Mod / "deffiles" / "material.awi", R"AWI(
void GenerateUI( asset Asset )
{
    Asset.AddEntry_Combo( "materialType", Asset.GetTechsetdefTypes( "" ) );
    Asset.AddEntry_Combo( "surfaceType", "<error> | <none> | concrete | metal" );
    Asset.AddEntry_Combo( "usage", "<not in editor> | floor" );
    Asset.AddEntry_AssetCombo( "colorMap", "image" );
}
)AWI");
        Put(Mod / "deffiles" / "xmodel.awi", R"AWI(
void GenerateUI( asset Asset )
{
    Asset.AddEntry_XModel( "filename", "" ).SetRelativePath( "model_export/" );
    Asset.AddEntry_Combo( "type", "rigid | animated" );
}
)AWI");
        Tc::Data Catalog;
        Catalog.LoadCatalogText(R"({"techsets":[{"n":"lit","c":"Geometry","f":"none","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap",null]]},
            {"n":"sky_latlong_hdr","c":"Geometry","f":"emissive sky","b":"Other","t":0,"s":[["colorMap","colorMap","HDR","$white_diffuse_hdr"]]}]})");
        GdtDb::Catalog Held;     // a gdt.db that holds nothing
        const auto Full = [&](const fs::path& Root) {
            Inputs In;
            In.Roots = { Root };
            In.ModToolsRoot = Mod;
            In.Catalog = &Catalog;
            In.Held = &Held;
            return In;
        };

        // 1. zero work is a failure, never a pass
        {
            Check(ResolveRoots(Base / "typo").empty(), "a typo'd folder gives no root (no silent fallback to itself)", std::to_string(ResolveRoots(Base / "typo").size()));
            Inputs In = Full(Base / "typo");
            In.Roots = ResolveRoots(Base / "typo");
            const Result R = Run(In);
            Check(R.ExitCode() == 2 && Has(R, "gate.input") >= 1, "a nonexistent input is exit 2 with gate.input", Describe(R));

            fs::create_directories(Base / "emptydir" / "bo3", Ec);
            const Result E = Run(Full(Base / "emptydir" / "bo3"));
            Check(E.ExitCode() == 2 && Has(E, "gate.no_input") == 1 && Has(E, "gate.no_entries") == 1, "a folder with no GDT is exit 2 (gate.no_input, gate.no_entries)", Describe(E));

            Put(Base / "zero" / "bo3" / "a.gdt", "");
            Put(Base / "zero" / "bo3" / "b.gdt", "{\r\n}\r\n");
            const Result Z = Run(Full(Base / "zero" / "bo3"));
            Check(Z.ExitCode() == 2 && Has(Z, "gdt.empty", Severity::Failure, "file:a.gdt") == 1 && Has(Z, "gdt.empty", Severity::Failure, "file:b.gdt") == 1 && Has(Z, "gate.no_entries") == 1, "a 0-byte GDT and an empty { } are failures", Describe(Z));

            Put(Base / "outside" / "x.gdt", Gdt({ Ent("zz", "image", { { "type", "image" } }) }));
            Inputs L = Full(Base / "emptydir" / "bo3");
            L.OnlyFiles = { Base / "outside" / "x.gdt" };
            const Result O = Run(L);
            Check(Has(O, "gate.input") >= 1 && O.ExitCode() == 2, "a listed GDT outside the root is a failure, not dropped", Describe(O));
        }

        // 2. exit codes count what was not judged
        {
            const fs::path Clean = Base / "clean" / "bo3";
            Put(Clean / "texture_assets" / "t" / "c.tif", Tiff(8, 8, 3, { 100, 100, 100 }));
            Put(Clean / "texture_assets" / "t.gdt", Gdt({
                Ent("i_c", "image", Img("c.tif", "diffuseMap", "sRGB3chAlpha", "compressed high color")),
                Ent("mtl_ok", "material", { { "colorMap", "i_c" }, { "materialType", "lit" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
            }));
            const Result Pass = Run(Full(Clean));
            Check(Pass.Failures == 0 && Pass.Warnings == 0 && Pass.ExitCode() == 0, "everything judged and clean: exit 0", Describe(Pass));
            Inputs NoRoot = Full(Clean);
            NoRoot.ModToolsRoot.clear();
            const Result A = Run(NoRoot);
            Check(A.ExitCode() == 1 && A.Failures == 0 && A.Summary().find("incomplete") != std::string::npos, "no Mod Tools root: exit 1, stated incomplete", Describe(A));
            Inputs NoCat = Full(Clean);
            NoCat.Catalog = nullptr;
            const Result B = Run(NoCat);
            Check(B.ExitCode() == 1 && B.Summary().find("incomplete") != std::string::npos, "no catalog: exit 1, stated incomplete", Describe(B));
            Inputs NoDb = Full(Clean);
            NoDb.Held = nullptr;
            Check(Run(NoDb).ExitCode() == 1, "no gdt.db: exit 1");
            // by-design not_run rows (checks 6 to 8) are still listed but do not make a run incomplete
            Check(!Pass.NotRun.empty() && Pass.ExitCode() == 0, "checks 6 to 8 stay visible as not_run without failing the run");

            const fs::path NoAwi = Base / "noawi" / "bo3";
            Put(NoAwi / "a.gdt", Gdt({ Ent("zone_thing", "unknownkind", { { "k", "v" } }) }));
            const Result N = Run(Full(NoAwi));
            Check(N.ExitCode() == 2 && Has(N, "awi.schema", Severity::Failure, "unknownkind:zone_thing") == 1, "an entry whose GDF has no awi cannot be judged: exit 2", Describe(N));
        }

        // 2b. the sky chain (M4e): a sky material needs an EXR image; a sky xmodel's skinOverride must name a material of the output
        {
            const fs::path Sky = Base / "sky" / "bo3";
            Put(Sky / "texture_assets" / "t" / "c.tif", Tiff(8, 8, 3, { 100, 100, 100 }));
            std::vector<uint16_t> ExrPixels(8 * 4 * 3, 0x3c00);
            std::vector<uint8_t> ExrBytes;
            std::string ExrWhy;
            Exr::WriteHalfRgb(8, 4, ExrPixels, Exr::WriteCompression::Zips, ExrBytes, ExrWhy);
            const std::string ExrFile(ExrBytes.begin(), ExrBytes.end());
            Put(Sky / "model_export" / "sky.exr", ExrFile);
            Put(Sky / "model_export" / "trunc.exr", ExrFile.substr(0, ExrFile.size() - 3));
            Put(Sky / "model_export" / "ren.exr", ExrFile);
            Put(Mod / "model_export" / "other" / "x.xmodel_bin", "x");
            Put(Mod / "model_export" / "t6_props" / "vista" / "skybox" / "t6_skybox.xmodel_bin", "x");
            Put(Sky / "sky.gdt", Gdt({
                Ent("i_sky", "image", { { "baseImage", "model_export\\\\sky.exr" }, { "semantic", "HDR" }, { "type", "image" } }),
                Ent("i_notexr", "image", { { "baseImage", "texture_assets\\\\t\\\\c.tif" }, { "semantic", "HDR" }, { "type", "image" } }),
                Ent("i_trunc", "image", { { "baseImage", "model_export\\\\trunc.exr" }, { "semantic", "HDR" }, { "type", "image" } }),
                Ent("i_ren", "image", { { "baseImage", "model_export\\\\ren.exr" }, { "semantic", "diffuseMap" }, { "type", "image" } }),
                Ent("x_other", "xmodel", { { "filename", "other\\\\x.xmodel_bin" }, { "type", "rigid" } }),
                Ent("x_sky", "xmodel", { { "filename", "t6_props\\\\vista\\\\skybox\\\\t6_skybox.xmodel_bin" }, { "type", "rigid" } }),
                Ent("mtl_sky_trunc", "material", { { "colorMap", "i_trunc" }, { "materialType", "sky_latlong_hdr" }, { "surfaceType", "<none>" }, { "usage", "<not in editor>" } }),
                Ent("mtl_sky_ok", "material", { { "colorMap", "i_sky" }, { "materialType", "sky_latlong_hdr" }, { "surfaceType", "<none>" }, { "usage", "<not in editor>" } }),
                Ent("mtl_sky_tif", "material", { { "colorMap", "i_notexr" }, { "materialType", "sky_latlong_hdr" }, { "surfaceType", "<none>" }, { "usage", "<not in editor>" } }),
                Ent("mtl_sky_noimg", "material", { { "colorMap", "i_missing" }, { "materialType", "sky_latlong_hdr" }, { "surfaceType", "<none>" }, { "usage", "<not in editor>" } }),
                Ent("skybox_ok", "xmodel", { { "materials", "mtl_skybox_default\\r\\n" }, { "skinOverride", "mtl_skybox_default mtl_sky_ok\\r\\n" }, { "type", "rigid" } }),
                Ent("skybox_bad", "xmodel", { { "materials", "mtl_skybox_default\\r\\n" }, { "skinOverride", "mtl_skybox_default mtl_sky_nowhere\\r\\n" }, { "type", "rigid" } }),
                Ent("skybox_none", "xmodel", { { "materials", "mtl_skybox_default\\r\\n" }, { "skinOverride", "" }, { "type", "rigid" } }),
            }));
            const Result R = Run(Full(Sky));
            Check(Has(R, "sky.chain", Severity::Failure, "material:mtl_sky_ok") == 0 && Has(R, "sky.chain", Severity::Failure, "material:mtl_sky_tif") == 1 && Has(R, "sky.chain", Severity::Failure, "material:mtl_sky_noimg") == 1,
                  "a sky material with an EXR image passes; a TIFF or a missing image fails (sky.chain)", Describe(R));
            Check(Has(R, "image.format", Severity::Failure, "image:i_sky") == 0 && Has(R, "image.format", Severity::Failure, "image:i_trunc") == 1, "a sky EXR is checked in structure: a whole file passes, one cut short fails (image.format)", Describe(R));
            Check(Has(R, "image.format", Severity::Failure, "image:i_ren") == 1, "an .exr that no sky material uses does not skip the image rules", Describe(R));
            Check(Has(R, "awi.file", Severity::Failure, "xmodel:x_other") == 1 && Has(R, "awi.file", Severity::Failure, "xmodel:x_sky") == 0, "the Mod Tools root stands in only for the stock sky mesh, not any xmodel", Describe(R));
            Check(Has(R, "sky.chain", Severity::Failure, "xmodel:skybox_ok") == 0 && Has(R, "sky.chain", Severity::Failure, "xmodel:skybox_bad") == 1 && Has(R, "sky.chain", Severity::Failure, "xmodel:skybox_none") == 1,
                  "a sky xmodel whose skinOverride names a material of the output passes; a missing material or none fails (sky.chain)", Describe(R));
        }

        // 3. parent entries are resolved and validated
        {
            const fs::path P = Base / "parent" / "bo3";
            Put(P / "m.gdt", "{\r\n"
                + Ent("mtl_base", "material", { { "materialType", "lit" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } })
                + "\t\"mtl_child\" [ \"mtl_base\" ]\r\n\t{\r\n\t\t\"surfaceType\" \"bogus\"\r\n\t}\r\n"
                + "\t\"mtl_grandchild\" [ \"mtl_child\" ]\r\n\t{\r\n\t\t\"surfaceType\" \"metal\"\r\n\t}\r\n"
                + "\t\"mtl_orphan\" [ \"nope\" ]\r\n\t{\r\n\t\t\"surfaceType\" \"metal\"\r\n\t}\r\n"
                + "\t\"c1\" [ \"c2\" ]\r\n\t{\r\n\t}\r\n\t\"c2\" [ \"c1\" ]\r\n\t{\r\n\t}\r\n"
                + "}\r\n");
            const Result R = Run(Full(P));
            Check(Has(R, "awi.combo", Severity::Failure, "material:mtl_child") == 1, "a derived entry is validated after the merge (bad surfaceType)", Describe(R));
            Check(Has(R, "awi.required", Severity::Failure, "material:mtl_child") == 0 && Has(R, "awi.required", Severity::Failure, "material:mtl_grandchild") == 0, "materialType comes from the parent chain", Describe(R));
            Check(Has(R, "awi.combo", Severity::Failure, "material:mtl_grandchild") == 0, "the grandchild overrides the bad value", Describe(R));
            Check(Has(R, "gdt.parent") >= 3 && R.ExitCode() == 2, "an unresolvable parent and a cycle are failures", Describe(R));
        }

        // 4. a path that leaves the layout does not resolve
        {
            const fs::path Root = Base / "esc" / "bo3";
            Put(Base / "outside" / "o.tif", Tiff(8, 8, 3, { 100, 100, 100 }));
            Put(Base / "outside" / "m.xmodel_bin", "x");
            Put(Root / "t.gdt", Gdt({
                Ent("i_abs", "image", { { "baseImage", Esc((Base / "outside" / "o.tif").string()) }, { "compressionMethod", "compressed" }, { "coreSemantic", "sRGB3chAlpha" }, { "semantic", "diffuseMap" }, { "type", "image" } }),
                Ent("i_dotdot", "image", { { "baseImage", "..\\\\..\\\\outside\\\\o.tif" }, { "compressionMethod", "compressed" }, { "coreSemantic", "sRGB3chAlpha" }, { "semantic", "diffuseMap" }, { "type", "image" } }),
                Ent("x_dotdot", "xmodel", { { "filename", "..\\\\..\\\\..\\\\outside\\\\m.xmodel_bin" }, { "type", "rigid" } }),
            }));
            const Result R = Run(Full(Root));
            Check(Has(R, "image.source", Severity::Failure, "image:i_abs") == 1 && Has(R, "image.source", Severity::Failure, "image:i_dotdot") == 1, "an absolute path and a .. escape do not resolve (image.source)", Describe(R));
            Check(Has(R, "awi.file", Severity::Failure, "xmodel:x_dotdot") == 1, "a .. escape does not resolve (awi.file)", Describe(R));
        }

        // 5. image headers and decoding
        {
            const std::string Bad12 = PngHeader(12, 8, 8), Zero = PngHeader(13, 0, 8), Good = PngHeader(13, 8, 8);
            Check(!ReadImageHeader(Reader(Bad12)).Ok, "a PNG IHDR length other than 13 is refused");
            Check(!ReadImageHeader(Reader(Zero)).Ok, "a PNG with a zero dimension is refused");
            Check(ReadImageHeader(Reader(Good)).Ok, "a good PNG header is read");
            const std::string T0 = TiffRaw(0, 8, { 8, 8, 8 }, { 1, 1, 1 });
            Check(!ReadImageHeader(Reader(T0)).Ok, "a TIFF with a zero dimension is refused");
            const std::string Fl = TiffRaw(4, 4, { 32, 32, 32 }, { 3, 3, 3 });
            const ImageInfo F = ReadImageHeader(Reader(Fl));
            Check(F.Ok && F.Float, "SampleFormat with count 3 is read from the pointed-to data", F.Why);
            const std::string MixF = TiffRaw(4, 4, { 8, 8, 8 }, { 1, 1, 3 });
            Check(!ReadImageHeader(Reader(MixF)).Ok, "samples of different sample formats are refused");
            const std::string Mix = TiffRaw(4, 4, { 8, 8, 16 }, { 1, 1, 1 });
            Check(!ReadImageHeader(Reader(Mix)).Ok, "samples of different bit depth are refused");
            const std::string Ok = TiffRaw(4, 4, { 16, 16, 16 }, { 1, 1, 1 });
            const ImageInfo K = ReadImageHeader(Reader(Ok));
            Check(K.Ok && K.BitsPerChannel == 16 && !K.Float, "three 16-bit unsigned samples", K.Why);
            // the IFD offset is near 2^32: Ifd + 2 must not wrap to 0
            const std::string Head = std::string("II") + std::string("\x2A\x00\xFE\xFF\xFF\xFF", 6);
            const std::string Wrapped = TiffRaw(8, 8, { 8, 8, 8 }, { 1, 1, 1 });
            const ImageInfo W = ReadImageHeader([&](uint64_t At, void* To, size_t N) {
                if (At + N <= Head.size()) { std::memcpy(To, &Head[At], N); return true; }
                if (At == 0xFFFFFFFEull && N == 2) { const char C[2] = { 10, 0 }; std::memcpy(To, C, 2); return true; }
                if (At == 0 && N == 120) { std::memcpy(To, &Wrapped[10], 120); return true; }     // what a wrapped Ifd + 2 would read
                return false;
            });
            Check(!W.Ok, "an IFD offset near 2^32 does not wrap", std::to_string(W.Width));

            // a required image that cannot be decoded is a failure, and a grey mask that cannot be judged is not skipped
            const fs::path Root = Base / "dec" / "bo3";
            Put(Root / "n.png", Good);
            Put(Root / "g.png", Good);
            Put(Root / "t.gdt", Gdt({
                Ent("i_g", "image", { { "baseImage", "g.png" }, { "compressionMethod", "compressed" }, { "coreSemantic", "Linear1ch" }, { "semantic", "glossMap" }, { "type", "image" } }),
                Ent("i_n", "image", { { "baseImage", "n.png" }, { "compressionMethod", "compressed" }, { "coreSemantic", "Normal" }, { "semantic", "normalMap" }, { "type", "image" } }),
            }));
            const Result R = Run(Full(Root));
            Check(Has(R, "image.decode", Severity::Failure, "image:i_n") == 1, "an undecodable normal map is a failure", Describe(R));
            Check(Has(R, "image.decode", Severity::Failure, "image:i_g") == 1 || Has(R, "image.channels", Severity::Failure, "image:i_g") == 1, "an undecodable multi-channel mask is not skipped", Describe(R));
            Check(R.ExitCode() == 2, "so the run fails");
        }

        // 6. the BOM has its own named failure
        {
            const fs::path Root = Base / "bom" / "bo3";
            Put(Root / "bom.gdt", std::string("\xEF\xBB\xBF") + Gdt({ Ent("b", "image", { { "type", "image" } }) }));
            const Result R = Run(Full(Root));
            Check(Has(R, "gdt.bom", Severity::Failure, "file:bom.gdt") == 1 && Has(R, "gdt.syntax") == 0, "a BOM is the named failure gdt.bom, not a syntax error", Describe(R));
        }

        // run identity: the TSV names the layout and the files it read; the identity changes with the files
        {
            const fs::path Root = Base / "ident" / "bo3";
            Put(Root / "a.gdt", Gdt({ Ent("zz", "image", { { "type", "image" } }) }));
            const Result R = Run(Full(Root));
            const std::string Tsv = R.Tsv();
            Check(Tsv.find("\ngate.run\tscan\tinfo\tmode=scan gdts=1 sha256=") != std::string::npos && !R.Identity.empty() && R.Mode == "scan", "the TSV carries a gate.run row", Tsv.substr(0, 200));
            std::string H1, H2, Why;
            size_t N = 0;
            Check(LayoutIdentity({ Root }, H1, N, Why) && N == 1 && H1 == R.Identity, "LayoutIdentity equals the gate's identity", Why);
            Put(Root / "a.gdt", Gdt({ Ent("zy", "image", { { "type", "image" } }) }));
            Check(LayoutIdentity({ Root }, H2, N, Why) && H2 != H1, "a changed GDT changes the identity");
            Check(Tsv.find("incomplete") != std::string::npos || R.Incomplete.empty(), "incomplete rows are written");
        }

        // the gate still holds only while the GDT content is the same (scan and listed alike)
        {
            const fs::path Root = Base / "hold" / "bo3";
            Put(Root / "a.gdt", Gdt({ Ent("zz", "image", { { "type", "image" } }) }));
            Put(Root / "sub" / "b.gdt", Gdt({ Ent("yy", "image", { { "type", "image" } }) }));
            const Result S = Run(Full(Root));
            Check(S.Tsv().find("\ngate.file\ta.gdt\tinfo\troot=0\n") != std::string::npos && S.GdtFiles.size() == 2, "the TSV lists every GDT read (gate.file rows)", S.Tsv().substr(0, 300));
            const std::vector<std::string> Roots = { Root.string() };
            std::string Why;
            Check(GateStillHolds("scan", Roots, S.GdtFiles, S.Identity, S.IdentityFiles, Why), "a scanned gate holds while the files are unchanged", Why);
            Put(Root / "sub" / "b.gdt", Gdt({ Ent("yx", "image", { { "type", "image" } }) }));
            Check(!GateStillHolds("scan", Roots, S.GdtFiles, S.Identity, S.IdentityFiles, Why) && Why.find("changed after the gate ran") != std::string::npos, "a scanned gate does not hold after a GDT changed", Why);
            Put(Root / "sub" / "b.gdt", Gdt({ Ent("yy", "image", { { "type", "image" } }) }));
            Put(Root / "c.gdt", Gdt({ Ent("new", "image", { { "type", "image" } }) }));
            Check(!GateStillHolds("scan", Roots, S.GdtFiles, S.Identity, S.IdentityFiles, Why), "a scanned gate does not hold when a GDT was added", Why);
            fs::remove(Root / "c.gdt", Ec);
            Check(GateStillHolds("scan", Roots, S.GdtFiles, S.Identity, S.IdentityFiles, Why), "... and holds again when it is the same set", Why);
            Check(!GateStillHolds("scan", { (Base / "hold" / "gone").string() }, S.GdtFiles, S.Identity, S.IdentityFiles, Why) && Why.find("cannot be re-read") != std::string::npos, "a layout that cannot be re-read: not accepted", Why);

            // listed mode (what --target modtools writes)
            Inputs L = Full(Root);
            L.ListedOnly = true;
            L.OnlyFiles = { Root / "a.gdt" };
            const Result Lr = Run(L);
            Check(Lr.Mode == "listed" && Lr.GdtFiles.size() == 1, "a listed run records the files it read", Lr.Mode);
            Check(GateStillHolds("listed", Roots, Lr.GdtFiles, Lr.Identity, Lr.IdentityFiles, Why), "a listed gate holds while the listed files are unchanged", Why);
            Put(Root / "a.gdt", Gdt({ Ent("zz", "image", { { "type", "image" }, { "mips", "3" } }) }));
            Check(!GateStillHolds("listed", Roots, Lr.GdtFiles, Lr.Identity, Lr.IdentityFiles, Why) && Why.find("changed after the gate ran") != std::string::npos, "an old listed gate does not hold after the GDT changed (path alone no longer counts)", Why);
            Check(!GateStillHolds("listed", Roots, {}, Lr.Identity, Lr.IdentityFiles, Why) && Why.find("gate.file") != std::string::npos, "a listed gate naming no files cannot be re-verified", Why);
            Check(!GateStillHolds("other", Roots, Lr.GdtFiles, Lr.Identity, Lr.IdentityFiles, Why), "an unknown mode is not accepted");
        }

        // --target modtools: the decision to scan nothing but the manifest's GDTs
        {
            Output::Manifest Listed;
            std::vector<fs::path> Files;
            std::string Why;
            Check(!ModToolsGdtFiles(Listed, false, Base / "mt", Files, Why) && Files.empty() && Why.find("did not load") != std::string::npos, "a manifest that did not load: the gate is not run", Why);
            Check(!ModToolsGdtFiles(Listed, true, Base / "mt", Files, Why) && Why.find("lists no .gdt") != std::string::npos, "an empty manifest: the gate is not run", Why);
            Output::ManifestEntry Cfg;
            Cfg.Path = "share\\raw\\x.cfg";
            Cfg.AssetName = "x";
            Listed.Record(Cfg);
            Check(!ModToolsGdtFiles(Listed, true, Base / "mt", Files, Why), "a manifest listing no GDT file: the gate is not run");
            Output::ManifestEntry G;
            G.Path = "source_data\\graywolf_x.GDT";
            G.AssetName = "x";
            Listed.Record(G);
            Check(ModToolsGdtFiles(Listed, true, Base / "mt", Files, Why) && Files.size() == 1 && Files[0] == Base / "mt" / "source_data\\graywolf_x.GDT", "only the listed GDT is read", std::to_string(Files.size()));
            // a listed-only run with an empty list never scans the root
            Put(Base / "mt" / "stock" / "stock.gdt", Gdt({ Ent("stock_a", "image", { { "type", "image" } }) }));
            Inputs In = Full(Base / "mt");
            In.ListedOnly = true;
            const Result R = Run(In);
            Check(R.Entries == 0 && R.ExitCode() == 2 && Has(R, "gate.no_input") == 1, "ListedOnly with no files fails and does not scan the root", Describe(R));
        }

        fs::remove_all(Base, Ec);
        return Failed;
    }

    // ---- self-test: the linker's slot table, the material closure of a model, the install collisions

    namespace
    {
        // A .xmodel_bin holding only MATERIAL records: "*LZ4*", the size, one literal-only LZ4 block.
        // Declared: NUMMATERIALS value, -1 none, -2 the number of materials. Header false: no MODEL and VERSION records (like an animation stream).
        std::string XModelBin(const std::vector<std::string>& Materials, int Declared = -2, bool Header = true)
        {
            std::string Payload;
            const auto P16 = [&](uint32_t V) { Payload += static_cast<char>(V & 255); Payload += static_cast<char>((V >> 8) & 255); };
            const auto Str = [&](const std::string& T) {
                Payload += T;
                Payload.append(((T.size() + 1 + 3) & ~size_t(3)) - T.size(), '\0');
            };
            // CRC16-CCITT of the keyword, seeded with its value type (MATERIAL UInt16String3 = 0xf, MODEL Marker = 1, UInt16 = 3)
            const auto Tag = [](const char* Name, uint32_t Type) {
                uint32_t Crc = Type;
                for (const char Ch : std::string_view(Name))
                {
                    Crc ^= static_cast<uint32_t>(static_cast<unsigned char>(Ch)) << 8;
                    for (int B = 0; B < 8; B++) Crc = (Crc & 0x8000) ? ((Crc << 1) ^ 0x1021) : (Crc << 1);
                }
                return Crc & 0xFFFF;
            };
            if (Header)
            {
                P16(Tag("MODEL", 1));
                P16(0);
                P16(Tag("VERSION", 3));
                P16(7);
            }
            if (Declared != -1)
            {
                P16(Tag("NUMMATERIALS", 3));
                P16(Declared == -2 ? static_cast<uint32_t>(Materials.size()) : static_cast<uint32_t>(Declared));
            }
            for (size_t I = 0; I < Materials.size(); I++)
            {
                P16(Tag("MATERIAL", 0xf));
                P16(static_cast<uint32_t>(I));
                Str(Materials[I]);
                Str("lambert");
                Str("color:x.tif");
            }
            std::string Block;
            if (Payload.size() < 15) Block += static_cast<char>(Payload.size() << 4);
            else
            {
                Block += '\xF0';
                size_t Rest = Payload.size() - 15;
                while (Rest >= 255) { Block += '\xFF'; Rest -= 255; }
                Block += static_cast<char>(Rest);
            }
            Block += Payload;
            std::string Out = "*LZ4*";
            const uint32_t N = static_cast<uint32_t>(Payload.size());
            for (int S = 0; S < 32; S += 8) Out += static_cast<char>((N >> S) & 255);
            return Out + Block;
        }
    }

    int LinkerTests(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got = std::string()) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL Verify(linker) {} (got '{}')\n", Name, Got);
        };
        std::error_code Ec;
        const fs::path Base = fs::temp_directory_path(Ec) / std::format("gw_verify_linker_{}", static_cast<unsigned long>(::GetCurrentProcessId()));
        fs::remove_all(Base, Ec);
        const fs::path Mod = Base / "modtools";
        Put(Mod / "deffiles" / "image.awi", R"AWI(
void GenerateUI( asset Asset )
{
    Asset.AddEntry_Combo( "semantic", "diffuseMap | normalMap | glossMap | specularMap | specularMask | occlusionMap | thicknessMap | revealMap" );
    Asset.AddEntry_Combo( "compressionMethod", "compressed | uncompressed | compressed high color{compressed high color}" );
    Asset.AddEntry_Texture( "baseImage", "" );
    Asset.AddEntry_String( "coreSemantic", "" );
    Asset.AddEntry_Combo( "type", "image | composite" );
}
)AWI");
        Put(Mod / "deffiles" / "material.awi", R"AWI(
void GenerateUI( asset Asset )
{
    Asset.AddEntry_Combo( "materialType", Asset.GetTechsetdefTypes( "" ) );
    Asset.AddEntry_Combo( "surfaceType", "<error> | <none> | concrete | metal" );
    Asset.AddEntry_Combo( "usage", "<not in editor> | floor" );
    Asset.AddEntry_AssetCombo( "colorMap", "image" );
}
)AWI");
        // the linker's own technique texture names (aoMap, glossMap, thicknessMap, revealMap) and GDT keys, as the shipped catalog has them
        Tc::Data Catalog;
        Check(Catalog.LoadCatalogText(R"({"techsets":[
            {"n":"lit_advanced","c":"Geometry Advanced","f":"none","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["normalMap","normalMap","normalMap","$identitynormalmap"],
              ["aoMap","occMap","occlusionMap","$white_ao"],["glossMap","cosinePowerMap","glossMap","$white_gloss"],["specColorMap","specColorMap","specularMask","$white_specular"]]},
            {"n":"lit_backlit","c":"Geometry","f":"none","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["thicknessMap","alphaRevealMap","thicknessMap","$thickness"]]},
            {"n":"lit_decal_reveal","c":"Geometry","f":"none","b":"Decal","t":0,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["revealMap","alphaRevealMap","revealMap","$white_reveal"]]}]})"), "linker: the synthetic catalog loads");

        const auto Image = [&](const char* Name, const char* Sem, const char* Core, const char* Comp) {
            return Ent(Name, "image", Img("x.tif", Sem, Core, Comp));
        };
        const std::string Out = (Base / "out" / "bo3").string();
        Put(Base / "out" / "bo3" / "texture_assets" / "t" / "x.tif", Tiff(8, 8, 3, { 100, 100, 100 }));
        Put(Base / "out" / "bo3" / "texture_assets" / "t" / "same.tif", Tiff(8, 8, 3, { 100, 100, 100 }));
        Put(Base / "out" / "bo3" / "texture_assets" / "t" / "diff.tif", Tiff(8, 8, 3, { 101, 100, 100 }));
        Put(Mod / "texture_assets" / "t" / "same.tif", Tiff(8, 8, 3, { 100, 100, 100 }));      // installed, identical bytes: no warning
        Put(Mod / "texture_assets" / "t" / "diff.tif", Tiff(8, 8, 3, { 7, 7, 7 }));            // installed, other bytes: a warning
        Put(Base / "out" / "bo3" / "texture_assets" / "t" / "dirimg.tif", Tiff(8, 8, 3, { 100, 100, 100 }));
        fs::create_directories(Mod / "texture_assets" / "t" / "dirimg.tif", Ec);              // installed path is a directory: cannot be compared, incomplete
        Put(Base / "out" / "bo3" / "texture_assets" / "t" / "locked.tif", Tiff(8, 8, 3, { 100, 100, 100 }));
        Put(Mod / "texture_assets" / "t" / "locked.tif", Tiff(8, 8, 3, { 100, 100, 100 }));   // installed, held open exclusively below: cannot be read, incomplete
        const std::vector<std::string> Entries = {
            Image("i_c", "diffuseMap", "sRGB3chAlpha", "compressed high color"), Image("i_n", "normalMap", "Normal", "compressed"),
            Image("i_gloss", "glossMap", "Linear1ch", "compressed"), Image("i_occ", "occlusionMap", "Linear1ch", "compressed"),
            Image("i_thick", "thicknessMap", "Linear1ch", "compressed"), Image("i_specmask", "specularMask", "Linear1ch", "compressed"),
            Image("i_reveal", "revealMap", "Linear1ch", "compressed"),
            // the five rows of the negative control, every slot on lit_advanced bound to the wrong semantic
            Ent("mtl_mis", "material", { { "colorMap", "i_gloss" }, { "cosinePowerMap", "i_specmask" }, { "materialType", "lit_advanced" }, { "normalMap", "i_occ" },
                                         { "occMap", "i_thick" }, { "specColorMap", "i_gloss" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            // the same slots bound correctly
            Ent("mtl_right", "material", { { "colorMap", "i_c" }, { "cosinePowerMap", "i_gloss" }, { "materialType", "lit_advanced" }, { "normalMap", "i_n" },
                                           { "occMap", "i_occ" }, { "specColorMap", "i_specmask" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            Ent("mtl_bl_ok", "material", { { "alphaRevealMap", "i_thick" }, { "colorMap", "i_c" }, { "materialType", "lit_backlit" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            Ent("mtl_bl_bad", "material", { { "alphaRevealMap", "i_reveal" }, { "colorMap", "i_c" }, { "materialType", "lit_backlit" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            Ent("mtl_dr_ok", "material", { { "alphaRevealMap", "i_reveal" }, { "colorMap", "i_c" }, { "materialType", "lit_decal_reveal" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            Ent("mtl_dr_bad", "material", { { "alphaRevealMap", "i_thick" }, { "colorMap", "i_c" }, { "materialType", "lit_decal_reveal" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            // install collisions: two images of the output (same.tif is installed with the same bytes, diff.tif with other bytes)
            Ent("i_same", "image", Img("same.tif", "diffuseMap", "sRGB3chAlpha", "compressed high color")),
            Ent("i_diff", "image", Img("diff.tif", "diffuseMap", "sRGB3chAlpha", "compressed high color")),
            Ent("i_dirimg", "image", Img("dirimg.tif", "diffuseMap", "sRGB3chAlpha", "compressed high color")),
            Ent("i_locked", "image", Img("locked.tif", "diffuseMap", "sRGB3chAlpha", "compressed high color")),
            Ent("mtl_in_db_as_image", "material", { { "colorMap", "i_c" }, { "materialType", "lit_backlit" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            Ent("mtl_in_db_same_type", "material", { { "colorMap", "i_c" }, { "materialType", "lit_backlit" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
        };
        Put(Base / "out" / "bo3" / "texture_assets" / "t.gdt", Gdt(Entries));
        Put(Base / "out" / "bo3" / "model_export" / "m" / "ok_lod0.xmodel_bin", XModelBin({ "mtl_right", "MTL_Mis", "mtl_stock_only" }));
        Put(Base / "out" / "bo3" / "model_export" / "m" / "bad_lod0.xmodel_bin", XModelBin({ "mtl_right", "mtl_nowhere", "mtl_nowhere" }));
        Put(Base / "out" / "bo3" / "model_export" / "m" / "junk_lod0.xmodel_bin", "this is not an xmodel_bin");
        Put(Base / "out" / "bo3" / "model_export" / "m" / "short_lod0.xmodel_bin", XModelBin({ "mtl_right" }, 2));         // declares 2, holds 1: cut short
        Put(Base / "out" / "bo3" / "model_export" / "m" / "nodecl_lod0.xmodel_bin", XModelBin({ "mtl_nowhere" }, -1));    // no NUMMATERIALS
        Put(Base / "out" / "bo3" / "model_export" / "m" / "anim_lod0.xmodel_bin", XModelBin({}, 0, false));               // a stream with no model header (an animation renamed)
        Put(Base / "out" / "bo3" / "model_export" / "m" / "zero_lod0.xmodel_bin", XModelBin({}, 0));                      // a real header and a declared zero: passes
        GdtDb::Catalog Held;
        GdtDb::Row Stock;
        Stock.Name = "mtl_stock_only";
        Stock.Gdf = "material";
        Held.Add(Stock);
        Stock.Name = "mtl_in_db_as_image";
        Stock.Gdf = "image";
        Held.Add(Stock);
        Stock.Name = "mtl_in_db_same_type";
        Stock.Gdf = "material";
        Held.Add(Stock);

        Inputs In;
        In.Roots = { Base / "out" / "bo3" };
        In.ModToolsRoot = Mod;
        In.Catalog = &Catalog;
        In.Held = &Held;
        // the exporter says it left mtl_nowhere out on purpose: the closure failure must name that reason
        Put(Base / "out" / "graywolf_approximations.tsv", "code\tasset\tdetail\tmessage\nmaterial.skipped_combo_image\tmtl_nowhere\tgame=bo3, image=i_x_combo\tleft out\n");
        In.Approximations = Base / "out" / "graywolf_approximations.tsv";
        // hold the installed file open with no sharing, so the gate cannot read it
        const HANDLE Lock = ::CreateFileW((Mod / "texture_assets" / "t" / "locked.tif").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const Result R = Run(In);
        if (Lock != INVALID_HANDLE_VALUE) ::CloseHandle(Lock);
        const auto Count = [&](const char* Check1, const char* Asset, Severity S = Severity::Failure) {
            size_t N = 0;
            for (const Finding& F : R.Findings)
                if (F.Check == Check1 && F.Level == S && F.Asset == Asset) N++;
            return N;
        };
        const auto Describe = [&] {
            std::string L;
            for (const Finding& F : R.Findings)
                if (F.Check.rfind("material.slot", 0) == 0 || F.Check.rfind("model.", 0) == 0 || F.Check.rfind("install.", 0) == 0) L += F.Check + "@" + F.Asset + ":" + F.Detail.substr(0, 60) + "; ";
            return L;
        };

        // 5. the linker's slot table: one case per row of the negative control, then the correct binding and the two techsets with a special slot
        Check(Count("material.slot_semantic", "material:mtl_mis") == 5, "linker: the five wrong slots of the negative control each fail", Describe());
        bool Named = false;
        for (const Finding& F : R.Findings)
            if (F.Check == "material.slot_semantic" && F.Asset == "material:mtl_mis" && F.Detail.find("colorMap (linker slot colorMap) takes diffuseMap but image \"i_gloss\" has semantic glossMap") != std::string::npos) Named = true;
        Check(Named, "linker: the message gives the linker's slot name and the semantic it wants", Describe());
        bool Ao = false, Gl = false, Sp = false;
        for (const Finding& F : R.Findings)
        {
            if (F.Check != "material.slot_semantic" || F.Asset != "material:mtl_mis") continue;
            Ao = Ao || F.Detail.find("occMap (linker slot aoMap) takes occlusionMap but image \"i_thick\" has semantic thicknessMap") != std::string::npos;
            Gl = Gl || F.Detail.find("cosinePowerMap (linker slot glossMap) takes glossMap but image \"i_specmask\" has semantic specularMask") != std::string::npos;
            Sp = Sp || F.Detail.find("specColorMap (linker slot specColorMap) takes specularMask but image \"i_gloss\" has semantic glossMap") != std::string::npos;
        }
        Check(Ao && Gl && Sp, "linker: occMap is aoMap (occlusionMap), cosinePowerMap is glossMap, specColorMap wants specularMask", Describe());
        Check(Count("material.slot_semantic", "material:mtl_right") == 0, "linker: the right semantic in every lit_advanced slot passes", Describe());
        Check(Count("material.slot_semantic", "material:mtl_bl_ok") == 0 && Count("material.slot_semantic", "material:mtl_bl_bad") == 1, "linker: lit_backlit takes thickness in alphaRevealMap, a reveal map fails", Describe());
        Check(Count("material.slot_semantic", "material:mtl_dr_ok") == 0 && Count("material.slot_semantic", "material:mtl_dr_bad") == 1, "linker: lit_decal_reveal takes reveal in alphaRevealMap, a thickness map fails", Describe());

        // 3. model.material_closure
        Check(Count("model.material_closure", "xmodel_bin:model_export/m/ok_lod0.xmodel_bin") == 0, "closure: output materials (any case) and a gdt.db material pass", Describe());
        Check(Count("model.material_closure", "xmodel_bin:model_export/m/bad_lod0.xmodel_bin") == 1, "closure: a material in no GDT and not in the gdt.db fails once", Describe());
        {
            bool Named = false;
            for (const Finding& F : R.Findings)
                if (F.Check == "model.material_closure" && F.Asset == "xmodel_bin:model_export/m/bad_lod0.xmodel_bin" && F.Detail.find("skipped on purpose (material.skipped_combo_image") != std::string::npos) Named = true;
            Check(Named, "closure: a model naming a material the exporter skipped says so and gives the reason", Describe());
        }
        bool Unparsed = false, JunkFailed = Count("model.material_closure", "xmodel_bin:model_export/m/junk_lod0.xmodel_bin") != 0;
        for (const auto& [Name, Why] : R.Incomplete) Unparsed = Unparsed || (Name == "model.material_closure (unparsed)" && Why.find("junk_lod0") != std::string::npos);
        {
            std::string Text;
            for (const auto& [Name, Why] : R.Incomplete) if (Name == "model.material_closure (unparsed)") Text = Why;
            Check(Text.find("4 of 7") != std::string::npos,
                  "closure: a cut-short stream, one with no NUMMATERIALS, and one with no model header are not judged (incomplete)", Text);
            Check(Count("model.material_closure", "xmodel_bin:model_export/m/zero_lod0.xmodel_bin") == 0 && Text.find("zero_lod0") == std::string::npos, "closure: a real header with a declared zero passes", Text);
        }
        Check(Unparsed && !JunkFailed && R.ExitCode() == 2, "closure: a .xmodel_bin that cannot be parsed is incomplete, not a pass and not a made-up failure", Describe());
        {
            Inputs NoDb = In;
            NoDb.Held = nullptr;
            const Result N = Run(NoDb);
            bool Inc = false, Failed1 = false;
            for (const auto& [Name, Why] : N.Incomplete) Inc = Inc || Name == "model.material_closure (no gdt.db)";
            for (const Finding& F : N.Findings) Failed1 = Failed1 || F.Check == "model.material_closure";
            Check(Inc && !Failed1, "closure: without a gdt.db the unresolved names are incomplete, not a pass and not a failure");
        }

        // 4. install collisions
        Check(Count("install.image_overwrite", "image:i_diff", Severity::Warning) == 1, "install: an output baseImage that exists with other bytes warns", Describe());
        Check(Count("install.image_overwrite", "image:i_same", Severity::Warning) == 0 && Count("install.image_overwrite", "image:i_c", Severity::Warning) == 0, "install: identical bytes and a path not installed give no warning", Describe());
        {
            std::string Why;
            for (const auto& [Name, W] : R.Incomplete) if (Name == "install.image_overwrite (part)") Why = W;
            Check(Lock != INVALID_HANDLE_VALUE && !Why.empty() && Count("install.image_overwrite", "image:i_dirimg", Severity::Warning) == 0 && Count("install.image_overwrite", "image:i_locked", Severity::Warning) == 0,
                  "install: a directory or an unreadable file at the installed path is incomplete, never 'same' and never a warning", Why);
        }
        Check(Count("install.name_collision", "material:mtl_in_db_as_image", Severity::Warning) == 1, "install: a name the gdt.db holds as another type warns", Describe());
        Check(Count("install.name_collision", "material:mtl_in_db_same_type", Severity::Warning) == 0 && Count("gdt.stock_pair", "material:mtl_in_db_same_type") == 1, "install: the same name and type is the existing gdt.stock_pair failure, not a second finding", Describe());
        {
            Inputs NoRoot = In;
            NoRoot.ModToolsRoot.clear();
            const Result N = Run(NoRoot);
            bool Any = false;
            for (const Finding& F : N.Findings) Any = Any || F.Check == "install.image_overwrite";
            Check(!Any, "install: no Mod Tools root, no install comparison");
        }
        {
            std::vector<std::string> Names;
            std::string Why;
            Check(ReadXModelBinMaterials(Base / "out" / "bo3" / "model_export" / "m" / "ok_lod0.xmodel_bin", Names, Why) && Names.size() == 3 && Names[1] == "MTL_Mis", "closure: the MATERIAL names are read from the bin", Why);
            Check(!ReadXModelBinMaterials(Base / "out" / "bo3" / "model_export" / "m" / "junk_lod0.xmodel_bin", Names, Why), "closure: a non-bin is refused");
        }
        // Optional: real bins. GRAYWOLF_REAL_XMODEL_DIR names a folder of a GrayWolf export (outside the repo); every .xmodel_bin under it must parse
        // and agree with its own NUMMATERIALS, and the gate must have judged every one of them (all LODs). Skipped when the variable is not set.
        std::string RealDirText;
        RealDirText.resize(GetEnvironmentVariableA("GRAYWOLF_REAL_XMODEL_DIR", nullptr, 0));
        if (!RealDirText.empty()) RealDirText.resize(GetEnvironmentVariableA("GRAYWOLF_REAL_XMODEL_DIR", RealDirText.data(), (DWORD)RealDirText.size()));
        if (const char* RealDir = RealDirText.c_str(); !RealDirText.empty())
        {
            size_t Bins = 0, Parsed = 0;
            std::string FirstBad;
            std::error_code E3;
            for (fs::recursive_directory_iterator It(RealDir, fs::directory_options::skip_permission_denied, E3), End; !E3 && It != End; It.increment(E3))
                if (It->is_regular_file() && Lower(It->path().extension().string()) == ".xmodel_bin")
                {
                    Bins++;
                    std::vector<std::string> Names;
                    std::string Why;
                    if (ReadXModelBinMaterials(It->path(), Names, Why)) Parsed++;
                    else if (FirstBad.empty()) FirstBad = It->path().filename().string() + ": " + Why;
                }
            Check(Bins > 0 && Parsed == Bins, "closure: every real .xmodel_bin of the export parses and agrees with its NUMMATERIALS", std::to_string(Parsed) + " of " + std::to_string(Bins) + " " + FirstBad);
            Inputs RealIn;
            RealIn.Roots = { fs::path(RealDir) };
            const Result RR = Run(RealIn);
            const auto J = RR.Checks.find("model.material_closure");
            Check(J != RR.Checks.end() && J->second.Judged == Bins, "closure: the gate judged every real .xmodel_bin (all LODs)", std::to_string(J == RR.Checks.end() ? 0 : J->second.Judged) + " of " + std::to_string(Bins));
        }
        fs::remove_all(Base, Ec);
        return Failed;
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got = std::string()) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL Verify {} (got '{}')\n", Name, Got);
        };
        const auto Reader = [](const std::string& Bytes) {
            return [&Bytes](uint64_t At, void* To, size_t N) {
                if (At + N > Bytes.size()) return false;
                std::memcpy(To, &Bytes[At], N);
                return true;
            };
        };

        // header readers
        {
            const std::string T = Tiff(8, 4, 3, { 10, 20, 30 });
            const ImageInfo I = ReadImageHeader(Reader(T));
            Check(I.Ok && I.Width == 8 && I.Height == 4 && I.Channels == 3 && I.BitsPerChannel == 8 && I.Container == "tiff", "tiff header", I.Why);
            const std::string G = Tiff(4, 4, 1, { 7 });
            const ImageInfo J = ReadImageHeader(Reader(G));
            Check(J.Ok && J.Channels == 1 && J.BitsPerChannel == 8, "tiff header, one channel", J.Why);
            std::string Png = "\x89PNG\r\n\x1a\n";
            Png += std::string("\0\0\0\x0D", 4) + "IHDR" + std::string("\0\0\0\x10\0\0\0\x08\x10\x06\0\0\0", 13);
            const ImageInfo K = ReadImageHeader(Reader(Png));
            Check(K.Ok && K.Width == 16 && K.Height == 8 && K.BitsPerChannel == 16 && K.Channels == 4 && K.Container == "png", "png header", K.Why);
            const ImageInfo L = ReadImageHeader([](uint64_t, void* To, size_t N) { std::memset(To, 0, N); return true; });
            Check(!L.Ok, "garbage is not an image");
        }

        // pixel rules
        {
            Rgba8Image Flat;
            Flat.Width = Flat.Height = 8;
            const auto Fill = [&](uint8_t R, uint8_t G, uint8_t B) {
                Flat.Pixels.assign(8 * 8 * 4, 255);
                for (size_t I = 0; I < Flat.Pixels.size(); I += 4)
                {
                    Flat.Pixels[I] = R;
                    Flat.Pixels[I + 1] = G;
                    Flat.Pixels[I + 2] = B;
                }
                return MakeThumb(Flat);
            };
            Check(NormalBlue(Fill(128, 128, 255)).MeanAbs < 1.0, "a flat normal with blue 255 is on the implied Z", std::to_string(NormalBlue(Fill(128, 128, 255)).MeanAbs));
            Check(NormalBlue(Fill(128, 128, 0)).MeanAbs > 100, "a normal with blue 0 is far from Z");
            Check(DiffuseRange(Fill(10, 10, 10)).Below == static_cast<size_t>(kThumb) * kThumb && DiffuseRange(Fill(100, 100, 100)).Below == 0 && DiffuseRange(Fill(250, 250, 250)).Above > 0, "diffuse range counts");
            const SpecStat S = SpecularMode(Fill(56, 56, 56));
            Check(S.Mode == 56 && S.Share > 0.99, "specular mode", std::to_string(S.Mode));
            Check(SpecularMode(Fill(0, 0, 0)).Mode == -1, "an all-black specular has no mode");
            const MetalStat M = BareMetal(Fill(200, 200, 200), Fill(220, 220, 220));
            Check(M.MetalPixels > 0 && M.NotBlack == M.MetalPixels, "a bright diffuse on a metal specular is not black");
            Check(BareMetal(Fill(0, 0, 0), Fill(220, 220, 220)).NotBlack == 0, "a black diffuse on a metal specular is bare metal");
        }

        // end to end on a temporary layout
        std::error_code Ec;
        const fs::path Base = fs::temp_directory_path(Ec) / std::format("gw_verify_selftest_{}", static_cast<unsigned long>(::GetCurrentProcessId()));
        fs::remove_all(Base, Ec);
        const fs::path Mod = Base / "modtools", Out = Base / "out" / "bo3";
        Put(Mod / "deffiles" / "image.awi", R"AWI(
void GenerateUI( asset Asset )
{
    string sem = "diffuseMap | normalMap | glossMap | specularMap";
    string comp = "compressed | uncompressed | compressed high color{compressed high color}";
    Asset.AddEntry_Combo( "semantic", sem );
    Asset.AddEntry_Combo( "compressionMethod", comp );
    Asset.AddEntry_Texture( "baseImage", "" );
    Asset.AddEntry_String( "coreSemantic", "" );
    Asset.AddEntry_Combo( "type", "image | composite" );
    Asset.AddEntry_CheckBox( "colorSRGB", false );
    Asset.AddEntry_Int( "mips", 1, 1, 8 );
}
)AWI");
        Put(Mod / "deffiles" / "material.awi", R"AWI(
void GenerateUI( asset Asset )
{
    Asset.AddEntry_Combo( "materialType", Asset.GetTechsetdefTypes( "" ) );
    Asset.AddEntry_Combo( "surfaceType", "<error> | <none> | concrete | metal" );
    Asset.AddEntry_Combo( "usage", "<not in editor> | floor | decal" );
    Asset.AddEntry_AssetCombo( "colorMap", "image" );
    Asset.AddEntry_AssetCombo( "normalMap", "image" );
    Asset.AddEntry_AssetCombo( "specColorMap", "image" );
    Asset.AddEntry_Color( "colorTint", 1, 1, 1, 1 );
    Asset.AddEntry_Vector2( "uScroll", "vScroll", 0.0, 0.0, -10.0, 10.0 );
    Asset.AddEntry_Float( "desaturationAmount", 0.0, 0.0, 1.0 );
    Asset.AddEntry_Float( "glossRangeMax", 17, 0, 17 );
}
)AWI");
        Tc::Data Catalog;
        Check(Catalog.LoadCatalogText(R"({"techsets":[
            {"n":"lit","c":"Geometry","f":"none","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap",null],["normalMap","normalMap","normalMap",null]]},
            {"n":"lit_weapon_camo","c":"Weapon","f":"none","b":"Opaque","t":32768,"s":[["colorMap","colorMap","diffuseMap",null]]}]})"), "the synthetic catalog loads");

        const std::string Tif3 = Tiff(8, 8, 3, { 100, 100, 100 });
        Put(Out / "texture_assets" / "t" / "c.tif", Tif3);
        Put(Out / "texture_assets" / "t" / "n.tif", Tiff(8, 8, 3, { 128, 128, 255 }));
        Put(Out / "texture_assets" / "t" / "n_bad.tif", Tiff(8, 8, 3, { 128, 128, 0 }));
        Put(Out / "texture_assets" / "t" / "g3.tif", Tiff(8, 8, 3, { 90, 90, 90 }));
        Put(Out / "texture_assets" / "t" / "g3c.tif", Tiff(8, 8, 3, { 90, 10, 200 }));
        Put(Out / "texture_assets" / "t" / "s_off.tif", Tiff(8, 8, 3, { 80, 80, 80 }));
        Put(Out / "texture_assets" / "t" / "s_ok.tif", Tiff(8, 8, 3, { 56, 56, 56 }));
        Put(Out / "texture_assets" / "t" / "s_metal.tif", Tiff(8, 8, 3, { 220, 220, 220 }));
        Put(Out / "texture_assets" / "t" / "sm3.tif", Tiff(8, 8, 3, { 90, 90, 90 }));
        Put(Out / "texture_assets" / "t" / "odd.tif", Tiff(10, 6, 3, { 100, 100, 100 }));
        Fields NBad = Img("n_bad.tif", "normalMap", "Normal", "bogus");
        NBad.insert(NBad.begin() + 1, std::pair<std::string, std::string>{ "colorSRGB", "1" });
        NBad.push_back({ "mips", "2.5" });
        Put(Out / "texture_assets" / "t.gdt", Gdt({
            Ent("i_c", "image", Img("c.tif", "diffuseMap", "sRGB3chAlpha", "compressed high color")),
            Ent("i_dds", "image", Img("c.dds", "normalMap", "Normal", "compressed")),
            Ent("i_g3", "image", Img("g3.tif", "glossMap", "Linear1ch", "compressed")),
            Ent("i_g3c", "image", Img("g3c.tif", "glossMap", "Linear1ch", "compressed")),
            Ent("i_n", "image", Img("n.tif", "normalMap", "Normal", "compressed")),
            Ent("i_n_bad", "image", NBad),
            Ent("i_s_off", "image", Img("s_off.tif", "specularMap", "sRGB3ch", "compressed")),
            Ent("i_s_ok", "image", Img("s_ok.tif", "specularMap", "sRGB3ch", "compressed")),
            Ent("i_s_metal", "image", Img("s_metal.tif", "specularMap", "sRGB3ch", "compressed")),
            Ent("mtl_barem", "material", { { "colorMap", "i_c" }, { "materialType", "lit" }, { "specColorMap", "i_s_metal" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            Ent("i_sm3", "image", Img("sm3.tif", "specularMask", "Linear1ch", "compressed")),
            Ent("i_odd", "image", Img("odd.tif", "diffuseMap", "sRGB3chAlpha", "compressed")),
            Ent("i_x_missing", "image", Img("nope.tif", "normalMap", "Normal", "compressed")),
            Ent("mtl_bad", "material", { { "colorMap", "i_n" }, { "colorTint", "0.5 1 1 1" }, { "invented", "1" }, { "materialType", "lit_weapon_camo" }, { "normalMap", "i_c" },
                                         { "specColorMap", "i_nowhere" }, { "surfaceType", "<none>" }, { "uScroll", "11" }, { "usage", "floor" } }),
            Ent("mtl_ok", "material", { { "colorMap", "i_c" }, { "colorTint", "1 1 1 1" }, { "materialType", "lit" }, { "normalMap", "i_n" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
            Ent("mtl_slotname", "material", { { "colorMap", "i_c" }, { "glossRangeMax", "18" }, { "materialType", "lit" }, { "surfaceType", "metal" }, { "usage", "<not in editor>" } }),
            Ent("mtl_map_hidden", "material", { { "colorMap", "i_c" }, { "materialType", "lit" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
            Ent("mtl_map_shown", "material", { { "colorMap", "i_c" }, { "materialType", "lit" }, { "surfaceType", "concrete" }, { "usage", "floor" } }),
        }));
        // a second file: a duplicate pair, an LF file, a BOM, a quote in a value
        Put(Out / "source_data" / "dup.gdt", Gdt({ Ent("mtl_ok", "material", { { "materialType", "lit" } }) }));
        Put(Out / "source_data" / "lf.gdt", Gdt({ Ent("z", "image", { { "type", "image" } }, "\n") }, "\n"));
        Put(Out / "source_data" / "bom.gdt", std::string("\xEF\xBB\xBF") + Gdt({ Ent("b", "image", { { "type", "image" }, { "mips", "1" } }) }));
        Put(Out / "source_data" / "bad.gdt", "{\r\n\t\"q\" ( \"image.gdf\" )\r\n\t{\r\n\t\t\"a\" \"b\"c\"\r\n\t}\r\n}\r\n");

        Inputs In;
        In.Roots = { Out };
        In.ModToolsRoot = Mod;
        In.Catalog = &Catalog;
        const Result R = Run(In);
        const auto Count = [&](const char* Check1, const char* Asset = nullptr, Severity S = Severity::Failure) {
            size_t N = 0;
            for (const Finding& F : R.Findings)
                if (F.Check == Check1 && F.Level == S && (Asset == nullptr || F.Asset == Asset)) N++;
            return N;
        };
        Check(Count("image.source", "image:i_x_missing") == 1, "a baseImage that does not resolve fails");
        Check(Count("image.dds", "image:i_dds") == 1, "a .dds source fails");
        Check(Count("image.channels", "image:i_g3", Severity::Warning) == 1 && Count("image.channels", "image:i_g3") == 0, "a grey three-channel gloss map warns");
        Check(Count("image.channels", "image:i_g3c") == 1, "a coloured three-channel gloss map fails the channel rule", [&] { std::string L; for (const Finding& F : R.Findings) if (F.Asset == "image:i_g3c") L += F.Check + ":" + F.Detail + "; "; return L; }());
        Check(Count("rules.specular_level", "image:i_s_off", Severity::Warning) == 1 && Count("rules.specular_level", "image:i_s_off") == 0 && Count("rules.specular_level", "image:i_s_ok", Severity::Warning) == 0,
              "a flat specular off the 56 level warns (stock breaks the guideline on 37%), one on 56 is silent");
        Check(Count("rules.bare_metal", "material:mtl_barem", Severity::Warning) == 1 && Count("rules.bare_metal", "material:mtl_barem") == 0, "a bright diffuse on a metal specular warns, never fails");
        Check(Count("image.channels", "image:i_sm3") == 0 && Count("image.channels", "image:i_sm3", Severity::Warning) == 0 && Count("image.channels", "image:i_g3", Severity::Warning) == 1,
              "a three-channel specularMask is accepted as stock has it; the same three channels in a gloss map still warn");
        Check(Count("image.block_size", "image:i_odd") == 1, "10x6 compressed fails the multiple-of-4 rule");
        Check(Count("image.srgb", "image:i_n_bad") == 1 && Count("image.semantic_table", "image:i_n_bad") == 1, "colorSRGB 1 and an off-table compression fail");
        Check(Count("image.normal_z", "image:i_n_bad") == 1 && Count("image.normal_z", "image:i_n") == 0, "a normal with blue 0 fails, one with Z passes", std::to_string(Count("image.normal_z")));
        Check(Count("awi.type", "image:i_n_bad") == 1, "2.5 in an int key fails the awi type rule");
        Check(Count("awi.unknown_key", "material:mtl_bad") == 1, "a key the awi does not declare fails");
        Check(Count("awi.range", "material:mtl_bad") == 1 && Count("awi.range", "material:mtl_slotname") == 1, "a value outside min/max fails (scroll 11, gloss 18)");
        Check(Count("material.camo", "material:mtl_bad") == 1, "lit_weapon_camo fails");
        Check(Count("material.surface_type", "material:mtl_bad") == 1 && Count("material.surface_type", "material:mtl_ok") == 0, "surfaceType <none> is the known failure, a real value is not");
        Check(Count("material.usage", "material:mtl_bad") == 1 && Count("material.usage", "material:mtl_ok") == 0, "usage on a non-map material fails");
        Check(Count("material.slot_semantic", "material:mtl_bad") == 1, "an image of the wrong semantic in a slot fails", std::to_string(Count("material.slot_semantic", "material:mtl_bad")));
        Check(Count("material.slot_key", "material:mtl_bad") == 2, "image keys that are not slots of the techset fail", std::to_string(Count("material.slot_key", "material:mtl_bad")));
        Check(Count("material.tint", "material:mtl_bad", Severity::Warning) == 1 && Count("material.tint", "material:mtl_ok", Severity::Warning) == 0, "a tint off the k/255 steps warns");
        Check(Count("awi.reference_unresolved", "material:mtl_bad", Severity::Warning) == 0, "no gdt.db: an outside reference is not judged, not a failure");
        Check(Count("gdt.single_file", "material:mtl_ok") == 1, "one (name, GDF) in two files fails");
        Check(Count("gdt.syntax", "file:source_data/lf.gdt") == 1 && Count("gdt.bom", "file:source_data/bom.gdt") == 1 && Count("gdt.unsafe_value", "image:q") == 1, "LF is a syntax failure, a BOM is gdt.bom, a quote in a value is an unsafe value");
        Check(Count("image.source", "image:i_c") == 0 && Count("image.format", "image:i_c") == 0 && Count("image.bit_depth", "image:i_c") == 0, "a clean image has no format failure");
        // the run says which materials the maps draw: a drawn one left <not in editor> fails and names its surface, a model-only one with a usage fails
        {
            Inputs Map = In;
            Map.MapSources = true;
            const Result Before = Run(Map);
            const auto Failing = [&](const Result& X, const char* Asset) {
                std::string Detail;
                for (const Finding& F : X.Findings)
                    if (F.Check == "material.usage" && F.Level == Severity::Failure && F.Asset == Asset) Detail += F.Detail + "|";
                return Detail;
            };
            Check(Before.ExitCode() != 0 && std::any_of(Before.Incomplete.begin(), Before.Incomplete.end(), [](const auto& I) { return I.first == "material.usage"; }),
                  "map sources and no list of the drawn materials: the usage check is incomplete, not a pass");
            const auto Infos = [&](const Result& X, const char* Asset) {
                size_t N = 0;
                for (const Finding& F : X.Findings)
                    if (F.Check == "material.usage" && F.Level == Severity::Info && F.Asset == Asset) N++;
                return N;
            };
            // with no list the gate says so for each material with a usage (an Info row each), and fails none of them
            Check(Failing(Before, "material:mtl_map_hidden").empty() && Failing(Before, "material:mtl_map_shown").empty() && Failing(Before, "material:mtl_bad").empty() &&
                      Infos(Before, "material:mtl_bad") == 1 && Infos(Before, "material:mtl_map_shown") == 1 && Infos(Before, "material:mtl_ok") == 0,
                  "map sources and no list of the drawn materials: usage is not judged (an Info row each, no failure)");
            Map.MapDrawn = { { "mtl_map_hidden", "world#7" }, { "mtl_map_shown", "terrain_0#3" } };
            Map.MapDrawnKnown = true;
            const Result Known = Run(Map);
            const std::string Hidden = Failing(Known, "material:mtl_map_hidden");
            Check(Hidden.find("world#7") != std::string::npos && Hidden.find("<not in editor>") != std::string::npos, "a map-drawn material left <not in editor> fails with the surface that draws it", Hidden);
            Check(Failing(Known, "material:mtl_map_shown").empty(), "a map-drawn material with a usage passes", Failing(Known, "material:mtl_map_shown"));
            Check(!Failing(Known, "material:mtl_bad").empty(), "a model-only material with a usage still fails", Failing(Known, "material:mtl_bad"));
            // a pass needs proof the check ran: every entry with a usage key was judged, and the model-only one left <not in editor> has no finding at all
            size_t OkFindings = 0;
            for (const Finding& F : Known.Findings)
                if (F.Check == "material.usage" && F.Asset == "material:mtl_ok") OkFindings++;
            Check(Known.Checks.count("material.usage") != 0 && Known.Checks.at("material.usage").Judged >= 6 && OkFindings == 0 && Known.Checks.at("material.usage").Failures == 2,
                  "a model-only material left <not in editor> passes: all usage keys judged, exactly the drawn-hidden and the model-only-with-usage fail",
                  Known.Checks.count("material.usage") ? std::to_string(Known.Checks.at("material.usage").Judged) + "/" + std::to_string(Known.Checks.at("material.usage").Failures) : "(none)");
            // held materials: a warning naming the gdt, only where the held usage hides the material or cannot be read
            Map.MapHeld = { { "mtl_held_hidden", { "model_export\\t7.gdt", "<not in editor>" } }, { "mtl_held_shown", { "model_export\\t7.gdt", "floor" } }, { "mtl_held_unread", { "", "unavailable: no file" } } };
            const Result Heldr = Run(Map);
            size_t HeldWarn = 0, HeldNamed = 0;
            for (const Finding& F : Heldr.Findings)
                if (F.Check == "material.usage_held" && F.Level == Severity::Warning)
                {
                    HeldWarn++;
                    if (F.Asset == "material:mtl_held_hidden" && F.Detail.find("t7.gdt") != std::string::npos) HeldNamed++;
                }
            Check(HeldWarn == 2 && HeldNamed == 1, "a held map material: a warning naming its gdt when its usage hides it or is unreadable, none when it has a usage", std::to_string(HeldWarn) + "/" + std::to_string(HeldNamed));
            // the stand-alone gate has no run behind it: the same warnings come from the material.usage_held rows of graywolf_approximations.tsv
            Inputs FromRows = Map;
            FromRows.MapHeld.clear();
            FromRows.Approximations = Base / "approx_held.tsv";
            Put(FromRows.Approximations,
                "code\tasset\tdetail\tmessage\n"
                "material.usage_held\tmtl_held_hidden\tgdt=model_export\\t7.gdt, held_usage=<not in editor>, this_export_would_choose=floor, surface=world#3\tx\n"
                "material.usage_held\tmtl_held_shown\tgdt=model_export\\t7.gdt, held_usage=floor, this_export_would_choose=floor, surface=world#4\tx\n"
                "material.usage_held\tmtl_held_unread\tgdt=(unknown), held_usage=unavailable: no file, this_export_would_choose=floor, surface=world#5\tx\n");
            const Result Rows = Run(FromRows);
            size_t RowWarn = 0, RowNamed = 0;
            for (const Finding& F : Rows.Findings)
                if (F.Check == "material.usage_held" && F.Level == Severity::Warning)
                {
                    RowWarn++;
                    if (F.Asset == "material:mtl_held_hidden" && F.Detail.find("t7.gdt") != std::string::npos && F.Detail.find("<not in editor>") != std::string::npos) RowNamed++;
                }
            Check(RowWarn == HeldWarn && RowNamed == HeldNamed, "the stand-alone gate reads usage_held rows from the approximations file and agrees with the export's gate", std::to_string(RowWarn) + "/" + std::to_string(RowNamed));
        }
        Check(R.ExitCode() == 2 && R.Failures > 0, "failures give exit 2");
        const std::string Tsv = R.Tsv();
        Check(Tsv.rfind("# static gate", 0) == 0 && Tsv.find("\ncheck\tasset\tseverity\tdetail\n") != std::string::npos && Tsv.find("gate.entries\timage\tinfo\tentries=") != std::string::npos,
              "the TSV is labelled static and carries per-GDF counts", Tsv.substr(0, 80));
        Check(!R.NotRun.empty() && R.Summary().find("not run by design:") != std::string::npos && R.Summary().find("STATIC") != std::string::npos, "the summary is labelled static and lists what was not run");
        bool SoundNotRun = false;
        for (const auto& [Name, Why] : R.NotRun) SoundNotRun = SoundNotRun || Name.find("sound") != std::string::npos;
        Check(SoundNotRun, "sound is listed as not run, never as passed");

        // a clean layout: only the pass cases
        const fs::path Out2 = Base / "clean" / "bo3";
        Put(Out2 / "texture_assets" / "t" / "c.tif", Tif3);
        Put(Out2 / "texture_assets" / "t" / "n.tif", Tiff(8, 8, 3, { 128, 128, 255 }));
        Put(Out2 / "texture_assets" / "t.gdt", Gdt({
            Ent("i_c", "image", Img("c.tif", "diffuseMap", "sRGB3chAlpha", "compressed high color")),
            Ent("i_n", "image", Img("n.tif", "normalMap", "Normal", "compressed")),
            Ent("mtl_ok", "material", { { "colorMap", "i_c" }, { "colorTint", "1 1 1 1" }, { "materialType", "lit" }, { "normalMap", "i_n" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
        }));
        Inputs In2 = In;
        In2.Roots = { Out2 };
        const Result Clean = Run(In2);
        Check(Clean.Failures == 0 && Clean.ExitCode() != 2 && Clean.Entries == 3 && Clean.Gdfs.at("image").second == 0, "a clean layout has no failure",
              Clean.Summary() + (Clean.Findings.empty() ? "" : " first: " + Clean.Findings.front().Check + " " + Clean.Findings.front().Detail));
        Check(Clean.ImagesDecoded == 2, "the pixel rules ran on both images", std::to_string(Clean.ImagesDecoded));


        // a slot bound to an image outside the output is judged by the semantic the Mod Tools give that image (stock GDT text, or the catalog's slot defaults)
        {
            const auto SHas = [](const Result& R, const char* C1, Severity Level, const char* Asset) {
                size_t N = 0;
                for (const Finding& F : R.Findings)
                    if (F.Check == C1 && F.Level == Level && F.Asset == Asset) N++;
                return N;
            };
            const auto SDescribe = [](const Result& R) {
                std::string L;
                for (const Finding& F : R.Findings) L += F.Check + "@" + F.Asset + "; ";
                return L + R.Summary();
            };
            Tc::Data Spec;
            Spec.LoadCatalogText(R"({"techsets":[
                {"n":"lit_full","c":"Geometry","f":"none","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["specColorMap","specColorMap","specularMap","$specular"]]},
                {"n":"lit_mask","c":"Geometry","f":"none","b":"Opaque","t":0,"s":[["colorMap","colorMap","diffuseMap","$white_diffuse"],["specColorMap","specColorMap","specularMask","$white_specular"]]}]})");
            GdtDb::Catalog Stock;
            for (const char* Name : { "i_stock_mask", "i_stock_color", "i_stock_gone", "i_stock_nosem" })
            {
                GdtDb::Row Row;
                Row.Name = Name;
                Row.Gdf = "image.gdf";
                Row.GdtFile = "stock/stock_images.gdt";
                Stock.Add(Row);
            }
            Put(Mod / "stock" / "stock_images.gdt", Gdt({
                Ent("i_stock_mask", "image", { { "semantic", "specularMask" }, { "type", "image" } }),
                Ent("i_stock_color", "image", { { "semantic", "specularMap" }, { "type", "image" } }),
                Ent("i_stock_nosem", "image", { { "type", "image" } }),
            }));
            const fs::path Root = Base / "stocksem" / "bo3";
            Put(Root / "m.gdt", Gdt({
                Ent("mtl_full_mask", "material", { { "materialType", "lit_full" }, { "specColorMap", "i_stock_mask" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
                Ent("mtl_mask_mask", "material", { { "materialType", "lit_mask" }, { "specColorMap", "i_stock_mask" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
                Ent("mtl_full_color", "material", { { "materialType", "lit_full" }, { "specColorMap", "i_stock_color" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
                Ent("mtl_full_code", "material", { { "materialType", "lit_full" }, { "specColorMap", "$white_specular" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
                Ent("mtl_full_unknown_code", "material", { { "materialType", "lit_full" }, { "specColorMap", "$mystery" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
                Ent("mtl_full_gone", "material", { { "materialType", "lit_full" }, { "specColorMap", "i_stock_gone" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
                Ent("mtl_full_nosem", "material", { { "materialType", "lit_full" }, { "specColorMap", "i_stock_nosem" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
                Ent("mtl_full_default", "material", { { "materialType", "lit_full" }, { "specColorMap", "$specular" }, { "surfaceType", "concrete" }, { "usage", "<not in editor>" } }),
            }));
            Inputs S = In;
            S.Roots = { Root };
            S.Catalog = &Spec;
            S.Held = &Stock;
            const Result Sr = Run(S);
            Check(SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_full_mask") == 1, "stock mask image in a colour specular slot fails", SDescribe(Sr));
            Check(SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_mask_mask") == 0 && SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_full_color") == 0, "stock mask in a mask slot and stock colour in a colour slot pass", SDescribe(Sr));
            Check(SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_full_code") == 1, "a code image that is a mask slot default fails in a colour specular slot", SDescribe(Sr));
            Check(SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_full_unknown_code") == 0 && SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_full_gone") == 0 &&
                  SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_full_nosem") == 0 && SHas(Sr, "material.slot_semantic", Severity::Failure, "material:mtl_full_default") == 0, "an image whose semantic cannot be read is not failed", SDescribe(Sr));
            const auto Part = std::find_if(Sr.Incomplete.begin(), Sr.Incomplete.end(), [](const auto& X) { return X.first == "material.slot_semantic (part)"; });
            Check(Part != Sr.Incomplete.end() && Part->second.find("3 filled slot") != std::string::npos && Sr.ExitCode() == 2, "unreadable images stay incomplete (3 of them), never a pass", Part == Sr.Incomplete.end() ? "(none)" : Part->second);
            Check(Sr.Checks.at("material.slot_semantic").Judged == 4, "four slots were judged by the Mod Tools data", std::to_string(Sr.Checks.at("material.slot_semantic").Judged));
            Inputs NoRoot = S;
            NoRoot.ModToolsRoot.clear();
            const Result Nr = Run(NoRoot);
            Check(SHas(Nr, "material.slot_semantic", Severity::Failure, "material:mtl_full_mask") == 0 && SHas(Nr, "material.slot_semantic", Severity::Failure, "material:mtl_full_code") == 1, "no Mod Tools root: the stock image is unread (not failed, not passed), the code image is still judged", SDescribe(Nr));
        }

        // the report writer and the layout roots
        std::string Error;
        Check(WriteReports(Clean, Base / "reports", Error) && fs::is_regular_file(Base / "reports" / "graywolf_gate.tsv") && fs::is_regular_file(Base / "reports" / "graywolf_gate_summary.txt"), "the reports are written", Error);
        Check(ResolveRoots(Base / "out").size() == 1 && ResolveRoots(Base / "out")[0] == Base / "out" / "bo3", "the layout root is found", std::to_string(ResolveRoots(Base / "out").size()));

        // the surfaceType <none> failure with the run's approximations: a reported reason is named, no reported reason is the source's own value (Info)
        {
            const auto SurfaceFindings = [&](const Inputs& I, Severity S, const char* Asset) {
                const Result Got = Run(I);
                size_t N = 0;
                for (const Finding& F : Got.Findings)
                    if (F.Check == "material.surface_type" && F.Level == S && F.Asset == Asset) N++;
                return N;
            };
            Inputs Why = In;
            Why.Approximations = Base / "approx_why.tsv";
            Put(Why.Approximations, "code\tasset\tdetail\tmessage\nmaterial.surface_unknown\tmtl_bad\tgame=bo4, why=no_table\tleft <none>\n");
            Inputs NoRow = In;
            NoRow.Approximations = Base / "approx_none.tsv";
            Put(NoRow.Approximations, "code\tasset\tdetail\tmessage\nmaterial.techset_fallback\tmtl_other\tx\ty\n");
            Inputs Positive = In;
            Positive.Approximations = Base / "approx_positive.tsv";
            Put(Positive.Approximations, "code\tasset\tdetail\tmessage\nmaterial.surface_none_in_source\tmtl_bad\tgame=bo4\tread and names none\n");
            Check(SurfaceFindings(In, Severity::Failure, "material:mtl_bad") == 1, "no approximations: a <none> surfaceType is a failure");
            Check(SurfaceFindings(Why, Severity::Failure, "material:mtl_bad") == 1 && SurfaceFindings(Why, Severity::Info, "material:mtl_bad") == 0, "a reported reason keeps the failure and names it");
            {
                // a fallback caused by an unusable rule table is a failure, any other fallback a warning
                const auto FallbackFindings = [&](const char* Detail, Severity S) {
                    Inputs I2 = In;
                    I2.Approximations = Base / "approx_fallback.tsv";
                    Put(I2.Approximations, std::string("code\tasset\tdetail\tmessage\nmaterial.techset_fallback\tmtl_bad\t") + Detail + "\ty\n");
                    size_t N = 0;
                    for (const Finding& F : Run(I2).Findings)
                        if (F.Check == "material.techset_fallback" && F.Level == S && F.Asset == "material:mtl_bad") N++;
                    return N;
                };
                Check(FallbackFindings("reason=rules_missing", Severity::Failure) == 1 && FallbackFindings("reason=no_rule", Severity::Warning) == 1 && FallbackFindings("reason=no_rule", Severity::Failure) == 0,
                      "a techset fallback caused by an unusable rule table is a gate failure; any other fallback is a counted warning");
                // the other conversion rows and the unbound textures: a gloss or reveal conversion failure fails the material, an unbound texture warns
                const auto RowFindings = [&](const char* Code, const char* CheckName, Severity S) {
                    Inputs I4 = In;
                    I4.Approximations = Base / "approx_row.tsv";
                    Put(I4.Approximations, std::string("code\tasset\tdetail\tmessage\n") + Code + "\tmtl_bad\timage=x.png\ty\n");
                    size_t N = 0;
                    for (const Finding& F : Run(I4).Findings)
                        if (F.Check == CheckName && F.Level == S && F.Asset == "material:mtl_bad") N++;
                    return N;
                };
                Check(RowFindings("material.gloss_conversion_failed", "material.gloss_conversion", Severity::Failure) == 1 &&
                          RowFindings("material.reveal_conversion_failed", "material.reveal_conversion", Severity::Failure) == 1 &&
                          RowFindings("material.texture_unbound", "material.texture_unbound", Severity::Warning) == 1 &&
                          RowFindings("material.family_surface_unknown_treated_as_metal", "material.family_assumed", Severity::Warning) == 1 &&
                          RowFindings("material.family_surface_unknown_specular_dropped_by_rule", "material.family_assumed", Severity::Warning) == 1,
                      "a gloss or reveal conversion failure is a gate failure; an unbound texture or an assumed family is a gate warning");
                Inputs I3 = In;
                I3.Approximations = Base / "approx_f0.tsv";
                Put(I3.Approximations, "code\tasset\tdetail\tmessage\nmaterial.f0_conversion_failed\tmtl_bad\timage=x_spc.png\ty\n");
                size_t F0Fails = 0;
                for (const Finding& F : Run(I3).Findings)
                    if (F.Check == "material.f0_conversion" && F.Level == Severity::Failure && F.Asset == "material:mtl_bad") F0Fails++;
                Check(F0Fails == 1, "a failed sqrt(F0) conversion is a gate failure naming the image");
            }
            Check(SurfaceFindings(NoRow, Severity::Failure, "material:mtl_bad") == 1 && SurfaceFindings(NoRow, Severity::Info, "material:mtl_bad") == 0, "approximations with no row for the material: no evidence, a failure (absence is never 'the source said none')");
            Check(SurfaceFindings(Positive, Severity::Failure, "material:mtl_bad") == 0 && SurfaceFindings(Positive, Severity::Info, "material:mtl_bad") == 1, "only a material.surface_none_in_source row makes <none> Info");
        }

        // no Mod Tools root and no catalog: those checks are listed as not run
        Inputs None = In2;
        None.ModToolsRoot.clear();
        None.Catalog = nullptr;
        const Result Blind = Run(None);
        bool AwiNotRun = false, CatalogNotRun = false;
        for (const auto& [Name, Why] : Blind.Incomplete)
        {
            AwiNotRun = AwiNotRun || Name == "awi.*";
            CatalogNotRun = CatalogNotRun || Name.find("material.type") != std::string::npos;
        }
        Check(AwiNotRun && CatalogNotRun && Blind.Checks.count("awi.unknown_key") == 0, "without the Mod Tools or the catalog those checks are listed as incomplete");

        Failed += ReviewTests(Failures);
        Failed += LinkerTests(Failures);
        fs::remove_all(Base, Ec);
        return Failed;
    }
}
