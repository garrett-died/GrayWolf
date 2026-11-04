#pragma once

// Module 1 of the Black Ops III conversion layer: undoing the way other games pack their
// images, so the Mod Tools get plain normal, gloss, occlusion, colour and specular maps.
// Pure functions over small RGBA8 buffers: no file I/O, no globals, no game knowledge. The
// callers (derived from a material description) decide which function a source image needs
// and what each returned image is called; results are keyed by meaning, never by a suffix.
//
// The math is a port of GameImageUtil's processors (GPL-3.0, Philip/Scobalula), as written up
// in docs/hydrax-port/gameimageutil.md. Where Core/GameImageProcessors already matches that
// digest it is called rather than repeated.

#include <cstdint>
#include <vector>

// Pixels are R,G,B,A bytes, row by row, top row first. A buffer is valid when
// Pixels.size() == Width * Height * 4 and both dimensions are non-zero.
struct Rgba8Image
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    std::vector<uint8_t> Pixels;

    bool IsValid() const
    {
        return Width != 0 && Height != 0 && Pixels.size() == (size_t)Width * Height * 4;
    }
};

enum class ImageChannel
{
    Red,
    Green,
    Blue,
    Alpha,
};

// An image rebuilt from a two-component normal. Empty (invalid) when the input was invalid.
struct UnpackedNormal
{
    Rgba8Image Normal;
};

// A packed normal/gloss/occlusion image split into its three meanings.
struct UnpackedNog
{
    Rgba8Image Normal;
    Rgba8Image Gloss;
    Rgba8Image Occlusion;
};

// A fused albedo/specular colour image split into diffuse colour and specular.
struct UnpackedColourSpecular
{
    Rgba8Image Colour;
    Rgba8Image Specular;
};

// A specular colour image carrying gloss in alpha, split into the two.
struct UnpackedSpecularGloss
{
    Rgba8Image Specular;
    Rgba8Image Gloss;
};

namespace Bo3Target
{
    // Normal with X in red and Y in green (modern games, BC5). Z is rebuilt; it is 0 where
    // X and Y already reach unit length. Output alpha is 1.
    UnpackedNormal UnpackNormalRedGreen(const Rgba8Image& Source);

    // Normal with X in alpha and Y in green (MW, WaW, MW2, MW3, BO1). Same output rules.
    UnpackedNormal UnpackNormalAlphaGreen(const Rgba8Image& Source);

    // IW7/IW8 style packed image: R gloss, G and A the hemi-octahedral normal (u, v), B occlusion.
    UnpackedNog UnpackNog(const Rgba8Image& Source);

    // Fused colour in RGB with the metal mask in alpha. Both outputs get alpha 1.
    UnpackedColourSpecular UnpackFusedColourMetalAlpha(const Rgba8Image& Source);

    // Fused colour whose metal mask is a separate image (WWII): metal from the mask's red,
    // insulator specular from the mask's alpha. The mask is resized to the colour image's
    // size first. The colour output keeps the colour image's own alpha; specular gets 1.
    // Returns empty images when either input is invalid.
    UnpackedColourSpecular UnpackFusedColourMetalMask(const Rgba8Image& Colour, const Rgba8Image& Mask);

    // One channel as a greyscale image (alpha 1), optionally inverted (1 - value).
    Rgba8Image ExtractChannel(const Rgba8Image& Source, ImageChannel Channel, bool Invert);

    // Specular colour in RGB and gloss in alpha: specular keeps RGB (alpha 1), gloss is the
    // alpha as greyscale (alpha 1).
    UnpackedSpecularGloss UnpackSpecularGloss(const Rgba8Image& Source);

    // Normalises a vector in place. A zero (or non-finite) length yields straight up (0,0,1).
    // Exposed so the guard can be tested: no hemi-octahedral pixel actually reaches it.
    void NormaliseOrUp(float& X, float& Y, float& Z);
}
