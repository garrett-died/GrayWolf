#include "stdafx.h"

// See Bo3Target.Awi.h.

#include "Bo3Target.Awi.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace Bo3Target::Awi
{
    const char* KindName(Kind K)
    {
        switch (K)
        {
        case Kind::Float: return "float";
        case Kind::Int: return "int";
        case Kind::CheckBox: return "checkbox";
        case Kind::Combo: return "combo";
        case Kind::AssetCombo: return "assetcombo";
        case Kind::Texture: return "texture";
        case Kind::String: return "string";
        case Kind::Path: return "path";
        case Kind::Text: return "text";
        case Kind::Color: return "color";
        case Kind::XModel: return "xmodel";
        case Kind::FileCombo: return "filecombo";
        case Kind::BoneCombo: return "bonecombo";
        case Kind::Other: break;
        }
        return "other";
    }

    namespace
    {
        bool IsIdentStart(char C) { return std::isalpha(static_cast<unsigned char>(C)) || C == '_'; }
        bool IsIdent(char C) { return std::isalnum(static_cast<unsigned char>(C)) || C == '_'; }

        // End (one past the closing quote) of the string literal starting at Text[At] == '"'; handles """ ... """.
        size_t StringEnd(const std::string& Text, size_t At)
        {
            if (Text.compare(At, 3, "\"\"\"") == 0)
            {
                const size_t E = Text.find("\"\"\"", At + 3);
                return E == std::string::npos ? Text.size() : E + 3;
            }
            size_t I = At + 1;
            while (I < Text.size() && Text[I] != '"')
            {
                if (Text[I] == '\\' && I + 1 < Text.size()) I++;
                I++;
            }
            return std::min(I + 1, Text.size());
        }

        // The text with // and /* */ comments cut out; strings are kept whole.
        std::string StripComments(const std::string& In)
        {
            std::string Out;
            Out.reserve(In.size());
            for (size_t I = 0; I < In.size();)
            {
                if (In[I] == '"')
                {
                    const size_t E = StringEnd(In, I);
                    Out.append(In, I, E - I);
                    I = E;
                }
                else if (In.compare(I, 2, "//") == 0)
                {
                    while (I < In.size() && In[I] != '\n') I++;
                }
                else if (In.compare(I, 2, "/*") == 0)
                {
                    const size_t E = In.find("*/", I + 2);
                    I = E == std::string::npos ? In.size() : E + 2;
                    Out += ' ';
                }
                else Out += In[I++];
            }
            return Out;
        }

        std::string Trim(std::string_view S)
        {
            size_t B = 0, E = S.size();
            while (B < E && std::isspace(static_cast<unsigned char>(S[B]))) B++;
            while (E > B && std::isspace(static_cast<unsigned char>(S[E - 1]))) E--;
            return std::string(S.substr(B, E - B));
        }

        // Splits at Sep outside strings and brackets.
        std::vector<std::string> SplitTop(const std::string& S, char Sep)
        {
            std::vector<std::string> Out;
            int Depth = 0;
            size_t From = 0;
            for (size_t I = 0; I < S.size();)
            {
                const char C = S[I];
                if (C == '"') { I = StringEnd(S, I); continue; }
                if (C == '(' || C == '[' || C == '{') Depth++;
                else if (C == ')' || C == ']' || C == '}') Depth--;
                else if (C == Sep && Depth == 0)
                {
                    Out.push_back(Trim(S.substr(From, I - From)));
                    From = I + 1;
                }
                I++;
            }
            Out.push_back(Trim(S.substr(From)));
            return Out;
        }

        // One past the ')' matching the '(' at Text[Open].
        size_t CloseParen(const std::string& Text, size_t Open)
        {
            int Depth = 0;
            for (size_t I = Open; I < Text.size();)
            {
                if (Text[I] == '"') { I = StringEnd(Text, I); continue; }
                if (Text[I] == '(') Depth++;
                else if (Text[I] == ')' && --Depth == 0) return I + 1;
                I++;
            }
            return std::string::npos;
        }

        std::string Unquote(const std::string& Lit)
        {
            if (Lit.compare(0, 3, "\"\"\"") == 0 && Lit.size() >= 6) return Lit.substr(3, Lit.size() - 6);
            if (Lit.size() >= 2) return Lit.substr(1, Lit.size() - 2);
            return std::string();
        }

        struct Variable
        {
            std::vector<std::string> Values;
            bool Known = true;
        };
        using Variables = std::map<std::string, Variable>;

        struct Value
        {
            std::string Text;
            bool Known = true;
        };

        // Evaluates a '+' chain of literals, numbers and string variables.
        // Union: a variable with several assignments contributes all of them, joined with " | " (for option lists).
        // Otherwise (Union false) the variable must have exactly one assignment, else the result is unknown.
        Value Eval(const std::string& Expr, const Variables& Vars, bool Union)
        {
            Value Out;
            for (const std::string& Term : SplitTop(Expr, '+'))
            {
                if (Term.empty()) continue;
                if (Term[0] == '"') Out.Text += Unquote(Term);
                else if (std::isdigit(static_cast<unsigned char>(Term[0])) || Term[0] == '-' || Term[0] == '.') Out.Text += Term;
                else if (IsIdentStart(Term[0]) && std::all_of(Term.begin(), Term.end(), IsIdent))
                {
                    const auto It = Vars.find(Term);
                    if (It == Vars.end() || !It->second.Known || It->second.Values.empty() || (!Union && It->second.Values.size() != 1)) Out.Known = false;
                    else
                    {
                        std::string Joined;
                        for (const std::string& V : It->second.Values) Joined += (Joined.empty() ? "" : " | ") + V;
                        Out.Text += Joined;
                    }
                }
                else Out.Known = false;
                if (!Out.Known && !Union) Out.Text += "*";
                else if (!Out.Known) Out.Text += " ";
            }
            return Out;
        }

        // Key expression: unknown parts become '*'.
        std::string KeyOf(const std::string& Expr, const Variables& Vars)
        {
            std::string Out;
            for (const std::string& Term : SplitTop(Expr, '+'))
            {
                if (Term.empty()) continue;
                const Value V = Eval(Term, Vars, false);
                Out += V.Known ? V.Text : "*";
            }
            for (size_t At; (At = Out.find("**")) != std::string::npos;) Out.erase(At, 1);
            return Out;
        }

        bool Number(const std::string& S, double& Out)
        {
            const std::string T = Trim(S);
            if (T.empty()) return false;
            char* End = nullptr;
            Out = std::strtod(T.c_str(), &End);
            // "0.25f" style suffixes
            return End != T.c_str() && (*End == '\0' || ((*End == 'f') && *std::next(End) == '\0'));
        }

        void ParseOptions(const std::string& Text, Entry& E)
        {
            E.OptionsKnown = true;
            for (std::string Raw : SplitTop(Text, '|'))
            {
                Raw = Trim(Raw);
                while (!Raw.empty() && Raw.back() == '*') Raw = Trim(std::string_view(Raw).substr(0, Raw.size() - 1));
                if (Raw.empty() || Raw.find_first_not_of('-') == std::string::npos) continue;
                const size_t Open = Raw.find('{');
                if (Open != std::string::npos && Raw.back() == '}')
                {
                    const std::string Shown = Trim(Raw.substr(0, Open)), Stored = Trim(Raw.substr(Open + 1, Raw.size() - Open - 2));
                    if (!Shown.empty()) E.Options.push_back(Shown);
                    if (!Stored.empty()) E.Options.push_back(Stored);
                }
                else E.Options.push_back(Raw);
            }
        }

        // '*' matches any run. Iterative with one backtrack point (the last '*'): O(text x pattern), no recursion on input length.
        bool Glob(std::string_view Pattern, std::string_view Text)
        {
            size_t P = 0, T = 0, Star = std::string_view::npos, Mark = 0;
            while (T < Text.size())
            {
                if (P < Pattern.size() && Pattern[P] == '*')
                {
                    Star = P++;
                    Mark = T;
                }
                else if (P < Pattern.size() && Pattern[P] == Text[T])
                {
                    P++;
                    T++;
                }
                else if (Star != std::string_view::npos)
                {
                    P = Star + 1;
                    T = ++Mark;
                }
                else return false;
            }
            while (P < Pattern.size() && Pattern[P] == '*') P++;
            return P == Pattern.size();
        }
    }

    bool Schema::Parse(std::string_view Raw, std::string& Error)
    {
        const std::string Text = StripComments(std::string(Raw));

        // the declarations a name may be given: string and numeric variables, "string x = ...;" and later "x = ...;" / "x += ...;"
        const auto CollectVariables = [&](const std::string& Body, Variables& Vars) {
            static const std::set<std::string> Types = { "string", "float", "int", "double", "uint" };
            for (size_t I = 0; I < Body.size();)
            {
                const char C = Body[I];
                if (C == '"') { I = StringEnd(Body, I); continue; }
                if (!IsIdentStart(C) || (I > 0 && (IsIdent(Body[I - 1]) || Body[I - 1] == '.'))) { I++; continue; }
                size_t E = I;
                while (E < Body.size() && IsIdent(Body[E])) E++;
                std::string Word = Body.substr(I, E - I);
                size_t P = E;
                const auto SkipSpace = [&]() { while (P < Body.size() && std::isspace(static_cast<unsigned char>(Body[P]))) P++; };
                SkipSpace();
                bool Declares = false;
                if (Word == "const" && P < Body.size() && IsIdentStart(Body[P]))
                {
                    size_t N = P;
                    while (N < Body.size() && IsIdent(Body[N])) N++;
                    Word = Body.substr(P, N - P);
                    P = N;
                    SkipSpace();
                }
                if (Types.count(Word) && P < Body.size() && IsIdentStart(Body[P]))
                {
                    Declares = true;
                    size_t N = P;
                    while (N < Body.size() && IsIdent(Body[N])) N++;
                    Word = Body.substr(P, N - P);
                    P = N;
                    SkipSpace();
                }
                if (Declares) Vars.try_emplace(Word);
                const bool Plus = P + 1 < Body.size() && Body[P] == '+' && Body[P + 1] == '=';
                const bool Assign = P < Body.size() && Body[P] == '=' && (P + 1 >= Body.size() || Body[P + 1] != '=');
                if ((Plus || Assign) && Vars.count(Word))
                {
                    size_t X = P + (Plus ? 2 : 1);
                    const size_t Start = X;
                    while (X < Body.size() && Body[X] != ';')
                    {
                        if (Body[X] == '"') X = StringEnd(Body, X);
                        else X++;
                    }
                    const Value V = Eval(Trim(Body.substr(Start, X - Start)), Vars, true);
                    Variable& Var = Vars[Word];
                    if (!V.Known) Var.Known = false;
                    if (Plus && !Var.Values.empty()) Var.Values.back() += V.Text;
                    else Var.Values.push_back(V.Text);
                    I = X;
                    continue;
                }
                I = std::max(E, I + 1);
            }
        };

        size_t Found = 0;
        const auto Merge = [&](Entry In) {
            Found++;
            In.Declarations = 1;
            if (In.Key.empty() || In.Key.find_first_not_of('*') == std::string::npos) return;
            In.Pattern = In.Key.find('*') != std::string::npos;
            Entry* At = nullptr;
            if (In.Pattern)
            {
                for (Entry& P : Patterns)
                    if (P.Key == In.Key) At = &P;
                if (At == nullptr) { Patterns.push_back(In); return; }
            }
            else
            {
                const auto [It, Added] = Exact.try_emplace(In.Key, In);
                if (Added) return;
                At = &It->second;
            }
            At->Declarations++;
            if (At->K != In.K) At->K = At->K == Kind::Other ? In.K : At->K;
            At->HasMin = At->HasMin && In.HasMin;
            At->HasMax = At->HasMax && In.HasMax;
            At->RangeUnknown = At->RangeUnknown || In.RangeUnknown;
            if (At->HasMin) At->Min = std::min(At->Min, In.Min);
            if (At->HasMax) At->Max = std::max(At->Max, In.Max);
            if (In.OptionsKnown)
            {
                At->OptionsKnown = true;
                for (const std::string& O : In.Options)
                    if (std::find(At->Options.begin(), At->Options.end(), O) == At->Options.end()) At->Options.push_back(O);
            }
            else if (At->K == Kind::Combo) At->OptionsKnown = false;
            if (!At->HasDefault && In.HasDefault) { At->HasDefault = true; At->Default = In.Default; }
            if (At->Target.empty()) At->Target = In.Target;
            if (At->RelativePath.empty()) At->RelativePath = In.RelativePath;
        };

        // the AddEntry calls of one scope
        const auto CollectEntries = [&](const std::string& Body, const Variables& Vars) {
            for (size_t At = Body.find("AddEntry_"); At != std::string::npos; At = Body.find("AddEntry_", At + 9))
            {
                if (At > 0 && IsIdent(Body[At - 1])) continue;
                size_t E = At + 9;
                while (E < Body.size() && IsIdent(Body[E])) E++;
                const std::string Name = Body.substr(At + 9, E - At - 9);
                size_t Open = E;
                while (Open < Body.size() && std::isspace(static_cast<unsigned char>(Body[Open]))) Open++;
                if (Open >= Body.size() || Body[Open] != '(') continue;
                const size_t Close = CloseParen(Body, Open);
                if (Close == std::string::npos) continue;
                const std::vector<std::string> Args = SplitTop(Body.substr(Open + 1, Close - Open - 2), ',');
                if (Args.empty() || Args[0].empty()) continue;

                // the chained calls up to ';' : SetRelativePath("...")
                std::string Relative;
                {
                    size_t Semi = Close;
                    while (Semi < Body.size() && Body[Semi] != ';')
                    {
                        if (Body[Semi] == '"') Semi = StringEnd(Body, Semi);
                        else Semi++;
                    }
                    const std::string Chain = Body.substr(Close, Semi - Close);
                    const size_t R = Chain.find("SetRelativePath");
                    if (R != std::string::npos)
                    {
                        const size_t Q = Chain.find('"', R);
                        if (Q != std::string::npos) Relative = Unquote(Chain.substr(Q, StringEnd(Chain, Q) - Q));
                    }
                }

                const auto Arg = [&](size_t N) { return N < Args.size() ? Args[N] : std::string(); };
                // a number: a literal, or a variable holding one. Present but unresolved sets Unknown.
                const auto Num = [&](size_t N, bool& Has, double& Out, bool& Unknown) {
                    Has = false;
                    const std::string A = Arg(N);
                    if (A.empty()) return;
                    if (Number(A, Out)) { Has = true; return; }
                    const Value V = Eval(A, Vars, false);
                    if (V.Known && Number(V.Text, Out)) { Has = true; return; }
                    Unknown = true;
                };
                Entry Base;
                Base.RelativePath = Relative;
                if (Name == "Float" || Name == "Int")
                {
                    Entry X = Base;
                    X.Key = KeyOf(Arg(0), Vars);
                    X.K = Name == "Float" ? Kind::Float : Kind::Int;
                    Num(2, X.HasMin, X.Min, X.RangeUnknown);
                    Num(3, X.HasMax, X.Max, X.RangeUnknown);
                    bool Unused = false;
                    Num(1, X.HasDefault, X.Default, Unused);
                    Merge(X);
                }
                else if (Name == "Vector2" || Name == "Vector3" || Name == "Vector4")
                {
                    const size_t N = static_cast<size_t>(Name[6] - '0');
                    for (size_t K = 0; K < N; K++)
                    {
                        Entry X = Base;
                        X.Key = KeyOf(Arg(K), Vars);
                        X.K = Kind::Float;
                        Num(2 * N, X.HasMin, X.Min, X.RangeUnknown);
                        Num(2 * N + 1, X.HasMax, X.Max, X.RangeUnknown);
                        bool Unused = false;
                        Num(N + K, X.HasDefault, X.Default, Unused);
                        Merge(X);
                    }
                }
                else if (Name == "Combo")
                {
                    Entry X = Base;
                    X.Key = KeyOf(Arg(0), Vars);
                    X.K = Kind::Combo;
                    const Value O = Eval(Arg(1), Vars, true);
                    if (O.Known) ParseOptions(O.Text, X);
                    if (X.Options.empty()) X.OptionsKnown = false;     // a combo filled in at run time ("") says nothing about its values
                    Merge(X);
                }
                else if (Name == "AssetCombo")
                {
                    Entry X = Base;
                    X.Key = KeyOf(Arg(0), Vars);
                    X.K = Kind::AssetCombo;
                    const Value T = Eval(Arg(1), Vars, false);
                    if (T.Known) X.Target = T.Text;
                    Merge(X);
                }
                else
                {
                    static const std::map<std::string, Kind> Simple = { { "CheckBox", Kind::CheckBox }, { "Texture", Kind::Texture }, { "String", Kind::String },
                                                                        { "Path", Kind::Path }, { "Text", Kind::Text }, { "Color", Kind::Color }, { "Label", Kind::Other },
                                                                        { "XModel", Kind::XModel }, { "FileCombo", Kind::FileCombo }, { "BoneCombo", Kind::BoneCombo } };
                    const auto It = Simple.find(Name);
                    if (It == Simple.end()) continue;     // ButtonGroup, LensFlare: not GDT fields
                    Entry X = Base;
                    X.Key = KeyOf(Arg(0), Vars);
                    X.K = It->second;
                    Merge(X);
                }
            }
        };

        // split into function bodies (a top-level brace block each) and the statements between them
        std::string Top;
        std::vector<std::string> Bodies;
        {
            int Depth = 0;
            size_t BodyStart = 0;
            for (size_t I = 0; I < Text.size();)
            {
                const char C = Text[I];
                if (C == '"')
                {
                    const size_t E = StringEnd(Text, I);
                    if (Depth == 0) Top.append(Text, I, E - I);
                    I = E;
                    continue;
                }
                if (C == '{')
                {
                    if (Depth++ == 0) BodyStart = I + 1;
                }
                else if (C == '}' && Depth > 0)
                {
                    if (--Depth == 0) Bodies.push_back(Text.substr(BodyStart, I - BodyStart));
                }
                else if (Depth == 0) Top += C;
                I++;
            }
            if (Depth > 0) Bodies.push_back(Text.substr(BodyStart));     // an unclosed body is still read
        }
        Variables Globals;
        CollectVariables(Top, Globals);
        CollectEntries(Top, Globals);
        for (const std::string& Body : Bodies)
        {
            Variables Vars = Globals;       // a function sees the shared scope and its own names only
            CollectVariables(Body, Vars);
            CollectEntries(Body, Vars);
        }
        if (Found == 0)
        {
            Error = "no AddEntry_ call found";
            return false;
        }
        return true;
    }

    const Entry* Schema::Find(const std::string& Key) const
    {
        if (const auto It = Exact.find(Key); It != Exact.end()) return &It->second;
        for (const Entry& P : Patterns)
            if (Glob(P.Key, Key)) return &P;
        return nullptr;
    }

    bool Schema::HasOption(const Entry& E, std::string_view Value)
    {
        if (!E.OptionsKnown) return false;
        while (!Value.empty() && Value.back() == '*') Value.remove_suffix(1);
        std::string V(Value);
        while (!V.empty() && V.back() == ' ') V.pop_back();
        return std::find(E.Options.begin(), E.Options.end(), V) != E.Options.end();
    }

    bool Load(const fs::path& Root, const std::string& Gdf, Schema& Out, std::string& Error)
    {
        std::ifstream In(Root / "deffiles" / (Gdf + ".awi"), std::ios::binary);
        if (!In)
        {
            Error = "no deffiles\\" + Gdf + ".awi";
            return false;
        }
        std::ostringstream Os;
        Os << In.rdbuf();
        return Out.Parse(Os.str(), Error);
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got = std::string()) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL Awi {} (got '{}')\n", Name, Got);
        };
        const char* Sample = R"AWI(
