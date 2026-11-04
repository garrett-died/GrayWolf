#include "stdafx.h"

// See Bo3Target.MaterialImages.h.

#include "Bo3Target.MaterialImages.h"

#include "ExportNaming.h"
#include "Image.h"

#include <atomic>
#include <objbase.h>

namespace fs = std::filesystem;

namespace
{
    // WIC needs COM on the calling thread. DirectXTex caches its WIC factory in a static, so COM must
    // stay up for the thread's life: a per-call CoUninitialize leaves that factory dangling (a crash on
    // the next TIFF or PNG call). A thread already in the other apartment mode is fine as it is.
    void EnsureCom()
    {
        struct Scope
        {
            HRESULT Result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // no CoUninitialize at thread exit: the static WIC factory may be released after this destructor ran
            ~Scope() = default;
        };
        static thread_local Scope Com;
    }

    bool IsOneChannel(DXGI_FORMAT Format)
    {
        return Format == DXGI_FORMAT_R8_UNORM || Format == DXGI_FORMAT_R8_SNORM || Format == DXGI_FORMAT_A8_UNORM ||
               Format == DXGI_FORMAT_R16_UNORM || Format == DXGI_FORMAT_R16_FLOAT || Format == DXGI_FORMAT_R32_FLOAT;
    }

    std::string Key(const Bo3Target::DeriveRequest& R)
    {
        return std::format("{}|{}|{}|{}|{}|{}", (int)R.Op, R.Source.generic_string(), R.Mask.generic_string(), (int)R.Channel,
                           R.Invert ? 1 : 0, R.FlipY ? 1 : 0);
    }
}

namespace Bo3Target
{
    bool LoadRgba8(const fs::path& File, Rgba8Image& Out, std::string& Error)
    {
        Out = Rgba8Image();
        EnsureCom();
        DirectX::ScratchImage Scratch;
        DirectX::TexMetadata Meta;
        const std::string Ext = ExportNaming::Lower(File.extension().string());
        HRESULT R;
        if (Ext == ".tga") R = DirectX::LoadFromTGAFile(File.c_str(), &Meta, Scratch);
        else if (Ext == ".dds") R = DirectX::LoadFromDDSFile(File.c_str(), DirectX::DDS_FLAGS_NONE, &Meta, Scratch);
        else if (Ext == ".png" || Ext == ".tif" || Ext == ".tiff") R = DirectX::LoadFromWICFile(File.c_str(), DirectX::WIC_FLAGS_NONE, &Meta, Scratch);
        else
        {
            Error = "unsupported_extension";
            return false;
        }
        if (FAILED(R) || Scratch.GetImageCount() == 0)
        {
            Error = "load_failed";
            return false;
        }

        // an sRGB file keeps its stored values: read as plain unorm, no curve applied
        DXGI_FORMAT Format = Meta.format;
        if (DirectX::IsSRGB(Format))
        {
            Format = DirectX::MakeLinear(Format);
            if (!Scratch.OverrideFormat(Format))
            {
                Error = "load_failed";
                return false;
            }
        }
        const DirectX::Image* First = Scratch.GetImage(0, 0, 0);
        if (First == nullptr || DirectX::IsPlanar(Format))
        {
            Error = "load_failed";
            return false;
        }

        DirectX::ScratchImage Rgba;
        if (DirectX::IsCompressed(Format))
            R = DirectX::Decompress(*First, DXGI_FORMAT_R8G8B8A8_UNORM, Rgba);
        else if (Format == DXGI_FORMAT_R8G8B8A8_UNORM)
            R = Rgba.InitializeFromImage(*First);
        else
            R = DirectX::Convert(*First, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_FORCE_NON_WIC, DirectX::TEX_THRESHOLD_DEFAULT, Rgba);
        const DirectX::Image* Image = FAILED(R) ? nullptr : Rgba.GetImage(0, 0, 0);
        if (Image == nullptr || Image->format != DXGI_FORMAT_R8G8B8A8_UNORM || Image->width == 0 || Image->height == 0)
        {
            Error = "load_failed";
            return false;
        }

        Out.Width = (uint32_t)Image->width;
        Out.Height = (uint32_t)Image->height;
        Out.Pixels.resize((size_t)Out.Width * Out.Height * 4);
        const bool Grey = IsOneChannel(Format);
        for (size_t Y = 0; Y < Image->height; Y++)
        {
            const uint8_t* Row = Image->pixels + Y * Image->rowPitch;
            uint8_t* To = &Out.Pixels[Y * Out.Width * 4];
            std::memcpy(To, Row, (size_t)Out.Width * 4);
            if (Grey)
                for (uint32_t X = 0; X < Out.Width; X++)
                {
                    To[X * 4 + 1] = To[X * 4 + 2] = To[X * 4];
                    To[X * 4 + 3] = 255;
                }
        }
        return true;
    }

