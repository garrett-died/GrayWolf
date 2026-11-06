#pragma once

#include "ByteView.h"

// One material description for every game: which file is the colour, normal, roughness or gloss,
// occlusion, specular or metalness, emission, transmission, which channel holds each value, and the
// alpha mode.
// Written beside a model as <model>.materials.json.

#include <cstdint>
#include <map>
#include <string>
#include <vector>


namespace Materials
{
    enum class AlphaMode
    {
        Opaque,
        Cutout,  // alpha tested at AlphaCutoff (alpha to coverage under MSAA)
        Blend,   // alpha blended
        Auto,    // cutout when the colour map's alpha is used, else opaque (decided on load)
    };

    // XY: x in r, y in g, z rebuilt. AG: x in a, y in g (DXT5nm). XYZ: all three.
    enum class NormalEncoding { XY, AG, XYZ };

    // How the exported image file of a map is packed, when it holds more than the one value the
    // map names (data the adapter states; never guessed from a file name). None: the file is one
    // plain map and Channel picks the value. The kinds below describe the file as exported:
    //   Nog                R gloss, G and A the hemi-octahedral normal (u, v), B occlusion
    //   NormalAlphaGreen   normal x in A, y in G (DXT5nm style), z not stored
    //   ColorMetalAlpha    colour in RGB, the metal mask in A (this map and the albedo share the file)
    //   ColorMetalMask     the metal mask of the albedo, held in another file: metal in R, the
    //                      insulator range in A (set on the Metalness map; the albedo is the colour)
    //   SpecularGlossAlpha a specular image whose alpha is the gloss (set on the gloss map: the BO3 unpack writes the alpha as a one-channel image,
    //                      unremapped; the adapter states the gloss range in Description::GlossStopsMax)
    //   SpecularSqrtF0     a specular colour stored as the square root of F0 (set on the Specular map: the BO3 unpack writes the sRGB-coded
    //                      F0 the Mod Tools' specColorMap holds, alpha dropped)
    enum class PackKind { None, Nog, NormalAlphaGreen, ColorMetalAlpha, ColorMetalMask, SpecularGlossAlpha, SpecularSqrtF0 };

    // A texture file (relative to the model file) and the channel holding the value (0..3 = rgba;
    // colour maps use rgb). Invert: the value is 1 - sample (a gloss map read as roughness).
    // Pack: how the file is packed (PackKind); saved in the sidecar as "pack" when not None.
    struct Map
    {
        std::string Path;
        int Channel = 0;
        bool Invert = false;
        PackKind Pack = PackKind::None;
        bool Empty() const { return Path.empty(); }
    };

    // A colour gradient looked up by the mesh's first UV set: albedo.rgb *= G(U, v), where
    // v = saturate(uv.y + OffsetScale * (2 * Offset[channel] - 1)) and G is Image.rgb (Rgb) or
    // Image[channel] * Color. Color is linear.
    struct Gradient
    {
        Map Image;
        bool Rgb = true;
        float U = 0.5f;
        Map Offset;
        float OffsetScale = 0.0f;
        float Color[3] = { 1, 1, 1 };
        bool Empty() const { return Image.Empty(); }
    };

    // A colour mixed in by a mask: albedo.rgb *= lerp(1, Color, Mask[channel]). Color is linear.
    struct ColorTint
    {
        Map Mask;
        float Color[3] = { 1, 1, 1 };
        bool Empty() const { return Mask.Empty(); }
    };

    // Light passing through a thin surface from behind (a leaf, a pod's shell): what comes out on
    // the viewer's side is the light reaching the far side times albedo.rgb * Color * Image[channel]
    // (Invert: the map is a thickness, 1 - sample). Color is linear.
    struct Backlight
    {
        Map Image;
        float Color[3] = { 1, 1, 1 };
        bool Empty() const { return Image.Empty(); }
    };

    // A map that sets the alpha in place of the colour map's: alpha = saturate((Image[channel] at
    // uv * UVScale - Low) / (High - Low)) (a reveal map's soft edge).
    struct Opacity
    {
        Map Image;
        float Low = 0.0f, High = 1.0f;
        float UVScale[2] = { 1, 1 };
        bool Empty() const { return Image.Empty(); }
    };

