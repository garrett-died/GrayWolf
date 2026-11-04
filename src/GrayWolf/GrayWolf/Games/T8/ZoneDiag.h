#pragma once

// Diagnostics for the BO4 map reader, written to the GH_MAPDEBUG file (Maps::DebugFile) and only
// when it is set: what a clip map's collision counts are, and whether the terrain's pointers land
// in the zone. They read the zone and change nothing.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "ByteView.h"
#include "MapScene.h"

namespace Bo4ZoneDiag
{
    // What the diagnostics need of the zone being read (ZonekitZone's Root and Off are protected)
    struct ZoneAccess
    {
        ByteView Image;
        std::function<int64_t(const char*)> Root;       // the zone-level asset of a type, -1 when none
        std::function<size_t(const char*)> Count;       // how many assets of a type the walk found
        std::function<int64_t(uint64_t)> Off;           // where a relocated pointer's data is, -1 elsewhere
        std::function<std::string(uint64_t)> Describe;  // a pointer field's value in words (null, marker, zone+0x.., ref)
        std::function<std::string(uint64_t)> NameAt;    // the hashed name at an in-zone asset header, "" if none
    };

    // [collision] lines: the clip map's raw count fields beside what the reader holds
    void Collision(FILE* Out, const ZoneAccess& Zone, const Maps::MapScene& Scene);

    // [terrain] lines: the five checks of plan-maps.md section 1, S1
    void Terrain(FILE* Out, const ZoneAccess& Zone);
}
