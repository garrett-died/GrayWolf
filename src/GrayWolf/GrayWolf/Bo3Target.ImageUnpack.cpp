#include "stdafx.h"

// See Bo3Target.ImageUnpack.h: pure RGBA8 unpacking for the BO3 conversion layer.

#include "Bo3Target.ImageUnpack.h"

#include "GameImageProcessors.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float InsulatorSpecRange = 0.1f; // GameImageUtil's insulatorSpecRange
    // BO3's non-metal specular level: the mode of 200 stock specularMap TIFFs is 56/255 (21.8% of all pixels; 0.21 = 53.55 is not
    // where stock sits). It is made up only for the non-metal share of a pixel (see SplitFused), never over a derived metal value.
    constexpr float SpecularFloor = 56.0f / 255.0f;

    float ToFloat(uint8_t Value)
    {
        return (float)Value / 255.0f;
    }

    uint8_t ToByte(float Value)
    {
        // NaN fails the first comparison, so it is sent to 0 explicitly
        if (!(Value > 0.0f))
        {
            return 0;
        }
        if (Value >= 1.0f)
        {
            return 255;
        }
        return (uint8_t)(Value * 255.0f + 0.5f);
    }

    // GameImageUtil calls MathUtilities.Clamp(v, 1.0, 0.0). PhilLibX was not read, so the
    // argument order is unverified; the intent is clamp to [0, 1], which is what this does.
    float Clamp01(float Value)
    {
        return std::min(std::max(Value, 0.0f), 1.0f);
    }

    ImagePixel ReadPixel(const Rgba8Image& Image, size_t Index)
    {
        const uint8_t* P = &Image.Pixels[Index * 4];
        ImagePixel Pixel;
        Pixel.R = ToFloat(P[0]);
        Pixel.G = ToFloat(P[1]);
        Pixel.B = ToFloat(P[2]);
        Pixel.A = ToFloat(P[3]);
        return Pixel;
    }

    void WritePixel(Rgba8Image& Image, size_t Index, const ImagePixel& Pixel)
    {
        uint8_t* P = &Image.Pixels[Index * 4];
        P[0] = ToByte(Pixel.R);
        P[1] = ToByte(Pixel.G);
        P[2] = ToByte(Pixel.B);
        P[3] = ToByte(Pixel.A);
    }

    Rgba8Image MakeImage(const Rgba8Image& Like)
    {
        Rgba8Image Result;
        Result.Width = Like.Width;
        Result.Height = Like.Height;
        Result.Pixels.assign(Like.Pixels.size(), 0);
        return Result;
    }

    ImagePixel Grey(float Value)
    {
        ImagePixel Pixel;
        Pixel.R = Pixel.G = Pixel.B = Value;
        Pixel.A = 1.0f;
        return Pixel;
    }

    // GameImageUtil's Resize filter is unknown (PhilLibX), so this is plain bilinear with
    // pixel-centre sampling. A constant mask resizes exactly under any filter.
    Rgba8Image ResizeBilinear(const Rgba8Image& Source, uint32_t Width, uint32_t Height)
    {
        Rgba8Image Result;
        Result.Width = Width;
        Result.Height = Height;
        Result.Pixels.assign((size_t)Width * Height * 4, 0);

        for (uint32_t Y = 0; Y < Height; Y++)
        {
            const float SY = std::min(std::max(((float)Y + 0.5f) * Source.Height / Height - 0.5f, 0.0f), (float)(Source.Height - 1));
            const uint32_t Y0 = (uint32_t)SY;
            const uint32_t Y1 = std::min(Y0 + 1, Source.Height - 1);
            const float FY = SY - (float)Y0;

            for (uint32_t X = 0; X < Width; X++)
            {
                const float SX = std::min(std::max(((float)X + 0.5f) * Source.Width / Width - 0.5f, 0.0f), (float)(Source.Width - 1));
                const uint32_t X0 = (uint32_t)SX;
                const uint32_t X1 = std::min(X0 + 1, Source.Width - 1);
                const float FX = SX - (float)X0;

                for (int C = 0; C < 4; C++)
                {
                    const auto At = [&](uint32_t PX, uint32_t PY) { return (float)Source.Pixels[((size_t)PY * Source.Width + PX) * 4 + C]; };
                    const float Top = At(X0, Y0) * (1.0f - FX) + At(X1, Y0) * FX;
                    const float Bottom = At(X0, Y1) * (1.0f - FX) + At(X1, Y1) * FX;
                    Result.Pixels[((size_t)Y * Width + X) * 4 + C] = (uint8_t)(Top * (1.0f - FY) + Bottom * FY + 0.5f);
                }
            }
        }

        return Result;
    }

    // The fused split shared by both colour/specular modes. Metal and insulator come from
    // the mask pixel in the mask variant, from the colour pixel's alpha in the fused one.
    void SplitFused(const ImagePixel& Colour, float Metal, float InsulatorSource, float ColourAlpha, ImagePixel& OutColour, ImagePixel& OutSpecular)
    {
        const float M = Clamp01(Metal - InsulatorSpecRange) * (1.0f / (1.0f - InsulatorSpecRange));
        const float D = Clamp01(1.0f - M);
        const float R = std::min(InsulatorSource, InsulatorSpecRange);

        OutColour.R = Colour.R * D;
        OutColour.G = Colour.G * D;
        OutColour.B = Colour.B * D;
        OutColour.A = ColourAlpha;

        // the dielectric level is no authored value: it stands in only for the share of the pixel that is not metal (1 - M)
        const float Floor = SpecularFloor * D;
        OutSpecular.R = std::max(R + M * Colour.R, Floor);
        OutSpecular.G = std::max(R + M * Colour.G, Floor);
        OutSpecular.B = std::max(R + M * Colour.B, Floor);
        OutSpecular.A = 1.0f;
    }

    UnpackedNormal UnpackNormal(const Rgba8Image& Source, bool XInAlpha)
    {
        UnpackedNormal Result;
        if (!Source.IsValid())
        {
            return Result;
        }

        Result.Normal = MakeImage(Source);
        const size_t Count = (size_t)Source.Width * Source.Height;
        for (size_t i = 0; i < Count; i++)
        {
            // Core's rebuild: Z = 0 (nZ <= 0) is stored as 0.5, alpha 1. GameImageUtil stores 0.0 there; that
            // measured worse against stock (see GameImageProcessors.cpp)
            const auto Pixel = ReadPixel(Source, i);
            WritePixel(Result.Normal, i, XInAlpha ? GameImageProcessors::UnpackNormalFromAlphaGreen(Pixel)
                                                  : GameImageProcessors::UnpackNormalFromRedGreen(Pixel));
        }
        return Result;
    }
}

