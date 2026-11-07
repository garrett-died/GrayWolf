#include "stdafx.h"

// Self-tests of the sky conversion (M4e): the EXR writer through the reader, the cube resample against the Black Ops III sky shader's own formula, the cube DDS decoder
// (BC6H included). The expected values are stated here from the shader text and Direct3D's face table, not from the code under test.

#include "DirectXTex.h"
#include "ExrReader.h"
#include "ExrWriter.h"
#include "SkyCubeDds.h"
#include "SkyLatLong.h"

#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

namespace Exr
{
    int WriterSelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("exr writer: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        // IEEE half bit patterns, worked by hand: 1.0 = 0x3c00, 0.5 = 0x3800, -2 = 0xc000, the largest 65504 = 0x7bff, the smallest normal 2^-14 = 0x0400, the smallest subnormal 2^-24 = 0x0001,
        // 1 + 2^-10 = 0x3c01; 1 + 2^-11 is a tie and goes to the even 0x3c00, 1 + 3 * 2^-11 is a tie and goes to the even 0x3c02
        Check(FloatToHalf(1.0f) == 0x3c00, "1.0 is 0x3c00");
        Check(FloatToHalf(0.5f) == 0x3800, "0.5 is 0x3800");
        Check(FloatToHalf(-2.0f) == 0xc000, "-2 is 0xc000");
        Check(FloatToHalf(65504.0f) == 0x7bff && FloatToHalf(1.0e9f) == 0x7bff && FloatToHalf(-1.0e9f) == 0xfbff, "the largest half and the clamp past it");
        Check(FloatToHalf(6.103515625e-05f) == 0x0400, "the smallest normal half");
        Check(FloatToHalf(5.9604645e-08f) == 0x0001, "the smallest subnormal half");
        Check(FloatToHalf(1.0009765625f) == 0x3c01, "1 + 2^-10");
        Check(FloatToHalf(1.00048828125f) == 0x3c00, "a tie rounds to even (down)");
        Check(FloatToHalf(1.00146484375f) == 0x3c02, "a tie rounds to even (up)");
        Check(FloatToHalf(0.0f) == 0 && FloatToHalf(std::nanf("")) == 0, "zero and NaN are zero");
        // every half survives half -> float -> half (the reader's HalfToFloat is exact)
        {
            size_t Bad = 0;
            for (uint32_t H = 0; H < 0x10000; H++)
            {
                if ((H & 0x7c00u) == 0x7c00u) continue;   // infinities and NaNs are clamped or zeroed, not kept
                if (FloatToHalf(HalfToFloat(static_cast<uint16_t>(H))) != H && !(H == 0x8000)) Bad++;
            }
            Check(Bad == 0, "every finite half round-trips through float", std::format("{} bad", Bad));
        }
        // a file through the reader, both codecs
        const int W = 37, H = 11;
        std::vector<uint16_t> Img(static_cast<size_t>(W) * H * 3);
        uint32_t Seed = 12345;
        for (auto& P : Img)
        {
            Seed = Seed * 1664525u + 1013904223u;
            P = FloatToHalf(static_cast<float>(Seed >> 8) / 16777216.0f * 40.0f - 5.0f);
        }
        for (const WriteCompression Method : { WriteCompression::None, WriteCompression::Zips })
        {
            std::vector<uint8_t> File;
            std::string Why;
            const char* Label = Method == WriteCompression::Zips ? "ZIPS" : "NONE";
            Check(WriteHalfRgb(W, H, Img, Method, File, Why), Label, Why);
            Check(File.size() > 8 && File[0] == 0x76 && File[1] == 0x2f && File[2] == 0x31 && File[3] == 0x01 && File[4] == 2, "the magic and version 2");
            Reader R;
            const Status Open = Reader::Open(ByteView(File.data(), File.size()), R);
            Check(Open.Ok(), "the reader opens the file", Open.Message);
            if (!Open.Ok()) continue;
            Check(R.GetInfo().Width == W && R.GetInfo().Height == H && R.GetInfo().Channels.size() == 3 && R.GetInfo().Channels[0].Name == "B" && R.GetInfo().Channels[2].Name == "R" &&
                      R.GetInfo().Method == (Method == WriteCompression::Zips ? Compression::Zips : Compression::None),
                  "size, channels B G R and the codec read back", Label);
            Pixels Out;
            ReadOptions Options;
            Options.IncludeAlpha = false, Options.Sanitize = false;
            const Status Read = R.ReadDownsampled(W, H, Options, Out);
            Check(Read.Ok() && Out.Width == W && Out.Height == H && Out.ChannelCount == 3, "the reader decodes it", Read.Message);
            if (!Read.Ok()) continue;
            size_t Bad = 0;
            for (size_t K = 0; K < Img.size(); K++)
                if (Out.Data[K] != HalfToFloat(Img[K])) Bad++;
            Check(Bad == 0, "every sample reads back exactly", std::format("{} {} bad", Label, Bad));
        }
        // a smooth image must compress (ZIPS shrinks a ramp a great deal: the pre-filter is what makes it so)
        {
            std::vector<uint16_t> Ramp(256 * 8 * 3);
            for (size_t K = 0; K < Ramp.size(); K++) Ramp[K] = FloatToHalf(static_cast<float>((K / 3) % 256) / 256.0f);
            std::vector<uint8_t> None, Zips;
            std::string Why;
            WriteHalfRgb(256, 8, Ramp, WriteCompression::None, None, Why);
            WriteHalfRgb(256, 8, Ramp, WriteCompression::Zips, Zips, Why);
            Check(Zips.size() * 2 < None.size(), "ZIPS shrinks a ramp to under half", std::format("{} of {}", Zips.size(), None.size()));
            // and it reads back exactly: a compressible scanline is stored deflated (an incompressible one, like the random image above, is stored raw), so this is the check of the pre-filter
            Reader R;
            Pixels Out;
            ReadOptions Options;
            Options.IncludeAlpha = false, Options.Sanitize = false;
            const Status Open = Reader::Open(ByteView(Zips.data(), Zips.size()), R);
            const Status Read = Open.Ok() ? R.ReadDownsampled(256, 8, Options, Out) : Open;
            size_t Bad = Out.Data.size() == Ramp.size() ? 0 : 1;
            for (size_t K = 0; Read.Ok() && K < Ramp.size() && K < Out.Data.size(); K++)
                if (Out.Data[K] != HalfToFloat(Ramp[K])) Bad++;
            Check(Read.Ok() && Bad == 0, "a deflated ramp reads back exactly", std::format("{} {} bad", Read.Message, Bad));
        }
        std::vector<uint8_t> Sink;
        std::string Why;
        Check(!WriteHalfRgb(0, 4, Img, WriteCompression::None, Sink, Why), "a zero width is refused");
        Check(!WriteHalfRgb(W, H, std::span<const uint16_t>(Img).first(10), WriteCompression::None, Sink, Why), "a short pixel span is refused");
        return Failed;
    }
}