void GenerateUI( asset Asset )
{
    string modes = "A | B* | C{c} | ---- | ";
    string more = """
        D |
        E
    """;
    string pick = modes;
    if (x) pick = more;
    /* Asset.AddEntry_Float( "commented", 0, 0, 1 ); */
    // Asset.AddEntry_Float( "commentedToo", 0, 0, 1 );
    Asset.AddEntry_Float( "gloss", 1.5, -2.0, 10 ).SetTitle( "a;b" );
    Asset.AddEntry_Int( "count", 1, 1, 64 );
    Asset.AddEntry_CheckBox( "flag", false );
    Asset.AddEntry_Combo( "mode", pick ).Show( false );
    Asset.AddEntry_Combo( "dynamic", Asset.GetTypes() );
    Asset.AddEntry_AssetCombo( "colorMap", "image" );
    Asset.AddEntry_XModel( "filename", "" ).SetRelativePath( "model_export/" );
    Asset.AddEntry_Vector2( "uScroll", "vScroll", 0.0, 0.0, -10.0, 10.0 );
    Asset.AddEntry_Vector4( "cg" + Digit + "_x", "cg" + Digit + "_y", "cg" + Digit + "_z", "cg" + Digit + "_w", 0.0, 0.0, 0.0, 0.0, -1000, 1000 );
    Asset.AddEntry_Float( "gloss", 0, -5, 3 );
    Asset.AddEntry_Label( "help", "text" );
}
)AWI";
        Schema S;
        std::string Error;
        Check(S.Parse(Sample, Error), "parses", Error);
        const Entry* G = S.Find("gloss");
        Check(G != nullptr && G->K == Kind::Float && G->HasMin && G->Min == -5 && G->HasMax && G->Max == 10 && G->Declarations == 2,
              "a key declared twice keeps the widest range", G ? std::format("{} {}", G->Min, G->Max) : "null");
        Check(S.Find("commented") == nullptr && S.Find("commentedToo") == nullptr, "commented-out calls are not fields");
        Check(S.Find("help") != nullptr && S.Find("help")->K == Kind::Other, "a label is a key (stock GDTs hold matError) with no value rule");
        const Entry* C = S.Find("count");
        Check(C != nullptr && C->K == Kind::Int && C->Min == 1 && C->Max == 64, "int range");
        const Entry* M = S.Find("mode");
        Check(M != nullptr && M->OptionsKnown && Schema::HasOption(*M, "A") && Schema::HasOption(*M, "B") && Schema::HasOption(*M, "B*") && Schema::HasOption(*M, "c")
                  && Schema::HasOption(*M, "C") && Schema::HasOption(*M, "D") && Schema::HasOption(*M, "E") && !Schema::HasOption(*M, "----") && !Schema::HasOption(*M, "F"),
              "combo options: union of the variable's assignments, * markers cut, {value} kept", M ? std::to_string(M->Options.size()) : "null");
        const Entry* D = S.Find("dynamic");
        Check(D != nullptr && !D->OptionsKnown && !Schema::HasOption(*D, "x"), "a combo built by a function call has no known options");
        const Entry* A = S.Find("colorMap");
        Check(A != nullptr && A->K == Kind::AssetCombo && A->Target == "image", "asset combo target");
        const Entry* X = S.Find("filename");
        Check(X != nullptr && X->K == Kind::XModel && X->RelativePath == "model_export/", "relative path");
        Check(G != nullptr && G->HasDefault && G->Default == 1.5, "default of a float");
        const Entry* U = S.Find("vScroll");
        Check(U != nullptr && U->K == Kind::Float && U->Min == -10 && U->Max == 10, "vector components are floats with the shared range");
        const Entry* Cg = S.Find("cg07_z");
        Check(Cg != nullptr && Cg->Pattern && Cg->Max == 1000 && S.Find("cg_q") == nullptr, "a loop-built key is a pattern", Cg ? Cg->Key : "null");
        Check(S.Find("nothing") == nullptr, "an undeclared key is null");
        Schema Empty;
        Check(!Empty.Parse("void f() {}", Error), "text without AddEntry is not an awi");

        // variables are scoped per function; a name reused in another function does not union its options
        const char* Scoped = R"AWI(
