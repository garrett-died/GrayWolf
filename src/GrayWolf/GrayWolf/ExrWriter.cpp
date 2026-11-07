#include "stdafx.h"

#include "ExrWriter.h"

#include <miniz/miniz.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>

namespace Exr
{
    uint16_t FloatToHalf(float Value)
    {
        if (std::isnan(Value)) return 0;
        const uint32_t Bits = std::bit_cast<uint32_t>(Value);
        const uint16_t Sign = static_cast<uint16_t>((Bits >> 16) & 0x8000u);
        const float Mag = std::min(std::fabs(Value), 65504.0f);
        const uint32_t M = std::bit_cast<uint32_t>(Mag);
        if (Mag < 6.103515625e-05f)
        {
            // subnormal half (or zero): value = k * 2^-24, k rounded to nearest even
            const float K = Mag * 16777216.0f;
            const float R = std::nearbyint(K);   // default rounding mode: to nearest even
            return static_cast<uint16_t>(Sign | static_cast<uint16_t>(R));
        }
        const uint32_t Exp = (M >> 23) - 127 + 15;
        uint32_t Mant = M & 0x7fffffu;
        uint32_t Half = (Exp << 10) | (Mant >> 13);
        const uint32_t Rest = Mant & 0x1fffu;
        if (Rest > 0x1000u || (Rest == 0x1000u && (Half & 1u))) Half++;   // round to nearest even; a carry runs into the exponent, which is right
        if (Half > 0x7bffu) Half = 0x7bffu;                               // 65504
        return static_cast<uint16_t>(Sign | Half);
    }

    namespace
    {
        void PutU32(std::vector<uint8_t>& B, uint32_t V)
        {
            for (int K = 0; K < 4; K++) B.push_back(static_cast<uint8_t>(V >> (8 * K)));
        }
        void PutU64(std::vector<uint8_t>& B, uint64_t V)
        {
            for (int K = 0; K < 8; K++) B.push_back(static_cast<uint8_t>(V >> (8 * K)));
        }
        void PutText(std::vector<uint8_t>& B, const char* S)
        {
            const std::string_view V(S);
            B.insert(B.end(), V.begin(), V.end());
            B.push_back(0);
        }
        void PutFloat(std::vector<uint8_t>& B, float F) { PutU32(B, std::bit_cast<uint32_t>(F)); }
        void Attribute(std::vector<uint8_t>& B, const char* Name, const char* Type, const std::vector<uint8_t>& Value)
        {
            PutText(B, Name);
            PutText(B, Type);
            PutU32(B, static_cast<uint32_t>(Value.size()));
            B.insert(B.end(), Value.begin(), Value.end());
        }

        // OpenEXR's ZIP pre-filter on a scanline's bytes: even-indexed bytes first, odd ones after, then each byte replaced by its difference from the one before (+128)
        void Filter(std::vector<uint8_t>& Raw)
        {
            const size_t N = Raw.size();
            std::vector<uint8_t> T(N);
            size_t A = 0, B = (N + 1) / 2;
            for (size_t I = 0; I < N;)
            {
                T[A++] = Raw[I++];
                if (I < N) T[B++] = Raw[I++];
            }
            for (size_t I = N; I-- > 1;) T[I] = static_cast<uint8_t>(T[I] - T[I - 1] + 128);
            Raw.swap(T);
        }
    }

