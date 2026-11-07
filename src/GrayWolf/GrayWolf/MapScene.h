#pragma once

// A level, the same for every game: what a game's map reader fills (Exports(Kind::Map, ...)) and
// the one writer (MapGlbWriter.h) turns into a .glb. Taken from C2M's CoDMap, which held 26
// games: world meshes, models placed by static, dynamic and splined instances, lights, materials
// by name and the images they use, the sky, the ambient colour and the entity string. Positions
// stay in the game's space (Z up, inches); triangles keep the game's winding.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <cstring>
#include <span>
#include <vector>

namespace Maps
{
    struct Vertex
    {
        float Pos[3] = { 0, 0, 0 };
        float Normal[3] = { 0, 0, 1 };
        bool NormalRead = false;   // set by the reader that read Normal; unset = unknown (the default (0, 0, 1) is also a real flat-up normal)
        float Uv[2] = { 0, 0 };
        uint8_t Color[4] = { 255, 255, 255, 255 };
    };

    // The guid of a light record that has none: FNV-1a of the origin's bits and the record's index (two lights at one origin differ by their index)
    inline uint32_t LightGuid(const float Origin[3], uint32_t Index)
    {
        uint32_t Guid = 2166136261u;
        for (const float Component : std::span<const float, 3>(Origin, 3))
        {
            uint32_t Bits;
            std::memcpy(&Bits, &Component, 4);
            for (int B = 0; B < 4; B++) Guid = (Guid ^ ((Bits >> (8 * B)) & 0xff)) * 16777619u;
        }
        for (int B = 0; B < 4; B++) Guid = (Guid ^ ((Index >> (8 * B)) & 0xff)) * 16777619u;
        return Guid;
    }

    // The layers a game blends a surface of (Modern Warfare Remastered): per vertex the weight of each layer above the base (0..255, kept apart from Color: the
    // vertex colour of such a surface is not a tint) and the layer's own texture coordinates. At most MaxBlendLayers.
    constexpr uint32_t MaxBlendLayers = 6;
    struct BlendVertex
    {
        int16_t Weight[MaxBlendLayers] = { -1, -1, -1, -1, -1, -1 };   // -1: not read (never 0, which is a read weight)
        float Uv[MaxBlendLayers][2] = {};
        // the worst of the vertex's rotation words (a 2x2 matrix the game applies to a layer's normal): 0 none or identity, 1 a flip (a diagonal of +-1, the rest 0), 2 anything else
        uint8_t Rot = 0;
        uint8_t BadUv = 0;   // bit L: the layer's texture coordinates were not finite (its weight is then -1)
    };
    struct BlendData
    {
        uint32_t Layers = 0;                // the layers above the base this surface's material has; 0: the surface is not blended
        std::vector<int16_t> Weight;        // Layers per vertex: 0..255, or -1 where the data was not read
        std::vector<float> Uv;              // two per layer per vertex
        bool Present() const { return Layers != 0; }
        // the weight of layer K (0 = the first above the base) of vertex V, -1 when not read
        int Of(size_t V, uint32_t K) const { return (V * Layers + K) < Weight.size() ? Weight[V * Layers + K] : -1; }
    };

    // Triangles of one material.
    struct Surface
    {
        std::string Material;
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices;   // three per triangle, into Vertices
        BlendData Blend;                 // empty unless the game blends this surface's material in layers
        // the UV sets after each vertex's Uv (a blended surface's other layers: WWII), UvSets - 1
        // pairs per vertex in a row; empty when the surface has one set
        uint32_t UvSets = 1;
        // the game's decal sort of each triangle (0: no decal layer), one per triangle, empty where the game gives none read; the surface and its primitives are not split by it
        std::vector<uint32_t> TriangleSort;
        std::vector<float> MoreUvs;
        size_t Triangles() const { return Indices.size() / 3; }
    };

    // World geometry (in world space) or a model (in its own space, LOD 0).
    struct Mesh
    {
        std::string Name;
        std::vector<Surface> Surfaces;
    };

    // A model placed in the world: Axis rows are its local X, Y and Z in world space.
    struct Instance
    {
        std::string Name;    // the node's name ("prop_12")
        std::string Model;   // a key of MapScene::Models
        float Axis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        float Origin[3] = { 0, 0, 0 };
        float Scale = 1.0f;
        std::string Destroyed;   // a destructible's other model, when it has one
    };

