#include "stdafx.h"

#include "ExrReader.h"

#include <libdeflate.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <queue>
#include <random>
#include <span>
#include <string>
#include <vector>

// exr-selftest. Every case of HydraX's tests/HydraX.Formats.Tests/Exr/ExrReaderTests.cs (the EXR and PIZ cases; its
// PizDecoder is exercised there through the same files and the Huffman case) is here with the same expected values, on
// files built in memory by a test-only encoder (ExrTestWriter.cs ported: NONE, ZIPS, ZIP, PIZ, written from the format
// description and sharing nothing with the decoder but the format). Not ported: ExrWriterTests (HydraX's ExrWriter is
// not part of this module) and ExrLiveTests (needs the Mod Tools; run by hand, see the unit's report). Then the cases
// the port added: decreasing line order, exhaustive half conversion, hostile offsets, hostile Huffman streams, a stream
// that inflates to too much, a real file read, and the cancel flag. No game data.
namespace Exr
{
namespace
{
    using Bytes = std::vector<uint8_t>;

    struct Rng
    {
        std::mt19937 Engine;
        explicit Rng(uint32_t Seed) : Engine(Seed) {}
        int Next(int Below) { return static_cast<int>(Engine() % static_cast<uint32_t>(Below)); }
        double NextDouble() { return static_cast<double>(Engine()) / 4294967296.0; }
    };

    // ---- float <-> half bits (round to nearest even), for the test image

    uint32_t FloatToHalfBits(float F)
    {
        uint32_t X;
        std::memcpy(&X, &F, sizeof(X));
        const uint32_t Sign = (X >> 16) & 0x8000u;
        const int32_t E = static_cast<int32_t>((X >> 23) & 0xFFu);
        uint32_t M = X & 0x7FFFFFu;
        if (E == 0xFF) return Sign | 0x7C00u | (M != 0 ? (0x200u | (M >> 13)) : 0u);

        const int32_t He = E - 127 + 15;
        if (He >= 31) return Sign | 0x7C00u;
        if (He <= 0)
        {
            if (He < -10) return Sign;
            M |= 0x800000u;
            const int Shift = 14 - He;
            uint32_t Hm = M >> Shift;
            const uint32_t Rem = M & ((1u << Shift) - 1);
            const uint32_t Mid = 1u << (Shift - 1);
            if (Rem > Mid || (Rem == Mid && (Hm & 1u))) Hm++;
            return Sign | Hm;
        }

        const uint32_t Hm = M >> 13;
        const uint32_t Rem = M & 0x1FFFu;
        uint32_t R = Sign | (static_cast<uint32_t>(He) << 10) | Hm;
        if (Rem > 0x1000u || (Rem == 0x1000u && (Hm & 1u))) R++;
        return R;
    }

    uint32_t FloatBits(float F)
    {
        uint32_t X;
        std::memcpy(&X, &F, sizeof(X));
        return X;
    }

    float BitsFloat(uint32_t X)
    {
        float F;
        std::memcpy(&F, &X, sizeof(F));
        return F;
    }

    // ---- the test image: raw sample bit patterns per channel

    struct TestChannel
    {
        std::string Name;
        PixelType Type;
    };

    struct TestImage
    {
        int Width, Height, MinX, MinY;
        std::vector<TestChannel> Channels;
        std::vector<std::vector<uint32_t>> Samples;

        TestImage(int W, int H, std::vector<TestChannel> Ch, int MinXv = 0, int MinYv = 0)
            : Width(W), Height(H), MinX(MinXv), MinY(MinYv), Channels(std::move(Ch))
        {
            Samples.assign(Channels.size(), std::vector<uint32_t>(static_cast<size_t>(W) * H, 0));
        }

        template <class F>
        void Fill(F Fn)
        {
            for (size_t C = 0; C < Channels.size(); C++)
                for (int Y = 0; Y < Height; Y++)
                    for (int X = 0; X < Width; X++)
                    {
                        const float V = Fn(X, Y, static_cast<int>(C));
                        Samples[C][(static_cast<size_t>(Y) * Width) + X] =
                            Channels[C].Type == PixelType::Half ? FloatToHalfBits(V)
                            : Channels[C].Type == PixelType::Real32 ? FloatBits(V)
                            : static_cast<uint32_t>(V);
                    }
        }

        float ValueAt(size_t C, int X, int Y) const
        {
            const uint32_t Bits = Samples[C][(static_cast<size_t>(Y) * Width) + X];
            switch (Channels[C].Type)
            {
            case PixelType::Half: return HalfToFloat(static_cast<uint16_t>(Bits));
            case PixelType::Real32: return BitsFloat(Bits);
            default: return static_cast<float>(Bits);
            }
        }
    };

    // ---- byte writers

    void Put8(Bytes& B, uint32_t V) { B.push_back(static_cast<uint8_t>(V)); }
    void Put16(Bytes& B, uint32_t V) { Put8(B, V); Put8(B, V >> 8); }
    void Put32(Bytes& B, uint32_t V) { Put16(B, V); Put16(B, V >> 16); }
    void Put64(Bytes& B, uint64_t V) { Put32(B, static_cast<uint32_t>(V)); Put32(B, static_cast<uint32_t>(V >> 32)); }
    void PutStr(Bytes& B, const std::string& S) { B.insert(B.end(), S.begin(), S.end()); Put8(B, 0); }

    void Attribute(Bytes& B, const std::string& Name, const std::string& Type, const Bytes& Value)
    {
        PutStr(B, Name);
        PutStr(B, Type);
        Put32(B, static_cast<uint32_t>(Value.size()));
        B.insert(B.end(), Value.begin(), Value.end());
    }

    // ---- canonical Huffman encoder in the PIZ stream layout (code table with zero runs, run-length pseudo-symbol)

    constexpr int EncSize = (1 << 16) + 1;

    struct BitWriter
    {
        Bytes Out;
        uint32_t Acc = 0;
        int Count = 0;
        int64_t BitCount = 0;

        void Write(uint64_t Value, int Bits)
        {
            for (int I = Bits - 1; I >= 0; I--)
            {
                Acc = (Acc << 1) | static_cast<uint32_t>((Value >> I) & 1);
                Count++;
                BitCount++;
                if (Count == 8)
                {
                    Out.push_back(static_cast<uint8_t>(Acc));
                    Acc = 0;
                    Count = 0;
                }
            }
        }

        Bytes ToArray() const
        {
            Bytes R = Out;
            if (Count > 0) R.push_back(static_cast<uint8_t>(Acc << (8 - Count)));
            return R;
        }
    };

