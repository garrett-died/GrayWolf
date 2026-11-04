#pragma once

// Black Ops III Mod Tools material GDT entries for any game's materials (the material_gdt
// setting): each Materials::Description the export already computed (the <model>.materials.json
// sidecars, a map's scene materials) as a `material.gdf` entry whose image slots name `image.gdf`
// entries for the exported image files, so a ported model or Radiant map links in the mod tools
// without its materials made by hand in APE. Only the description is read: a game supports it by
// having a material adapter.

#include "Bo3Target.MaterialImages.h"
#include "Bo3Target.Techsets.h"
#include "GdtWriter.h"
#include "MaterialDescription.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace DiskGame { struct Context; struct Totals; }

namespace MaterialGdt
{
    // A source material's BO3 asset name: lowercased, every character outside [a-z0-9_] as '_',
    // leading and trailing '_' dropped ("mc/mtl_Foo" -> "mc_mtl_foo", "~mtl_x-1" -> "mtl_x_1").
    // A function of the name alone, so a model file written on its own names what the GDT defines.
    std::string AssetName(const std::string& Source);

    // The number in a `_lodNNNN` suffix's digits; INT_MAX when they do not fit an int (never throws).
    int LodNumber(const std::string& Digits);

    class Builder
    {
    public:
        // BaseImage: an image file (absolute) as the GDT's baseImage, a path below the BO3 root
        // with '/' separators ("" leaves the slot out). Prefix: the counters' ("material_gdt_").
        Builder(std::function<std::string(const std::filesystem::path& File)> BaseImage, std::string Prefix);

        // With On, an image slot that has no BO3 form as exported (a packed normal, a channel other than
        // red or alpha, a roughness map, a fused colour with its metal mask) is converted: the image is
        // loaded, unpacked (Bo3Target.ImageUnpack.h) and the BO3 image written beside the source, and
        // the slot names that file. Counters: converted_<what> for each conversion, not_converted_<what>_<reason>
        // for a conversion that was due and failed (the slot then keeps its old form and counter). Off
        // (the default) counts only, as before. Only maps whose Description states a PackKind, or
        // whose channel or roughness has no direct slot, are touched.
        void ConvertImages(bool On) { Convert = On; }
        // The folder a conversion may read from and write into (the run's output). Without it nothing is converted:
        // a sidecar path outside it (absolute, ../) is refused.
        void SetRunRoot(std::filesystem::path Root) { RunRoot = std::move(Root); }
        // On: the conversions are worked out and counted but no image file is written (a plan)
        void PlanOnly(bool On) { Derived.DryRun(On); }
        // True for a file this builder derived (written, reused, or in a plan only worked out)
        bool IsDerived(const std::filesystem::path& File) const { return Derived.Made(File); }

        // With data, the BO3 techset is chosen from the catalog (Bo3Target.Techsets.h) by the slots the description
        // fills, each image gets the semantic of the slot it lands on, and the entry holds every key of the techset
        // and the stock defaults. Without it (null, the default) or with an empty catalog, the techset is picked by
        // the fixed rules of old and the entry holds the few keys it always did (an empty catalog is counted
        // techset_data_missing). Only a target sets it: a run with no target stays what it was.
        void TechsetData(const Bo3Target::Techsets::Data* Data) { Targets = Data; }

        // A material, its image paths relative to Dir. The first description of an asset name
        // wins: the same material from another model is counted shared, another source name
        // that gives the same asset name is counted a collision (callers add in a sorted order).
        // Source: the game's raw material (its techset name, image semantics, constant block) when the sidecar
        // holds it; Game: the sidecar's game id ("bo4"). Both only matter with TechsetData.
        void Add(const Materials::Description& D, const std::filesystem::path& Dir, const Materials::Source* Source = nullptr,
                 const std::string& Game = std::string());

