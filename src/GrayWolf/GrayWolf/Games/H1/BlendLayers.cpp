#include "stdafx.h"

#include "Games/H1/BlendLayers.h"

#include "Games/H1/LayerBits.h"

#include <algorithm>
#include <array>
#include "MapMaterials.h"
#include "MaterialDescription.h"
#include <map>
#include <span>
#include <set>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

namespace H1Blend
{
    namespace
    {
        constexpr std::string_view Names[5] = { "colorMap", "normalMap", "specularMap", "occlusionMap", "revealMap" };

        // every layer slot's hash, base and layers 1..6, built once from the names
        const std::array<std::array<uint32_t, 7>, 5>& Table()
        {
            static const auto T = [] {
                std::array<std::array<uint32_t, 7>, 5> Out{};
                for (size_t S = 0; S < 5; S++)
                    for (uint32_t L = 0; L <= 6; L++) Out[S][L] = Hash(L == 0 ? std::string(Names[S]) : std::string(Names[S]) + std::to_string(L));
                return Out;
            }();
            return T;
        }
    }

    uint32_t SemanticOf(Slot S, uint32_t Layer)
    {
        return Layer <= 6 ? Table()[static_cast<size_t>(S)][Layer] : 0u;
    }

    bool SlotOf(uint32_t Semantic, Slot& S, uint32_t& Layer)
    {
        const auto& T = Table();
        for (size_t K = 0; K < T.size(); K++)
            for (uint32_t L = 0; L <= 6; L++)
                if (T[K][L] == Semantic)
                {
                    S = static_cast<Slot>(K);
                    Layer = L;
                    return true;
                }
        return false;
    }

    uint32_t RevealParamsOf(uint32_t Layer)
    {
        return Layer >= 1 && Layer <= 6 ? Hash("revealParams" + std::to_string(Layer)) : 0u;
    }

    Shape ShapeOf(const std::vector<uint32_t>& Semantics, const std::string& Name)
    {
        const auto Has = [&](uint32_t H) {
            for (const uint32_t S : Semantics)
                if (S == H) return true;
            return false;
        };
        Shape Out;
        while (Out.Layers < Maps::MaxBlendLayers && Has(SemanticOf(Slot::Colour, Out.Layers + 1))) Out.Layers++;
        // the id tokens of the name: one per layer, the base first
        std::vector<std::string> Tokens;
        size_t Start = 0;
        while (Start < Name.size() && (Name[Start] == ',' || Name[Start] == '*')) Start++;
        while (Start <= Name.size())
        {
            size_t End = Name.find('_', Start);
            if (End == std::string::npos) End = Name.size();
            if (End > Start) Tokens.push_back(Name.substr(Start, End - Start));
            Start = End + 1;
        }
        if (Tokens.size() == static_cast<size_t>(Out.Layers) + 1)
        {
            // a token ending in 'dn' holds two normal frames, 'n' or 'z' one, any other none; the first frame of the material needs no rotation word, each other one has one
            // (tested: this gives the slice size of every blended surface of mp_crash, mp_bog, mp_crossfire, mp_strike and mp_shipment, docs/hydrax-port/m3.md)
            // a token ending in 'z' always has one (mp_shipment: 127_143z holds 12 bytes a vertex, 8 of uv and one rotation word; mp_crash: 317n_337z the same)
            uint32_t Frames = 0, Zs = 0;
            for (const std::string& T : Tokens)
            {
                const bool Dn = T.size() >= 2 && T.compare(T.size() - 2, 2, "dn") == 0;
                Frames += Dn ? 2 : T.back() == 'n' ? 1 : 0;
                Zs += T.back() == 'z' ? 1 : 0;
            }
            Out.Rotations = (Frames > 0 ? Frames - 1 : 0) + Zs;
        }
        else
            for (uint32_t L = 1; L <= Out.Layers; L++)
                if (Has(SemanticOf(Slot::Normal, L))) Out.Rotations++;
        return Out;
    }

