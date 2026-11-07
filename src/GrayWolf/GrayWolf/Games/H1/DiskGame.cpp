#include "stdafx.h"

// Modern Warfare Remastered (H1) on the disk template: a ZonekitZone with the h1 profile, served to
// the live MWR readers (GameModernWarfareRM.cpp). What is MWR's own: the streamed-image table in the
// .ff header, the LOD surfaces the game fills in at load time, the pixels of loaded images inside
// the zone, and sounds through the aliases.

#include "Games/H1/BlendLayers.h"
#include "MapBlend.h"
#include "MapSurfaceFromCollision.h"
#include "MapCreateFx.h"
#include "MapSunProbes.h"
#include "ExrWriter.h"
#include "ExrReader.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include "SkyCubeDds.h"
#include "FastFile.h"
#include "Games/H1/LayerBits.h"
#include "CoDAssets.h"
#include "DiskGame.h"
#include "DiskGameSizes.h"
#include "ExportWriters.h"
#include "GameModernWarfareRM.h"
#include "GameReaders.h"
#include "Games/IW/IWMaterialBooks.h"
#include "Games/IW/IWSoundAliases.h"
#include "Games/IW/IWZone.h"
#include "Games/IW/IwEffects.h"
#include "SurfaceTypes.h"
#include "Games/IW/IwWeaponDefs.h"
#include "Games/IW/IwZoneAnimOwners.h"
#include "Games/H1/EntityKeys.h"
#include "MapLive.h"
#include "PAKCache.h"
#include "PAKSupport.h"