    bool SaveRgba8(const fs::path& File, const Rgba8Image& Pixels, std::string& Error)
    {
        const std::string Ext = ExportNaming::Lower(File.extension().string());
        const ImageFormat Format = Ext == ".png" ? ImageFormat::Standard_PNG
                                 : Ext == ".tif" || Ext == ".tiff" ? ImageFormat::Standard_TIFF
                                 : Ext == ".tga" ? ImageFormat::Standard_TGA : ImageFormat::NoConversion;
        if (Format == ImageFormat::NoConversion || !Pixels.IsValid())
        {
            Error = "bad_request";
            return false;
        }
        EnsureCom();
        auto Scratch = std::make_unique<DirectX::ScratchImage>();
        if (FAILED(Scratch->Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, Pixels.Width, Pixels.Height, 1, 1)))
        {
            Error = "write_failed";
            return false;
        }
        const DirectX::Image* To = Scratch->GetImage(0, 0, 0);
        for (uint32_t Y = 0; Y < Pixels.Height; Y++)
            std::memcpy(To->pixels + (size_t)Y * To->rowPitch, &Pixels.Pixels[(size_t)Y * Pixels.Width * 4], (size_t)Pixels.Width * 4);

        std::error_code Ec;
        fs::create_directories(File.parent_path(), Ec);
        // Never removes or replaces: a file that is there (or appears before the move) stays, and the failure is reported
        if (fs::exists(File, Ec) || Ec)
        {
            Error = "exists";
            return false;
        }
        // a unique temp beside the destination, claimed exclusively (CREATE_NEW), written, then moved without replace
        static std::atomic<unsigned> Counter{ 0 };
        fs::path Temp;
        for (int Try = 0;; Try++)
        {
            if (Try >= 100)
            {
                Error = "write_failed";
                return false;
            }
            Temp = File.parent_path() / (File.stem().native() + L".gw" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(Counter.fetch_add(1)) + L".tmp" + File.extension().native());
            const HANDLE Claim = CreateFileW(Temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (Claim != INVALID_HANDLE_VALUE)
            {
                CloseHandle(Claim);
                break;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            {
                Error = "write_failed";
                return false;
            }
        }
        bool Encoded = false;
        try
        {
            const std::u8string U8 = Temp.u8string();
            const std::string Utf8(reinterpret_cast<const char*>(U8.data()), U8.size());
            const bool Ascii = std::all_of(Utf8.begin(), Utf8.end(), [](char C) { return static_cast<unsigned char>(C) < 0x80; });
            if (Format == ImageFormat::Standard_PNG && !Ascii)
            {
                // the PNG fast path of ConvertToFormat opens the name with the ANSI code page: WIC takes the wide name
                Encoded = SUCCEEDED(DirectX::SaveToWICFile(*To, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), Temp.c_str()));
            }
            else
            {
                Encoded = Image::ConvertToFormat(Scratch, Scratch->GetMetadata(), Utf8, Format);
            }
        }
        catch (const std::exception&)   // boundary: the encoder library throws on a name it cannot read; reported as a failed write
        {
            Encoded = false;
        }
        const auto Drop = [&] { std::error_code E2; fs::remove(Temp, E2); };
        if (!Encoded || !fs::is_regular_file(Temp, Ec) || fs::file_size(Temp, Ec) == 0)
        {
            Drop();
            Error = "write_failed";
            return false;
        }
        if (!MoveFileExW(Temp.c_str(), File.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            Drop();
            Error = GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_FILE_EXISTS ? "exists" : "write_failed";
            return false;
        }
        return true;
    }

    void FlipGreen(Rgba8Image& Image)
    {
        for (size_t I = 1; I < Image.Pixels.size(); I += 4) Image.Pixels[I] = (uint8_t)(255 - Image.Pixels[I]);
    }

    bool DerivedImages::Read(Slot& Where, const fs::path& File)
    {
        if (Where.File != File)
        {
            Where = Slot();
            Where.File = File;
        }
        if (!Where.Loaded && !Where.Failed)
        {
            std::string Error;
            if (LoadRgba8(File, Where.Image, Error)) Where.Loaded = true;
            else Where.Failed = true;
        }
        return Where.Loaded;
    }

    bool DerivedImages::Made(const fs::path& File) const
    {
        return MadeFiles.count(ExportNaming::Lower(File.lexically_normal().generic_string())) != 0;
    }

    DeriveResult DerivedImages::Derive(const DeriveRequest& Request)
    {
        const std::string K = Key(Request);
        if (const auto It = Done.find(K); It != Done.end()) return It->second;
        DeriveResult Result;
        const auto Remember = [&](const char* Error) {
            Result.Error = Error;
            if (*Error) Result.File.clear();
            return Done.emplace(K, Result).first->second;
        };
        if (Request.Source.empty() || Request.Tag.empty()) return Remember("bad_request");
        const bool UsesMask = Request.Op == DeriveOp::FusedMaskColour || Request.Op == DeriveOp::FusedMaskSpecular;
        if (UsesMask && Request.Mask.empty()) return Remember("bad_request");

        if (!Read(SourceImage, Request.Source)) return Remember("load_failed");
        const Rgba8Image& Source = SourceImage.Image;

        Rgba8Image Out;
        switch (Request.Op)
        {
        case DeriveOp::NormalNog:
            Out = UnpackNog(Source).Normal;
            break;
        case DeriveOp::NormalAlphaGreen:
            Out = UnpackNormalAlphaGreen(Source).Normal;
            break;
        case DeriveOp::NormalFlipOnly:
            Out = Source;
            break;
        case DeriveOp::Channel:
            Out = ExtractChannel(Source, Request.Channel, Request.Invert);
            break;
        case DeriveOp::FusedAlphaColour:
        case DeriveOp::FusedAlphaSpecular:
        case DeriveOp::FusedMaskColour:
        case DeriveOp::FusedMaskSpecular:
        {
            const std::string FK = std::format("{}|{}|{}", UsesMask ? 1 : 0, Request.Source.generic_string(), Request.Mask.generic_string());
            if (FK != FusedKey)
            {
                if (UsesMask)
                {
                    if (!Read(MaskImage, Request.Mask)) return Remember("mask_load_failed");
                    Fused = UnpackFusedColourMetalMask(Source, MaskImage.Image);
                }
                else
                {
                    Fused = UnpackFusedColourMetalAlpha(Source);
                }
                FusedKey = Fused.Colour.IsValid() ? FK : std::string();
            }
            Out = (Request.Op == DeriveOp::FusedAlphaColour || Request.Op == DeriveOp::FusedMaskColour) ? Fused.Colour : Fused.Specular;
            break;
        }
        }
        if (!Out.IsValid()) return Remember("load_failed");
        if (Request.FlipY && (Request.Op == DeriveOp::NormalNog || Request.Op == DeriveOp::NormalAlphaGreen || Request.Op == DeriveOp::NormalFlipOnly))
            FlipGreen(Out);

        // <stem>_<tag><ext> beside the source; a name another request took gets _2, _3...
        std::string Ext = ExportNaming::Lower(Request.Source.extension().string());
        if (Ext != ".png" && Ext != ".tif" && Ext != ".tiff") Ext = ".png";
        const std::string Stem = Request.Source.stem().string() + "_" + Request.Tag;
        fs::path File;
        bool Reused = false;
        for (int N = 1;; N++)
        {
            File = Request.Source.parent_path() / (Stem + (N > 1 ? "_" + std::to_string(N) : std::string()) + Ext);
            const auto [It, Fresh] = Claimed.emplace(ExportNaming::Lower(File.generic_string()), K);
            if (!Fresh && It->second != K) continue;
            if (!Fresh) break;
            std::error_code ExistsEc;
            if (!fs::exists(File, ExistsEc) && !ExistsEc) break;
            // something is already there that this run did not derive: it stays unless it is exactly what we would
            // write (an earlier run of the same export), so a rerun is stable and a real image is never replaced
            Rgba8Image Have;
            std::string LoadError;
            if (!ExistsEc && LoadRgba8(File, Have, LoadError) && Have.Width == Out.Width && Have.Height == Out.Height && Have.Pixels == Out.Pixels)
            {
                Reused = true;
                break;
            }
        }
        std::string Error;
        if (!Plan && !Reused)
        {
            if (!SaveRgba8(File, Out, Error)) return Remember("write_failed");   // an existing file stays ("exists"): reported as a failed write
        }
        FilesWritten++;
        MadeFiles.insert(ExportNaming::Lower(File.generic_string()));
        Result.File = File;
        return Remember("");
    }
}