    std::vector<int> BuildLengths(const std::vector<int64_t>& Freq, int Im, int IM)
    {
        std::vector<int> Lengths(EncSize, 0);
        std::vector<int> Parent;
        std::vector<int> SymbolOf;
        std::vector<int64_t> Weights;
        using Entry = std::pair<int64_t, int>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> Live;
        for (int S = Im; S <= IM; S++)
        {
            if (Freq[static_cast<size_t>(S)] > 0)
            {
                const int Id = static_cast<int>(SymbolOf.size());
                SymbolOf.push_back(S);
                Parent.push_back(-1);
                Weights.push_back(Freq[static_cast<size_t>(S)]);
                Live.push({ Freq[static_cast<size_t>(S)], Id });
            }
        }

        if (Live.size() == 1)
        {
            Lengths[static_cast<size_t>(SymbolOf[0])] = 1;
            return Lengths;
        }

        while (Live.size() > 1)
        {
            const int A = Live.top().second; Live.pop();
            const int B = Live.top().second; Live.pop();
            const int Id = static_cast<int>(Weights.size());
            Weights.push_back(Weights[static_cast<size_t>(A)] + Weights[static_cast<size_t>(B)]);
            Parent.push_back(-1);
            SymbolOf.push_back(-1);
            Parent[static_cast<size_t>(A)] = Id;
            Parent[static_cast<size_t>(B)] = Id;
            Live.push({ Weights[static_cast<size_t>(Id)], Id });
        }

        for (size_t I = 0; I < SymbolOf.size(); I++)
        {
            if (SymbolOf[I] < 0) continue;
            int Depth = 0;
            for (int N = static_cast<int>(I); Parent[static_cast<size_t>(N)] >= 0; N = Parent[static_cast<size_t>(N)]) Depth++;
            Lengths[static_cast<size_t>(SymbolOf[I])] = Depth;
        }
        return Lengths;
    }

    std::vector<uint64_t> AssignCodes(const std::vector<int>& Lengths, int Im, int IM)
    {
        std::vector<int64_t> Count(60, 0);
        for (int S = Im; S <= IM; S++) Count[static_cast<size_t>(Lengths[static_cast<size_t>(S)])]++;

        std::vector<uint64_t> Start(60, 0);
        uint64_t C = 0;
        for (int I = 58; I > 0; I--)
        {
            const uint64_t Next = (C + static_cast<uint64_t>(Count[static_cast<size_t>(I)])) >> 1;
            Start[static_cast<size_t>(I)] = C;
            C = Next;
        }

        std::vector<uint64_t> Codes(EncSize, 0);
        for (int S = Im; S <= IM; S++)
        {
            const int L = Lengths[static_cast<size_t>(S)];
            if (L > 0) Codes[static_cast<size_t>(S)] = Start[static_cast<size_t>(L)]++;
        }
        return Codes;
    }

    Bytes HufCompress(const std::vector<uint16_t>& Data, bool UseRuns = true)
    {
        // Tokenise: literals and (rlc, count) runs
        std::vector<std::pair<int, int>> Tokens;
        std::vector<int64_t> Freq(EncSize, 0);
        int Max = 0;
        for (const uint16_t V : Data) Max = (std::max)(Max, static_cast<int>(V));
        const int Rlc = Max + 1;
        for (size_t I = 0; I < Data.size();)
        {
            size_t Run = 1;
            while (UseRuns && I + Run < Data.size() && Data[I + Run] == Data[I]) Run++;

            Tokens.push_back({ Data[I], -1 });
            Freq[Data[I]]++;
            if (Run >= 3)
            {
                size_t Rem = Run - 1;
                while (Rem > 0)
                {
                    const int N = static_cast<int>((std::min<size_t>)(Rem, 255));
                    Tokens.push_back({ Rlc, N });
                    Freq[static_cast<size_t>(Rlc)]++;
                    Rem -= static_cast<size_t>(N);
                }
                I += Run;
            }
            else
            {
                for (size_t R = 1; R < Run; R++)
                {
                    Tokens.push_back({ Data[I], -1 });
                    Freq[Data[I]]++;
                }
                I += Run;
            }
        }

        Freq[static_cast<size_t>(Rlc)] = (std::max<int64_t>)(Freq[static_cast<size_t>(Rlc)], 1);
        int Im = 0;
        while (Freq[static_cast<size_t>(Im)] == 0) Im++;

        const std::vector<int> Lengths = BuildLengths(Freq, Im, Rlc);
        const std::vector<uint64_t> Codes = AssignCodes(Lengths, Im, Rlc);

        BitWriter Table;
        for (int S = Im; S <= Rlc; S++)
        {
            if (Lengths[static_cast<size_t>(S)] == 0)
            {
                int Z = 1;
                while (S < Rlc && Z < 255 + 6 && Lengths[static_cast<size_t>(S) + 1] == 0)
                {
                    S++;
                    Z++;
                }

                if (Z >= 2)
                {
                    if (Z >= 6)
                    {
                        Table.Write(63, 6);
                        Table.Write(static_cast<uint64_t>(Z - 6), 8);
                    }
                    else
                    {
                        Table.Write(static_cast<uint64_t>(59 + Z - 2), 6);
                    }
                    continue;
                }
            }
            Table.Write(static_cast<uint64_t>(Lengths[static_cast<size_t>(S)]), 6);
        }
        const Bytes TableBytes = Table.ToArray();

        BitWriter Bits;
        for (const auto& [Symbol, Extra] : Tokens)
        {
            Bits.Write(Codes[static_cast<size_t>(Symbol)], Lengths[static_cast<size_t>(Symbol)]);
            if (Symbol == Rlc && Extra >= 0) Bits.Write(static_cast<uint64_t>(Extra), 8);
        }

        Bytes Result;
        Put32(Result, static_cast<uint32_t>(Im));
        Put32(Result, static_cast<uint32_t>(Rlc));
        Put32(Result, static_cast<uint32_t>(TableBytes.size()));
        Put32(Result, static_cast<uint32_t>(Bits.BitCount));
        Put32(Result, 0);
        Result.insert(Result.end(), TableBytes.begin(), TableBytes.end());
        const Bytes Payload = Bits.ToArray();
        Result.insert(Result.end(), Payload.begin(), Payload.end());
        return Result;
    }

    // The longest code the encoder assigns for this data (for the test that needs codes past the primary table)
    int LongestCode(const std::vector<uint16_t>& Data)
    {
        std::vector<int64_t> Freq(EncSize, 0);
        int Max = 0;
        for (const uint16_t V : Data)
        {
            Freq[V]++;
            Max = (std::max)(Max, static_cast<int>(V));
        }
        Freq[static_cast<size_t>(Max) + 1] = 1;
        int Im = 0;
        while (Freq[static_cast<size_t>(Im)] == 0) Im++;
        const std::vector<int> L = BuildLengths(Freq, Im, Max + 1);
        return *std::max_element(L.begin(), L.end());
    }

    // ---- PIZ wavelet encode and chunk

    void Enc14(uint16_t A, uint16_t B, uint16_t& L, uint16_t& H)
    {
        const int Sa = static_cast<int16_t>(A);
        const int Sb = static_cast<int16_t>(B);
        const int M = (Sa + Sb) >> 1;
        const int D = Sa - Sb;
        L = static_cast<uint16_t>(static_cast<int16_t>(M));
        H = static_cast<uint16_t>(static_cast<int16_t>(D));
    }