    void Decode(ByteView Slice, const Shape& S, const uint8_t Color[4], Maps::BlendVertex& Out)
    {
        Out = Maps::BlendVertex();
        const std::span<const uint8_t, 4> Rgba(Color, 4);
        const uint32_t Layers = S.Layers < Maps::MaxBlendLayers ? S.Layers : Maps::MaxBlendLayers;
        // the rotation words follow the uvs: four bytes each, v * 2.007874 - 1 a component of a 2x2 matrix (the vertex shader's expansion)
        uint8_t RotClass = 0;
        for (uint32_t R = 0; R < S.Rotations; R++)
        {
            const size_t At = static_cast<size_t>(S.Layers) * 8 + static_cast<size_t>(R) * 4;
            if (At + 4 > Slice.size()) break;
            double X[4];
            for (size_t K = 0; K < 4; K++) X[K] = Slice[At + K] / 255.0 * 2.007874 - 1.0;
            const bool Off = std::fabs(X[1]) < 0.1 && std::fabs(X[2]) < 0.1;
            const bool Unit = std::fabs(std::fabs(X[0]) - 1.0) < 0.1 && std::fabs(std::fabs(X[3]) - 1.0) < 0.1;
            const bool Plus = X[0] > 0 && X[3] > 0;
            const uint8_t Class = Off && Unit ? (Plus ? 0 : 1) : 2;
            RotClass = std::max(RotClass, Class);
        }
        for (uint32_t L = 0; L < Layers; L++)
        {
            // weights: layers 1..3 are the vertex colour's G, B, A; layers 4..6 are the trailing COLOR1 word's bytes 0..2
            int Weight = -1;
            if (L < 3) Weight = Rgba[1 + L];
            else if (Slice.size() >= S.Stride() && S.Stride() >= 4) Weight = Slice[S.Stride() - 4 + (L - 3)];
            float Uv[2] = { 0, 0 };
            bool Ok = Slice.size() >= static_cast<size_t>(L + 1) * 8;
            if (Ok)
            {
                std::memcpy(Uv, Slice.subspan(static_cast<size_t>(L) * 8, 8).data(), 8);
                Ok = std::isfinite(Uv[0]) && std::isfinite(Uv[1]);
            }
            Out.Weight[L] = static_cast<int16_t>(Ok ? Weight : -1);
            if (!Ok && Slice.size() >= static_cast<size_t>(L + 1) * 8) Out.BadUv = static_cast<uint8_t>(Out.BadUv | (1u << L));
            Out.Rot = RotClass;
            if (Ok)
            {
                Out.Uv[L][0] = Uv[0];
                Out.Uv[L][1] = Uv[1];
            }
        }
    }

    namespace
    {
        // the art of one layer: its images by slot and K
        struct LayerArt
        {
            std::string Colour, Normal, Specular, Occlusion, Reveal;
            float K = -1.0f;
            std::string Id;   // the layer id of the first blend that held the art (the surface word is looked up by it)
            // K by its exact bits: two sharpnesses that differ in the last bit are two arts
            std::string Key() const
            {
                uint32_t Bits;
                std::memcpy(&Bits, &K, 4);
                return Colour + "|" + Normal + "|" + Specular + "|" + Occlusion + "|" + Reveal + "|" + std::format("{:08x}", Bits);
            }
        };

        uint32_t Fnv(const std::string& Text)
        {
            uint32_t H = 2166136261u;
            for (const char C : Text) H = (H ^ static_cast<unsigned char>(C)) * 16777619u;
            return H;
        }

        std::string Lower(std::string S)
        {
            for (char& C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            return S;
        }

        // "ct_sand_02_col" -> "ct_sand_02"
        std::string StemOf(const std::string& Image)
        {
            std::string S = Lower(Image);
            for (const char* Suffix : { "_col", "_c" })
            {
                const std::string X = Suffix;
                if (S.size() > X.size() && S.compare(S.size() - X.size(), X.size(), X) == 0)
                {
                    S.resize(S.size() - X.size());
                    break;
                }
            }
            return S;
        }

        std::string KText(float K)
        {
            std::string T = std::format("{:g}", K);
            for (char& C : T)
                if (C == '.') C = 'p';
            return T;
        }

        // revealParams<k>.x from the material's constant table (0x20-byte records: hash, 12 name bytes, four floats); -1 when absent
        float RevealK(const Maps::Material& M, uint32_t Layer)
        {
            const uint32_t Want = RevealParamsOf(Layer);
            for (const auto& Block : M.Constants)
                for (size_t At = 0; At + 32 <= Block.size(); At += 32)
                {
                    uint32_t Hash;
                    float K;
                    std::memcpy(&Hash, &Block[At], 4);
                    std::memcpy(&K, &Block[At + 16], 4);
                    if (Hash == Want && std::isfinite(K) && K >= 1.0f) return K;
                }
            return -1.0f;
        }
    }

