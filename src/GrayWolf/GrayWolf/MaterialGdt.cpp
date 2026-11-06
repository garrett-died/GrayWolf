#include "stdafx.h"

#include "MaterialGdt.h"

bool H1TechsetRulesSelfTest(const std::string& TempDir, std::string& Got);   // Games/H1/MaterialAdapter.cpp
#include "ImageFacts.h"
#include "WraithModel.h"
#include "XMEExport.h"
#include <stdexcept>
#include <thread>

#include "DiskGame.h"
#include <optional>
#include <cmath>
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "GdtWriter.h"
#include "SurfaceTypes.h"
#include "Bo3Target.Awi.h"
#include "Bo3Target.Output.h"
#include "Bo3Target.Placement.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <fstream>
#include <regex>
#include <tuple>

namespace fs = std::filesystem;

namespace MaterialGdt
{
namespace
{
    thread_local bool UseAssetNames = false;     // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): per-thread run state, not shared;     // per thread: the run that sets it is the thread that reads it (as LiveExport's ModelFiles); two runs at once do not share it

    // APE writes an entry's keys sorted, case aside
    bool KeyLess(const std::string& A, const std::string& B)
    {
        return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end(), [](char X, char Y) {
            return std::tolower((unsigned char)X) < std::tolower((unsigned char)Y);
        });
    }

    std::string Floats3(const float C[3])
    {
        char B[96];
        std::snprintf(B, sizeof(B), "%g %g %g 1", C[0], C[1], C[2]);
        return B;
    }

    // the short tag an image's name takes when its file serves two semantics
    std::string Tag(const std::string& Semantic)
    {
        static const std::map<std::string, std::string> Tags = { { "diffuseMap", "c" }, { "normalMap", "n" }, { "glossMap", "g" },
                                                                 { "specularMap", "s" }, { "specularMask", "sm" }, { "occlusionMap", "o" },
                                                                 { "thicknessMap", "t" }, { "revealMap", "r" } };
        const auto It = Tags.find(Semantic);
        return It == Tags.end() ? Semantic : It->second;
    }

    // The suffix BO3 gives the image of a material slot ("i_<material>_<suffix>": measured on stock, bo3-stock-naming.md section 1):
    // c, n, g, s, o as stock writes them; m for a reveal map (stock: m 3,505 against r 1,406, so m is the majority; it is also the name of
    // some multipleMask and thicknessMap images, the semantic field is what tells them apart); t for a thickness map; e for the emissive
    // colour of colorMap00 (stock 448); any other slot is named by its key.
    std::string SlotTag(const std::string& Key, const std::string& Semantic)
    {
        if (Key == "colorMap") return "c";
        if (Key == "normalMap") return "n";
        if (Key == "cosinePowerMap") return "g";
        if (Key == "specColorMap") return "s";
        if (Key == "occMap") return "o";
        if (Key == "alphaRevealMap") return Semantic == "thicknessMap" ? "t" : "m";
        if (Key == "colorMap00" && Semantic == "diffuseMap") return "e";
        const std::string K = AssetName(Key);
        return K.empty() ? Tag(Semantic) : K;
    }

    // The code images BO3's own materials name ("$normal"): what the stock GDTs of its mod tools put
    // in a material's image slots
    const char* const StockCodeImages[] = { "$black_color", "$black_gloss", "$black_reveal", "$black_thickness",
                                            "$blacktransparent_color", "$color_black_40", "$gloss", "$gray", "$gray_color",
                                            "$identitynormalmap", "$normal", "$occlusion", "$spec_simple", "$specular_mask",
                                            "$white_ao", "$white_diffuse", "$white_gloss", "$white_multimask", "$white_reveal",
                                            "$white_specular", "$white_thickness" };

    ImageChannel ChannelOf(int Channel)
    {
        return Channel == 1 ? ImageChannel::Green : Channel == 2 ? ImageChannel::Blue
             : Channel == 3 ? ImageChannel::Alpha : ImageChannel::Red;
    }

    bool TiffOrPng(const fs::path& File)
    {
        const std::string E = ExportNaming::Lower(File.extension().string());
        return E == ".tif" || E == ".tiff" || E == ".png";
    }
}

std::string AssetName(const std::string& Source)
{
    std::string Out;
    Out.reserve(Source.size());
    for (const char C : Source)
    {
        const char L = (char)std::tolower((unsigned char)C);
        Out += (L >= 'a' && L <= 'z') || (L >= '0' && L <= '9') ? L : '_';
    }
    const size_t First = Out.find_first_not_of('_');
    if (First == std::string::npos) return std::string();
    return Out.substr(First, Out.find_last_not_of('_') - First + 1);
}

std::string EntryName(const std::string& Source)
{
    return Bo3Target::Placement::Shorten(AssetName(Source));
}

void NameModelMaterials(WraithModel& Model, std::vector<std::string>& Saved)
{
    Saved.clear();
    for (WraithMaterial& Material : Model.Materials)
    {
        Saved.push_back(Material.MaterialName);
        Material.MaterialName = EntryName(Material.MaterialName);
    }
}

void RestoreModelMaterialNames(WraithModel& Model, const std::vector<std::string>& Saved)
{
    for (size_t I = 0; I < Saved.size() && I < Model.Materials.size(); ++I) Model.Materials[I].MaterialName = Saved[I];
}

ScopedModelMaterialNames::ScopedModelMaterialNames(WraithModel& Model, bool On) : Model(Model), On(On)
{
    if (On) NameModelMaterials(Model, Saved);
}

ScopedModelMaterialNames::~ScopedModelMaterialNames()
{
    if (On) RestoreModelMaterialNames(Model, Saved);
}

// A name without the leading prefix the game adapter declares left out ("mtl_"); one the prefix would empty is kept
std::string WithoutPrefix(const Materials::ImageNaming& Naming, const std::string& Name)
{
    for (const std::string& P : Naming.StripPrefixes)
        if (!P.empty() && Name.size() > P.size() && Name.compare(0, P.size(), P) == 0) return Name.substr(P.size());
    return Name;
}

bool IsDecoratedImageName(const std::string& Stem)
{
    std::string L;
    for (const char C : Stem) L += (char)std::tolower((unsigned char)C);
    if (L.rfind("packed_", 0) == 0) return true;
    size_t Start = 0;
    for (size_t I = 0; I <= L.size(); I++)
    {
        const bool End = I == L.size();
        const char C = End ? '_' : L[I];
        if (!End && (C == '&' || C == '~')) return true;       // the Infinity Ward form markers (_c&, _s~); any other odd character is left to AssetName
        if (std::isalnum((unsigned char)C)) continue;
        const size_t Len = I - Start;
        if (Len == 8)
        {
            bool Digit = false, Letter = false, Hex = true;
            for (size_t K = Start; K < I; K++)
            {
                if (L[K] >= '0' && L[K] <= '9') Digit = true;
                else if (L[K] >= 'a' && L[K] <= 'f') Letter = true;
                else Hex = false;
            }
            if (Hex && Digit && Letter) return true;
        }
        Start = I + 1;
    }
    return false;
}

bool Builder::Image::operator<(const Image& O) const
{
    return std::tie(Builtin, File, Semantic, FromAlpha) < std::tie(O.Builtin, O.File, O.Semantic, O.FromAlpha);
}

Builder::Builder(std::function<std::string(const fs::path&)> BaseImage, std::string CounterPrefix)
    : BaseImageOf(std::move(BaseImage)), Prefix(std::move(CounterPrefix))
{
}

bool Builder::Empty() const
{
    return Defined.empty();
}

// One image slot: the map's file, as the BO3 image of Semantic. A single-channel semantic takes the
// file's red channel, or its alpha (fromAlpha): a value in green or blue has no BO3 slot.
bool Builder::Slot(Material& M, const char* Key, const Materials::Map& Map, const fs::path& Dir, const char* Semantic,
                   const char* Core, const char* Compression)
{
    if (Map.Empty()) return false;
    const bool Single = std::string(Core) == "Linear1ch";
    if (Single && (Map.Channel == 1 || Map.Channel == 2))
    {
        Counter(std::string(Key) + "_channel_dropped")++;
        return false;
    }
    Image I;
    I.File = (Dir / fs::path(Map.Path)).lexically_normal();
    I.FromAlpha = Single && Map.Channel == 3;
    // a code image BO3 has too: named as it is, no file
    const std::string Stem = ExportNaming::Lower(I.File.stem().string());
    if (!I.FromAlpha && std::find(std::begin(StockCodeImages), std::end(StockCodeImages), Stem) != std::end(StockCodeImages))
    {
        I.Builtin = Stem;
        M.Slots[Key] = std::move(I);
        Counter(std::string("slot_") + Key)++;
        Counter("slots_stock_images")++;
        return true;
    }
    std::error_code Ec;
    if (!fs::is_regular_file(I.File, Ec))
    {
        Counter("images_missing")++;
        return false;
    }
    if (BaseImageOf(I.File).empty())
    {
        Counter("images_outside")++;
        return false;
    }
    if (!TiffOrPng(I.File)) Counter("slots_not_tiff_png")++;
    I.Semantic = Semantic;
    I.Core = Core;
    I.Compression = Compression;
    M.Slots[Key] = std::move(I);
    Counter(std::string("slot_") + Key)++;
    return true;
}

bool Builder::DerivedSlot(Material& M, const char* Key, const fs::path& File, const char* Semantic, const char* Core, const char* Compression)
{
    if (BaseImageOf(File).empty())
    {
        Counter("images_outside")++;
        return false;
    }
    Image I;
    I.File = File;
    I.Semantic = Semantic;
    I.Core = Core;
    I.Compression = Compression;
    M.Slots[Key] = std::move(I);
    Counter(std::string("slot_") + Key)++;
    return true;
}

bool Builder::Convertible(const Materials::Map& Map, const fs::path& Dir, fs::path& File)
{
    if (!Convert || Map.Empty()) return false;
    File = (Dir / fs::path(Map.Path)).lexically_normal();
    // a sidecar is data: an absolute path or ../ must not make us read, write or delete outside the run
    if (RunRoot.empty() || !Bo3Target::Output::IsUnder(RunRoot, File))
    {
        Counter("images_outside")++;
        return false;
    }
    const std::string Stem = ExportNaming::Lower(File.stem().string());
    if (std::find(std::begin(StockCodeImages), std::end(StockCodeImages), Stem) != std::end(StockCodeImages)) return false;
    std::error_code Ec;
    return fs::is_regular_file(File, Ec);   // a missing file is Slot's images_missing
}

bool Builder::Derive(const char* What, const Bo3Target::DeriveRequest& Request, fs::path& File)
{
    if (DeriveHook) return DeriveHook(What, Request, File);
    // Convertible has checked the source already; the mask and the output (beside the source) are checked here too
    if (RunRoot.empty() || !Bo3Target::Output::IsUnder(RunRoot, Request.Source) ||
        (!Request.Mask.empty() && !Bo3Target::Output::IsUnder(RunRoot, Request.Mask)))
    {
        Counter(std::string("not_converted_") + What + "_outside")++;
        return false;
    }
    const Bo3Target::DeriveResult R = Derived.Derive(Request);
    Count[Prefix + "images_derived"] = Derived.Written();
    if (R.Ok())
    {
        File = R.File;
        return true;
    }
    Counter(std::string("not_converted_") + What + "_" + R.Error)++;
    return false;
}