    struct Light
    {
        enum class Type { Sun, Spot, Point };
        Type Kind = Type::Point;
        float Origin[3] = { 0, 0, 0 };
        float Dir[3] = { 0, 0, -1 };
        float Color[3] = { 1, 1, 1 };   // as the game gives it (HDR: its brightness is its intensity)
        float Radius = 0.0f;
        float CosHalfFovOuter = 0.0f;
        // A compiled primary light read whole (Black Ops 4): its basis, guid, definition name and the
        // fields no conversion is proven for (the Radiant export keeps them under unresolved keys)
        bool Compiled = false;
        float Right[3] = { 0, 1, 0 }, Up[3] = { 0, 0, 1 };
        uint32_t Guid = 0;
        std::string Def;
        float Unresolved204 = 0.0f, Unresolved448 = 0.0f;
        // A game whose compiled record has only the colour among the unresolved fields (Modern Warfare Remastered) names the key it is written under
        // and leaves the two Black Ops 4 floats out
        std::string ColorKey;
        bool OnlyColor = false;
        // A Black Ops III compiled primary light read whole (564 bytes): the fields the stock light keys were
        // fitted against (docs/LIGHT-CONVERSION.md). Color is then the linear radiance
        // srgb2lin(_color) * 2^stops, which the compiled light keeps and not the two keys apart.
        bool Bo3 = false;
        float Forward[3] = { 1, 0, 0 };
        float FalloffDistance = 0.0f, NearEdge = 0.0f, FarEdge = 0.0f, SpecScale = 1.0f, CutOn = 0.0f, Penumbra = 0.0f, BulbRadius = 0.0f;
        float SuperEllipse[4] = { 1, 1, 1, 1 };
    };

    // A texture a material uses: the game's semantic hash (0 when it has none) and the image.
    struct Texture
    {
        uint32_t Semantic = 0;
        std::string Image;   // a key of MapScene::Images
    };

    struct Material
    {
        std::string Name;
        std::string Techset;
        std::string ColorMap;   // the image drawn as the base colour ("" when none)
        std::vector<Texture> Textures;
        // the semantics of textures whose image the reader could not name (left out of Textures): a blended material's shape needs them
        std::vector<uint32_t> UnnamedSemantics;
        // blended surfaces of this material whose layer weights were not read (set by MarkUnreadBlends)
        uint32_t SurfacesUnread = 0;
        std::vector<std::vector<uint8_t>> Constants;   // the game's constant blocks (the adapter's tint, layers)
        int SortKey = -1;                               // the game's sort key, -1 when unknown
        int64_t SurfaceFlags = -1, RenderFlags = -1;    // the game's flag words (XMaterial_t), -1 when unknown
        std::string SurfaceVia;                         // the evidence when SurfaceFlags came from an inference (XMaterial_t::SurfaceVia)
        // The layers a blended material has above its base, bottom to top (a game's reader makes one scene material per distinct layer art, Games/<game>): the material that
        // draws the layer, whether the layer has a reveal map, and the reveal's sharpness K (< 0: not read)
        struct BlendLayer
        {
            std::string Material;
            bool Reveal = false;
            float Sharpness = -1.0f;
        };
        std::vector<BlendLayer> Layers;
        // a material a game's reader made for one layer of a blend (above its base): it is drawn as a decal and has no collision of its own
        bool IsLayer = false;
    };

    // An image's pixels: PNG bytes embedded in the .glb, or files (absolute paths, which may be
    // written later in the run: a dependency). File is the image kind's (what the materials sidecar
    // names); Gltf a PNG of it for glTF tools (the map_png setting). The .glb names Gltf, else File
    // (a DDS through MSFT_texture_dds).
    struct Image
    {
        std::vector<uint8_t> Png;
        std::string File, Gltf;
        bool Empty() const { return Png.empty() && File.empty() && Gltf.empty(); }
    };

    // A reflection probe the game keeps (Modern Warfare Remastered: GfxWorld draw data, 22 on mp_crash). Size: the half extents on each side of Origin when the record holds them
    // (SizeRead), else not read (never a made-up value).
    struct Probe
    {
        float Origin[3] = { 0, 0, 0 };
        float SizeMin[3] = { 0, 0, 0 }, SizeMax[3] = { 0, 0, 0 };
        bool SizeRead = false;
    };

    // The map's sun as a game gives it, written as a volume_sun and an ssi entry (MapSunProbes.h). Not Sky::Sun, which other games fill for the preview only.
    struct SunData
    {
        bool Present = false;
        float Toward[3] = { 0, 0, 0 };   // unit vector from the world toward the sun, game space
        float Color[3] = { 0, 0, 0 };    // as the game gives it (HDR, intensity premultiplied): only its ratios are used
    };

    // An effect or sound the map's script places (MWR createfx: kind "fx", "loopsound", "delaysound"), written as a script_struct
    struct EffectPlacement
    {
        std::string Kind, Name;
        float Origin[3] = { 0, 0, 0 }, Angles[3] = { 0, 0, 0 };
    };

    struct Sky
    {
        std::string Model;   // a sky drawn as a model (a key of Models), placed at the origin
        std::string Image;   // a sky drawn as a texture (a key of Images): written beside the .glb
        float Sun[3] = { 0, 0, 0 };   // unit vector toward the sun, game space (zero: unknown)
        bool Atmosphere = false;      // the game draws the sky by scattering (no texture): a physical sky
    };