    // One surface layer. Its maps are sampled at uv' = (R(UVRotation) * (uv - 0.5) + 0.5) * UVScale;
    // its colour is tinted by its own alpha (rgb *= lerp(1, Tint, a)); its normal is a detail
    // normal whose xy, times NormalScale, add to the base normal's. A layer after the first covers
    // the result below it by lerp with Mask[channel], sampled at uv * MaskUVScale.
    struct Layer
    {
        Map Albedo, Normal, Roughness, Mask;
        float NormalScale = 1.0f;
        float RoughnessValue = -1.0f;   // < 0: none (the material's own roughness)
        float UVScale[2] = { 1, 1 }, MaskUVScale[2] = { 1, 1 };
        float UVRotation = 0.0f;        // radians
        float Tint[3] = { 1, 1, 1 };    // linear
    };

    // How a game's image file names become BO3's "i_<stem>_<tag>" (declared by the game's adapter, never by shared code): a clean image name that ends
    // in a declared suffix is renamed to its stem and the tag ("<stem>_col" is "i_<stem>_c"), and a leading prefix of the material name or the stem is
    // dropped ("mtl_", which the stock names never carry).
    struct ImageNaming
    {
        std::vector<std::pair<std::string, std::string>> Suffixes;   // (source suffix with its '_', BO3 tag): ("_col", "c")
        std::vector<std::string> StripPrefixes;                      // leading text of a name left out ("mtl_")
        bool Empty() const { return Suffixes.empty() && StripPrefixes.empty(); }
    };

    struct Description
    {
        std::string Name;
        Gradient Tint;                  // empty: none
        ColorTint Color;                // empty: none (applied before Tint)
        std::vector<Layer> Layers;      // when set, the colour (and roughness) is theirs; Normal is the base
        Map Albedo;                     // sRGB colour, alpha in a
        Map Normal;
        NormalEncoding NormalEnc = NormalEncoding::XY;
        bool NormalFlipY = false;
        Map Roughness;
        Map Metalness;                  // metal workflow
        Map Specular;                   // sRGB F0 colour, used when Metalness is empty
        // A pre-PBR specular map (reflection colour, not F0): F0 = map * SpecularScale, the colour
        // kept whole instead of what the reflection takes from it.
        bool LegacySpecular = false;
        float SpecularScale = 1.0f;
        Map Occlusion;
        Map Emissive;                   // sRGB colour
        float EmissiveColor[3] = { 1, 1, 1 };   // the emission times this (linear)
        Backlight Transmission;         // empty: none
        Opacity AlphaMap;               // empty: the colour map's alpha
        AlphaMode Alpha = AlphaMode::Opaque;
        float AlphaCutoff = 0.5f;
        float RoughnessValue = 0.6f;    // without a roughness map
        float MetalnessValue = 0.0f;    // without a metalness or specular map
        // The surface type the game's own flags give, as a BO3 surfaceType combo name ("metal"); empty when it states none.
        // SurfaceWhy then says why (SurfaceTypes.h): "no_table", "not_read", "none_in_source", "unnamed:0x..."; empty with a type.
        std::string SurfaceType, SurfaceWhy;
        // The evidence when SurfaceType rests on an inference rather than a read (MWR: a layer id intersection); the exporter writes it as a material.surface_from_layer_id row
        std::string SurfaceVia;
        // A game's rule that the material is not exported at all (HydraX D-13: a BO3 material with an image named *_combo is an unusable
        // duplicate). Set by the game's adapter, never by shared code; the reason is a code:detail string for the report row.
        std::string SkipWhy;
        // A game whose materials name their techset in the catalog's own terms (BO3, HydraX Bo3MaterialDecoder.Export) sets this to the techset's name
        // (cut at #, without folder or extension): materialType is then that techset, and a name the catalog lacks is an error (no fallback).
        std::string TechsetName;
        // Why the game's rule left TechsetName empty: "no_techset" (the material names none) or "name_unread" (the reader gave no name). Empty: a name is set.
        std::string TechsetWhy;
        // The BO3 gloss range (glossRangeMin / glossRangeMax, in stops) the game's gloss image maps to, when the adapter states one (< 0: not stated, the
        // techset default stays and the exporter says so).
        float GlossStopsMin = -1.0f, GlossStopsMax = -1.0f;
        // The adapter's naming of this material's images (ImageNaming)
        ImageNaming Naming;
        // Layers of a layered material the description does not carry (the maps are the first layer's): the exporter says how many
        int LayersDropped = 0;
        // The game's slot hash of the reveal image of a decal, when the adapter binds one: the builder fills alphaRevealMap of a reveal techset from it
        uint32_t RevealSemantic = 0;
        int RevealChannel = 0;   // the channel of that image that holds the reveal (MWR: the grey picture's red); the game's adapter states it
        // Textures of the source the description does not carry (comma separated names, one per kind: "detail", "parallax", ...): the exporter writes a
        // material.texture_unbound row and the gate warns
        std::string TexturesUnbound;
        // Why the techset family was chosen against the usual rule: "surface_unknown_treated_as_metal" (an ambiguous or unread surface keeps its specular map)
        std::string FamilyWhy;
    };