namespace Bo3Target
{
    UnpackedNormal UnpackNormalRedGreen(const Rgba8Image& Source)
    {
        return UnpackNormal(Source, false);
    }

    UnpackedNormal UnpackNormalAlphaGreen(const Rgba8Image& Source)
    {
        return UnpackNormal(Source, true);
    }

    void NormaliseOrUp(float& X, float& Y, float& Z)
    {
        const float Length = std::sqrt(X * X + Y * Y + Z * Z);

        // GameImageUtil's Vector3.Normalize gives NaN for a zero vector. Straight up is the
        // neutral tangent-space normal, so a bad pixel stays harmless.
        if (!(Length > 0.0f) || !std::isfinite(Length))
        {
            X = 0.0f;
            Y = 0.0f;
            Z = 1.0f;
            return;
        }

        X /= Length;
        Y /= Length;
        Z /= Length;
    }

    UnpackedNog UnpackNog(const Rgba8Image& Source)
    {
        UnpackedNog Result;
        if (!Source.IsValid())
        {
            return Result;
        }

        Result.Normal = MakeImage(Source);
        Result.Gloss = MakeImage(Source);
        Result.Occlusion = MakeImage(Source);

        // Core's UnpackNormalOcclusionGloss has the same math, but turns a zero vector into
        // (0.5,0.5,0.5) instead of straight up, so the normal is decoded here.
        const size_t Count = (size_t)Source.Width * Source.Height;
        for (size_t i = 0; i < Count; i++)
        {
            const auto Pixel = ReadPixel(Source, i);

            const float U = Pixel.G * 2.0f - 1.0f;
            const float V = Pixel.A * 2.0f - 1.0f;
            float X = (U + V) * 0.5f;
            float Y = (U - V) * 0.5f;
            float Z = 1.0f - std::fabs(X) - std::fabs(Y);
            NormaliseOrUp(X, Y, Z);

            ImagePixel Normal;
            Normal.R = X * 0.5f + 0.5f;
            Normal.G = Y * 0.5f + 0.5f;
            Normal.B = Z * 0.5f + 0.5f;
            Normal.A = 1.0f;

            WritePixel(Result.Normal, i, Normal);
            WritePixel(Result.Gloss, i, Grey(Pixel.R));
            WritePixel(Result.Occlusion, i, Grey(Pixel.B));
        }
        return Result;
    }

