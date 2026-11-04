#include "stdafx.h"

// A model's name-only material headers (",name": no image slot), given the images of the material
// they stand for: this zone's own, or another zone's (Context::MaterialsElsewhere, the install
// index's), read by the game's live material reader (ZonekitZone::ReadsMaterials). The game swaps
// a header for that material once loaded; the readers, reading the header, see no image.

#include "ZonekitZone.h"

#include "CoDAssetType.h"
#include "ExportNaming.h"
#include "ExportWriters.h"

#include <exception>
#include <unordered_map>

namespace
{
    // a material as the readers name it (",mc/mtl_x" and "mtl_x" alike): no ',', no folder, lower case
    std::string Key(std::string N)
    {
        if (!N.empty() && N[0] == ',') N.erase(0, 1);
        if (const auto Slash = N.find_last_of("/\\"); Slash != std::string::npos) N.erase(0, Slash + 1);
        if (!N.empty() && N[0] == ',') N.erase(0, 1);
        return ExportNaming::Lower(N);
    }
}

bool ZonekitZone::MaterialImages(DiskGame::MaterialBook& Out)
{
    using DiskGame::Kind;
    const auto& Read = MaterialOf ? MaterialOf : SlotsOf;
    const char* const Type = MaterialType != nullptr ? MaterialType : "material";
    if (!Read || !Session) return false;
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, Type)) continue;
        const std::string N = AssetName(Kind::Material, I.Pos);
        Counter("material_instances")++;
        if (N.empty() || IsReference(Kind::Material, N, I.Pos))
        {
            Counter(N.empty() ? "material_instances_unnamed" : "material_instances_reference")++;
            continue;
        }
        // (a name met twice: the instance that has images counts, whichever came first)
        if (const auto Known = Out.find(N); Known != Out.end() && !Known->second.empty()) continue;
        XMaterial_t M(0);
        try
        {
            M = Read(Live().Addr((uint64_t)I.Pos));
        }
        catch (const std::exception&)
        {
            continue;
        }
        auto& Images = Out[N];
        for (const XImage_t& Image : M.Images)
        {
            std::string Name = Image.ImageName;
            if (!Name.empty() && Name[0] == ',') Name.erase(0, 1);
            if (!Name.empty()) Images.push_back({ (uint8_t)Image.ImageUsage, Image.SemanticHash, std::move(Name) });
        }
    }
    return true;
}

void ZonekitZone::ImageSlotsByLink(const DiskGame::MaterialBook& Known, DiskGame::MaterialBook& Out) const
{
    using DiskGame::Kind;
    if (SemanticAt < 0) return;
    std::set<uint32_t> Hashes;
    std::set<std::string> Named;
    for (const auto& [Material, Images] : Known)
        for (const auto& I : Images) Hashes.insert(I.Hash), Named.insert(ExportNaming::Lower(I.Image));
    // data -> every pointer field to it: an image met through several fields has a slot for each, and a
    // first-only map would hide the one that disagrees with the others (a role conflict)
    std::unordered_multimap<int64_t, int64_t> FieldOf;
    FieldOf.reserve(R.Links.size());
    for (const auto& [Field, Data] : R.Links) FieldOf.emplace(Data, Field);
    auto& Loose = Out[std::string()];
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, ImageType())) continue;
        std::string Name = AssetName(Kind::Image, I.Pos + LinkedNameAt);
        if (!Name.empty() && Name[0] == ',') Name.erase(0, 1);
        if (Name.empty() || Named.count(ExportNaming::Lower(Name))) continue;
        const auto [From, To] = FieldOf.equal_range(I.Pos);
        std::set<uint32_t> Seen;
        for (auto Field = From; Field != To; ++Field)
        {
            if (Field->second + SemanticAt + 4 > (int64_t)Image().size()) continue;
            const uint32_t Semantic = At<uint32_t>(Image(), Field->second + SemanticAt);
            // a hash some material of the zone binds a slot with: other bytes beside a pointer are not one
            if (Hashes.count(Semantic) && Seen.insert(Semantic).second) Loose.push_back({ 0, Semantic, Name });
        }
    }
    if (Loose.empty()) Out.erase(std::string());
}

void ZonekitZone::MaterialHeaders(std::set<std::string>& Out) const
{
    using DiskGame::Kind;
    const char* const Type = MaterialType != nullptr ? MaterialType : "material";
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, Type)) continue;
        const std::string N = AssetName(Kind::Material, I.Pos);
        if (!IsReference(Kind::Material, N, I.Pos)) continue;
        if (std::string H = Referenced(Kind::Material, N); !H.empty()) Out.insert(std::move(H));
    }
}

std::function<void(XMaterial_t& Material)> ZonekitZone::HeaderFiller(const DiskGame::Context& C)
{
    using DiskGame::Kind;
    if (!MaterialOf || MaterialType == nullptr) return nullptr;
    std::set<std::string> Defined, Headers;
    for (const auto& I : R.Instances)
    {
        if (!OwnerIs(I, MaterialType)) continue;
        const std::string N = AssetName(Kind::Material, I.Pos);
        if (!IsReference(Kind::Material, N, I.Pos)) Defined.insert(ExportNaming::Lower(N));
        else if (std::string H = Referenced(Kind::Material, N); !H.empty()) Headers.insert(std::move(H));
    }
    std::set<std::string> Here, Elsewhere;
    for (const auto& H : Headers) (Defined.count(ExportNaming::Lower(H)) ? Here : Elsewhere).insert(H);
    if (Here.empty() && Elsewhere.empty()) return nullptr;
    DiskGame::MaterialBook Book;
    if (!Here.empty())
    {
        DiskGame::MaterialBook Own;
        MaterialImages(Own);
        for (auto& [N, Images] : Own)
            if (Here.count(N)) Book[N] = std::move(Images);
    }
    // (another zone may be opened meanwhile, this one parked and resumed)
    if (!Elsewhere.empty() && C.MaterialsElsewhere) C.MaterialsElsewhere(*this, Elsewhere, Book);
    auto ByKey = std::make_shared<std::unordered_map<std::string, std::vector<DiskGame::MaterialImage>>>();
    for (auto& [N, Images] : Book)
        if (!Images.empty()) ByKey->try_emplace(Key(N), std::move(Images));
    Counter("material_headers") += Headers.size();
    if (ByKey->empty()) return nullptr;
    return [this, ByKey](XMaterial_t& M) {
        const auto It = ByKey->find(Key(M.MaterialName));
        if (It == ByKey->end()) return;
        for (const auto& I : It->second) M.Images.emplace_back((ImageUsageType)I.Usage, I.Hash, ExportWriters::ImageElsewhere, I.Image);
        Counter("model_materials_from_headers")++;
    };
}
