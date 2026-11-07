#pragma once

// A map's script-placed effects (M4d, docs/hydrax-port/m4.md). Modern Warfare Remastered places effects and sounds from a createfx script (maps/createfx/<map>_fx), not the entity
// string; the script lives in the map's patch zone (patch_<map>.ff) as a compiled scriptfile: a name, a zlib string stack, the bytecode. The game's facts (header offsets, the marker
// byte before each placement's string, the bytecode's vector, call and field-set bytes) are a CreateFxLayout the game's reader supplies (Games/H1); this file knows none.
//
// How a placement is tied (tested on mp_crash, mp_bog, mp_crossfire; NOT a full decode of the bytecode): the stack holds one marked string per placement, in code order (the
// anchor); its neighbours say the kind (loop sound, delayed sound, effect). The bytecode holds, per placement, one instruction group: a prefix byte, two vector literals
// (angles, origin), the call marker, then field-set instructions. Each vector pair is taken only when it has the prefix before it and the call marker after it; the group up to the
// next pair must hold exactly one call marker and the number of field-set instructions its kind needs (1 loop sound, 2 effect, 3 delayed sound on mp_crash: 230 of 230). Equal
// counts are not enough: a file whose k-th group does not fit the k-th anchor's kind is rejected whole (Unties). A placement with angles beyond the limit or an origin outside the
// map's bounds is rejected alone (counted). Delays and other keys stay unread.

#include "MapScene.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Maps
{
    struct CreateFxLayout
    {
        std::string ScriptPrefix, ScriptSuffix;     // the scriptfile is ScriptPrefix + map + ScriptSuffix
        std::array<size_t, 3> HeaderBack{};          // bytes before the name: the compressed stack size, the stack size, the bytecode size (u32 each)
        int AnchorPrefix = -1;                       // the byte before each placement's anchor string
        std::string LoopNext;                        // loop sound: anchor "~name", next string LoopNext
        std::string DelayAnchor, DelayNext, DelayAfter;   // delayed sound: anchor DelayAnchor, then DelayNext, then the name, then DelayAfter
        std::string FxNext;                          // effect: the string before the anchor is "name", the anchor "~name", the next FxNext
        uint8_t VectorOp = 0, PairPrefix = 0;        // a vector literal: VectorOp and three floats; the byte before a pair
        std::array<uint8_t, 2> Call{};               // the bytes after a pair that start its call
        std::vector<uint8_t> FieldSet;               // one field-set instruction
        int FieldSetsLoop = 0, FieldSetsDelay = 0, FieldSetsFx = 0;
        float AngleLimit = 720.0f;
    };

    struct CreateFxBounds
    {
        float Lo[3] = { 0, 0, 0 }, Hi[3] = { 0, 0, 0 };
        bool Valid = false;
    };

    struct CreateFxResult
    {
        std::vector<EffectPlacement> Placements;
        size_t Entries = 0, VectorPairs = 0;   // the anchors parsed, the tied-looking vector pairs
        size_t Unparsed = 0;                   // anchors whose neighbours fit no kind, or whose name is unsafe: the file is rejected
        size_t Unties = 0;                     // groups that do not fit their anchor's kind: the file is rejected
        size_t Outside = 0, BadAngles = 0;     // placements rejected alone
        bool Mismatch = false;                 // the file was rejected (Placements empty)
    };
    CreateFxResult ParseCreateFx(const CreateFxLayout& Layout, std::span<const uint8_t> Stack, std::span<const uint8_t> Bytecode, const CreateFxBounds& Bounds = {});

    // The scriptfile Name in a decompressed zone, trying every place the name occurs: its inflated stack and its bytecode. False (Why filled) when none fits the zone.
    bool FindScriptFile(const CreateFxLayout& Layout, std::span<const uint8_t> Zone, const std::string& Name, std::vector<uint8_t>& Stack, std::vector<uint8_t>& Bytecode, std::string& Why);

    int CreateFxSelfTest(std::string& Failures);
}