    // What an exporter or a model file knows about one material: the input of an adapter.
    // Thickness: a transmission map read inverted (as Gloss is a roughness map read inverted).
    enum class Role { Unknown, Albedo, Normal, Gloss, Roughness, Specular, Metalness, Occlusion, Emissive, Transmission, Thickness };
    struct Source
    {
        struct Image
        {
            std::string Path;           // as the model file refers to it
            Role Hint = Role::Unknown;  // the usage the game reader gave it
            uint32_t Semantic = 0;      // the game's semantic hash, 0 when unknown
            uint32_t Format = 0;        // the image's DXGI format when the reader knows it (Black Ops 4 models), else 0
        };
        std::string Name, Techset;
        std::vector<Image> Images;
        std::vector<std::vector<uint8_t>> Constants;   // the game's constant bytes, by its own block index
        int SortKey = -1;                               // the game's sort key, -1 when unknown
        // The game's own flag words, -1 when its reader gave none: the material's surface flags (BO4 +0x28, BO3 +0x24) and
        // its techset's renderFlags (BO4 +0x20). Read by SurfaceTypes against the profile's flag_names.txt.
        int64_t SurfaceFlags = -1, RenderFlags = -1;
        std::string SurfaceVia;   // the evidence when SurfaceFlags came from an inference (a layer id intersection)
        const Image* BySemantic(uint32_t Semantic) const;
    };

    // Marks every map of D (and of its layers) whose file is Path as packed Kind: what an adapter
    // calls once it knows how a game packs the image its slots share.
    void MarkPacked(Description& D, const std::string& Path, PackKind Kind);
    // The sidecar's name of a kind ("none", "nog", "normal_ag", "color_metal_alpha", "color_metal_mask")
    const char* PackName(PackKind Kind);
    PackKind PackFromName(const std::string& Name);   // an unknown name is None

    // The row a game's zonekit profile gives a technique set in shading.txt (read off the game's
    // pixel shaders): "recipe" and the constant offsets it names; null when none.
    using Recipe = std::map<std::string, std::string>;
    const Recipe* ShadingRecipe(const std::string& Profile, const std::string& Techset);
    // The float at a recipe field's offset in Block (the field a decimal or 0x offset), else Default.
    float RecipeFloat(const Recipe& R, const std::string& Field, ByteView Block, int Component, float Default);

    // One per game (Games/<game>/MaterialAdapter.cpp): its materials as a Description.
    class IAdapter
    {
    public:
        virtual ~IAdapter() = default;
        virtual const char* Name() const = 0;
        virtual Description Describe(const Source& Material) const = 0;
    };
    struct AdapterRegistration
    {
        AdapterRegistration(const char* Engine, const IAdapter& Adapter);   // the game's Games/<ENGINE>
    };
    const IAdapter& AdapterFor(const std::string& Engine);   // the engine's, else the default
    const IAdapter& DefaultAdapter();                  // texture name suffixes
    // The pre-PBR games' (colorMap, normalMap, specularMap with gloss in its alpha): a game registers
    // it as its own adapter.
    const IAdapter& LegacySpecularAdapter();

