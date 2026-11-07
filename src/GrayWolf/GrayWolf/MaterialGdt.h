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

class WraithModel;
namespace DiskGame { struct Context; struct Totals; }

namespace MaterialGdt
{
    // A source material's BO3 asset name: lowercased, every character outside [a-z0-9_] as '_',
    // leading and trailing '_' dropped ("mc/mtl_Foo" -> "mc_mtl_foo", "~mtl_x-1" -> "mtl_x_1").
    // A function of the name alone, so a model file written on its own names what the GDT defines.
    std::string AssetName(const std::string& Source);

    // The name a material has in the BO3 GDT and in the model files that name it: AssetName, then Placement::Shorten (over 63 characters becomes the
    // first 55, '_' and 7 hex of the FNV-1a of the name; HydraX MaterialEntryNames). The one function both writers call.
    std::string EntryName(const std::string& Source);

    // The material names of a model, in place, as EntryName gives them; the old names go to Saved (RestoreModelMaterialNames puts them back).
    // Called only while a BO3 material GDT is being written (ModelFilesUseAssetNames), so no other output changes.
    void NameModelMaterials(WraithModel& Model, std::vector<std::string>& Saved);
    void RestoreModelMaterialNames(WraithModel& Model, const std::vector<std::string>& Saved);
    // The same as a scope: names on construction when On, puts the model's own names back on destruction, also when a writer throws.
    class ScopedModelMaterialNames
    {
    public:
        ScopedModelMaterialNames(WraithModel& Model, bool On);
        ~ScopedModelMaterialNames();
        ScopedModelMaterialNames(const ScopedModelMaterialNames&) = delete;
        ScopedModelMaterialNames& operator=(const ScopedModelMaterialNames&) = delete;

    private:
        WraithModel& Model;
        bool On;
        std::vector<std::string> Saved;
    };

    // True for a source image name an engine decorated (Infinity Ward): a "&" or "~" form marker ("_c&", "_s~"), a "packed_" prefix, or an 8-hex
    // token with a digit and a letter ("..._482cf5df_c"). Such an image is named from its material and slot ("i_<material>_<c|n|g|s|o|m|t|e>").
    // Decided from the source file name alone. Limit: a plain word that reads as a hash ("decade00", "facade12", "a1b2c3d4") is taken for one and is
    // renamed too (stock has 0 such names in 23,218; the result is a legal material-and-slot name). Any other stem, including "-", space, "." and
    // "@" ("warning@col"), is not decorated and is cleaned by AssetName as before. Clean BO4 and Cold War names keep their stem: stock does so in 74%.
    bool IsDecoratedImageName(const std::string& Stem);

    // The number in a `_lodNNNN` suffix's digits; INT_MAX when they do not fit an int (never throws).
    int LodNumber(const std::string& Digits);

    // The options of material.awi's surfaceType combo under a Mod Tools root (Bo3Target::Awi, read as data, no copy of the list);
    // empty when the file or the key is not there.
    std::vector<std::string> SurfaceChoicesOf(const std::filesystem::path& ModToolsRoot);

    class Builder
    {
    public:
        // A test replaces the derive step (null: the real one): it gets the request and fills the file, or fails
        std::function<bool(const char* What, const Bo3Target::DeriveRequest& Request, std::filesystem::path& File)> DeriveHook;

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
        // The 99th percentile of the grey level (0..255) of an image file's colour channels, -1 when it cannot be read (the pixel check of Description::PixelMetal)
        static int GreyP99(const std::filesystem::path& File);
        void Add(const Materials::Description& D, const std::filesystem::path& Dir, const Materials::Source* Source = nullptr,
                 const std::string& Game = std::string());

        // The values material.awi's surfaceType combo holds (Bo3Target::Awi, read from the Mod Tools by the caller). With them a
        // description's surface type is written only when an option has its name; else <none> and a material.surface_unmapped note.
        // Without them (the default: no Mod Tools root) the name is written as the game's table spells it, which is unchecked.
        void SurfaceOptions(std::vector<std::string> Options) { SurfaceChoices = std::move(Options); }

        // What the map measured about the materials its own geometry draws (their Source names). Every other material keeps
        // "<not in editor>", which hides it in Radiant; these get a usage chosen from the BO3 techset they ended up with and the
        // measure (Techsets::UsageRules, zonekit material-usage.json). A material whose facing cannot be measured gets the
        // fallback value and a material.usage_fallback note. Without TechsetData the facing alone decides.
        void SetMapUse(std::map<std::string, Bo3Target::Techsets::MapUse> BySource) { Usage = std::move(BySource); }
        // The semantic a stock image (one the Mod Tools hold under the name this builder would give the image) declares, "" when it
        // cannot be read or the image is not stock. With it the techset and the slot are chosen by what the stock image is, not by
        // what the source game slot meant: a stock specular mask never lands in a colour specular slot.
        void SetStockImages(std::function<std::string(const std::string&)> SemanticOf) { StockSemanticOf = std::move(SemanticOf); }

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
            std::vector<std::pair<std::string, Note>> Skipped;   // materials a game's adapter said not to export (asset name, why)
            size_t NameCollisions = 0;                  // images whose name another image already had (they got _2, _3 ...; none is dropped)
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
            std::string StockSemantic;   // the semantic the stock image of this name declares (SetStockImages), else empty
            bool operator<(const Image& O) const;
        };
        struct Material
        {
            std::string Source, Category, Type;
            std::string SurfaceType = "<none>";                 // the game's own, never guessed (Description::SurfaceType)
            std::map<std::string, Image> Slots;                 // colorMap -> image
            std::vector<std::pair<std::string, std::string>> Values;   // other fields set
            bool Targeted = false;
            Materials::ImageNaming Naming;                      // the game adapter's declaration of its image names (Description::Naming)
            bool SourceDecal = false;                           // the source game's techset name or renderFlags say decal (read when no catalog chose the BO3 techset)
            std::vector<Note> Notes;
        };

    private:
        std::function<std::string(const std::filesystem::path&)> BaseImageOf;
        std::function<std::string(const std::string&)> StockSemanticOf;
        std::string Prefix;
        std::map<std::string, Material> Defined;                // by asset name
        std::map<std::string, std::set<std::string>> Collisions;
        std::map<std::string, Bo3Target::Techsets::MapUse> Usage;
        std::vector<std::string> SurfaceChoices;
        std::map<std::string, size_t> Count;
        std::vector<std::pair<std::string, Note>> SkippedMaterials;
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
        std::filesystem::path AddDir;       // the folder of the sidecar being added (ChooseTechset resolves source image paths against it)
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
