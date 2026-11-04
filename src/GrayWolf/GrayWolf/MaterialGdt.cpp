#include "stdafx.h"

#include "MaterialGdt.h"

#include "DiskGame.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "GdtWriter.h"
#include "SurfaceTypes.h"
#include "Bo3Target.Awi.h"
#include "Bo3Target.Output.h"

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
    std::atomic<bool> UseAssetNames{ false };

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
                                                                 { "thicknessMap", "t" } };
        const auto It = Tags.find(Semantic);
        return It == Tags.end() ? Semantic : It->second;
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
    const std::string Name = AssetName(D.Name);
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
    if (Name != D.Name) Counter("renamed")++;

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
        }
    }
    else if (D.SurfaceWhy == "none_in_source" && Source != nullptr && SurfaceTypes::ReadAndNamesNoSurface(Game, Source->SurfaceFlags))
    {
        // positive evidence: a word was read and names no surface. Every <none> carries a row, so the gate can tell this from a word never read.
        Counter("surface_none_in_source")++;
        M.Notes.push_back({ "material.surface_none_in_source", "game=" + Game, "the source's surface word was read and names no surface type: <none> is the source's own value" });
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
                Counter(Roughness.Invert ? "converted_channel_cosinePowerMap" : "converted_roughness_to_gloss")++;
            }
        }
    }
    if (GlossDone) {}
    else if (!Roughness.Empty() && !Roughness.Invert) Counter("roughness_not_gloss")++;
    else Slot(M, "cosinePowerMap", Roughness, Dir, "glossMap", "Linear1ch", "compressed");
    if (Spec)
    {
        // a full colour in the fullspec and emissive techniques, else the intensity mask theirs take
        // (with a target the slot the techset has decides: ChooseTechset gives a scalar slot its mask semantic)
        const bool Colour = Catalog || M.Type == "lit_advanced_fullspec" || M.Type == "lit_emissive";
        if (Fused ? DerivedSlot(M, "specColorMap", FusedSpec, Colour ? "specularMap" : "specularMask", Colour ? "sRGB3ch" : "Linear1ch", "compressed")
                  : Slot(M, "specColorMap", D.Specular, Dir, Colour ? "specularMap" : "specularMask", Colour ? "sRGB3ch" : "Linear1ch", "compressed"))
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
        if (Key == "alphaRevealMap") return "thicknessMap";
        return "diffuseMap";
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
    Rq.SourceTechset = Source != nullptr ? Source->Techset : std::string();
    for (const auto& [Key, I] : M.Slots)
    {
        // a placeholder of the slot ($white_gloss) is no wish for the slot
        if (!I.Builtin.empty() && Targets->IsDefaultImage(Key, I.Builtin)) continue;
        Rq.Wanted.emplace_back(Key, I.Builtin.empty() ? I.Semantic : IntentOfKey(Key));
    }
    Rq.Class = Blend ? Tc::Blend::Blend : Cutout ? Tc::Blend::AlphaTest : Tc::Blend::Opaque;
    // a decal is told by the game's own techset name when it has one, and by the techset's renderFlags where the name is a hash (BO4)
    const bool DecalByName = !Rq.SourceTechset.empty() && ExportNaming::Lower(Rq.SourceTechset).find("decal") != std::string::npos;
    const bool DecalByFlags = Source != nullptr && SurfaceTypes::IsDecal(ExportNaming::Lower(Game), Source->RenderFlags);
    if (DecalByName || DecalByFlags) Rq.Class = Tc::Blend::Decal;
    if (DecalByFlags && !DecalByName) Counter("decal_from_render_flags")++;
    Rq.Emissive = Emissive;
    Rq.HasColorSpec = Spec;
    Rq.ColorSpec = true;    // Description.Specular is an RGB colour (F0)
    const Tc::Choice C = Tc::Choose(*Targets, Rq);
    if (C.Chosen == nullptr)
    {
        M.Type = "lit";
        Counter("techset_none")++;
        return;
    }
    const Tc::Techset& T = *C.Chosen;
    M.Type = T.Name;
    M.Category = T.Category.empty() ? "Geometry" : T.Category;
    Counter(std::string("techset_rule_") + Tc::RuleName(C.Used))++;
    if (!C.Exact())
    {
        Counter("techset_fallback")++;
        M.Notes.push_back({ "material.techset_fallback", std::string("rule=") + Tc::RuleName(C.Used) + ", source_techset=" + Rq.SourceTechset + ", result=" + T.Name, C.Reason });
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
            ++It;
            continue;
        }
        if (S == nullptr || !Tc::Accepts(*S, I.Semantic))
        {
            M.Notes.push_back({ "material.unmapped_slot", "slot=" + Key + ", techset=" + T.Name + ", image=" + I.File.filename().string(),
                                S == nullptr ? "the techset has no slot " + Key + ": the image is not written"
                                             : "the slot " + Key + " takes " + S->Semantic + ", not " + I.Semantic + ": the image is not written" });
            Counter("unmapped_slot")++;
            Counter("unmapped_slot_" + Key)++;
            It = M.Slots.erase(It);
            continue;
        }
        const std::string Sem = Tc::SemanticFor(*S, I.Semantic);
        if (Sem != I.Semantic)
        {
            M.Notes.push_back({ "material.slot_semantic_changed", "slot=" + Key + ", techset=" + T.Name + ", from=" + I.Semantic + ", to=" + Sem,
                                "the slot takes " + Sem + ": a colour specular map is read as a scalar mask" });
            if (Sem == "specularMask") Counter("spec_as_mask")++;
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
            float V;
            if (ReadFloat(Block, At.Offset, V))
            {
                M.Values.emplace_back(At.Key, Tc::FloatText(V));   // BO4 gloss is 0 to 17 stops like BO3: no conversion
                Counter("constants_" + At.Key)++;
            }
        }
    }
}

