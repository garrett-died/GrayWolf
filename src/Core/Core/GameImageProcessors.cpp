#include "stdafx.h"

#include "GameImageProcessors.h"

#include <cmath>

namespace
{
    // The channels come in as 0 to 1 and the vectors they encode run -1 to 1
    inline float ToSigned(float Value)
    {
        return Value * 2.0f - 1.0f;
    }

    inline float ToChannel(float Value)
    {
        return Value * 0.5f + 0.5f;
    }

    // Rebuilds a normal from the two kept components. Where they already reach unit length
    // there's no root for Z: that is Z = 0, which a channel stores as 0.5 (Image.cpp NormalZ does
    // the same). GameImageUtil writes 0.0 there.
    //
    // The rim stays 0.5 for every caller and takes no parameter (a reviewer asked for one; rejected).
    // The reason is the encoding, not a score: a channel holds (Z + 1) / 2, so Z = 0 is 0.5 in any game,
    // and 0.0 would mean Z = -1, a vector pointing into the surface. The earlier comparison here (0.5
    // at 60.7 dB against 0.0 at 53.8 dB, 299 pairs) is not cited: 261 of those 299 pairs were copies of
    // earlier HydraX exports, which hold 0.5 by construction. Against the independent pairs only
    // (stock sources in other folders, tools/bo3target/image_oracle.py), 0.5 measures: Black Ops 4,
    // 162 normals, Z 38.0 dB median against a ceiling of 38.8 dB (the stock Z against the Z its own X and
    // Y imply); Black Ops III, 476 normals, Z 36.1 dB against a ceiling of 33.6 dB. 0.0 was not
    // re-measured on them.
    ImagePixel RebuildNormal(float X, float Y)
    {
        const auto Z = 1.0f - (X * X) - (Y * Y);

        ImagePixel Result;
        Result.R = ToChannel(X);
        Result.G = ToChannel(Y);
        Result.B = Z > 0.0f ? ToChannel(std::sqrt(Z)) : ToChannel(0.0f);
        Result.A = 1.0f;
        return Result;
    }
}

ImagePixel GameImageProcessors::UnpackNormalFromAlphaGreen(const ImagePixel& Packed)
{
    return RebuildNormal(ToSigned(Packed.A), ToSigned(Packed.G));
}

ImagePixel GameImageProcessors::UnpackNormalFromRedGreen(const ImagePixel& Packed)
{
    return RebuildNormal(ToSigned(Packed.R), ToSigned(Packed.G));
}

void GameImageProcessors::UnpackNormalOcclusionGloss(const ImagePixel& Packed, ImagePixel& Normal, ImagePixel& Occlusion, ImagePixel& Gloss)
{
    // Gloss and occlusion are stored plainly, one channel each
    Gloss.R = Gloss.G = Gloss.B = Packed.R;
    Gloss.A = 1.0f;

    Occlusion.R = Occlusion.G = Occlusion.B = Packed.B;
    Occlusion.A = 1.0f;

    // The normal was rotated and scaled from the centre diamond onto the unit square to
    // use the space better, so undo that before normalising
    const auto VectorX = ToSigned(Packed.G);
    const auto VectorY = ToSigned(Packed.A);

    const auto X = (VectorX + VectorY) * 0.5f;
    const auto Y = (VectorX - VectorY) * 0.5f;
    const auto Z = 1.0f - std::fabs(X) - std::fabs(Y);

    const auto Length = std::sqrt(X * X + Y * Y + Z * Z);
    const auto Scale = Length > 0.0f ? 1.0f / Length : 0.0f;

    Normal.R = ToChannel(X * Scale);
    Normal.G = ToChannel(Y * Scale);
    Normal.B = ToChannel(Z * Scale);
    Normal.A = 1.0f;
}

void GameImageProcessors::SplitSpecularGloss(const ImagePixel& Packed, ImagePixel& Specular, ImagePixel& Gloss)
{
    Specular.R = Packed.R;
    Specular.G = Packed.G;
    Specular.B = Packed.B;
    Specular.A = 1.0f;

    Gloss.R = Gloss.G = Gloss.B = Packed.A;
    Gloss.A = 1.0f;
}

void GameImageProcessors::SplitChannels(const ImagePixel& Packed, ImagePixel& Red, ImagePixel& Green, ImagePixel& Blue)
{
    Red.R = Red.G = Red.B = Packed.R;
    Red.A = 1.0f;

    Green.R = Green.G = Green.B = Packed.G;
    Green.A = 1.0f;

    Blue.R = Blue.G = Blue.B = Packed.B;
    Blue.A = 1.0f;
}
