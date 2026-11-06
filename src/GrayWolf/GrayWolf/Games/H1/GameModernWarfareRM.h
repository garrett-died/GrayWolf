#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <set>
#include <vector>

#include "CoDAssetType.h"

class GameModernWarfareRM
{
public:
    static std::unique_ptr<XAnim_t> ReadXAnim(const CoDAnim_t* Animation);
    static std::unique_ptr<XModel_t> ReadXModel(const CoDModel_t* Model);

    static XMaterial_t ReadXMaterial(uint64_t MaterialPointer);

    // A name-only material header (no image: another zone defines it, a map's techsets_<map> zone
    // or a common one) takes its texture slots from there: (semantic, image name) pairs, read by
    // the disk exporter. Unset, the header has no texture.
    static thread_local std::function<bool(const std::string& MaterialName, std::vector<std::pair<uint32_t, std::string>>& Images)> ImagesElsewhere;

    // What a name-only material header (no definition here) takes from the zone that defines it: its techset name, sort key, surface word (a layered
    // blend: the bit of its first layer where the book can tell, else the union of its layers) and the raw bytes of its constant table
    struct Details
    {
        std::string Techset;
        int SortKey = -1;
        int64_t SurfaceBits = -1;
        std::vector<uint8_t> Constants;
        std::string SurfaceVia;   // set when SurfaceBits came from a layer id intersection (LayerBits)
    };
    // The two hooks of a served zone: Details completes a header from the books; Blend gives a level blend ("*49n_50n") its first layer's surface word, for a
    // header as well as for a material the zone defines itself (empty: the word is kept). No static data member: the hooks live in a scoped object
    // (HooksScope installs them for the thread and puts the previous ones back), reached through CurrentHooks().
    struct Hooks
    {
        std::function<bool(const std::string& MaterialName, Details& Out)> Details;
        std::function<int64_t(const std::string& MaterialName, int64_t Word, std::string& Via)> Blend;
    };
    static Hooks& CurrentHooks();
    class HooksScope
    {
    public:
        explicit HooksScope(Hooks H) : Saved(std::move(CurrentHooks())) { CurrentHooks() = std::move(H); }
        ~HooksScope() { CurrentHooks() = std::move(Saved); }
        HooksScope(const HooksScope&) = delete;
        HooksScope& operator=(const HooksScope&) = delete;

    private:
        Hooks Saved;
    };
    // Applies the Blend hook to a read material: the one place a blend gets its first layer's surface
    static void ResolveBlendSurface(XMaterial_t& Result, const std::string& RawName);
    // A header names its material only (the zone leaves every other field zero: no images, no image table, no techset, no surface word, no constants); a
    // material the zone defines with no texture has a techset or a surface word and keeps its own data.
    static bool IsMaterialHeader(const MWRXMaterial& M);

    // The sounds one alias names (loaded, streamed, primed), each loaded sound once across calls
    static void AddAliasSounds(const MWRSoundAlias& Alias, const std::string& SoundName, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Out);
};