    size_t AddLayerMaterials(Maps::MapScene& Scene, const std::function<int64_t(const std::string& Id, std::string& Via)>& SurfaceOfId, std::map<std::string, size_t>* Counters)
    {
        std::vector<std::string> Blends;
        for (const auto& [Name, M] : Scene.Materials)
        {
            std::vector<uint32_t> Semantics;
            for (const Maps::Texture& T : M.Textures) Semantics.push_back(T.Semantic);
            Semantics.insert(Semantics.end(), M.UnnamedSemantics.begin(), M.UnnamedSemantics.end());
            if (M.Layers.empty() && ShapeOf(Semantics, Name).Layers != 0) Blends.push_back(Name);
        }
        // the art of every layer of every blend, by key
        std::map<std::string, LayerArt> Arts;
        std::map<std::string, std::vector<std::string>> KeysOf;   // blend -> the key of each layer
        for (const std::string& Name : Blends)
        {
            const Maps::Material& M = Scene.Materials.at(Name);
            std::vector<uint32_t> Semantics;
            for (const Maps::Texture& T : M.Textures) Semantics.push_back(T.Semantic);
            Semantics.insert(Semantics.end(), M.UnnamedSemantics.begin(), M.UnnamedSemantics.end());
            const Shape Sh = ShapeOf(Semantics, Name);
            const std::vector<std::string> Ids = H1Layers::IdsOf(Name);
            for (uint32_t L = 1; L <= Sh.Layers; L++)
            {
                LayerArt Art;
                const auto Image = [&](Slot S) {
                    const uint32_t Want = SemanticOf(S, L);
                    for (const Maps::Texture& T : M.Textures)
                        if (T.Semantic == Want) return T.Image;
                    return std::string();
                };
                Art.Colour = Image(Slot::Colour), Art.Normal = Image(Slot::Normal), Art.Specular = Image(Slot::Specular), Art.Occlusion = Image(Slot::Occlusion), Art.Reveal = Image(Slot::Reveal);
                // a layer whose colour image the reader could not name is counted on its own and has no material (its slice is still sized right: the shape counts its slot)
                if (Art.Colour.empty())
                {
                    if (Counters != nullptr) (*Counters)["map_blend_layer_no_colour"]++;
                    KeysOf[Name].push_back(std::string());
                    continue;
                }
                if (!Art.Reveal.empty())
                {
                    Art.K = RevealK(M, L);
                    // a reveal with no sharpness (or one below 1) cannot be written: the vertex alpha needs K. The layer is a plain alpha layer and the reveal image is not bound.
                    if (Art.K < 1.0f)
                    {
                        if (Counters != nullptr) (*Counters)["map_blend_layer_reveal_no_k"]++;
                        Art.Reveal.clear();
                        Art.K = -1.0f;
                    }
                }
                if (Ids.size() == static_cast<size_t>(Sh.Layers) + 1) Art.Id = Ids[L];
                const std::string Key = Art.Key();
                KeysOf[Name].push_back(Key);
                Arts.emplace(Key, std::move(Art));
            }
        }
        // the rule the dedupe rests on (mwr-materials.md 3.1: an id always has the same colour image): per id the distinct arts, per art the distinct ids
        if (Counters != nullptr)
        {
            std::map<std::string, std::set<std::string>> ArtsOfId, IdsOfArt, ColoursOfId;
            for (const std::string& Name : Blends)
            {
                const std::vector<std::string> Ids = H1Layers::IdsOf(Name);
                const std::vector<std::string>& Keys = KeysOf[Name];
                for (size_t K = 0; K < Keys.size(); K++)
                    if (Ids.size() == Keys.size() + 1 && !Keys[K].empty())
                    {
                        ArtsOfId[Ids[K + 1]].insert(Keys[K]);
                        ColoursOfId[Ids[K + 1]].insert(Arts.at(Keys[K]).Colour);
                        IdsOfArt[Keys[K]].insert(Ids[K + 1]);
                    }
            }
            (*Counters)["map_blend_layer_ids"] += ArtsOfId.size();
            for (const auto& [Id, Set] : ArtsOfId) (*Counters)["map_blend_layer_ids_one_art"] += Set.size() == 1;
            for (const auto& [Id, Set] : ColoursOfId) (*Counters)["map_blend_layer_ids_one_colour"] += Set.size() == 1;
            for (const auto& [Key, Set] : IdsOfArt) (*Counters)["map_blend_arts_one_id"] += Set.size() == 1;
            (*Counters)["map_blend_arts"] += Arts.size();
            (*Counters)["map_blend_layers_total"] += [&] { size_t N = 0; for (const auto& [Name, Keys] : KeysOf) N += Keys.size(); return N; }();
        }
        // names: the art's own name and a short hash of the whole art key, always. The same stem can mean other art on another map, and the owner's maps share one BO3 root; the
        // key holds the image names and the exact K, so the same art is the same name wherever it is met.
        std::map<std::string, std::string> NameOf;
        std::set<std::string> Taken;
        for (const auto& [Key, Art] : Arts)
        {
            std::string Name = StemOf(Art.Colour) + "_dec" + (Art.Reveal.empty() ? std::string() : "_k" + KText(Art.K)) + std::format("_{:06x}", Fnv(Key) & 0xffffff);
            while (Taken.count(Name) || Scene.Materials.count(Name)) Name += "_x";   // two keys with one hash and one stem: a name apart, never one material for two arts
            Taken.insert(Name);
            NameOf[Key] = Name;
        }
        for (const auto& [Key, Art] : Arts)
        {
            Maps::Material Layer;
            Layer.Name = NameOf[Key];
            Layer.Techset = "wc_layer";
            Layer.IsLayer = true;
            const auto Bind = [&](Slot S, const std::string& Image) {
                if (Image.empty()) return;
                Layer.Textures.push_back({ SemanticOf(S, 0), Image });
            };
            Bind(Slot::Colour, Art.Colour), Bind(Slot::Normal, Art.Normal), Bind(Slot::Specular, Art.Specular), Bind(Slot::Occlusion, Art.Occlusion);
            if (!Art.Reveal.empty()) Layer.Textures.push_back({ 0x34D849D5u, Art.Reveal });   // the base reveal slot ("revealMap")
            Layer.ColorMap = Art.Colour;
            if (Art.K >= 1.0f)
            {
                std::vector<uint8_t> Record(32, 0);
                const uint32_t Hash = 0x8C67DED5u;   // "revealParams"
                std::memcpy(Record.data(), &Hash, 4);
                std::memcpy(&Record[16], &Art.K, 4);
                Layer.Constants.push_back(std::move(Record));
            }
            if (!Art.Id.empty() && SurfaceOfId)
            {
                std::string Via;
                const int64_t Word = SurfaceOfId(Art.Id, Via);
                Layer.SurfaceFlags = Word;
                Layer.SurfaceVia = Via;
            }
            Scene.Materials.emplace(Layer.Name, std::move(Layer));
        }
        for (const std::string& Name : Blends)
        {
            Maps::Material& M = Scene.Materials.at(Name);
            for (const std::string& Key : KeysOf[Name])
            {
                if (Key.empty())
                {
                    M.Layers.push_back({ std::string(), false, -1.0f });   // no material for a layer with no colour image
                    continue;
                }
                const LayerArt& Art = Arts.at(Key);
                M.Layers.push_back({ NameOf.at(Key), !Art.Reveal.empty(), Art.K });
            }
        }
        return Arts.size();
    }