void Builder::Add(const Materials::Description& D, const fs::path& Dir, const Materials::Source* Source, const std::string& Game)
{
    AddDir = Dir;
    const std::string Name = AssetName(D.Name);
    const std::string& SkipWhy = D.SkipWhy;
    if (!SkipWhy.empty())
    {
        // the game's adapter said this material is not exported (BO3 *_combo images, HydraX D-13): no entry, no image, a row in the report
        Counter("skipped_by_game_rule")++;
        const size_t Colon = SkipWhy.find(':');
        SkippedMaterials.push_back({ Name, { "material.skipped_" + SkipWhy.substr(0, Colon), "game=" + Game + ", " + (Colon == std::string::npos ? std::string() : SkipWhy.substr(Colon + 1)),
                                             "the game's own rule says this material is not exported (HydraX D-13: an image named *_combo makes it an unusable duplicate)" } });
        return;
    }
    if (Name.empty())
    {
        Counter("unnamed")++;
        return;
    }
    if (const auto It = Defined.find(Name); It != Defined.end())
    {
        if (It->second.Source == D.Name) Counter("shared")++;
        else if (Collisions[Name].insert(D.Name).second) Counter("name_collisions")++;
        return;
    }
    Material M;
    M.Source = D.Name;
    M.Naming = D.Naming;
    if (Name != D.Name) Counter("renamed")++;
    if (D.LayersDropped > 0)
    {
        // a layered material has the maps of its first layer only: the others are not carried (a multi-mesh decomposition is a later step)
        Counter("layers_dropped_materials")++;
        Counter("layers_dropped") += (size_t)D.LayersDropped;
        M.Notes.push_back({ "material.layers_dropped", "game=" + Game + ", layers=" + std::to_string(D.LayersDropped), "the material has more layers than the first: only the first layer maps are written, the others are not" });
    }

    if (!D.TexturesUnbound.empty())
    {
        // textures of the source the description does not carry: named, counted per kind, and the gate warns (nothing is dropped silently)
        Counter("texture_unbound_materials")++;
        std::stringstream Kinds(D.TexturesUnbound);
        for (std::string Kind; std::getline(Kinds, Kind, ',');) Counter("texture_unbound_" + Kind.substr(0, Kind.find('(')))++;
        M.Notes.push_back({ "material.texture_unbound", "game=" + Game + ", semantics=" + D.TexturesUnbound, "the source binds textures this export does not write: they are named here and not carried to BO3" });
    }
    if (!D.FamilyWhy.empty())
    {
        Counter("family_" + D.FamilyWhy)++;
        M.Notes.push_back({ "material.family_" + D.FamilyWhy, "game=" + Game, "the surface is ambiguous or unread, so the material is not known to be a dielectric: the row's name says what became of its specular map" });
    }

    // the surface type: the game's own flags (SurfaceTypes.h), never a guess; <none> with a note when the game gave none
    if (!D.SurfaceType.empty())
    {
        const std::string Value = SurfaceChoices.empty() ? D.SurfaceType : SurfaceTypes::ToBo3(D.SurfaceType, SurfaceChoices);
        if (Value.empty())
        {
            Counter("surface_unmapped")++;
            M.Notes.push_back({ "material.surface_unmapped", "source_surface=" + D.SurfaceType, "material.awi's surfaceType has no option of that name: left <none>" });
        }
        else
        {
            M.SurfaceType = Value;
            Counter("surface_" + Value)++;
            if (!D.SurfaceVia.empty())
            {
                // the value rests on an intersection of the level's blends, not on this material's own word: said with its evidence (hypothesis)
                Counter("surface_from_layer_id")++;
                M.Notes.push_back({ "material.surface_from_layer_id", "game=" + Game + ", surface=" + Value + ", " + D.SurfaceVia,
                                    "the first layer's surface was inferred by intersecting the words of the blends that hold its layer id; a layer with no bit of its own in a single-bit partner's blend would be handed the partner's bit" });
            }
        }
    }
    else if (D.SurfaceWhy == "none_in_source" && Source != nullptr && SurfaceTypes::ReadAndNamesNoSurface(Game, Source->SurfaceFlags))
    {
        // positive evidence: a word was read and names no surface. Every <none> carries a row, so the gate can tell this from a word never read.
        Counter("surface_none_in_source")++;
        M.Notes.push_back({ "material.surface_none_in_source", "game=" + Game, "the source's surface word was read and names no surface type: <none> is the source's own value" });
    }
    else if (D.SurfaceWhy.rfind("no_bo3_use:", 0) == 0)
    {
        // the game names a surface type that no stock BO3 material uses (HydraX writes <none> for BO4 player and riotshield): <none>, said in a row
        Counter("surface_no_bo3_use")++;
        M.Notes.push_back({ "material.surface_no_bo3_use", "game=" + Game + ", source_surface=" + D.SurfaceWhy.substr(11),
                            "no stock BO3 material entry uses this surface type: <none>, as HydraX writes it" });
    }
    else
    {
        // a description that says none_in_source but whose source flags do not show a read word with no type bits (a sidecar written by hand, or one
        // without the flags) is not evidence: it is reported unknown
        const std::string Why = D.SurfaceWhy == "none_in_source" ? "none_in_source_unverified" : D.SurfaceWhy.empty() ? "not_read" : D.SurfaceWhy;
        Counter("surface_unknown")++;
        Counter("surface_unknown_" + Why.substr(0, Why.find(':')))++;
        M.Notes.push_back({ "material.surface_unknown", "game=" + Game + ", why=" + Why,
                            "the source's surface type is not known (no_table: the game's flag word is not measured; not_read: the reader gives none; unnamed: a value no table row declares): left <none>, not guessed" });
    }

    // the colour and roughness of a layered material are its layers': the first one's
    Materials::Map Albedo = D.Albedo, Roughness = D.Roughness;
    if (!D.Layers.empty())
    {
        if (Albedo.Empty()) Albedo = D.Layers.front().Albedo;
        if (Roughness.Empty()) Roughness = D.Layers.front().Roughness;
        Counter("layered_first_layer")++;
    }
    if (!D.Tint.Empty() || !D.Color.Empty()) Counter("tint_dropped")++;
    if (!D.AlphaMap.Empty()) Counter("alpha_map_dropped")++;

    // A colour fused with its metal mask (a Description pack kind) becomes BO3's colour and colour
    // specular. A metalness map with no pack kind stays dropped: its channel cannot be trusted
    // to have survived the export (IW7's colour alpha is stripped).
    using Materials::PackKind;
    using Bo3Target::DeriveOp;
    fs::path FusedColour, FusedSpec;
    bool Fused = false;
    if (!D.Metalness.Empty() && Convert && D.Specular.Empty() && !D.Albedo.Empty())
    {
        const bool ByAlpha = D.Metalness.Pack == PackKind::ColorMetalAlpha, ByMask = D.Metalness.Pack == PackKind::ColorMetalMask;
        fs::path Colour, Mask;
        if ((ByAlpha && D.Albedo.Path == D.Metalness.Path && Convertible(D.Albedo, Dir, Colour)) ||
            (ByMask && D.Albedo.Path != D.Metalness.Path && Convertible(D.Albedo, Dir, Colour) && Convertible(D.Metalness, Dir, Mask)))
        {
            Bo3Target::DeriveRequest Rq;
            Rq.Source = Colour;
            Rq.Mask = Mask;
            Rq.Op = ByAlpha ? DeriveOp::FusedAlphaColour : DeriveOp::FusedMaskColour;
            Rq.Tag = "c";
            Fused = Derive("metal_fused", Rq, FusedColour);
            Rq.Op = ByAlpha ? DeriveOp::FusedAlphaSpecular : DeriveOp::FusedMaskSpecular;
            Rq.Tag = "s";
            Fused = Derive("metal_fused", Rq, FusedSpec) && Fused;
            if (Fused) Counter(ByAlpha ? "converted_metal_fused_alpha" : "converted_metal_fused_mask")++;
        }
        else if (ByAlpha || ByMask)
            Counter("not_converted_metal_fused_inputs")++;
    }
    if (!D.Metalness.Empty() && !Fused) Counter("metalness_dropped")++;

    // render class: where a game's techset flags decide the decal class (BO4), a material whose flags were not read is reported, never guessed
    if (Source != nullptr)
        if (const SurfaceTypes::Names* Table = SurfaceTypes::For(ExportNaming::Lower(Game)); Table != nullptr && Table->HasRenderBits() &&
                                                                                       SurfaceTypes::ClassOf(ExportNaming::Lower(Game), Source->RenderFlags) == SurfaceTypes::RenderClass::Unread)
        {
            Counter("render_class_unread")++;
            M.Notes.push_back({ "material.render_class_unread", "game=" + Game + ", techset=" + Source->Techset,
                                "the techset's renderFlags were not read (a stub techset or a failed read): the material is not classed as a decal, by flags or by anything else" });
        }

    // a decal by the source (its techset name, or its renderFlags where the name is a hash): only used when no catalog chose the BO3 techset
    if (Source != nullptr)
        M.SourceDecal = ExportNaming::Lower(Source->Techset).find("decal") != std::string::npos || SurfaceTypes::IsDecal(ExportNaming::Lower(Game), Source->RenderFlags);

    // the technique: emission, then light through the surface, then the alpha, then a colour specular
    const bool Emissive = !D.Emissive.Empty(), Backlit = !D.Transmission.Empty() && D.Transmission.Image.Invert;
    if (!D.Transmission.Empty() && !D.Transmission.Image.Invert) Counter("transmission_not_thickness")++;
    const bool Blend = D.Alpha == Materials::AlphaMode::Blend, Cutout = D.Alpha == Materials::AlphaMode::Cutout;
    const bool Spec = !D.Specular.Empty() || Fused;
    // with a target the techset is chosen once the slots are known (ChooseTechset); else by the fixed rules of old
    const bool Catalog = Targets != nullptr && Targets->HasCatalog();
    if (Targets != nullptr && !Catalog) Counter("techset_data_missing")++;
    if (Catalog && !Targets->HasDefaults()) Counter("material_defaults_missing")++;
    M.Category = "Geometry";
    if (Catalog) M.Targeted = true;
    else if (Emissive) M.Type = "lit_emissive";
    else if (Backlit) M.Type = "lit_backlit";
    else if (Blend) M.Type = Spec ? "lit_transparent_advanced" : "lit_transparent";
    else if (Cutout) M.Type = Spec ? "lit_alphatest_advanced" : "lit_alphatest";
    else if (Spec) M.Type = "lit_advanced_fullspec";
    else M.Type = "lit";
    if (!Catalog && M.Type.find("advanced") != std::string::npos) M.Category = "Geometry Advanced";
    if (!Catalog && (Emissive || Backlit) && (Blend || Cutout)) Counter("alpha_dropped")++;
    if (D.Alpha == Materials::AlphaMode::Auto) Counter("alpha_auto_opaque")++;

    const char* const ColourCompression = D.Alpha == Materials::AlphaMode::Opaque ? "compressed no alpha" : "compressed high color";
    if (!(Fused ? DerivedSlot(M, "colorMap", FusedColour, "diffuseMap", "sRGB3chAlpha", ColourCompression)
                : Slot(M, "colorMap", Albedo, Dir, "diffuseMap", "sRGB3chAlpha", ColourCompression)))
        Counter("no_color")++;

    // the normal: unpacked from its pack, and/or green flipped, when the description says so
    bool NormalDone = false;
    {
        fs::path From, File;
        const bool Nog = D.Normal.Pack == PackKind::Nog, Ag = D.Normal.Pack == PackKind::NormalAlphaGreen;
        if ((Nog || Ag || D.NormalFlipY) && Convertible(D.Normal, Dir, From))
        {
            Bo3Target::DeriveRequest Rq;
            Rq.Source = From;
            Rq.Op = Nog ? DeriveOp::NormalNog : Ag ? DeriveOp::NormalAlphaGreen : DeriveOp::NormalFlipOnly;
            Rq.FlipY = D.NormalFlipY;
            Rq.Tag = Tag("normalMap");
            if (Derive("normal", Rq, File) && DerivedSlot(M, "normalMap", File, "normalMap", "Normal", "compressed"))
            {
                NormalDone = true;
                if (Nog) Counter("converted_normal_nog")++;
                if (Ag) Counter("converted_normal_ag")++;
                if (D.NormalFlipY) Counter("converted_normal_flip_y")++;
            }
        }
        else if (Convert && D.NormalEnc == Materials::NormalEncoding::AG && !D.Normal.Empty())
            Counter("not_converted_normal_ag_unmarked")++;   // the export may have decoded it already: not touched
    }
    if (!NormalDone && Slot(M, "normalMap", D.Normal, Dir, "normalMap", "Normal", "compressed"))
    {
        Counter(D.NormalEnc == Materials::NormalEncoding::AG ? "normal_ag" : D.NormalEnc == Materials::NormalEncoding::XYZ ? "normal_xyz" : "normal_xy")++;
        if (D.NormalFlipY) Counter("normal_flip_y")++;
    }

    // BO3's cosinePowerMap is a gloss map: a roughness map is inverted into one, a gloss in a green or
    // blue channel or in a pack is taken out as a grey image
    bool GlossDone = false;
    {
        fs::path From, File;
        const bool Direct = Roughness.Invert && Roughness.Pack == PackKind::None && Roughness.Channel != 1 && Roughness.Channel != 2;
        if (!Direct && Convertible(Roughness, Dir, From))
        {
            Bo3Target::DeriveRequest Rq;
            Rq.Op = DeriveOp::Channel;
            Rq.Source = From;
            Rq.Channel = ChannelOf(Roughness.Channel);
            Rq.Invert = !Roughness.Invert;   // Invert set: the map is already a gloss
            Rq.Tag = Tag("glossMap");
            if (Derive("gloss", Rq, File) && DerivedSlot(M, "cosinePowerMap", File, "glossMap", "Linear1ch", "compressed"))
            {
                GlossDone = true;
                Counter(Roughness.Pack == PackKind::SpecularGlossAlpha ? "converted_spec_gloss_alpha" : Roughness.Invert ? "converted_channel_cosinePowerMap" : "converted_roughness_to_gloss")++;
            }
        }
    }
    if (GlossDone) {}
    else if (!Roughness.Empty() && Roughness.Pack == PackKind::SpecularGlossAlpha)
    {
        // the gloss is the alpha of a composite: never publish the composite itself as the gloss map (its RGB would be read as gloss). cosinePowerMap stays empty, the gate fails the material
        Counter("gloss_conversion_failed")++;
        M.Notes.push_back({ "material.gloss_conversion_failed", "image=" + fs::path(Roughness.Path).filename().string(), "the gloss is the alpha of a specular composite and could not be extracted; cosinePowerMap is left empty" });
    }
    else if (!Roughness.Empty() && !Roughness.Invert) Counter("roughness_not_gloss")++;
    else Slot(M, "cosinePowerMap", Roughness, Dir, "glossMap", "Linear1ch", "compressed");
    if (Spec)
    {
        // a full colour in the fullspec and emissive techniques, else the intensity mask theirs take
        // (with a target the slot the techset has decides: ChooseTechset gives a scalar slot its mask semantic)
        const bool Colour = Catalog || M.Type == "lit_advanced_fullspec" || M.Type == "lit_emissive";
        // a specular colour stored as sqrt(F0) (the description says so by its pack kind): the sRGB-coded F0 BO3 specColorMap holds, alpha dropped
        fs::path F0From, F0File;
        bool F0Done = false;
        if (!Fused && Colour && D.Specular.Pack == PackKind::SpecularSqrtF0 && Convertible(D.Specular, Dir, F0From))
        {
            Bo3Target::DeriveRequest Rq;
            Rq.Source = F0From;
            Rq.Op = DeriveOp::SpecularSqrtF0;
            Rq.Tag = Tag("specularMap");
            F0Done = Derive("spec_sqrt_f0", Rq, F0File) && DerivedSlot(M, "specColorMap", F0File, "specularMap", "sRGB3ch", "compressed");
            if (F0Done) Counter("converted_spec_sqrt_f0")++;
        }
        if (!F0Done && !Fused && Colour && D.Specular.Pack == PackKind::SpecularSqrtF0)
        {
            // never publish the sqrt(F0) encoding as if it were F0: the slot stays empty and the gate fails the material, naming the image
            Counter("f0_conversion_failed")++;
            M.Notes.push_back({ "material.f0_conversion_failed", "image=" + fs::path(D.Specular.Path).filename().string(), "the specular colour is sqrt(F0) and could not be converted to F0; specColorMap is left empty" });
        }
        else if (F0Done || (Fused ? DerivedSlot(M, "specColorMap", FusedSpec, Colour ? "specularMap" : "specularMask", Colour ? "sRGB3ch" : "Linear1ch", "compressed")
                  : Slot(M, "specColorMap", D.Specular, Dir, Colour ? "specularMap" : "specularMask", Colour ? "sRGB3ch" : "Linear1ch", "compressed")))
        {
            if (!Colour) Counter("spec_as_mask")++;
            if (D.LegacySpecular) Counter("spec_legacy")++;
        }
    }
    {
        // an occlusion in a pack or in green or blue is taken out as a grey image
        fs::path From, File;
        bool Done = false;
        if ((D.Occlusion.Pack != PackKind::None || D.Occlusion.Channel == 1 || D.Occlusion.Channel == 2) && Convertible(D.Occlusion, Dir, From))
        {
            Bo3Target::DeriveRequest Rq;
            Rq.Op = DeriveOp::Channel;
            Rq.Source = From;
            Rq.Channel = ChannelOf(D.Occlusion.Channel);
            Rq.Invert = D.Occlusion.Invert;
            Rq.Tag = Tag("occlusionMap");
            Done = Derive("occlusion", Rq, File) && DerivedSlot(M, "occMap", File, "occlusionMap", "Linear1ch", "compressed");
            if (Done) Counter("converted_channel_occMap")++;
        }
        if (!Done) Slot(M, "occMap", D.Occlusion, Dir, "occlusionMap", "Linear1ch", "compressed");
    }
    if (Emissive) Slot(M, "colorMap00", D.Emissive, Dir, "diffuseMap", "sRGB3chAlpha", "compressed no alpha");
    if (Backlit && Slot(M, "alphaRevealMap", D.Transmission.Image, Dir, "thicknessMap", "Linear1ch", "compressed"))
        M.Values.emplace_back("backlightScatterColor", Floats3(D.Transmission.Color));
    if (Catalog) ChooseTechset(M, D, Source, Game, Emissive, Spec, Blend, Cutout);
    // a gloss map's full range, as the source game's reads it
    else if (M.Slots.count("cosinePowerMap")) M.Values.emplace_back("glossSurfaceType", "<full>");
    Counter("type_" + M.Type)++;
    Counter("materials")++;
    Defined.emplace(Name, std::move(M));
}

namespace
{
    namespace Tc = Bo3Target::Techsets;

    // the semantic an image has when it is placed by its key alone (a code image names no semantic)
    std::string IntentOfKey(const std::string& Key)
    {
        if (Key == "normalMap") return "normalMap";
        if (Key == "cosinePowerMap") return "glossMap";
        if (Key == "specColorMap") return "specularMap";
        if (Key == "occMap") return "occlusionMap";
        if (Key == "alphaRevealMap") return "revealMap";    // 164 catalog slots of this key take a reveal map, 39 a thickness map (a backlit material binds its own, explicitly)
        return "diffuseMap";
    }

    // The semantic a code image stands for: the dominant one of the catalog slots that default to it. $specular_mask is no slot default, so
    // the catalog cannot say; t7kb gdt_check reads it as a specular mask (mp_frenetic, 2026-11-04), which the name also says.
    std::string CodeSemanticOf(const Tc::Data& D, const std::string& Image)
    {
        return D.DominantCodeSemantic(Image);     // $specular_mask is a (hypothesis) entry of the catalog data, like the gate's
    }

    bool CodeImageAccepted(const Tc::Data& D, const Tc::Slot& S, const std::string& Image, std::string& Shown)
    {
        std::vector<std::string> Own;
        if (const auto* All = D.CodeImageSemantics(Image))
            for (const auto& [Semantic, Count] : *All) Own.push_back(Semantic);
        if (Own.empty())
        {
            const std::string One = CodeSemanticOf(D, Image);
            if (!One.empty()) Own.push_back(One);
        }
        if (Own.empty()) return true;    // nothing known about it: not judged here
        for (const std::string& O : Own)
        {
            Shown += (Shown.empty() ? "" : "/") + O;
            if (Tc::Accepts(S, O)) return true;
        }
        return false;
    }

    bool ReadFloat(const std::vector<uint8_t>& Block, uint32_t At, float& Out)
    {
        if (static_cast<size_t>(At) + 4 > Block.size()) return false;
        std::memcpy(&Out, &Block[At], 4);
        return std::isfinite(Out);
    }
}