    // The role a file name's suffix gives (_c colour, _n normal, _g gloss, _s specular, _o
    // occlusion, _e emissive and their long forms), a "-<hash>" tail ignored.
    Role RoleFromName(const std::string& FileName);
    // Hair, beard, fur, cards, foliage, decals: names of alpha-tested surfaces.
    bool LooksAlphaTested(const std::string& Name);

    // For games whose materials give each texture a semantic hash: the roles those semantics
    // mean (one semantic may feed two roles, e.g. a specular map's alpha read as gloss). Images
    // whose semantic is not in the table are left out; alpha is Cutout for an alpha-tested name
    // (material or colour map), else Auto.
    struct SemanticRole
    {
        uint32_t Semantic;
        Role Use;
        int Channel = 0;
    };
    Description DescribeBySemantic(const Source& Material, const std::vector<SemanticRole>& Table);
    // The IW engine's colorMap, normalMap and specularMap (rgb the specular colour, alpha the gloss):
    // the pre-PBR games', Ghosts' and Advanced Warfare's table.
    const std::vector<SemanticRole>& SpecularGlossTable();

    // The adapter of a game whose materials give each texture a semantic: its table read
    // (DescribeBySemantic), the names read instead when the game says so and the table matched
    // nothing (a material with no semantics: a map's, an older exporter's), then what the game adds.
    // A game with a table only is an instance; one that adds more derives and overrides the hooks.
    class SemanticAdapter : public IAdapter
    {
    public:
        enum class Unmatched { Empty, Names };   // what a material the table does not match is described by
        SemanticAdapter(const char* Name, std::vector<SemanticRole> Table, Unmatched Else = Unmatched::Empty)
            : AdapterName(Name), Rows(std::move(Table)), Else(Else) {}
        const char* Name() const final { return AdapterName; }
        Description Describe(const Source& Material) const final;
    protected:
        // the table for this material (a game whose slots depend on its images builds it here)
        virtual std::vector<SemanticRole> TableFor(const Source&) const { return Rows; }
        // whether the table described the material: a colour or a normal map found
        virtual bool Matched(const Description& D) const { return !D.Albedo.Empty() || !D.Normal.Empty(); }
        // what the game adds (alpha, tints, layers...); BySemantic false when the names described it
        virtual void Refine(const Source&, Description&, bool /*BySemantic*/) const {}
    private:
        const char* AdapterName;
        std::vector<SemanticRole> Rows;
        Unmatched Else;
    };

    // What a format with a colour factor only (glTF's baseColorFactor) multiplies the colour map by:
    // the colour tint whole, its mask left out (exact where the mask is 1, as on a white colour map),
    // else white. Linear.
    void BaseColorFactor(const Description& D, float Rgb[3]);

    struct Sidecar
    {
        std::string Game, Adapter;
        std::vector<Description> Materials;
        std::vector<Source> Sources;    // the raw inputs, parallel to Materials
    };
    std::string SidecarPath(const std::string& ModelPath);  // <dir>/<stem>.materials.json
    // While on, WriteSidecar also keeps the first 0x100 bytes of each material's first constant block ("constants", hex),
    // which the BO3 target reads colorTint and glossRange from. Set by a run with a --target; off, the sidecar is what it was.
    void KeepConstantsInSidecar(bool On);
    bool WriteSidecar(const std::string& Path, const Sidecar& S);   // a file already holding it is left alone
    bool SidecarHolds(const std::string& Path, const Sidecar& S);   // the file holds exactly what WriteSidecar writes
    // Reads the descriptions and, parallel to them, the raw inputs the sidecar kept (techset, image semantics, constants).
    bool ReadSidecar(const std::string& Path, Sidecar& S);
}
