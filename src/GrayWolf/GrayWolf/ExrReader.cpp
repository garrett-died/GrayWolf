#include "stdafx.h"

#include "ExrReader.h"

#include "ZipInflate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

// Ported from HydraX.Formats/Exr/ExrReader.cs and PizDecoder.cs; see ExrReader.h for what differs. Every byte read goes
// through a span (ByteView) and an explicit bound, never raw pointer arithmetic (tools/safety_gate.py).
namespace Exr
{
namespace
{
    constexpr uint32_t Magic = 0x01312f76;
    constexpr uint32_t FlagTiled = 0x200;
    constexpr uint32_t FlagDeep = 0x800;
    constexpr uint32_t FlagMultipart = 0x1000;

    // Little-endian reads at At; the caller has checked D.size() >= At + width
    uint16_t U16(ByteView D, size_t At) { return static_cast<uint16_t>(D[At] | (D[At + 1] << 8)); }
    uint32_t U32(ByteView D, size_t At)
    {
        return static_cast<uint32_t>(D[At]) | (static_cast<uint32_t>(D[At + 1]) << 8) | (static_cast<uint32_t>(D[At + 2]) << 16) | (static_cast<uint32_t>(D[At + 3]) << 24);
    }
    int32_t I32(ByteView D, size_t At) { return static_cast<int32_t>(U32(D, At)); }
    int64_t I64(ByteView D, size_t At) { return static_cast<int64_t>(static_cast<uint64_t>(U32(D, At)) | (static_cast<uint64_t>(U32(D, At + 4)) << 32)); }

    Status Fail(Code Kind, std::string Message)
    {
        return Status{ Kind, std::move(Message) };
    }

    // Lines of one chunk: ZIP 16, PIZ 32, every other method 1 (also the ones this reader does not decode, as in HydraX)
    int LinesPerChunk(Compression Method)
    {
        switch (Method)
        {
        case Compression::Zip: return 16;
        case Compression::Piz: return 32;
        default: return 1;
        }
    }

    // The header ended inside the limit-sized window: truncated if the file is that short, else too big a header
    Status HeaderOverrun(int64_t FileLength, const Limits& L)
    {
        return FileLength <= static_cast<int64_t>(L.MaxHeaderBytes) + 16
            ? Fail(Code::Truncated, "The EXR file ends inside the header.")
            : Fail(Code::Invalid, "The EXR header is larger than the reader's limit.");
    }

    // A zero-terminated name of at most MaxLength bytes at P (moved past the terminator)
    bool ReadCString(ByteView Data, size_t& P, size_t MaxLength, std::string& Text)
    {
        const size_t End = (std::min)(Data.size(), P + MaxLength + 1);
        for (size_t I = P; I < End; I++)
        {
            if (Data[I] == 0)
            {
                Text.assign(Data.begin() + static_cast<std::ptrdiff_t>(P), Data.begin() + static_cast<std::ptrdiff_t>(I));
                P = I + 1;
                return true;
            }
        }
        Text.clear();
        return false;
    }

    bool ParseChannels(ByteView Data, int MaxChannels, std::vector<Channel>& List)
    {
        List.clear();
        const size_t Size = Data.size();
        size_t P = 0;
        while (true)
        {
            if (P >= Size) return false;
            if (Data[P] == 0) return !List.empty();

            size_t End = P;
            while (End < Size && Data[End] != 0) End++;
            if (End == Size) return false;
            const size_t Length = End - P;
            if (Length < 1 || Length > 255) return false;

            Channel C;
            C.Name.assign(Data.begin() + static_cast<std::ptrdiff_t>(P), Data.begin() + static_cast<std::ptrdiff_t>(End));
            P = End + 1;
            if (Size - P < 16) return false;

            const int32_t Type = I32(Data, P);
            C.XSampling = I32(Data, P + 8);
            C.YSampling = I32(Data, P + 12);
            P += 16;
            if (Type < 0 || Type > 2 || static_cast<int>(List.size()) >= MaxChannels) return false;
            C.Type = static_cast<PixelType>(Type);
            List.push_back(std::move(C));
        }
    }

    // Adds one row of one channel into the accumulator (the box filter's running sums)
    void AccumulateRow(ByteView Row, PixelType Type, int Width, int Factor, bool Sanitize, std::span<float> Accumulator, size_t BaseIndex, size_t Stride)
    {
        int Count = 0;
        size_t Index = BaseIndex;
        for (int X = 0; X < Width; X++)
        {
            float V;
            switch (Type)
            {
            case PixelType::Half: V = HalfToFloat(U16(Row, static_cast<size_t>(X) * 2)); break;
            case PixelType::Real32:
            {
                const uint32_t Bits = U32(Row, static_cast<size_t>(X) * 4);
                std::memcpy(&V, &Bits, sizeof(V));
                break;
            }
            default: V = static_cast<float>(U32(Row, static_cast<size_t>(X) * 4)); break;
            }

            if (Sanitize)
            {
                V = std::isnan(V) ? 0.0f : (std::clamp)(V, -65504.0f, 65504.0f);
            }

            Accumulator[Index] += V;
            if (++Count == Factor)
            {
                Count = 0;
                Index += Stride;
            }
        }
    }