string shared = "s1 | s2";
void A( asset Asset ) { string m = "x | y"; Asset.AddEntry_Combo( "ca", m ); Asset.AddEntry_Combo( "cs", shared ); }
void B( asset Asset )
{
    string m = "p | q";
    Asset.AddEntry_Combo( "cb", m );
    float LO = -2;
    const float HI = 5;
    Asset.AddEntry_Float( "r", 0, LO, HI );
    Asset.AddEntry_Float( "s", 0, Whatever(), 9 );
    for (int i = 0; i < 2; i++) Asset.AddEntry_Float( "pre" + DATA_KEYS[i], 0, 0, 1 );
    Asset.AddEntry_Float( "pq" + prefixOf + DATA_KEYS[i] + "_z", 0, 0, 1 );
}
)AWI";
        Schema Sc;
        Check(Sc.Parse(Scoped, Error), "scoped sample parses", Error);
        const Entry* Ca = Sc.Find("ca");
        const Entry* Cb = Sc.Find("cb");
        Check(Ca != nullptr && Ca->OptionsKnown && Schema::HasOption(*Ca, "x") && Schema::HasOption(*Ca, "y") && !Schema::HasOption(*Ca, "p") && !Schema::HasOption(*Ca, "q"),
              "function A's m is not unioned with function B's m", Ca ? std::to_string(Ca->Options.size()) : "null");
        Check(Cb != nullptr && Schema::HasOption(*Cb, "p") && !Schema::HasOption(*Cb, "x"), "function B sees only its own m");
        const Entry* Cs = Sc.Find("cs");
        Check(Cs != nullptr && Schema::HasOption(*Cs, "s1"), "the top-level scope is shared");
        const Entry* Rr = Sc.Find("r");
        Check(Rr != nullptr && Rr->HasMin && Rr->Min == -2 && Rr->HasMax && Rr->Max == 5 && !Rr->RangeUnknown, "range limits given as variables are resolved", Rr ? std::format("{} {} unknown={}", Rr->Min, Rr->Max, Rr->RangeUnknown) : "null");
        const Entry* Ss = Sc.Find("s");
        Check(Ss != nullptr && Ss->RangeUnknown && !Ss->HasMin && Ss->HasMax && Ss->Max == 9, "a limit that is an expression is recorded as not known, not as no range", Ss ? std::format("unknown={} hasMin={}", Ss->RangeUnknown, Ss->HasMin) : "null");
        Check(Sc.Find("preu") != nullptr && Sc.Find("pre") != nullptr, "prefix + DATA_KEYS[i] makes the pattern pre* (one star)");
        const Entry* Pq = Sc.Find("pqabc_z");
        Check(Pq != nullptr && Pq->Key == "pq*_z", "adjacent unknown terms give one star, not **", Pq ? Pq->Key : "null");

        // a pathological wildcard is bounded in time (the recursive match was exponential here)
        {
            std::string Key = "\"a\"";
            for (int I = 0; I < 40; I++) Key += " + Q + \"a\"";
            std::string Src = "void f( asset Asset ) { Asset.AddEntry_Float( " + Key + ", 0, 0, 1 ); }";
            Schema Path;
            Check(Path.Parse(Src, Error), "pathological pattern parses", Error);
            const auto Start = std::chrono::steady_clock::now();
            const Entry* Hit = Path.Find(std::string(200, 'a') + "c");
            const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
            Check(Hit == nullptr && Seconds < 1.0, "a pathological pattern against 200 a's and a c is bounded (< 1 s)", std::format("{:.3f}s hit={}", Seconds, Hit != nullptr));
            Check(Path.Find(std::string(41, 'a')) != nullptr, "the same pattern still matches what it should");
        }
        return Failed;
    }
}
