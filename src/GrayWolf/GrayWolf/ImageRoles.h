#pragma once

// What a run's images are for, from the materials that bind them, and the one thing that is done with
// it: Black Ops III, 4 and Cold War ship a normal map as two components (X in red, Y in green, blue
// holds a variance term, not Z), so a normal exported as it is looks yellow, with no Z. Their
// readers set no ImagePatch, and an image's file says nothing of its use (the name `detail_noise` is a
// normal, a name ending `_n` is not always one), so the role comes from the material slot: the
// semantic hash of the shader texture the image is bound to (the hash of the techset's texture name:
// normalMap 59D30D0F, transNormalMap AB07D475, detailMap1 55A604DC...).
//
// Why a pass over the written files and not a patch set while decoding (IW7 and WWII do that): a
// material is read in one zone and its image is written by the zone that holds the image, or by the
// dependency round after it, so at the time the pixels are written the role is often not known yet.
// Export reads each zone's materials as it opens the zone (IDiskZone::MaterialImages, one Book for
// the run), and after the last image is written (zones, dependencies, After work) this pass rebuilds
// the Z of the images the Book says are normals. It is idempotent: an image whose blue already holds
// the Z its red and green imply is left as it is, so a second run, or an image a reader already
// patched, is not changed again. DDS output is the game's own compressed bytes (BC5 has no third
// channel) and is not touched.

#include "Bo3Target.ImageUnpack.h"
#include "DiskGame.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ImageRoles
{
    // The texture slots (semantic hashes) that hold a normal map in Treyarch's PBR shaders: the
    // normal slots of the Black Ops III techset definitions, found again by the hash of their names.
    bool IsNormalSlot(uint32_t Semantic);

    // The slots the run's materials bind each image to
    class Book
    {
    public:
        // Every image slot of the materials of one zone (IDiskZone::MaterialImages)
        void Add(const DiskGame::MaterialBook& Materials);
        void Note(const std::string& Image, uint32_t Semantic);

        enum class Role
        {
            Unknown,    // no material slot of the run names it
            Normal,     // bound to normal slots only
            Other,      // bound to slots that are not normals only
            Conflict,   // both: rebuilt only when the pixels look like a normal map (LooksLikeNormal)
        };
        Role RoleOf(const std::string& Image) const;
        size_t Images() const { return ByName.size(); }
        // Image names bound to a normal slot only, lower case
        std::vector<std::string> Normals() const;
        size_t Conflicts() const;
        // image name (lower case) -> the semantic hashes of the slots it is bound to
        const std::map<std::string, std::set<uint32_t>>& All() const { return ByName; }

    private:
        std::map<std::string, std::set<uint32_t>> ByName;
    };

    // Rebuilds blue from red and green (Core's NormalZ: Z = 0 where X and Y reach unit length, stored
    // 0.5), alpha untouched. False, and Image unchanged, when 97% of the pixels' blue already holds
    // that Z within 12/255 (the test normal_stats.py uses): a normal that has its Z.
    bool RebuildZ(Rgba8Image& Image);

    // Whether the pixels are those of a tangent-space normal map: red and green centred on 0.5, within the
    // unit disc (97% of the pixels, 1.05 allowed for the encoding), neither spread over the whole range.
    // Only the tie-break for an image bound to a normal slot and to another kind of slot.
    // Strict (the default, the tie-break above) is that; not strict is the same test with the looser bounds of an image whose
    // slot already says normal: means within 0.25 of 0.5, spread under 0.4, 90% in the disc.
    bool LooksLikeNormal(const Rgba8Image& Image, bool Strict = true);

    // What the pass did, to the report's counters. Conflict: images bound to both kinds of slot, of
    // which ConflictRebuilt looked like a normal map and were rebuilt, the others were left
    struct Outcome
    {
        size_t Rebuilt = 0, AlreadyZ = 0, NotInFolder = 0, Failed = 0, Conflict = 0, ConflictRebuilt = 0, ConflictLeft = 0, LeftDds = 0, Unknown = 0, Roles = 0;
        // NotThisRun: a normal's file no writer of this run wrote (left untouched). Collision: files several image
        // names are written under (left untouched). NotNormal: bound to normal slots only, pixels not a normal's.
        // SkippedLayout: not 3 or 4 channel 8-bit (left untouched).
        size_t NotThisRun = 0, Collision = 0, NotNormal = 0, SkippedLayout = 0;
    };

    // Rebuilds the Z of the Book's normals whose pixels look like normals (LooksLikeNormal, for every one)
    // among the image files in ImagesDir (named as the image
    // kind names them, in the run's image format), in place: each file is rewritten beside itself and
    // moved over the old one (a hard link to it, written by the dedup, is not changed). The
    // "patchnormals" setting off: nothing is done. Adds its counters to T.
    Outcome RebuildNormals(const Book& Roles, const std::filesystem::path& ImagesDir, DiskGame::Totals& T);

    // RebuildNormals for a given image format, whatever the "patchnormals" setting says. Only files
    // ExportWriters::ImageWritten vouches for are rewritten, and only when their header says 8-bit with 3 or 4
    // channels and their pixels look like a normal map (LooksLikeNormal), whatever slot kind the Book gives.
    Outcome RebuildNormalsIn(const Book& Roles, const std::filesystem::path& ImagesDir, const std::string& Format, DiskGame::Totals& T);

    int SelfTest(std::string& Failures);
}