    // zlib stream, then the predictor, then the even/odd byte split undone: Raw gets exactly Raw.size() bytes. nullptr on success.
    const char* InflateChunk(ByteView Data, std::span<uint8_t> Raw)
    {
        std::vector<uint8_t> Packed(Raw.size());
        size_t Got = 0;
        if (!ZipInflate::Zlib(Data.data(), Data.size(), Packed.data(), Packed.size(), Got))
        {
            return "An EXR deflate chunk is corrupt: the zlib stream does not inflate to the chunk's scanlines.";
        }
        if (Got < Packed.size())
        {
            return "An EXR deflate chunk is shorter than its scanlines.";
        }

        // Undo the predictor, then the even/odd byte split
        for (size_t I = 1; I < Packed.size(); I++)
        {
            Packed[I] = static_cast<uint8_t>(Packed[I - 1] + Packed[I] - 128);
        }

        const size_t Half = (Packed.size() + 1) / 2;
        size_t T1 = 0;
        size_t T2 = Half;
        for (size_t I = 0; I < Raw.size(); I += 2)
        {
            Raw[I] = Packed[T1++];
            if (I + 1 < Raw.size())
            {
                Raw[I + 1] = Packed[T2++];
            }
        }
        return nullptr;
    }

    // One chunk's stored bytes to the uncompressed scanline layout (per line, per channel, little-endian). Another codec is one more case here.
    const char* DecodeChunk(const Info& Header, PizDecoder& Piz, ByteView Data, std::span<uint8_t> Raw, int Width, int Lines)
    {
        if (Header.Method == Compression::None)
        {
            if (Data.size() != Raw.size()) return "An uncompressed EXR chunk has the wrong size.";
            std::copy(Data.begin(), Data.end(), Raw.begin());
            return nullptr;
        }

        if (Data.size() == Raw.size())
        {
            std::copy(Data.begin(), Data.end(), Raw.begin()); // writers store a chunk raw when compression would not shrink it
            return nullptr;
        }

        switch (Header.Method)
        {
        case Compression::Zips:
        case Compression::Zip:
            return InflateChunk(Data, Raw);
        case Compression::Piz:
            return Piz.Decode(Data, Width, Lines, Header.Channels, Raw);
        default:
            return "Unsupported compression.";
        }
    }

