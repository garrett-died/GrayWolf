// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX

#include <stdio.h>
#include <cmath>
#include <map>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <iterator>
#include <span>
#include <functional>
#include <string>
#include <format>
#include <cstring>
#include <fstream>

#include "CliOptions.h"
#include "CliCommands.h"

#include <CLI/CLI.hpp>
#include "CliSupport.h"

#include "CoDAssets.h"
#include "FileSystems.h"
#include "Strings.h"
#include "BinaryWriter.h"
#include "FastFile.h"
#include "Games/GameModule.h"
#include "ZoneAssetTypes.h"
#include "ZonekitZone.h"
#include "SurfaceTypes.h"

#include "XMEExport.h"
#include "XMBExport.h"
#include "XMEImport.h"
#include "Encryption.h"
#include "HuskyMath.h"
#include "GameImageProcessors.h"
#include "Bo3Target.GdtDb.h"
#include "Bo3Target.Placement.h"
#include "Bo3Target.Output.h"
#include "Bo3Target.Techsets.h"
#include "PoolCoverage.h"
#include "Bo3Target.Publish.h"
#include "Bo3Target.Awi.h"
#include "Bo3Target.Verify.h"
#include "MapFullExport.h"
#include "Bo3Target.ImageUnpack.h"
#include "Bo3Target.MaterialImages.h"
#include "ImageRoles.h"
#include "MaterialGdt.h"
#include "MapRadiantRules.h"
#include "MapRadiantWriter.h"
#include "MaterialDescription.h"
#include "GdtWriter.h"
#include "Image.h"
#include "FBXWriter.h"
#include "WraithModel.h"
#include "Games/Common/MeshDecode.h"