void Builder::ChooseTechset(Material& M, const Materials::Description& D, const Materials::Source* Source, const std::string& Game,
                            bool Emissive, bool Spec, bool Blend, bool Cutout)
{
    Tc::Request Rq;
    const std::string GameId = ExportNaming::Lower(Game);
    const bool Hashed = GameId == "bo4" || GameId == "coldwar";     // the two games whose techsets are known by hash
    Rq.SourceTechset = Source != nullptr ? Source->Techset : std::string();
    std::string ResolvedName;
    if (Hashed && Source != nullptr && !Source->Techset.empty())
    {
        // the name list (package_index/cod_techsets.wni) turns the hash into the name the Name, Normalized, Sky and Family rules work on
        ResolvedName = Targets->NameOfSource(Source->Techset);
        if (!ResolvedName.empty()) Rq.SourceTechset = ResolvedName;
        Counter(ResolvedName.empty() ? "techset_name_miss" : "techset_name_hit")++;
        if (!Targets->HasNames()) Counter("techset_names_missing")++;
    }
    for (auto& [Key, I] : M.Slots)
    {
        // a placeholder of the slot ($white_gloss) is no wish for the slot
        if (!I.Builtin.empty() && Targets->IsDefaultImage(Key, I.Builtin)) continue;
        if (!I.Builtin.empty())
        {
            // a code image is what the catalog slots that default to it say it is ($white_specular is a specular mask), else what the key means
            const std::string Own = CodeSemanticOf(*Targets, I.Builtin);
            Rq.Wanted.emplace_back(Key, Own.empty() ? IntentOfKey(Key) : Own);
            continue;
        }
        std::string Intent = I.Semantic;
        if (StockSemanticOf)
        {
            // an image of a stock name is the stock image (the publish step leaves it out): its own semantic decides the slot
            const std::string Stem = AssetName(I.File.stem().string());
            const std::string Stock = StockSemanticOf(Stem.rfind("i_", 0) == 0 ? Stem : "i_" + Stem);
            if (!Stock.empty())
            {
                I.StockSemantic = Stock;
                Intent = Stock;
                Counter("slots_stock_semantic_read")++;
            }
        }
        Rq.Wanted.emplace_back(Key, Intent);
    }
    Rq.Class = Blend ? Tc::Blend::Blend : Cutout ? Tc::Blend::AlphaTest : Tc::Blend::Opaque;
    // a decal is told by the game's own techset name when it has one, and by the techset's renderFlags where the name is a hash (BO4)
    const bool DecalByName = !Rq.SourceTechset.empty() && ExportNaming::Lower(Rq.SourceTechset).find("decal") != std::string::npos;
    const bool DecalByFlags = Source != nullptr && SurfaceTypes::IsDecal(ExportNaming::Lower(Game), Source->RenderFlags);
    if (DecalByName || DecalByFlags) Rq.Class = Tc::Blend::Decal;
    if (DecalByFlags && !DecalByName) Counter("decal_from_render_flags")++;
    Rq.Emissive = Emissive;
    // Colour or mask specular: from the specular image's pixel format (the sidecar carries it for Black Ops 4 models), a code image's or a stock
    // image's declared semantic. No specular image, or a format that was not read: HydraX's null, except that a description's specular is an
    // RGB colour (F0) by contract, which the games that do not name their techsets keep.
    Rq.HasColorSpec = false;
    Rq.ColorSpec = true;
    if (const auto SpecIt = M.Slots.find("specColorMap"); SpecIt != M.Slots.end())
    {
        const Image& SI = SpecIt->second;
        bool Known = false, Colour = true;
        if (!SI.Builtin.empty())
        {
            if (!Targets->IsDefaultImage("specColorMap", SI.Builtin))
            {
                const std::string Own = CodeSemanticOf(*Targets, SI.Builtin);
                Known = Own == "specularMask" || Own == "specularMap";
                Colour = Own == "specularMap";
            }
        }
        else if (!SI.StockSemantic.empty())
        {
            Known = true;
            Colour = SI.StockSemantic != "specularMask";
        }
        else if (Source != nullptr)
        {
            // HydraX: the pixel format of the specular image (single channel: a mask). The format is the source image's own, read by the game's
            // reader (Black Ops 4 models today); with none the answer is null, never a guess from the pixels.
            for (const Materials::Source::Image& Im : Source->Images)
                if (Im.Semantic == Tc::SlotHash("specColorMap") && Im.Format != 0)
                {
                    Known = true;
                    Colour = !(Im.Format == 80 || Im.Format == 81 || Im.Format == 61 || Im.Format == 56 || Im.Format == 54);     // BC4, R8, R16, R16F (HydraX IsSingleChannel)
                    break;
                }
        }
        if (Known)
        {
            Rq.HasColorSpec = true;
            Rq.ColorSpec = Colour;
        }
        else if (!Hashed && Spec)
            Rq.HasColorSpec = true;
    }
    // The exact slot set rule compares the material's slot hashes with the techset's texture names.
    if (Source != nullptr && Hashed)
        for (const Materials::Source::Image& Im : Source->Images)
            if (Im.Semantic != 0 && std::find(Rq.SlotHashes.begin(), Rq.SlotHashes.end(), Im.Semantic) == Rq.SlotHashes.end()) Rq.SlotHashes.push_back(Im.Semantic);
    // BO4: the class is the techset's own renderFlags (HydraX). Unread is no class, never a guess from the description.
    if (Hashed)
    {
        Rq.RenderFromFlags = true;
        if (Source != nullptr && SurfaceTypes::ClassOf(GameId, Source->RenderFlags) != SurfaceTypes::RenderClass::Unread)
            Rq.HasRender = Tc::ClassifyRender(Source->RenderFlags, Rq.RenderBlend, Rq.RenderTraits);
        // the techset record was not read but its name is known: the class is the one of the catalog techset the name stands for
        if (!Rq.HasRender && !ResolvedName.empty() && Tc::ClassFromName(*Targets, ResolvedName, Rq.RenderBlend, Rq.RenderTraits))
        {
            Rq.HasRender = true;
            Counter("techset_class_from_name")++;
        }
        if (!Rq.HasRender) Counter("techset_render_class_unread")++;
    }
    Tc::Choice C;
    // Why the game's own techset name was not used (BO3): no techset, a name the reader did not get, or a name the catalog lacks
    std::string NameFallback;
    if (!D.TechsetWhy.empty()) NameFallback = D.TechsetWhy;
    else if (!D.TechsetName.empty() && Targets->Find(D.TechsetName) == nullptr) NameFallback = "not_in_catalog";
    const Tc::Techset* Own = D.TechsetName.empty() ? nullptr : Targets->Find(D.TechsetName);
    // the weapon camo techsets are never written, whatever the game names (the Mod Tools cannot register them; the same exclusion as the name rules)
    if (Own != nullptr && (Own->HasTrait(Tc::Trait::Camo) || Own->Name.find("camo") != std::string::npos))
    {
        NameFallback = "camo_excluded";
        Own = nullptr;
    }
    if (Own != nullptr)
    {
        // the game names its techset in the catalog's terms (BO3): materialType is that techset, as HydraX takes it; no rule decides
        C.Chosen = Own;
        C.Used = Tc::Rule::Name;
        C.Reason = "The material's own techset " + Own->Name + " (the source name cut at #) is in the catalog; the game states it, so it is taken as it is.";
        Counter("techset_from_source_name")++;
    }
    else
        C = Tc::Choose(*Targets, Rq);
    if (C.Chosen == nullptr)
    {
        M.Type = "lit";
        Counter("techset_none")++;
        // no techset at all: the fallback is still reported (a rules_* reason must reach the gate)
        Counter("techset_fallback")++;
        Counter("techset_fallback_" + (NameFallback.empty() ? std::string("none_chosen") : NameFallback))++;
        M.Notes.push_back({ "material.techset_fallback", "reason=" + (NameFallback.empty() ? std::string("none_chosen") : NameFallback) + ", source_name=" + (Source != nullptr && !Source->Techset.empty() ? Source->Techset : D.TechsetName.empty() ? std::string("(none)") : D.TechsetName) + ", result=lit",
                            "no techset could be chosen: the material is written as lit" });
        return;
    }
    const Tc::Techset& T = *C.Chosen;
    M.Type = T.Name;
    M.Category = T.Category.empty() ? "Geometry" : T.Category;
    Counter(std::string("techset_rule_") + Tc::RuleName(C.Used))++;
    // every chosen techset states its rule (graywolf_techset_rules.tsv), so Exact% can be counted
    M.Notes.push_back({ "material.techset_rule", std::string("rule=") + Tc::RuleName(C.Used) + ", exact=" + (C.Exact() ? "1" : "0") + ", source_techset=" + Rq.SourceTechset + ", result=" + T.Name, C.Reason });
    if (!NameFallback.empty())
    {
        // the model that names this material still links: it is written by the slot rules, and the row says why and from which source name
        Counter("techset_fallback")++;
        Counter("techset_fallback_" + NameFallback)++;
        M.Notes.push_back({ "material.techset_fallback", "reason=" + NameFallback + ", source_name=" + (Source != nullptr && !Source->Techset.empty() ? Source->Techset : D.TechsetName.empty() ? std::string("(none)") : D.TechsetName) +
                            ", rule=" + Tc::RuleName(C.Used) + ", result=" + T.Name, C.Reason });
    }
    else if (!C.Exact())
    {
        Counter("techset_fallback")++;
        M.Notes.push_back({ "material.techset_fallback", std::string("rule=") + Tc::RuleName(C.Used) + ", source_techset=" + Rq.SourceTechset + ", result=" + T.Name, C.Reason });
    }

    // A reveal map is no field of the material description: the source material's own reveal image (the texture named revealMap, a decal's
    // alpha reveal) is bound by its slot hash to the key the chosen techset gives that texture (alphaRevealMap). Found on
    // t8_decal_grunge_drips_lrg_01_lgt, which lost its reveal image.
    if ((Hashed || D.RevealSemantic != 0) && Source != nullptr)   // a game whose adapter states its reveal image's slot hash (D.RevealSemantic) is bound like a hashed one
    {
        const uint32_t SlotHash = Tc::SlotHash("revealMap");
        const uint32_t RevealHash = D.RevealSemantic != 0 ? D.RevealSemantic : SlotHash;
        const Tc::Slot* RevealSlot = nullptr;
        for (const Tc::Slot& S : T.Slots)
            if (S.Hash == SlotHash && !S.Key.empty()) RevealSlot = &S;
        for (const Materials::Source::Image& Im : Source->Images)
        {
            if (Im.Semantic != RevealHash) continue;
            if (RevealSlot == nullptr)
            {
                Counter("reveal_image_no_slot")++;
                M.Notes.push_back({ "material.unmapped_slot", "slot=revealMap, techset=" + T.Name + ", image=" + fs::path(Im.Path).filename().string(), "the techset has no revealMap texture: the reveal image is not written" });
            }
            else if (!M.Slots.count(RevealSlot->Key) && Tc::Accepts(*RevealSlot, "revealMap"))
            {
                Materials::Map Reveal;
                Reveal.Path = Im.Path;
                bool Bound = false, Failed = false;
                if (D.RevealSemantic != 0)
                {
                    // the game's reveal image is a grey picture in four channels: its red channel is the one-channel reveal map the techset takes (written as _r)
                    fs::path Grey, From;
                    if (Convertible(Reveal, AddDir, From))
                    {
                        Bo3Target::DeriveRequest Rq;
                        Rq.Op = Bo3Target::DeriveOp::Channel;
                        Rq.Source = From;
                        Rq.Channel = ChannelOf(D.RevealChannel);   // the adapter states which channel of the game's image holds the reveal
                        Rq.Tag = Tag("revealMap");
                        Bound = Derive("reveal", Rq, Grey) && DerivedSlot(M, RevealSlot->Key.c_str(), Grey, "revealMap", "Linear1ch", "compressed");
                    }
                    Failed = !Bound;
                }
                if (Failed)
                {
                    // never bind the raw four-channel picture as a reveal map: the slot stays empty, the gate fails the material, reveal_image_bound is not counted
                    Counter("reveal_conversion_failed")++;
                    M.Notes.push_back({ "material.reveal_conversion_failed", "image=" + fs::path(Im.Path).filename().string(), "the reveal image could not be reduced to one channel; alphaRevealMap is left empty" });
                }
                else if (Bound || Slot(M, RevealSlot->Key.c_str(), Reveal, AddDir, "revealMap", "Linear1ch", "compressed")) Counter("reveal_image_bound")++;
            }
            break;
        }
    }

    // the images land on the slots of the chosen techset; one it lacks is reported, never written
    for (auto It = M.Slots.begin(); It != M.Slots.end();)
    {
        const std::string Key = It->first;
        Image& I = It->second;
        const Tc::Slot* S = T.ByKey(Key);
        if (!I.Builtin.empty())
        {
            if (S != nullptr && ExportNaming::Lower(S->Default) == I.Builtin)
            {
                Counter("slots_default_left_out")++;
                It = M.Slots.erase(It);
                continue;
            }
            if (S == nullptr)
            {
                if (Targets->IsDefaultImage(Key, I.Builtin))
                    Counter("slots_default_left_out")++;    // the slot's own placeholder on a techset without the slot: nothing is lost
                else
                {
                    M.Notes.push_back({ "material.unmapped_slot", "slot=" + Key + ", techset=" + T.Name + ", image=" + I.Builtin, "the techset has no slot " + Key + ": the code image is not written" });
                    Counter("unmapped_slot")++;
                }
                It = M.Slots.erase(It);
                continue;
            }
            if (std::string Shown; !CodeImageAccepted(*Targets, *S, I.Builtin, Shown))
            {
                if (Targets->IsDefaultImage(Key, I.Builtin))
                {
                    // the neutral placeholder of a slot of another kind ($white_specular, the mask slot's default, offered to a colour specular slot): the source had no image there
                    Counter("slots_default_left_out")++;
                    Counter("slot_placeholder_of_other_slot")++;
                    It = M.Slots.erase(It);
                    continue;
                }
                // a code image of another meaning in this slot ($white_specular, a mask, in a colour specular slot): the slot keeps its own default
                M.Notes.push_back({ "material.slot_semantic_rejected", "slot=" + Key + ", techset=" + T.Name + ", image=" + I.Builtin + ", image_semantic=" + Shown,
                                    "the slot " + Key + " takes " + S->Semantic + ", not what " + I.Builtin + " is: the image is not bound and the slot keeps its default" });
                Counter("slot_semantic_rejected")++;
                It = M.Slots.erase(It);
                continue;
            }
            ++It;
            continue;
        }
        // a stock image is judged by what it declares, not by what the source slot meant
        const std::string Effective = I.StockSemantic.empty() ? I.Semantic : I.StockSemantic;
        // An effect techset's colour slot takes effectMap, a diffuse image in the same sRGB colour form. An image this builder writes
        // itself is retargeted to the slot's semantic (as a colour specular is to a mask below), not dropped: 29 effect materials of the
        // 300-model BO4 run lost their colorMap to this. A stock image keeps what it declares and is still judged by it.
        const bool ColourToEffect = S != nullptr && I.StockSemantic.empty() && Effective == "diffuseMap" && !Tc::Accepts(*S, "diffuseMap") && Tc::Accepts(*S, "effectMap");
        if (S == nullptr || (!Tc::Accepts(*S, Effective) && !ColourToEffect))
        {
            M.Notes.push_back({ "material.unmapped_slot", "slot=" + Key + ", techset=" + T.Name + ", image=" + I.File.filename().string(),
                                S == nullptr ? "the techset has no slot " + Key + ": the image is not written"
                                             : "the slot " + Key + " takes " + S->Semantic + ", not " + Effective + ": the image is not written" });
            Counter("unmapped_slot")++;
            Counter("unmapped_slot_" + Key)++;
            It = M.Slots.erase(It);
            continue;
        }
        if (!I.StockSemantic.empty())
        {
            ++It;       // the stock image keeps its own semantic; nothing of it is written
            continue;
        }
        const std::string Sem = ColourToEffect ? std::string("effectMap") : Tc::SemanticFor(*S, I.Semantic);
        if (Sem != I.Semantic)
        {
            M.Notes.push_back({ "material.slot_semantic_changed", "slot=" + Key + ", techset=" + T.Name + ", from=" + I.Semantic + ", to=" + Sem,
                                Sem == "effectMap" ? "the slot takes effectMap: a colour image is written as an effect map (the same sRGB colour form)" : "the slot takes " + Sem + ": a colour specular map is read as a scalar mask" });
            if (Sem == "specularMask") Counter("spec_as_mask")++;
            if (ColourToEffect) Counter("colour_as_effect_map")++;
            const Tc::Form F = Tc::FormOf(Sem);
            I.Semantic = Sem;
            I.Core = F.Core;
            I.Compression = F.Compression;
        }
        else if (Key == "specColorMap" && D.LegacySpecular)
            Counter("spec_legacy")++;
        ++It;
    }
    // stock writes <custom> for most materials, with a gloss map or without (BO3 stock: 73% of 14650 with a gloss image, 83% of 9179 without)
    M.Values.emplace_back("glossSurfaceType", "<custom>");

    // The gloss range maps the gloss image to BO3 stops (0..17). It is written only where a source constant proves it (BO4 below); for
    // every other game the image is written and the range is left at the techset default, which is a stated assumption, never a copied value.
    if (M.Slots.count("cosinePowerMap") && D.GlossStopsMax >= 0.0f)
    {
        // the adapter states the range its gloss image maps to (a measured law of the source game shader, not a copied value)
        M.Values.emplace_back("glossRangeMin", Tc::FloatText(D.GlossStopsMin < 0.0f ? 0.0f : D.GlossStopsMin));
        M.Values.emplace_back("glossRangeMax", Tc::FloatText(D.GlossStopsMax));
        Counter("gloss_range_from_adapter")++;
    }
    else if (M.Slots.count("cosinePowerMap") && ExportNaming::Lower(Game) != "bo4" && !(Source != nullptr && Source->Techset.rfind("xtechset_", 0) == 0))
    {
        const std::string G = ExportNaming::Lower(Game);
        const char* Label = G == "t5" || G == "t4" ? "hypothesis" : "unknown";
        const char* What = G == "t5" || G == "t4" ? "a legacy gloss alpha: the stock T6 legacy ports use glossRangeMax 13 on 78% of 261 (a hypothesis, no BO1 pairs exist to fit it); the range is not written, the map is kept linear"
                         : G == "bo3" || G == "t7" ? "BO3's range is in stops already but this path does not read it from the material's constant block"
                                                   : "the source's gloss curve is not measured; the range is not written and stays at the techset default";
        Counter("gloss_range_default")++;
        M.Notes.push_back({ "material.gloss_range_default", "game=" + G + ", label=" + Label, std::string("glossRangeMin/Max are not set from the source: ") + What });
    }

    // BO4 constants, only where constant-layouts.json lists the techset (no size-gated fallback)
    if (Source == nullptr) return;
    const std::string& Ts = Source->Techset;
    if (Ts.rfind("xtechset_", 0) == 0)
    {
        Counter("constants_not_read_cw")++;
        M.Notes.push_back({ "material.constants_defaulted", "techset=" + Ts, "Cold War: no constant layout data (constant-layouts.json is Black Ops 4 only): colorTint and glossRange stay at the defaults" });
        return;
    }
    if (ExportNaming::Lower(Game) != "bo4") return;
    if (Ts.empty() || Ts.size() > 16 || Ts.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return;
    const std::vector<Tc::ConstantAt>* Layout = Targets->LayoutOf(std::stoull(Ts, nullptr, 16));
    if (Layout == nullptr || Source->Constants.empty())
    {
        Counter(Layout == nullptr ? "constants_no_layout" : "constants_no_block")++;
        M.Notes.push_back({ "material.constants_defaulted", "techset=" + Ts,
                            Layout == nullptr ? "no proven constant layout for this techset in constant-layouts.json: colorTint and glossRange stay at the defaults"
                                              : "the sidecar holds no constant block: colorTint and glossRange stay at the defaults" });
        return;
    }
    const std::vector<uint8_t>& Block = Source->Constants.front();
    std::optional<float> GlossMin, GlossMax;
    for (const Tc::ConstantAt& At : *Layout)
    {
        if (At.Key == "colorTint" && At.Size == 12)
        {
            float V[3];
            if (ReadFloat(Block, At.Offset, V[0]) && ReadFloat(Block, At.Offset + 4, V[1]) && ReadFloat(Block, At.Offset + 8, V[2]))
            {
                M.Values.emplace_back("colorTint", Tc::TintComponentText(V[0]) + " " + Tc::TintComponentText(V[1]) + " " + Tc::TintComponentText(V[2]) + " 1");
                Counter("constants_colorTint")++;
            }
        }
        else if ((At.Key == "glossRangeMin" || At.Key == "glossRangeMax") && At.Size == 4)
        {
            float V;     // read raw: a NaN or infinity is judged below and reported, not silently skipped
            if (static_cast<size_t>(At.Offset) + 4 <= Block.size())
            {
                std::memcpy(&V, &Block[At.Offset], 4);
                (At.Key == "glossRangeMin" ? GlossMin : GlossMax) = V;
            }   // BO4 gloss is 0 to 17 stops like BO3: no conversion
        }
    }
    // a read float is used only when it is a number inside what material.awi allows (0..17) and min <= max; else the default stays, with the reason
    const char* Why = nullptr;
    for (const std::optional<float>& G : { GlossMin, GlossMax })
        if (G && (!std::isfinite(*G) || *G < 0.0f || *G > 17.0f)) Why = "a gloss range float is NaN, infinite or outside material.awi's 0..17";
    if (Why == nullptr && GlossMin && GlossMax && *GlossMin > *GlossMax) Why = "glossRangeMin is above glossRangeMax";
    if (Why != nullptr && (GlossMin || GlossMax))
    {
        Counter("gloss_range_rejected")++;
        M.Notes.push_back({ "material.gloss_range_default", "game=bo4, label=unknown, techset=" + Ts, std::string("glossRangeMin/Max are not set from the source: ") + Why });
        return;
    }
    if (GlossMin) { M.Values.emplace_back("glossRangeMin", Tc::FloatText(*GlossMin)); Counter("constants_glossRangeMin")++; }
    if (GlossMax) { M.Values.emplace_back("glossRangeMax", Tc::FloatText(*GlossMax)); Counter("constants_glossRangeMax")++; }
}

Builder::Resolved Builder::Resolve() const
{
    // The techset and the stock decision were made on the name an image was expected to get (i_<stem>); the final name carries a tag when one stem
    // names several images. So the names are worked out, each stock decision is asked again with the final name, and where the answer differs the
    // image is decided again: no longer stock, it is this builder's own image with its own metadata for the slot (as the non-stock path gives it);
    // another stock image, it is judged by what it declares. A slot that does not fit is left empty. The names are then worked out again.
    std::map<std::string, Material> Work = Defined;
    std::map<std::string, std::vector<Note>> Late;       // by material: what was decided again
    std::map<Image, std::string> ImageNames;
    size_t NameCollisions = 0;      // images whose name was taken by another image and so got a number (before this check one of them was dropped)
    const auto NameImages = [&] {
        ImageNames.clear();
        NameCollisions = 0;
        std::map<std::string, std::vector<Image>> ByStem;
        // A decorated source name is replaced by BO3's own: i_<material>_<slot suffix>. A file several materials share is named from the first of
        // them (material name, then slot key, in sorted order), so the name is the same wherever the image is used. "Decorated" is read from the
        // source file name alone: a stock decision about the new name must not move an image back to its raw stem (the two would alternate).
        std::map<Image, std::pair<std::string, std::string>> Owner;
        std::map<Image, std::string> Declared;     // a name the game adapter's suffix declaration gave
        std::set<Image> Seen;
        for (const auto& [Name, M] : Work)
            for (const auto& [Key, I] : M.Slots)
                if (I.Builtin.empty() && Seen.insert(I).second)
                {
                    const std::string Raw = I.File.stem().string();
                    if (IsDecoratedImageName(Raw))
                    {
                        Owner.emplace(I, std::make_pair(Name, Key));
                        continue;
                    }
                    // BO3's own convention, "i_<name>" (a name that has it already keeps it)
                    std::string Stem = AssetName(Raw);
                    // the game adapter's declaration: a leading prefix left out, a declared suffix replaced by its BO3 tag ("<stem>_col" is "i_<stem>_c")
                    if (!M.Naming.Empty())
                    {
                        Stem = WithoutPrefix(M.Naming, Stem);
                        std::string Tagged;
                        for (const auto& [Suffix, Tag2] : M.Naming.Suffixes)
                            if (Stem.size() > Suffix.size() && Stem.compare(Stem.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
                            {
                                Tagged = "i_" + Stem.substr(0, Stem.size() - Suffix.size()) + "_" + Tag2;
                                break;
                            }
                        if (!Tagged.empty())
                        {
                            Declared[I] = Tagged;
                            continue;
                        }
                    }
                    ByStem[Stem.rfind("i_", 0) == 0 ? Stem : "i_" + Stem].push_back(I);
                }
        std::map<Image, std::string> Candidate;
        for (auto& [Stem, List] : ByStem)
        {
            std::sort(List.begin(), List.end());
            std::map<std::string, size_t> Same;
            for (const Image& I : List)
            {
                std::string Name = Stem;
                if (List.size() > 1) Name += "_" + Tag(I.Semantic) + (I.FromAlpha ? "a" : "");
                if (const size_t N = Same[Name]++) Name += "_" + std::to_string(N + 1);
                Candidate[I] = Name;
            }
        }
        for (const auto& [I, Name] : Declared) Candidate[I] = Name;
        for (const auto& [I, From] : Owner) Candidate[I] = "i_" + WithoutPrefix(Work.at(From.first).Naming, From.first) + "_" + SlotTag(From.second, I.Semantic);
        // One name for one source image and semantic, across clean, decorated and shortened names (Publish shortens a name over 63 characters;
        // two long names can meet there). A reference to a stock image (a clean stem the stock database holds) may be shared: nothing is written
        // for it. Every other image that finds its name taken gets _2, _3, ... in Image order, which does not depend on the order materials were added.
        std::set<std::string> Taken;
        for (const auto& [I, Name] : Candidate)
            if (!I.StockSemantic.empty() && !Owner.count(I)) { ImageNames[I] = Name; Taken.insert(Bo3Target::Placement::Shorten(Name)); }
        for (const auto& [I, Name] : Candidate)
        {
            if (ImageNames.count(I)) continue;
            std::string Final = Name;
            for (size_t N = 2; !Taken.insert(Bo3Target::Placement::Shorten(Final)).second; N++)
            {
                Final = Name + "_" + std::to_string(N);
                if (N == 2) NameCollisions++;
            }
            ImageNames[I] = Final;
        }
    };
    NameImages();
    for (int Round = 0; Round < 3 && StockSemanticOf; ++Round)
    {
        bool Changed = false;
        for (auto& [Name, M] : Work)
        {
            const Tc::Techset* TS = Targets != nullptr ? Targets->Find(M.Type) : nullptr;
            for (auto It = M.Slots.begin(); It != M.Slots.end();)
            {
                const std::string Key = It->first;
                Image& I = It->second;
                if (!I.Builtin.empty()) { ++It; continue; }
                const std::string Bound = ImageNames.at(I);
                const std::string Now = StockSemanticOf(Bound);
                if (Now == I.StockSemantic) { ++It; continue; }
                Changed = true;
                const std::string Judged = I.StockSemantic;
                Late[Name].push_back({ "material.stock_semantic_late", "slot=" + Key + ", image=" + Bound + ", judged=" + (Judged.empty() ? "(none)" : Judged) + ", final=" + (Now.empty() ? "(none)" : Now),
                                       "the image got its final name after the techset was chosen; it is held by an installed GDT, or stock: that decision is made again with the final name" });
                const Tc::Slot* Sl = TS != nullptr ? TS->ByKey(Key) : nullptr;
                I.StockSemantic = Now;
                const std::string Effective = Now.empty() ? I.Semantic : Now;
                if (Sl == nullptr || !Tc::Accepts(*Sl, Effective))
                {
                    Late[Name].push_back({ "material.slot_semantic_rejected", "slot=" + Key + ", techset=" + M.Type + ", image=" + Bound + ", image_semantic=" + Effective,
                                           "the image of the final name does not fit the slot: the slot is left empty" });
                    It = M.Slots.erase(It);
                    continue;
                }
                if (Now.empty())
                {
                    // our own image after all: the metadata the slot gives it (a colour specular map in a mask slot is read as a mask)
                    const std::string Sem = Tc::SemanticFor(*Sl, I.Semantic);
                    if (Sem != I.Semantic)
                    {
                        const Tc::Form Form = Tc::FormOf(Sem);
                        I.Semantic = Sem;
                        I.Core = Form.Core;
                        I.Compression = Form.Compression;
                    }
                }
                ++It;
            }
        }
        if (!Changed) break;
        NameImages();      // the semantics (and so the tags) may have moved
    }

    Resolved Out;
    Out.NameCollisions = NameCollisions;
    Out.Skipped = SkippedMaterials;
    for (const auto& [Name, M] : Work)
    {
        // the usage: only a material the map's own geometry draws has one; it is chosen from the BO3 techset (Category, Type) and the measure
        std::string UsageValue = "<not in editor>";
        Note UsageNote;
        if (const auto Used = Usage.find(M.Source); Used != Usage.end())
        {
            Bo3Target::Techsets::UsageQuery Q;
            // the BO3 techset decides; with no catalog behind it (the fixed rules write only lit techsets) the source's own decal class stands in
            Q.Category = !M.Targeted && M.SourceDecal ? "Decal" : M.Category;
            Q.Type = M.Type;
            Q.Name = Name;
            Q.Use = Used->second;
            // the rules do not depend on the catalog: the data in use, else the shipped table (without it the facing alone decides)
            const Bo3Target::Techsets::UsageRules& Rules = Targets != nullptr && Targets->Usage().Loaded() ? Targets->Usage() : Bo3Target::Techsets::Shared().Usage();
            const Bo3Target::Techsets::UsageChoice C = Rules.Choose(Q);
            UsageValue = C.Usage;
            if (C.Fallback)
                UsageNote = { "material.usage_fallback", "usage=" + C.Usage + ", techset=" + M.Type + ", category=" + M.Category + ", via=" + Used->second.Via,
                              C.Why + ": the usage is the fallback value, an assumption (interior or exterior is not in the data)" };
        }
        Gdt::Fields F = { { "materialCategory", M.Category }, { "materialType", M.Type }, { "surfaceType", M.SurfaceType },
                          { "template", "material.template" }, { "usage", UsageValue } };
        ResolvedMaterial R;
        R.Name = Name;
        R.Source = M.Source;
        R.Targeted = M.Targeted;
        R.Notes = M.Notes;
        if (!UsageNote.Code.empty()) R.Notes.push_back(UsageNote);
        if (const auto L = Late.find(Name); L != Late.end()) R.Notes.insert(R.Notes.end(), L->second.begin(), L->second.end());
        for (const auto& [Key, I] : M.Slots)
        {
            F.emplace_back(Key, I.Builtin.empty() ? ImageNames.at(I) : I.Builtin);
            if (I.Builtin.empty()) R.Images.push_back(ImageNames.at(I));
        }
        F.insert(F.end(), M.Values.begin(), M.Values.end());
        if (M.Targeted)
        {
            // stock writes every key: the techset's image slots empty unless set, then the stock modal value of the rest
            std::set<std::string> Have;
            for (const auto& [Key, Value] : F) Have.insert(Key);
            const Bo3Target::Techsets::Techset* T = Targets != nullptr ? Targets->Find(M.Type) : nullptr;
            if (T != nullptr)
                for (const auto& S : T->Slots)
                    if (!S.Key.empty() && Have.insert(S.Key).second) F.emplace_back(S.Key, std::string());
            if (Targets != nullptr)
                for (const auto& [Key, Value] : Targets->Defaults())
                    if (Have.count(Key) == 0 && (T == nullptr || T->ByKey(Key) == nullptr)) F.emplace_back(Key, Value);
        }
        std::stable_sort(F.begin(), F.end(), [](const auto& A, const auto& B) { return KeyLess(A.first, B.first); });
        R.Fields = std::move(F);
        Out.Materials.push_back(std::move(R));
    }
    std::map<std::string, const Image*> Images;
    for (const auto& [I, Name] : ImageNames) Images[Name] = &I;
    for (const auto& [Name, I] : Images)
    {
        // a block compressed image needs width and height in multiples of 4 (the gate's image.block_size): a smaller one (the 1x1 black and white a zone ships) is uncompressed
        std::string Compression = I->Compression;
        {
            uint32_t W = 0, H = 0;
            if (ImageFacts::SizeOf(I->File, W, H)) Compression = ImageFacts::CompressionFor(Compression, W, H);
        }
        Gdt::Fields F = { { "baseImage", BaseImageOf(I->File) }, { "compressionMethod", Compression },
                          { "coreSemantic", I->Core }, { "imageType", "Texture" }, { "semantic", I->Semantic },
                          { "streamable", "1" }, { "type", "image" } };
        if (I->FromAlpha) F.emplace_back("fromAlpha", "1");
        std::stable_sort(F.begin(), F.end(), [](const auto& A, const auto& B) { return KeyLess(A.first, B.first); });
        ResolvedImage R;
        R.Name = Name;
        R.File = I->File;
        R.Fields = std::move(F);
        Out.ImagesOut.push_back(std::move(R));
    }
    return Out;
}

std::vector<std::string> Builder::Entries(const char* Eol) const
{
    const Resolved R = Resolve();
    std::vector<std::string> Out;
    // an image over 63 characters is named by EntryName and the material slots that name it follow (the publish step does the same)
    std::map<std::string, std::string> Renamed;
    for (const auto& I : R.ImagesOut)
        if (EntryName(I.Name) != I.Name) Renamed[I.Name] = EntryName(I.Name);
    for (const auto& M : R.Materials)
    {
        Gdt::Fields F = M.Fields;
        for (auto& [Key, Value] : F)
            if (const auto Hit = Renamed.find(Value); Hit != Renamed.end()) Value = Hit->second;
        Out.push_back(Gdt::Entry(EntryName(M.Name), "material.gdf", F, Eol));
    }
    for (const auto& I : R.ImagesOut)
    {
        Gdt::Fields F = I.Fields;
        for (auto& [Key, Value] : F)
            if (Key == "baseImage") Value = Gdt::Path(Value);
        Out.push_back(Gdt::Entry(EntryName(I.Name), "image.gdf", F, Eol));
    }
    return Out;
}

std::string Builder::NamesJson() const
{
    nlohmann::ordered_json Names = nlohmann::ordered_json::object(), Twice = nlohmann::ordered_json::object();
    for (const auto& [Name, M] : Defined) Names[Name] = M.Source;
    for (const auto& [Name, Sources] : Collisions) Twice[Name] = std::vector<std::string>(Sources.begin(), Sources.end());
    const nlohmann::ordered_json Root{ { "materials", Names }, { "collisions", Twice } };
    return Root.dump(1, '\t', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

namespace
{
    // XMODEL_EXPORT's bone count: one is a rigid model
    bool Rigid(const fs::path& File)
    {
        std::ifstream In(File);
        std::string Line;
        for (int K = 0; K < 64 && std::getline(In, Line); K++)
            if (Line.rfind("NUMBONES ", 0) == 0) return std::atoi(Line.c_str() + 9) <= 1;
        return true;
    }

    std::string Generic(const fs::path& P) { return P.generic_string(); }

}

std::vector<std::string> SurfaceChoicesOf(const fs::path& ModToolsRoot)
{
    std::vector<std::string> Options;
    if (ModToolsRoot.empty()) return Options;
    Bo3Target::Awi::Schema Schema;
    std::string Error;
    if (!Bo3Target::Awi::Load(ModToolsRoot, "material", Schema, Error)) return Options;
    if (const Bo3Target::Awi::Entry* E = Schema.Find("surfaceType"); E != nullptr && E->OptionsKnown) Options = E->Options;
    return Options;
}

int LodNumber(const std::string& Digits)
{
    int V = 0;
    const char* End = std::to_address(Digits.end());
    const auto R = std::from_chars(Digits.data(), End, V);
    return R.ec == std::errc() ? V : std::numeric_limits<int>::max();   // a long run of digits is capped, never thrown on
}

void WriteForModels(const DiskGame::Context& C, DiskGame::Totals& T)
{
    const fs::path Out(C.OutRoot);
    const fs::path Models = Out / DiskGame::KindFolder(DiskGame::Kind::Model);
    const std::string Folder = Out.filename().string();
    // <out> copied to <BO3>/model_export/graywolf/<folder>/: its files below the BO3 root
    const fs::path Root = fs::path("model_export") / "graywolf" / Folder;
    const auto Below = [&](const fs::path& File) {
        std::error_code Ec;
        const fs::path R = fs::relative(File, Out, Ec);
        return Ec || R.empty() || *R.begin() == ".." ? std::string() : Generic(R);
    };
    Builder B([&](const fs::path& File) {
        const std::string R = Below(File);
        return R.empty() ? std::string() : Generic(Root / R);
    }, "material_gdt_");
    B.SetRunRoot(Out);
    // Convert only when a target is set: plain --material-gdt on stays what it was
    B.ConvertImages(C.ConvertImages);
    if (C.ConvertImages) B.TechsetData(&Bo3Target::Techsets::Shared());

    std::vector<fs::path> Sidecars;
    std::error_code Ec;
    for (fs::recursive_directory_iterator It(Models, Ec), End; !Ec && It != End; It.increment(Ec))
    {
        const std::string Name = It->path().filename().string();
        if (It->is_regular_file(Ec) && Name.size() > 15 && Name.compare(Name.size() - 15, 15, ".materials.json") == 0) Sidecars.push_back(It->path());
    }
    std::sort(Sidecars.begin(), Sidecars.end());
    // the models' XMODEL_EXPORT files by model folder, LOD by LOD
    std::map<fs::path, std::vector<fs::path>> Xmodels;
    const std::regex Lod(R"(_lod(\d+)$)", std::regex::icase);
    for (const fs::path& S : Sidecars)
    {
        Materials::Sidecar Side;
        if (!Materials::ReadSidecar(S.string(), Side))
        {
            T.Counters["material_gdt_sidecars_unread"]++;
            continue;
        }
        T.Counters["material_gdt_sidecars"]++;
        for (size_t K = 0; K < Side.Materials.size(); ++K)
            B.Add(Side.Materials[K], S.parent_path(), K < Side.Sources.size() ? &Side.Sources[K] : nullptr, Side.Game);
        const std::string Stem = S.filename().string().substr(0, S.filename().string().size() - 15);
        const fs::path X = S.parent_path() / (Stem + ".XMODEL_EXPORT");
        if (std::regex_search(Stem, Lod) && fs::is_regular_file(X, Ec)) Xmodels[S.parent_path()].push_back(X);
    }
    for (const auto& [K, N] : B.Counters()) T.Counters[K] += N;
    if (B.Empty()) return;

    std::vector<std::string> Entries = B.Entries("\r\n");
    // an xmodel per model folder: its LODs (the first four) by number
    static const char* LodKeys[] = { "filename", "mediumLod", "lowLod", "lowestLod" };
    std::vector<std::string> ModelEntries;
    for (auto& [Dir, Files] : Xmodels)
    {
        std::sort(Files.begin(), Files.end(), [&](const fs::path& A, const fs::path& Bp) {
            std::smatch Ma, Mb;
            const std::string Sa = A.stem().string(), Sb = Bp.stem().string();
            std::regex_search(Sa, Ma, Lod);
            std::regex_search(Sb, Mb, Lod);
            return LodNumber(Ma[1].str()) < LodNumber(Mb[1].str());
        });
        const std::string Name = AssetName(Dir.filename().string());
        if (Name.empty()) continue;
        Gdt::Fields F;
        for (size_t K = 0; K < Files.size() && K < 4; K++)
        {
            const std::string R = Below(Files[K]);
            if (!R.empty()) F.emplace_back(LodKeys[K], Gdt::Path(Generic(fs::path("graywolf") / Folder / R)));
        }
        if (F.empty()) continue;
        F.emplace_back("type", Rigid(Files.front()) ? "rigid" : "animated");
        ModelEntries.push_back(Gdt::Entry(Name, "xmodel.gdf", F));
        T.Counters["material_gdt_xmodels"]++;
    }
    Entries.insert(Entries.begin(), ModelEntries.begin(), ModelEntries.end());

    const fs::path Dir = Out / "source_data";
    fs::create_directories(Dir, Ec);
    const fs::path File = Dir / ("graywolf_" + AssetName(Folder) + ".gdt");
    const std::string Text = Gdt::Document(Entries), Names = B.NamesJson();
    if (!ExportWriters::WriteBytes(File, reinterpret_cast<const uint8_t*>(Text.data()), Text.size()) ||
        !ExportWriters::WriteBytes(fs::path(File).replace_extension(".names.json"), reinterpret_cast<const uint8_t*>(Names.data()), Names.size()))
        T.Fail("", DiskGame::Kind::Model, File.filename().string(), "write failed");
    else
        T.Counters["material_gdt_files"]++;
}

bool ModelFilesUseAssetNames()
{
    return UseAssetNames;
}

int SelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Condition, const char* Name, const std::string& Got) {
        if (Condition) return;
        ++Failed;
        Failures += std::string("FAIL MaterialGdt ") + Name + " (got '" + Got + "')\n";
    };
    namespace Tc = Bo3Target::Techsets;
    const Tc::Data& Data = Tc::Shared();
    if (!Data.HasCatalog())
    {
        Check(false, "the shipped techset data is there", Data.Problems().empty() ? "" : Data.Problems().front());
        return Failed;
    }
    std::error_code Ec;
    const fs::path Dir = fs::temp_directory_path() / ("graywolf_selftest_matgdt_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(Dir, Ec);
    fs::create_directories(Dir, Ec);
    for (const char* Name : { "c.png", "n.png", "g.png", "o.png", "s.png", "e.png" })
    {
        std::ofstream F(Dir / Name, std::ios::binary);
        F << "x";
    }
    const auto Base = [](const fs::path& File) { return File.generic_string(); };
    const auto Map = [](const char* Path, bool Invert = false) {
        Materials::Map M;
        M.Path = Path;
        M.Invert = Invert;
        return M;
    };
    enum Feature { Gloss = 1, Occ = 2, Spec = 4, Cutout = 8, Emissive = 16 };
    const auto Make = [&](const char* Name, int Features) {
        Materials::Description D;
        D.Name = Name;
        D.Albedo = Map("c.png");
        D.Normal = Map("n.png");
        if (Features & Gloss) D.Roughness = Map("g.png", true);    // Invert set: the map already is a gloss
        if (Features & Occ) D.Occlusion = Map("o.png");
        if (Features & Spec) D.Specular = Map("s.png");
        if (Features & Emissive) D.Emissive = Map("e.png");
        if (Features & Cutout) D.Alpha = Materials::AlphaMode::Cutout;
        return D;
    };
    const auto Value = [](const Builder::ResolvedMaterial& M, const std::string& Key) {
        for (const auto& [K, V] : M.Fields)
            if (K == Key) return V;
        return std::string("(no key)");
    };
    const auto One = [&](const Materials::Description& D, const Tc::Data* Targets, const std::string& Techset, Builder::Resolved& Out, std::map<std::string, size_t>* Counters = nullptr, const char* Game = "bo4", int64_t RenderFlags = -1) {
        Builder B(Base, "t_");
        B.TechsetData(Targets);
        Materials::Source Src;
        Src.Name = D.Name;
        Src.Techset = Techset;
        Src.RenderFlags = RenderFlags;
        B.Add(D, Dir, &Src, Game);
        Out = B.Resolve();
        if (Counters != nullptr) *Counters = B.Counters();
        return Out.Materials.size() == 1;
    };

    // usage: <not in editor> unless the builder was told the map draws the material; then chosen from the BO3 techset it got and the measure
    {
        Check(Data.Usage().Loaded(), "the shipped usage rules are there", Data.Problems().empty() ? "" : Data.Problems().front());
        Builder B(Base, "t_");
        B.TechsetData(&Data);
        Materials::Source Src;
        Materials::Description A = Make("mtl_floor", 0), C = Make("mtl_model_only", 0);
        Src.Name = A.Name;
        B.Add(A, Dir, &Src, "bo4");
        Src.Name = C.Name;
        B.Add(C, Dir, &Src, "bo4");
        std::string Before, UsedA, UsedC;
        for (const auto& M : B.Resolve().Materials) Before += Value(M, "usage") + ";";
        Tc::MapUse Floor;
        Floor.Floor = 90, Floor.Wall = 10, Floor.Via = "world#0";
        B.SetMapUse({ { "mtl_floor", Floor } });
        for (const auto& M : B.Resolve().Materials) (M.Name == "mtl_floor" ? UsedA : UsedC) = Value(M, "usage");
        Check(Before == "<not in editor>;<not in editor>;", "usage defaults to <not in editor>", Before);
        Check(UsedA == "floor" && UsedC == "<not in editor>", "a material the map draws gets its usage, the others keep <not in editor>", UsedA + " / " + UsedC);
    }
    {
        // the decision follows the BO3 techset the material got, not the source name: the same measure (a wall) gives decal for a decal techset
        // and a window for glass, a terrain mesh gives terrain over the decal category, and an unmeasured material falls back with a note
        const auto UsageOf = [&](const Materials::Description& D, const char* Techset, const Tc::MapUse& Use, std::string& Type, std::string& Category, bool& Noted) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Materials::Source Src;
            Src.Name = D.Name;
            Src.Techset = Techset;
            B.Add(D, Dir, &Src, "bo3");
            B.SetMapUse({ { D.Name, Use } });
            const Builder::Resolved R = B.Resolve();
            if (R.Materials.size() != 1) return std::string("(none)");
            Type = Value(R.Materials[0], "materialType");
            Category = Value(R.Materials[0], "materialCategory");
            Noted = false;
            for (const auto& N : R.Materials[0].Notes) Noted |= N.Code == "material.usage_fallback";
            return Value(R.Materials[0], "usage");
        };
        std::string Type, Category;
        bool Noted = false;
        Tc::MapUse Wall;
        Wall.Wall = 100, Wall.Via = "world#1";
        const std::string Decal = UsageOf(Make("mtl_marking", 0), "lit_decal", Wall, Type, Category, Noted);
        Check(Category == "Decal" && Decal == "decal" && !Noted, "usage: a material whose BO3 techset is a decal is a decal", Category + "/" + Type + "/" + Decal);
        Tc::MapUse Layer = Wall;
        Layer.Terrain = true;
        const std::string Ter = UsageOf(Make("mtl_layer", 0), "lit_decal_reveal", Layer, Type, Category, Noted);
        Check(Ter == "terrain", "usage: a terrain mesh's layer is terrain over the decal category", Category + "/" + Type + "/" + Ter);
        const std::string Glass = UsageOf(Make("mtl_pane", 0), "glass", Wall, Type, Category, Noted);
        Check(Type.rfind("glass", 0) == 0 && Glass == "window", "usage: a glass techset is a window", Type + "/" + Glass);
        Tc::MapUse None;
        None.Via = "world#2";
        const std::string Fb = UsageOf(Make("mtl_unseen", 0), "lit", None, Type, Category, Noted);
        Check(Fb == "interior wall" && Noted, "usage: a material with no measured triangle gets interior wall and a material.usage_fallback note", Fb);
        Check(Fb != "<not in editor>", "usage: a material the map draws is never <not in editor>", Fb);
        // no catalog behind the builder (the fixed rules write only lit techsets): the source's own decal class stands in
        {
            Builder Old(Base, "t_");
            Materials::Source Src;
            Src.Name = "mtl_old_decal";
            Src.Techset = "lit_decal";
            Old.Add(Make("mtl_old_decal", 0), Dir, &Src, "bo3");
            Old.SetMapUse({ { "mtl_old_decal", Wall } });
            const Builder::Resolved R = Old.Resolve();
            Check(R.Materials.size() == 1 && Value(R.Materials[0], "usage") == "decal", "usage: with no catalog a source decal is still a decal", R.Materials.empty() ? "(none)" : Value(R.Materials[0], "usage"));
        }
    }
    // surfaceType: the description's own, never a guess; <none> with a note when the game gave none or the awi has no such option
    {
        const auto SurfaceOf = [&](const char* Type, const char* Why, const std::vector<std::string>& Options, std::string& Note, std::map<std::string, size_t>& Counters, int64_t Flags = -1) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            B.SurfaceOptions(Options);
            Materials::Description D = Make("mtl_surface", 0);
            D.SurfaceType = Type;
            D.SurfaceWhy = Why;
            Materials::Source Src;
            Src.Name = D.Name;
            Src.SurfaceFlags = Flags;
            B.Add(D, Dir, &Src, "bo4");
            const auto R = B.Resolve();
            Counters = B.Counters();
            Note.clear();
            if (!R.Materials.empty())
                for (const auto& N : R.Materials[0].Notes)
                    if (N.Code.rfind("material.surface_", 0) == 0) Note = N.Code;
            return R.Materials.empty() ? std::string("(none)") : Value(R.Materials[0], "surfaceType");
        };
        std::string Note;
        std::map<std::string, size_t> Counters;
        const std::vector<std::string> Options = { "<error>", "<none>", "concrete", "metal" };
        Check(SurfaceOf("metal", "", Options, Note, Counters) == "metal" && Counters["t_surface_metal"] == 1, "a surface type the awi holds is written", "");
        Check(SurfaceOf("metal", "", {}, Note, Counters) == "metal", "without the awi's options the game's name is written as it is", "");
        Check(SurfaceOf("ladder", "", Options, Note, Counters) == "<none>" && Note == "material.surface_unmapped" && Counters["t_surface_unmapped"] == 1, "a name the awi lacks is <none> with a surface_unmapped note", Note);
        Check(SurfaceOf("", "no_table", Options, Note, Counters) == "<none>" && Note == "material.surface_unknown" && Counters["t_surface_unknown_no_table"] == 1, "a game with no table is <none> with a surface_unknown note", Note);
        Check(SurfaceOf("", "unnamed:0x2800000", Options, Note, Counters) == "<none>" && Counters["t_surface_unknown_unnamed"] == 1, "a value no row declares is unknown, not guessed", "");
        Check(SurfaceOf("", "none_in_source", Options, Note, Counters, 0x440A0) == "<none>" && Note == "material.surface_none_in_source" && Counters["t_surface_none_in_source"] == 1, "a word that was read and names no surface is <none> with a positive surface_none_in_source row", Note);
        // a description that only SAYS none_in_source (a hand-made or flag-less sidecar) is not evidence: no flags, -1, or a typed word give an unknown row and no positive row
        for (const int64_t Forged : { int64_t(-1), int64_t(0xD00000) })
            Check(SurfaceOf("", "none_in_source", Options, Note, Counters, Forged) == "<none>" && Note == "material.surface_unknown" && Counters["t_surface_none_in_source"] == 0 && Counters["t_surface_unknown_none_in_source_unverified"] == 1,
                  "a forged none_in_source (flags -1, or a typed word) is surface_unknown, never the positive row", Note + " " + std::to_string(Forged));
        // render class: a BO4 material whose techset flags were not read (-1) or read 0 (a stub) is reported; a decal word is not
        for (const int64_t Flags : { int64_t(-1), int64_t(0), int64_t(0x80000a) })
        {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Materials::Description D = Make("mtl_render", 0);
            Materials::Source Src;
            Src.Name = D.Name;
            Src.RenderFlags = Flags;
            B.Add(D, Dir, &Src, "bo4");
            Check(B.Counters().count("t_render_class_unread") == (Flags > 0 ? 0u : 1u), "render_class_unread is counted for -1 and 0, not for a decal word", std::to_string(Flags));
        }
        // a word never read is unknown, and gets no positive row
        Check(SurfaceOf("", "table_missing", Options, Note, Counters) == "<none>" && Note == "material.surface_unknown" && Counters["t_surface_unknown_table_missing"] == 1, "table_missing is unknown with a surface_unknown row", Note);
        Check(SurfaceOf("", "", Options, Note, Counters) == "<none>" && Counters["t_surface_unknown_not_read"] == 1, "a reader that gives nothing counts as not_read", "");
        Check(SurfaceOf("", "no_bo3_use:player", Options, Note, Counters) == "<none>" && Note == "material.surface_no_bo3_use" && Counters["t_surface_no_bo3_use"] == 1,
              "a surface no stock BO3 material uses is <none> with its own row, not unknown", Note);
    }
    // Modern Warfare Remastered: the techset grammar, the surface bit, the packs and the gloss range the adapter states (zonekit/profiles/h1)
    {
        const Materials::IAdapter& Mwr = Materials::AdapterFor("H1");
        const auto Described = [&](const char* Techset, int SortKey, int64_t Surface, bool Layer) {
            Materials::Source Src;
            Src.Name = "mtl_h1_probe";
            Src.Techset = Techset;
            Src.SortKey = SortKey;
            Src.SurfaceFlags = Surface;
            Src.Images.push_back({ "h1_probe_col.png", Materials::Role::Unknown, 0xA0AB1041u, 0 });
            Src.Images.push_back({ "h1_probe_nml.png", Materials::Role::Unknown, 0x59D30D0Fu, 0 });
            Src.Images.push_back({ "~h1_probe_spc&h1_probe_cos-l~1.png", Materials::Role::Unknown, 0x34ECCCB3u, 0 });
            if (Layer) Src.Images.push_back({ "h1_probe2_col.png", Materials::Role::Unknown, 0xB60D1850u, 0 });
            return Mwr.Describe(Src);
        };
        const Materials::Description Lit = Described("m_l_sm_lmpb_r0c0n0sd0om0_nfwpf", 2, 1ll << 38, false);
        Check(Lit.TechsetName == "lit_advanced_fullspec" && Lit.SurfaceType == "metalcar" && Lit.SurfaceWhy.empty() && Lit.Alpha == Materials::AlphaMode::Opaque,
              "mwr: a lit single-layer material is lit_advanced_fullspec, opaque, with the surface of its one bit", Lit.TechsetName + " " + Lit.SurfaceType);
        Check(Lit.Specular.Pack == Materials::PackKind::SpecularSqrtF0 && Lit.Roughness.Pack == Materials::PackKind::SpecularGlossAlpha && Lit.Roughness.Channel == 3 && Lit.Roughness.Invert &&
                  Lit.GlossStopsMin == 0.0f && Lit.GlossStopsMax == 16.0f,
              "mwr: the specular is sqrt(F0), the gloss its alpha in 0..16 stops", "");
        Check(!Lit.Naming.Empty() && Lit.LayersDropped == 0, "mwr: the image naming is declared, no layer is dropped on a single layer", "");
        const Materials::Description Decal = Described("m_l_sm_lmpb_r0c0n0sd0om0_ndw_aat_nfwpf", 0x10, 1ll << 4, false);
        Check(Decal.TechsetName == "lit_decal_plus" && Decal.Specular.Empty() && Decal.Alpha == Materials::AlphaMode::Blend, "mwr: ndw + aat is a decal (plus family for a non-metal, no specular map), blended", Decal.TechsetName);
        const Materials::Description Pane = Described("m_l_sm_lmpb_r0c0n0sd0om0_cltrans_nfwpf", 0x20, 1ll << 8, false);
        const Materials::Description PaneNoBit = Described("m_l_sm_lmpb_r0c0n0sd0om0_cltrans_nfwpf", 0x20, 1ll << 12, false);
        Check(Pane.TechsetName == "glass" && PaneNoBit.TechsetName == "lit_transparent_advanced_fullspec", "mwr: cltrans with the glass surface is glass, without it coloured glass", Pane.TechsetName + " / " + PaneNoBit.TechsetName);
        const Materials::Description Test = Described("m_l_sm_lmpb_r0c0n0sd0om0_aat_nfwpf", 2, 1ll << 7, false);
        Check(Test.TechsetName == "lit_alphatest_plus" && Test.Alpha == Materials::AlphaMode::Cutout, "mwr: aat without ndw is alpha tested, cutout", Test.TechsetName);
        const Materials::Description Ui = Described("2d_add_nfwpf", 0x3c, 0, false);
        Check(Ui.SkipWhy.rfind("ui_2d:", 0) == 0, "mwr: a 2d techset is skipped", Ui.SkipWhy);
        const Materials::Description Fx = Described("effect_blend_nfwpf", 0x35, 0, false);
        Check(Fx.TechsetName == "effect_lit_blend" && Fx.SurfaceWhy == "none_in_source", "mwr: an effect is effect_lit_blend and states no surface", Fx.TechsetName + " " + Fx.SurfaceWhy);
        const Materials::Description Unknown = Described("zz_unheard_of", 2, 0, false);
        Check(Unknown.TechsetName.empty() && Unknown.TechsetWhy == "no_rule", "mwr: a name no rule fits gets no techset and says why", Unknown.TechsetWhy);
        const Materials::Description NoLightmap = Described("m_l_sm_r0c0n0sd0_nfwpf", 2, 1ll << 21, false);
        Check(NoLightmap.TechsetName == "lit_plus" && NoLightmap.Specular.Empty(), "mwr: a lit name without the lmpb token is lit too; a non-metal (wood) takes lit_plus with no specular colour map", NoLightmap.TechsetName);
        {
            std::string Got;
            const bool Ok = ::H1TechsetRulesSelfTest(Dir.string(), Got);
            Check(Ok, "mwr: a missing rule table, a malformed row and an unknown condition are distinct errors, and a failed load is not kept", Got);
        }
        const Materials::Description Layered = Described("l_sm_lmpb_r0c0nl0sld0oml0_b1c1vl1nl1sl1oml1_nfwpf", 2, (1ll << 4) | (1ll << 5), true);
        Check(Layered.LayersDropped == 1 && Layered.SurfaceType.empty() && Layered.SurfaceWhy.rfind("ambiguous:", 0) == 0,
              "mwr: a layered blend counts its dropped layers, and a word of two bits is ambiguous until the reader picks the first layer bit", std::to_string(Layered.LayersDropped) + " " + Layered.SurfaceWhy);
        // the Builder: the layers_dropped row, the gloss range and the F0 conversion's slot (the gloss is extracted by a derive that works here: a failing one is C4's test)
        for (const char* F : { "c.png", "n.png", "g.png", "s.png" }) std::ofstream(Dir / F, std::ios::binary) << "x";
        Builder B(Base, "t_");
        B.TechsetData(&Data);
        B.ConvertImages(true);
        B.SetRunRoot(Dir);
        B.DeriveHook = [&Dir](const char* What, const Bo3Target::DeriveRequest&, fs::path& File) {
            File = Dir / (std::string("derived_") + What + ".png");
            std::ofstream(File, std::ios::binary) << "x";
            return true;
        };
        Materials::Source Src;
        Src.Name = "mtl_h1_layered";
        Materials::Description Written = Layered;
        Written.Name = "mtl_h1_layered";
        Written.Albedo = Map("c.png");
        Written.Normal = Map("n.png");
        Written.Roughness = Map("g.png", true);
        Written.Roughness.Channel = 3;
        Written.Roughness.Pack = Materials::PackKind::SpecularGlossAlpha;
        Written.Specular = Map("s.png");
        B.Add(Written, Dir, &Src, "mwr");
        const auto R = B.Resolve();
        bool Dropped = false;
        for (const auto& N : R.Materials.empty() ? std::vector<Builder::Note>() : R.Materials[0].Notes) Dropped = Dropped || N.Code == "material.layers_dropped";
        Check(Dropped && B.Counters().at("t_layers_dropped") == 1, "mwr: the Builder writes a material.layers_dropped row", "");
        {
            // B2, Builder side: a sqrt(F0) specular whose conversion fails is not published as it came: specColorMap stays empty and a material.f0_conversion_failed row names the image
            std::ofstream(Dir / "f0s.png", std::ios::binary) << "x";
            Builder Bf(Base, "t_");
            Bf.TechsetData(&Data);
            Bf.ConvertImages(true);
            Bf.SetRunRoot(Dir);
            Bf.DeriveHook = [](const char*, const Bo3Target::DeriveRequest&, fs::path&) { return false; };
            Materials::Description F0 = Written;
            F0.Name = "mtl_h1_f0fail";
            F0.LayersDropped = 0;
            F0.TechsetName = "lit_advanced_fullspec";
            F0.Specular = Map("f0s.png");
            F0.Specular.Pack = Materials::PackKind::SpecularSqrtF0;
            Materials::Source Sf;
            Sf.Name = F0.Name;
            Bf.Add(F0, Dir, &Sf, "mwr");
            const auto Rf = Bf.Resolve();
            bool Row = false;
            for (const auto& N : Rf.Materials.empty() ? std::vector<Builder::Note>() : Rf.Materials[0].Notes) Row = Row || (N.Code == "material.f0_conversion_failed" && N.Detail.find("f0s.png") != std::string::npos);
            Check(!Rf.Materials.empty() && Value(Rf.Materials[0], "specColorMap").empty() && Row, "mwr: a failed sqrt(F0) conversion leaves specColorMap empty and writes a material.f0_conversion_failed row naming the image",
                  Rf.Materials.empty() ? "(none)" : Value(Rf.Materials[0], "specColorMap"));
            // C4: the same failing derive leaves cosinePowerMap empty (the RGBA composite is never the gloss map) and writes a gate-failing row
            std::ofstream(Dir / "g.png", std::ios::binary) << "x";
            bool GlossRow = false;
            for (const auto& N : Rf.Materials.empty() ? std::vector<Builder::Note>() : Rf.Materials[0].Notes) GlossRow = GlossRow || N.Code == "material.gloss_conversion_failed";
            Check(!Rf.Materials.empty() && Value(Rf.Materials[0], "cosinePowerMap").empty() && GlossRow, "C4: a gloss derive failure leaves cosinePowerMap empty and writes material.gloss_conversion_failed",
                  Rf.Materials.empty() ? "(none)" : Value(Rf.Materials[0], "cosinePowerMap"));
        }
        {
            // C5: a reveal that cannot be reduced to one channel is not bound raw: alphaRevealMap empty, a row, reveal_image_bound not counted
            std::ofstream(Dir / "rvl.png", std::ios::binary) << "x";
            Builder Br(Base, "t_");
            Br.TechsetData(&Data);
            Br.ConvertImages(true);
            Br.SetRunRoot(Dir);
            Br.DeriveHook = [](const char*, const Bo3Target::DeriveRequest&, fs::path&) { return false; };
            Materials::Description Rv;
            Rv.Name = "mtl_h1_revealfail";
            Rv.Albedo = Map("c.png");
            Rv.Normal = Map("n.png");
            Rv.TechsetName = "lit_decal_reveal_plus";
            Rv.RevealSemantic = 0x34D849D5u;
            Materials::Source Sr;
            Sr.Name = Rv.Name;
            Sr.Images.push_back({ "rvl.png", Materials::Role::Unknown, 0x34D849D5u, 0 });
            Br.Add(Rv, Dir, &Sr, "mwr");
            const auto Rr = Br.Resolve();
            bool RevealRow = false;
            for (const auto& N : Rr.Materials.empty() ? std::vector<Builder::Note>() : Rr.Materials[0].Notes) RevealRow = RevealRow || N.Code == "material.reveal_conversion_failed";
            const auto Counted = Br.Counters().find("t_reveal_image_bound");
            Check(!Rr.Materials.empty() && Value(Rr.Materials[0], "alphaRevealMap").empty() && RevealRow && (Counted == Br.Counters().end() || Counted->second == 0),
                  "C5: a reveal derive failure binds no raw image, writes material.reveal_conversion_failed and does not count reveal_image_bound", Rr.Materials.empty() ? "(none)" : Value(Rr.Materials[0], "alphaRevealMap"));
            // D12: the channel of the reveal image comes from the adapter (the description), not from shared code
            ImageChannel Asked = ImageChannel::Red;
            Builder Bc(Base, "t_");
            Bc.TechsetData(&Data);
            Bc.ConvertImages(true);
            Bc.SetRunRoot(Dir);
            Bc.DeriveHook = [&](const char*, const Bo3Target::DeriveRequest& Request, fs::path& File) {
                Asked = Request.Channel;
                File = Dir / "derived_reveal.png";
                std::ofstream(File, std::ios::binary) << "x";
                return true;
            };
            Rv.RevealChannel = 2;
            Bc.Add(Rv, Dir, &Sr, "mwr");
            Bc.Resolve();
            Check(Asked == ChannelOf(2), "D12: the reveal channel asked of the derive is the description's (RevealChannel), not a constant in shared code", std::to_string(static_cast<int>(Asked)));
        }
        {
            // C6: no techset can be chosen: the fallback is still reported, and a rules_* reason reaches the gate
            Tc::Data None;
            None.LoadCatalogText(R"({"techsets":[{"n":"zz_unrelated","c":"Effect","f":"none","b":"Other","t":0,"s":[["colorMap","colorMap",null,null]]}]})");
            Builder Bn(Base, "t_");
            Bn.TechsetData(&None);
            Materials::Description Nd;
            Nd.Name = "mtl_h1_nonechosen";
            Nd.Albedo = Map("c.png");
            Nd.TechsetWhy = "rules_missing:test";
            Materials::Source Sn;
            Sn.Name = Nd.Name;
            Bn.Add(Nd, Dir, &Sn, "mwr");
            const auto Rn = Bn.Resolve();
            std::string FallbackDetail;
            for (const auto& N : Rn.Materials.empty() ? std::vector<Builder::Note>() : Rn.Materials[0].Notes)
                if (N.Code == "material.techset_fallback") FallbackDetail = N.Detail;
            Check(FallbackDetail.rfind("reason=rules_missing:test", 0) == 0, "C6: with no techset chosen material.techset_fallback is still written, so a rules_* reason reaches the gate", FallbackDetail);
        }
        {
            // B1/B3: the emissive map binds on an emissive family, the unbound textures are named, an ambiguous surface keeps its specular map
            const auto Described = [&](const char* Techset, int64_t Surface, bool WithEmissive, bool WithDetail) {
                Materials::Source Src;
                Src.Name = "mtl_h1_probe";
                Src.Techset = Techset;
                Src.SortKey = 2;
                Src.SurfaceFlags = Surface;
                Src.Images.push_back({ "p_col.png", Materials::Role::Unknown, 0xA0AB1041u, 0 });
                Src.Images.push_back({ "p_nml.png", Materials::Role::Unknown, 0x59D30D0Fu, 0 });
                Src.Images.push_back({ "~p_spc.png", Materials::Role::Unknown, 0x34ECCCB3u, 0 });
                if (WithEmissive) Src.Images.push_back({ "p_ems.png", Materials::Role::Unknown, 0x34614347u, 0 });
                if (WithDetail) Src.Images.push_back({ "p_detail.png", Materials::Role::Unknown, 0xEB529B4Du, 0 });
                return Materials::AdapterFor("H1").Describe(Src);
            };
            const Materials::Description Em = Described("m_l_sm_lmpb_r0c0n0sd0om0_nfwpf", 1ll << 4, true, false);
            const Materials::Description EmMetal = Described("m_l_sm_lmpb_r0c0n0sd0om0_nfwpf", 1ll << 12, true, false);
            const Materials::Description EmDecal = Described("m_l_sm_lmpb_r0c0n0sd0om0_ndw_aat_nfwpf", 1ll << 4, true, true);
            const Materials::Description Amb = Described("m_l_sm_lmpb_r0c0n0sd0om0_nfwpf", (1ll << 4) | (1ll << 5), false, false);
            Check(Em.TechsetName == "lit_emissive_plus" && !Em.Emissive.Empty() && EmMetal.TechsetName == "lit_emissive_advanced_fullspec" && !EmMetal.Emissive.Empty(),
                  "B1: an emissive map (0x34614347) binds, on lit_emissive_plus (non-metal) or lit_emissive_advanced_fullspec (metal)", Em.TechsetName + " " + EmMetal.TechsetName);
            Check(EmDecal.Emissive.Empty() && EmDecal.TexturesUnbound.find("emissive") != std::string::npos && EmDecal.TexturesUnbound.find("detail") != std::string::npos,
                  "B2: an emissive map on a decal family and a detail texture are named in TexturesUnbound", EmDecal.TexturesUnbound);
            Check(Amb.TechsetName == "lit_advanced_fullspec" && !Amb.Specular.Empty() && Amb.FamilyWhy == "surface_unknown_treated_as_metal",
                  "B3: an ambiguous surface keeps its specular map (treated as metal, with a row reason)", Amb.TechsetName + " " + Amb.FamilyWhy);
        }
        Check(!R.Materials.empty() && std::atof(Value(R.Materials[0], "glossRangeMax").c_str()) == 16.0 && std::atof(Value(R.Materials[0], "glossRangeMin").c_str()) == 0.0 && Value(R.Materials[0], "glossRangeMax") != "(no key)", "mwr: the adapter's gloss range is written", R.Materials.empty() ? "(none)" : Value(R.Materials[0], "glossRangeMax"));
    }
    // HydraX D-13: the BO3 adapter says a material with a *_combo image is not exported; the builder writes no entry and no image and reports it
    {
        Materials::Source Src;
        Src.Name = "mtl_dup";
        Src.Techset = ",mc/lit#e6142445";
        Src.Images.push_back({ "../images/i_mtl_dup_combo.png", Materials::Role::Unknown, 0, 0 });
        const Materials::Description Skipped = Materials::AdapterFor("T7").Describe(Src);
        Materials::Source Plain;
        Plain.Name = "mtl_ok";
        Plain.Techset = ",mc/lit#e6142445";
        Plain.Images.push_back({ "../images/i_mtl_ok_combo_not.png", Materials::Role::Unknown, 0, 0 });
        Plain.Images.push_back({ "../images/I_MTL_OK_c.png", Materials::Role::Unknown, 0, 0 });
        Plain.Images.push_back({ "../images/I_MTL_OK_COMBO.png", Materials::Role::Unknown, 0, 0 });     // the test is ordinal: another case is not _combo
        const Materials::Description Kept = Materials::AdapterFor("T7").Describe(Plain);
        Check(Skipped.SkipWhy.rfind("combo_image:", 0) == 0 && Kept.SkipWhy.empty(), "the BO3 adapter skips a material whose image name ends in _combo (ordinal, as HydraX), and no other", Skipped.SkipWhy + " | " + Kept.SkipWhy);
        Check(Materials::AdapterFor("bo4").Describe(Src).SkipWhy.empty(), "the rule is the BO3 adapter's: another game's adapter does not skip", "");
        Builder B(Base, "t_");
        B.TechsetData(&Data);
        Materials::Description D = Make("mtl_dup", 0);
        D.SkipWhy = Skipped.SkipWhy;
        B.Add(D, Dir, &Src, "bo3");
        const auto R = B.Resolve();
        Check(R.Materials.empty() && R.ImagesOut.empty() && R.Skipped.size() == 1 && R.Skipped[0].second.Code == "material.skipped_combo_image" && B.Counters().at("t_skipped_by_game_rule") == 1,
              "a skipped material has no entry, no image, one report row and a counter", std::to_string(R.Materials.size()) + "/" + std::to_string(R.Skipped.size()));
    }
    // BO3 takes materialType from the source techset's name; a name the catalog lacks, or no techset, is written by the slot rules with a
    // material.techset_fallback row (a published model names the material, so it is not skipped)
    {
        Materials::Source Src;
        Src.Name = "mtl_named";
        Src.Techset = ",mc/lit_advanced#e6142445";
        Src.Images.push_back({ "../images/i_mtl_named_c.png", Materials::Role::Unknown, 0xA0AB1041u, 0 });
        const Materials::Description Named = Materials::AdapterFor("T7").Describe(Src);
        Src.Techset = "mc/no_such_techset_zz#1";
        const Materials::Description Unknown = Materials::AdapterFor("T7").Describe(Src);
        Src.Techset.clear();
        const Materials::Description None = Materials::AdapterFor("T7").Describe(Src);
        Src.Techset = "<unreadable>";
        const Materials::Description Unread = Materials::AdapterFor("T7").Describe(Src);
        Check(Named.TechsetName == "lit_advanced" && Named.SkipWhy.empty() && Named.TechsetWhy.empty() && None.SkipWhy.empty() && None.TechsetName.empty() && None.TechsetWhy == "no_techset" &&
              Unread.SkipWhy.empty() && Unread.TechsetName.empty() && Unread.TechsetWhy == "name_unread",
              "the BO3 adapter names the techset (folder, # and the leading comma cut); no techset and an unread name stay apart and neither skips the material", Named.TechsetName + " | " + None.TechsetWhy + " | " + Unread.TechsetWhy);
        Check(Materials::AdapterFor("bo4").Describe(Src).TechsetName.empty(), "another game's adapter sets no techset name", "");
        const auto Run = [&](const Materials::Description& Described, Builder::Resolved& R) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Materials::Description D = Make("mtl_named", 0);
            D.TechsetName = Described.TechsetName;
            D.TechsetWhy = Described.TechsetWhy;
            D.SkipWhy = Described.SkipWhy;
            B.Add(D, Dir, nullptr, "bo3");
            R = B.Resolve();
        };
        {
            // the game rules ride in the sidecar: the publish step builds from the file, not from the adapter
            Materials::Sidecar Side, Back;
            Side.Game = "BO3";
            Side.Adapter = "bo3";
            Side.Materials.push_back(Named);
            Side.Materials.back().SkipWhy = "combo_image:image=x";
            Side.Materials.back().TechsetWhy = "no_techset";
            Side.Materials.back().RevealSemantic = 0x34D849D5u;
            const std::string SidePath = (Dir / "rules.materials.json").string();
            const bool Wrote = Materials::WriteSidecar(SidePath, Side), Read = Materials::ReadSidecar(SidePath, Back);
            Check(Wrote && Read && Back.Materials.size() == 1 && Back.Materials[0].TechsetName == "lit_advanced" && Back.Materials[0].SkipWhy == "combo_image:image=x" && Back.Materials[0].TechsetWhy == "no_techset" && Back.Materials[0].RevealSemantic == 0x34D849D5u,
                  "the sidecar keeps a game's techset name, skip reason and reveal slot", std::format("{} {}", Wrote, Read));
        }
        Builder::Resolved Rn, Ru, Rz, Rr;
        Run(Named, Rn);
        Run(Unknown, Ru);
        Run(None, Rz);
        Run(Unread, Rr);
        const auto FallbackRow = [](const Builder::Resolved& R) {
            if (R.Materials.size() == 1)
                for (const auto& N : R.Materials[0].Notes)
                    if (N.Code == "material.techset_fallback" && N.Detail.rfind("reason=", 0) == 0) return N.Detail;
            return std::string();
        };
        Check(Rn.Materials.size() == 1 && Value(Rn.Materials[0], "materialType") == "lit_advanced", "a named BO3 techset is the materialType whatever the slots would pick", Rn.Materials.empty() ? "" : Value(Rn.Materials[0], "materialType"));
        Check(Ru.Materials.size() == 1 && Ru.Skipped.empty() && FallbackRow(Ru).find("reason=not_in_catalog") != std::string::npos && FallbackRow(Ru).find("no_such_techset_zz") != std::string::npos,
              "a BO3 techset the catalog lacks is not skipped: slot rules and a techset_fallback row naming the source", FallbackRow(Ru));
        Check(Rz.Materials.size() == 1 && FallbackRow(Rz).find("reason=no_techset") != std::string::npos && Rr.Materials.size() == 1 && FallbackRow(Rr).find("reason=name_unread") != std::string::npos,
              "no techset and an unread name are not skipped either, and the rows tell them apart", FallbackRow(Rz) + " | " + FallbackRow(Rr));
        {
            // a camo techset named by the game is never written: slot rules, a fallback row (",mc/lit_weapon_camo#..." cuts to lit_weapon_camo)
            Src.Techset = ",mc/lit_weapon_camo#e6142445";
            const Materials::Description Camo = Materials::AdapterFor("T7").Describe(Src);
            Builder::Resolved Rc;
            Run(Camo, Rc);
            Check(Camo.TechsetName == "lit_weapon_camo" && Rc.Materials.size() == 1 && Value(Rc.Materials[0], "materialType").find("camo") == std::string::npos &&
                  FallbackRow(Rc).find("reason=camo_excluded") != std::string::npos,
                  "a camo techset named by the game is not written: slot rules and a techset_fallback row", Rc.Materials.empty() ? "" : Value(Rc.Materials[0], "materialType") + " | " + FallbackRow(Rc));
        }
    }
    // colour plus normal: lit; every key of the techset and the stock defaults are written
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_plain", 0), &Data, "", R);
        Check(Ok && Value(R.Materials[0], "materialType") == "lit" && Value(R.Materials[0], "materialCategory") == "Geometry", "colour and normal give lit", Ok ? Value(R.Materials[0], "materialType") : "");
        Check(Ok && R.Materials[0].Fields.size() > 100 && Value(R.Materials[0], "heatmap") == "" &&
              Value(R.Materials[0], "glossSurfaceType") == "<custom>" && Value(R.Materials[0], "colorTint") == "1 1 1 1", "the entry holds every key with the stock defaults",
              Ok ? std::to_string(R.Materials[0].Fields.size()) : "");
    }
    // a Treyarch normal (X and Y, no pack kind, no flip: ImageRoles has rebuilt its Z in the file) is the
    // same image under --target: the same name (no second tag), nothing derived
    {
        const auto Names = [&](bool Convert) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            B.ConvertImages(Convert);
            Materials::Source Src;
            Materials::Description D = Make("mtl_treyarch", 0);
            D.NormalEnc = Materials::NormalEncoding::XY;
            Src.Name = D.Name;
            B.Add(D, Dir, &Src, "bo4");
            const auto R = B.Resolve();
            std::string Out = R.Materials.empty() ? std::string("(none)") : Value(R.Materials[0], "normalMap");
            for (const auto& I : R.ImagesOut) Out += ";" + I.Name;
            return Out;
        };
        const std::string Plain = Names(false), Converted = Names(true);
        Check(Plain == Converted && Plain.find("_n_n") == std::string::npos, "a Treyarch normal under --target keeps its image and its name", Plain + " / " + Converted);
    }
    // gloss and occlusion: a techset that has both slots, and the images are written to them
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_gloss", Gloss | Occ), &Data, "", R);
        const Tc::Techset* T = Ok ? Data.Find(Value(R.Materials[0], "materialType")) : nullptr;
        Check(T != nullptr && T->Name != "lit" && T->ByKey("cosinePowerMap") != nullptr && T->ByKey("occMap") != nullptr &&
              Value(R.Materials[0], "cosinePowerMap") != "" && Value(R.Materials[0], "occMap") != "", "gloss and occlusion give a techset that has those slots, both written", T ? T->Name : "(none)");
    }
    // a stock image is judged by what the Mod Tools say it is: a stock specular mask never lands in a colour specular slot
    {
        const auto Build = [&](const std::string& StockSemantic, Builder::Resolved& R, std::map<std::string, size_t>& Counters) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            B.SetStockImages([StockSemantic](const std::string& Name) { return Name == "i_s" ? StockSemantic : std::string(); });   // the spec map "s.png" is the stock image i_s
            Materials::Source Src;
            Src.Name = "mtl_stock_spec";
            B.Add(Make("mtl_stock_spec", Gloss | Occ | Spec), Dir, &Src, "iw8");
            R = B.Resolve();
            Counters = B.Counters();
            return R.Materials.size() == 1;
        };
        const auto SpecSlot = [&](const Builder::Resolved& R) -> const Tc::Slot* {
            const Tc::Techset* T = Data.Find(Value(R.Materials[0], "materialType"));
            return T == nullptr ? nullptr : T->ByKey("specColorMap");
        };
        Builder::Resolved Plain, Mask, Colour, Odd;
        std::map<std::string, size_t> CPlain, CMask, CColour, COdd;
        const bool Ok = Build("", Plain, CPlain) && Build("specularMask", Mask, CMask) && Build("specularMap", Colour, CColour) && Build("bogusMap", Odd, COdd);
        const Tc::Slot* SP = Ok ? SpecSlot(Plain) : nullptr;
        const Tc::Slot* SM = Ok ? SpecSlot(Mask) : nullptr;
        const Tc::Slot* SC = Ok ? SpecSlot(Colour) : nullptr;
        Check(SP != nullptr && Tc::Accepts(*SP, "specularMap") && CPlain["t_slots_stock_semantic_read"] == 0, "stock semantic: with nothing read the colour spec map takes a colour specular slot", Ok ? Value(Plain.Materials[0], "materialType") : "");
        Check(SM != nullptr && Tc::Accepts(*SM, "specularMask") && SM->Semantic == "specularMask" && Value(Mask.Materials[0], "specColorMap") == "i_s" && CMask["t_slots_stock_semantic_read"] == 1,
              "stock semantic: a stock specular mask gets a techset whose specular slot takes a mask, and stays bound", Ok ? Value(Mask.Materials[0], "materialType") : "");
        Check(SC != nullptr && Tc::Accepts(*SC, "specularMap") && Value(Colour.Materials[0], "specColorMap") == "i_s", "stock semantic: a stock colour specular map keeps a colour specular slot", Ok ? Value(Colour.Materials[0], "materialType") : "");
        bool Reported = false;
        if (Ok)
            for (const auto& N : Odd.Materials[0].Notes) Reported |= N.Code == "material.unmapped_slot";
        Check(Ok && Reported && (Value(Odd.Materials[0], "specColorMap") == "(no key)" || Value(Odd.Materials[0], "specColorMap").empty()),
              "stock semantic: a stock image no slot takes is reported unmapped and not bound", Ok ? Value(Odd.Materials[0], "specColorMap") : "");
        // a code image of another meaning: a mask code image offered to a colour specular slot is not bound
        {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Materials::Source Src;
            Src.Name = "mtl_code_spec";
            Materials::Description D = Make("mtl_code_spec", Gloss | Occ | Spec);
            D.Specular = Map("$specular_mask.png");    // a code image (no file), a mask by what the Mod Tools say it is
            B.Add(D, Dir, &Src, "iw8");
            const Builder::Resolved R = B.Resolve();
            const Tc::Slot* S = R.Materials.size() == 1 ? SpecSlot(R) : nullptr;
            const std::string Bound = R.Materials.size() == 1 ? Value(R.Materials[0], "specColorMap") : "";
            Check(R.Materials.size() == 1 && (S == nullptr || Tc::Accepts(*S, "specularMask")) && (Bound.empty() || Bound == "(no key)" || Bound == "$specular_mask"),
                  "code image: a mask code image never sits in a slot that takes a colour specular map", Bound);
            if (S != nullptr && Bound == "$specular_mask") Check(Tc::Accepts(*S, "specularMask"), "code image: bound only where the slot takes a mask", Bound);
        }
    }
    // follow-up tests: the colour or mask specular from the picture, the reveal image, the stock name asked again with the final name
    {
        // the colour or mask specular comes from the source image's format: BC4 (80) is a mask, BC7 (98) a colour, 0 (unread) is HydraX's null
        const auto Type = [&](uint32_t Format) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Materials::Source Src;
            Src.Name = "mtl_cs";
            Src.RenderFlags = 0x4;       // opaque
            Src.Images = { { "c.png", Materials::Role::Unknown, Tc::SlotHash("colorMap"), 0 }, { "n.png", Materials::Role::Unknown, Tc::SlotHash("normalMap"), 0 },
                           { "g.png", Materials::Role::Unknown, Tc::SlotHash("glossMap"), 0 }, { "o.png", Materials::Role::Unknown, Tc::SlotHash("aoMap"), 0 },
                           { "s.png", Materials::Role::Unknown, Tc::SlotHash("specColorMap"), Format } };
            Materials::Description D = Make("mtl_cs", Gloss | Occ);
            D.Specular = Map("s.png");
            B.Add(D, Dir, &Src, "bo4");
            const Builder::Resolved R = B.Resolve();
            return R.Materials.size() == 1 ? Value(R.Materials[0], "materialType") : std::string("(none)");
        };
        const std::string Mask = Type(80), Colour = Type(98), Unread = Type(0);
        const Tc::Techset* TM = Data.Find(Mask);
        const Tc::Techset* TC = Data.Find(Colour);
        Check(TM != nullptr && TC != nullptr && !TM->HasTrait(Tc::Trait::ColorSpec) && TC->HasTrait(Tc::Trait::ColorSpec),
              "colour or mask specular: a BC4 specular image gets a mask techset, a BC7 one a colour-specular techset", Mask + " / " + Colour);
        {
            const Tc::Techset* TU = Data.Find(Unread);
            Check(TU != nullptr && Unread != "(none)", "colour or mask specular: an unread format is HydraX's null (a techset is still chosen, nothing is guessed)", Unread);
        }

        // the reveal image of a hashed material is bound to the key the techset gives that texture
        {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            std::ofstream(Dir / "r.png", std::ios::binary) << "x";
            Materials::Source Src;
            Src.Name = "mtl_reveal";
            Src.RenderFlags = 0x80000a;       // a decal
            Src.Images = { { "c.png", Materials::Role::Unknown, Tc::SlotHash("colorMap") }, { "n.png", Materials::Role::Unknown, Tc::SlotHash("normalMap") },
                           { "r.png", Materials::Role::Unknown, Tc::SlotHash("revealMap") } };
            B.Add(Make("mtl_reveal", 0), Dir, &Src, "bo4");
            const Builder::Resolved R = B.Resolve();
            const std::string Bound = R.Materials.size() == 1 ? Value(R.Materials[0], "alphaRevealMap") : "(none)";
            const Tc::Techset* TR = R.Materials.size() == 1 ? Data.Find(Value(R.Materials[0], "materialType")) : nullptr;
            Check(TR != nullptr && TR->ByKey("alphaRevealMap") != nullptr && Bound.rfind("i_r", 0) == 0, "reveal: the source reveal image is bound to alphaRevealMap of the chosen techset", Bound + " on " + (TR ? TR->Name : "(none)"));
        }

        // selection judged i_s; the final name of the image carries a tag (one stem, two meanings): the answer is asked again and the difference said
        {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            auto Asked = std::make_shared<int>(0);      // only the first question (the selection of mtl_late_a) is answered: the name "i_s" is judged a mask once
            B.SetStockImages([Asked](const std::string& Name) { return Name == "i_s" && (*Asked)++ == 0 ? std::string("specularMask") : std::string(); });
            Materials::Source Src;
            Src.Name = "mtl_late_a";
            B.Add(Make("mtl_late_a", Spec), Dir, &Src, "iw8");
            Materials::Description Other = Make("mtl_late_b", 0);
            Other.Occlusion = Map("s.png");
            Src.Name = "mtl_late_b";
            B.Add(Other, Dir, &Src, "iw8");
            const Builder::Resolved R = B.Resolve();
            bool Late = false;
            for (const auto& M : R.Materials)
                if (M.Name == "mtl_late_a")
                    for (const auto& N : M.Notes) Late |= N.Code == "material.stock_semantic_late";
            std::string Seen;
            for (const auto& M : R.Materials) { Seen += M.Name + ":"; for (const auto& N : M.Notes) Seen += N.Code + ","; for (const auto& [K, V] : M.Fields) if (K == "specColorMap" || K == "occMap") Seen += K + "=" + V + ","; Seen += " "; }
            Check(R.Materials.size() == 2 && Late, "stock name: when the final image name differs from the judged one the stock semantic is asked again and reported", Seen);
            std::string BoundA, SemA, CoreA;
            for (const auto& M : R.Materials)
                if (M.Name == "mtl_late_a")
                    for (const auto& [K, V] : M.Fields)
                        if (K == "specColorMap") BoundA = V;
            for (const auto& I : R.ImagesOut)
                if (I.Name == BoundA)
                    for (const auto& [K, V] : I.Fields)
                    {
                        if (K == "semantic") SemA = V;
                        if (K == "coreSemantic") CoreA = V;
                    }
            Check(!BoundA.empty() && BoundA != "i_s" && SemA == "specularMask" && CoreA == "Linear1ch",
                  "stock name: an image that is no longer stock gets the metadata of the slot it sits in, not the colour metadata it had", BoundA + " " + SemA + " " + CoreA);
        }
    }

    // spec: a colour specular map on a fullspec slot is specularMap, sRGB3ch
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_spec", Gloss | Occ | Spec), &Data, "", R, nullptr, "iw8");   // the description's class (BO4 takes it from the techset flags)
        const Tc::Techset* T = Ok ? Data.Find(Value(R.Materials[0], "materialType")) : nullptr;
        std::string Sem, Core;
        if (Ok)
            for (const auto& I : R.ImagesOut)
                if (I.Name.find("_s") != std::string::npos || I.Name == "i_s")
                    for (const auto& [K, V] : I.Fields)
                    {
                        if (K == "semantic") Sem = V;
                        if (K == "coreSemantic") Core = V;
                    }
        Check(T != nullptr && T->Name == "lit_advanced_fullspec" && Sem == "specularMap" && Core == "sRGB3ch", "a spec map on a fullspec slot is specularMap, sRGB3ch", (T ? T->Name : "(none)") + " " + Sem + " " + Core);
    }
    // lit_weapon_camo is never chosen, even when the game names it
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_camo", Gloss | Occ), &Data, "lit_weapon_camo", R);
        const std::string Type = Ok ? Value(R.Materials[0], "materialType") : "";
        Check(Ok && !Type.empty() && Type.find("camo") == std::string::npos, "lit_weapon_camo is never chosen when named", Type);
    }
    // a name the game gives that the catalog holds is taken (exact, not reported); the slots it lacks are reported, not written
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_named", Gloss | Occ), &Data, "lit", R);
        bool Unmapped = false, Fallback = false;
        if (Ok)
            for (const auto& N : R.Materials[0].Notes)
            {
                Unmapped |= N.Code == "material.unmapped_slot";
                Fallback |= N.Code == "material.techset_fallback";
            }
        Check(Ok && Value(R.Materials[0], "materialType") == "lit" && Unmapped && !Fallback && (Value(R.Materials[0], "cosinePowerMap") == "(no key)" || Value(R.Materials[0], "cosinePowerMap") == "") && (Value(R.Materials[0], "occMap") == "(no key)" || Value(R.Materials[0], "occMap") == ""),
              "an exact name is taken; an image whose slot the techset lacks is reported unmapped_slot, not written", Ok ? Value(R.Materials[0], "materialType") : "");
    }
    // an effect techset's colour slot takes effectMap: the builder's own colour image is retargeted to it, not dropped (BO4 300-model run: 29 lost colorMap)
    {
        Builder::Resolved R;
        Materials::Description Fx = Make("mtl_fx", 0);
        Fx.Alpha = Materials::AlphaMode::Blend;
        Tc::Data Effects;     // one effect techset whose colour slot takes effectMap
        Effects.LoadCatalogText(R"({"techsets":[{"n":"effect_lit_emissive_add","c":"Effect","f":"none","b":"Blend","t":0,"s":[["colorMap","colorMap","effectMap",null],["normalMap","normalMap","normalMap",null]]}]})");
        std::map<std::string, size_t> FxCounters;
        const bool Ok = One(Fx, &Effects, "", R, &FxCounters, "iw8");
        std::string FxSeen;
        for (const auto& [K, V] : FxCounters) FxSeen += K + "=" + std::to_string(V) + " ";
        const std::string Bound = Ok ? Value(R.Materials[0], "colorMap") : "";
        std::string Sem, Core;
        if (Ok)
            for (const auto& I : R.ImagesOut)
                if (!Bound.empty() && I.Name == Bound)
                    for (const auto& [K, V] : I.Fields)
                    {
                        if (K == "semantic") Sem = V;
                        if (K == "coreSemantic") Core = V;
                    }
        Check(Ok && Value(R.Materials[0], "materialType").rfind("effect_", 0) == 0 && !Bound.empty() && Bound != "(no key)" && Sem == "effectMap" && Core == "sRGB3chAlpha",
              "an effect techset keeps the colour image of its colorMap slot as an effectMap", (Ok ? Value(R.Materials[0], "materialType") : std::string("(none)")) + " " + Bound + " " + Sem + " " + Core + " " + FxSeen);
    }
    // BO4 gloss range floats: used when a number inside 0..17 with min <= max; else the default stays and a row says why
    {
        const auto Range = [&](float Min, float Max, std::string& MinText, std::string& MaxText, std::string& Note) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Materials::Source Src;
            Materials::Description D = Make("mtl_range", Gloss);
            Src.Name = D.Name;
            Src.Techset = "00293375A8079D3E";     // a BO4 techset whose layout has glossRangeMin at 0x14 and glossRangeMax at 0x18
            std::vector<uint8_t> Block(32, 0);
            std::memcpy(&Block[20], &Min, 4);
            std::memcpy(&Block[24], &Max, 4);
            Src.Constants.push_back(Block);
            B.Add(D, Dir, &Src, "bo4");
            const Builder::Resolved R = B.Resolve();
            if (R.Materials.size() != 1) return false;
            MinText = Value(R.Materials[0], "glossRangeMin");
            MaxText = Value(R.Materials[0], "glossRangeMax");
            for (const auto& N : R.Materials[0].Notes)
                if (N.Code == "material.gloss_range_default") Note = N.Message;
            return true;
        };
        std::string Mn, Mx, Nt;
        Check(Range(2.0f, 10.0f, Mn, Mx, Nt) && Mn == "2" && Mx == "10" && Nt.empty(), "a valid BO4 gloss range is written", Mn + " " + Mx + " " + Nt);
        Mn.clear(); Mx.clear(); Nt.clear();
        Check(Range(12.0f, 3.0f, Mn, Mx, Nt) && Mn != "12" && Mx != "3" && Nt.find("above") != std::string::npos, "a BO4 gloss range with min above max keeps the default and says why", Mn + " " + Mx + " " + Nt);
        Nt.clear();
        Check(Range(0.0f, std::numeric_limits<float>::quiet_NaN(), Mn, Mx, Nt) && Mx.find("nan") == std::string::npos && Nt.find("NaN") != std::string::npos, "a NaN BO4 gloss range keeps the default and says why", Mx + " " + Nt);
        Nt.clear();
        Check(Range(0.0f, 20.0f, Mn, Mx, Nt) && Mx != "20" && Nt.find("0..17") != std::string::npos, "a BO4 gloss range above 17 keeps the default and says why", Mx + " " + Nt);
    }
    // a gloss image without a range proven by the source says so, per game; BO4 (constants read) does not
    {
        Builder::Resolved R1, R2, R3;
        const bool A = One(Make("mtl_gr1", Gloss), &Data, "", R1, nullptr, "t5");
        const bool B = One(Make("mtl_gr2", Gloss), &Data, "", R2, nullptr, "iw8");
        const bool C = One(Make("mtl_gr3", Gloss), &Data, "", R3, nullptr, "bo4");
        const auto Detail = [](const Builder::Resolved& R) {
            for (const auto& N : R.Materials[0].Notes)
                if (N.Code == "material.gloss_range_default") return N.Detail;
            return std::string();
        };
        Check(A && B && C && Detail(R1) == "game=t5, label=hypothesis" && Detail(R2) == "game=iw8, label=unknown" && Detail(R3).empty(),
              "a gloss image whose range the source does not prove is reported per game with its label; BO4 is not", Detail(R1) + " | " + Detail(R2) + " | " + Detail(R3));
    }
    // a fallback is reported with its rule, the source techset and the result
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_cutout", Cutout), &Data, "2ce22f911a30ce4", R, nullptr, "iw8");
        const Builder::Note* N = nullptr;
        if (Ok)
            for (const auto& X : R.Materials[0].Notes)
                if (X.Code == "material.techset_fallback") N = &X;
        const Tc::Techset* T = Ok ? Data.Find(Value(R.Materials[0], "materialType")) : nullptr;
        Check(N != nullptr && T != nullptr && T->Class == Tc::Blend::AlphaTest && N->Detail.find("rule=slot_superset") != std::string::npos &&
              N->Detail.find("source_techset=2ce22f911a30ce4") != std::string::npos && N->Detail.find("result=" + T->Name) != std::string::npos,
              "a fallback is reported with rule, source techset and result", N ? N->Detail : "(no note)");
    }
    // no data: today's rules, counted
    {
        Tc::Data Empty;
        Empty.Load(Dir / "no_profiles");
        Builder::Resolved R;
        std::map<std::string, size_t> Counters;
        const bool Ok = One(Make("mtl_nodata", Gloss | Occ | Spec), &Empty, "", R, &Counters);
        Check(Ok && Value(R.Materials[0], "materialType") == "lit_advanced_fullspec" && !R.Materials[0].Targeted && Counters["t_techset_data_missing"] == 1 && R.Materials[0].Fields.size() < 30,
              "missing data falls back to the old rules and says so", Ok ? Value(R.Materials[0], "materialType") : "");
    }
    // no target: not touched
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_notarget", Gloss | Occ | Spec), nullptr, "", R);
        Check(Ok && Value(R.Materials[0], "materialType") == "lit_advanced_fullspec" && !R.Materials[0].Targeted && R.Materials[0].Fields.size() < 30 && Value(R.Materials[0], "glossSurfaceType") == "<full>",
              "no target: the old entry, unchanged", Ok ? Value(R.Materials[0], "materialType") : "");
    }
    // every image slot written exists on the chosen techset
    {
        int Bad = 0;
        std::string First;
        const char* const Slots[] = { "colorMap", "normalMap", "cosinePowerMap", "specColorMap", "occMap", "colorMap00", "alphaRevealMap" };
        for (int F = 0; F < 32; ++F)
        {
            Builder::Resolved R;
            if (!One(Make("mtl_all", F), &Data, "", R))
            {
                ++Bad;
                First = "no material";
                continue;
            }
            const Tc::Techset* T = Data.Find(Value(R.Materials[0], "materialType"));
            if (T == nullptr) { ++Bad; First = "unknown techset"; continue; }
            for (const char* S : Slots)
                if (Value(R.Materials[0], S) != "(no key)" && Value(R.Materials[0], S) != "" && T->ByKey(S) == nullptr) { ++Bad; First = T->Name + " has no " + S; }
        }
        Check(Bad == 0, "every slot written exists on the chosen techset", First);
    }
    // image names from the material and the slot (decorated Infinity Ward names), clean names kept
    {
        Check(IsDecoratedImageName("csub_mtl_aac_crate_col_sub_482cf5df_c") && IsDecoratedImageName("i_x_c&") && IsDecoratedImageName("i_x_s~") &&
              IsDecoratedImageName("packed_foo_bar") && IsDecoratedImageName("nsub_mtl_x_a296bca2"), "decorated forms are told", "");
        Check(!IsDecoratedImageName("i_p8_col_rocks_sm_beach_03_muddy_c") && !IsDecoratedImageName("i_feedbabe_c") && !IsDecoratedImageName("i_12345678_c") && !IsDecoratedImageName("i_t8_micro_metal_rust_01_c") && !IsDecoratedImageName("$occlusion_black"),
              "clean BO3 style names are not decorated", "");
        for (const char* Name : { "csub_mtl_x_482cf5df_c.png", "csub_mtl_x_482cf5df_s.png", "csub_mtl_x_de28a1e1_g.png", "nsub_mtl_x_a296bca2.png", "i_clean_c.png", "i_one_c&.png", "i_two_s~.png", "packed_pk.png" })
        {
            std::ofstream F(Dir / Name, std::ios::binary);
            F << "x";
        }
        const auto Deco = [&](const char* Mat, const char* C, const char* N, const char* G, const char* S) {
            Materials::Description D;
            D.Name = Mat;
            D.Albedo = Map(C);
            D.Normal = Map(N);
            if (G != nullptr) D.Roughness = Map(G, true);
            if (S != nullptr) D.Specular = Map(S);
            return D;
        };
        Builder B(Base, "t_");
        B.TechsetData(&Data);
        Materials::Source Src;
        Materials::Description A = Deco("mtl_x_a", "csub_mtl_x_482cf5df_c.png", "nsub_mtl_x_a296bca2.png", "csub_mtl_x_de28a1e1_g.png", "csub_mtl_x_482cf5df_s.png");
        Materials::Description Z = Deco("mtl_x_b", "csub_mtl_x_482cf5df_c.png", "nsub_mtl_x_a296bca2.png", nullptr, nullptr);     // shares two images with mtl_x_a
        Materials::Description K = Deco("mtl_clean", "i_clean_c.png", "i_one_c&.png", nullptr, nullptr);
        Materials::Description P = Deco("mtl_pk", "packed_pk.png", "i_two_s~.png", nullptr, nullptr);
        for (const Materials::Description* D : { &Z, &A, &K, &P })      // the order of Add must not matter
        {
            Src.Name = D->Name;
            B.Add(*D, Dir, &Src, "ww2");
        }
        const Builder::Resolved R = B.Resolve();
        std::map<std::string, std::string> Img;
        for (const auto& M : R.Materials)
            for (const auto& [Key, V] : M.Fields)
                if (!V.empty() && (Key == "colorMap" || Key == "normalMap" || Key == "cosinePowerMap" || Key == "specColorMap")) Img[M.Name + "." + Key] = V;
        Check(Img["mtl_x_a.colorMap"] == "i_mtl_x_a_c" && Img["mtl_x_a.normalMap"] == "i_mtl_x_a_n" && Img["mtl_x_a.cosinePowerMap"] == "i_mtl_x_a_g" && Img["mtl_x_a.specColorMap"] == "i_mtl_x_a_s",
              "a decorated image is named i_<material>_<c|n|g|s>", Img["mtl_x_a.colorMap"] + "," + Img["mtl_x_a.normalMap"] + "," + Img["mtl_x_a.cosinePowerMap"] + "," + Img["mtl_x_a.specColorMap"]);
        Check(Img["mtl_x_b.colorMap"] == "i_mtl_x_a_c" && Img["mtl_x_b.normalMap"] == "i_mtl_x_a_n", "a shared image has one name, the first material's", Img["mtl_x_b.colorMap"]);
        Check(Img["mtl_clean.colorMap"] == "i_clean_c", "a clean name keeps its stem", Img["mtl_clean.colorMap"]);
        Check(Img["mtl_clean.normalMap"] == "i_mtl_clean_n" && Img["mtl_pk.colorMap"] == "i_mtl_pk_c" && Img["mtl_pk.normalMap"] == "i_mtl_pk_n", "the c& s~ and packed_ forms are named from material and slot",
              Img["mtl_clean.normalMap"] + "," + Img["mtl_pk.colorMap"] + "," + Img["mtl_pk.normalMap"]);
        std::set<std::string> Names;
        for (const auto& I : R.ImagesOut) Names.insert(I.Name);
        Check(Names.size() == R.ImagesOut.size(), "every image entity name is unique", std::to_string(Names.size()) + " of " + std::to_string(R.ImagesOut.size()));
        bool Legal = true;
        for (const std::string& N : Names)
            for (const char C : N) Legal = Legal && ((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_');
        Check(Legal, "image names are lower case letters, digits and underscore", "");
    }
    // a game adapter's declared image naming: a leading mtl_ is left out, and a declared suffix becomes its BO3 tag (_col is _c, _nml is _n)
    {
        for (const char* Name : { "h1_car_col.png", "h1_car_nml.png", "~h1_car_spc&h1_car_cos-l~abc123.png", "mtl_h1_other_col.png" })
        {
            std::ofstream F(Dir / Name, std::ios::binary);
            F << "x";
        }
        Materials::ImageNaming Naming;
        Naming.Suffixes = { { "_col", "c" }, { "_nml", "n" } };
        Naming.StripPrefixes = { "mtl_" };
        const auto Named = [&](const Materials::ImageNaming& Use) {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Materials::Description Car;
            Car.Name = "mtl_h1_car";
            Car.Albedo = Map("h1_car_col.png");
            Car.Normal = Map("h1_car_nml.png");
            Car.Specular = Map("~h1_car_spc&h1_car_cos-l~abc123.png");
            Car.Naming = Use;
            Materials::Description Other;
            Other.Name = "mtl_h1_other";
            Other.Albedo = Map("mtl_h1_other_col.png");
            Other.Naming = Use;
            Materials::Source Src;
            for (const Materials::Description* D : { &Car, &Other })
            {
                Src.Name = D->Name;
                B.Add(*D, Dir, &Src, "mwr");
            }
            std::map<std::string, std::string> Img;
            for (const auto& M : B.Resolve().Materials)
                for (const auto& [Key, V] : M.Fields)
                    if (!V.empty() && (Key == "colorMap" || Key == "normalMap" || Key == "specColorMap")) Img[M.Name + "." + Key] = V;
            return Img;
        };
        auto With = Named(Naming);
        Check(With["mtl_h1_car.colorMap"] == "i_h1_car_c" && With["mtl_h1_car.normalMap"] == "i_h1_car_n", "naming: _col and _nml become _c and _n", With["mtl_h1_car.colorMap"] + "," + With["mtl_h1_car.normalMap"]);
        Check(With["mtl_h1_car.specColorMap"] == "i_h1_car_s", "naming: a decorated image is i_<material without mtl_>_s", With["mtl_h1_car.specColorMap"]);
        Check(With["mtl_h1_other.colorMap"] == "i_h1_other_c", "naming: a leading mtl_ of the stem is left out", With["mtl_h1_other.colorMap"]);
        auto Without = Named(Materials::ImageNaming());
        Check(Without["mtl_h1_car.colorMap"] == "i_h1_car_col" && Without["mtl_h1_car.specColorMap"] == "i_mtl_h1_car_s", "naming: with no declaration nothing changes", Without["mtl_h1_car.colorMap"] + "," + Without["mtl_h1_car.specColorMap"]);
    }
    // one name per source image: the clean, the tagged and the shortened name never meet, and no source is dropped
    {
        for (const char* D : { "a", "b", "c" }) fs::create_directories(Dir / D, Ec);
        for (const char* Name : { "a/foo.png", "b/foo.png", "c/foo_c.png", "dec/x_482cf5df_c.png", "dec/y_482cf5df_c.png" })
        {
            fs::create_directories((Dir / Name).parent_path(), Ec);
            std::ofstream F(Dir / Name, std::ios::binary);
            F << "x";
        }
        const auto Mat = [&](const char* Name, const std::string& Colour) {
            Materials::Description D;
            D.Name = Name;
            D.Albedo = Map(Colour.c_str());
            D.Normal = Map("n.png");
            return D;
        };
        const auto ColourOf = [&](const Builder::Resolved& R, const char* Material) {
            for (const auto& M : R.Materials)
                if (M.Name == Material)
                    for (const auto& [K, V] : M.Fields)
                        if (K == "colorMap") return V;
            return std::string("(none)");
        };
        const auto Fill = [&](Builder& B, const std::vector<Materials::Description>& List) {
            Materials::Source Src;
            for (const auto& D : List)
            {
                Src.Name = D.Name;
                B.Add(D, Dir, &Src, "ww2");
            }
        };
        // 1. a/foo.png and b/foo.png share the stem foo (tagged _c and _c_2), c/foo_c.png is named foo_c on its own: three images, three names
        {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Fill(B, { Mat("m_a", "a/foo.png"), Mat("m_b", "b/foo.png"), Mat("m_c", "c/foo_c.png") });
            const Builder::Resolved R = B.Resolve();
            const std::set<std::string> Names = { ColourOf(R, "m_a"), ColourOf(R, "m_b"), ColourOf(R, "m_c") };
            std::set<std::string> Files;
            for (const auto& I : R.ImagesOut)
                if (I.File.stem().string().rfind("foo", 0) == 0) Files.insert(I.File.generic_string());
            Check(Names.size() == 3 && Files.size() == 3 && R.NameCollisions >= 1, "three source images with a clean, a tagged and a repeated name keep three names and three entities",
                  ColourOf(R, "m_a") + "," + ColourOf(R, "m_b") + "," + ColourOf(R, "m_c") + " files " + std::to_string(Files.size()) + " collisions " + std::to_string(R.NameCollisions));
        }
        // 2. two long names that meet after Bo3Target::Placement::Shorten: the second stem is the first one's shortened name
        {
            const std::string LongStem(70, 'x');
            const std::string Short = Bo3Target::Placement::Shorten("i_" + LongStem);
            for (const std::string& Stem : { LongStem, Short })
            {
                std::ofstream F(Dir / (Stem + ".png"), std::ios::binary);
                F << "x";
            }
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            Fill(B, { Mat("m_long", LongStem + ".png"), Mat("m_short", Short + ".png") });
            const Builder::Resolved R = B.Resolve();
            const std::string A = ColourOf(R, "m_long"), S = ColourOf(R, "m_short");
            std::set<std::string> Files;
            for (const auto& I : R.ImagesOut)
                if (I.File.stem().string().rfind("i_xx", 0) == 0 || I.File.stem().string().rfind("xxx", 0) == 0) Files.insert(I.File.generic_string());
            Check(Short.size() == 63 && Bo3Target::Placement::Shorten(A) != Bo3Target::Placement::Shorten(S) && Files.size() == 2, "names that meet after Shorten stay two images",
                  A + " | " + S + " files " + std::to_string(Files.size()));
        }
        // 3. a stock name for the material-and-slot name must not flip a decorated image back to its raw stem
        {
            Builder B(Base, "t_");
            B.TechsetData(&Data);
            B.SetStockImages([](const std::string& Name) { return Name == "i_mtl_a_c" ? std::string("diffuseMap") : std::string(); });
            Fill(B, { Mat("mtl_a", "dec/x_482cf5df_c.png") });
            const Builder::Resolved R = B.Resolve();
            size_t Late = 0;
            for (const auto& M : R.Materials)
                for (const auto& N : M.Notes)
                    if (N.Code == "material.stock_semantic_late") Late++;
            const Builder::Resolved R2 = B.Resolve();
            Check(ColourOf(R, "mtl_a") == "i_mtl_a_c" && ColourOf(R2, "mtl_a") == "i_mtl_a_c" && Late <= 1,
                  "a decorated image whose material-and-slot name is a stock image stays on that name (one decision, no alternating rounds)", ColourOf(R, "mtl_a") + " late " + std::to_string(Late));
        }
        // 4. the same inputs give the same names in any order (the owner of a shared image is the first material by name, not the first added)
        {
            std::vector<Materials::Description> List = { Mat("mtl_x_b", "dec/x_482cf5df_c.png"), Mat("mtl_x_a", "dec/x_482cf5df_c.png"), Mat("mtl_y", "dec/y_482cf5df_c.png"),
                                                         Mat("m_a", "a/foo.png"), Mat("m_b", "b/foo.png"), Mat("m_c", "c/foo_c.png") };
            Builder One(Base, "t_"), Two(Base, "t_");
            One.TechsetData(&Data);
            Two.TechsetData(&Data);
            Fill(One, List);
            std::reverse(List.begin(), List.end());
            Fill(Two, List);
            const auto E1 = One.Entries("\n"), E2 = Two.Entries("\n");
            Check(E1 == E2 && !E1.empty() && ColourOf(One.Resolve(), "mtl_x_b") == "i_mtl_x_a_c", "the same materials added in any order give the same entries; a shared image is named from the first material by name",
                  ColourOf(One.Resolve(), "mtl_x_b"));
        }
        // 5. the decorated rule on real-looking and look-alike names (the documented limit: hash look-alikes are taken for hashes)
        Check(IsDecoratedImageName("decade00") && IsDecoratedImageName("facade12") && IsDecoratedImageName("a1b2c3d4") && IsDecoratedImageName("cfoo_c&") && IsDecoratedImageName("~&-cger_camo_rope_col&$white2~97cb0cbd"),
              "hash look-alikes and the & and ~ forms are decorated", "");
        Check(!IsDecoratedImageName("warning@col") && !IsDecoratedImageName("a-b c.d") && !IsDecoratedImageName("feedbabe") && !IsDecoratedImageName("12345678") && !IsDecoratedImageName("decade0") && !IsDecoratedImageName("$occlusion_black"),
              "@ - space . $ and non-hash tokens do not make a name decorated", "");
        Check(AssetName("warning@col") == "warning_col", "warning@col is cleaned to warning_col as before", AssetName("warning@col"));
        {
            // The real model writer (XME, after the step LiveExport takes while a material GDT is written) and the real GDT writer (Builder::Entries) on one
            // 70-character source name: the MATERIAL line of the model and the material entry of the GDT must be the same shortened name.
            const std::string LongName = "Mtl_P8_Zm_Some_Really_Long_Material_Name_For_A_Dirty_Decal_Past_63_Yes";
            WraithModel Model;
            WraithMaterial& Mat = Model.AddMaterial();
            Mat.MaterialName = LongName;
            WraithSubmesh& Sub = Model.AddSubmesh();      // the writer lists the materials its submeshes use
            Sub.AddMaterial(0);
            for (int V = 0; V < 3; V++)
            {
                WraithVertex& Vert = Sub.AddVertex();
                Vert.Position = Vector3(static_cast<float>(V), 0.0f, 0.0f);
                Vert.Normal = Vector3(0, 0, 1);
                Vert.AddUVLayer(0.0f, 0.0f);
            }
            Sub.AddFace(0, 1, 2);
            std::vector<std::string> Saved;
            NameModelMaterials(Model, Saved);
            const fs::path XmePath = Dir / "long_name.XMODEL_EXPORT";
            CodXME::ExportXME(Model, XmePath.string());
            std::ifstream XmeIn(XmePath, std::ios::binary);
            const std::string XmeText((std::istreambuf_iterator<char>(XmeIn)), std::istreambuf_iterator<char>());
            std::string OnModel;
            const std::string Marker = "MATERIAL 0 \"";
            if (const size_t At = XmeText.find(Marker); At != std::string::npos)
                OnModel = XmeText.substr(At + Marker.size(), XmeText.find('"', At + Marker.size()) - (At + Marker.size()));
            Builder Long(Base, "t_");
            Long.TechsetData(&Data);
            Long.Add(Make(LongName.c_str(), 0), Dir, nullptr, "bo4");
            std::string OnEntry;
            for (const std::string& E : Long.Entries("\n"))
                if (E.find("material.gdf") != std::string::npos)
                {
                    const size_t Open = E.find('"');
                    OnEntry = E.substr(Open + 1, E.find('"', Open + 1) - Open - 1);
                }
            Check(LongName.size() == 70 && OnModel.size() == 63 && OnModel == OnEntry && OnModel.rfind("mtl_p8_zm_some_really_long", 0) == 0,
                  "a 70-character material name is the same 63-character name in the model file and in the GDT entry", OnModel + " | " + OnEntry);
            RestoreModelMaterialNames(Model, Saved);
            Check(Model.Materials[0].MaterialName == LongName, "the model's own name is put back after the files are written", "");
            // a writer that throws still leaves the caller's model with its own names
            bool Threw = false;
            try
            {
                const ScopedModelMaterialNames Scoped(Model, true);
                Check(Model.Materials[0].MaterialName.size() == 63, "inside the scope the model carries the GDT names", Model.Materials[0].MaterialName);
                throw std::runtime_error("writer failed");
            }
            catch (const std::runtime_error&)
            {
                Threw = true;
            }
            Check(Threw && Model.Materials[0].MaterialName == LongName, "an exception from a writer does not leave the model renamed", Model.Materials[0].MaterialName);
            {
                const ScopedModelMaterialNames Off(Model, false);
                Check(Model.Materials[0].MaterialName == LongName, "a scope that is off changes nothing", "");
            }
            // two exports at once: each thread has its own naming mode
            std::atomic<int> Ready{ 0 }, Done{ 0 };
            bool SeenOn = false, SeenOff = true;
            std::thread A([&] {
                const AssetNamesInModelFiles On(true);
                Ready++;
                while (Ready.load() < 2) std::this_thread::yield();
                SeenOn = ModelFilesUseAssetNames();
                Done++;
                while (Done.load() < 2) std::this_thread::yield();
                SeenOn = SeenOn && ModelFilesUseAssetNames();
            });
            std::thread B([&] {
                const AssetNamesInModelFiles Off(false);
                Ready++;
                while (Ready.load() < 2) std::this_thread::yield();
                SeenOff = ModelFilesUseAssetNames();
                Done++;
                while (Done.load() < 2) std::this_thread::yield();
                SeenOff = SeenOff || ModelFilesUseAssetNames();
            });
            A.join();
            B.join();
            Check(SeenOn && !SeenOff && !ModelFilesUseAssetNames(), "two exports at once keep their own naming mode, and the caller's is untouched", std::format("{} {}", SeenOn, SeenOff));
        }
    }
    fs::remove_all(Dir, Ec);
    return Failed;
}

AssetNamesInModelFiles::AssetNamesInModelFiles(bool On) : Was(UseAssetNames)
{
    UseAssetNames = On;
}

AssetNamesInModelFiles::~AssetNamesInModelFiles()
{
    UseAssetNames = Was;
}
}
