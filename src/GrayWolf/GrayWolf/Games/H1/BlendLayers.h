#pragma once

#include "ByteView.h"
#include "MapScene.h"

#include <cstdint>
#include <string>
#include <functional>
#include <map>
#include <string_view>
#include <vector>

// The layers of a Modern Warfare Remastered blend (docs/hydrax-port/mwr-materials.md 3.2, 3.3 and 12.3), as the game's world surfaces keep them.
//
// A blend material ("*145n_123n") holds the base layer's slots (colorMap, normalMap, specularMap, occlusionMap, revealMap) and, for each layer k above it,
// colorMap<k>, normalMap<k>, specularMap<k>, occlusionMap<k>, revealMap<k> (k = 1..6; the names hash as below). The weights of the layers are the vertex's:
//   - layers 1 to 3: the colour bytes G, B, A of the 44-byte vertex (R is unused);
//   - layers 4 to 6: bytes 0, 1, 2 of a trailing word (COLOR1) of the surface's slice of the world's vertex layer data (gfx_map +0x150 size, +0x158 pointer;
//     a world surface's u32 at offset 0 is the byte offset of its slice).
// A vertex's slice is, in this order, the layer texture coordinates (8 bytes per layer above the base: two floats), a packed 2x2 rotation word (4 bytes) for each
// layer that has a normal map, and for four or more layers above the base the COLOR1 word. That order is read, not proven for the rotation words: this reader uses
// only the uvs (the first bytes) and COLOR1 (the last), and takes the stride from the material; Shape says it, and the reader checks it against the slice sizes.
namespace H1Blend
{
    // The game's hash of a slot name: lower case, h = h * 33 ^ c (proven on colorMap 0xA0AB1041, normalMap 0x59D30D0F, revealMap 0x34D849D5)
    constexpr uint32_t Hash(std::string_view Name)
    {
        uint32_t H = 0;
        for (const char C : Name) H = (H * 33u) ^ static_cast<uint32_t>(static_cast<unsigned char>(C >= 'A' && C <= 'Z' ? C + 32 : C));
        return H;
    }

    // The slot a layer's texture has: kind of image and the layer it belongs to (0 = the base)
    enum class Slot { Colour, Normal, Specular, Occlusion, Reveal };
    // The semantic hash of a layer slot: "colorMap1" for Slot::Colour layer 1; layer 0 is the base's "colorMap"
    uint32_t SemanticOf(Slot S, uint32_t Layer);
    // The slot and layer a semantic hash is, false when it is none of them (layers 0 to 6)
    bool SlotOf(uint32_t Semantic, Slot& S, uint32_t& Layer);
    // The constant revealParams<k> (k = 1..6): x is the sharpness of the layer's reveal
    uint32_t RevealParamsOf(uint32_t Layer);

    // What a blend's vertex slice is made of
    struct Shape
    {
        uint32_t Layers = 0;     // the layers above the base (a colorMap<k> slot each, counted up to the first missing one)
        uint32_t Rotations = 0;  // the layers with a normalMap<k>
        uint32_t Stride() const { return Layers * 8 + Rotations * 4 + (Layers >= 4 ? 4 : 0); }
        bool Color1() const { return Layers >= 4; }
    };
    // From the semantic hashes of a material's textures and its name ("145n_123n_9": a layer id ending in 'n' has a rotation word in the slice; measured on mp_crash, where
    // reading a rotation per normalMap<k> slot gave 6,222 of 15,029 surfaces a slice of another size, and the 'n' rule is the size the slices have). When the name does not
    // have one token per layer, the normal maps count. Layers 0 when it holds no colorMap1.
    Shape ShapeOf(const std::vector<uint32_t>& Semantics, const std::string& Name = std::string());

    // One vertex: Color is the 44-byte vertex's colour bytes (R G B A); Slice the Shape::Stride bytes of the vertex's slice. Weights of layers 4..6 are -1 when the
    // slice is too short for them. Texture coordinates that are not finite make the weight of that layer -1 (not read).
    void Decode(ByteView Slice, const Shape& S, const uint8_t Color[4], Maps::BlendVertex& Out);

    // The layer materials of a level's blends. Every blend material of the scene ("145n_123n": a colorMap1 slot) gets, for each layer above its base, a scene material of its own
    // (Material::Layers names them, bottom to top): the layer's colour, normal, specular, occlusion and reveal images re-bound to the base slots, the techset name
    // "wc_layer" (techset_rules.txt draws the BO3 decal family from it), the reveal's sharpness K (revealParams<k>.x) as a revealParams constant, and the layer's surface word
    // from SurfaceOfId (the layer id's bits, with its evidence). One material per distinct (colour, normal, specular, occlusion, reveal image, K) of the level, named from the
    // colour image ("<stem>_dec", "<stem>_dec_k4" with a reveal; "_<hash>" when two distinct arts share a name). Returns the number of layer materials made.
    size_t AddLayerMaterials(Maps::MapScene& Scene, const std::function<int64_t(const std::string& Id, std::string& Via)>& SurfaceOfId, std::map<std::string, size_t>* Counters = nullptr);

    int SelfTest(std::string& Failures);
}
