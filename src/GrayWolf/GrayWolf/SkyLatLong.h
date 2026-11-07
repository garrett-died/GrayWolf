#pragma once

// A game's sky cube resampled by direction into the 2:1 lat-long image a Black Ops III sky material holds (M4e, docs/hydrax-port/m4e.md). Pure: float faces in, half RGB out,
// CPU only. The game's facts (which stored face looks where, how its colours scale) are a SkyCubeSpec the game's reader supplies; this file knows none.
//
// Cube: six square faces in Direct3D's order +X -X +Y -Y +Z -Z, rows top to bottom, RGB floats. Direct3D's face frames are the documented ones (docs/hydrax-port/sky-axes.md):
// texel (s, t) of a face looks along Forward + s * Right + t * Up, s = 2(x + 0.5)/n - 1 across, t = 1 - 2(y + 0.5)/n up the rows. A game's world direction is the face direction
// permuted and signed by CubeConvention (identity: the world direction IS Direct3D's direction, which is what the Modern Warfare Remastered sky shader does: it samples the cube
// with the world position minus the camera).
//
// Lat-long: the Black Ops III sky shader (share/raw/shaders_stable/techsetdef_sky_latlong_hdr.hlsl) reads texCoords = (atan2(c.y, c.x) / -2pi + 0.5, acos(c.z) / pi) of the
// direction c, which is the sky direction turned by the material's skyRotation. Here c is the world direction (skyRotation 0 taken as no turn: how the material's degrees
// become the shader's float2 is not in share/raw, so that is an assumption, listed).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SkyConv
{
    struct Cube
    {
        int Size = 0;                          // the face's side in texels
        std::array<std::vector<float>, 6> Rgb;   // Size * Size * 3 floats each
    };

    // world[i] = Sign[i] * d3d[Perm[i]]. Identity: Perm {0,1,2}, Sign {1,1,1}.
    struct CubeConvention
    {
        std::array<int, 3> Perm = { 0, 1, 2 };
        std::array<int, 3> Sign = { 1, 1, 1 };
        void ToWorld(const double (&D3d)[3], double (&World)[3]) const;
        void ToD3d(const double (&World)[3], double (&D3d)[3]) const;
    };

    // The direction the texel (X, Y) of a face looks along (X, Y in texels, a texel's centre at +0.5), Direct3D space, not normalised
    void TexelDirection(int Face, int Size, double X, double Y, double (&Dir)[3]);
    // The face a Direct3D-space direction hits and the texel position on it (texel units, centre at +0.5). False for a zero vector.
    bool DirectionToTexel(const double (&Dir)[3], int Size, int& Face, double& X, double& Y);

    // The direction of lat-long pixel (I, J) of a W x H image by the Black Ops III sky shader's own mapping: longitude (0.5 - u) * 2pi, colatitude v * pi
    void LatLongDirection(int I, int J, int W, int H, double (&Dir)[3]);

    // The cube resampled (bilinear within a face) into W x H half RGB, every channel multiplied by Scale first. Threads: 0 = hardware concurrency. False (Why) for a bad cube or size.
    bool ResampleToLatLong(const Cube& C, const CubeConvention& Conv, float Scale, int W, int H, std::vector<uint16_t>& HalfRgb, std::string& Why, int Threads = 0);

    // The lat-long size for a cube face: width 4 * face, capped at the Mod Tools' largest stock sky (8192 by 4096), at least 2:1 even
    void LatLongSize(int FaceSize, int& W, int& H);

    struct Peak
    {
        bool Found = false;         // false: no texel qualified (an all-black cube, or none in the asked hemisphere): every other field is then meaningless
        double Luminance = 0;       // of the brightest texel (Rec. 709 weights)
        int Face = 0;
        double X = 0, Y = 0;        // its texel
        double Dir[3] = { 0, 0, 0 };   // Direct3D space, unit
        double Mean = 0;            // the mean luminance of the whole cube
        double MaxChannel = 0;      // the largest single value
    };
    // MinWorldZ: only texels whose world direction (by Conv) has z / length at least this (-2: all of them; 0: the upper hemisphere). A bake that mirrors the sky below the horizon
    // has its brightest texel in the mirror as often as above.
    Peak Brightest(const Cube& C, const CubeConvention& Conv = CubeConvention(), double MinWorldZ = -2.0);

    // The angle between two directions, degrees
    double AngleBetween(const double (&A)[3], const double (&B)[3]);

    int SelfTest(std::string& Failures);
}