    Status ParseHeader(ByteView Data, const Limits& L, Info& OutInfo, std::vector<int64_t>& OutOffsets)
    {
        const int64_t Length = static_cast<int64_t>(Data.size());
        const size_t Want = static_cast<size_t>((std::min)(Length, static_cast<int64_t>(L.MaxHeaderBytes) + 16));
        const ByteView Head = Data.first(Want);

        if (Want < 4 || U32(Head, 0) != Magic)
        {
            return Fail(Code::BadMagic, "Not an OpenEXR file (bad magic number).");
        }
        if (Want < 8)
        {
            return Fail(Code::Truncated, "The EXR file ends inside the version field.");
        }

        const int Version = Head[4];
        const uint32_t Flags = (static_cast<uint32_t>(Head[5]) << 8) | (static_cast<uint32_t>(Head[6]) << 16) | (static_cast<uint32_t>(Head[7]) << 24);
        if (Version != 2)
        {
            return Fail(Code::UnsupportedLayout, "EXR version " + std::to_string(Version) + " is not supported (only version 2).");
        }
        if ((Flags & (FlagTiled | FlagDeep | FlagMultipart)) != 0)
        {
            return Fail(Code::UnsupportedLayout, "Tiled, deep and multipart EXR files are not supported.");
        }

        constexpr size_t MaxName = 255; // HydraX: the long-names flag and its absence both give 255
        size_t P = 8;
        std::vector<Channel> Channels;
        bool HaveChannels = false;
        int Compress = -1;
        int LineOrder = 0;
        int32_t MinX = 0, MinY = 0, MaxX = -1, MaxY = -1;
        bool HaveWindow = false;
        bool Ended = false;
        std::string Name, Type;
        while (P < Want)
        {
            if (!ReadCString(Head, P, MaxName, Name)) return HeaderOverrun(Length, L);
            if (Name.empty())
            {
                Ended = true;
                break;
            }
            if (!ReadCString(Head, P, MaxName, Type)) return HeaderOverrun(Length, L);
            if (Want - P < 4) return HeaderOverrun(Length, L);

            const int32_t Size = I32(Head, P);
            P += 4;
            if (Size < 0 || static_cast<size_t>(Size) > Want - P) return HeaderOverrun(Length, L);

            const ByteView Value = Head.subspan(P, static_cast<size_t>(Size));
            P += static_cast<size_t>(Size);
            if (Name == "channels" && Type == "chlist")
            {
                if (!ParseChannels(Value, L.MaxChannels, Channels))
                {
                    return Fail(Code::Invalid, "The EXR channel list is malformed.");
                }
                HaveChannels = true;
            }
            else if (Name == "compression" && Size == 1)
            {
                Compress = Value[0];
            }
            else if (Name == "lineOrder" && Size == 1)
            {
                LineOrder = Value[0];
            }
            else if (Name == "dataWindow" && Size == 16)
            {
                MinX = I32(Value, 0);
                MinY = I32(Value, 4);
                MaxX = I32(Value, 8);
                MaxY = I32(Value, 12);
                HaveWindow = true;
            }
            else if (Name == "tiles")
            {
                return Fail(Code::UnsupportedLayout, "Tiled EXR files are not supported.");
            }
        }

        if (!Ended) return HeaderOverrun(Length, L);

        if (!HaveChannels || Compress < 0 || !HaveWindow)
        {
            return Fail(Code::Invalid, "The EXR header lacks channels, compression or dataWindow.");
        }
        if (Compress > 9)
        {
            return Fail(Code::Invalid, "The EXR compression value is unknown. (value " + std::to_string(Compress) + ")");
        }

        const int64_t W = static_cast<int64_t>(MaxX) - MinX + 1;
        const int64_t H = static_cast<int64_t>(MaxY) - MinY + 1;
        if (W < 1 || H < 1)
        {
            return Fail(Code::Invalid, "The EXR data window is empty or inverted.");
        }
        if (W > L.MaxDimension || H > L.MaxDimension || W * H > L.MaxPixels)
        {
            return Fail(Code::TooLarge, "The EXR image is larger than the reader's limits. (" + std::to_string(W) + " x " + std::to_string(H) + ")");
        }

        const Compression Method = static_cast<Compression>(Compress);
        const int64_t Lpc = LinesPerChunk(Method);
        const int64_t Chunks = (H + Lpc - 1) / Lpc;
        const int64_t TableStart = static_cast<int64_t>(P);
        const int64_t TableBytes = Chunks * 8;
        if (TableStart + TableBytes > Length)
        {
            return Fail(Code::Truncated, "The EXR file ends inside the offset table.");
        }

        std::vector<int64_t> Offsets(static_cast<size_t>(Chunks));
        const int64_t Floor = TableStart + TableBytes;
        for (int64_t I = 0; I < Chunks; I++)
        {
            const int64_t Offset = I64(Data, static_cast<size_t>(TableStart + (I * 8)));
            // written Offset > Length - 8, not Offset + 8 > Length: a hostile offset near INT64_MAX must not wrap
            if (Offset < Floor || Offset > Length - 8)
            {
                return Fail(Code::Truncated, "An EXR chunk offset is missing or outside the file (incomplete file). (chunk " + std::to_string(I) + ")");
            }
            Offsets[static_cast<size_t>(I)] = Offset;
        }

        OutInfo.Width = static_cast<int>(W);
        OutInfo.Height = static_cast<int>(H);
        OutInfo.MinX = MinX;
        OutInfo.MinY = MinY;
        OutInfo.Method = Method;
        OutInfo.Channels = std::move(Channels);
        OutInfo.LineOrder = LineOrder;
        OutOffsets = std::move(Offsets);
        return Status();
    }
}

const char* CodeName(Code C)
{
    switch (C)
    {
    case Code::Ok: return "ok";
    case Code::BadMagic: return "exr.bad_magic";
    case Code::Truncated: return "exr.truncated";
    case Code::Invalid: return "exr.invalid";
    case Code::TooLarge: return "exr.too_large";
    case Code::UnsupportedLayout: return "exr.unsupported_layout";
    case Code::UnsupportedCompression: return "exr.unsupported_compression";
    case Code::UnsupportedChannels: return "exr.unsupported_channels";
    case Code::Io: return "exr.io";
    case Code::Cancelled: return "exr.cancelled";
    }
    return "exr.invalid";
}

float HalfToFloat(uint16_t H)
{
    const uint32_t Sign = static_cast<uint32_t>(H & 0x8000u) << 16;
    uint32_t E = (H >> 10) & 0x1Fu;
    uint32_t M = H & 0x3FFu;
    uint32_t Bits;
    if (E == 0)
    {
        if (M == 0)
        {
            Bits = Sign;
        }
        else
        {
            // subnormal: M * 2^-24, renormalised
            E = 1;
            while ((M & 0x400u) == 0)
            {
                M <<= 1;
                E--;
            }
            M &= 0x3FFu;
            Bits = Sign | ((E + 112u) << 23) | (M << 13);
        }
    }
    else if (E == 31)
    {
        Bits = Sign | 0x7F800000u | (M << 13);
    }
    else
    {
        Bits = Sign | ((E + 112u) << 23) | (M << 13);
    }
    float F;
    std::memcpy(&F, &Bits, sizeof(F));
    return F;
}

// ======== Reader ========

Status Reader::Open(ByteView Data, Reader& Out, const Limits& L)
{
    try
    {
        Info Parsed;
        std::vector<int64_t> Offsets;
        Status S = ParseHeader(Data, L, Parsed, Offsets);
        if (!S.Ok()) return S;
        Out.Data = Data;
        Out.Header = std::move(Parsed);
        Out.Offsets = std::move(Offsets);
        Out.Bounds = L;
        return Status();
    }
    catch (const std::bad_alloc&)
    {
        return Fail(Code::TooLarge, "Not enough memory for the EXR header.");
    }
    catch (const std::exception& Ex)
    {
        return Fail(Code::Invalid, std::string("The EXR data is corrupt: ") + Ex.what());
    }
}

Status Reader::ReadDownsampled(int MaxWidth, int MaxHeight, const ReadOptions& Options, Pixels& Out, const std::atomic<bool>* Cancel) const
{
    try
    {
        const Info& Hd = Header;
        if (Hd.Method != Compression::None && Hd.Method != Compression::Zips && Hd.Method != Compression::Zip && Hd.Method != Compression::Piz)
        {
            return Fail(Code::UnsupportedCompression, "EXR compression " + std::to_string(static_cast<int>(Hd.Method)) + " is not supported (NONE, ZIPS, ZIP and PIZ are).");
        }

        if (MaxWidth < 1 || MaxHeight < 1 || static_cast<int64_t>(MaxWidth) * MaxHeight > Bounds.MaxPixels)
        {
            return Fail(Code::TooLarge, "The requested output size is out of range.");
        }

        // Channel mapping: R, G, B (or Y for all three), optional A
        int R = -1, G = -1, B = -1, A = -1, Y = -1;
        int PixelBytes = 0;
        std::vector<int> ChannelOffset(Hd.Channels.size());
        for (size_t I = 0; I < Hd.Channels.size(); I++)
        {
            const Channel& C = Hd.Channels[I];
            if (C.XSampling != 1 || C.YSampling != 1)
            {
                return Fail(Code::UnsupportedChannels, "EXR channel " + C.Name + " is subsampled.");
            }

            ChannelOffset[I] = PixelBytes;
            PixelBytes += C.ByteSize();
            if (C.Name == "R") R = static_cast<int>(I);
            else if (C.Name == "G") G = static_cast<int>(I);
            else if (C.Name == "B") B = static_cast<int>(I);
            else if (C.Name == "A") A = static_cast<int>(I);
            else if (C.Name == "Y") Y = static_cast<int>(I);
        }

        if (R < 0 || G < 0 || B < 0)
        {
            if (Y < 0)
            {
                return Fail(Code::UnsupportedChannels, "The EXR file has no R, G and B channels.");
            }
            R = G = B = Y;
        }

        const int Width = Hd.Width;
        const int Height = Hd.Height;
        int Factor = 1;
        while (((Width + Factor - 1) / Factor) > MaxWidth || ((Height + Factor - 1) / Factor) > MaxHeight)
        {
            Factor++;
        }

        const int OutW = (Width + Factor - 1) / Factor;
        const int OutH = (Height + Factor - 1) / Factor;
        const int OutChannels = Options.IncludeAlpha ? 4 : 3;
        const int64_t Total = static_cast<int64_t>(OutW) * OutH * OutChannels;
        if (Total > (int64_t(1) << 28))
        {
            return Fail(Code::TooLarge, "The output image is too large.");
        }

        std::vector<float> Accumulator(static_cast<size_t>(Total), 0.0f);

        const int Sources[4] = { R, G, B, A };
        const int Lpc = LinesPerChunk(Hd.Method);
        std::vector<uint8_t> Raw;
        PizDecoder Piz;
        const bool Sanitize = Options.Sanitize;
        const int64_t Length = static_cast<int64_t>(Data.size());

        for (size_t Chunk = 0; Chunk < Offsets.size(); Chunk++)
        {
            if (Cancel != nullptr && Cancel->load())
            {
                return Fail(Code::Cancelled, "The EXR read was cancelled.");
            }

            const int FirstLine = static_cast<int>(Chunk) * Lpc;
            const int Lines = (std::min)(Lpc, Height - FirstLine);
            const size_t RawSize = static_cast<size_t>(Lines) * static_cast<size_t>(Width) * static_cast<size_t>(PixelBytes);

            const int64_t At = Offsets[Chunk];
            if (At < 0 || At > Length - 8)
            {
                return Fail(Code::Truncated, "The EXR file ends inside a chunk header. (chunk " + std::to_string(Chunk) + ")");
            }

            const ByteView Stored = Data.subspan(static_cast<size_t>(At));
            const int32_t ChunkY = I32(Stored, 0);
            const int32_t DataSize = I32(Stored, 4);
            if (static_cast<int64_t>(ChunkY) != static_cast<int64_t>(Hd.MinY) + FirstLine)
            {
                return Fail(Code::Invalid, "An EXR chunk is at the wrong scanline. (chunk " + std::to_string(Chunk) + ", y " + std::to_string(ChunkY) + ")");
            }

            if (DataSize < 0 || static_cast<int64_t>(DataSize) > (static_cast<int64_t>(RawSize) * 2) + 1024)
            {
                return Fail(Code::Invalid, "An EXR chunk has an impossible size. (chunk " + std::to_string(Chunk) + ", size " + std::to_string(DataSize) + ")");
            }

            if (static_cast<int64_t>(Stored.size()) - 8 < DataSize)
            {
                return Fail(Code::Truncated, "The EXR file ends inside a chunk. (chunk " + std::to_string(Chunk) + ")");
            }

            if (Raw.size() < RawSize)
            {
                Raw.resize(RawSize);
            }
            const std::span<uint8_t> RawView(Raw.data(), RawSize);

            const char* DecodeError = DecodeChunk(Hd, Piz, Stored.subspan(8, static_cast<size_t>(DataSize)), RawView, Width, Lines);
            if (DecodeError != nullptr)
            {
                return Fail(Code::Invalid, std::string(DecodeError) + " (chunk " + std::to_string(Chunk) + ")");
            }

            for (int Line = 0; Line < Lines; Line++)
            {
                const int OutY = (FirstLine + Line) / Factor;
                for (int Oc = 0; Oc < OutChannels; Oc++)
                {
                    const int Source = Sources[Oc];
                    if (Source < 0) continue; // alpha missing: filled with 1 below

                    const Channel& C = Hd.Channels[static_cast<size_t>(Source)];
                    const size_t Start = (static_cast<size_t>(Line) * Width * PixelBytes) + (static_cast<size_t>(Width) * ChannelOffset[static_cast<size_t>(Source)]);
                    AccumulateRow(ByteView(RawView).subspan(Start, static_cast<size_t>(Width) * C.ByteSize()), C.Type, Width, Factor, Sanitize, Accumulator,
                                  (static_cast<size_t>(OutY) * OutW * OutChannels) + Oc, static_cast<size_t>(OutChannels));
                }
            }
        }

        // Normalise by the number of source samples in each box
        for (int Oy = 0; Oy < OutH; Oy++)
        {
            const int Rows = (std::min)(Factor, Height - (Oy * Factor));
            for (int Ox = 0; Ox < OutW; Ox++)
            {
                const int Cols = (std::min)(Factor, Width - (Ox * Factor));
                const float Inv = 1.0f / static_cast<float>(Rows * Cols);
                const size_t O = (static_cast<size_t>(Oy) * OutW + Ox) * OutChannels;
                for (int C = 0; C < OutChannels; C++)
                {
                    if (Sources[C] < 0) Accumulator[O + C] = 1.0f;
                    else Accumulator[O + C] *= Inv;
                }
            }
        }

        Out.Width = OutW;
        Out.Height = OutH;
        Out.SourceWidth = Width;
        Out.SourceHeight = Height;
        Out.ChannelCount = OutChannels;
        Out.Data = std::move(Accumulator);
        Out.Factor = Factor;
        return Status();
    }
    catch (const std::bad_alloc&)
    {
        return Fail(Code::TooLarge, "Not enough memory to read the EXR image.");
    }
    catch (const std::exception& Ex)
    {
        return Fail(Code::Invalid, std::string("The EXR data is corrupt: ") + Ex.what());
    }
}

Status LoadFile(const std::filesystem::path& Path, std::vector<uint8_t>& Bytes, const Limits& L)
{
    try
    {
        std::ifstream In(Path, std::ios::binary | std::ios::ate);
        if (!In)
        {
            return Fail(Code::Io, "Cannot open the EXR file: " + Path.filename().string());
        }

        const std::streamoff Size = In.tellg();
        if (Size < 0)
        {
            return Fail(Code::Io, "Cannot read the EXR file: " + Path.filename().string());
        }
        if (Size > L.MaxFileBytes)
        {
            return Fail(Code::TooLarge, "The EXR file is larger than the reader's limit.");
        }

        Bytes.assign(static_cast<size_t>(Size), 0);
        In.seekg(0);
        if (Size > 0 && !In.read(reinterpret_cast<char*>(Bytes.data()), Size))
        {
            Bytes.clear();
            return Fail(Code::Io, "Cannot read the EXR file: " + Path.filename().string());
        }
        return Status();
    }
    catch (const std::bad_alloc&)
    {
        return Fail(Code::TooLarge, "Not enough memory to load the EXR file.");
    }
    catch (const std::exception& Ex)
    {
        return Fail(Code::Io, std::string("Cannot read the EXR file: ") + Ex.what());
    }
}

Status ReadFile(const std::filesystem::path& Path, int MaxWidth, int MaxHeight, const ReadOptions& Options, Pixels& Out, Info* HeaderInfo)
{
    std::vector<uint8_t> Bytes;
    Status S = LoadFile(Path, Bytes);
    if (!S.Ok()) return S;

    Reader R;
    S = Reader::Open(Bytes, R);
    if (!S.Ok()) return S;

    S = R.ReadDownsampled(MaxWidth, MaxHeight, Options, Out);
    if (S.Ok() && HeaderInfo != nullptr) *HeaderInfo = R.GetInfo();
    return S;
}

// ======== PIZ ========

namespace
{
    constexpr int BitmapSize = 8192; // 65536 values, one bit each
    constexpr int UShortRange = 1 << 16;
    constexpr int HufEncSize = (1 << 16) + 1;
    constexpr int HufDecBits = 14;
    constexpr int HufDecSize = 1 << HufDecBits;
    constexpr int ShortZeroCodeRun = 59;
    constexpr int LongZeroCodeRun = 63;
    constexpr int ShortestLongRun = 2 + LongZeroCodeRun - ShortZeroCodeRun;
    constexpr int MaxCodeLength = 58;