        bool Empty() const;
        // the material entries then the image entries, each by name
        std::vector<std::string> Entries(const char* Eol) const;
        // The same entries as data, for a caller that places them itself (Bo3Target.Publish): fields in
        // the order Entries writes them. An image's baseImage holds what the BaseImage function gave,
        // not yet escaped (Entries escapes it with Gdt::Path).
        // A line for graywolf_approximations.tsv: material.techset_fallback, material.unmapped_slot, ...
        struct Note
        {
            std::string Code, Detail, Message;
        };
        struct ResolvedMaterial
        {
            std::string Name, Source;
            Gdt::Fields Fields;
            std::vector<std::string> Images;    // the image entities its slots name (code images left out)
            bool Targeted = false;              // the techset came from the catalog (TechsetData)
            std::vector<Note> Notes;
        };
        struct ResolvedImage
        {
            std::string Name;
            std::filesystem::path File;         // the image file the entity is made from
            Gdt::Fields Fields;
        };
        struct Resolved
        {
            std::vector<ResolvedMaterial> Materials;    // by name
            std::vector<ResolvedImage> ImagesOut;       // by name
        };
        Resolved Resolve() const;
        // { "materials": { asset: source }, "collisions": { asset: [other sources] } }
        std::string NamesJson() const;
        // what was written and what had no BO3 slot: <Prefix><what> -> count
        const std::map<std::string, size_t>& Counters() const { return Count; }

        struct Image
        {
            std::filesystem::path File;
            std::string Semantic, Core, Compression;
            bool FromAlpha = false;
            std::string Builtin;   // a BO3 code image ("$normal") named as it is: no entry, no file
            bool operator<(const Image& O) const;
        };
        struct Material
        {
            std::string Source, Category, Type;
            std::map<std::string, Image> Slots;                 // colorMap -> image
            std::vector<std::pair<std::string, std::string>> Values;   // other fields set
            bool Targeted = false;
            std::vector<Note> Notes;
        };

    private:
        std::function<std::string(const std::filesystem::path&)> BaseImageOf;
        std::string Prefix;
        std::map<std::string, Material> Defined;                // by asset name
        std::map<std::string, std::set<std::string>> Collisions;
        std::map<std::string, size_t> Count;
        bool Convert = false;
        std::filesystem::path RunRoot;
        Bo3Target::DerivedImages Derived;
        const Bo3Target::Techsets::Data* Targets = nullptr;
        size_t& Counter(const std::string& What) { return Count[Prefix + What]; }
        bool Slot(Material& M, const char* Key, const Materials::Map& Map, const std::filesystem::path& Dir, const char* Semantic,
                  const char* Core, const char* Compression);
        // a slot naming a derived image file (written by Bo3Target::DerivedImages)
        bool DerivedSlot(Material& M, const char* Key, const std::filesystem::path& File, const char* Semantic, const char* Core,
                         const char* Compression);
        // the map's file when it can be converted: a real image file, not a BO3 code image
        bool Convertible(const Materials::Map& Map, const std::filesystem::path& Dir, std::filesystem::path& File);
        // runs the request; on failure counts not_converted_<What>_<reason>
        bool Derive(const char* What, const Bo3Target::DeriveRequest& Request, std::filesystem::path& File);
        // The catalog techset for the slots M holds; the slots it lacks are dropped and reported; the constants a
        // BO4 techset's layout proves are read from the source's constant block
        void ChooseTechset(Material& M, const Materials::Description& D, const Materials::Source* Source, const std::string& Game,
                           bool Emissive, bool Spec, bool Blend, bool Cutout);
    };

    // The model kind's GDT: every <model>.materials.json under the run's models folder (this run's
    // and earlier ones'), its materials, their images and the models' XMODEL_EXPORT files, as
    // <out>/source_data/graywolf_<out folder>.gdt. Paths assume <out> is copied to
    // <BO3>/model_export/graywolf/<out folder>/. Counters into T.
    void WriteForModels(const DiskGame::Context& C, DiskGame::Totals& T);

    // While a run with material_gdt on exports, the mod tools' model files (XMODEL_EXPORT, _BIN)
    // name their materials by AssetName (LiveExport reads it); off again when the guard ends.
    // Assertions that need no game files: the techset the Builder writes for a target (Bo3Target.Techsets.h)
    int SelfTest(std::string& Failures);

    bool ModelFilesUseAssetNames();
    class AssetNamesInModelFiles
    {
    public:
        explicit AssetNamesInModelFiles(bool On);
        ~AssetNamesInModelFiles();
        AssetNamesInModelFiles(const AssetNamesInModelFiles&) = delete;
        AssetNamesInModelFiles& operator=(const AssetNamesInModelFiles&) = delete;

    private:
        bool Was;
    };
}