    bool WriteHalfRgb(int Width, int Height, std::span<const uint16_t> Rgb, WriteCompression Method, std::vector<uint8_t>& Out, std::string& Why)
    {
        if (Width <= 0 || Height <= 0 || Width > 16384 || Height > 16384)
        {
            Why = "the size is not between 1 and 16384";
            return false;
        }
        if (Rgb.size() != static_cast<size_t>(Width) * static_cast<size_t>(Height) * 3)
        {
            Why = "the pixel span is not width * height * 3 halves";
            return false;
        }
        Out.clear();
        const std::array<uint8_t, 4> Magic = { 0x76, 0x2f, 0x31, 0x01 };
        Out.insert(Out.end(), Magic.begin(), Magic.end());
        PutU32(Out, 2);   // version 2, no flags (scanline, single part)
        {
            std::vector<uint8_t> Channels;
            for (const char* Name : { "B", "G", "R" })
            {
                PutText(Channels, Name);
                PutU32(Channels, 1);   // HALF
                Channels.insert(Channels.end(), { 0, 0, 0, 0 });   // pLinear, 3 reserved
                PutU32(Channels, 1);
                PutU32(Channels, 1);
            }
            Channels.push_back(0);
            Attribute(Out, "channels", "chlist", Channels);
        }
        Attribute(Out, "compression", "compression", { static_cast<uint8_t>(Method == WriteCompression::Zips ? 2 : 0) });
        std::vector<uint8_t> Box;
        PutU32(Box, 0), PutU32(Box, 0), PutU32(Box, static_cast<uint32_t>(Width - 1)), PutU32(Box, static_cast<uint32_t>(Height - 1));
        Attribute(Out, "dataWindow", "box2i", Box);
        Attribute(Out, "displayWindow", "box2i", Box);
        Attribute(Out, "lineOrder", "lineOrder", { 0 });
        {
            std::vector<uint8_t> V;
            PutFloat(V, 1.0f);
            Attribute(Out, "pixelAspectRatio", "float", V);
            V.clear();
            PutFloat(V, 0.0f), PutFloat(V, 0.0f);
            Attribute(Out, "screenWindowCenter", "v2f", V);
            V.clear();
            PutFloat(V, 1.0f);
            Attribute(Out, "screenWindowWidth", "float", V);
        }
        Out.push_back(0);   // end of header

        // chunks: one scanline each (NONE and ZIPS both)
        std::vector<std::vector<uint8_t>> Chunks(static_cast<size_t>(Height));
        const size_t Row = static_cast<size_t>(Width) * 6;
        std::vector<uint8_t> Raw(Row);
        for (int Y = 0; Y < Height; Y++)
        {
            const std::span<const uint16_t> P = Rgb.subspan(static_cast<size_t>(Y) * static_cast<size_t>(Width) * 3, static_cast<size_t>(Width) * 3);
            for (int X = 0; X < Width; X++)
                for (int C = 0; C < 3; C++)   // file order B, G, R; the span is R, G, B
                {
                    const uint16_t H = P[X * 3 + (2 - C)];
                    const size_t At = (static_cast<size_t>(C) * static_cast<size_t>(Width) + static_cast<size_t>(X)) * 2;
                    Raw[At] = static_cast<uint8_t>(H & 0xff);
                    Raw[At + 1] = static_cast<uint8_t>(H >> 8);
                }
            std::vector<uint8_t>& Chunk = Chunks[static_cast<size_t>(Y)];
            if (Method == WriteCompression::None)
            {
                Chunk = Raw;
                continue;
            }
            std::vector<uint8_t> Filtered = Raw;
            Filter(Filtered);
            mz_ulong Size = mz_compressBound(static_cast<mz_ulong>(Filtered.size()));
            Chunk.resize(Size);
            if (mz_compress2(Chunk.data(), &Size, Filtered.data(), static_cast<mz_ulong>(Filtered.size()), 4) != MZ_OK)
            {
                Why = "zlib failed on a scanline";
                return false;
            }
            Chunk.resize(Size);
            if (Chunk.size() >= Raw.size()) Chunk = Raw;   // OpenEXR stores a scanline raw when compression does not shrink it
        }
        uint64_t At = Out.size() + static_cast<uint64_t>(Height) * 8;
        for (const auto& Chunk : Chunks)
        {
            PutU64(Out, At);
            At += 8 + Chunk.size();
        }
        for (int Y = 0; Y < Height; Y++)
        {
            PutU32(Out, static_cast<uint32_t>(Y));
            PutU32(Out, static_cast<uint32_t>(Chunks[static_cast<size_t>(Y)].size()));
            Out.insert(Out.end(), Chunks[static_cast<size_t>(Y)].begin(), Chunks[static_cast<size_t>(Y)].end());
        }
        return true;
    }
}
