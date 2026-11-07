#pragma once

// OpenEXR scanline writer: version 2, one part, half RGB (channels B, G, R, in the alphabetical order the format requires), compression ZIPS (one scanline per chunk, the
// codec the Mod Tools' own lat-long skies use) or NONE. Pure: pixels in, bytes out. The reader that proves it is ExrReader.h (Exr::Reader); the round trip is in exr-selftest.
// Layout and the ZIP pre-filter (reorder the bytes into even and odd halves, then the delta predictor) are the OpenEXR file layout documentation's.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Exr
{
    // float -> IEEE half, round to nearest even; values past the half range are clamped to +-65504, NaN becomes 0
    uint16_t FloatToHalf(float Value);

    enum class WriteCompression { None, Zips };

    // Rgb: Width * Height * 3 halves, rows top to bottom, R G B per pixel. Out gets the whole file. False (Why filled) for a size that is not positive, too large (over
    // 16384 either way) or a span of the wrong length.
    bool WriteHalfRgb(int Width, int Height, std::span<const uint16_t> Rgb, WriteCompression Method, std::vector<uint8_t>& Out, std::string& Why);

    // the writer's checks, run by exr-selftest after the reader's: the half conversion, files through Exr::Reader
    int WriterSelfTest(std::string& Failures);
}