    // Wavelet inverse of two values, 14-bit and 16-bit variants
    void Dec14(uint16_t L, uint16_t H, uint16_t& A, uint16_t& B)
    {
        const int Ls = static_cast<int16_t>(L);
        const int Hs = static_cast<int16_t>(H);
        const int Ai = Ls + (Hs & 1) + (Hs >> 1);
        A = static_cast<uint16_t>(static_cast<int16_t>(Ai));
        B = static_cast<uint16_t>(static_cast<int16_t>(Ai - Hs));
    }

    void Dec16(uint16_t L, uint16_t H, uint16_t& A, uint16_t& B)
    {
        constexpr int Offset = 1 << 15;
        constexpr int Mask = 0xFFFF;
        const int M = L;
        const int D = H;
        const int Bb = (M - (D >> 1)) & Mask;
        const int Aa = (D + Bb - Offset) & Mask;
        B = static_cast<uint16_t>(Bb);
        A = static_cast<uint16_t>(Aa);
    }

    // The inverse 2D wavelet over one interleaved plane of A starting at Start (Nx by Ny, steps Ox and Oy), in place
    void Wav2Decode(std::span<uint16_t> A, int Start, int Nx, int Ox, int Ny, int Oy, int Mx)
    {
        const bool W14 = Mx < (1 << 14);
        const int N = (std::min)(Nx, Ny);
        int P = 1;
        while (P <= N) P <<= 1;
        P >>= 1;
        int P2 = P;
        P >>= 1;
        while (P >= 1)
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
                        Dec14(A[Px], A[P10], I00, I10);
                        Dec14(A[P01], A[P11], I01, I11);
                        Dec14(I00, I01, A[Px], A[P01]);
                        Dec14(I10, I11, A[P10], A[P11]);
                    }
                    else
                    {
                        Dec16(A[Px], A[P10], I00, I10);
                        Dec16(A[P01], A[P11], I01, I11);
                        Dec16(I00, I01, A[Px], A[P01]);
                        Dec16(I10, I11, A[P10], A[P11]);
                    }
                }