Builder::Resolved Builder::Resolve() const
{
    // the images: "i_<file stem>", the semantic's tag added when one stem names several images
    std::map<Image, std::string> ImageNames;
    std::map<std::string, std::vector<Image>> ByStem;
    for (const auto& [Name, M] : Defined)
        for (const auto& [Key, I] : M.Slots)
            if (I.Builtin.empty() && !ImageNames.count(I))
            {
                ImageNames[I];
                // BO3's own convention, "i_<name>" (a name that has it already keeps it)
                const std::string Stem = AssetName(I.File.stem().string());
                ByStem[Stem.rfind("i_", 0) == 0 ? Stem : "i_" + Stem].push_back(I);
            }
    for (auto& [Stem, List] : ByStem)
    {
        std::sort(List.begin(), List.end());
        std::map<std::string, size_t> Seen;
        for (const Image& I : List)
        {
            std::string Name = Stem;
            if (List.size() > 1) Name += "_" + Tag(I.Semantic) + (I.FromAlpha ? "a" : "");
            if (const size_t N = Seen[Name]++) Name += "_" + std::to_string(N + 1);
            ImageNames[I] = Name;
        }
    }

    Resolved Out;
    for (const auto& [Name, M] : Defined)
    {
        const auto Used = Usage.find(M.Source);
        Gdt::Fields F = { { "materialCategory", M.Category }, { "materialType", M.Type }, { "surfaceType", M.SurfaceType },
                          { "template", "material.template" }, { "usage", Used != Usage.end() ? Used->second : "<not in editor>" } };
        ResolvedMaterial R;
        R.Name = Name;
        R.Source = M.Source;
        R.Targeted = M.Targeted;
        R.Notes = M.Notes;
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
        Gdt::Fields F = { { "baseImage", BaseImageOf(I->File) }, { "compressionMethod", I->Compression },
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
    for (const auto& M : R.Materials) Out.push_back(Gdt::Entry(M.Name, "material.gdf", M.Fields, Eol));
    for (const auto& I : R.ImagesOut)
    {
        Gdt::Fields F = I.Fields;
        for (auto& [Key, Value] : F)
            if (Key == "baseImage") Value = Gdt::Path(Value);
        Out.push_back(Gdt::Entry(I.Name, "image.gdf", F, Eol));
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
    return UseAssetNames.load();
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
    const auto One = [&](const Materials::Description& D, const Tc::Data* Targets, const std::string& Techset, Builder::Resolved& Out, std::map<std::string, size_t>* Counters = nullptr) {
        Builder B(Base, "t_");
        B.TechsetData(Targets);
        Materials::Source Src;
        Src.Name = D.Name;
        Src.Techset = Techset;
        B.Add(D, Dir, &Src, "bo4");
        Out = B.Resolve();
        if (Counters != nullptr) *Counters = B.Counters();
        return Out.Materials.size() == 1;
    };

    // usage: <not in editor> unless the builder was told one for the material (a map's geometry), and then only that one
    {
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
        B.SetUsage({ { "mtl_floor", "floor" } });
        for (const auto& M : B.Resolve().Materials) (M.Name == "mtl_floor" ? UsedA : UsedC) = Value(M, "usage");
        Check(Before == "<not in editor>;<not in editor>;", "usage defaults to <not in editor>", Before);
        Check(UsedA == "floor" && UsedC == "<not in editor>", "a named material gets its usage, the others keep <not in editor>", UsedA + " / " + UsedC);
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
    // spec: a colour specular map on a fullspec slot is specularMap, sRGB3ch
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_spec", Gloss | Occ | Spec), &Data, "", R);
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
    // a fallback is reported with its rule, the source techset and the result
    {
        Builder::Resolved R;
        const bool Ok = One(Make("mtl_cutout", Cutout), &Data, "2ce22f911a30ce4", R);
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
    fs::remove_all(Dir, Ec);
    return Failed;
}

AssetNamesInModelFiles::AssetNamesInModelFiles(bool On) : Was(UseAssetNames.exchange(On))
{
}

AssetNamesInModelFiles::~AssetNamesInModelFiles()
{
    UseAssetNames.store(Was);
}
}
