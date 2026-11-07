#include "stdafx.h"

#include "MapCreateFx.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

namespace Maps
{
    namespace
    {
        struct Token
        {
            std::string Text;
            int Prev = -1, Next = -1;   // the bytes around the printable run (-1: the start or end of the stack)
        };

        // the printable runs of three characters or more, with the bytes either side
        std::vector<Token> Tokens(std::span<const uint8_t> Stack)
        {
            std::vector<Token> Out;
            size_t Start = 0;
            bool In = false;
            const auto Flush = [&](size_t End) {
                if (!In) return;
                In = false;
                if (End - Start < 3) return;
                Token T;
                for (const uint8_t C : Stack.subspan(Start, End - Start)) T.Text.push_back(static_cast<char>(C));
                T.Prev = Start == 0 ? -1 : Stack[Start - 1];
                T.Next = End >= Stack.size() ? -1 : Stack[End];
                Out.push_back(std::move(T));
            };
            for (size_t I = 0; I < Stack.size(); I++)
            {
                const bool Printable = Stack[I] >= 0x20 && Stack[I] <= 0x7e;
                if (Printable && !In) In = true, Start = I;
                else if (!Printable) Flush(I);
            }
            Flush(Stack.size());
            return Out;
        }

        // a name a script can hold: letters, digits and a few separators. Anything else (a quote, a newline, a glued binary byte) is not a name.
        bool SafeName(const std::string& S)
        {
            if (S.empty() || S.size() > 128) return false;
            return std::all_of(S.begin(), S.end(), [](char C) { return std::isalnum(static_cast<unsigned char>(C)) || C == '_' || C == '.' || C == '/' || C == '-'; });
        }

        // a name token: NUL either side, so nothing printable is glued onto it
        bool CleanName(const Token& T) { return T.Prev == 0 && T.Next == 0 && SafeName(T.Text); }

        float F32(std::span<const uint8_t> B, size_t At)
        {
            float V;
            std::memcpy(&V, B.data() + At, 4);
            return V;
        }

        size_t CountOf(std::span<const uint8_t> Hay, std::span<const uint8_t> Needle)
        {
            size_t N = 0;
            if (Needle.empty()) return 0;
            for (size_t I = 0; I + Needle.size() <= Hay.size();)
            {
                if (std::equal(Needle.begin(), Needle.end(), Hay.begin() + static_cast<std::ptrdiff_t>(I)))
                {
                    N++;
                    I += Needle.size();
                }
                else
                    I++;
            }
            return N;
        }
    }