    uint32_t NameHash(std::string_view Name)
    {
        uint32_t H = 0;
        for (const char C : Name) H = (H * 33u) ^ static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(C)));
        return H;
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("h1 blend: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        // the name hash against the values read off the game's own tables (mwr-materials.md 2 and 5)
        Check(NameHash("colorMap") == 0xa0ab1041u && NameHash("normalMap") == 0x59d30d0fu && NameHash("specularMap") == 0x34ecccb3u && NameHash("envMapParms") == 0x3d9994dcu &&
                  NameHash("revealParams") == 0x8C67DED5u && NameHash("envMapParms1") == 0xf0cc306du && NameHash("COLORMAP") == 0xa0ab1041u,
              "the name hash reproduces the proven hashes of the game's tables (case aside)");
        Check(NameHash("hdrColorParms") == 0xc97690deu, "hdrColorParms hashes to the constant the sky material holds (c97690de, x = 30 on mp_crash; the name is plural, hdrColorParm is 1d60af0d and is not in the table)");
        // the hash is the game's: three names the zones prove, and the layer slots measured in mwr-materials.md 4.1
        Check(Hash("colorMap") == 0xA0AB1041u && Hash("normalMap") == 0x59D30D0Fu && Hash("revealMap") == 0x34D849D5u, "the slot name hash is the game's (colorMap, normalMap, revealMap)");
        Check(SemanticOf(Slot::Colour, 1) == 0xB60D1850u && SemanticOf(Slot::Colour, 2) == 0xB60D1853u && SemanticOf(Slot::Normal, 1) == 0x9434AEDEu &&
                  SemanticOf(Slot::Specular, 1) == 0xD2866322u && SemanticOf(Slot::Occlusion, 1) == 0x60411F60u && SemanticOf(Slot::Reveal, 1) == 0xCFE18444u && SemanticOf(Slot::Reveal, 4) == 0xCFE18441u,
              "the layer slots hash to the values the zones hold");
        Check(RevealParamsOf(1) == 0x1963B944u && RevealParamsOf(0) == 0 && RevealParamsOf(7) == 0, "revealParams<k>");
        Slot S{};
        uint32_t L = 9;
        Check(SlotOf(0xB60D1853u, S, L) && S == Slot::Colour && L == 2 && SlotOf(0x34D849D5u, S, L) && S == Slot::Reveal && L == 0 && !SlotOf(0x12345678u, S, L), "a semantic names its slot and layer");

        // the shape of a material: counted up to the first missing colour, the rotations by the normal maps
        const std::vector<uint32_t> Two = { SemanticOf(Slot::Colour, 0), SemanticOf(Slot::Colour, 1), SemanticOf(Slot::Normal, 1), SemanticOf(Slot::Reveal, 1) };
        const Shape A = ShapeOf(Two);
        Check(A.Layers == 1 && A.Rotations == 1 && A.Stride() == 12 && !A.Color1(), "one layer above the base with a normal map is 12 bytes a vertex", std::to_string(A.Stride()));
        const std::vector<uint32_t> Five = { SemanticOf(Slot::Colour, 1), SemanticOf(Slot::Colour, 2), SemanticOf(Slot::Colour, 3), SemanticOf(Slot::Colour, 4), SemanticOf(Slot::Normal, 1),
                                             SemanticOf(Slot::Normal, 2), SemanticOf(Slot::Normal, 4) };
        const Shape B = ShapeOf(Five);
        Check(B.Layers == 4 && B.Rotations == 3 && B.Stride() == 48 && B.Color1(), "four layers, three with a normal map: 48 bytes and a COLOR1 word", std::to_string(B.Stride()));
        Check(ShapeOf({ SemanticOf(Slot::Colour, 2) }).Layers == 0 && ShapeOf({}).Layers == 0, "a layer 2 with no layer 1 is no layer");
        // measured on mp_crash: *145n_123n_146n_9_19n has 4 layers above the base and 3 rotation words (the token 9 has none): 48 bytes, not 52
        const Shape C = ShapeOf(Five, "145n_123n_146n_9_19n");
        Check(C.Layers == 4 && C.Rotations == 3 && C.Stride() == 48, "a layer id with no 'n' has no rotation word", std::to_string(C.Stride()));
        // the slice sizes mp_crash holds, per vertex: 539n_457dn 16, 317n_337z 12, 181n_371n_370dn 28, 9_121n 8, 413_6n_119n 20, 82_9_143n 16, 456n_371n_142n_370dn_457dn 60
        const std::vector<uint32_t> Slots = { SemanticOf(Slot::Colour, 1), SemanticOf(Slot::Colour, 2), SemanticOf(Slot::Colour, 3), SemanticOf(Slot::Colour, 4), SemanticOf(Slot::Colour, 5) };
        const auto Take = [&](size_t N) { return std::vector<uint32_t>(Slots.begin(), Slots.begin() + static_cast<std::ptrdiff_t>(N)); };
        Check(ShapeOf(Take(1), "127_143z").Stride() == 12 && ShapeOf(Take(1), "539n_457dn").Stride() == 16 && ShapeOf(Take(1), "317n_337z").Stride() == 12 && ShapeOf(Take(2), "181n_371n_370dn").Stride() == 28 &&
                  ShapeOf(Take(1), "9_121n").Stride() == 8 && ShapeOf(Take(2), "413_6n_119n").Stride() == 20 && ShapeOf(Take(2), "82_9_143n").Stride() == 16 &&
                  ShapeOf(Take(4), "456n_371n_142n_370dn_457dn").Stride() == 60,
              "the slice sizes measured on mp_crash (dn holds two frames, the first frame no rotation word)");
        const Shape D = ShapeOf({ SemanticOf(Slot::Colour, 1), SemanticOf(Slot::Colour, 2), SemanticOf(Slot::Normal, 1), SemanticOf(Slot::Normal, 2) }, ",*59n_244_9");
        Check(D.Layers == 2 && D.Rotations == 0 && D.Stride() == 16, "59n_244_9: two layers, no rotation word: 16 bytes", std::to_string(D.Stride()));

        // the layer materials of a level: two blends share art, one is new
        {
            Maps::MapScene Scene;
            const auto Blend = [&](const char* Name, std::initializer_list<const char*> Colours, float K) {
                Maps::Material M;
                M.Name = Name;
                uint32_t Layer = 0;
                for (const char* C : Colours)
                {
                    M.Textures.push_back({ SemanticOf(Slot::Colour, Layer), C });
                    if (Layer > 0 && std::string(C) != "b_col") M.Textures.push_back({ SemanticOf(Slot::Reveal, Layer), std::string(C) + "_rvl" });
                    Layer++;
                }
                std::vector<uint8_t> Record(32, 0);
                const uint32_t Hash = RevealParamsOf(1);
                std::memcpy(Record.data(), &Hash, 4);
                std::memcpy(&Record[16], &K, 4);
                M.Constants.push_back(Record);
                Scene.Materials[Name] = M;
            };
            Blend("1n_2n", { "a_col", "b_col" }, 4.0f);   // layer 1: b_col, no reveal
            Blend("3n_4n", { "a_col", "c_col" }, 4.0f);   // layer 1: c_col with a reveal, K 4
            Blend("5n_4n", { "a_col", "c_col" }, 8.0f);   // the same colour with another K: another material
            Blend("6n_4n", { "a_col", "c_col" }, 4.0f);   // the same art as 3n_4n: the same material
            Blend("11n_4n", { "a_col", "c_col" }, std::nextafter(4.0f, 5.0f));   // K one bit above 4: another art, not rounded into the same
            Scene.Materials["plain"].Name = "plain";
            std::map<std::string, size_t> Counters;
            const size_t N = AddLayerMaterials(Scene, [](const std::string& Id, std::string& Via) { Via = "id " + Id; return Id == "4n" ? (int64_t(1) << 5) : int64_t(-1); }, &Counters);
            Check(N == 4, "one material per distinct art (colour, normal, specular, occlusion, reveal, exact K): b, c at K 4, c at K 8, c at K 4 plus one bit", std::to_string(N));
            const Maps::Material& A = Scene.Materials.at("3n_4n");
            Check(A.Layers.size() == 1 && A.Layers[0].Reveal && A.Layers[0].Sharpness == 4.0f && Scene.Materials.at("6n_4n").Layers[0].Material == A.Layers[0].Material &&
                      Scene.Materials.at("5n_4n").Layers[0].Material != A.Layers[0].Material,
                  "the same art is the same material across blends, another K is another", A.Layers.empty() ? "" : A.Layers[0].Material);
            const auto Named = [&](const char* Blend) { return Scene.Materials.at(Blend).Layers[0].Material; };
            Check(Named("3n_4n").rfind("c_dec_k4_", 0) == 0 && Named("3n_4n").size() == std::string("c_dec_k4_").size() + 6 && Named("1n_2n").rfind("b_dec_", 0) == 0 && Named("5n_4n").rfind("c_dec_k8_", 0) == 0,
                  "named from the colour image, with K when there is a reveal, and a hash of the whole art", A.Layers[0].Material);
            Check(Named("3n_4n") != Named("5n_4n") && Named("3n_4n") == Named("6n_4n") && Named("3n_4n") != Named("11n_4n"), "the hash makes the same art one name and another K, down to one bit, another");
            const Maps::Material& L = Scene.Materials.at(A.Layers[0].Material);
            Check(L.Techset == "wc_layer" && L.SurfaceFlags == (int64_t(1) << 5) && L.SurfaceVia == "id 4n" && L.ColorMap == "c_col" && Scene.Materials.at("plain").Layers.empty(),
                  "the layer's techset, its surface by its id and its colour map");
            bool Rebound = false;
            for (const Maps::Texture& T : L.Textures) Rebound = Rebound || (T.Semantic == 0x34D849D5u && T.Image == "c_col_rvl");
            Check(Rebound, "the layer's reveal image is bound to the base reveal slot");
            Check(Counters["map_blend_layer_ids"] == 2 && Counters["map_blend_layer_ids_one_art"] == 1 && Counters["map_blend_arts"] == 4, "the id rule is counted", std::to_string(Counters["map_blend_layer_ids"]));
            // the adapter: a layer with a reveal is a reveal decal whose image is 1 - green and whose soft edge is 1/(2K)
            Materials::Source Src;
            Src.Name = L.Name;
            Src.Techset = L.Techset;
            Src.Constants = L.Constants;
            Src.Images.push_back({ "c_col.png", Materials::Role::Unknown, 0xA0AB1041u, 0 });
            Src.Images.push_back({ "c_col_rvl.png", Materials::Role::Unknown, 0x34D849D5u, 0 });
            const Materials::Description D = Materials::AdapterFor("H1").Describe(Src);
            Check(D.TechsetName.rfind("lit_decal_reveal", 0) == 0 && D.RevealInvert && D.RevealChannel == 1 && D.RevealRamp == 0.0f && std::fabs(D.RevealSoftEdge - 0.125f) < 1e-6f,
                  "a layer with a reveal is a reveal decal: image 1 - green, ramp 0, soft edge 1/(2K)", D.TechsetName + " " + std::to_string(D.RevealSoftEdge));
            Check(D.Approximations.find("weight_squared_interpolation") != std::string::npos && D.Approximations.find("colour_alpha_not_folded") != std::string::npos, "a reveal layer states the squared-weight and colour-alpha approximations", D.Approximations);
            // a sharpness of 512 would be a soft edge of 0.00098, outside material.awi's 0.01..1: it is written as 0.01 and said
            {
                Materials::Source Sharp = Src;
                const float K512 = 512.0f;
                std::memcpy(&Sharp.Constants[0][16], &K512, 4);
                const Materials::Description DS = Materials::AdapterFor("H1").Describe(Sharp);
                Check(std::fabs(DS.RevealSoftEdge - 0.01f) < 1e-6f && DS.Approximations.find("reveal_soft_edge_clamped") != std::string::npos, "a soft edge below 0.01 is clamped to the range awi allows, and said", std::to_string(DS.RevealSoftEdge) + " " + DS.Approximations);
            }
            Src.Images.pop_back();
            const Materials::Description P = Materials::AdapterFor("H1").Describe(Src);
            Check(P.Approximations.find("weight_squared_interpolation") == std::string::npos && P.Approximations.find("colour_alpha_not_folded") != std::string::npos, "a layer with no reveal does not state the squared-weight one", P.Approximations);
            // a blend whose layers the scene carries has none dropped, unless some of its surfaces' weights were not read
            {
                Maps::MapScene S3;
                Maps::Material B3;
                B3.Name = "3n_4n";
                B3.Techset = "l_sm_lmpb";
                B3.Textures = { { SemanticOf(Slot::Colour, 0), "a_col" }, { SemanticOf(Slot::Colour, 1), "d_col" } };
                B3.Layers.push_back({ "d_dec_000000", false, -1.0f });
                S3.Materials["3n_4n"] = B3;
                S3.Images["a_col"].File = "x/a_col.png";
                S3.Images["d_col"].File = "x/d_col.png";
                const int Whole = Maps::DescribeMaterial(S3, "3n_4n", Materials::AdapterFor("H1"), "x").second.LayersDropped;
                S3.Materials["3n_4n"].SurfacesUnread = 1;
                const int Hidden = Maps::DescribeMaterial(S3, "3n_4n", Materials::AdapterFor("H1"), "x").second.LayersDropped;
                Check(Whole == 0 && Hidden == 1, "layers_dropped is 0 for a carried blend and stays when a surface's weights were not read", std::to_string(Whole) + "/" + std::to_string(Hidden));
            }
            Check(P.TechsetName.rfind("lit_decal", 0) == 0 && P.TechsetName.find("reveal") == std::string::npos && !P.RevealInvert, "a layer with no reveal is a plain decal", P.TechsetName);
        }
        // the rotation words: 0xff7f7fff is the identity, 0x007f7fff a flip of x, 0x7fff00ff a quarter turn
        {
            std::array<uint8_t, 48> R{};
            const Shape Two{ 2, 2 };
            const uint8_t Col[4] = { 255, 1, 2, 3 };
            Maps::BlendVertex V2;
            const uint8_t Id[4] = { 0xfe, 0x7f, 0x7f, 0xfe }, Flip[4] = { 0x00, 0x7f, 0x7f, 0xfe }, Turn[4] = { 0x7f, 0xfe, 0x00, 0x7f };
            std::copy(std::begin(Id), std::end(Id), R.begin() + 16), std::copy(std::begin(Id), std::end(Id), R.begin() + 20);
            Decode(ByteView(R.data(), 24), Two, Col, V2);
            Check(V2.Rot == 0, "two identity rotation words are class 0", std::to_string(V2.Rot));
            std::copy(std::begin(Flip), std::end(Flip), R.begin() + 20);
            Decode(ByteView(R.data(), 24), Two, Col, V2);
            Check(V2.Rot == 1, "a flipped rotation word is class 1", std::to_string(V2.Rot));
            std::copy(std::begin(Turn), std::end(Turn), R.begin() + 16);
            Decode(ByteView(R.data(), 24), Two, Col, V2);
            Check(V2.Rot == 2, "a quarter turn is class 2", std::to_string(V2.Rot));
            const uint8_t Shear[4] = { 0xfe, 190, 0x7f, 0xfe };   // a unit diagonal with an off-diagonal of 0.5
            std::copy(std::begin(Id), std::end(Id), R.begin() + 20), std::copy(std::begin(Shear), std::end(Shear), R.begin() + 16);
            Decode(ByteView(R.data(), 24), Two, Col, V2);
            Check(V2.Rot == 2, "a unit diagonal with an off-diagonal is not an identity or a flip", std::to_string(V2.Rot));
        }
        // a reveal with no sharpness is a plain alpha layer whose reveal image is not bound; a layer whose colour image has no name has no material and is counted
        {
            Maps::MapScene S2;
            Maps::Material A;
            A.Name = "7n_8n";
            A.Textures = { { SemanticOf(Slot::Colour, 0), "a_col" }, { SemanticOf(Slot::Colour, 1), "d_col" }, { SemanticOf(Slot::Reveal, 1), "d_rvl" } };
            S2.Materials["7n_8n"] = A;
            Maps::Material N;
            N.Name = "9n_10n";
            N.Textures = { { SemanticOf(Slot::Colour, 0), "a_col" } };
            N.UnnamedSemantics = { SemanticOf(Slot::Colour, 1) };
            S2.Materials["9n_10n"] = N;
            std::map<std::string, size_t> C2;
            AddLayerMaterials(S2, nullptr, &C2);
            const Maps::Material& R = S2.Materials.at("7n_8n");
            bool Bound = false;
            if (R.Layers.size() == 1 && S2.Materials.count(R.Layers[0].Material))
                for (const Maps::Texture& T : S2.Materials.at(R.Layers[0].Material).Textures) Bound = Bound || T.Semantic == 0x34D849D5u;
            Check(R.Layers.size() == 1 && !R.Layers[0].Reveal && !Bound && C2["map_blend_layer_reveal_no_k"] == 1, "a reveal with no K is a plain layer, its reveal image not bound, and counted", std::to_string(C2["map_blend_layer_reveal_no_k"]));
            const Maps::Material& U = S2.Materials.at("9n_10n");
            Check(U.Layers.size() == 1 && U.Layers[0].Material.empty() && C2["map_blend_layer_no_colour"] == 1, "a layer with an unnamed colour image has no material and is counted on its own", std::to_string(C2["map_blend_layer_no_colour"]));
        }
        // a vertex: weights of layers 1..3 from the colour bytes, 4 from COLOR1.x, the uvs from the first bytes
        std::array<uint8_t, 48> Bytes{};
        const float Uvs[8] = { 0.5f, 1.5f, 2.5f, 3.5f, -1.0f, 2.0f, 8.0f, 9.0f };
        std::memcpy(Bytes.data(), Uvs, sizeof Uvs);
        Bytes[44] = 200;   // COLOR1.x
        Bytes[45] = 99;    // COLOR1.y: no layer 5 here
        const uint8_t Colour[4] = { 255, 10, 20, 30 };
        Maps::BlendVertex V;
        Decode(Bytes, B, Colour, V);
        Check(V.Weight[0] == 10 && V.Weight[1] == 20 && V.Weight[2] == 30 && V.Weight[3] == 200 && V.Weight[4] == -1 && V.Weight[5] == -1, "weights: G, B, A of the colour then COLOR1.x; layers beyond are not read");
        Check(V.Uv[0][0] == 0.5f && V.Uv[0][1] == 1.5f && V.Uv[3][0] == 8.0f && V.Uv[3][1] == 9.0f, "each layer has its own texture coordinates");
        // a weight of 0 is read (0), not unread (-1)
        const uint8_t Zero[4] = { 255, 0, 0, 0 };
        Decode(Bytes, B, Zero, V);
        Check(V.Weight[0] == 0 && V.Weight[1] == 0, "a zero weight is a read weight");
        // a slice too short: the layers it cannot hold are not read
        Decode(ByteView(Bytes.data(), 16), B, Colour, V);
        Check(V.Weight[0] == 10 && V.Weight[1] == 20 && V.Weight[2] == -1 && V.Weight[3] == -1, "a short slice reads the layers it holds and no more");
        // a texture coordinate that is not finite is not read
        const float Bad = std::nanf("");
        std::memcpy(&Bytes[8], &Bad, 4);
        Decode(Bytes, B, Colour, V);
        Check(V.Weight[0] == 10 && V.Weight[1] == -1, "a layer whose texture coordinate is not finite is not read");
        return Failed;
    }
}