    // A brush of the clip map: the convex volume its sides bound. Each side is a plane whose normal
    // points out of the brush (a point p is inside when Normal . p <= Dist for every side), with
    // the clip map's material name when the game keeps one per side ("" otherwise) and the flags
    // the game gives the side. Contents are the brush's content flags (Quake's lineage: 1 solid,
    // 0x10000 player clip, 0x20000 monster clip, ...). The axis-aligned sides the game keeps as a
    // bounding box are sides here like the others; a side may bound nothing (a bevel).
    struct BrushSide
    {
        float Normal[3] = { 0, 0, 1 };
        float Dist = 0.0f;
        std::string Material;
        uint32_t Contents = 0, Surface = 0;
    };

    struct Brush
    {
        uint32_t Contents = 0;
        std::vector<BrushSide> Sides;
        // The brush's corners (three floats each) when the clip map keeps them (Black Ops III, 4):
        // the brush is their convex hull, the sides give materials and flags
        std::vector<float> Vertices;
    };

    // The level's collision, from its clip map: the world's brushes, and each brush model's, in
    // the model's own space (Models[N] is "*N" in the entity string; Models[0], the world, empty).
    // the key of a clip tool row: the contents word and the surface word (0xFFFFFFFF: any surface)
    inline uint64_t ClipToolKey(int64_t Surface, uint32_t Contents) { return (static_cast<uint64_t>(Surface < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(Surface)) << 32) | Contents; }

    struct Collision
    {
        std::vector<Brush> Brushes;
        std::vector<std::vector<Brush>> Models;
        // the entity string's trigger volumes (MapTriggers: "model" "?N" is element N), in its entity's space
        std::vector<std::vector<Brush>> Triggers;
        // the game's brush contents are Radiant's `contents weaponClip detail umbraTarget;` bits
        // (Black Ops III; Black Ops 4 writes none)
        bool ContentsKeywords = false;
        size_t ClipBrushes = 0;   // the clip map's brush count (world and models): what was read from
        // the clip map's materials (name, content flags): what names the tool materials of this game
        std::vector<std::pair<std::string, uint32_t>> Materials;
        std::vector<uint32_t> MaterialSurface;   // their surface flags, beside Materials (0 where the game keeps none)
        // the world's collision triangles (patches, terrain): three floats per vertex, three
        // vertex numbers per triangle (a, b, c), in world space, each facing out where
        // (b - a) x (c - a) points (a game storing them the other way round is turned by its reader)
        std::vector<float> Vertices;
        std::vector<uint32_t> Triangles;
        // one label per triangle: the clip material index its partition names (an index, into Materials where the game keeps names, else into a table the game keeps elsewhere), -1 where the
        // game gives none or it was not read; empty: none read for any
        std::vector<int32_t> TriangleLabel;
        bool TriangleLabelIsClipMaterial = false;   // the labels index Materials (names and contents the clip map keeps), so a mesh with no drawn material is named by the contents like a brush
        // exact clip contents -> the BO3 tool material the game's profile names for it (zonekit/profiles/<profile>/flag_names.txt, `clip_tool` rows; filled by the game's reader); the contents mapping of brushes is used for the rest
        std::map<uint64_t, std::string> ClipToolByContents;   // key: ClipToolKey(surface or -1, contents)
        std::map<uint64_t, std::string> ClipToolApproximation;   // beside it: what the tool cannot express (reported as an approximation)
        // the tool a (surface, contents) pair is named by when the game's profile has a table of tools by properties (Black Ops 4: SurfaceTypes::Names::ClipMatch); empty name: none
        std::function<std::string(uint32_t Surface, uint32_t Contents)> ClipToolMatch;
        bool Empty() const { return Brushes.empty() && Models.empty() && Triggers.empty(); }
    };

    struct MapScene
    {
        std::string Name;
        std::string Game;                           // the game id ("bo4"): the materials sidecar's
        std::vector<Mesh> Objects;                  // world geometry
        std::map<std::string, Mesh> Models;         // what the instances place, by name
        std::vector<Instance> StaticInstances, DynamicInstances, SplinedInstances;
        std::vector<Light> Lights;
        std::map<std::string, Material> Materials;  // by name
        std::map<std::string, Image> Images;        // by name
        float AmbientColor[3] = { 0, 0, 0 };
        Maps::Sky Sky;
        std::vector<Probe> Probes;   // reflection probes the game keeps (empty: none read)
        SunData Sun;                 // the map's sun (Present false: none read)
        std::vector<EffectPlacement> Effects;   // script-placed effects (empty: none read)
        std::string MapEnts;                        // the entity string, as the game keeps it
        std::map<std::string, std::string> EntityKeys;   // a key the entity string numbers (MW3, Ghosts on), by number
        Maps::Collision Collision;                  // the clip map's brushes, when the game reads them
        // the game's decal sort values that have an editor layer name (BO4: HydraX Bo4MapWorld.DecalLayerNames, measured against stock layerSortDecal); a sort with no name is written no layer
        std::map<int64_t, std::string> DecalLayerNames;
    };
}