namespace SkyConv
{
    namespace
    {
        // Direct3D's face directions as the table in sky-axes.md gives them for u across and v down, both in -1..1
        void TableDirection(int Face, double U, double V, double (&D)[3])
        {
            switch (Face)
            {
            case 0: D[0] = 1, D[1] = -V, D[2] = -U; break;
            case 1: D[0] = -1, D[1] = -V, D[2] = U; break;
            case 2: D[0] = U, D[1] = 1, D[2] = V; break;
            case 3: D[0] = U, D[1] = -1, D[2] = -V; break;
            case 4: D[0] = U, D[1] = -V, D[2] = 1; break;
            default: D[0] = -U, D[1] = -V, D[2] = -1; break;
            }
        }

        // the Black Ops III sky shader's mapping, forward: direction -> (u, v)
        void ShaderTexCoords(const double (&C)[3], double& U, double& V)
        {
            U = std::atan2(C[1], C[0]) / (-2.0 * std::numbers::pi) + 0.5;
            V = std::acos(C[2]) / std::numbers::pi;
        }

        Cube EncodedCube(int N)
        {
            Cube C;
            C.Size = N;
            for (int F = 0; F < 6; F++)
            {
                C.Rgb[static_cast<size_t>(F)].resize(static_cast<size_t>(N) * N * 3);
                for (int Y = 0; Y < N; Y++)
                    for (int X = 0; X < N; X++)
                    {
                        double D[3];
                        TableDirection(F, 2.0 * (X + 0.5) / N - 1.0, 2.0 * (Y + 0.5) / N - 1.0, D);
                        const double L = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
                        for (size_t A = 0; A < 3; A++) C.Rgb[static_cast<size_t>(F)][(static_cast<size_t>(Y) * N + X) * 3 + A] = static_cast<float>((D[A] / L + 1.0) * 0.5);
                    }
            }
            return C;
        }