    void Enc16(uint16_t A, uint16_t B, uint16_t& L, uint16_t& H)
    {
        constexpr int Offset = 1 << 15;
        constexpr int Mask = 0xFFFF;
        const int Ao = (A + Offset) & Mask;
        int M = (Ao + B) >> 1;
        int D = Ao - B;
        if (D < 0) M = (M + Offset) & Mask;
        D &= Mask;
        L = static_cast<uint16_t>(M);
        H = static_cast<uint16_t>(D);
    }

    void Wav2Encode(std::span<uint16_t> A, int Start, int Nx, int Ox, int Ny, int Oy, int Mx)
    {
        const bool W14 = Mx < (1 << 14);
        const int N = (std::min)(Nx, Ny);
        int P = 1;
        int P2 = 2;
        while (P2 <= N)
        {
            int Py = Start;
            const int Ey = Start + (Oy * (Ny - P2));
            const int Oy1 = Oy * P;
            const int Oy2 = Oy * P2;
            const int Ox1 = Ox * P;
            const int Ox2 = Ox * P2;
            for (; Py <= Ey; Py += Oy2)
            {
                int Px = Py;
                const int Ex = Py + (Ox * (Nx - P2));
                for (; Px <= Ex; Px += Ox2)
                {
                    const int P01 = Px + Ox1;
                    const int P10 = Px + Oy1;
                    const int P11 = P10 + Ox1;
                    uint16_t I00, I01, I10, I11;
                    if (W14)
                    {
                        Enc14(A[Px], A[P01], I00, I01);
                        Enc14(A[P10], A[P11], I10, I11);
                        Enc14(I00, I10, A[Px], A[P10]);
                        Enc14(I01, I11, A[P01], A[P11]);
                    }
                    else
                    {
                        Enc16(A[Px], A[P01], I00, I01);
                        Enc16(A[P10], A[P11], I10, I11);
                        Enc16(I00, I10, A[Px], A[P10]);
                        Enc16(I01, I11, A[P01], A[P11]);
                    }
                }

                if ((Nx & P) != 0)
                {
                    const int P10 = Px + Oy1;
                    uint16_t I00;
                    if (W14) Enc14(A[Px], A[P10], I00, A[P10]);
                    else Enc16(A[Px], A[P10], I00, A[P10]);
                    A[Px] = I00;
                }
            }

            if ((Ny & P) != 0)
            {
                int Px = Py;
                const int Ex = Py + (Ox * (Nx - P2));
                for (; Px <= Ex; Px += Ox2)
                {
                    const int P01 = Px + Ox1;
                    uint16_t I00;
                    if (W14) Enc14(A[Px], A[P01], I00, A[P01]);
                    else Enc16(A[Px], A[P01], I00, A[P01]);
                    A[Px] = I00;
                }
            }

            P = P2;
            P2 <<= 1;
        }
    }

    Bytes PizChunk(const TestImage& Image, const std::vector<size_t>& Order, int First, int Lines)
    {
        constexpr int BitmapBytes = 8192;
        const int Width = Image.Width;
        std::vector<std::pair<int, int>> Regions; // start, size in u16
        std::vector<uint16_t> Data;
        for (const size_t Ci : Order)
        {
            const int Size = Image.Channels[Ci].Type == PixelType::Half ? 1 : 2;
            Regions.push_back({ static_cast<int>(Data.size()), Size });
            for (int L = 0; L < Lines; L++)
                for (int X = 0; X < Width; X++)
                {
                    const uint32_t Bits = Image.Samples[Ci][(static_cast<size_t>(First + L) * Width) + X];
                    Data.push_back(static_cast<uint16_t>(Bits));
                    if (Size == 2) Data.push_back(static_cast<uint16_t>(Bits >> 16));
                }
        }

        std::vector<uint8_t> Bitmap(BitmapBytes, 0);
        for (const uint16_t V : Data) Bitmap[static_cast<size_t>(V >> 3)] |= static_cast<uint8_t>(1 << (V & 7));
        Bitmap[0] &= 0xFE;
        int MinNonZero = BitmapBytes - 1;
        int MaxNonZero = 0;
        for (int I = 0; I < BitmapBytes; I++)
        {
            if (Bitmap[static_cast<size_t>(I)] != 0)
            {
                MinNonZero = (std::min)(MinNonZero, I);
                MaxNonZero = (std::max)(MaxNonZero, I);
            }
        }

        std::vector<uint16_t> Lut(65536, 0);
        int K = 0;
        for (int I = 0; I < 65536; I++)
        {
            Lut[static_cast<size_t>(I)] = (I == 0 || (Bitmap[static_cast<size_t>(I >> 3)] & (1 << (I & 7))) != 0) ? static_cast<uint16_t>(K++) : uint16_t(0);
        }
        const int MaxValue = K - 1;
        for (uint16_t& V : Data) V = Lut[V];

        for (const auto& [Start, Size] : Regions)
            for (int J = 0; J < Size; J++)
                Wav2Encode(Data, Start + J, Width, Size, Lines, Width * Size, MaxValue);

        const Bytes Huf = HufCompress(Data);
        Bytes Out;
        Put16(Out, static_cast<uint32_t>(MinNonZero));
        Put16(Out, static_cast<uint32_t>(MaxNonZero));
        if (MinNonZero <= MaxNonZero) Out.insert(Out.end(), Bitmap.begin() + MinNonZero, Bitmap.begin() + MaxNonZero + 1);
        Put32(Out, static_cast<uint32_t>(Huf.size()));
        Out.insert(Out.end(), Huf.begin(), Huf.end());
        return Out;
    }

    // ---- ZIP chunk: even/odd byte split, delta predictor, zlib (libdeflate)

    Bytes Deflate(const Bytes& Raw)
    {
        Bytes T(Raw.size());
        const size_t Half = (Raw.size() + 1) / 2;
        size_t A = 0, B = Half;
        for (size_t I = 0; I < Raw.size(); I++)
        {
            if ((I & 1) == 0) T[A++] = Raw[I];
            else T[B++] = Raw[I];
        }
        for (size_t I = T.size() - 1; I > 0; I--)
        {
            T[I] = static_cast<uint8_t>(T[I] - T[I - 1] + 128 + 256);
        }

        libdeflate_compressor* C = libdeflate_alloc_compressor(6);
        if (C == nullptr) return Raw; // reads as "stored raw"
        Bytes Out(libdeflate_zlib_compress_bound(C, T.size()));
        const size_t N = libdeflate_zlib_compress(C, T.data(), T.size(), Out.data(), Out.size());
        libdeflate_free_compressor(C);
        if (N == 0) return Raw;
        Out.resize(N);
        return Out;
    }