// Assertions that need neither game files nor a window, so they can run anywhere.
// Returns the number of failures and appends each one to Failures.
static int SelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Condition, const char* Name, const std::string& Got)
    {
        if (!Condition)
        {
            Failed++;
            Failures += std::format("FAIL {} (got '{}')\n", Name, Got.c_str());
        }
    };

    // Trimming
    {
        std::string A = "  spaced  ";
        Check(Strings::Trim(A) == "spaced", "Strings::Trim", A);

        std::string B = "\t\r\n padded";
        Check(Strings::Trim(B) == "padded", "Strings::Trim leading", B);

        std::string C = "padded \t\r\n";
        Check(Strings::Trim(C) == "padded", "Strings::Trim trailing", C);

        std::string D = "   ";
        Check(Strings::Trim(D).empty(), "Strings::Trim all blank", D);

        std::string E = "";
        Check(Strings::Trim(E).empty(), "Strings::Trim empty", E);

        // High bytes must not reach std::isspace as negative chars, and must survive intact
        std::string F = " \xC3\xA9\xC3\xA0\xE4\xB8\xAD ";
        const auto FTrimmed = Strings::Trim(F);
        Check(FTrimmed == "\xC3\xA9\xC3\xA0\xE4\xB8\xAD", "Strings::Trim non-ascii", FTrimmed);

        std::string G = "\x80\xFF";
        const auto GTrimmed = Strings::Trim(G);
        Check(GTrimmed == "\x80\xFF", "Strings::Trim high bytes", GTrimmed);
    }

    // Path helpers, every export path is built with these
    {
        const auto A = FileSystems::GetExtension("a\\b\\c.XPAK");
        Check(A == ".XPAK", "FileSystems::GetExtension", A);

        const auto B = FileSystems::GetFileName("a\\b\\c.xpak");
        Check(B == "c.xpak", "FileSystems::GetFileName", B);

        const auto C = FileSystems::GetFileNameWithoutExtension("a\\b\\c.xpak");
        Check(C == "c", "FileSystems::GetFileNameWithoutExtension", C);

        const auto D = (std::filesystem::path("a\\b") / "c.xpak").string();
        Check(D == "a\\b\\c.xpak", "path join separator", D);

        const auto E = FileSystems::GetDirectoryName("a\\b\\c.xpak");
        Check(E == "a\\b", "FileSystems::GetDirectoryName", E);
    }

    // Case folding, the asset search lowercases every name it looks at
    {
        std::string A = "MiXeD";
        Check(Strings::ToLower(A) == "mixed", "Strings::ToLower", A);

        std::string B = "MiXeD";
        Check(Strings::ToUpper(B) == "MIXED", "Strings::ToUpper", B);
    }

    // Half floats, against the values binary16 defines.
    {
        struct { uint16_t Half; float Expected; const char* What; } Cases[] = {
            { 0x0000,  0.0f,      "zero" },
            { 0x8000, -0.0f,      "negative zero" },
            { 0x3C00,  1.0f,      "one" },
            { 0xBC00, -1.0f,      "minus one" },
            { 0x4000,  2.0f,      "two" },
            { 0x3800,  0.5f,      "half" },
            { 0xC500, -5.0f,      "minus five" },
            { 0x7BFF,  65504.0f,  "largest normal" },
            { 0x0400,  0.000061035156f, "smallest normal" },
            { 0x0001,  0.000000059604645f, "smallest subnormal" },
        };

        for (const auto& Case : Cases)
        {
            const auto Got = HuskyMath::HalfToFloat(Case.Half);
            const auto Tolerance = std::fabs(Case.Expected) * 1e-6f + 1e-12f;

            Check(std::fabs(Got - Case.Expected) <= Tolerance,
                std::format("HalfToFloat {}", Case.What).c_str(),
                std::format("{:g} expected {:g}", Got, Case.Expected));
        }

        // Ten bit normals span -1 to 1, so the extremes and the centre pin the mapping
        const auto Low = HuskyMath::UnpackNormalTenBit(0);
        Check(std::fabs(Low.X + 1.0f) < 0.001f && std::fabs(Low.Y + 1.0f) < 0.001f && std::fabs(Low.Z + 1.0f) < 0.001f,
            "UnpackNormalTenBit at zero", std::format("{:f} {:f} {:f}", Low.X, Low.Y, Low.Z));

        const uint32_t AllOnes = 0x3FF | (0x3FFu << 10) | (0x3FFu << 20);
        const auto High = HuskyMath::UnpackNormalTenBit(AllOnes);
        Check(std::fabs(High.X - 1.0f) < 0.001f && std::fabs(High.Y - 1.0f) < 0.001f && std::fabs(High.Z - 1.0f) < 0.001f,
            "UnpackNormalTenBit at maximum", std::format("{:f} {:f} {:f}", High.X, High.Y, High.Z));

        // A byte of 127 is the zero point of the scaled form, so all three axes collapse
        const uint32_t Centred = 127u | (127u << 8) | (127u << 16) | (200u << 24);
        const auto Middle = HuskyMath::UnpackNormalScaled(Centred);
        Check(std::fabs(Middle.X) < 0.0001f && std::fabs(Middle.Y) < 0.0001f && std::fabs(Middle.Z) < 0.0001f,
            "UnpackNormalScaled at its zero point", std::format("{:f} {:f} {:f}", Middle.X, Middle.Y, Middle.Z));

        // And a unit vector should come back with roughly unit length
        const uint32_t Unit = 255u | (127u << 8) | (127u << 16) | (0xFFu << 24);
        const auto Long = HuskyMath::UnpackNormalScaled(Unit);
        const auto Length = std::sqrt(Long.X * Long.X + Long.Y * Long.Y + Long.Z * Long.Z);
        Check(Length > 0.0f && Length < 2.0f, "UnpackNormalScaled stays in range",
            std::format("length {:f}", Length));
    }

    // Binary FBX. The format makes every record state the byte offset where it ends, and
    // a wrong one is the classic way to produce a file that looks fine and opens in
    // nothing. So the check is to write a model, then walk the record tree back and
    // require every end offset to land exactly where the record actually stops.
    {
        FBXModelInput Model;
        Model.Name = "selftest";
        Model.Materials.push_back("mtl_test");

        Model.Bones.resize(2);
        Model.Bones[0].Name = "root";
        Model.Bones[0].Parent = -1;
        Model.Bones[1].Name = "j_head";
        Model.Bones[1].Parent = 0;
        Model.Bones[1].Position[2] = 10.0;

        FBXModelInput::Mesh Mesh;
        Mesh.Name = "mesh0";
        Mesh.MaterialIndex = 0;
        Mesh.Vertices.resize(4);

        for (int v = 0; v < 4; v++)
        {
            Mesh.Vertices[v].Position[0] = (v & 1) ? 1.0 : 0.0;
            Mesh.Vertices[v].Position[1] = (v & 2) ? 1.0 : 0.0;
            Mesh.Vertices[v].Normal[2] = 1.0;
            Mesh.Vertices[v].UV[0] = (v & 1) ? 1.0 : 0.0;
            Mesh.Vertices[v].UV[1] = (v & 2) ? 1.0 : 0.0;
            Mesh.Vertices[v].Weights.emplace_back(v < 2 ? 0 : 1, 1.0);
        }

        Mesh.Indices = { 0, 1, 2, 1, 3, 2 };
        Model.Meshes.push_back(std::move(Mesh));

        char TempPath[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, TempPath);
        const auto Path = std::string(TempPath) + "greyhound-selftest.fbx";

        Check(FBXWriter::Write(Path, Model), "binary FBX writes", Path.c_str());

        std::vector<uint8_t> File;
        {
            FILE* Raw = nullptr;
            if (fopen_s(&Raw, Path.c_str(), "rb") == 0 && Raw != nullptr)
            {
                fseek(Raw, 0, SEEK_END);
                File.resize((size_t)ftell(Raw));
                fseek(Raw, 0, SEEK_SET);
                if (fread(File.data(), 1, File.size(), Raw) != File.size())
                {
                    File.clear();
                }
                fclose(Raw);
            }
        }

        Check(File.size() > 64, "the FBX has content",
            std::format("{} bytes", (unsigned long long)File.size()).c_str());

        if (File.size() > 64)
        {
            Check(std::memcmp(File.data(), "Kaydara FBX Binary  ", 20) == 0, "FBX magic", "");

            uint32_t Version = 0;
            std::memcpy(&Version, File.data() + 23, sizeof(Version));
            Check(Version == 7400, "FBX version is 7400", std::format("{}", Version).c_str());

            // Walk every record, following the end offsets. A record whose declared end
            // does not match where its own contents finish is the failure this catches.
            uint32_t Records = 0;
            uint32_t Mismatches = 0;
            uint32_t Deepest = 0;

            const std::function<void(size_t, size_t, uint32_t)> Walk =
                [&](size_t At, size_t Limit, uint32_t Depth)
            {
                Deepest = std::max(Deepest, Depth);

                while (At + 13 <= Limit)
                {
                    uint32_t End = 0, Properties = 0, PropertyBytes = 0;
                    std::memcpy(&End, File.data() + At, 4);
                    std::memcpy(&Properties, File.data() + At + 4, 4);
                    std::memcpy(&PropertyBytes, File.data() + At + 8, 4);
                    const auto NameLength = File[At + 12];

                    // An all zero record closes the list
                    if (End == 0 && Properties == 0 && PropertyBytes == 0 && NameLength == 0)
                    {
                        return;
                    }

                    if (End > Limit || End <= At)
                    {
                        Mismatches++;
                        return;
                    }

                    Records++;

                    const auto ChildrenAt = At + 13 + NameLength + PropertyBytes;

                    if (ChildrenAt > End)
                    {
                        Mismatches++;
                        return;
                    }

                    // Anything left over past the properties has to be nested records
                    if (ChildrenAt < End)
                    {
                        Walk(ChildrenAt, End, Depth + 1);
                    }

                    At = End;
                }
            };

            Walk(27, File.size(), 0);

            Check(Mismatches == 0, "every FBX record ends where it says it does",
                std::format("{} bad of {}", Mismatches, Records).c_str());
            Check(Records > 20, "the FBX holds a real tree",
                std::format("{} records", Records).c_str());
            Check(Deepest >= 3, "the FBX tree nests",
                std::format("depth {}", Deepest).c_str());
        }

        remove(Path.c_str());
    }

    // GameImageUtil's per pixel processors. These are checked against what the maths says
    // a normal map is, not against the numbers the port happens to produce.
    {
        // A pixel encoding a normal pointing straight out: X and Y at zero, so Z takes the
        // whole unit length and comes back at 1, which is 1.0 in the channel
        ImagePixel Flat;
        Flat.A = 0.5f; Flat.G = 0.5f;
        const auto Straight = GameImageProcessors::UnpackNormalFromAlphaGreen(Flat);
        Check(std::fabs(Straight.R - 0.5f) < 1e-5f && std::fabs(Straight.G - 0.5f) < 1e-5f &&
              std::fabs(Straight.B - 1.0f) < 1e-5f,
            "normal map with no tilt points straight out",
            std::format("{:f} {:f} {:f}", Straight.R, Straight.G, Straight.B));

        // Every unpacked normal has to be a unit vector, which is the property that says
        // the reconstruction is right rather than merely plausible
        for (int i = 0; i <= 8; i++)
        {
            ImagePixel Tilted;
            Tilted.A = 0.5f + 0.05f * i;
            Tilted.G = 0.5f - 0.02f * i;

            const auto Normal = GameImageProcessors::UnpackNormalFromAlphaGreen(Tilted);
            const auto NX = Normal.R * 2.0f - 1.0f;
            const auto NY = Normal.G * 2.0f - 1.0f;
            const auto NZ = Normal.B * 2.0f - 1.0f;
            const auto Unit = std::sqrt(NX * NX + NY * NY + NZ * NZ);

            Check(std::fabs(Unit - 1.0f) < 1e-4f,
                std::format("unpacked normal {} is a unit vector", i).c_str(),
                std::format("length {:f}", Unit));
        }

        // The two forms differ only in which channel carried X
        ImagePixel Packed;
        Packed.R = 0.7f; Packed.G = 0.3f; Packed.B = 0.0f; Packed.A = 0.7f;
        const auto FromAlpha = GameImageProcessors::UnpackNormalFromAlphaGreen(Packed);
        const auto FromRed = GameImageProcessors::UnpackNormalFromRedGreen(Packed);
        Check(std::fabs(FromAlpha.R - FromRed.R) < 1e-6f && std::fabs(FromAlpha.B - FromRed.B) < 1e-6f,
            "both normal map forms agree when their X channels match", "");

        // A channel pair that already overreaches unit length leaves nothing for Z: Z = 0, which a
        // channel stores as 0.5 (GameImageUtil writes 0.0; that measured worse against stock), and
        // never the root of a negative number
        ImagePixel Overreaching;
        Overreaching.A = 1.0f; Overreaching.G = 1.0f;
        const auto NoZ = GameImageProcessors::UnpackNormalFromAlphaGreen(Overreaching);
        Check(NoZ.B == 0.5f && std::isfinite(NoZ.B), "a normal with no room left for Z comes back as Z = 0 (0.5)",
            std::format("{:f}", NoZ.B));

        // The packed normal, occlusion and gloss map hands back three images, and its
        // normal is normalised by construction
        ImagePixel Nog;
        Nog.R = 0.25f; Nog.G = 0.6f; Nog.B = 0.75f; Nog.A = 0.4f;
        ImagePixel Normal, Occlusion, Gloss;
        GameImageProcessors::UnpackNormalOcclusionGloss(Nog, Normal, Occlusion, Gloss);

        Check(std::fabs(Gloss.R - 0.25f) < 1e-6f && Gloss.R == Gloss.G && Gloss.G == Gloss.B,
            "gloss comes out of the red channel as grey", std::format("{:f}", Gloss.R));
        Check(std::fabs(Occlusion.R - 0.75f) < 1e-6f && Occlusion.R == Occlusion.B,
            "occlusion comes out of the blue channel as grey", std::format("{:f}", Occlusion.R));

        const auto MX = Normal.R * 2.0f - 1.0f;
        const auto MY = Normal.G * 2.0f - 1.0f;
        const auto MZ = Normal.B * 2.0f - 1.0f;
        const auto MLength = std::sqrt(MX * MX + MY * MY + MZ * MZ);
        Check(std::fabs(MLength - 1.0f) < 1e-4f, "the packed normal comes back a unit vector",
            std::format("length {:f}", MLength));

        // Splitting is the plain one: three greyscale maps out of three channels
        ImagePixel Red, Green, Blue;
        GameImageProcessors::SplitChannels(Nog, Red, Green, Blue);
        Check(Red.R == 0.25f && Green.G == 0.6f && Blue.B == 0.75f &&
              Red.A == 1.0f && Green.A == 1.0f && Blue.A == 1.0f,
            "splitting a packed texture keeps each channel whole", "");

        ImagePixel Specular, SplitGloss;
        GameImageProcessors::SplitSpecularGloss(Nog, Specular, SplitGloss);
        Check(Specular.R == 0.25f && Specular.G == 0.6f && Specular.B == 0.75f && Specular.A == 1.0f &&
              SplitGloss.R == 0.4f && SplitGloss.B == 0.4f,
            "specular keeps its colour and gloss comes out of alpha", "");
    }

    // Bo3Target image unpacking. Expected bytes are worked out by hand from the digest formulas
    // (docs/hydrax-port/gameimageutil.md), with a tolerance of one for rounding.
    {
        const auto Make = [](uint32_t W, uint32_t H, std::initializer_list<uint8_t> Bytes)
        {
            Rgba8Image Image;
            Image.Width = W;
            Image.Height = H;
            Image.Pixels.assign(Bytes.begin(), Bytes.end());
            return Image;
        };
        const auto Near = [](const Rgba8Image& Image, size_t Pixel, int R, int G, int B, int A)
        {
            if (!Image.IsValid() || Image.Pixels.size() < (Pixel + 1) * 4)
            {
                return false;
            }
            const int Want[4] = { R, G, B, A };
            for (int C = 0; C < 4; C++)
            {
                if (std::abs((int)Image.Pixels[Pixel * 4 + C] - Want[C]) > 1)
                {
                    return false;
                }
            }
            return true;
        };
        const auto Show = [](const Rgba8Image& Image, size_t Pixel)
        {
            if (Image.Pixels.size() < (Pixel + 1) * 4)
            {
                return std::string("(empty)");
            }
            return std::format("{} {} {} {}", Image.Pixels[Pixel * 4], Image.Pixels[Pixel * 4 + 1],
                Image.Pixels[Pixel * 4 + 2], Image.Pixels[Pixel * 4 + 3]);
        };

        // XY in RG: 128,128 is about zero tilt so Z is 1 (255). 255,255 overshoots unit length
        // (nZ = -1), so Z is 0, stored as 0.5 (128). GameImageUtil stores 0.0 here; that measured
        // worse against Treyarch's stock sources
        const auto RG = Bo3Target::UnpackNormalRedGreen(Make(2, 1, { 128, 128, 7, 9,  255, 255, 7, 9 }));
        Check(Near(RG.Normal, 0, 128, 128, 255, 255), "Bo3 normal RG straight up", Show(RG.Normal, 0));
        Check(Near(RG.Normal, 1, 255, 255, 128, 255), "Bo3 normal RG Z = 0 (stored 0.5) when nZ <= 0", Show(RG.Normal, 1));

        // XY in AG: X from alpha, Y from green, red ignored. A=128,G=128 is straight up.
        // A=255,G=128 is X=1, Y~0, so nZ is not above zero and Z is 0 (stored 0.5)
        const auto AG = Bo3Target::UnpackNormalAlphaGreen(Make(2, 1, { 10, 128, 0, 128,  10, 128, 0, 255 }));
        Check(Near(AG.Normal, 0, 128, 128, 255, 255), "Bo3 normal AG straight up", Show(AG.Normal, 0));
        Check(Near(AG.Normal, 1, 255, 128, 128, 255), "Bo3 normal AG X comes from alpha, Z = 0 (stored 0.5) at nZ <= 0", Show(AG.Normal, 1));

        // NOG: R gloss 64, B occlusion 192. u=1,v=-1 (G=255,A=0): x=0,y=1,z=0 -> normal (0,1,0).
        // u=1,v=1 (G=255,A=255): x=1,y=0,z=0 -> (1,0,0). G=A=128 is about zero: straight up
        const auto Nog = Bo3Target::UnpackNog(Make(3, 1, { 64, 255, 192, 0,  64, 255, 192, 255,  64, 128, 192, 128 }));
        Check(Near(Nog.Normal, 0, 128, 255, 128, 255), "Bo3 NOG decodes u=1 v=-1 to +Y", Show(Nog.Normal, 0));
        Check(Near(Nog.Normal, 1, 255, 128, 128, 255), "Bo3 NOG decodes u=1 v=1 to +X", Show(Nog.Normal, 1));
        Check(Near(Nog.Normal, 2, 128, 128, 255, 255), "Bo3 NOG decodes the centre to straight up", Show(Nog.Normal, 2));
        Check(Near(Nog.Gloss, 0, 64, 64, 64, 255), "Bo3 NOG gloss is R as grey", Show(Nog.Gloss, 0));
        Check(Near(Nog.Occlusion, 0, 192, 192, 192, 255), "Bo3 NOG occlusion is B as grey", Show(Nog.Occlusion, 0));

        // The zero-length guard. No hemi-octahedral pixel reaches it (z is 1 where x and y are 0),
        // so it is tested on the helper. GameImageUtil would give NaN here
        float GX = 0.0f, GY = 0.0f, GZ = 0.0f;
        Bo3Target::NormaliseOrUp(GX, GY, GZ);
        Check(GX == 0.0f && GY == 0.0f && GZ == 1.0f, "Bo3 zero normal guard emits straight up",
            std::format("{:f} {:f} {:f}", GX, GY, GZ));
        float NX = 3.0f, NY = 0.0f, NZ = 4.0f;
        Bo3Target::NormaliseOrUp(NX, NY, NZ);
        Check(std::fabs(NX - 0.6f) < 1e-6f && std::fabs(NZ - 0.8f) < 1e-6f, "Bo3 normalise keeps a real direction",
            std::format("{:f} {:f}", NX, NZ));

        // Fused colour, metal in alpha. (200,100,0,A=255): m=1, d=0 -> colour 0,0,0 with A 1;
        // r=0.1, spec = (0.1+200/255, 0.1+100/255, 0.1) = 226, 125, 26: a fully metal pixel gets no dielectric floor (it would rewrite the derived value).
        // (200,100,50,A=0): m=0, d=1 -> colour unchanged with A forced to 255; r=0 and m=0 so
        // every spec channel is the BO3 non-metal level 56/255 (tested on stock) = 56
        const auto Fused = Bo3Target::UnpackFusedColourMetalAlpha(Make(2, 1, { 200, 100, 0, 255,  200, 100, 50, 0 }));
        Check(Near(Fused.Colour, 0, 0, 0, 0, 255), "Bo3 fused metal alpha: full metal has no diffuse", Show(Fused.Colour, 0));
        Check(Near(Fused.Specular, 0, 226, 125, 26, 255), "Bo3 fused metal alpha: 0.1 range, no floor on a full metal pixel", Show(Fused.Specular, 0));
        Check(Near(Fused.Colour, 1, 200, 100, 50, 255), "Bo3 fused metal alpha: no metal keeps colour, alpha 1", Show(Fused.Colour, 1));
        Check(Near(Fused.Specular, 1, 56, 56, 56, 255), "Bo3 fused metal alpha: no metal spec is the 56/255 non-metal level", Show(Fused.Specular, 1));

        // Fused colour with a separate mask. Colour is 2x2, all (200,100,50,A=77); the mask is one
        // pixel, so it is resized to 2x2. Mask R=255,A=0: m=1,d=0,r=0 -> colour 0,0,0 with A 77 kept,
        // spec = X = 200,100,50 (no floor over metal). Mask R=0,A=255: m=0,d=1,r=0.1 -> colour unchanged,
        // spec = max(0.1, 56/255) = 56 everywhere
        const auto Colour = Make(2, 2, { 200, 100, 50, 77,  200, 100, 50, 77,  200, 100, 50, 77,  200, 100, 50, 77 });
        const auto Metal = Bo3Target::UnpackFusedColourMetalMask(Colour, Make(1, 1, { 255, 0, 0, 0 }));
        Check(Metal.Colour.Width == 2 && Metal.Colour.Height == 2 && Metal.Specular.IsValid(),
            "Bo3 fused mask: output takes the colour image size", std::format("{}x{}", Metal.Colour.Width, Metal.Colour.Height));
        Check(Near(Metal.Colour, 3, 0, 0, 0, 77), "Bo3 fused mask: colour keeps its own alpha", Show(Metal.Colour, 3));
        Check(Near(Metal.Specular, 3, 200, 100, 50, 255), "Bo3 fused mask: spec from the resized mask", Show(Metal.Specular, 3));
        const auto Plain = Bo3Target::UnpackFusedColourMetalMask(Colour, Make(1, 1, { 0, 0, 0, 255 }));
        Check(Near(Plain.Colour, 0, 200, 100, 50, 77), "Bo3 fused mask: no metal leaves colour", Show(Plain.Colour, 0));
        Check(Near(Plain.Specular, 0, 56, 56, 56, 255), "Bo3 fused mask: insulator spec is the 56/255 non-metal level", Show(Plain.Specular, 0));
        Check(!Bo3Target::UnpackFusedColourMetalMask(Colour, Rgba8Image()).Colour.IsValid(),
            "Bo3 fused mask: an empty mask gives nothing", "");

        // the dielectric level stands in for the non-metal share only: mask R=128 gives M=0.447, D=0.553, so a black colour's
        // specular is 56 * 0.553 = 31 (it is 56 at M=0 and absent at M=1)
        const auto Half = Bo3Target::UnpackFusedColourMetalMask(Make(1, 1, { 0, 0, 0, 255 }), Make(1, 1, { 128, 0, 0, 0 }));
        Check(Near(Half.Specular, 0, 31, 31, 31, 255), "Bo3 fused mask: the non-metal level is scaled by the non-metal share", Show(Half.Specular, 0));

        // Channel extraction: (10,20,30,40), blue is 30, inverted is 225, alpha is 40
        const auto Source = Make(1, 1, { 10, 20, 30, 40 });
        Check(Near(Bo3Target::ExtractChannel(Source, ImageChannel::Blue, false), 0, 30, 30, 30, 255), "Bo3 extract blue", "");
        Check(Near(Bo3Target::ExtractChannel(Source, ImageChannel::Blue, true), 0, 225, 225, 225, 255), "Bo3 extract blue inverted", "");
        Check(Near(Bo3Target::ExtractChannel(Source, ImageChannel::Alpha, false), 0, 40, 40, 40, 255), "Bo3 extract alpha", "");

        // Specular RGB and gloss A
        const auto SG = Bo3Target::UnpackSpecularGloss(Source);
        Check(Near(SG.Specular, 0, 10, 20, 30, 255), "Bo3 specular keeps RGB, alpha 1", Show(SG.Specular, 0));
        Check(Near(SG.Gloss, 0, 40, 40, 40, 255), "Bo3 gloss is alpha as grey", Show(SG.Gloss, 0));
    }

    // Module 2a: the material GDT converts images (MaterialGdt, Bo3Target.MaterialImages). Pack kinds come
    // from the adapters' descriptions; a tiny NOG PNG goes through loader, unpack and writer in a
    // scratch folder; slot names follow the slot; a Treyarch description gives the text it always did.
    {
        namespace fs = std::filesystem;
        const auto Make = [](uint32_t W, uint32_t H, std::initializer_list<uint8_t> Bytes)
        {
            Rgba8Image Image;
            Image.Width = W;
            Image.Height = H;
            Image.Pixels.assign(Bytes.begin(), Bytes.end());
            return Image;
        };
        const auto Px = [](const Rgba8Image& Image, size_t Pixel, int R, int G, int B, int A)
        {
            if (!Image.IsValid() || Image.Pixels.size() < (Pixel + 1) * 4) return false;
            const int Want[4] = { R, G, B, A };
            for (int C = 0; C < 4; C++)
                if (std::abs((int)Image.Pixels[Pixel * 4 + C] - Want[C]) > 1) return false;
            return true;
        };
        const auto Has = [](const std::string& Text, const std::string& Part) { return Text.find(Part) != std::string::npos; };
        const auto Count = [](const std::map<std::string, size_t>& Counters, const std::string& Key)
        {
            const auto It = Counters.find("t_" + Key);
            return It == Counters.end() ? (size_t)0 : It->second;
        };

        // 1. the pack kind from a description (data the adapters state, not file names)
        {
            using Materials::PackKind;
            Materials::Source Bo6;
            Bo6.Name = "bo6_mat";
            Bo6.Images = { { "c.png", Materials::Role::Unknown, 82 }, { "nog.png", Materials::Role::Unknown, 83 } };
            const auto D6 = Materials::AdapterFor("BO6").Describe(Bo6);
            Check(D6.Normal.Pack == PackKind::Nog && D6.Roughness.Pack == PackKind::Nog && D6.Occlusion.Pack == PackKind::Nog,
                "Bo3 pack: BO6 nog maps are marked Nog", std::format("{} {} {}", (int)D6.Normal.Pack, (int)D6.Roughness.Pack, (int)D6.Occlusion.Pack));
            Check(D6.Albedo.Pack == PackKind::None && D6.Occlusion.Channel == 2 && D6.Roughness.Invert,
                "Bo3 pack: BO6 colour is plain, occlusion is channel 2, gloss is a gloss", "");

            Materials::Source Bo7;
            Bo7.Name = "bo7_mat";
            Bo7.Images = { { "c.png", Materials::Role::Unknown, 0x47 }, { "nog.png", Materials::Role::Unknown, 0x48 } };
            const auto D7 = Materials::AdapterFor("BO7").Describe(Bo7);
            Check(D7.Normal.Pack == PackKind::Nog && D7.Occlusion.Pack == PackKind::Nog, "Bo3 pack: BO7 nog maps are marked Nog",
                std::format("{} {}", (int)D7.Normal.Pack, (int)D7.Occlusion.Pack));

            Materials::Source Ww2;
            Ww2.Name = "ww2_mat";
            Ww2.Images = { { "c.png", Materials::Role::Unknown, 0xA0AB1041 }, { "m.png", Materials::Role::Unknown, 0x34ECCCB3 } };
            const auto Dw = Materials::AdapterFor("S2").Describe(Ww2);
            Check(Dw.Metalness.Pack == PackKind::ColorMetalMask && Dw.Albedo.Pack == PackKind::None && Dw.Roughness.Pack == PackKind::None && Dw.Roughness.Channel == 2,
                "Bo3 pack: WWII metal image is a colour metal mask, its gloss a plain channel",
                std::format("{} {} {}", (int)Dw.Metalness.Pack, (int)Dw.Albedo.Pack, (int)Dw.Roughness.Pack));

            // a game that states nothing stays None, among them IW7 (its export patches the images)
            Materials::Source Iw7;
            Iw7.Name = "iw7_mat";
            Iw7.Images = { { "x_packed_cs.png", Materials::Role::Unknown, 0xA0AB1041 }, { "y_packed_nog.png", Materials::Role::Unknown, 0x59D30D0F } };
            const auto Di = Materials::AdapterFor("IW7").Describe(Iw7);
            Check(Di.Normal.Pack == PackKind::None && Di.Albedo.Pack == PackKind::None && Di.Metalness.Pack == PackKind::None,
                "Bo3 pack: IW7 states no pack", std::format("{} {} {}", (int)Di.Normal.Pack, (int)Di.Albedo.Pack, (int)Di.Metalness.Pack));
            Materials::Source T8;
            T8.Name = "t8_mat";
            T8.Images = { { "c.png", Materials::Role::Unknown, 0xA0AB1041 }, { "n.png", Materials::Role::Unknown, 0x59D30D0F } };
            const auto Dt = Materials::AdapterFor("T8").Describe(T8);
            Check(Dt.Normal.Pack == PackKind::None, "Bo3 pack: Treyarch states no pack", std::format("{}", (int)Dt.Normal.Pack));

            // the sidecar keeps the kind, and a file from before it loads as None
            const fs::path Scratch = fs::temp_directory_path() / std::format("graywolf_selftest_pack_{}", (unsigned long long)GetCurrentProcessId());
            std::error_code Ec;
            fs::create_directories(Scratch, Ec);
            Materials::Sidecar Side;
            Side.Game = "BO6";
            Side.Adapter = "bo6";
            Side.Materials.push_back(D6);
            const std::string SidePath = (Scratch / "m.materials.json").string();
            Materials::Sidecar Back;
            const bool Wrote = Materials::WriteSidecar(SidePath, Side), Read = Materials::ReadSidecar(SidePath, Back);
            Check(Wrote && Read && Back.Materials.size() == 1 && Back.Materials[0].Normal.Pack == PackKind::Nog &&
                  Back.Materials[0].Occlusion.Pack == PackKind::Nog && Back.Materials[0].Albedo.Pack == PackKind::None,
                "Bo3 pack: the sidecar keeps the pack kind", std::format("{} {}", Wrote, Read));
            {
                std::ofstream Old(Scratch / "old.materials.json", std::ios::binary);
                Old << R"({"format":"greyhound-materials","version":1,"game":"BO6","adapter":"bo6","materials":[{"name":"a","alpha":"opaque","normal":{"path":"n.png","encoding":"ag"},"occlusion":{"path":"n.png","channel":"b"}}]})";
            }
            Materials::Sidecar OldBack;
            const bool OldRead = Materials::ReadSidecar((Scratch / "old.materials.json").string(), OldBack);
            Check(OldRead && OldBack.Materials.size() == 1 && OldBack.Materials[0].Normal.Pack == PackKind::None &&
                  OldBack.Materials[0].Normal.Path == "n.png" && OldBack.Materials[0].Occlusion.Channel == 2,
                "Bo3 pack: a sidecar from before the pack field loads as None", std::format("{}", OldRead));
            fs::remove_all(Scratch, Ec);
        }

        // 2. the loader reads every format the exports emit (PNG, TIFF, TGA, DDS), and the writer round trips
        const fs::path Dir = fs::temp_directory_path() / std::format("graywolf_selftest_gdt_{}", (unsigned long long)GetCurrentProcessId());
        std::error_code Ec;
        fs::remove_all(Dir, Ec);
        fs::create_directories(Dir, Ec);
        {
            const Rgba8Image Source = Make(2, 2, { 10, 20, 30, 255,  200, 100, 50, 255,  0, 255, 128, 255,  64, 64, 64, 255 });
            for (const char* Name : { "fmt.png", "fmt.tif", "fmt.tga" })
            {
                std::string Error;
                Rgba8Image Back;
                const bool Saved = Bo3Target::SaveRgba8(Dir / Name, Source, Error);
                const bool Loaded = Saved && Bo3Target::LoadRgba8(Dir / Name, Back, Error);
                Check(Loaded && Back.Width == 2 && Back.Height == 2 && Px(Back, 1, 200, 100, 50, 255) && Px(Back, 2, 0, 255, 128, 255),
                    std::format("Bo3 image IO: {} round trip", Name).c_str(), Error);
            }
            // DDS: uncompressed RGBA8 and BC1 (solid colour, so the block error is small)
            DirectX::ScratchImage Dds;
            Dds.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1);
            for (size_t P = 0; P < 16; P++)
            {
                const DirectX::Image* Whole = Dds.GetImage(0, 0, 0);
                const std::span<uint8_t> Pix = std::span<uint8_t>(Whole->pixels, Whole->slicePitch).subspan(P * 4, 4);
                Pix[0] = 90; Pix[1] = 150; Pix[2] = 200; Pix[3] = 255;
            }
            const bool Raw = SUCCEEDED(DirectX::SaveToDDSFile(*Dds.GetImage(0, 0, 0), DirectX::DDS_FLAGS_NONE, (Dir / "raw.dds").c_str()));
            DirectX::ScratchImage Bc1;
            const bool Compressed = SUCCEEDED(DirectX::Compress(*Dds.GetImage(0, 0, 0), DXGI_FORMAT_BC1_UNORM, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, Bc1)) &&
                                    SUCCEEDED(DirectX::SaveToDDSFile(*Bc1.GetImage(0, 0, 0), DirectX::DDS_FLAGS_NONE, (Dir / "bc1.dds").c_str()));
            Check(Raw && Compressed, "Bo3 image IO: test DDS files written", std::format("{} {}", Raw, Compressed));
            std::string Error;
            Rgba8Image RawBack, BcBack;
            Check(Bo3Target::LoadRgba8(Dir / "raw.dds", RawBack, Error) && Px(RawBack, 5, 90, 150, 200, 255), "Bo3 image IO: DDS RGBA8 loads", Error);
            const bool BcOk = Bo3Target::LoadRgba8(Dir / "bc1.dds", BcBack, Error);
            const auto Near8 = [&](int A, int B) { return std::abs(A - B) <= 8; };
            Check(BcOk && BcBack.IsValid() && Near8(BcBack.Pixels[0], 90) && Near8(BcBack.Pixels[1], 150) && Near8(BcBack.Pixels[2], 200),
                "Bo3 image IO: DDS BC1 decodes", Error);
            Rgba8Image Missing;
            Check(!Bo3Target::LoadRgba8(Dir / "nothing.png", Missing, Error) && Error == "load_failed", "Bo3 image IO: a missing file fails with a reason", Error);
        }

        // 3. one NOG PNG through loader, unpack and writer into BO3 images named by their slot; a shared map unpacks once
        {
            // pixel 0: gloss 64, u=1 v=-1 -> normal +Y, occlusion 192; pixel 1: gloss 64, centre -> straight up
            std::string Error;
            Bo3Target::SaveRgba8(Dir / "m_nog.png", Make(2, 1, { 64, 255, 192, 0,  64, 128, 192, 128 }), Error);
            Bo3Target::SaveRgba8(Dir / "m_c.png", Make(1, 1, { 200, 100, 50, 255 }), Error);
            Materials::Description D;
            D.Name = "Test Mat";
            D.Albedo = { "m_c.png", 0, false };
            D.Normal = { "m_nog.png", 0, false, Materials::PackKind::Nog };
            D.NormalEnc = Materials::NormalEncoding::AG;
            D.Roughness = { "m_nog.png", 0, true, Materials::PackKind::Nog };
            D.Occlusion = { "m_nog.png", 2, false, Materials::PackKind::Nog };
            const auto BaseImage = [](const fs::path& File) { return "model_export/t/" + File.filename().string(); };

            MaterialGdt::Builder On(BaseImage, "t_");
            On.ConvertImages(true);
            On.SetRunRoot(Dir);
            On.Add(D, Dir);
            Materials::Description Second = D;
            Second.Name = "Other Mat";
            On.Add(Second, Dir);
            const auto& C = On.Counters();
            Check(Count(C, "converted_normal_nog") == 2 && Count(C, "converted_channel_cosinePowerMap") == 2 && Count(C, "converted_channel_occMap") == 2,
                "Bo3 gdt: each conversion is counted per slot", std::format("{} {} {}", Count(C, "converted_normal_nog"), Count(C, "converted_channel_cosinePowerMap"), Count(C, "converted_channel_occMap")));
            Check(Count(C, "images_derived") == 3, "Bo3 gdt: a map shared by two materials unpacks once (3 files)", std::format("{}", Count(C, "images_derived")));
            Check(Count(C, "normal_ag") == 0 && Count(C, "occMap_channel_dropped") == 0 && Count(C, "roughness_not_gloss") == 0,
                "Bo3 gdt: nothing is counted as unconverted once converted", std::format("{} {}", Count(C, "normal_ag"), Count(C, "occMap_channel_dropped")));
            Rgba8Image N, G, O;
            Check(Bo3Target::LoadRgba8(Dir / "m_nog_n.png", N, Error) && Px(N, 0, 128, 255, 128, 255) && Px(N, 1, 128, 128, 255, 255),
                "Bo3 gdt: the derived normal is RGB with Z in blue", Error);
            Check(Bo3Target::LoadRgba8(Dir / "m_nog_g.png", G, Error) && Px(G, 0, 64, 64, 64, 255), "Bo3 gdt: the derived gloss is grey in RGB", Error);
            Check(Bo3Target::LoadRgba8(Dir / "m_nog_o.png", O, Error) && Px(O, 1, 192, 192, 192, 255), "Bo3 gdt: the derived occlusion is grey in RGB", Error);

            const std::vector<std::string> Entries = On.Entries("\n");
            std::string Text;
            for (const auto& E : Entries) Text += E;
            Check(Has(Text, "\"normalMap\" \"i_m_nog_n\"") && Has(Text, "\"cosinePowerMap\" \"i_m_nog_g\"") && Has(Text, "\"occMap\" \"i_m_nog_o\"") &&
                  Has(Text, "\"colorMap\" \"i_m_c\""),
                "Bo3 gdt: derived images are named from the slot (i_<file>_<slot letter>)", Text);
            Check(Has(Text, "\"i_m_nog_n\" ( \"image.gdf\" )") && Has(Text, "\"baseImage\" \"model_export\\\\t\\\\m_nog_n.png\"") &&
                  Has(Text, "\"semantic\" \"occlusionMap\"") && Has(Text, "\"semantic\" \"glossMap\""),
                "Bo3 gdt: derived image entries point at the derived files", Text);
            size_t Entities = 0;
            for (size_t At = Text.find("\"i_m_nog_n\" ( \"image.gdf\" )"); At != std::string::npos; At = Text.find("\"i_m_nog_n\" ( \"image.gdf\" )", At + 1)) Entities++;
            Check(Entities == 1, "Bo3 gdt: the shared derived image has one entity", std::format("{}", Entities));

            // the same description with conversion off: counted as before, no file written
            MaterialGdt::Builder Off(BaseImage, "t_");
            Off.Add(D, Dir);
            Check(Count(Off.Counters(), "normal_ag") == 1 && Count(Off.Counters(), "occMap_channel_dropped") == 1 && Count(Off.Counters(), "images_derived") == 0,
                "Bo3 gdt: conversion off counts only", std::format("{} {}", Count(Off.Counters(), "normal_ag"), Count(Off.Counters(), "occMap_channel_dropped")));

            // a source that cannot be read is counted as not converted, with the old slot kept
            {
                std::ofstream Bad(Dir / "bad_nog.png", std::ios::binary);
                Bad << "not an image";
            }
            Materials::Description DBad = D;
            DBad.Name = "Bad Mat";
            DBad.Normal.Path = DBad.Roughness.Path = DBad.Occlusion.Path = "bad_nog.png";
            MaterialGdt::Builder Fail(BaseImage, "t_");
            Fail.ConvertImages(true);
            Fail.SetRunRoot(Dir);
            Fail.Add(DBad, Dir);
            Check(Count(Fail.Counters(), "not_converted_normal_load_failed") == 1 && Count(Fail.Counters(), "not_converted_occlusion_load_failed") == 1 &&
                  Count(Fail.Counters(), "not_converted_gloss_load_failed") == 1 && Count(Fail.Counters(), "normal_ag") == 1,
                "Bo3 gdt: an unreadable source is counted not_converted with a reason", std::format("{}", Count(Fail.Counters(), "not_converted_normal_load_failed")));
        }

        // 4. the other conversions: roughness inverted, green flipped, a channel taken out, colour fused with metal
        {
            std::string Error;
            Bo3Target::SaveRgba8(Dir / "r_rough.png", Make(1, 1, { 40, 0, 0, 255 }), Error);
            Bo3Target::SaveRgba8(Dir / "f_xyz.png", Make(1, 1, { 100, 60, 200, 255 }), Error);
            Bo3Target::SaveRgba8(Dir / "s_pack.png", Make(1, 1, { 0, 0, 90, 255 }), Error);
            Bo3Target::SaveRgba8(Dir / "w_c.png", Make(1, 1, { 200, 100, 50, 255 }), Error);
            Bo3Target::SaveRgba8(Dir / "w_cs.png", Make(1, 1, { 200, 100, 50, 255 }), Error);
            Bo3Target::SaveRgba8(Dir / "w_m.png", Make(1, 1, { 255, 0, 0, 0 }), Error);
            const auto BaseImage = [](const fs::path& File) { return "model_export/t/" + File.filename().string(); };

            Materials::Description D;
            D.Name = "Convert Mat";
            D.Albedo = { "w_c.png", 0, false };
            D.Normal = { "f_xyz.png", 0, false };
            D.NormalEnc = Materials::NormalEncoding::XYZ;
            D.NormalFlipY = true;
            D.Roughness = { "r_rough.png", 0, false };   // a roughness map
            D.Occlusion = { "s_pack.png", 2, false };    // blue, no pack: still a channel
            MaterialGdt::Builder B(BaseImage, "t_");
            B.ConvertImages(true);
            B.SetRunRoot(Dir);
            B.Add(D, Dir);
            const auto& C = B.Counters();
            Check(Count(C, "converted_roughness_to_gloss") == 1 && Count(C, "roughness_not_gloss") == 0, "Bo3 gdt: a roughness map becomes a gloss image",
                std::format("{}", Count(C, "converted_roughness_to_gloss")));
            Check(Count(C, "converted_normal_flip_y") == 1 && Count(C, "normal_flip_y") == 0, "Bo3 gdt: NormalFlipY is converted", std::format("{}", Count(C, "converted_normal_flip_y")));
            Check(Count(C, "converted_channel_occMap") == 1 && Count(C, "occMap_channel_dropped") == 0, "Bo3 gdt: a blue channel is taken out, not dropped", "");
            Rgba8Image Gloss, Flip, Occ;
            Check(Bo3Target::LoadRgba8(Dir / "r_rough_g.png", Gloss, Error) && Px(Gloss, 0, 215, 215, 215, 255), "Bo3 gdt: roughness 40 is gloss 215", Error);
            Check(Bo3Target::LoadRgba8(Dir / "f_xyz_n.png", Flip, Error) && Px(Flip, 0, 100, 195, 200, 255), "Bo3 gdt: flip inverts green only", Error);
            Check(Bo3Target::LoadRgba8(Dir / "s_pack_o.png", Occ, Error) && Px(Occ, 0, 90, 90, 90, 255), "Bo3 gdt: the blue channel as grey", Error);

            // colour with the metal in alpha (IW7-style pack kind), and colour with a separate metal mask (WWII)
            Materials::Description Fa;
            Fa.Name = "Fused Alpha";
            Fa.Albedo = { "w_cs.png", 0, false, Materials::PackKind::ColorMetalAlpha };
            Fa.Metalness = { "w_cs.png", 3, false, Materials::PackKind::ColorMetalAlpha };
            Materials::Description Fm;
            Fm.Name = "Fused Mask";
            Fm.Albedo = { "w_c.png", 0, false };
            Fm.Metalness = { "w_m.png", 0, false, Materials::PackKind::ColorMetalMask };
            Materials::Description Plain = Fa;
            Plain.Name = "Unmarked";
            Plain.Albedo.Pack = Plain.Metalness.Pack = Materials::PackKind::None;
            MaterialGdt::Builder F(BaseImage, "t_");
            F.ConvertImages(true);
            F.SetRunRoot(Dir);
            F.Add(Fa, Dir);
            F.Add(Fm, Dir);
            F.Add(Plain, Dir);
            Check(Count(F.Counters(), "converted_metal_fused_alpha") == 1 && Count(F.Counters(), "converted_metal_fused_mask") == 1 &&
                  Count(F.Counters(), "metalness_dropped") == 1 && Count(F.Counters(), "type_lit_advanced_fullspec") == 2,
                "Bo3 gdt: fused colour and metal become colour plus specular; an unmarked metalness is still counted dropped",
                std::format("{} {} {}", Count(F.Counters(), "converted_metal_fused_alpha"), Count(F.Counters(), "converted_metal_fused_mask"), Count(F.Counters(), "metalness_dropped")));
            // the mask material: M=(1-0.1)/0.9=1 so the colour goes black; specular is the colour itself, no floor over metal (50 stays 50)
            Rgba8Image FmC, FmS;
            Check(Bo3Target::LoadRgba8(Dir / "w_c_c.png", FmC, Error) && Px(FmC, 0, 0, 0, 0, 255) && Bo3Target::LoadRgba8(Dir / "w_c_s.png", FmS, Error) && Px(FmS, 0, 200, 100, 50, 255),
                "Bo3 gdt: the fused mask split gives black colour and metal specular", Error);
        }

        // 5. an unchanged Treyarch description gives the same GDT text with conversion on or off (golden)
        {
            for (const char* Name : { "wall_c.png", "wall_n.png", "wall_g.png", "wall_o.png" })
            {
                std::ofstream F(Dir / Name, std::ios::binary);
                F << "x";
            }
            Materials::Description D;
            D.Name = "Mtl_Test/Wall";
            D.Albedo = { "wall_c.png", 0, false };
            D.Normal = { "wall_n.png", 0, false };
            D.Roughness = { "wall_g.png", 0, true };
            D.Occlusion = { "wall_o.png", 0, false };
            const auto BaseImage = [](const fs::path& File) { return "model_export/t/" + File.filename().string(); };
            const std::vector<std::string> Golden = {
                "{",
                "\t\"mtl_test_wall\" ( \"material.gdf\" )",
                "\t{",
                "\t\t\"colorMap\" \"i_wall_c\"",
                "\t\t\"cosinePowerMap\" \"i_wall_g\"",
                "\t\t\"glossSurfaceType\" \"<full>\"",
                "\t\t\"materialCategory\" \"Geometry\"",
                "\t\t\"materialType\" \"lit\"",
                "\t\t\"normalMap\" \"i_wall_n\"",
                "\t\t\"occMap\" \"i_wall_o\"",
                "\t\t\"surfaceType\" \"<none>\"",
                "\t\t\"template\" \"material.template\"",
                "\t\t\"usage\" \"<not in editor>\"",
                "\t}",
                "\t\"i_wall_c\" ( \"image.gdf\" )",
                "\t{",
                "\t\t\"baseImage\" \"model_export\\\\t\\\\wall_c.png\"",
                "\t\t\"compressionMethod\" \"compressed no alpha\"",
                "\t\t\"coreSemantic\" \"sRGB3chAlpha\"",
                "\t\t\"imageType\" \"Texture\"",
                "\t\t\"semantic\" \"diffuseMap\"",
                "\t\t\"streamable\" \"1\"",
                "\t\t\"type\" \"image\"",
                "\t}",
                "\t\"i_wall_g\" ( \"image.gdf\" )",
                "\t{",
                "\t\t\"baseImage\" \"model_export\\\\t\\\\wall_g.png\"",
                "\t\t\"compressionMethod\" \"compressed\"",
                "\t\t\"coreSemantic\" \"Linear1ch\"",
                "\t\t\"imageType\" \"Texture\"",
                "\t\t\"semantic\" \"glossMap\"",
                "\t\t\"streamable\" \"1\"",
                "\t\t\"type\" \"image\"",
                "\t}",
                "\t\"i_wall_n\" ( \"image.gdf\" )",
                "\t{",
                "\t\t\"baseImage\" \"model_export\\\\t\\\\wall_n.png\"",
                "\t\t\"compressionMethod\" \"compressed\"",
                "\t\t\"coreSemantic\" \"Normal\"",
                "\t\t\"imageType\" \"Texture\"",
                "\t\t\"semantic\" \"normalMap\"",
                "\t\t\"streamable\" \"1\"",
                "\t\t\"type\" \"image\"",
                "\t}",
                "\t\"i_wall_o\" ( \"image.gdf\" )",
                "\t{",
                "\t\t\"baseImage\" \"model_export\\\\t\\\\wall_o.png\"",
                "\t\t\"compressionMethod\" \"compressed\"",
                "\t\t\"coreSemantic\" \"Linear1ch\"",
                "\t\t\"imageType\" \"Texture\"",
                "\t\t\"semantic\" \"occlusionMap\"",
                "\t\t\"streamable\" \"1\"",
                "\t\t\"type\" \"image\"",
                "\t}",
                "}",
                "" };
            std::string Want;
            for (const auto& Line : Golden) Want += Line + (&Line == &Golden.back() ? "" : "\r\n");
            for (const bool Convert : { false, true })
            {
                MaterialGdt::Builder B(BaseImage, "t_");
                B.ConvertImages(Convert);
                B.SetRunRoot(Dir);
                B.Add(D, Dir);
                const std::string Got = Gdt::Document(B.Entries("\r\n"));
                Check(Got == Want, Convert ? "Bo3 gdt golden: Treyarch text unchanged with conversion on" : "Bo3 gdt golden: Treyarch text with conversion off", Got);
                Check(Count(B.Counters(), "normal_xy") == 1 && Count(B.Counters(), "images_derived") == 0 && Count(B.Counters(), "materials") == 1,
                    "Bo3 gdt golden: Treyarch counters and no derived files", std::format("{}", Count(B.Counters(), "images_derived")));
            }
        }
        fs::remove_all(Dir, Ec);
    }

    // Salsa20 against the published eSTREAM vector: Black Ops 2 fast files are encrypted with it.
    {
        // Set 1, vector 0 for a 256 bit key: a single set bit, a zero nonce
        uint8_t Key[32] = {};
        Key[0] = 0x80;
        uint8_t Nonce[8] = {};

        // Encrypting zeroes hands back the raw keystream
        uint8_t Block[64] = {};

        Salsa20Key Cipher;
        Cipher.Key = Key;
        Cipher.KeyLength = 256;
        Cipher.IV = Nonce;

        Encryption::Salsa20Block((int8_t*)Block, sizeof(Block), Cipher);

        const uint8_t Expected[64] = {
            0xE3, 0xBE, 0x8F, 0xDD, 0x8B, 0xEC, 0xA2, 0xE3, 0xEA, 0x8E, 0xF9, 0x47, 0x5B, 0x29, 0xA6, 0xE7,
            0x00, 0x39, 0x51, 0xE1, 0x09, 0x7A, 0x5C, 0x38, 0xD2, 0x3B, 0x7A, 0x5F, 0xAD, 0x9F, 0x68, 0x44,
            0xB2, 0x2C, 0x97, 0x55, 0x9E, 0x27, 0x23, 0xC7, 0xCB, 0xBD, 0x3F, 0xE4, 0xFC, 0x8D, 0x9A, 0x07,
            0x44, 0x65, 0x2A, 0x83, 0xE7, 0x2A, 0x9C, 0x46, 0x18, 0x76, 0xAF, 0x4D, 0x7E, 0xF1, 0xA1, 0x17,
        };

        std::string Got;
        for (size_t i = 0; i < 8; i++)
        {
            Got += std::format("{:02X}", Block[i]);
        }

        Check(std::memcmp(Block, Expected, sizeof(Expected)) == 0,
            "Salsa20 matches the eSTREAM 256 bit vector", Got);

        // A stream cipher is its own inverse, so running it twice restores the input
        uint8_t Message[32];
        for (size_t i = 0; i < sizeof(Message); i++)
        {
            Message[i] = (uint8_t)(i * 7 + 3);
        }

        uint8_t Copy[32];
        std::memcpy(Copy, Message, sizeof(Copy));

        Encryption::Salsa20Block((int8_t*)Copy, sizeof(Copy), Cipher);
        const auto Changed = std::memcmp(Copy, Message, sizeof(Copy)) != 0;
        Encryption::Salsa20Block((int8_t*)Copy, sizeof(Copy), Cipher);

        Check(Changed, "Salsa20 actually alters the buffer", "buffer came back unchanged");
        Check(std::memcmp(Copy, Message, sizeof(Copy)) == 0, "Salsa20 round trips", "did not restore");
    }

    // XMODEL_EXPORT round trip: build a model, write it with the existing writer, read it back
    // with the reader, and compare - which exercises both.
    {
        std::error_code Ec;
        WraithModel Model;

        const char* BoneNames[] = { "tag_origin", "j_mainroot", "j_spine" };
        const int32_t BoneParents[] = { -1, 0, 1 };

        for (int i = 0; i < 3; i++)
        {
            WraithBone Bone;
            Bone.TagName = BoneNames[i];
            Bone.BoneParent = BoneParents[i];
            Bone.GlobalPosition = Vector3((float)i, (float)i * 2.0f, (float)i * 3.0f);
            Bone.BoneScale = Vector3(1.0f, 1.0f, 1.0f);
            Model.Bones.push_back(Bone);
        }

        const auto Path = (std::filesystem::path(FileSystems::GetApplicationPath()) / "selftest_roundtrip.XMODEL_EXPORT").string();

        CodXME::ExportXME(Model, Path);

        XMEModel ReadBack;
        std::string ReadError;
        const auto Ok = CodXMEReader::ReadFile(Path, ReadBack, ReadError);

        Check(Ok, "XMODEL_EXPORT round trip reads", ReadError);

        if (Ok)
        {
            Check(ReadBack.Version == 6, "XMODEL_EXPORT version",
                std::format("{}", ReadBack.Version));

            Check(ReadBack.Bones.size() == 3, "XMODEL_EXPORT bone count",
                std::format("{}", (unsigned long long)ReadBack.Bones.size()));

            if (ReadBack.Bones.size() == 3)
            {
                for (int i = 0; i < 3; i++)
                {
                    Check(ReadBack.Bones[i].Name == BoneNames[i],
                        "XMODEL_EXPORT bone name", ReadBack.Bones[i].Name);

                    Check(ReadBack.Bones[i].Parent == BoneParents[i],
                        "XMODEL_EXPORT bone parent", std::format("{}", ReadBack.Bones[i].Parent));

                    // Positions survive a decimal round trip, so compare with a tolerance
                    const float Expected[3] = { (float)i, (float)i * 2.0f, (float)i * 3.0f };
                    for (int Axis = 0; Axis < 3; Axis++)
                    {
                        Check(std::fabs(ReadBack.Bones[i].Offset[Axis] - Expected[Axis]) < 0.001f,
                            "XMODEL_EXPORT bone offset",
                            std::format("{:f} expected {:f}", ReadBack.Bones[i].Offset[Axis], Expected[Axis]));
                    }
                }
            }
        }

        // Same round trip through the binary form. Only the container is decoded, so this
        // checks the magic and that the block really holds what its header claims, then
        // looks for a bone name in the token stream to be sure it is the model we wrote.
        const auto BinPath = (std::filesystem::path(FileSystems::GetApplicationPath()) / "selftest_roundtrip.XMODEL_BIN").string();

        CodXMB::ExportXMB(Model, BinPath);

        std::vector<uint8_t> Unpacked;
        std::string BinError;
        const auto BinOk = CodXMBReader::ReadFile(BinPath, Unpacked, BinError);

        Check(BinOk, "XMODEL_BIN round trip unpacks", BinError);

        if (BinOk)
        {
            Check(!Unpacked.empty(), "XMODEL_BIN payload is not empty",
                std::format("{}", (unsigned long long)Unpacked.size()));

            const std::string Haystack((const char*)Unpacked.data(), Unpacked.size());
            Check(Haystack.find("j_mainroot") != std::string::npos,
                "XMODEL_BIN carries the bones we wrote", "j_mainroot missing");
        }

        std::filesystem::remove(BinPath, Ec);
        std::filesystem::remove(Path, Ec);
    }

    // The mesh decoder every model reader shares
    Failed += MeshDecode::SelfTest(Failures);

    // Module 4: the gdt.db name rules, match modes and stock rule on synthetic rows
    Failed += Bo3Target::GdtDb::SelfTest(Failures);

    // Module 3: the placement rules, the fixes, name shortening and the game tokens on synthetic rows
    Failed += Bo3Target::Placement::SelfTest(Failures);

    // Module 5: the manifest, the never-overwrite rule, GDT merge, the install guard; then the publish step end to end
    Failed += Bo3Target::Output::SelfTest(Failures);
    Failed += Bo3Target::Techsets::SelfTest(Failures);
    Failed += MaterialGdt::SelfTest(Failures);
    Failed += SurfaceTypes::SelfTest(Failures);
    Failed += ImageRoles::SelfTest(Failures);
    Failed += Bo3Target::Publish::SelfTest(Failures);
    Failed += Bo3Target::Awi::SelfTest(Failures);
    Failed += Bo3Target::Verify::SelfTest(Failures);

    // pool-coverage: the state of a pool on synthetic rows, the pool map file, the run report parsers
    Failed += PoolCoverage::SelfTest(Failures);
    Failed += Maps::FullMapSelfTest(Failures);

    // The Radiant source export's stand-alone rules (hulls, limits, tiny faces, contents, lights, angles, usage)
    Failed += Maps::RadiantRulesSelfTest(Failures);
    Failed += Maps::RadiantWriterSelfTest(Failures);

    return Failed;
}