                if ((Nx & P) != 0)
                {
                    const int P10 = Px + Oy1;
                    uint16_t I00;
                    if (W14) Dec14(A[Px], A[P10], I00, A[P10]);
                    else Dec16(A[Px], A[P10], I00, A[P10]);
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
                    if (W14) Dec14(A[Px], A[P01], I00, A[P01]);
                    else Dec16(A[Px], A[P01], I00, A[P01]);
                    A[Px] = I00;
                }
            }

            P2 = P;
            P >>= 1;
        }
    }

    bool GetBits(int NBits, uint64_t& C, int& Lc, ByteView Data, size_t& Pos, uint32_t& Value)
    {
        while (Lc < NBits)
        {
            if (Pos >= Data.size())
            {
                Value = 0;
                return false;
            }
            C = (C << 8) | Data[Pos++];
            Lc += 8;
        }

        Lc -= NBits;
        Value = static_cast<uint32_t>((C >> Lc) & ((uint64_t(1) << NBits) - 1));
        return true;
    }
}

PizDecoder::PizDecoder()
    : Bitmap(BitmapSize), Lut(UShortRange), CodeLength(HufEncSize), CodeValue(HufEncSize), Primary(HufDecSize), SymbolsByLength(HufEncSize),
      LengthCount(MaxCodeLength + 2), LengthStart(MaxCodeLength + 2), LengthOffset(MaxCodeLength + 2)
{
}