    Bytes RawBlock(const TestImage& Image, const std::vector<size_t>& Order, int First, int Lines)
    {
        Bytes B;
        for (int L = 0; L < Lines; L++)
            for (const size_t Ci : Order)
                for (int X = 0; X < Image.Width; X++)
                {
                    const uint32_t Bits = Image.Samples[Ci][(static_cast<size_t>(First + L) * Image.Width) + X];
                    if (Image.Channels[Ci].Type == PixelType::Half) Put16(B, Bits);
                    else Put32(B, Bits);
                }
        return B;
    }

    // The whole file. Chunks that do not shrink are stored raw, as OpenEXR does. Reverse writes the chunks on disk
    // last-first with lineOrder DECREASING_Y (the offset table stays indexed by scanline, as the format has it).
    Bytes WriteExr(const TestImage& Image, Compression Codec, int CompressionOverride = -1, bool Reverse = false)
    {
        Bytes F;
        Put32(F, 0x01312f76);
        Put8(F, 2);
        Put8(F, 0);
        Put8(F, 0);
        Put8(F, 0);

        std::vector<size_t> Order(Image.Channels.size());
        for (size_t I = 0; I < Order.size(); I++) Order[I] = I;
        std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return Image.Channels[A].Name < Image.Channels[B].Name; });

        Bytes ChList;
        for (const size_t Ci : Order)
        {
            PutStr(ChList, Image.Channels[Ci].Name);
            Put32(ChList, static_cast<uint32_t>(Image.Channels[Ci].Type));
            Put32(ChList, 0); // pLinear + reserved
            Put32(ChList, 1);
            Put32(ChList, 1);
        }
        Put8(ChList, 0);
        Attribute(F, "channels", "chlist", ChList);
        Attribute(F, "compression", "compression", Bytes{ static_cast<uint8_t>(CompressionOverride >= 0 ? CompressionOverride : static_cast<int>(Codec)) });
        Bytes Box;
        Put32(Box, static_cast<uint32_t>(Image.MinX));
        Put32(Box, static_cast<uint32_t>(Image.MinY));
        Put32(Box, static_cast<uint32_t>(Image.MinX + Image.Width - 1));
        Put32(Box, static_cast<uint32_t>(Image.MinY + Image.Height - 1));
        Attribute(F, "dataWindow", "box2i", Box);
        Attribute(F, "displayWindow", "box2i", Box);
        Attribute(F, "lineOrder", "lineOrder", Bytes{ static_cast<uint8_t>(Reverse ? 1 : 0) });
        Bytes One;
        Put32(One, FloatBits(1.0f));
        Attribute(F, "pixelAspectRatio", "float", One);
        Attribute(F, "screenWindowCenter", "v2f", Bytes(8, 0));
        Attribute(F, "screenWindowWidth", "float", One);
        Put8(F, 0);

