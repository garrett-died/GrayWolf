#pragma once

// A cube-map DDS (the sky image a game stores: six square faces, BC6H or a float format) decoded to float RGB faces for SkyConv. DirectXTex does the block decode on the CPU;
// no D3D device is made.

#include "SkyLatLong.h"

#include <cstdint>
#include <span>
#include <string>

namespace SkyConv
{
    // Dds: a whole .dds file. The cube must be square, a cube map (or an array of six), a BC6H or float format. The first mip of every face is read. Format gets the DXGI format
    // number and the face size as text for the report. False (Why) when the file is not such a cube.
    inline constexpr size_t kMaxFace = 4096;   // the largest face DecodeCubeDds takes: six faces of RGB floats are 1.2 GB
    bool DecodeCubeDds(std::span<const uint8_t> Dds, Cube& Out, std::string& Format, std::string& Why);
}
