#pragma once

// OpenEXR scanline reader (a pure module: bytes in, floats out, no D3D, no globals), ported from HydraX's
// HydraX.Formats/Exr (ExrReader, PizDecoder, ExrTypes). Version 2, single part, scanline images; compression NONE,
// ZIPS, ZIP and PIZ (the codecs of the Mod Tools' sky files); half, float and uint channels; one output of RGB or
// RGBA floats (a file with Y and no R, G, B gives Y in all three), optionally box-filtered by the smallest integer
// factor that fits a size while the chunks stream through (memory is the output plus one chunk).
//
// Nothing throws across this API. Bad, truncated or oversized input is a Status with a code (Code, CodeName); nothing is
// allocated from a size the file claims before it is checked against Limits and the buffer's length, and no read goes
// past the buffer. Tiled, deep, multipart and non-version-2 files are UnsupportedLayout, as in HydraX.
//
// Differences from HydraX (the port keeps its error codes and messages otherwise):
//  - the file is read whole into memory (LoadFile) and parsed in place, where HydraX seeks in a stream; a file over
//    MaxFileBytes is TooLarge
//  - a ZIP/ZIPS chunk whose zlib stream inflates to MORE than the chunk's scanlines is Invalid (libdeflate refuses it;
//    HydraX's reader ignored the surplus); one that inflates to less is Invalid in both
//  - cancellation is a flag the caller sets (std::atomic<bool>), checked before every chunk
// Self-test: SelfTest (the exr-selftest command).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "ByteView.h"

namespace Exr
{
    enum class Code
    {
        Ok,
        BadMagic,               // exr.bad_magic: not an OpenEXR file
        Truncated,              // exr.truncated: the data ends inside the header, the offset table or a chunk
        Invalid,                // exr.invalid: structurally invalid (attribute, size, offset, chunk, compressed stream)
        TooLarge,               // exr.too_large: over the limits, or the requested output is
        UnsupportedLayout,      // exr.unsupported_layout: not version 2, or tiled, deep or multipart
        UnsupportedCompression, // exr.unsupported_compression: only NONE, ZIPS, ZIP and PIZ are read
        UnsupportedChannels,    // exr.unsupported_channels: no R, G, B (or Y), or a subsampled channel
        Io,                     // exr.io: the file could not be opened or read
        Cancelled,              // exr.cancelled: the caller's flag was set
    };

    // "exr.bad_magic" ... ("ok" for Code::Ok)
    const char* CodeName(Code C);

    struct Status
    {
        Code Kind = Code::Ok;
        std::string Message;
        bool Ok() const { return Kind == Code::Ok; }
    };

    enum class Compression
    {
        None = 0, Rle = 1, Zips = 2, Zip = 3, Piz = 4, Pxr24 = 5, B44 = 6, B44A = 7, Dwaa = 8, Dwab = 9,
    };

    enum class PixelType
    {
        Unsigned32 = 0, Half = 1, Real32 = 2,
    };

    struct Channel
    {
        std::string Name;
        PixelType Type = PixelType::Half;
        int XSampling = 1;
        int YSampling = 1;
        int ByteSize() const { return Type == PixelType::Half ? 2 : 4; }
    };

    struct Info
    {
        int Width = 0, Height = 0;       // data window size
        int MinX = 0, MinY = 0;          // data window origin
        Compression Method = Compression::None;
        std::vector<Channel> Channels;   // file order (alphabetical, as the format requires)
        int LineOrder = 0;               // 0 increasing Y, 1 decreasing Y, 2 random
    };

    // Bounds applied before anything is allocated (the Mod Tools' largest sky is 8192 by 4096)
    struct Limits
    {
        int MaxDimension = 16384;
        int64_t MaxPixels = int64_t(1) << 27;
        int MaxHeaderBytes = 1 << 20;
        int MaxChannels = 64;
        int64_t MaxFileBytes = int64_t(1) << 30; // LoadFile only
    };