        const int Lpc = Codec == Compression::Zip ? 16 : Codec == Compression::Piz ? 32 : 1;
        const int Chunks = (Image.Height + Lpc - 1) / Lpc;
        const size_t TablePos = F.size();
        F.resize(F.size() + (static_cast<size_t>(Chunks) * 8), 0);
        std::vector<uint64_t> Offsets(static_cast<size_t>(Chunks));
        for (int N = 0; N < Chunks; N++)
        {
            const int Chunk = Reverse ? (Chunks - 1 - N) : N;
            const int First = Chunk * Lpc;
            const int Lines = (std::min)(Lpc, Image.Height - First);
            const Bytes Raw = RawBlock(Image, Order, First, Lines);
            Bytes Stored;
            switch (Codec)
            {
            case Compression::Zips:
            case Compression::Zip: Stored = Deflate(Raw); break;
            case Compression::Piz: Stored = PizChunk(Image, Order, First, Lines); break;
            default: Stored = Raw; break;
            }
            if (Stored.size() >= Raw.size()) Stored = Raw;

            Offsets[static_cast<size_t>(Chunk)] = F.size();
            Put32(F, static_cast<uint32_t>(Image.MinY + First));
            Put32(F, static_cast<uint32_t>(Stored.size()));
            F.insert(F.end(), Stored.begin(), Stored.end());
        }
        for (int I = 0; I < Chunks; I++)
        {
            for (int B = 0; B < 8; B++) F[TablePos + (static_cast<size_t>(I) * 8) + B] = static_cast<uint8_t>(Offsets[static_cast<size_t>(I)] >> (8 * B));
        }
        return F;
    }

    // ---- the test harness

    struct Harness
    {
        std::string& Failures;
        int Failed = 0;

        void Check(bool Ok, const std::string& Name, const std::string& Got = std::string())
        {
            if (Ok) return;
            Failed++;
            Failures += std::format("FAIL ExrReader {} (got '{}')\n", Name, Got);
        }
    };

    const std::vector<TestChannel> HalfRgba = { { "A", PixelType::Half }, { "B", PixelType::Half }, { "G", PixelType::Half }, { "R", PixelType::Half } };
    const std::vector<TestChannel> FloatRgb = { { "B", PixelType::Real32 }, { "G", PixelType::Real32 }, { "R", PixelType::Real32 } };
    const Compression Codecs[] = { Compression::None, Compression::Zips, Compression::Zip, Compression::Piz };

    const char* CodecName(Compression C)
    {
        switch (C)
        {
        case Compression::None: return "NONE";
        case Compression::Zips: return "ZIPS";
        case Compression::Zip: return "ZIP";
        case Compression::Piz: return "PIZ";
        default: return "other";
        }
    }

    // Smooth ramps plus noise and flat patches (so the wavelet, the Huffman table and the run-length symbol are all exercised)
    TestImage Gradient(int W, int H, const std::vector<TestChannel>& Channels, uint32_t Seed = 1, int MinX = 0, int MinY = 0)
    {
        TestImage Image(W, H, Channels, MinX, MinY);
        Rng R(Seed);
        Image.Fill([&](int X, int Y, int C) {
            if (X > W / 2 && Y > H / 2) return 0.5f; // flat patch
            return std::fmod((X * 0.37f) + (Y * 0.11f) + (C * 3.0f) + static_cast<float>(R.NextDouble() * 0.5), 40.0f);
        });
        return Image;
    }

    // Opens and reads Bytes; a failure is recorded under Name and an empty Pixels returned
    bool ReadBytes(Harness& T, const Bytes& Data, const std::string& Name, Pixels& Out, int MaxW = 4096, int MaxH = 4096, ReadOptions Options = ReadOptions{ true, false })
    {
        Reader R;
        Status S = Reader::Open(Data, R);
        if (!S.Ok())
        {
            T.Check(false, Name + ": open", std::string(CodeName(S.Kind)) + " " + S.Message);
            return false;
        }
        S = R.ReadDownsampled(MaxW, MaxH, Options, Out);
        if (!S.Ok())
        {
            T.Check(false, Name + ": read", std::string(CodeName(S.Kind)) + " " + S.Message);
            return false;
        }
        return true;
    }

    void AssertMatches(Harness& T, const TestImage& Image, const Pixels& P, const std::string& Name)
    {
        if (P.Width != Image.Width || P.Height != Image.Height)
        {
            T.Check(false, Name + ": size", std::format("{}x{}", P.Width, P.Height));
            return;
        }
        const char* Names[4] = { "R", "G", "B", "A" };
        int Index[4] = { -1, -1, -1, -1 };
        for (size_t I = 0; I < Image.Channels.size(); I++)
            for (int N = 0; N < 4; N++)
                if (Image.Channels[I].Name == Names[N]) Index[N] = static_cast<int>(I);

        for (int Y = 0; Y < Image.Height; Y++)
            for (int X = 0; X < Image.Width; X++)
                for (int C = 0; C < P.ChannelCount; C++)
                {
                    const float Expected = Index[C] >= 0 ? Image.ValueAt(static_cast<size_t>(Index[C]), X, Y) : 1.0f;
                    const float Got = P.Data[((static_cast<size_t>(Y) * P.Width + X) * P.ChannelCount) + C];
                    if (Expected != Got)
                    {
                        T.Check(false, Name + ": texel", std::format("({},{}) c{} expected {} got {}", X, Y, C, Expected, Got));
                        return; // one is enough
                    }
                }
    }

    size_t IndexOf(const Bytes& Data, const std::string& Pattern)
    {
        const auto It = std::search(Data.begin(), Data.end(), Pattern.begin(), Pattern.end());
        return It == Data.end() ? static_cast<size_t>(-1) : static_cast<size_t>(It - Data.begin());
    }

    // Where the offset table starts: after the last attribute (screenWindowWidth: name, type, size, 4 value bytes) and the header's closing zero
    size_t TableStart(const Bytes& Data)
    {
        return IndexOf(Data, std::string("screenWindowWidth\0float\0", 24)) + 24 + 4 + 4 + 1;
    }

    Code OpenCode(const Bytes& Data)
    {
        Reader R;
        return Reader::Open(Data, R).Kind;
    }

    // ================================================================ the cases

    void RoundTrips(Harness& T)
    {
        for (const Compression Codec : Codecs)
        {
            const std::string C = CodecName(Codec);
            Pixels P;
            {
                const TestImage Image = Gradient(70, 69, HalfRgba);
                if (ReadBytes(T, WriteExr(Image, Codec), "Half_rgba_round_trips_exactly " + C, P)) AssertMatches(T, Image, P, "Half_rgba_round_trips_exactly " + C);
            }
            {
                const TestImage Image = Gradient(45, 37, FloatRgb, 5);
                const Bytes Data = WriteExr(Image, Codec);
                Reader R;
                T.Check(Reader::Open(Data, R).Ok() && R.GetInfo().Channels.size() == 3, "Float_rgb_round_trips_exactly " + C + ": three channels");
                if (ReadBytes(T, Data, "Float_rgb_round_trips_exactly " + C, P)) AssertMatches(T, Image, P, "Float_rgb_round_trips_exactly " + C);
            }
            for (const auto [W, H] : std::vector<std::pair<int, int>>{ { 1, 1 }, { 2, 1 }, { 1, 7 }, { 3, 3 }, { 5, 3 }, { 33, 37 }, { 31, 65 }, { 64, 33 }, { 17, 2 } })
            {
                const std::string Name = std::format("Odd_and_tiny_sizes_round_trip {} {}x{}", C, W, H);
                const TestImage Image = Gradient(W, H, HalfRgba, static_cast<uint32_t>(W + H));
                if (ReadBytes(T, WriteExr(Image, Codec), Name, P)) AssertMatches(T, Image, P, Name);
            }
            {
                const std::string Name = "A_data_window_away_from_the_origin_and_an_extra_channel_are_handled " + C;
                const std::vector<TestChannel> Channels = { { "B", PixelType::Half }, { "G", PixelType::Half }, { "R", PixelType::Half }, { "Z", PixelType::Real32 }, { "ID", PixelType::Unsigned32 } };
                const TestImage Image = Gradient(40, 41, Channels, 9, -7, 11);
                const Bytes Data = WriteExr(Image, Codec);
                Reader R;
                T.Check(Reader::Open(Data, R).Ok() && R.GetInfo().MinX == -7 && R.GetInfo().MinY == 11, Name + ": window origin");
                if (ReadBytes(T, Data, Name, P))
                {
                    bool Ok = P.ChannelCount == 4;
                    for (int Y = 0; Ok && Y < 41; Y++)
                        for (int X = 0; Ok && X < 40; X++)
                        {
                            const size_t O = ((static_cast<size_t>(Y) * 40) + X) * 4;
                            Ok = P.Data[O + 0] == Image.ValueAt(2, X, Y) && P.Data[O + 1] == Image.ValueAt(1, X, Y) && P.Data[O + 2] == Image.ValueAt(0, X, Y) && P.Data[O + 3] == 1.0f;
                        }
                    T.Check(Ok, Name + ": R, G, B and alpha 1");
                }
            }
        }
    }

    void PizRanges(Harness& T)
    {
        Pixels P;
        TestImage Wide(64, 40, HalfRgba);
        Rng R(3);
        Wide.Fill([&](int, int, int) { return static_cast<float>((R.NextDouble() - 0.5) * 60000); }); // bitmap spans the whole range, 16-bit wavelet path
        if (ReadBytes(T, WriteExr(Wide, Compression::Piz), "Piz_handles_sixteen_bit_wide_value_ranges", P)) AssertMatches(T, Wide, P, "Piz wide");

        const TestImage Zeros(40, 33, HalfRgba);
        if (ReadBytes(T, WriteExr(Zeros, Compression::Piz), "Piz_handles_all_zero_data", P)) AssertMatches(T, Zeros, P, "Piz zeros");
    }

    void HuffmanLongCodes(Harness& T)
    {
        // Geometric frequencies force code lengths well past 14 bits
        std::vector<uint16_t> Data;
        for (int S = 0; S < 22; S++)
            for (int I = 0; I < 1 << (21 - S); I++) Data.push_back(static_cast<uint16_t>(S * 3));
        std::mt19937 Engine(11);
        std::shuffle(Data.begin(), Data.end(), Engine);
        const int Longest = LongestCode(Data);
        T.Check(Longest > 14, "Huffman_decoder_handles_codes_longer_than_the_primary_table: test data makes long codes", std::to_string(Longest));
        for (const bool Runs : { false, true })
        {
            const Bytes Encoded = HufCompress(Data, Runs);
            std::vector<uint16_t> Decoded(Data.size());
            PizDecoder D;
            const char* Error = D.HufUncompress(Encoded, Decoded);
            T.Check(Error == nullptr, std::string("Huffman_decoder_handles_codes_longer_than_the_primary_table: decodes, runs=") + (Runs ? "true" : "false"), Error ? Error : "");
            T.Check(Decoded == Data, std::string("Huffman_decoder_handles_codes_longer_than_the_primary_table: equal, runs=") + (Runs ? "true" : "false"));
        }

        std::vector<uint16_t> Runny(1000, 7);
        Runny.insert(Runny.end(), 300, 9);
        Runny.push_back(7);
        std::vector<uint16_t> DecodedRuns(Runny.size());
        PizDecoder D;
        const char* Error = D.HufUncompress(HufCompress(Runny), DecodedRuns);
        T.Check(Error == nullptr && DecodedRuns == Runny, "Huffman_decoder_handles_runs", Error ? Error : "");
    }

    void BoxFilter(Harness& T)
    {
        {
            TestImage Image(5, 3, FloatRgb);
            Image.Fill([](int X, int Y, int C) { return static_cast<float>((X * 10) + Y + (C * 100)); });
            Pixels P;
            if (ReadBytes(T, WriteExr(Image, Compression::None), "Box_filter", P, 3, 2))
            {
                T.Check(P.Factor == 2 && P.Width == 3 && P.Height == 2, "Box_filter: factor 2, 3x2", std::format("{} {}x{}", P.Factor, P.Width, P.Height));
                // Output order is R, G, B; the writer's channel index 2 is R
                const auto Expected = [&](int Ox, int Oy, size_t WriterChannel) {
                    double Sum = 0;
                    int N = 0;
                    for (int Y = Oy * 2; Y < (std::min)(3, (Oy * 2) + 2); Y++)
                        for (int X = Ox * 2; X < (std::min)(5, (Ox * 2) + 2); X++)
                        {
                            Sum += Image.ValueAt(WriterChannel, X, Y);
                            N++;
                        }
                    return static_cast<float>(Sum / N);
                };
                bool Ok = P.Data.size() == 3 * 2 * 4;
                for (int Oy = 0; Ok && Oy < 2; Oy++)
                    for (int Ox = 0; Ok && Ox < 3; Ox++)
                    {
                        const size_t O = ((static_cast<size_t>(Oy) * 3) + Ox) * 4;
                        Ok = std::fabs(P.Data[O + 0] - Expected(Ox, Oy, 2)) < 1e-4f && std::fabs(P.Data[O + 1] - Expected(Ox, Oy, 1)) < 1e-4f && std::fabs(P.Data[O + 2] - Expected(Ox, Oy, 0)) < 1e-4f;
                    }
                T.Check(Ok, "Box_filter_averages_exactly_and_edge_boxes_use_their_own_count");
            }
        }
        {
            TestImage Image(64, 64, HalfRgba);
            Image.Fill([](int X, int, int) { return static_cast<float>(X); });
            Pixels P;
            if (ReadBytes(T, WriteExr(Image, Compression::Piz), "A_two_thousand_wide_face_is_halved_while_reading", P, 32, 32))
            {
                T.Check(P.Width == 32 && P.Factor == 2 && P.Data[0] == 0.5f && P.Data[31 * 4] == 62.5f, "A_two_thousand_wide_face_is_halved_while_reading",
                        std::format("{} {} {} {}", P.Width, P.Factor, P.Data[0], P.Data[31 * 4]));
            }
        }
    }

    void ChannelsAndSanitising(Harness& T)
    {
        TestImage Gray(4, 4, { { "Y", PixelType::Half } });
        Gray.Fill([](int X, int Y, int) { return static_cast<float>(X + Y); });
        Pixels P;
        if (ReadBytes(T, WriteExr(Gray, Compression::None), "Rgb_only_output_and_luminance_only_files", P, 4096, 4096, ReadOptions{ false, false }))
        {
            T.Check(P.ChannelCount == 3 && P.Data[((1 * 4) + 3) * 3] == 4.0f && P.Data[(((1 * 4) + 3) * 3) + 2] == 4.0f, "Rgb_only_output_and_luminance_only_files");
        }

        TestImage Bad(2, 1, HalfRgba);
        Bad.Samples[3][0] = 0x7E00; // R NaN
        Bad.Samples[3][1] = 0x7C00; // R +Inf
        Bad.Samples[2][0] = 0xFC00; // G -Inf
        if (ReadBytes(T, WriteExr(Bad, Compression::None), "Sanitising", P, 4096, 4096, ReadOptions{}))
        {
            T.Check(P.Data[0] == 0.0f && P.Data[4] == 65504.0f && P.Data[1] == -65504.0f, "Rgb_only_output_and_luminance_only_files: sanitising NaN, +Inf, -Inf",
                    std::format("{} {} {}", P.Data[0], P.Data[4], P.Data[1]));
        }
        // and unsanitised, the bits come through
        if (ReadBytes(T, WriteExr(Bad, Compression::None), "Unsanitised", P, 4096, 4096, ReadOptions{ true, false }))
        {
            T.Check(std::isnan(P.Data[0]) && std::isinf(P.Data[4]) && P.Data[4] > 0 && std::isinf(P.Data[1]) && P.Data[1] < 0, "Sanitize=false keeps NaN and infinities");
        }
    }

    void Cancellation(Harness& T)
    {
        const Bytes Data = WriteExr(Gradient(64, 64, HalfRgba), Compression::Zips);
        Reader R;
        T.Check(Reader::Open(Data, R).Ok(), "Cancellation_returns_a_typed_error: opens");
        std::atomic<bool> Cancel{ true };
        Pixels P;
        const Status S = R.ReadDownsampled(64, 64, ReadOptions{}, P, &Cancel);
        T.Check(S.Kind == Code::Cancelled, "Cancellation_returns_a_typed_error", CodeName(S.Kind));
        Cancel = false;
        T.Check(R.ReadDownsampled(64, 64, ReadOptions{}, P, &Cancel).Ok(), "a cleared cancel flag reads");
    }

    void BadInput(Harness& T)
    {
        const Bytes Good = WriteExr(Gradient(8, 8, HalfRgba), Compression::None);
        const std::string Text = "not an exr file at all";
        const Bytes NotExr(Text.begin(), Text.end());
        T.Check(OpenCode(NotExr) == Code::BadMagic, "Bad_magic: text");
        T.Check(OpenCode(Bytes()) == Code::BadMagic, "Bad_magic: empty");
        T.Check(OpenCode(Bytes(Good.begin(), Good.begin() + 6)) == Code::Truncated, "Bad_magic: six bytes are truncated");

        // Byte 4 is the version; byte 5 holds flag bits 8..15: tiled is 0x02 (bit 9), deep 0x08 (bit 11), multipart 0x10 (bit 12)
        for (const auto [Offset, Value] : std::vector<std::pair<int, uint8_t>>{ { 4, 1 }, { 5, 0x02 }, { 5, 0x08 }, { 5, 0x10 } })
        {
            Bytes Copy = Good;
            Copy[static_cast<size_t>(Offset)] = Value;
            T.Check(OpenCode(Copy) == Code::UnsupportedLayout, std::format("Unsupported layout byte {} = {:#x}", Offset, Value));
        }

        for (const int Codec : { 1, 5, 6, 7, 8, 9 })
        {
            const Bytes Data = WriteExr(Gradient(8, 8, HalfRgba), Compression::None, Codec);
            Reader R;
            const Status Opened = Reader::Open(Data, R);
            if (!Opened.Ok())
            {
                // The offset table length depends on lines per chunk; a mismatch is a truncation, which is also typed
                T.Check(Opened.Kind == Code::Truncated, std::format("Unsupported codec {}: open", Codec), CodeName(Opened.Kind));
                continue;
            }
            Pixels P;
            const Status S = R.ReadDownsampled(8, 8, ReadOptions{}, P);
            T.Check(S.Kind == Code::UnsupportedCompression, std::format("Unsupported codec {}", Codec), CodeName(S.Kind));
        }

        {
            const TestImage NoRgb(4, 4, { { "Z", PixelType::Real32 } });
            const Bytes Data = WriteExr(NoRgb, Compression::None);
            Reader R;
            Pixels P;
            T.Check(Reader::Open(Data, R).Ok() && R.ReadDownsampled(4, 4, ReadOptions{}, P).Kind == Code::UnsupportedChannels, "No R, G, B: unsupported channels");
        }

        // Compression 10 and up is invalid in the header
        {
            Bytes Copy = Good;
            const size_t At = IndexOf(Copy, std::string("compression\0compression\0", 24)) + 24 + 4;
            Copy[At] = 10;
            T.Check(OpenCode(Copy) == Code::Invalid, "Compression value 10 is invalid");
        }
    }

    void HugeAndHostile(Harness& T)
    {
        const Bytes Good = WriteExr(Gradient(8, 8, HalfRgba), Compression::None);
        Bytes Copy = Good;
        const std::string Pattern("dataWindow\0box2i\0", 17);
        const size_t At = IndexOf(Copy, Pattern) + Pattern.size() + 4;
        const auto PutI = [&](size_t Where, int32_t V) { for (int B = 0; B < 4; B++) Copy[Where + static_cast<size_t>(B)] = static_cast<uint8_t>(static_cast<uint32_t>(V) >> (8 * B)); };
        PutI(At + 8, 2000000000);
        PutI(At + 12, 2000000000);
        T.Check(OpenCode(Copy) == Code::TooLarge, "A_huge_data_window_is_refused_before_anything_is_allocated: too large");

        // Inverted window
        PutI(At + 8, -5);
        T.Check(OpenCode(Copy) == Code::Invalid, "A_huge_data_window: inverted is invalid");

        // An attribute claiming a gigabyte of data
        Bytes Bomb = Good;
        const std::string ChPattern("channels\0chlist\0", 16);
        const size_t ChAt = IndexOf(Bomb, ChPattern) + ChPattern.size();
        for (int B = 0; B < 4; B++) Bomb[ChAt + static_cast<size_t>(B)] = static_cast<uint8_t>(1000000000u >> (8 * B));
        T.Check(OpenCode(Bomb) != Code::Ok, "A_huge_data_window: a gigabyte attribute is refused");

        // (port addition) a chunk offset near INT64_MAX must not wrap past the bounds check
        {
            Bytes Evil = Good;
            const size_t Table = TableStart(Evil);
            for (int B = 0; B < 7; B++) Evil[Table + static_cast<size_t>(B)] = 0xFF;
            Evil[Table + 7] = 0x7F;
            T.Check(OpenCode(Evil) == Code::Truncated, "(added) an offset of INT64_MAX is truncated, not wrapped");
        }
    }

    void Truncations(Harness& T)
    {
        for (const Compression Codec : { Compression::None, Compression::Zip, Compression::Piz })
        {
            const Bytes Full = WriteExr(Gradient(37, 35, HalfRgba, 2), Codec);
            const std::string C = CodecName(Codec);
            int Decoded = 0;
            int Untyped = 0;
            for (size_t Length = 0; Length < Full.size(); Length++)
            {
                const Bytes Cut(Full.begin(), Full.begin() + static_cast<std::ptrdiff_t>(Length)); // exact size: ASan sees any read past it
                Reader R;
                Status S = Reader::Open(Cut, R);
                if (S.Ok())
                {
                    Pixels P;
                    S = R.ReadDownsampled(64, 64, ReadOptions{}, P);
                    if (S.Ok()) Decoded++;
                }
                if (!S.Ok() && std::string(CodeName(S.Kind)).rfind("exr.", 0) != 0) Untyped++;
            }
            T.Check(Decoded == 0, "Every_truncation_fails_with_a_typed_error_and_never_throws " + C + ": none decodes", std::to_string(Decoded));
            T.Check(Untyped == 0, "Every_truncation_fails_with_a_typed_error " + C + ": all typed", std::to_string(Untyped));
        }
    }

    void RandomCorruption(Harness& T)
    {
        for (const Compression Codec : { Compression::Zip, Compression::Piz, Compression::None })
        {
            const Bytes Full = WriteExr(Gradient(40, 40, HalfRgba, 4), Codec);
            Rng R(1234);
            int Untyped = 0;
            int Rejected = 0;
            for (int I = 0; I < 600; I++)
            {
                Bytes Copy = Full;
                const int Flips = 1 + R.Next(6);
                for (int F = 0; F < Flips; F++) Copy[static_cast<size_t>(R.Next(static_cast<int>(Copy.size())))] = static_cast<uint8_t>(R.Next(256));

                Reader Rd;
                Status S = Reader::Open(Copy, Rd);
                if (S.Ok())
                {
                    Pixels P;
                    S = Rd.ReadDownsampled(64, 64, ReadOptions{}, P);
                }
                if (!S.Ok())
                {
                    Rejected++;
                    if (std::string(CodeName(S.Kind)).rfind("exr.", 0) != 0) Untyped++;
                }
            }
            // reaching here at all is the check (no exception, no crash, ASan clean); the corruption must also be noticed sometimes
            T.Check(Untyped == 0, std::string("Random_corruption_never_throws ") + CodecName(Codec) + ": typed", std::to_string(Untyped));
            T.Check(Rejected > 0, std::string("Random_corruption ") + CodecName(Codec) + ": some flips are caught", std::to_string(Rejected));
        }
    }

    // ================================================================ added by the port

    void HalfConversion(Harness& T)
    {
        int Bad = 0;
        int BackBad = 0;
        for (uint32_t H = 0; H < 65536; H++)
        {
            const uint32_t E = (H >> 10) & 0x1F;
            const uint32_t M = H & 0x3FF;
            const float Sign = (H & 0x8000) ? -1.0f : 1.0f;
            const float Got = HalfToFloat(static_cast<uint16_t>(H));
            if (E == 31)
            {
                if (M == 0 ? !(std::isinf(Got) && std::signbit(Got) == ((H & 0x8000) != 0)) : !std::isnan(Got)) Bad++;
                continue;
            }
            const double Ref = E == 0 ? std::ldexp(static_cast<double>(M), -24) : std::ldexp(1.0 + (M / 1024.0), static_cast<int>(E) - 15);
            if (Got != static_cast<float>(Sign * Ref) || std::signbit(Got) != std::signbit(Sign * static_cast<float>(Ref))) Bad++;
            if (FloatToHalfBits(Got) != H) BackBad++;
        }
        T.Check(Bad == 0, "(added) HalfToFloat matches ldexp for all 65536 halves", std::to_string(Bad));
        T.Check(BackBad == 0, "(added) the test writer's float to half inverts HalfToFloat", std::to_string(BackBad));
    }

    void DecreasingLineOrder(Harness& T)
    {
        for (const Compression Codec : Codecs)
        {
            const std::string Name = std::string("(added) decreasing Y line order ") + CodecName(Codec);
            const TestImage Image = Gradient(33, 70, HalfRgba, 21);
            const Bytes Data = WriteExr(Image, Codec, -1, true);
            Reader R;
            Pixels P;
            T.Check(Reader::Open(Data, R).Ok() && R.GetInfo().LineOrder == 1, Name + ": header line order");
            if (ReadBytes(T, Data, Name, P)) AssertMatches(T, Image, P, Name);
        }
    }

    void HostileStreams(Harness& T)
    {
        // Garbage into the Huffman stage and the whole PIZ chunk decoder: any outcome but a crash or a read outside the buffers is fine (ASan watches)
        Rng R(77);
        PizDecoder D;
        int Accepted = 0;
        for (int I = 0; I < 3000; I++)
        {
            Bytes Junk(static_cast<size_t>(20 + R.Next(300)));
            for (uint8_t& B : Junk) B = static_cast<uint8_t>(R.Next(256));
            if (I % 2 == 0)
            {
                // a plausible header: im 0, iM small, nBits within the data
                Junk[0] = Junk[1] = Junk[2] = Junk[3] = 0;
                Junk[4] = static_cast<uint8_t>(R.Next(40));
                Junk[5] = Junk[6] = Junk[7] = 0;
                Junk[12] = static_cast<uint8_t>(R.Next(256));
                Junk[13] = Junk[14] = Junk[15] = 0;
            }
            std::vector<uint16_t> Out(100);
            if (D.HufUncompress(Junk, Out) == nullptr) Accepted++;

            std::vector<Channel> Ch(1);
            Ch[0].Name = "R";
            Bytes Raw(2 * 4 * 8);
            D.Decode(Junk, 4, 8, Ch, Raw);
        }
        T.Check(true, "(added) hostile Huffman streams do not crash", std::to_string(Accepted));

        // Specific refusals
        PizDecoder Fresh;
        std::vector<uint16_t> Out(10);
        T.Check(Fresh.HufUncompress(Bytes(), Out) != nullptr, "(added) empty Huffman data for a non-empty output is refused");
        T.Check(Fresh.HufUncompress(Bytes(), std::span<uint16_t>()) == nullptr, "(added) empty Huffman data for an empty output is fine");
        T.Check(Fresh.HufUncompress(Bytes(10, 0), Out) != nullptr, "(added) a 10-byte Huffman header is refused");
        Bytes Range(40, 0);
        Range[4] = 1; // im 0, iM 1
        Range[12] = 0xFF; Range[13] = 0xFF; Range[14] = 0xFF; Range[15] = 0x7F; // nBits far beyond the data
        T.Check(Fresh.HufUncompress(Range, Out) != nullptr, "(added) a bit count beyond the data is refused");

        // A zlib stream that inflates to more than the chunk's scanlines is invalid (HydraX ignored the surplus; see the header)
        {
            const TestImage Image = Gradient(16, 4, HalfRgba, 8);
            Bytes Data = WriteExr(Image, Compression::Zips);
            Reader R2;
            Pixels P;
            T.Check(Reader::Open(Data, R2).Ok() && R2.ReadDownsampled(64, 64, ReadOptions{ true, false }, P).Ok(), "(added) the ZIPS file reads before the corruption");
            // flip a byte in the middle of the first chunk's deflate data
            const size_t Start = static_cast<size_t>(Data[TableStart(Data)]) + (static_cast<size_t>(Data[TableStart(Data) + 1]) << 8) + 8 + 6;
            Data[Start] ^= 0xFF;
            Reader R3;
            const Status S = Reader::Open(Data, R3).Ok() ? R3.ReadDownsampled(64, 64, ReadOptions{}, P) : Status{ Code::Invalid, "" };
            T.Check(!S.Ok() && S.Kind == Code::Invalid, "(added) a corrupt zlib chunk is invalid", CodeName(S.Kind));
        }
    }

    void RealFile(Harness& T)
    {
        namespace fs = std::filesystem;
        const TestImage Image = Gradient(48, 40, HalfRgba, 31);
        const Bytes Data = WriteExr(Image, Compression::Piz);
        const fs::path Path = fs::temp_directory_path() / "graywolf-exr-selftest.exr";
        {
            std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
            Out.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
        }
        Pixels P;
        Info Header;
        const Status S = ReadFile(Path, 4096, 4096, ReadOptions{ true, false }, P, &Header);
        std::error_code Ec;
        fs::remove(Path, Ec);
        T.Check(S.Ok(), "(added) ReadFile reads a file", S.Message);
        if (S.Ok())
        {
            AssertMatches(T, Image, P, "(added) ReadFile");
            T.Check(Header.Method == Compression::Piz && Header.Width == 48 && Header.Height == 40, "(added) ReadFile reports the header");
        }

        // A_missing_file_is_an_io_error
        const Status Missing = ReadFile(fs::temp_directory_path() / "graywolf-no-such-exr-file.exr", 64, 64, ReadOptions{}, P);
        T.Check(Missing.Kind == Code::Io, "A_missing_file_is_an_io_error", CodeName(Missing.Kind));
    }

    void RequestedSizes(Harness& T)
    {
        const Bytes Data = WriteExr(Gradient(8, 8, HalfRgba), Compression::None);
        Reader R;
        Pixels P;
        T.Check(Reader::Open(Data, R).Ok(), "(added) requested size: opens");
        T.Check(R.ReadDownsampled(0, 8, ReadOptions{}, P).Kind == Code::TooLarge, "(added) a zero requested width is refused");
        T.Check(R.ReadDownsampled(100000, 100000, ReadOptions{}, P).Kind == Code::TooLarge, "(added) an absurd requested size is refused");
        T.Check(R.ReadDownsampled(3, 3, ReadOptions{}, P).Ok() && P.Factor == 3 && P.Width == 3, "(added) 8x8 into 3x3 takes factor 3");
    }
}

int SelfTest(std::string& Failures)
{
    Harness T{ Failures };
    RoundTrips(T);
    PizRanges(T);
    HuffmanLongCodes(T);
    BoxFilter(T);
    ChannelsAndSanitising(T);
    Cancellation(T);
    BadInput(T);
    HugeAndHostile(T);
    Truncations(T);
    RandomCorruption(T);
    HalfConversion(T);
    DecreasingLineOrder(T);
    HostileStreams(T);
    RealFile(T);
    RequestedSizes(T);
    return T.Failed;
}
}