const char* PizDecoder::Decode(ByteView Input, int Width, int Lines, const std::vector<Channel>& Channels, std::span<uint8_t> Output)
{
    const size_t TotalU16 = Output.size() / 2;
    if (TotalU16 == 0) return nullptr;
    if (Input.size() < 4) return "PIZ chunk too short";

    size_t P = 0;
    const int MinNonZero = U16(Input, P);
    const int MaxNonZero = U16(Input, P + 2);
    P += 4;
    if (MaxNonZero >= BitmapSize) return "PIZ bitmap range out of bounds";

    std::fill(Bitmap.begin(), Bitmap.end(), uint8_t(0));
    if (MinNonZero <= MaxNonZero)
    {
        const size_t Count = static_cast<size_t>(MaxNonZero - MinNonZero) + 1;
        if (Input.size() - P < Count) return "PIZ bitmap truncated";
        std::copy_n(Input.begin() + static_cast<std::ptrdiff_t>(P), Count, Bitmap.begin() + MinNonZero);
        P += Count;
    }

    const int MaxValue = ReverseLutFromBitmap();

    if (Input.size() - P < 4) return "PIZ Huffman length missing";

    const int32_t HufLength = I32(Input, P);
    P += 4;
    if (HufLength < 0 || static_cast<size_t>(HufLength) > Input.size() - P) return "PIZ Huffman length out of bounds";

    if (Tmp.size() < TotalU16) Tmp.resize(TotalU16);
    const std::span<uint16_t> Plane(Tmp.data(), TotalU16);

    const char* HuffError = HufUncompress(Input.subspan(P, static_cast<size_t>(HufLength)), Plane);
    if (HuffError != nullptr) return HuffError;

    // Wavelet decode per channel; channel c owns width * lines * (size in u16) consecutive values, row major
    std::vector<size_t> ChannelStart(Channels.size());
    size_t Cursor = 0;
    for (size_t C = 0; C < Channels.size(); C++)
    {
        ChannelStart[C] = Cursor;
        Cursor += static_cast<size_t>(Width) * Lines * (Channels[C].ByteSize() / 2);
    }
    if (Cursor != TotalU16) return "PIZ channel sizes do not match the chunk";

    for (size_t C = 0; C < Channels.size(); C++)
    {
        const int Size = Channels[C].ByteSize() / 2;
        for (int J = 0; J < Size; J++)
        {
            Wav2Decode(Plane, static_cast<int>(ChannelStart[C]) + J, Width, Size, Lines, Width * Size, MaxValue);
        }
    }

    for (size_t I = 0; I < TotalU16; I++)
    {
        Plane[I] = Lut[Plane[I]];
    }

    // Interleave: per line, per channel
    size_t O = 0;
    std::vector<size_t> RowStart = ChannelStart;
    for (int Y = 0; Y < Lines; Y++)
    {
        for (size_t C = 0; C < Channels.size(); C++)
        {
            const size_t N = static_cast<size_t>(Width) * (Channels[C].ByteSize() / 2);
            for (size_t I = 0; I < N; I++)
            {
                const uint16_t V = Plane[RowStart[C] + I];
                Output[O++] = static_cast<uint8_t>(V & 0xFF);
                Output[O++] = static_cast<uint8_t>(V >> 8);
            }
            RowStart[C] += N;
        }
    }
    return nullptr;
}

