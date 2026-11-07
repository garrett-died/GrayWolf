#include "stdafx.h"

#include "SkyCubeDds.h"

#include "DirectXTex.h"

#include <format>

namespace SkyConv
{
    bool DecodeCubeDds(std::span<const uint8_t> Dds, Cube& Out, std::string& Format, std::string& Why)
    {
        using namespace DirectX;
        TexMetadata Meta;
        ScratchImage Loaded;
        if (FAILED(LoadFromDDSMemory(Dds.data(), Dds.size(), DDS_FLAGS_NONE, &Meta, Loaded)))
        {
            Why = "the DDS did not load";
            return false;
        }
        Format = std::format("dxgi {} {}x{} faces {} mips {}", static_cast<int>(Meta.format), Meta.width, Meta.height, Meta.arraySize, Meta.mipLevels);
        if (!Meta.IsCubemap() && Meta.arraySize != 6)
        {
            Why = "the image is not a cube map";
            return false;
        }
        if (Meta.width != Meta.height || Meta.width < 2 || Meta.width > kMaxFace)
        {
            Why = std::format("the cube's faces are not square or are over {} (a larger cube would need over 1.2 GB of floats)", kMaxFace);
            return false;
        }
        const bool Bc6 = Meta.format == DXGI_FORMAT_BC6H_UF16 || Meta.format == DXGI_FORMAT_BC6H_SF16 || Meta.format == DXGI_FORMAT_BC6H_TYPELESS;
        const bool Float = Meta.format == DXGI_FORMAT_R16G16B16A16_FLOAT || Meta.format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (!Bc6 && !Float)
        {
            Why = "the cube's format is neither BC6H nor a float format";
            return false;
        }
        Out.Size = static_cast<int>(Meta.width);
        const size_t N = Meta.width * Meta.height;
        for (size_t Face = 0; Face < 6; Face++)
        {
            const Image* Src = Loaded.GetImage(0, Face, 0);
            if (Src == nullptr)
            {
                Why = "a face is missing";
                return false;
            }
            ScratchImage Decoded;
            const Image* Use = Src;
            if (Bc6)
            {
                if (FAILED(Decompress(*Src, DXGI_FORMAT_R32G32B32A32_FLOAT, Decoded)))
                {
                    Why = "BC6H did not decode";
                    return false;
                }
                Use = Decoded.GetImage(0, 0, 0);
            }
            else if (Src->format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                if (FAILED(Convert(*Src, DXGI_FORMAT_R32G32B32A32_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, Decoded)))
                {
                    Why = "the half float face did not convert";
                    return false;
                }
                Use = Decoded.GetImage(0, 0, 0);
            }
            std::vector<float>& Rgb = Out.Rgb[Face];
            Rgb.resize(N * 3);
            for (size_t Y = 0; Y < Meta.height; Y++)
            {
                const std::span<const uint8_t> Bytes(Use->pixels, Use->slicePitch);
                const std::span<const uint8_t> RowBytes = Bytes.subspan(Y * Use->rowPitch, Meta.width * 16);
                const std::span<const float> Row(reinterpret_cast<const float*>(RowBytes.data()), Meta.width * 4);
                for (size_t X = 0; X < Meta.width; X++)
                    for (size_t C = 0; C < 3; C++) Rgb[(Y * Meta.width + X) * 3 + C] = Row[X * 4 + C];
            }
        }
        return true;
    }
}