int RunSelfTest(const CliOptions& Options)
{
    std::string Failures;
    const auto Failed = SelfTest(Failures);

    CliReport(Options, std::format(
        "{}result={}\nself_test_failures={}\n",
        Failures.c_str(),
        (Failed == 0) ? "ok" : "error",
        Failed));

    return (Failed == 0) ? 0 : 4;
}


// Classifies a fast file from its header alone, without decompressing anything - the cheap path for
// a sweep that only needs to know what a directory holds (Decompress runs the whole RSA/AES/LZ4/zlib
// pipeline over the entire file).
int RunIdentify(const CliOptions& Options)
{
    const auto Info = FastFile::Identify(Options.Identify);

    const char* FormatName = "unknown";
    switch (Info.Format)
    {
    case FastFileFormat::InfinityWardZLib:    FormatName = "iw-zlib"; break;
    case FastFileFormat::TreyarchChunked:     FormatName = "treyarch-chunked"; break;
    case FastFileFormat::SledgehammerLZ4:     FormatName = "sledgehammer-lz4"; break;
    case FastFileFormat::TreyarchEncrypted:   FormatName = "treyarch-encrypted"; break;
    case FastFileFormat::InfinityWardBlocked: FormatName = "iw-blocked"; break;
    default: break;
    }

    CliReport(Options, std::format(
        "result={}\nfile={}\nmagic={}\nff_version={}\nformat={}\nencrypted={}\n"
        "declared_zone_size={}\ndata_offset={}\nspliced_table_bytes={}\n",
        Info.Format == FastFileFormat::Unknown ? "error" : "ok",
        Options.Identify.c_str(),
        Info.Magic.c_str(),
        Info.Version,
        FormatName,
        Info.Encrypted ? "yes" : "no",
        (unsigned long long)Info.DeclaredZoneSize,
        (unsigned long long)Info.DataOffset,
        (unsigned long long)Info.SplicedTableBytes));

    return Info.Format == FastFileFormat::Unknown ? 5 : 0;
}