int PizDecoder::ReverseLutFromBitmap()
{
    int K = 0;
    for (int I = 0; I < UShortRange; I++)
    {
        if (I == 0 || (Bitmap[static_cast<size_t>(I >> 3)] & (1 << (I & 7))) != 0)
        {
            Lut[static_cast<size_t>(K++)] = static_cast<uint16_t>(I);
        }
    }

    const int N = K - 1;
    while (K < UShortRange) Lut[static_cast<size_t>(K++)] = 0;
    return N;
}

const char* PizDecoder::HufUncompress(ByteView Data, std::span<uint16_t> Output)
{
    if (Data.empty())
    {
        return Output.empty() ? nullptr : "PIZ Huffman data missing";
    }
    if (Data.size() < 20) return "PIZ Huffman header truncated";

    const uint32_t Im = U32(Data, 0);
    const uint32_t IM = U32(Data, 4);
    const uint32_t NBits = U32(Data, 12);
    if (Im >= static_cast<uint32_t>(HufEncSize) || IM >= static_cast<uint32_t>(HufEncSize) || Im > IM)
    {
        return "PIZ Huffman symbol range invalid";
    }

    size_t Pos = 20;
    const char* TableError = UnpackEncodingTable(Data, Pos, static_cast<int>(Im), static_cast<int>(IM));
    if (TableError != nullptr) return TableError;

    const size_t BytesLeft = Data.size() - Pos;
    if (static_cast<int64_t>(NBits) > 8 * static_cast<int64_t>(BytesLeft)) return "PIZ Huffman bit count exceeds the data";

    const char* BuildError = BuildDecodingTable(static_cast<int>(Im), static_cast<int>(IM));
    if (BuildError != nullptr) return BuildError;

    return DecodeSymbols(Data.subspan(Pos), static_cast<int64_t>(NBits), static_cast<int>(IM), Output);
}

const char* PizDecoder::UnpackEncodingTable(ByteView Data, size_t& Pos, int Im, int IM)
{
    std::fill(CodeLength.begin(), CodeLength.end(), uint8_t(0));
    uint64_t C = 0;
    int Lc = 0;
    for (; Im <= IM; Im++)
    {
        uint32_t L;
        if (!GetBits(6, C, Lc, Data, Pos, L)) return "PIZ Huffman table truncated";

        CodeLength[static_cast<size_t>(Im)] = static_cast<uint8_t>(L);
        if (L >= static_cast<uint32_t>(ShortZeroCodeRun))
        {
            int ZeroRun;
            if (L == static_cast<uint32_t>(LongZeroCodeRun))
            {
                uint32_t Extra;
                if (!GetBits(8, C, Lc, Data, Pos, Extra)) return "PIZ Huffman table truncated";
                ZeroRun = static_cast<int>(Extra) + ShortestLongRun;
            }
            else
            {
                ZeroRun = static_cast<int>(L) - ShortZeroCodeRun + 2;
            }

            if (Im + ZeroRun > IM + 1) return "PIZ Huffman table zero run overflows";

            while (ZeroRun-- > 0) CodeLength[static_cast<size_t>(Im++)] = 0;
            Im--;
        }
    }

    // Canonical codes: longest codes first
    std::fill(LengthCount.begin(), LengthCount.end(), int64_t(0));
    for (int I = 0; I < HufEncSize; I++) LengthCount[CodeLength[static_cast<size_t>(I)]]++;

    uint64_t Code = 0;
    for (int I = MaxCodeLength; I > 0; I--)
    {
        const uint64_t Next = (Code + static_cast<uint64_t>(LengthCount[static_cast<size_t>(I)])) >> 1;
        LengthStart[static_cast<size_t>(I)] = Code;
        Code = Next;
    }

    std::vector<uint64_t> Running(LengthStart);
    for (int I = 0; I < HufEncSize; I++)
    {
        const int L = CodeLength[static_cast<size_t>(I)];
        CodeValue[static_cast<size_t>(I)] = L > 0 ? Running[static_cast<size_t>(L)]++ : 0;
    }
    return nullptr;
}

