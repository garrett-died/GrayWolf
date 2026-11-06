#pragma once

// Surface types and render class, read from a material's real flag words (docs/hydrax-port/p2-surface-decals.md).
//
// A game's `flag_names.txt` (zonekit/profiles/<profile>/) names the surface types of its surface-flags word and the
// render bits of its techset's renderFlags word, as data. This module reads that file, decodes a word with it, and maps
// a name onto the combo values of the Mod Tools' material.awi (read through Bo3Target::Awi by the caller; no list of
// Black Ops III values is kept here).
//
// A word that was not read is -1 everywhere (Source::SurfaceFlags / RenderFlags), never 0: 0 is a word that was read.

#include "MaterialDescription.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SurfaceTypes
{
    class Names
    {
    public:
        // Reads the rows of a flag_names.txt. False when it holds no surface_mask row or no surface row.
        bool Parse(std::string_view Text);

        enum class State
        {
            Named,       // the masked word is a declared surface type: Name holds it
            NoSurface,   // the masked word is zero: the material states no surface type
            Unnamed,     // a non-zero value that no row declares
        };
        State Decode(uint32_t SurfaceFlags, std::string& Name) const;

        // A decal: the techset sets the decal bit and not the opaque bit (HydraX TreyarchRenderFlags.Classify).
        // False when the file has no render rows.
        bool IsDecal(uint32_t RenderFlags) const;
        bool HasRenderBits() const { return DecalBit != 0; }
        size_t Count() const { return Types.size(); }
        uint32_t Mask() const { return TypeMask; }
        bool HasGame(const std::string& Game) const;

        // A game whose surface word is a bit set (a `surface_bits` row: Modern Warfare Remastered, a u64): bit n of the word names surface type n.
        // Zero bits state no surface; one bit is named by its `surface_bit` row; two or more are ambiguous (a layered material holds the union
        // of its layers' bits: the reader resolves the layer it wants before the word is decoded). Target is the BO3 surfaceType the row maps to
        // (empty: none near enough; Name is then the game's own name, which the exporter reports as unmapped).
        bool IsBitSet() const { return BitSet; }
        enum class BitState { NoSurface, Named, Unnamed, Ambiguous };
        BitState DecodeBits(uint64_t Word, std::string& Name, std::string& Target) const;
        // A `bo3_unused|<name>|<value>` row: the game names this surface type but no stock BO3 material entry uses it (HydraX writes
        // <none> for the BO4 player and riotshield). Per game, as data: the t7 table has no such row, BO3 keeps both.
        bool UnusedInBo3(const std::string& SurfaceName) const;
        // `clip_tool|<BO3 tool material>|<exact clip contents word>|<what BO3 cannot express>` rows: the tool material a collision mesh whose clip material has exactly these contents is named, as data of the
        // game's profile (the writer's contents mapping for brushes is used for the rest). The fourth field is the approximation, reported.
        struct ClipToolRow
        {
            uint32_t Contents;
            std::string Tool, Approximation;
            int64_t Surface = -1;   // the clip material's surface word the row needs, -1: any
        };
        const std::vector<ClipToolRow>& ClipTools() const { return ClipToolRows; }
        // `clip_match|<tool>|<contents>||<surface>` rows (Black Ops 4): the tool a (surface, contents) pair is named by, by properties: the tool whose declared surface bits equal the pair's and whose contents (the
        // compile-added ones ignored) are all set in the pair's, fewest extra contents bits first; a tie among different names, or no match, gives an empty name (HydraX ToolMaterialProperties.Match).
        std::string ClipMatch(uint32_t Surface, uint32_t Contents) const;

    private:
        uint32_t TypeMask = 0, OpaqueBit = 0, DecalBit = 0;
        bool BitSet = false;
        struct BitRow
        {
            int Bit;
            std::string Name, Target;
        };
        std::vector<BitRow> Bits;
        std::vector<std::string> Games;
        std::vector<std::pair<uint32_t, std::string>> Types;
        std::vector<std::string> Unused;
        std::vector<ClipToolRow> ClipToolRows;
        std::vector<ClipToolRow> ClipMatchRows;
    };

    // The profile's table (zonekit/profiles/<profile>/flag_names.txt, found as the other profile files are); null when there is none.
    // ProfileFound (when given) tells a profile folder that was not found at all (zonekit/profiles missing: nothing is cached, the next
    // call looks again) from a found profile with no table.
    const Names* For(const std::string& Profile, bool* ProfileFound = nullptr);

    // A u32 of the attached image at Address, or -1 when fewer than four bytes could be read there. (ProcessReader::Read<T> zero-fills a
    // failed read, so it cannot tell a failed read from a word of 0.)
    int64_t ReadWord(uint64_t Address);

    // True only when Flags (>= 0) is a word that was read and, with the table of the game the materials sidecar names (Source game id: "bo4", "bo3",
    // "coldwar"; the table's `game` row), has no surface-type bits. This is what the writer's positive surface_none_in_source row rests on: it is
    // derived from the flags, never taken from a sidecar's text.
    bool ReadAndNamesNoSurface(const std::string& Game, int64_t Flags);

    // How a techset's renderFlags classify a material. Unread: no word (-1), a word of 0 (it says nothing: a stub techset reads 0), or a
    // game with no render rows.
    enum class RenderClass { Unread, Decal, NotDecal };
    RenderClass ClassOf(const std::string& Profile, int64_t RenderFlags);

    // Fills D.SurfaceType from Source.SurfaceFlags with the profile's table; when it cannot, SurfaceType stays empty and
    // SurfaceWhy says why: "table_missing" (zonekit/profiles was not found), "no_table" (the profile has none: not measured),
    // "not_read" (the reader gave no word), "none_in_source" (a word was read and names no surface), "unnamed:0x..." (a value
    // no table row declares).
    void Apply(const std::string& Profile, const Materials::Source& Source, Materials::Description& D);

    // True when ClassOf is Decal.
    bool IsDecal(const std::string& Profile, int64_t RenderFlags);

    // The combo option equal to Name (case aside); empty when Options holds none. Options come from material.awi.
    std::string ToBo3(const std::string& Name, const std::vector<std::string>& Options);

    int SelfTest(std::string& Failures);
}
