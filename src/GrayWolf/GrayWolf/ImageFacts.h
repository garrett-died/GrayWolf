#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

// Facts read from an image file's header, without decoding it
namespace ImageFacts
{
    // The size from the first bytes of a PNG or a DDS (false: neither, or too short)
    inline bool SizeFromHeader(const unsigned char* B, size_t Count, uint32_t& W, uint32_t& H)
    {
        if (Count < 24) return false;
        const std::span<const unsigned char> Bytes(B, Count);
        const auto Be = [&](size_t At) { return (uint32_t(Bytes[At]) << 24) | (uint32_t(Bytes[At + 1]) << 16) | (uint32_t(Bytes[At + 2]) << 8) | Bytes[At + 3]; };
        const auto Le = [&](size_t At) { return uint32_t(Bytes[At]) | (uint32_t(Bytes[At + 1]) << 8) | (uint32_t(Bytes[At + 2]) << 16) | (uint32_t(Bytes[At + 3]) << 24); };
        if (Bytes[0] == 0x89 && Bytes[1] == 'P' && Bytes[2] == 'N' && Bytes[3] == 'G')
        {
            W = Be(16), H = Be(20);
            return true;
        }
        if (Bytes[0] == 'D' && Bytes[1] == 'D' && Bytes[2] == 'S' && Bytes[3] == ' ')
        {
            H = Le(12), W = Le(16);
            return true;
        }
        return false;
    }

    inline bool SizeOf(const std::filesystem::path& File, uint32_t& W, uint32_t& H)
    {
        std::ifstream In(File, std::ios::binary);
        unsigned char B[32] = {};
        In.read(reinterpret_cast<char*>(B), sizeof(B));
        return SizeFromHeader(B, static_cast<size_t>(In.gcount()), W, H);
    }

    // The compressionMethod to write for an image of this size: a block compressed image needs width and height in multiples of 4 (the gate's
    // image.block_size), so a smaller or odd one (the 1x1 black and white a zone ships) is "uncompressed". The R8/R16 formats are not block compressed.
    inline std::string CompressionFor(const std::string& Method, uint32_t W, uint32_t H)
    {
        if (Method == "uncompressed" || Method.rfind("R", 0) == 0) return Method;
        if (W != 0 && H != 0 && ((W % 4) != 0 || (H % 4) != 0)) return "uncompressed";
        return Method;
    }
}
