#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

// A layered blend ("*49n_50n") is a material of the level's own: each token is a per-map layer id, the first token is the base layer, and its surface word is
// the union of its layers (mwr-materials.md 1.2, exact on 7,653 of 7,653 blends whose ids resolve). BO3 takes one surface per material, so the bit of the first
// layer is wanted: the bit of one layer id is found by intersecting the words of all the blends of the level that hold the id (301 of 409, 271 of 371, 245 of 306
// ids resolve to exactly one bit). An id that does not resolve leaves the union, which the surface decoding calls ambiguous.
//
// The intersection is evidence, not a reading: a layer with no surface bit of its own that sits in a blend with a single-bit partner is handed the partner's bit.
// Every value that came from an intersection is therefore reported (Via: the id, the bit, how many blends held the id and how many of those had two or more
// bits), and the exporter writes it as a material.surface_from_layer_id row.
namespace H1Layers
{
    inline int Popcount(uint64_t V)
    {
        int N = 0;
        for (; V != 0; V &= V - 1) N++;
        return N;
    }

    // The layer ids of a blend name: "*49n_50n" (or "49n_50n") gives 49n, 50n
    inline std::vector<std::string> IdsOf(const std::string& Name)
    {
        std::vector<std::string> Ids;
        size_t Start = 0;
        while (Start < Name.size() && (Name[Start] == ',' || Name[Start] == '*')) Start++;   // a header is named ",*49n_50n", a defined blend "*49n_50n"
        while (Start <= Name.size())
        {
            size_t End = Name.find('_', Start);
            if (End == std::string::npos) End = Name.size();
            if (End > Start) Ids.push_back(Name.substr(Start, End - Start));
            Start = End + 1;
        }
        return Ids;
    }

    class LayerBits
    {
    public:
        // One blend of the level and its surface word (a word of 0 or less carries nothing)
        void Add(const std::string& Name, int64_t Word)
        {
            if (Name.empty() || Name[0] != '*' || Word <= 0) return;
            const bool Multi = Popcount(static_cast<uint64_t>(Word)) >= 2;
            for (const std::string& Id : IdsOf(Name))
            {
                Entry& E = Candidates.try_emplace(Id, Entry{ static_cast<uint64_t>(Word), 0, 0 }).first->second;
                if (E.Blends != 0) E.Bits &= static_cast<uint64_t>(Word);
                E.Blends++;
                if (Multi) E.MultiBit++;
            }
        }

        // The surface word to give a material: Word for anything but a blend with several bits; for such a blend the first layer's bit when it resolved. Via is
        // set whenever the value of a blend rests on the table: an intersection (kind=intersection), or a single-bit word that the first layer is given without
        // any evidence that the layer has that bit itself (kind=single_bit_word: the thin case, a layer with no bit of its own takes its partner's). A resolved bit
        // that does not lie inside the blend's own word is no resolution.
        int64_t Surface(const std::string& Name, int64_t Word, std::string* Via = nullptr) const
        {
            if (Word <= 0 || Name.empty() || (Name.find('*') == std::string::npos && Name[0] != ',')) return Word;
            const std::vector<std::string> Ids = IdsOf(Name);
            if (Ids.empty()) return Word;
            const auto It = Candidates.find(Ids.front());
            const bool Single = Popcount(static_cast<uint64_t>(Word)) < 2;
            if (Single)
            {
                if (Via != nullptr && It != Candidates.end())
                    *Via = std::format("kind=single_bit_word, layer_id={}, bit=0x{:X}, blends_with_id={}, multi_bit_blends_with_id={}", Ids.front(), static_cast<uint64_t>(Word),
                                       It->second.Blends, It->second.MultiBit);
                return Word;
            }
            if (It == Candidates.end() || Popcount(It->second.Bits) != 1) return Word;
            if ((It->second.Bits & static_cast<uint64_t>(Word)) != It->second.Bits) return Word;   // not one of this blend's own bits: the intersection says nothing about it
            if (Via != nullptr)
                *Via = std::format("kind=intersection, layer_id={}, bit=0x{:X}, union=0x{:X}, blends_with_id={}, multi_bit_blends_with_id={}", Ids.front(), It->second.Bits,
                                   static_cast<uint64_t>(Word), It->second.Blends, It->second.MultiBit);
            return static_cast<int64_t>(It->second.Bits);
        }

        // The surface word the intersection gives one layer id (the layer's own, whichever place it has in a blend): its bits, one when the id resolved, several when the
        // blends that hold it share more than one bit (then the layer is one of them: ambiguous, never a guess), -1 for an id no blend held. Via: the evidence, as Surface's.
        int64_t OfId(const std::string& Id, std::string* Via = nullptr) const
        {
            const auto It = Candidates.find(Id);
            if (It == Candidates.end() || It->second.Bits == 0) return -1;
            if (Via != nullptr)
                *Via = std::format("kind=layer_id, layer_id={}, bits=0x{:X}, blends_with_id={}, multi_bit_blends_with_id={}", Id, It->second.Bits, It->second.Blends, It->second.MultiBit);
            return static_cast<int64_t>(It->second.Bits);
        }

        size_t Resolved() const
        {
            size_t N = 0;
            for (const auto& [Id, E] : Candidates) N += Popcount(E.Bits) == 1;
            return N;
        }
        size_t Ids() const { return Candidates.size(); }

    private:
        struct Entry
        {
            uint64_t Bits = 0;
            int Blends = 0, MultiBit = 0;
        };
        std::unordered_map<std::string, Entry> Candidates;
    };
}