// Unpacks a Fast File into its zone. The zone still needs a per-title parser to reach the assets;
// this only handles the container.
int RunDecompressFF(const CliOptions& Options)
{
    const auto Info = FastFile::Identify(Options.DecompressFF);

    ZoneBytes Zone;
    std::string FailureReason;
    const auto Ok = FastFile::Decompress(Options.DecompressFF, Zone, FailureReason);

    if (Ok && !Options.Out.empty())
    {
        auto Writer = BinaryWriter();
        if (Writer.Create(Options.Out))
        {
            Writer.Write((const int8_t*)Zone.data(), (uint32_t)Zone.size());
        }
        else
        {
            CliReport(Options, std::format(
                "result=error\nreason=could not write {}\n", Options.Out.c_str()));
            return 6;
        }
    }

    // The strings are what identify what a zone holds, and they are reachable without
    // replaying the engine's pointer fixup, so they are the first thing worth having
    std::string StringReport;
    if (Ok && Options.Strings)
    {
        const auto Strings = FastFile::ScanStrings(Zone);
        StringReport = std::format("strings={}\n", (unsigned long long)Strings.size());

        // The Treyarch zones carry a real string table and asset list, so report what
        // parsing them gives alongside what scanning found. Black Ops 2 and Black Ops 3
        // are both Treyarch and share nothing here: one is 32 bit behind its own header
        // and the other 64 bit from the first byte, and even the type ids differ.
        const auto Layout = FastFile::LayoutFor(Info);
        const auto IsT7 = Info.Format == FastFileFormat::TreyarchChunked;
        const auto IsS1 = Info.Format == FastFileFormat::SledgehammerLZ4;

        if (Layout.Known || IsT7 || IsS1)
        {
            ZoneInfo Parsed;
            std::string ParseError;

            // PointerSize is the one thing that separates the 64-bit rebuilds of MW2/MW3
            // from every other Infinity Ward title: same container, same version number,
            // 32-bit zone header, but an asset list of 8-byte padded pointers behind it.
            const auto ParsedOk =
                IsT7 ? FastFile::ParseTreyarchZone(Zone, Parsed, ParseError) :
                IsS1 ? FastFile::ParseSledgehammerZone(Zone, Parsed, ParseError) :
                Layout.PointerSize == 8 ? FastFile::ParseIWZone(Zone, Layout, Parsed, ParseError) :
                       FastFile::Parse32BitZone(Zone, Layout, Parsed, ParseError);

            if (IsS1)
            {
                StringReport += "zone_title=Advanced Warfare\n";
            }
            else if (!IsT7)
            {
                StringReport += std::format("zone_title={}\n", Layout.Title.c_str());
            }

            if (ParsedOk)
            {
                StringReport += std::format(
                    "string_table_count={}\nstring_table_inline={}\nstring_table_offset={}\nstring_table_read={}\n",
                    Parsed.StringCount,
                    Parsed.StringsInline,
                    (unsigned long long)Parsed.StringTableOffset,
                    (unsigned long long)Parsed.Strings.size());

                // How many of the assets carry their header inline says how much of the
                // zone is reachable by reading forwards, and how much needs the blocks
                uint32_t InlineAssets = 0;
                for (const auto& Asset : Parsed.Assets)
                {
                    InlineAssets += Asset.Inline ? 1 : 0;
                }

                // What the zone actually holds, by name - works on every zone whose layout is known.
                // BO3 isn't one of those layouts (64-bit, its own parser) but still has an asset list
                // worth naming, with ids from t7x since OpenAssetTools stops at BO2.
                const auto* const* Names = IsT7 ? ZoneAssetTypes::BlackOps3 : Layout.TypeNames;
                const auto NameCount = IsT7 ? std::size(ZoneAssetTypes::BlackOps3) : Layout.TypeNameCount;

                if (Names != nullptr)
                {
                    std::map<std::string, uint32_t> Counts;

                    for (const auto& Asset : Parsed.Assets)
                    {
                        const auto* TypeName = ZoneAssetTypes::Lookup(Names, NameCount, Asset.Type);

                        Counts[TypeName != nullptr
                            ? TypeName
                            : std::format("type_{}", Asset.Type)]++;
                    }

                    std::string Inventory;
                    for (const auto& Entry : Counts)
                    {
                        if (!Inventory.empty())
                        {
                            Inventory += ",";
                        }
                        Inventory += std::format("{}:{}", Entry.first.c_str(), Entry.second);
                    }

                    StringReport += std::format("asset_types={}\n", Inventory.c_str());
                }

                StringReport += std::format(
                    "asset_count={}\nasset_list_offset={}\nasset_list_read={}\nassets_inline={}\n",
                    Parsed.AssetCount,
                    (unsigned long long)Parsed.AssetListOffset,
                    (unsigned long long)Parsed.Assets.size(),
                    InlineAssets);

                // Then the stream those assets describe, walked by the game's zonekit tables
                ProfileWalk Walk;
                std::string WalkError;

                if (WalkWithProfile(Options.DecompressFF, Zone, Walk, WalkError))
                {
                    // The zone-level assets the walk entered, in list order: the inline entries'
                    // types, and where each one's struct starts
                    std::vector<uint32_t> InlineTypes;
                    for (const auto& Asset : Parsed.Assets)
                    {
                        if (Asset.Inline) InlineTypes.push_back(Asset.Type);
                    }
                    std::vector<const ZoneTables::ZoneResult::Instance*> Entered;
                    for (const auto& I : Walk.R.Instances)
                    {
                        if (I.Parent < 0) Entered.push_back(&I);
                    }

                    StringReport += std::format(
                        "walk_profile={}\nwalk_assets={}\nwalk_complete={}\nwalk_stream_offset={}\nwalk_end_offset={}\nwalk_why={}\n",
                        Walk.Profile,
                        Walk.Walked,
                        Walk.Complete ? "yes" : "no",
                        Entered.empty() ? 0ull : (unsigned long long)Entered.front()->Pos,
                        Entered.empty() ? 0ull : (unsigned long long)Entered.back()->End,
                        Walk.R.Why);

                    // The first inline asset not read whole
                    if (!Walk.Complete)
                    {
                        StringReport += std::format(
                            "walk_stopped_at={}\nwalk_stopped_type={}\n",
                            Walk.Walked, Walk.Walked < InlineTypes.size() ? InlineTypes[Walk.Walked] : 0u);
                    }

                    // Every asset the walk read, by name - "type\tname" per line: the string its
                    // first field points to, a localized string's key its second (the text first).
                    if (!Options.Out.empty() && Walk.Walked > 0)
                    {
                        std::ofstream AssetWriter(Options.Out + ".assets.txt");
                        if (AssetWriter)
                        {
                            const auto* const* Names = IsT7 ? ZoneAssetTypes::BlackOps3 : Layout.TypeNames;
                            const auto NameCount = IsT7 ? std::size(ZoneAssetTypes::BlackOps3) : Layout.TypeNameCount;

                            for (size_t K = 0; K < Walk.Walked && K < InlineTypes.size(); K++)
                            {
                                const uint32_t Type = InlineTypes[K];
                                const auto* TypeName = (Names != nullptr)
                                    ? ZoneAssetTypes::Lookup(Names, NameCount, Type)
                                    : nullptr;
                                const bool Localize = TypeName != nullptr && strcmp(TypeName, "localize_entry") == 0;
                                const std::string Name = Walk.String(Zone, Entered[K]->Pos + (Localize ? Walk.PointerSize : 0));
                                if (Name.empty())
                                {
                                    continue;
                                }

                                // A type whose first field is not its name can point at other
                                // text than a path: every byte of a real name is printable ASCII.
                                const auto IsRealName = std::all_of(
                                    Name.begin(), Name.end(),
                                    [](char C) { return C >= 0x20 && C < 0x7F; });

                                if (!IsRealName)
                                {
                                    continue;
                                }

                                AssetWriter << std::format("{}\t{}",
                                    TypeName != nullptr ? std::string(TypeName) : std::format("type_{}", Type),
                                    Name.c_str()) << '\n';
                            }
                        }
                    }
                }
                else
                {
                    StringReport += std::format("walk_error={}\n", WalkError.c_str());
                }
            }
            else
            {
                StringReport += std::format("string_table_error={}\n", ParseError.c_str());
            }
        }

        if (!Options.Out.empty())
        {
            std::ofstream Writer(Options.Out + ".strings.txt");
            if (Writer)
            {
                for (const auto& Entry : Strings)
                {
                    Writer << std::format("{:08x} {}\n", (unsigned long long)Entry.first, Entry.second.c_str());
                }
            }
        }
    }

    // A failed unpack reports no zone at all. How far it got before giving up is worth
    // knowing, but under its own name: anything reading zone_bytes is asking how much
    // came out, and the answer when this fails is none of it.
    const auto Partial = Ok
        ? std::string()
        : std::format("zone_partial_bytes={}\n", (unsigned long long)Zone.size());

    // The title the module table gives (its install folder or its header deciding between titles
    // that share a version), and the zonekit profile of that row
    const auto* Module = FindGameModule(Info.Magic, Info.Version, Options.DecompressFF);
    const auto ModuleReport = Module == nullptr ? std::string("game=\n")
        : std::format("game={}\ngame_title={}\ngame_profile={}\n", Module->Engine, Module->Title,
                      Module->Profile != nullptr ? Module->Profile : "");

    CliReport(Options, std::format(
        "result={}\nfile={}\nmagic={}\nff_version={}\n{}zone_bytes={}\n{}{}{}{}",
        Ok ? "ok" : "error",
        Options.DecompressFF.c_str(),
        Info.Magic.c_str(),
        Info.Version,
        ModuleReport,
        (unsigned long long)(Ok ? Zone.size() : 0),
        Partial.c_str(),
        StringReport.c_str(),
        Ok ? "" : "reason=",
        Ok ? "" : (FailureReason + "\n").c_str()));

    return Ok ? 0 : 6;
}

// ---- the subcommands (CliCommands.h)

void AddMiscCommands(CliCommandBuilders& B)
{
    B["self-test"] = [](CLI::App& App, CliOptions& Options) {
        App.add_subcommand("self-test", "Run built-in assertions that need no game files or window")
            ->callback([&Options] { Options.SelfTest = true; });
    };
    B["decompress-ff"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("decompress-ff", "Decompress a fast file and report its header/zone info");
        Sub->add_option("file", Options.DecompressFF, "Fast file (.ff) to decompress")->required()->check(CLI::ExistingFile);
        Sub->add_flag("--strings", Options.Strings, "Also dump the zone's string table");
    };
    B["identify"] = [](CLI::App& App, CliOptions& Options) {
        auto* Sub = App.add_subcommand("identify", "Identify a game/asset by name or hash");
        Sub->add_option("name", Options.Identify, "Name or hash to identify")->required();
    };
}