#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    // XModel (0x2a8, MWRXModel): the bone count at 8, six 0x40 LOD records from 0x80, the LOD count
    // at 0x200, a LOD's surfaces at +0x30
    constexpr int64_t ModelBoneCountAt = 8, LodCountAt = 0x200;
    IWZone::LodLayout ModelLods()
    {
        IWZone::LodLayout L;
        L.Lods = 0x80, L.LodSize = 0x40, L.LodCount = LodCountAt, L.LodSurfs = 0x30;
        return L;
    }

    // GfxImage (0x68): format at 0x18, streamed flag at 0x35, name at 0x60. A loaded image keeps
    // its pixels in the zone (0x38), their size at 0x24 and its size at 0x2c/0x2e; a streamed one
    // its levels' sizes at 0x40 (MWRGfxImage) and its pixels in imagefile<N>.pak.
    constexpr int64_t ImageFormatAt = 0x18, ImageDataSizeAt = 0x24, ImageWidthAt = 0x2c, ImageHeightAt = 0x2e;
    constexpr int64_t ImageStreamedAt = 0x35, ImageDataAt = 0x38, ImageNameAt = 0x60;

    // The install folder: a localized zone (french/fra_*.ff) reads the root's image paks, and the
    // sound reader finds its language folder from there.
    fs::path InstallRoot(const std::string& FastFilePath)
    {
        return IWMaterialBooks::InstallDir(FastFilePath, "imagefile1.pak");
    }

    // Material (0x250): name at 0, image count at 0x120, its slots at 0x130. A level's models and
    // the cosmetic zones name their materials by header only (",name", no slot): the material is
    // defined in the level's techsets_<level> zone or a common one.
    IWMaterialBooks::Layout MaterialLayout()
    {
        IWMaterialBooks::Layout L;
        L.Profile = "h1";
        L.ImageCount = 0x120, L.Images = 0x130, L.ImageName = ImageNameAt;
        L.WithSlotsOnly = true;
        // what a header takes from the zone that defines it: the sort key (u8 at 9), the techset (the pointer at 0x128), the surface word (u64 at 0x20)
        // and the constant table (u8 count at 0x121, the pointer at 0x138, 0x20 bytes each)
        L.SortKey = 9, L.Techset = 0x128, L.SurfaceBits = 0x20;
        L.ConstantCount = 0x121, L.Constants = 0x138, L.ConstantSize = 0x20;
        return L;
    }


    using H1Layers::LayerBits;

    // the level's blends (the first book), as the layer table wants them
    std::unique_ptr<LayerBits> MakeLayers(IWMaterialBooks::Lookup& Books)
    {
        auto L = std::make_unique<LayerBits>();
        Books.EachDetails([&](const std::string& Name, const IWMaterialBooks::Extra& E) { L->Add(Name, E.SurfaceBits); });
        return L;
    }

    // Where a zone's material headers are defined, in the order the game loads them: the level's
    // techsets zone, then the mode's common zones (MP: *_mp).
    std::vector<fs::path> BookZones(const std::string& FastFilePath)
    {
        const fs::path Dir = fs::path(FastFilePath).parent_path();
        const std::string Zone = fs::path(FastFilePath).stem().string();
        const bool Mp = Zone.rfind("mp_", 0) == 0 || Zone.find("_mp") != std::string::npos;
        std::vector<std::string> Names = { "techsets_" + Zone };
        // a level's patch zone (patch_<level>) takes its materials from the level's
        if (Zone.rfind("patch_", 0) == 0) Names.insert(Names.end(), { "techsets_" + Zone.substr(6), Zone.substr(6) });
        if (Mp) Names.insert(Names.end(), { "techsets_common_mp", "patch_common_mp", "common_mp", "techsets_common_core_mp", "common_core_mp" });
        else Names.insert(Names.end(), { "techsets_common", "patch_common", "common" });
        std::vector<fs::path> Out;
        std::error_code Ec;
        for (const auto& N : Names)
            if (N != Zone && fs::exists(Dir / (N + ".ff"), Ec)) Out.push_back(Dir / (N + ".ff"));
        return Out;
    }

    // The weapon fields (IwWeaponDefs.h): the weapon asset is the WeaponDef itself (0xe20), at x64-zt's
    // H1 structs.hpp offsets (zonetool rebuilds these zones), checked on common_mp's weapons (README).
    // weapType, weapClass, fireType and the hit locations are named by the exe's tables (h1_mp64_flat
    // RVAs: 0x10b2460 entries 0, 6 and 37; 0x10b5370).
    const IwWeaponDefs::Layout& WeaponLayout()
    {
        static const IwWeaponDefs::Layout L = [] {
            IwWeaponDefs::Layout L;
            L.Name = 0, L.Display = 8;
            L.Types = { "none", "bullet", "grenade", "projectile", "riotshield", "energy" };
            L.Classes = { "rifle", "sniper", "mg", "smg", "spread", "pistol", "grenade", "rocketlauncher",
                          "turret", "throwingknife", "non-player", "item", "cone", "beam", "shield", "hover",
                          "cloak", "ping", "repulsor", "adrenaline", "health", "mute", "underwater", "ball" };
            L.FireTypes = { "Full Auto", "Single Shot", "2-Round Burst", "3-Round Burst", "4-Round Burst", "5-Round Burst", "Double Barrel" };
            constexpr int64_t Timers = 0x668;   // stateTimers (StateTimers, 0x12c)
            L.Fields = { { "weaponType", 0x53c }, { "weaponClass", 0x540 }, { "fireType", 0x554 }, { "startAmmo", 0x62c }, { "maxAmmo", 0x640 },
                         { "clipSize", 0x648 }, { "shotCount", 0x64c }, { "damage", 0x658 }, { "meleeDamage", 0x660 },
                         { "fireTime", Timers + 0x10 }, { "reloadTime", Timers + 0x34 }, { "reloadEmptyTime", Timers + 0x3c },
                         { "reloadAddTime", Timers + 0x40 }, { "reloadEmptyAddTime", Timers + 0x44 }, { "reloadStartTime", Timers + 0x48 },
                         { "reloadEndTime", Timers + 0x50 }, { "dropTime", Timers + 0x6c }, { "raiseTime", Timers + 0x70 },
                         { "quickRaiseTime", Timers + 0x80 }, { "firstRaiseTime", Timers + 0x84 }, { "sprintOutTime", Timers + 0x9c },
                         { "adsTransInTime", 0x9a0 }, { "adsTransOutTime", 0x9a8 },
                         { "explosionRadius", 0xa7c }, { "explosionRadiusMin", 0xa80 }, { "explosionInnerDamage", 0xa84 },
                         { "explosionOuterDamage", 0xa88 }, { "projectileSpeed", 0xa98 },
                         { "minDamage", 0xca4 }, { "maxDamageRange", 0xcb0 }, { "minDamageRange", 0xcb4 }, { "locationDamageMultipliers", 0x470 } };
            L.Notes = IwWeaponDefs::GameNotes("MW Remastered", "french/fra_code_post_gfx_mp", "common_mp",
                                              "the WeaponDef (the weapon asset itself) at the offsets of x64-zt's H1 structs.hpp",
                                              "M4 857 rpm, 30 rounds, 30-20 damage; AK-47 706 rpm, 40-30; MP5 800 rpm; M1014 8 pellets");
            L.Notes.Source += "The damage falls linearly from damage (0x658) at maxDamageRange (0xcb0) to minDamage at\n"
                              "minDamageRange (0xcb4) (h1_mp64_flat 0x2e5c50, through the getters 0x2e19c0 and 0x2e17d0 that take\n"
                              "a field's offset); that minDamage is the i32 at 0xca4, the one x64-zt calls midDamage (its 0xca0 is\n"
                              "read but not used there).\n";
            return L;
        }();
        return L;
    }

    // The anim owners (IwZoneAnimOwners.h): the weapon's view model, the first of its models array at
    // 0x18, its hands at 0x20 (the weapons' struct dumps); the multiplayer body and head, a player's of
    // the install (mp_shirt_003_tr, mp_head_003_tr: the Marines' SMG body and head); the zones a
    // zone loads with it
    const IwZoneAnimOwners::Layout& AnimOwnerLayout()
    {
        static const IwZoneAnimOwners::Layout L = [] {
            IwZoneAnimOwners::Layout L;
            L.Gun = { 0x18, 0 }, L.Hands = { 0x20 };
            L.Trees["multiplayer"] = { { "body_usmc_desert_smg_mp", "head_usmc_desert_smg_mp" },
                                       "the install's player zones mp_shirt_003_tr, mp_head_003_tr" };
            L.SharedMp = { "patch_common_mp", "common_mp", "patch_common_core_mp", "common_core_mp" };
            L.SharedSp = { "patch_common", "common" };
            L.LevelTrees = { "generic_human" };
            return L;
        }();
        return L;
    }

    class MwrZone final : public IWZone
    {
    public:
        MwrZone(const std::string& Path, std::string BookDir)
            : IWZone(Path), Books(MaterialLayout(), std::move(BookDir), [Path] { return BookZones(Path); })
        {
            using namespace DiskGame;
            Exports(Kind::Image, [this](const Context& C, Totals& T) { ExportImages(C, T); });
            Exports(Kind::Sound, [this](const Context& C, Totals& T) { ExportSounds(C, T); });
            Exports(Kind::Map, [this](const Context& C, Totals& T) { ExportMap(C, T); });
            ExportsScriptFiles(Scripts::GscToolBin("h1"));
            ExportsIwRawfiles();
            ExportsIwStringTables();
            ExportsIwWeapons(WeaponLayout());
            // the material and fx kinds: FxEffectDef 0x40, elements 0x140; Material's techniqueSet at
            // 0x128 (the h1 tables f36c7d0 and f375ba0, x64-zt's H1 structs.hpp)
            ExportsIwEffects(IwEffects::ModernWarfareRemastered(), GameModernWarfareRM::ReadXMaterial, 0x128);
        }
        // the material headers the books filled, and those they did not
        void Measures(DiskGame::Totals& T) const override
        {
            ZonekitZone::Measures(T);
            Books.Measure(T, "materials_from_books", "materials_in_no_book");
            if (Layers)
            {
                T.Counters["blend_layer_ids"] += Layers->Ids();
                T.Counters["blend_layer_ids_one_bit"] += Layers->Resolved();
            }
        }

    protected:
        const char* Profile() const override { return "h1"; }
        // its readers and Fixups work from the walk's instances: a partial walk serves them
        bool OpensPartially() const override { return true; }
        // a streamed image's stream records are the table's by its rank among the zone's images
        std::vector<std::string> Ranked() const override { return { "image" }; }
        const ICoDGame& LiveGame() const override { return LiveGames::Get("H1"); }
        VirtualGameSession::Handlers Handlers() override { return { SlotString, DiskImage }; }
        bool DerefAssetPointers() const override { return true; }
        void Fixups() override;
        void Served(bool On) override;
        // with the images
        void DefinesAt(std::vector<std::pair<DiskGame::AssetId, int64_t>>& Out) const override
        {
            ZonekitZone::DefinesAt(Out);
            DefineStreamed(Out, ImageNameAt, Loaded);
        }
        // a streamed image: its largest level (a resident one is its bytes in the zone)
        void Weigh(DiskGame::Kind K, int64_t Pos, DiskGame::Weight& W) const override
        {
            if (K == DiskGame::Kind::Image && Streams.count(Pos) && Pos + (int64_t)sizeof(MWRGfxImage) <= (int64_t)Image().size())
                W = DiskGame::Sizes::PakImageWeight(At<MWRGfxImage>(Image(), Pos));
        }
        // a stream entry's word: {u16 locale, u16 pak index, u32 checksum}
        uint64_t PakOf(const PakEntry& E) const override { return (E.Pak >> 16) & 0xffff; }
        bool ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models) override;
        // the string tables, then what the anim owners read (IwZoneAnimOwners.h)
        void Documents(std::vector<std::pair<std::string, std::string>>& Out) const override
        {
            ZonekitZone::Documents(Out);
            IwZoneAnimOwners::Documents(AnimOwnerLayout(), { &Image(), &Walk(), [this](uint64_t A) { return Off(A); },
                                                             [this](int64_t F) { return TextAt(F); },
                                                             [this](const std::string& T, int64_t P) { return NamedAsset(T, P); },
                                                             [this](int64_t P, std::string& N, std::vector<uint8_t>& B) {
                                                                 N = AssetName(DiskGame::Kind::Rawfile, P);
                                                                 return RawBytes(P, { 0xc, 0x10, 8 }, B);
                                                             },
                                                             [this](uint64_t A, uint64_t N, std::vector<uint8_t>& B) { return BytesAt(A, N, B); }, &Slots },
                                        Out);
        }
        // what Defines lists beyond the profile's walk: images, names by reference, model headers
        std::string IndexKey() const override { return ZonekitZone::IndexKey() + "_images_names1"; }
        // a model with no LOD and no bone, named in place (a weapon's view model): another zone
        // defines it
        bool IsReference(DiskGame::Kind K, const std::string& Name, int64_t Pos) const override
        {
            if (ZonekitZone::IsReference(K, Name, Pos)) return true;
            return K == DiskGame::Kind::Model && At<uint8_t>(Zone(), Pos + LodCountAt) == 0 && At<uint8_t>(Zone(), Pos + ModelBoneCountAt) == 0;
        }

    private:
        static std::unique_ptr<XImageDDS> DiskImage(const XImage_t& Image);

        std::unique_ptr<XImageDDS> Decode(int64_t Pos);
        std::unique_ptr<XImageDDS> DecodeAt(int64_t Pos, ImageUsageType Usage);
        bool HasPixels(int64_t Pos) const;
        void ExportImages(const DiskGame::Context& C, DiskGame::Totals& T);
        void ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T);
        std::vector<int64_t> Loaded;   // images whose pixels are in the zone
        IWMaterialBooks::Lookup Books;
        // the readers' lookup into Books while the zone is served (after Books: it ends first)
        std::optional<CoDAssets::Scoped<decltype(GameModernWarfareRM::ImagesElsewhere)>> ImagesServed;
        std::optional<GameModernWarfareRM::HooksScope> HooksServed;
        std::unique_ptr<LayerBits> Layers;   // the level blend ids, read from the first book on the first header that needs them
    };

    std::unique_ptr<XImageDDS> MwrZone::DiskImage(const XImage_t& Image)
    {
        MwrZone* Current = ServedZone<MwrZone>();
        if (Current == nullptr) return nullptr;
        return Current->DecodeAt((int64_t)(Image.ImagePtr - VirtualGameSession::Base), Image.ImageUsage);
    }

    bool MwrZone::HasPixels(int64_t Pos) const
    {
        return Off(PointerAt(Pos + ImageDataAt)) >= 0 && At<uint32_t>(Image(), Pos + ImageDataSizeAt) != 0
            && At<uint16_t>(Image(), Pos + ImageWidthAt) != 0 && At<uint16_t>(Image(), Pos + ImageHeightAt) != 0;
    }

    std::unique_ptr<XImageDDS> MwrZone::DecodeAt(int64_t Pos, ImageUsageType Usage)
    {
        if (Pos < 0 || Pos + ImageNameAt + 8 > (int64_t)Image().size()) return nullptr;
        const auto Info = CoDAssets::GameInstance->Read<MWRGfxImage>(VirtualGameSession::Address((uint64_t)Pos));
        if (const auto It = Streams.find(Pos); It != Streams.end())
        {
            uint32_t Width = 0, Height = 0;
            const PakEntry& E = It->second[GameReaders::LargestLevel(Info, Width, Height)];
            if (E.End <= E.Start) return nullptr;
            const std::string Pak = (InstallRoot(FastFilePath()) / std::format("imagefile{}.pak", (unsigned)PakOf(E))).string();
            uint32_t Size = 0;
            auto Pixels = PAKSupport::AWExtractImagePackage(Pak, E.Start, E.End - E.Start, Size);
            return GameReaders::TranslatePakImage(std::move(Pixels), Size, Info.ImageFormat, Info.MapType, Width, Height, Usage);
        }
        if (!HasPixels(Pos)) return nullptr;
        const int64_t Data = Off(PointerAt(Pos + ImageDataAt));
        const uint32_t Size = At<uint32_t>(Image(), Pos + ImageDataSizeAt);
        if (Data + (int64_t)Size > (int64_t)Image().size()) return nullptr;
        auto Pixels = std::make_unique<uint8_t[]>(Size);
        std::memcpy(Pixels.get(), Image().data() + Data, Size);
        return GameReaders::TranslatePakImage(std::move(Pixels), Size, Info.ImageFormat, Info.MapType, At<uint16_t>(Image(), Pos + ImageWidthAt),
                                                 At<uint16_t>(Image(), Pos + ImageHeightAt), Usage);
    }

    void MwrZone::Served(bool On)
    {
        ImagesServed.reset();
        HooksServed.reset();
        if (!On) return;
        ImagesServed.emplace(GameModernWarfareRM::ImagesElsewhere, [this](const std::string& Header, std::vector<std::pair<uint32_t, std::string>>& Out) {
            return Books.Find(!Header.empty() && Header[0] == ',' ? Header.substr(1) : Header, Out);
        });
        GameModernWarfareRM::Hooks H;
        H.Details = [this](const std::string& Name, GameModernWarfareRM::Details& Out) {
            IWMaterialBooks::Extra E;
            if (!Books.Details(Name, E)) return false;
            if (!Layers) Layers = MakeLayers(Books);
            Out.Techset = E.Techset;
            Out.SortKey = E.SortKey;
            Out.SurfaceBits = E.SurfaceBits;   // the blend's first layer is resolved once, for headers and defined materials alike (ResolveBlendSurface)
            Out.Constants = E.Constants;
            return true;
        };
        H.Blend = [this](const std::string& Name, int64_t Word, std::string& Via) {
            if (!Layers) Layers = MakeLayers(Books);
            return Layers->Surface(Name, Word, &Via);
        };
        HooksServed.emplace(std::move(H));
    }

    void MwrZone::Fixups()
    {
        Served(true);   // opened: the zone the handlers reach
        Slots = HeaderScriptStrings();
        const auto& Z = Zone();
        LinkLodSurfaces(ModelLods());
        ReadStreams(ImageStreamedAt);
        for (const auto& I : Walk().Instances)
            if (OwnerIs(I, "image") && At<uint8_t>(Z, I.Pos + ImageStreamedAt) == 0 && HasPixels(I.Pos)) Loaded.push_back(I.Pos);
        Counter("images_loaded") += Loaded.size();
    }

    std::unique_ptr<XImageDDS> MwrZone::Decode(int64_t Pos)
    {
        const auto Usage = At<uint8_t>(Zone(), Pos + ImageFormatAt) == 84 ? ImageUsageType::NormalMap : ImageUsageType::DiffuseMap;
        return DecodeAt(Pos, Usage);
    }

    void MwrZone::ExportImages(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        ImageList L;
        // the streamed ones in pak order, then the loaded ones in zone order
        L.Order = StreamOrder();
        L.Order.insert(L.Order.end(), Loaded.begin(), Loaded.end());
        L.NameAt = ImageNameAt;
        // what the output file is made of: the four pak entries (or the zone's pixels), the format
        // and the extension
        L.KeyOf = [this](int64_t Pos, const std::string& Extension) {
            ExportWriters::ContentKey Key;
            if (Streams.count(Pos))
                MixStreams(Pos, Key);
            else
            {
                const int64_t Data = Off(PointerAt(Pos + ImageDataAt));
                const uint32_t Size = At<uint32_t>(Image(), Pos + ImageDataSizeAt);
                for (uint32_t K = 0; Data >= 0 && K < Size && Data + K < (int64_t)Image().size(); K++) Key.Mix(Image()[(size_t)(Data + K)]);
                Key.Mix(At<uint32_t>(Image(), Pos + ImageWidthAt));
            }
            return Key.Mix(At<uint8_t>(Zone(), Pos + ImageFormatAt)).Mix(Extension).Key();
        };
        L.Decode = [this](int64_t Pos, const std::string&) { return Decode(Pos); };
        ExportImageList(C, T, L);
    }

    // snd_alias_t (0xf8, zonetool's H1 Structs.hpp): subtitle 8, secondary 0x10, SoundFile 0x20,
    // volumes 0x44/0x48, pitches 0x50/0x54, distances 0x58/0x5c, flags 0x64 (looping 0, spatializedIs3D 7),
    // probability 0x78, center 0x90, start delay 0x94, envelope 0xa0/0xa4, allowDoppler 0xe4; the list's
    // count a byte at 0x18; a primed SoundFile's LoadedSound at 8
    IWSoundAliases::Layout AliasLayout()
    {
        IWSoundAliases::Layout L;
        L.ListCount = 0x18, L.ListCountBytes = 1, L.Size = 0xf8;
        L.Subtitle = 8, L.Secondary = 0x10, L.SoundFile = 0x20, L.VolMin = 0x44, L.VolMax = 0x48, L.PitchMin = 0x50, L.PitchMax = 0x54;
        L.DistMin = 0x58, L.DistMax = 0x5c, L.Flags = 0x64, L.Is3DBit = 7, L.Probability = 0x78, L.CenterPercentage = 0x90;
        L.StartDelay = 0x94, L.EnvelopMin = 0xa0, L.EnvelopMax = 0xa4, L.AllowDoppler = 0xe4;
        L.PrimedLoaded = 8;
        return L;
    }

    void MwrZone::ExportSounds(const DiskGame::Context& C, DiskGame::Totals& T)
    {
        // loaded sounds are in the zone, streamed and primed ones FLAC in soundfile<N>.pak
        // (french/fra_soundfile<N>.pak when localized); an empty file is a failure
        ExportPakSounds(C, T, InstallRoot(FastFilePath()).string(), true, AliasLayout(),
                        [](uint64_t Address, const std::string& Name, std::set<uint64_t>& Unique, std::vector<CoDAsset_t*>& Found) {
                            GameModernWarfareRM::AddAliasSounds(CoDAssets::GameInstance->Read<MWRSoundAlias>(Address), Name, Unique, Found);
                        });
    }

    // The level (the h1 tables): GfxWorld (gfx_map, 0xb30) with GfxWorldDraw at 0xb0 and
    // GfxWorldDpvsStatic at 0x748 (surfaces at +0x298, static models 0x50 each at +0x2a8), 96-byte
    // brush models, surfaces whose indices tile the index array (triangle count 18, first index
    // 20); ComWorld's primary lights 0x90 each; col_map_mp's dynamic entities 0x88 each
    IWZone::MapLayout Level()
    {
        IWZone::MapLayout M;
        M.VertexCount = 0x130, M.Vertices = 0x138, M.IndexCount = 0x170, M.Indices = 0x178;
        M.BrushCount = 0x5e8, M.Brushes = 0x5f0, M.BrushSize = 96, M.BrushFirstSurface = 84, M.BrushSurfaceCount = 88;
        M.SModelCount = 0x748, M.Surfaces = 0x9e0, M.SModels = 0x9f0, M.SModelSize = 0x50;
        M.SurfaceTris = 18, M.SurfaceBaseIndex = 20;
        M.Clip = "col_map_mp", M.DynSize = 0x88;
        M.Lights.Size = 0x90, M.Lights.Color = 8, M.Lights.Dir = 20, M.Lights.Origin = 44, M.Lights.Radius = 80, M.Lights.CosHalfFov = 84;
        // the compiled record (mwr-materials.md 6.1): the up vector at 32 and the definition name pointer at 0x88; the colour is HDR and intensity premultiplied
        // (channel / 15500.03 is 0.5..90 on 188 records of four maps: tested, the unit a game constant). Whether that is BO3 stops is not proven: the divided colour
        // is written under an unresolved key, never as BO3's colour or intensity.
        M.Lights.Up = 32, M.Lights.Def = 0x88, M.Lights.ColorUnit = 15500.03f, M.Lights.ColorKey = "color_div_15500_03";
        M.ImageNameAt = ImageNameAt;
        // the reflection probes (mwr-materials.md 7; the records read from mp_crash: count 22 at +0xb0, the records pointer at +0xc0, 0x20 bytes each, origin first, record 0 all zero)
        M.ProbeCount = 0xb0, M.ProbeRecords = 0xc0, M.ProbeStride = 0x20;
        // the layers of a blend: the world's vertex layer data (size u32 at gfx_map +0x150, pointer at +0x158) and a surface's slice offset (u32 at 0) and vertex count (u16 at 16)
        // (mwr-materials.md 12.3); the shape and the decoding are Games/H1/BlendLayers.h's
        M.Blend.DataSize = 0x150, M.Blend.Data = 0x158, M.Blend.SurfaceSlice = 0, M.Blend.SurfaceVertices = 16;
        M.Blend.Shape = [](const std::vector<uint32_t>& Semantics, const std::string& Name, uint32_t& Layers, uint32_t& Stride, uint32_t& Code) {
            const H1Blend::Shape S = H1Blend::ShapeOf(Semantics, Name);
            Layers = S.Layers, Stride = S.Stride(), Code = S.Layers | (S.Rotations << 8);
            return S.Layers != 0;
        };
        M.Blend.Decode = [](ByteView Slice, uint32_t Code, const uint8_t Color[4], Maps::BlendVertex& Out) {
            H1Blend::Shape S;
            S.Layers = Code & 0xff, S.Rotations = Code >> 8;
            H1Blend::Decode(Slice, S, Color, Out);
        };
        M.EntityKeys = H1Keys::EntityKeys;
        M.ClipBrushes = IWZone::AwClipBrushes(), M.Triggers = 0x18;   // Advanced Warfare's shapes; MapEnts' MapTriggers at 0x18
        // The partitions add a vertex segment to a triangle's u16 numbers (Modern Warfare Remastered, measured on mp_crash: 16-byte records, triangle count u8, border count u8,
        // segment u16 at 2, first triangle at 4; 12,580 partitions, 154,742 triangles, 93,910 vertices, last segment 27, base = segment x 1024: every number is then in range and
        // every vertex used). Advanced Warfare (S1) shares the rest of the layout but this was not measured on an S1 zone, so S1 keeps reading segment 0.
        M.ClipBrushes.PartitionCount = 0x10 + 0xd8, M.ClipBrushes.Partitions = 0x10 + 0xe0, M.ClipBrushes.PartitionSize = 0x10, M.ClipBrushes.SegmentVertices = 1024, M.ClipBrushes.PartitionSegmentBytes = 2;
        // the AABB tree: 24,449 records of 32 bytes at ClipInfo +0x48 (count at +0x40) on mp_crash; midpoint, material u16 at 12, child count u16 at 14, half size, partition at 28
        M.ClipBrushes.TreeCount = 0x10 + 0x40, M.ClipBrushes.Trees = 0x10 + 0x48, M.ClipBrushes.TreeSize = 0x20, M.ClipBrushes.TreeMaterial = 0xc, M.ClipBrushes.TreeChildren = 0xe, M.ClipBrushes.TreePartition = 0x1c;
        return M;
    }

    // The createfx script's facts for this game (MapCreateFx.h): measured on patch_mp_crash, checked on mp_bog and mp_crossfire (m4.md). The scriptfile's header holds three u32
    // 32, 28 and 24 bytes before the name; every placement's anchor string follows the byte 0x8f; a vector literal is 0x56 and three floats, a pair follows 0x89 and is followed
    // by the call 0x6f 0x9e; a field-set instruction is 88 00 52 42 9c 97 5b (1 per loop sound, 2 per effect, 3 per delayed sound on mp_crash: 230 of 230 groups).
    static Maps::CreateFxLayout H1CreateFxLayout()
    {
        Maps::CreateFxLayout L;
        L.ScriptPrefix = "maps/createfx/", L.ScriptSuffix = "_fx";
        L.HeaderBack = { 32, 28, 24 };
        L.AnchorPrefix = 0x8f;
        L.LoopNext = "soundalias";
        L.DelayAnchor = "~delay_min", L.DelayNext = "delay_max", L.DelayAfter = "soundalias";
        // optional effect keys after "fxid", "delay" (mp_bog): sizescale alone is +1 (3 groups of 3); tintcolor, tintalpha and emissive together (with sizescale) give 6 field sets in 2 groups, so +3 for the three. How the 3 divides among them is not known: only the whole combination is tied, any subset fails closed and is named.
        L.FxNext = "fxid", L.FxDelay = "delay", L.FxExtraKeys = { { "sizescale", 1 } }, L.FxKeyCombos = { { { "tintcolor", "tintalpha", "emissive" }, 3 } };
        L.VectorOp = 0x56, L.PairPrefix = 0x89, L.Call = { 0x6f, 0x9e };
        L.FieldSet = { 0x88, 0x00, 0x52, 0x42, 0x9c, 0x97, 0x5b };
        L.FieldSetsLoop = 1, L.FieldSetsDelay = 3, L.FieldSetsFx = 2;
        L.AngleLimit = 720.0f;
        return L;
    }

    // The sky's brightness calibration of this game (zonekit/profiles/h1/target/sky.json, target data): one factor for every MWR map, so the maps keep their levels relative to each other.
    // The file is found beside the exe or in a checkout above it. A missing file is a reported 1.
    static double H1SkyCalibration(std::string& Source)
    {
        wchar_t Buf[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, Buf, static_cast<DWORD>(std::size(Buf)));
        std::error_code Ec;
        for (fs::path P = fs::path(Buf).parent_path(); !P.empty(); P = P.parent_path())
        {
            const fs::path File = P / "zonekit" / "profiles" / "h1" / "target" / "sky.json";
            if (fs::exists(File, Ec))
            {
                std::ifstream In(File, std::ios::binary);
                std::stringstream Text;
                Text << In.rdbuf();
                try
                {
                    const nlohmann::json J = nlohmann::json::parse(Text.str());
                    const double Scale = J.at("exr_scale").get<double>();
                    Source = J.value("basis", std::string());
                    if (std::isfinite(Scale) && Scale > 0) return Scale;
                }
                catch (const nlohmann::json::exception&)
                {
                }
                Source = "sky.json unreadable";
                return 1.0;
            }
            if (P == P.parent_path()) break;
        }
        Source = "zonekit/profiles/h1/target/sky.json not found";
        return 1.0;
    }

    bool MwrZone::ReadMap(Maps::MapScene& Scene, std::map<std::string, uint64_t>& Models)
    {
        const bool Ok = ReadLevel(Scene, Models, Level(), GameModernWarfareRM::ReadXMaterial);
        // one scene material per distinct layer art of the level's blends
        {
            if (!Layers) Layers = MakeLayers(Books);
            std::map<std::string, size_t> Made;
            const size_t N = H1Blend::AddLayerMaterials(Scene, [this](const std::string& Id, std::string& Via) { return Layers->OfId(Id, &Via); }, &Made);
            Counter("map_blend_layer_materials") += N;
            for (const auto& [Name, V] : Made) Counter(Name) += V;
        }
        {
            std::map<std::string, size_t> Interp;
            Maps::MeasureBlendInterpolation(Scene, Interp);
            for (const auto& [Name, N] : Interp) Counter(Name) += N;
        }
        // evidence for the blend reader's field positions that does not rest on the slice size (counters map_blend_uv_*, map_blend_shared_*, map_blend_smooth_*)
        if (GetEnvironmentVariableA("GH_BLEND_EVIDENCE", nullptr, 0) > 0)   // a measurement, off by default
        {
            std::map<std::string, size_t> Evidence;
            Maps::MeasureBlendEvidence(Scene, [](const std::string& Material, uint32_t Layer) {
                const std::vector<std::string> Ids = H1Layers::IdsOf(Material);
                return Layer < Ids.size() ? Ids[Layer] : std::string();
            }, Evidence);
            for (const auto& [Name, N] : Evidence) Counter(Name) += N;
        }
        // M4d: the effects and sounds the map's createfx script places. The script is in the map's patch zone (patch_<map>.ff beside the map's), not in the map's: a compiled
        // scriptfile whose string stack and vector literals give the placements in code order (MapCreateFx.h; the pairing is by order, not a decoded call).
        {
            const fs::path Own = FastFilePath();
            const fs::path Patch = Own.parent_path() / ("patch_" + Own.stem().string() + ".ff");
            std::error_code Ec;
            if (!fs::exists(Patch, Ec))
                Counter("map_createfx_no_patch_zone")++;
            else
            {
                std::vector<uint8_t> PatchZone, Stack, Code;
                std::string Why;
                if (!FastFile::Decompress(Patch.string(), PatchZone, Why))
                    Counter("map_createfx_patch_zone_unreadable")++;
                else if (const Maps::CreateFxLayout Layout = H1CreateFxLayout(); !Maps::FindScriptFile(Layout, PatchZone, Layout.ScriptPrefix + Own.stem().string() + Layout.ScriptSuffix, Stack, Code, Why))
                    Counter("map_createfx_script_not_found")++;
                else
                {
                    // the map's collision bounds, padded, say where an origin can be
                    Maps::CreateFxBounds Bounds;
                    std::array<float, 3> Lo{}, Hi{};
                    if (Maps::CollisionBounds(Scene, Lo, Hi))
                    {
                        Bounds.Valid = true;
                        for (size_t A = 0; A < 3; A++) Bounds.Lo[A] = Lo[A] - 512.0f, Bounds.Hi[A] = Hi[A] + 512.0f;
                    }
                    else
                        Counter("map_createfx_no_bounds")++;
                    Maps::CreateFxResult Fx = Maps::ParseCreateFx(Layout, Stack, Code, Bounds);
                    Counter("map_createfx_entries") += Fx.Entries;
                    Counter("map_createfx_vector_pairs") += Fx.VectorPairs;
                    if (Fx.Unparsed) Counter("map_createfx_anchors_unparsed") += Fx.Unparsed;
                    for (std::string Key : Fx.UnknownKeys)
                    {
                        for (char& C : Key)
                            if (!std::isalnum(static_cast<unsigned char>(C))) C = '_';
                        Counter("map_createfx_unknown_key_" + Key)++;
                    }
                    if (Fx.Unties) Counter("map_createfx_groups_untied") += Fx.Unties;
                    for (std::string Group : Fx.UntiedGroups)
                    {
                        for (char& C : Group)
                            if (!std::isalnum(static_cast<unsigned char>(C))) C = '_';
                        Counter("map_createfx_untied_" + Group)++;
                    }
                    if (Fx.Mismatch) Counter("map_createfx_file_rejected")++;
                    if (Fx.Outside) Counter("map_createfx_origin_outside_bounds") += Fx.Outside;
                    if (Fx.BadAngles) Counter("map_createfx_angles_out_of_range") += Fx.BadAngles;
                    // an independent look at the origins: how many lie within 64 and within 256 units of a collision vertex (the soup and the brushes' corners). A placement in the air
                    // far from every surface is not proof of an error, a high share near one is evidence the origins are real positions.
                    {
                        std::vector<float> Corners = Scene.Collision.Vertices;
                        for (const Maps::Brush& B : Scene.Collision.Brushes) Corners.insert(Corners.end(), B.Vertices.begin(), B.Vertices.end());
                        size_t Near64 = 0, Near256 = 0;
                        for (const Maps::EffectPlacement& E : Fx.Placements)
                        {
                            double Best = 1e30;
                            for (size_t K = 0; K + 2 < Corners.size(); K += 3)
                            {
                                const double Dx = Corners[K] - E.Origin[0], Dy = Corners[K + 1] - E.Origin[1], Dz = Corners[K + 2] - E.Origin[2];
                                Best = std::min(Best, Dx * Dx + Dy * Dy + Dz * Dz);
                            }
                            Near64 += Best <= 64.0 * 64.0, Near256 += Best <= 256.0 * 256.0;
                        }
                        Counter("map_createfx_placements_kept") += Fx.Placements.size();
                        Counter("map_createfx_near_collision_vertex_64") += Near64;
                        Counter("map_createfx_near_collision_vertex_256") += Near256;
                    }
                    Scene.Effects = std::move(Fx.Placements);
                }
            }
        }
        // the sun (M4b): the com_map light of type 1 (colour HDR, direction toward the sun: mp_crash (-0.321, -0.557, 0.766) = the worldspawn sundirection angles (-50, 240))
        for (const Maps::Light& L : Scene.Lights)
            if (L.Kind == Maps::Light::Type::Sun)
            {
                Scene.Sun.Present = true;
                for (int A = 0; A < 3; A++) Scene.Sun.Toward[A] = L.Dir[A], Scene.Sun.Color[A] = L.Color[A];
                break;
            }
        // M4e: the sky. The sky image is a cube map (BC6H, six faces); the sky shader of the game (wc_sky_cso_nfwpf, read from the techset zones) samples it with the world
        // direction and multiplies the texel by the material's first constant (hdrColorParm: mad texel, cb3[0].x, cb3[0].y; the material holds 30 0 0 0). The cube is resampled
        // by direction into the lat-long a Black Ops III sky material holds, the texels times that multiplier, and written as an EXR (SkyLatLong.h, ExrWriter.h).
        Scene.SealWithSkyShell = true;   // MWR: its export has only clip and caulk brushes, so it is not enclosed (m4e.md 3)
        if (Scene.Sky.ReaderImage >= 0)
        {
            const std::unique_ptr<XImageDDS> Dds = DecodeAt(Scene.Sky.ReaderImage, ImageUsageType::DiffuseMap);
            SkyConv::Cube Cube;
            std::string Format, Why;
            if (!Dds || Dds->DataBuffer == nullptr || Dds->DataSize == 0)
                Counter("map_sky_image_unreadable")++;
            else if (!SkyConv::DecodeCubeDds(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(Dds->DataBuffer), Dds->DataSize), Cube, Format, Why))
            {
                Counter("map_sky_cube_not_decoded")++;
                Scene.Sky.LatLongNote = "the sky image is not a cube this reader decodes: " + Why + " (" + Format + ")";
            }
            else
            try
            {
                // the multiplier: the constant named hdrColorParms of the sky material (MWR's name hash, mwr-materials.md 2; 30 0 0 0 on mp_crash), its x
                const uint32_t HdrColorParm = H1Blend::NameHash("hdrColorParms");
                float Multiplier = 1.0f;
                bool FoundMultiplier = false;
                for (const auto& [Name, Material] : Scene.Materials)
                {
                    if (Material.Techset.rfind("wc_sky", 0) != 0) continue;
                    for (const auto& Block : Material.Constants)
                        for (size_t At = 0; At + 32 <= Block.size() && !FoundMultiplier; At += 32)
                        {
                            std::array<float, 4> V{};
                            uint32_t Hash = 0;
                            std::memcpy(&Hash, std::span<const uint8_t>(Block).subspan(At, 4).data(), 4);
                            std::memcpy(V.data(), std::span<const uint8_t>(Block).subspan(At + 16, sizeof(V)).data(), sizeof(V));
                            if (Hash == HdrColorParm && std::isfinite(V[0]) && V[0] > 0.0f) Multiplier = V[0], FoundMultiplier = true;
                        }
                }
                Counter(FoundMultiplier ? "map_sky_multiplier_read" : "map_sky_multiplier_hdrcolorparm_not_found")++;
                const SkyConv::Peak Peak = SkyConv::Brightest(Cube);
                if (!Peak.Found) Counter("map_sky_cube_black")++;
                const SkyConv::CubeConvention Convention;   // identity: the shader samples the cube with the world direction
                double World[3];
                Convention.ToWorld(Peak.Dir, World);
                Counter("map_sky_face_size") += static_cast<size_t>(Cube.Size);
                Counter("map_sky_peak_x1000") += static_cast<size_t>(Peak.MaxChannel * 1000.0);
                Counter("map_sky_mean_x1000000") += static_cast<size_t>(Peak.Mean * 1e6);
                Counter("map_sky_multiplier_x1000") += static_cast<size_t>(Multiplier * 1000.0f);
                Counter("map_sky_peak_face") += static_cast<size_t>(Peak.Face);
                std::string Angle = "no sun";
                if (Scene.Sun.Present)
                {
                    const double Toward[3] = { Scene.Sun.Toward[0], Scene.Sun.Toward[1], Scene.Sun.Toward[2] };
                    const double Deg = SkyConv::AngleBetween(World, Toward);
                    Counter("map_sky_peak_to_sun_centidegrees") += static_cast<size_t>(Deg * 100.0);
                    Angle = std::format("{:.2f} degrees from the sun", Deg);
                }
                // the brightest texel above the horizon: a bake that mirrors the sky below it has the global peak in the mirror, so the sun's own disc is found in the upper hemisphere
                if (Scene.Sun.Present)
                {
                    const SkyConv::Peak Upper = SkyConv::Brightest(Cube, Convention, 0.0);
                    double UpperWorld[3] = { 0, 0, 0 };
                    if (!Upper.Found) Counter("map_sky_no_texel_above_horizon")++;
                    Convention.ToWorld(Upper.Dir, UpperWorld);
                    const double Toward[3] = { Scene.Sun.Toward[0], Scene.Sun.Toward[1], Scene.Sun.Toward[2] };
                    const double Deg = SkyConv::AngleBetween(UpperWorld, Toward);
                    if (Upper.Found)
                    {
                        Counter("map_sky_upper_peak_to_sun_centidegrees") += static_cast<size_t>(Deg * 100.0);
                        Counter("map_sky_upper_peak_x1000") += static_cast<size_t>(Upper.Luminance * 1000.0);
                        Angle += std::format("; brightest texel above the horizon {:.1f} at ({:.3f}, {:.3f}, {:.3f}), {:.2f} degrees from the sun", Upper.Luminance, UpperWorld[0], UpperWorld[1], UpperWorld[2], Deg);
                    }
                    else
                        Angle += "; no texel above the horizon is lit";
                }
                std::string CalibrationBasis;
                const double Calibration = H1SkyCalibration(CalibrationBasis);
                if (CalibrationBasis.rfind("zonekit", 0) == 0 || CalibrationBasis == "sky.json unreadable") Counter("map_sky_calibration_default")++;
                const float Applied = static_cast<float>(Multiplier * Calibration);
                int W = 0, H = 0;
                SkyConv::LatLongSize(Cube.Size, W, H);
                std::vector<uint16_t> Half;
                std::vector<uint8_t> Exr;
                if (!SkyConv::ResampleToLatLong(Cube, Convention, Applied, W, H, Half, Why) || !Exr::WriteHalfRgb(W, H, Half, Exr::WriteCompression::Zips, Exr, Why))
                {
                    Counter("map_sky_not_converted")++;
                    Scene.Sky.LatLongNote = "the sky cube was not converted: " + Why;
                }
                else
                {
                    // the mean luminance of the upper half of the lat-long, before and after the calibration: the figure the calibration is set from (the stock day skies' upper mean is 0.377)
                    double UpperSum = 0;
                    for (size_t K = 0; K < static_cast<size_t>(W) * static_cast<size_t>(H / 2); K++)
                        UpperSum += 0.2126 * Exr::HalfToFloat(Half[K * 3]) + 0.7152 * Exr::HalfToFloat(Half[K * 3 + 1]) + 0.0722 * Exr::HalfToFloat(Half[K * 3 + 2]);
                    const double UpperOut = UpperSum / (static_cast<double>(W) * static_cast<double>(H / 2));
                    Counter("map_sky_upper_mean_before_calibration_x1000000") += static_cast<size_t>(UpperOut / Calibration * 1e6);
                    Counter("map_sky_upper_mean_written_x1000000") += static_cast<size_t>(UpperOut * 1e6);
                    Scene.Sky.LatLongExr = std::move(Exr);
                    Scene.Sky.LatLongWidth = W, Scene.Sky.LatLongHeight = H, Scene.Sky.ExrScale = Applied;
                    Scene.Sky.UpperMeanWritten = static_cast<float>(UpperOut);
                    Scene.Sky.CalibrationBasis = CalibrationBasis;
                    Scene.Sky.LatLongNote = std::format("{} ({}); brightest texel {:.1f} (largest channel {:.1f}, mean luminance {:.4g}) on face {} at world direction ({:.3f}, {:.3f}, {:.3f}), {}; texels times {}",
                                                        Format, Scene.Sky.Image, Peak.Luminance, Peak.MaxChannel, Peak.Mean, Peak.Face, World[0], World[1], World[2], Angle, Multiplier);
                    Counter("map_sky_latlong_written")++;
                    Scene.Images.erase(Scene.Sky.Image);   // the cube's PNG is of no use now: nothing reads it
                }
            }
            catch (const std::bad_alloc&)
            {
                // the cube or the lat-long did not fit in memory: a reported failure, the export goes on without a sky
                Counter("map_sky_out_of_memory")++;
                Scene.Sky.LatLongExr.clear();
                Scene.Sky.LatLongNote = "the sky cube was not converted: out of memory";
            }
        }
        // M4a: a blend or layer whose surface word still holds several bits. The clip material's surface index is bits 20..24 of its surface word and equals the render surface's
        // bit number plus one (tested on mp_crash 96.1%, mp_crossfire 96.5%, mp_bloc 97.6%: m2c.md 1); a bit wins only by the thresholds in MapSurfaceFromCollision.h
        {
            Maps::SurfaceFromCollisionConfig Rule;
            Rule.Enabled = true, Rule.Shift = 20, Rule.Mask = 0x1f, Rule.Offset = -1, Rule.MinShare = 0.9, Rule.MinTriangles = 8;
            std::map<std::string, size_t> Resolved;
            Maps::ResolveSurfaceFromCollision(Scene, Rule, Resolved);
            for (const auto& [Name, N] : Resolved) Counter(Name) += N;
        }
        // the tool materials this game's clip contents name, as data of the profile (flag_names.txt, clip_tool rows)
        if (const SurfaceTypes::Names* Names = SurfaceTypes::For("h1"))
            for (const auto& Row : Names->ClipTools())
            {
                Scene.Collision.ClipToolByContents[Maps::ClipToolKey(Row.Surface, Row.Contents)] = Row.Tool;
                if (!Row.Approximation.empty()) Scene.Collision.ClipToolApproximation[Maps::ClipToolKey(Row.Surface, Row.Contents)] = Row.Approximation;
            }
        return Ok;
    }

    class MwrGame final : public DiskGame::IDiskGame
    {
    public:
        const char* Id() const override { return "mwr"; }
        const char* Title() const override { return "Call of Duty: Modern Warfare Remastered"; }
        const char* Engine() const override { return "H1"; }
        std::vector<std::string> Aliases() const override { return { "h1" }; }
        std::unique_ptr<DiskGame::IDiskZone> Unopened() const override { return std::make_unique<MwrZone>(std::string(), std::string()); }
        // the patch's strings, the language's own (fra_patch_) after the install's: multiplayer
        // maps by MPUI_ (mp_crash_snow "Winter Crash"), the campaign by its rich presence
        // PRESENCE_SP_ (ac130 "Death from Above")
        const LevelNaming* LevelNames() const override
        {
            static const LevelNaming Naming{ { "patch_code_post_gfx", "patch_code_post_gfx_mp", "???_patch_code_post_gfx",
                                               "???_patch_code_post_gfx_mp" },
                                             { { "mp_*", { "MPUI_{REST}", "PRESENCE_{NAME}" } },
                                               { "*", { "PRESENCE_SP_{NAME}", "LUA_MENU_MAPNAME_{NAME}" } } } };
            return &Naming;
        }

        // the anims' weapons, trees and animclasses (IwZoneAnimOwners.h)
        std::vector<std::string> AnimOwnerDocuments() const override { return IwZoneAnimOwners::Patterns(); }
        void AnimOwners(const DiskGame::ExportOptions& Options, const std::vector<AnimZone>& Zones, ::AnimOwners::Book& Out) const override
        {
            IwZoneAnimOwners::Fill(*this, AnimOwnerLayout(), Options, Zones, Out);
        }

        std::unique_ptr<DiskGame::IDiskZone> Open(const std::string& Path, const DiskGame::ExportOptions& Options, std::string& Error) const override
        {
            const fs::path Cache = Options.Cache.empty() ? fs::temp_directory_path() / "greyhound_mwr" : fs::path(Options.Cache);
            auto Zone = std::make_unique<MwrZone>(Path, (Cache / "material_books").string());
            if (!Zone->Open(Error)) return nullptr;
            return Zone;
        }
    };

    const MwrGame Game;
    const DiskGame::Registration Registered(Game);
}