    CreateFxResult ParseCreateFx(const CreateFxLayout& L, std::span<const uint8_t> Stack, std::span<const uint8_t> Bytecode, const CreateFxBounds& Bounds)
    {
        CreateFxResult R;
        struct Entry
        {
            std::string Kind, Name;
            int FieldSets = 0;
        };
        std::vector<Entry> Entries;
        const std::vector<Token> T = Tokens(Stack);
        for (size_t I = 0; I < T.size(); I++)
        {
            if (T[I].Prev != L.AnchorPrefix || T[I].Next != 0) continue;
            const auto Next = [&](size_t Off) -> const Token* { return I + Off < T.size() ? &T[I + Off] : nullptr; };
            const Token* After = Next(1);
            if (T[I].Text == L.DelayAnchor)
            {
                const Token* Name = Next(2);
                const Token* Tail = Next(3);
                if (After != nullptr && After->Text == L.DelayNext && Name != nullptr && CleanName(*Name) && Tail != nullptr && Tail->Text == L.DelayAfter && Tail->Prev == 0)
                {
                    Entries.push_back({ "delaysound", Name->Text, L.FieldSetsDelay });
                    continue;
                }
            }
            else if (T[I].Text.size() > 1 && T[I].Text[0] == '~')
            {
                const std::string Name = T[I].Text.substr(1);
                if (I >= 1 && T[I - 1].Text == Name && CleanName(T[I - 1]) && After != nullptr && After->Text == L.FxNext)
                {
                    Entries.push_back({ "fx", Name, L.FieldSetsFx });
                    continue;
                }
                if (After != nullptr && After->Text == L.LoopNext && SafeName(Name))
                {
                    Entries.push_back({ "loopsound", Name, L.FieldSetsLoop });
                    continue;
                }
            }
            R.Unparsed++;
        }
        // the vector pairs: the prefix before, the call marker after
        struct Pair
        {
            size_t At;
            float Angles[3], Origin[3];
        };
        std::vector<Pair> Pairs;
        for (size_t I = 1; I + 28 <= Bytecode.size(); I++)
        {
            if (Bytecode[I] != L.VectorOp || Bytecode[I + 13] != L.VectorOp || Bytecode[I - 1] != L.PairPrefix) continue;
            if (Bytecode[I + 26] != L.Call[0] || Bytecode[I + 27] != L.Call[1]) continue;
            if (!Pairs.empty() && I - Pairs.back().At < 26) continue;
            Pair P{};
            P.At = I;
            bool Ok = true;
            for (size_t A = 0; A < 3; A++)
            {
                P.Angles[A] = F32(Bytecode, I + 1 + A * 4);
                P.Origin[A] = F32(Bytecode, I + 14 + A * 4);
                Ok = Ok && std::isfinite(P.Angles[A]) && std::isfinite(P.Origin[A]);
            }
            if (Ok) Pairs.push_back(P);
        }
        R.Entries = Entries.size(), R.VectorPairs = Pairs.size();
        if (R.Unparsed != 0 || Entries.size() != Pairs.size())
        {
            R.Mismatch = R.Unparsed != 0 || !Entries.empty() || !Pairs.empty();
            return R;
        }
        // every group must fit its anchor's kind: one call marker and the kind's number of field-set instructions
        for (size_t K = 0; K < Pairs.size(); K++)
        {
            const size_t From = Pairs[K].At + 26, To = K + 1 < Pairs.size() ? Pairs[K + 1].At - 1 : Bytecode.size();
            const std::span<const uint8_t> Group = Bytecode.subspan(From, To - From);
            if (CountOf(Group, L.Call) != 1 || CountOf(Group, L.FieldSet) != static_cast<size_t>(Entries[K].FieldSets)) R.Unties++;
        }
        if (R.Unties != 0)
        {
            R.Mismatch = true;
            return R;
        }
        for (size_t K = 0; K < Entries.size(); K++)
        {
            EffectPlacement E;
            E.Kind = Entries[K].Kind, E.Name = Entries[K].Name;
            bool Angles = true, Inside = true;
            for (size_t A = 0; A < 3; A++)
            {
                E.Origin[A] = Pairs[K].Origin[A], E.Angles[A] = Pairs[K].Angles[A];
                Angles = Angles && std::fabs(E.Angles[A]) <= L.AngleLimit;
                Inside = Inside && (!Bounds.Valid || (E.Origin[A] >= Bounds.Lo[A] && E.Origin[A] <= Bounds.Hi[A]));
            }
            if (!Angles) R.BadAngles++;
            else if (!Inside) R.Outside++;
            else R.Placements.push_back(std::move(E));
        }
        return R;
    }

    bool FindScriptFile(const CreateFxLayout& L, std::span<const uint8_t> Zone, const std::string& Name, std::vector<uint8_t>& Stack, std::vector<uint8_t>& Bytecode, std::string& Why)
    {
        const std::string Needle = Name + std::string(1, '\0');
        Why = "the zone holds no scriptfile of that name";
        for (auto It = std::search(Zone.begin(), Zone.end(), Needle.begin(), Needle.end()); It != Zone.end();
             It = std::search(It + 1, Zone.end(), Needle.begin(), Needle.end()))
        {
            const size_t At = static_cast<size_t>(It - Zone.begin());
            if (At < L.HeaderBack[0] || At < L.HeaderBack[1] || At < L.HeaderBack[2])
            {
                Why = "the name is too near the start of the zone for a header";
                continue;
            }
            uint32_t Compressed = 0, Size = 0, Code = 0;
            std::memcpy(&Compressed, Zone.data() + (At - L.HeaderBack[0]), 4);
            std::memcpy(&Size, Zone.data() + (At - L.HeaderBack[1]), 4);
            std::memcpy(&Code, Zone.data() + (At - L.HeaderBack[2]), 4);
            const uint64_t Body = At + Needle.size();
            if (Compressed == 0 || Size == 0 || Size > (64u << 20) || Body + Compressed + Code > Zone.size())
            {
                Why = std::format("the header's sizes ({}, {}, {}) do not fit the zone", Compressed, Size, Code);
                continue;
            }
            std::vector<uint8_t> Inflated(Size, 0);
            mz_ulong Out = Size;
            if (mz_uncompress(Inflated.data(), &Out, Zone.data() + Body, Compressed) != MZ_OK || Out != Size)
            {
                Why = "the string stack did not inflate to its declared size";
                continue;
            }
            Stack = std::move(Inflated);
            Bytecode.assign(Zone.begin() + static_cast<std::ptrdiff_t>(Body + Compressed), Zone.begin() + static_cast<std::ptrdiff_t>(Body + Compressed + Code));
            return true;
        }
        return false;
    }