    UnpackedColourSpecular UnpackFusedColourMetalAlpha(const Rgba8Image& Source)
    {
        UnpackedColourSpecular Result;
        if (!Source.IsValid())
        {
            return Result;
        }

        Result.Colour = MakeImage(Source);
        Result.Specular = MakeImage(Source);

        const size_t Count = (size_t)Source.Width * Source.Height;
        for (size_t i = 0; i < Count; i++)
        {
            const auto Pixel = ReadPixel(Source, i);
            ImagePixel Colour, Specular;
            // Metal and insulator range both come from the colour pixel's alpha; colour alpha is 1
            SplitFused(Pixel, Pixel.A, Pixel.A, 1.0f, Colour, Specular);
            WritePixel(Result.Colour, i, Colour);
            WritePixel(Result.Specular, i, Specular);
        }
        return Result;
    }

    UnpackedColourSpecular UnpackFusedColourMetalMask(const Rgba8Image& Colour, const Rgba8Image& Mask)
    {
        UnpackedColourSpecular Result;
        if (!Colour.IsValid() || !Mask.IsValid())
        {
            return Result;
        }

        const bool SameSize = Mask.Width == Colour.Width && Mask.Height == Colour.Height;
        const Rgba8Image Resized = SameSize ? Rgba8Image() : ResizeBilinear(Mask, Colour.Width, Colour.Height);
        const Rgba8Image& UseMask = SameSize ? Mask : Resized;

        Result.Colour = MakeImage(Colour);
        Result.Specular = MakeImage(Colour);

        const size_t Count = (size_t)Colour.Width * Colour.Height;
        for (size_t i = 0; i < Count; i++)
        {
            const auto Pixel = ReadPixel(Colour, i);
            const auto MaskPixel = ReadPixel(UseMask, i);
            ImagePixel OutColour, OutSpecular;
            // Metal from the mask's red, insulator range from the mask's alpha, colour keeps its alpha
            SplitFused(Pixel, MaskPixel.R, MaskPixel.A, Pixel.A, OutColour, OutSpecular);
            WritePixel(Result.Colour, i, OutColour);
            WritePixel(Result.Specular, i, OutSpecular);
        }
        return Result;
    }

    Rgba8Image ExtractChannel(const Rgba8Image& Source, ImageChannel Channel, bool Invert)
    {
        if (!Source.IsValid())
        {
            return Rgba8Image();
        }

        Rgba8Image Result = MakeImage(Source);
        const size_t Count = (size_t)Source.Width * Source.Height;
        for (size_t i = 0; i < Count; i++)
        {
            const auto Pixel = ReadPixel(Source, i);
            float Value = Pixel.R;
            switch (Channel)
            {
            case ImageChannel::Red: Value = Pixel.R; break;
            case ImageChannel::Green: Value = Pixel.G; break;
            case ImageChannel::Blue: Value = Pixel.B; break;
            case ImageChannel::Alpha: Value = Pixel.A; break;
            }
            WritePixel(Result, i, Grey(Invert ? 1.0f - Value : Value));
        }
        return Result;
    }

    UnpackedSpecularGloss UnpackSpecularGloss(const Rgba8Image& Source)
    {
        UnpackedSpecularGloss Result;
        if (!Source.IsValid())
        {
            return Result;
        }

        Result.Specular = MakeImage(Source);
        Result.Gloss = MakeImage(Source);

        const size_t Count = (size_t)Source.Width * Source.Height;
        for (size_t i = 0; i < Count; i++)
        {
            ImagePixel Specular, Gloss;
            // Core's split matches the digest (RGB kept, alpha as grey, both alpha 1)
            GameImageProcessors::SplitSpecularGloss(ReadPixel(Source, i), Specular, Gloss);
            WritePixel(Result.Specular, i, Specular);
            WritePixel(Result.Gloss, i, Gloss);
        }
        return Result;
    }
}