        bool Pixel(const std::vector<uint16_t>& Img, int W, int H, const double (&C)[3], float (&Rgb)[3])
        {
            double U, V;
            ShaderTexCoords(C, U, V);
            const int I = std::min(W - 1, static_cast<int>(U * W)), J = std::min(H - 1, static_cast<int>(V * H));
            for (size_t A = 0; A < 3; A++) Rgb[A] = Exr::HalfToFloat(Img[(static_cast<size_t>(J) * W + I) * 3 + A]);
            return true;
        }

        void Unit(double (&V)[3])
        {
            const double L = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
            for (double& X : V) X /= L;
        }
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("sky convert: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        // directions to test: a seeded spread over the sphere
        std::vector<std::array<double, 3>> Dirs;
        {
            uint32_t Seed = 99;
            const auto Rnd = [&] {
                Seed = Seed * 1664525u + 1013904223u;
                return static_cast<double>(Seed >> 8) / 16777216.0 * 2.0 - 1.0;
            };
            while (Dirs.size() < 400)
            {
                std::array<double, 3> D = { Rnd(), Rnd(), Rnd() };
                const double L = std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
                if (L < 0.2 || L > 1.0) continue;
                for (double& X : D) X /= L;
                Dirs.push_back(D);
            }
        }
        const int W = 128, H = 64;
        // 1. a cube whose every texel holds its own direction, resampled: the lat-long pixel that the shader's forward formula picks for a direction holds that direction
        {
            const Cube C = EncodedCube(64);
            std::vector<uint16_t> Img;
            std::string Why;
            Check(ResampleToLatLong(C, CubeConvention(), 1.0f, W, H, Img, Why, 3), "resample", Why);
            size_t Bad = 0;
            double Worst = 0;
            for (const auto& D : Dirs)
            {
                float Rgb[3];
                const double Dir[3] = { D[0], D[1], D[2] };
                Pixel(Img, W, H, Dir, Rgb);
                double Got[3] = { Rgb[0] * 2.0 - 1.0, Rgb[1] * 2.0 - 1.0, Rgb[2] * 2.0 - 1.0 };
                Unit(Got);
                const double Deg = AngleBetween(Got, Dir);
                Worst = std::max(Worst, Deg);
                if (Deg > 3.5) Bad++;   // a pixel is 2.8 by 2.8 degrees: the picked pixel's centre is within 2 degrees, the cube's own texel adds a little
            }
            Check(Bad == 0, "a direction-coded cube comes out holding the direction each lat-long pixel looks along (shader formula)", std::format("{} of {} off, worst {:.2f} degrees", Bad, Dirs.size(), Worst));
            // hand cases from the shader text: +X is the image centre (u 0.5, v 0.5), +Y is a quarter in from the left (atan2 = pi/2, u = 0.25), -Y three quarters, +Z the top row, -Z the bottom
            struct Case
            {
                double Dir[3];
                int I, J;
            };
            const Case Cases[] = { { { 1, 0, 0 }, W / 2, H / 2 }, { { 0, 1, 0 }, W / 4, H / 2 }, { { 0, -1, 0 }, 3 * W / 4, H / 2 }, { { 0, 0, 1 }, W / 2, 0 }, { { 0, 0, -1 }, W / 2, H - 1 } };
            for (const Case& K : Cases)
            {
                const size_t At = (static_cast<size_t>(K.J) * W + static_cast<size_t>(K.I)) * 3;
                double Got[3] = { Exr::HalfToFloat(Img[At]) * 2.0 - 1.0, Exr::HalfToFloat(Img[At + 1]) * 2.0 - 1.0, Exr::HalfToFloat(Img[At + 2]) * 2.0 - 1.0 };
                Unit(Got);
                const double Deg = AngleBetween(Got, K.Dir);
                Check(Deg < 5.0, "a hand-placed direction sits where the shader's formula puts it", std::format("({}, {}, {}) is {:.1f} degrees off", K.Dir[0], K.Dir[1], K.Dir[2], Deg));
            }
        }
        // 2. a cube of one colour per face: the face a direction hits is the one of its largest component
        {
            Cube C;
            C.Size = 8;
            const float Colours[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 1, 0, 1 } };
            for (size_t F = 0; F < 6; F++)
            {
                C.Rgb[F].resize(8 * 8 * 3);
                for (size_t K = 0; K < 64; K++)
                    for (size_t A = 0; A < 3; A++) C.Rgb[F][K * 3 + A] = Colours[F][A];
            }
            std::vector<uint16_t> Img;
            std::string Why;
            ResampleToLatLong(C, CubeConvention(), 1.0f, W, H, Img, Why, 2);
            size_t Bad = 0, Tested = 0;
            for (const auto& D : Dirs)
            {
                const double Ax = std::fabs(D[0]), Ay = std::fabs(D[1]), Az = std::fabs(D[2]);
                const double Top = std::max({ Ax, Ay, Az });
                double Second = 0;
                for (const double X : { Ax, Ay, Az })
                    if (X != Top) Second = std::max(Second, X);
                if (Second > Top * 0.87) continue;   // within ~8 degrees of a seam: the pixel may straddle it
                const int Face = Ax == Top ? (D[0] > 0 ? 0 : 1) : Ay == Top ? (D[1] > 0 ? 2 : 3) : (D[2] > 0 ? 4 : 5);
                float Rgb[3];
                const double Dir[3] = { D[0], D[1], D[2] };
                Pixel(Img, W, H, Dir, Rgb);
                Tested++;
                for (size_t A = 0; A < 3; A++)
                    if (std::fabs(Rgb[A] - Colours[Face][A]) > 0.01f) { Bad++; break; }
            }
            Check(Bad == 0 && Tested > 100, "each direction shows the face of its largest component", std::format("{} of {} wrong", Bad, Tested));
        }
        // 3. another convention: world = (d3d.y, d3d.x, -d3d.z), so the pixel for world direction c holds the d3d direction (c.y, c.x, -c.z)
        {
            const Cube C = EncodedCube(64);
            CubeConvention Conv;
            Conv.Perm = { 1, 0, 2 };
            Conv.Sign = { 1, 1, -1 };
            std::vector<uint16_t> Img;
            std::string Why;
            ResampleToLatLong(C, Conv, 1.0f, W, H, Img, Why, 1);
            size_t Bad = 0;
            for (const auto& D : Dirs)
            {
                float Rgb[3];
                const double Dir[3] = { D[0], D[1], D[2] };
                Pixel(Img, W, H, Dir, Rgb);
                double Got[3] = { Rgb[0] * 2.0 - 1.0, Rgb[1] * 2.0 - 1.0, Rgb[2] * 2.0 - 1.0 };
                Unit(Got);
                const double Expected[3] = { D[1], D[0], -D[2] };
                if (AngleBetween(Got, Expected) > 3.5) Bad++;
            }
            Check(Bad == 0, "a permuted and signed convention turns the direction the cube is read along", std::format("{} wrong", Bad));
        }
        // 4. the scale multiplies, and the half range clamps
        {
            Cube C;
            C.Size = 4;
            for (auto& F : C.Rgb) F.assign(4 * 4 * 3, 2.0f);
            std::vector<uint16_t> Img;
            std::string Why;
            ResampleToLatLong(C, CubeConvention(), 30.0f, 16, 8, Img, Why, 1);
            Check(Exr::HalfToFloat(Img[0]) == 60.0f && Exr::HalfToFloat(Img[Img.size() - 1]) == 60.0f, "texels times the scale");
            ResampleToLatLong(C, CubeConvention(), 1.0e6f, 16, 8, Img, Why, 1);
            Check(Exr::HalfToFloat(Img[5]) == 65504.0f, "a product past the half range is clamped");
            Cube Bad;
            Bad.Size = 4;
            Check(!ResampleToLatLong(Bad, CubeConvention(), 1.0f, 16, 8, Img, Why, 1), "a cube with empty faces is refused");
        }
        // 5. the brightest texel: face -Y (3), texel (10, 5) of 32; its direction by the table
        {
            Cube C;
            C.Size = 32;
            for (auto& F : C.Rgb) F.assign(32 * 32 * 3, 0.01f);
            for (size_t A = 0; A < 3; A++) C.Rgb[3][(5 * 32 + 10) * 3 + A] = 50.0f;
            const Peak P = Brightest(C);
            double Expected[3];
            TableDirection(3, 2.0 * 10.5 / 32 - 1.0, 2.0 * 5.5 / 32 - 1.0, Expected);
            Check(P.Face == 3 && P.X == 10.5 && P.Y == 5.5 && AngleBetween(P.Dir, Expected) < 1e-6 && P.MaxChannel == 50.0, "the brightest texel and its direction");
        }
        // 5b. the brightest texel above the horizon: a hotter texel on the -Z face (below) is skipped by MinWorldZ 0, and a convention that flips z makes that face the upper one
        {
            Cube C;
            C.Size = 8;
            for (auto& F : C.Rgb) F.assign(8 * 8 * 3, 0.01f);
            for (size_t A = 0; A < 3; A++) C.Rgb[4][(3 * 8 + 3) * 3 + A] = 10.0f, C.Rgb[5][(3 * 8 + 3) * 3 + A] = 20.0f;
            Check(Brightest(C).Face == 5 && Brightest(C, CubeConvention(), 0.0).Face == 4, "the peak above the horizon skips a hotter texel below it");
            CubeConvention Flip;
            Flip.Sign = { 1, 1, -1 };
            Check(Brightest(C, Flip, 0.0).Face == 5, "a z-flipping convention makes the -Z face the upper one");
        }
        // 5c. no qualifying texel is "none", not the corner (0, 0): a black cube, and a cube lit only below the horizon asked for the upper hemisphere
        {
            Cube Black;
            Black.Size = 8;
            for (auto& F : Black.Rgb) F.assign(8 * 8 * 3, 0.0f);
            Check(!Brightest(Black).Found, "a black cube has no brightest texel");
            Cube Low = Black;
            for (size_t A = 0; A < 3; A++) Low.Rgb[5][(2 * 8 + 2) * 3 + A] = 4.0f;
            Check(Brightest(Low).Found && !Brightest(Low, CubeConvention(), 0.0).Found, "a cube lit only below the horizon has no texel above it");
        }
        // 5d. a cube over the face cap is refused with a note, not allocated
        {
            using namespace DirectX;
            ScratchImage Big;
            const size_t N = kMaxFace + 4;
            Blob File;
            // a header-only claim: a 2D array of 6 float faces would be 3 GB; the decoder must refuse on the metadata
            if (SUCCEEDED(Big.InitializeCube(DXGI_FORMAT_BC6H_UF16, N, N, 1, 1)) && SUCCEEDED(SaveToDDSMemory(Big.GetImages(), Big.GetImageCount(), Big.GetMetadata(), DDS_FLAGS_NONE, File)))
            {
                Cube Out;
                std::string Format, Why;
                Check(!DecodeCubeDds(std::span<const uint8_t>(static_cast<const uint8_t*>(File.GetBufferPointer()), File.GetBufferSize()), Out, Format, Why) && Why.find("over 4096") != std::string::npos && Out.Rgb[0].empty(),
                      "a cube over the face cap is refused before anything is allocated", Why);
            }
        }
        // 6. the size
        {
            int W2, H2;
            LatLongSize(2048, W2, H2);
            Check(W2 == 8192 && H2 == 4096, "a 2048 face gives 8192 by 4096");
            LatLongSize(256, W2, H2);
            Check(W2 == 1024 && H2 == 512, "a 256 face gives 1024 by 512");
            LatLongSize(4096, W2, H2);
            Check(W2 == 8192 && H2 == 4096, "a 4096 face is capped at the stock 8192 by 4096");
            LatLongSize(8, W2, H2);
            Check(W2 == 64 && H2 == 32, "a tiny face gives at least 64 by 32");
        }
        // 7. the cube DDS decoder: a float cube exactly, a BC6H cube within the codec's error
        {
            using namespace DirectX;
            const int N = 16;
            const Cube Src = EncodedCube(N);
            for (const bool Bc6 : { false, true })
            {
                ScratchImage Img;
                if (FAILED(Img.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, 1, 1))) { Check(false, "build a cube"); continue; }
                for (size_t F = 0; F < 6; F++)
                {
                    const Image* Face = Img.GetImage(0, F, 0);
                    for (int Y = 0; Y < N; Y++)
                    {
                        const std::span<uint8_t> RowBytes = std::span<uint8_t>(Face->pixels, Face->slicePitch).subspan(static_cast<size_t>(Y) * Face->rowPitch, static_cast<size_t>(N) * 16);
                        const std::span<float> Row(reinterpret_cast<float*>(RowBytes.data()), static_cast<size_t>(N) * 4);
                        for (int X = 0; X < N; X++)
                        {
                            for (size_t A = 0; A < 3; A++) Row[X * 4 + A] = Src.Rgb[F][(static_cast<size_t>(Y) * N + X) * 3 + A] * 3.0f;
                            Row[X * 4 + 3] = 1.0f;
                        }
                    }
                }
                ScratchImage Packed;
                if (Bc6 && FAILED(Compress(Img.GetImages(), Img.GetImageCount(), Img.GetMetadata(), DXGI_FORMAT_BC6H_UF16, TEX_COMPRESS_DEFAULT, 1.0f, Packed))) { Check(false, "compress BC6H"); continue; }
                Blob File;
                const ScratchImage& Use = Bc6 ? Packed : Img;
                if (FAILED(SaveToDDSMemory(Use.GetImages(), Use.GetImageCount(), Use.GetMetadata(), DDS_FLAGS_NONE, File))) { Check(false, "save the DDS"); continue; }
                Cube Out;
                std::string Format, Why;
                const bool Ok = DecodeCubeDds(std::span<const uint8_t>(static_cast<const uint8_t*>(File.GetBufferPointer()), File.GetBufferSize()), Out, Format, Why);
                Check(Ok && Out.Size == N, Bc6 ? "a BC6H cube decodes" : "a float cube decodes", Why + " " + Format);
                if (!Ok) continue;
                double Worst = 0;
                for (size_t F = 0; F < 6; F++)
                    for (size_t K = 0; K < Out.Rgb[F].size(); K++) Worst = std::max(Worst, static_cast<double>(std::fabs(Out.Rgb[F][K] - Src.Rgb[F][K] * 3.0f)));
                Check(Worst <= (Bc6 ? 0.4 : 0.0), Bc6 ? "BC6H texels within the codec's error of the source" : "float texels exactly", std::format("worst {:.4f}", Worst));
            }
            // a 2D image is no cube
            ScratchImage Flat;
            Flat.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 8, 8, 1, 1);
            Blob File;
            SaveToDDSMemory(Flat.GetImages(), Flat.GetImageCount(), Flat.GetMetadata(), DDS_FLAGS_NONE, File);
            Cube Out;
            std::string Format, Why;
            Check(!DecodeCubeDds(std::span<const uint8_t>(static_cast<const uint8_t*>(File.GetBufferPointer()), File.GetBufferSize()), Out, Format, Why), "a flat image is not a cube");
        }
        return Failed;
    }
}