    int CreateFxSelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("createfx: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        // a layout of the same shape as the game's, spelled out here so the test does not lean on Games/H1
        CreateFxLayout L;
        L.ScriptPrefix = "maps/createfx/", L.ScriptSuffix = "_fx";
        L.HeaderBack = { 32, 28, 24 };
        L.AnchorPrefix = 0x8f;
        L.LoopNext = "soundalias";
        L.DelayAnchor = "~delay_min", L.DelayNext = "delay_max", L.DelayAfter = "soundalias";
        L.FxNext = "fxid";
        L.VectorOp = 0x56, L.PairPrefix = 0x89, L.Call = { 0x6f, 0x9e };
        L.FieldSet = { 0x88, 0x00, 0x52, 0x42, 0x9c, 0x97, 0x5b };
        L.FieldSetsLoop = 1, L.FieldSetsDelay = 3, L.FieldSetsFx = 2;
        L.AngleLimit = 720.0f;

        const auto Put = [](std::vector<uint8_t>& B, const std::string& S) {
            B.insert(B.end(), S.begin(), S.end());
            B.push_back(0);
        };
        const auto Anchor = [&](std::vector<uint8_t>& B, const std::string& S) {
            B.push_back(0x8f);
            Put(B, S);
        };
        // the strings, as the stack has them: binary bytes (a hash) before an anchor and between
        std::vector<uint8_t> Stack = { 0xf2, 0xa4, ')', '$', 0xf2 };
        Anchor(Stack, "~fire_med_02");
        Put(Stack, "soundalias");
        Anchor(Stack, "~delay_min");
        Put(Stack, "delay_max");
        Put(Stack, "metal_creaks");
        Put(Stack, "soundalias");
        Put(Stack, "smoldering");
        Anchor(Stack, "~smoldering");
        Put(Stack, "fxid");
        Put(Stack, "delay");
        const auto Vec = [](std::vector<uint8_t>& B, float X, float Y, float Z) {
            B.push_back(0x56);
            for (const float V : { X, Y, Z })
            {
                std::array<uint8_t, 4> Raw{};
                std::memcpy(Raw.data(), &V, 4);
                B.insert(B.end(), Raw.begin(), Raw.end());
            }
        };
        // one instruction group: prefix, angles, origin, call, a few filler bytes, N field-set instructions
        const auto Group = [&](std::vector<uint8_t>& B, std::array<float, 3> Ang, std::array<float, 3> Org, int FieldSets, bool Call = true) {
            B.push_back(0x89);
            Vec(B, Ang[0], Ang[1], Ang[2]);
            Vec(B, Org[0], Org[1], Org[2]);
            if (Call) B.insert(B.end(), { 0x6f, 0x9e, 0x01, 0x02, 0x03, 0x6a });
            for (int K = 0; K < FieldSets; K++) B.insert(B.end(), { 0x53, 0x11, 0x22, 0x00, 0x00, 0x88, 0x00, 0x52, 0x42, 0x9c, 0x97, 0x5b });
        };
        std::vector<uint8_t> Code = { 0x34, 0x32, 0x2d, 0x64 };
        Group(Code, { 270, 0, 0 }, { 787.09f, 605.22f, 159.97f }, 1);
        Group(Code, { 0, 90, 0 }, { -10, 20, 30 }, 3);
        Group(Code, { 270, 0, 0 }, { 786.87f, 611.93f, 151.89f }, 2);
        const CreateFxResult R = ParseCreateFx(L, Stack, Code);
        Check(R.Entries == 3 && R.VectorPairs == 3 && R.Placements.size() == 3 && !R.Mismatch, "three anchors tie to three groups", std::format("{} {} {} unties {}", R.Entries, R.VectorPairs, R.Placements.size(), R.Unties));
        if (R.Placements.size() == 3)
        {
            Check(R.Placements[0].Kind == "loopsound" && R.Placements[0].Name == "fire_med_02" && std::fabs(R.Placements[0].Origin[0] - 787.09f) < 1e-3f && R.Placements[0].Angles[0] == 270.0f, "a loop sound with its origin and angles");
            Check(R.Placements[1].Kind == "delaysound" && R.Placements[1].Name == "metal_creaks" && R.Placements[1].Origin[2] == 30.0f && R.Placements[1].Angles[1] == 90.0f, "a delayed sound is the name after delay_max, before its soundalias");
            Check(R.Placements[2].Kind == "fx" && R.Placements[2].Name == "smoldering" && std::fabs(R.Placements[2].Origin[1] - 611.93f) < 1e-3f, "an effect is its name before ~name and fxid");
        }
        {
            // a stray vector pair with its own call marker, and the last group's origin dropped: three anchors, three pairs, but the kinds no longer fit
            std::vector<uint8_t> Bad = { 0x34 };
            Group(Bad, { 270, 0, 0 }, { 1, 1, 1 }, 2);   // a stray effect-shaped group first
            Group(Bad, { 270, 0, 0 }, { 787.09f, 605.22f, 159.97f }, 1);
            Group(Bad, { 0, 90, 0 }, { -10, 20, 30 }, 3);
            const CreateFxResult B = ParseCreateFx(L, Stack, Bad);
            Check(B.Mismatch && B.Placements.empty() && B.Entries == 3 && B.VectorPairs == 3 && B.Unties > 0, "equal counts with a stray group and a dropped one are rejected by the kinds", std::format("pairs {} unties {}", B.VectorPairs, B.Unties));
        }
        {
            // an extra adjacent pair (no call marker after it): it is no group, so the counts are the same and the file still ties
            std::vector<uint8_t> Extra = { 0x34 };
            Group(Extra, { 270, 0, 0 }, { 787.09f, 605.22f, 159.97f }, 0);
            Extra.clear();
            Extra.push_back(0x34);
            Group(Extra, { 1, 2, 3 }, { 4, 5, 6 }, 0, false);   // an adjacent pair that is no call
            Group(Extra, { 270, 0, 0 }, { 787.09f, 605.22f, 159.97f }, 1);
            Group(Extra, { 0, 90, 0 }, { -10, 20, 30 }, 3);
            Group(Extra, { 270, 0, 0 }, { 786.87f, 611.93f, 151.89f }, 2);
            const CreateFxResult E = ParseCreateFx(L, Stack, Extra);
            Check(E.Placements.size() == 3 && E.Placements[0].Origin[0] > 700 && !E.Mismatch, "a pair with no call after it is not counted and does not move the others", std::format("{} unties {}", E.Placements.size(), E.Unties));
        }
        {
            // a dropped origin: a group with the angles only has no pair; the counts differ and nothing is written
            std::vector<uint8_t> Drop = { 0x34, 0x89 };
            Vec(Drop, 270, 0, 0);
            Drop.insert(Drop.end(), { 0x6f, 0x9e, 0x01, 0x53, 0x11, 0x22, 0x00, 0x00, 0x88, 0x00, 0x52, 0x42, 0x9c, 0x97, 0x5b });
            Group(Drop, { 0, 90, 0 }, { -10, 20, 30 }, 3);
            Group(Drop, { 270, 0, 0 }, { 786.87f, 611.93f, 151.89f }, 2);
            const CreateFxResult D = ParseCreateFx(L, Stack, Drop);
            Check(D.Mismatch && D.Placements.empty() && D.Entries == 3 && D.VectorPairs == 2, "a dropped origin writes nothing", std::format("{} {}", D.Entries, D.VectorPairs));
        }
        {
            // sanity: angles beyond the limit and an origin outside the bounds are rejected alone
            std::vector<uint8_t> Wild = { 0x34 };
            Group(Wild, { 270, 0, 0 }, { 787.09f, 605.22f, 159.97f }, 1);
            Group(Wild, { 5000, 90, 0 }, { -10, 20, 30 }, 3);
            Group(Wild, { 270, 0, 0 }, { 786.87f, 611.93f, 151.89f }, 2);
            CreateFxBounds B;
            B.Valid = true;
            B.Lo[0] = 0, B.Lo[1] = 0, B.Lo[2] = 0, B.Hi[0] = 800, B.Hi[1] = 700, B.Hi[2] = 200;
            const CreateFxResult W = ParseCreateFx(L, Stack, Wild, B);
            Check(W.Placements.size() == 2 && W.BadAngles == 1 && W.Outside == 0 && W.Placements[0].Name == "fire_med_02" && W.Placements[1].Name == "smoldering", "angles beyond 720 are rejected alone", std::format("{} {}", W.Placements.size(), W.BadAngles));
            std::vector<uint8_t> Far = { 0x34 };
            Group(Far, { 270, 0, 0 }, { 787.09f, 605.22f, 159.97f }, 1);
            Group(Far, { 0, 90, 0 }, { -10, 20, 30 }, 3);
            Group(Far, { 270, 0, 0 }, { 786.87f, 611.93f, 151.89f }, 2);
            const CreateFxResult F = ParseCreateFx(L, Stack, Far, B);
            Check(F.Placements.size() == 2 && F.Outside == 1, "an origin outside the bounds is rejected alone", std::format("{} {}", F.Placements.size(), F.Outside));
        }
        {
            // printable bytes before a name: nothing glues onto it; the anchor needs its marker byte and a NUL after it
            std::vector<uint8_t> Glued = { 'x', 'y' };
            Anchor(Glued, "~fire_med_02");
            Put(Glued, "soundalias");
            std::vector<uint8_t> One = { 0x34 };
            Group(One, { 270, 0, 0 }, { 787.09f, 605.22f, 159.97f }, 1);
            Check(ParseCreateFx(L, Glued, One).Placements.size() == 1, "binary marker byte then the anchor is read");
            std::vector<uint8_t> NoMark = { 'x', 'y', '~' };
            Put(NoMark, "fire_med_02");
            Put(NoMark, "soundalias");
            const CreateFxResult NM = ParseCreateFx(L, NoMark, One);
            Check(NM.Placements.empty() && NM.Entries == 0, "a ~name glued after printable bytes is no anchor", std::format("{}", NM.Entries));
            std::vector<uint8_t> GluedName = { 0xf2 };
            Put(GluedName, "q");
            GluedName.pop_back();
            Put(GluedName, "xsmoldering");   // the effect's first string has a printable byte glued on
            Anchor(GluedName, "~smoldering");
            Put(GluedName, "fxid");
            std::vector<uint8_t> OneFx = { 0x34 };
            Group(OneFx, { 270, 0, 0 }, { 1, 2, 3 }, 2);
            const CreateFxResult G = ParseCreateFx(L, GluedName, OneFx);
            Check(G.Placements.empty() && G.Unparsed == 1, "an effect name that does not equal its ~name is not read", std::format("{}", G.Unparsed));
            std::vector<uint8_t> NoSound = { 0xf2 };
            Anchor(NoSound, "~delay_min");
            Put(NoSound, "delay_max");
            Put(NoSound, "metal_creaks");
            Put(NoSound, "wrongtail");
            const CreateFxResult NS = ParseCreateFx(L, NoSound, One);
            Check(NS.Placements.empty() && NS.Unparsed == 1, "a delayed sound whose name is not followed by soundalias is not read");
            std::vector<uint8_t> Quote = { 0xf2 };
            Anchor(Quote, "~fire\"x");
            Put(Quote, "soundalias");
            Check(ParseCreateFx(L, Quote, One).Placements.empty(), "a name with a quote is refused");
        }
        // a scriptfile cut out of a zone: header, name, zlib stack, bytecode
        std::vector<uint8_t> Zone(100, 0xAA);
        const std::string Name = "maps/createfx/x_fx";
        // a first occurrence that is no scriptfile (the name inside other data), then the real one
        Put(Zone, Name);
        Zone.insert(Zone.end(), 40, 0xAA);
        std::vector<uint8_t> Packed(mz_compressBound(static_cast<mz_ulong>(Stack.size())));
        mz_ulong PackedSize = static_cast<mz_ulong>(Packed.size());
        mz_compress(Packed.data(), &PackedSize, Stack.data(), static_cast<mz_ulong>(Stack.size()));
        Packed.resize(PackedSize);
        std::vector<uint8_t> Head(32, 0xff);
        const std::array<uint32_t, 3> Sizes = { static_cast<uint32_t>(PackedSize), static_cast<uint32_t>(Stack.size()), static_cast<uint32_t>(Code.size()) };
        std::memcpy(Head.data(), Sizes.data(), 12);
        std::fill(Head.begin() + 12, Head.begin() + 16, static_cast<uint8_t>(0));
        Zone.insert(Zone.end(), Head.begin(), Head.end());
        Put(Zone, Name);
        Zone.insert(Zone.end(), Packed.begin(), Packed.end());
        Zone.insert(Zone.end(), Code.begin(), Code.end());
        Zone.insert(Zone.end(), 40, 0xBB);
        std::vector<uint8_t> S2, B2;
        std::string Why;
        Check(FindScriptFile(L, Zone, Name, S2, B2, Why) && S2 == Stack && B2 == Code, "the scriptfile is found at its second occurrence and cut right", Why);
        Check(!FindScriptFile(L, Zone, "maps/createfx/other_fx", S2, B2, Why), "another name is not found");
        Zone[Zone.size() - 40 - Code.size() - 3] ^= 0x55;   // corrupt the packed stack's tail
        Check(!FindScriptFile(L, Zone, Name, S2, B2, Why), "a stack that does not inflate is refused", Why);
        return Failed;
    }
}