    struct ReadOptions
    {
        bool IncludeAlpha = true; // four channels (alpha 1 when the file has none); false: RGB
        bool Sanitize = true;     // NaN becomes 0 and every sample is clamped to +-65504 before averaging
    };

    // Decoded, possibly box-filtered pixels: ChannelCount floats per pixel, rows top to bottom
    struct Pixels
    {
        int Width = 0, Height = 0;             // output
        int SourceWidth = 0, SourceHeight = 0; // the file's data window
        int ChannelCount = 0;                  // 3 or 4
        std::vector<float> Data;               // Width * Height * ChannelCount
        int Factor = 1;                        // integer box factor (1: no filtering)
    };

    // IEEE half -> float, exact (denormals, infinities and NaNs included)
    float HalfToFloat(uint16_t Half);

    // ---- PIZ (OpenEXR's wavelet plus Huffman codec, from the format description). Public for the self-test only.
    // Every length and count is bounded by the input and the expected output: corrupt data returns a message
    // (never nullptr), never reads or writes outside the buffers. An instance owns reusable tables; not thread safe.
    class PizDecoder
    {
    public:
        PizDecoder();

        // One chunk into the uncompressed scanline layout (per line, per channel, little-endian samples); Output is
        // exactly lines * width * bytes per pixel bytes. nullptr on success.
        const char* Decode(ByteView Input, int Width, int Lines, const std::vector<Channel>& Channels, std::span<uint8_t> Output);

        // The Huffman stage alone: Output.size() values out of Data. nullptr on success.
        const char* HufUncompress(ByteView Data, std::span<uint16_t> Output);

    private:
        int ReverseLutFromBitmap();
        const char* UnpackEncodingTable(ByteView Data, size_t& Pos, int Im, int IM);
        const char* BuildDecodingTable(int Im, int IM);
        const char* DecodeSymbols(ByteView Data, int64_t NBits, int Rlc, std::span<uint16_t> Output);

        std::vector<uint8_t> Bitmap;
        std::vector<uint16_t> Lut;
        std::vector<uint8_t> CodeLength;
        std::vector<uint64_t> CodeValue;
        std::vector<uint32_t> Primary;
        std::vector<int32_t> SymbolsByLength;
        std::vector<int64_t> LengthCount;
        std::vector<uint64_t> LengthStart;
        std::vector<int32_t> LengthOffset;
        std::vector<uint16_t> Tmp;
    };

    // ---- the reader: Open parses the header and offset table, Read decodes. The view must outlive the Reader.
    class Reader
    {
    public:
        // Parses the header and offset table of Data (no pixel data is read)
        static Status Open(ByteView Data, Reader& Out, const Limits& L = Limits());

        const Info& GetInfo() const { return Header; }

        // The whole image, box-filtered by the smallest integer factor that makes it fit MaxWidth by MaxHeight (factor 1
        // returns the pixels as they are). Cancel, when given, is checked before every chunk.
        Status ReadDownsampled(int MaxWidth, int MaxHeight, const ReadOptions& Options, Pixels& Out, const std::atomic<bool>* Cancel = nullptr) const;

    private:
        ByteView Data;
        Info Header;
        std::vector<int64_t> Offsets;
        Limits Bounds;
    };

    // The file's bytes (Io when it cannot be opened or read, TooLarge past L.MaxFileBytes)
    Status LoadFile(const std::filesystem::path& Path, std::vector<uint8_t>& Bytes, const Limits& L = Limits());

    // LoadFile + Open + ReadDownsampled in one call; HeaderInfo, when given, receives the header facts on success
    Status ReadFile(const std::filesystem::path& Path, int MaxWidth, int MaxHeight, const ReadOptions& Options, Pixels& Out, Info* HeaderInfo = nullptr);

    // exr-selftest: the ported HydraX EXR and PIZ cases on files built in memory, then the cases the port added.
    // Returns the number of failures; Failures gets one line per failure.
    int SelfTest(std::string& Failures);
}
