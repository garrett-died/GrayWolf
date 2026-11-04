#pragma once

// Module 2a of the Black Ops III conversion layer: the file side of image unpacking. It loads an
// exported image (PNG, TIFF, TGA or DDS) as RGBA8, runs the Bo3Target function a request names
// (Bo3Target.ImageUnpack.h), writes the result beside the source and remembers it, so a map that
// several materials share is read and unpacked once per run. Which function a map needs is the
// caller's decision, taken from the material description (MaterialGdt); this file guesses nothing
// from names.

#include "Bo3Target.ImageUnpack.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace Bo3Target
{
    // Loads any image DirectXTex reads as plain RGBA8 bytes: sRGB formats keep their stored
    // values, compressed ones are decoded, a one-channel image becomes grey (R=G=B), a missing
    // alpha is 255. Only the first image of a mipped, array or cube file is read.
    bool LoadRgba8(const std::filesystem::path& File, Rgba8Image& Out, std::string& Error);

    // Saves RGBA8 as .png, .tif/.tiff or .tga by the extension. Anything else is an error. Never removes or replaces
    // a file: the image is written to a unique temp beside File and moved without replace, so an existing File
    // (or one that appears before the move) is left alone and Error is "exists". Other failures: "write_failed".
    bool SaveRgba8(const std::filesystem::path& File, const Rgba8Image& Image, std::string& Error);

    // Inverts the green channel in place (a DirectX normal map to OpenGL and back).
    void FlipGreen(Rgba8Image& Image);

    enum class DeriveOp
    {
        NormalNog,               // normal from a NOG image (hemi-octahedral)
        NormalAlphaGreen,        // normal from x in alpha and y in green
        NormalFlipOnly,          // the image as it is with green flipped
        Channel,                 // one channel as grey, optionally inverted
        FusedAlphaColour,        // colour of a colour image whose alpha is the metal mask
        FusedAlphaSpecular,      // specular colour of the same
        FusedMaskColour,         // colour of a colour image with a separate metal mask image
        FusedMaskSpecular,       // specular colour of the same
    };

    struct DeriveRequest
    {
        DeriveOp Op = DeriveOp::Channel;
        std::filesystem::path Source;   // the packed image (the colour image for the fused mask ops)
        std::filesystem::path Mask;     // FusedMask*: the metal mask image
        ImageChannel Channel = ImageChannel::Red;   // Channel
        bool Invert = false;            // Channel
        bool FlipY = false;             // Normal*: green flipped after the decode
        std::string Tag;                // the material slot's letter(s) in the new file name ("n", "g", "sm"...)
    };

    // File: the written image. Error empty on success, else a short reason without spaces
    // ("load_failed", "mask_load_failed", "write_failed", "bad_request") a caller can count.
    struct DeriveResult
    {
        std::filesystem::path File;
        std::string Error;
        bool Ok() const { return Error.empty(); }
    };

    // The derived images of one run. The file is <source dir>/<source stem>_<Tag><ext>, ext being
    // the source's when it is .png, .tif or .tiff, else .png. Two requests that would name the same
    // file get "_2", "_3"...; the same request twice is answered from memory and written once.
    class DerivedImages
    {
    public:
        DeriveResult Derive(const DeriveRequest& Request);
        // the images derived (written, or reused when an identical one is already there)
        size_t Written() const { return FilesWritten; }
        // True for a file this object derived, in a plan too
        bool Made(const std::filesystem::path& File) const;
        // On: work out every result, load every source, write no file (a plan)
        void DryRun(bool On) { Plan = On; }

    private:
        struct Slot
        {
            std::filesystem::path File;
            Rgba8Image Image;
            bool Loaded = false, Failed = false;
        };
        bool Read(Slot& Where, const std::filesystem::path& File);

        std::map<std::string, DeriveResult> Done;       // by request key
        std::set<std::string> MadeFiles;                // lower-case output paths this object derived
        std::map<std::string, std::string> Claimed;     // lower-case output path -> request key
        Slot SourceImage, MaskImage;                    // the last ones read: a pack's outputs come one after another
        std::string FusedKey;
        UnpackedColourSpecular Fused;
        size_t FilesWritten = 0;
        bool Plan = false;
    };
}