const char* PizDecoder::BuildDecodingTable(int Im, int IM)
{
    std::fill(Primary.begin(), Primary.end(), uint32_t(0));
    int Offset = 0;
    for (int L = 1; L <= MaxCodeLength; L++)
    {
        LengthOffset[static_cast<size_t>(L)] = Offset;
        Offset += static_cast<int>(LengthCount[static_cast<size_t>(L)]);
    }

    std::vector<int> Fill(MaxCodeLength + 2, 0);
    for (int S = Im; S <= IM; S++)
    {
        const int L = CodeLength[static_cast<size_t>(S)];
        if (L == 0) continue;

        const uint64_t C = CodeValue[static_cast<size_t>(S)];
        if ((C >> L) != 0) return "PIZ Huffman table entry invalid";

        SymbolsByLength[static_cast<size_t>(LengthOffset[static_cast<size_t>(L)] + Fill[static_cast<size_t>(L)]++)] = S;
        if (L <= HufDecBits)
        {
            const int First = static_cast<int>(C << (HufDecBits - L));
            const int Span = 1 << (HufDecBits - L);
            for (int I = 0; I < Span; I++)
            {
                if (Primary[static_cast<size_t>(First + I)] != 0) return "PIZ Huffman table is not prefix free";
                Primary[static_cast<size_t>(First + I)] = (static_cast<uint32_t>(L) << 20) | static_cast<uint32_t>(S);
            }
        }
    }
    return nullptr;
}

const char* PizDecoder::DecodeSymbols(ByteView Data, int64_t NBits, int Rlc, std::span<uint16_t> Output)
{
    uint64_t Buffer = 0;
    int Bits = 0;
    size_t Index = 0;
    int64_t Consumed = 0;
    size_t OutPos = 0;
    while (OutPos < Output.size())
    {
        while (Bits <= 56 && Index < Data.size())
        {
            Buffer |= static_cast<uint64_t>(Data[Index++]) << (56 - Bits);
            Bits += 8;
        }

        const uint32_t Entry = Primary[static_cast<size_t>(Buffer >> (64 - HufDecBits))];
        int Length;
        int Symbol;
        if (Entry != 0)
        {
            Length = static_cast<int>(Entry >> 20);
            Symbol = static_cast<int>(Entry & 0xFFFFF);
        }
        else
        {
            Length = 0;
            Symbol = -1;
            for (int L = HufDecBits + 1; L <= MaxCodeLength; L++)
            {
                const int64_t N = LengthCount[static_cast<size_t>(L)];
                if (N == 0) continue;

                const uint64_t Prefix = Buffer >> (64 - L);
                if (Prefix >= LengthStart[static_cast<size_t>(L)] && Prefix - LengthStart[static_cast<size_t>(L)] < static_cast<uint64_t>(N))
                {
                    Length = L;
                    Symbol = SymbolsByLength[static_cast<size_t>(LengthOffset[static_cast<size_t>(L)]) + static_cast<size_t>(Prefix - LengthStart[static_cast<size_t>(L)])];
                    break;
                }
            }

            if (Length == 0) return "PIZ Huffman code not found";
        }

        Consumed += Length;
        if (Consumed > NBits || Length > Bits) return "PIZ Huffman data ends early";

        Buffer <<= Length;
        Bits -= Length;

        if (Symbol == Rlc)
        {
            if (Bits < 8 || Consumed + 8 > NBits) return "PIZ Huffman run length missing";

            const size_t Repeat = static_cast<size_t>(Buffer >> 56);
            Buffer <<= 8;
            Bits -= 8;
            Consumed += 8;
            if (OutPos == 0) return "PIZ Huffman run with no previous value";
            if (OutPos + Repeat > Output.size()) return "PIZ Huffman run overflows the chunk";

            std::fill(Output.begin() + static_cast<std::ptrdiff_t>(OutPos), Output.begin() + static_cast<std::ptrdiff_t>(OutPos + Repeat), Output[OutPos - 1]);
            OutPos += Repeat;
        }
        else
        {
            Output[OutPos++] = static_cast<uint16_t>(Symbol);
        }
    }
    return nullptr;
}
}
