#include "stdafx.h"

#include "SkyLatLong.h"

#include "ExrWriter.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <thread>

namespace SkyConv
{
    namespace
    {
        struct Frame
        {
            double Fwd[3], Right[3], Up[3];
        };
        // Direct3D's cube faces (sky-axes.md): +X (1,-v,-u), -X (-1,-v,u), +Y (u,1,v), -Y (u,-1,-v), +Z (u,-v,1), -Z (-u,-v,-1) for u across and v down, so with t = -v up
        constexpr Frame kFrames[6] = {
            { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
            { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
            { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
            { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
        };

        double Dot(const double (&A)[3], const double (&B)[3]) { return A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; }
        double Lum(float R, float G, float B) { return 0.2126 * R + 0.7152 * G + 0.0722 * B; }
    }

    void CubeConvention::ToWorld(const double (&D3d)[3], double (&World)[3]) const
    {
        for (size_t I = 0; I < 3; I++) World[I] = Sign[I] * D3d[static_cast<size_t>(Perm[I])];
    }

    void CubeConvention::ToD3d(const double (&World)[3], double (&D3d)[3]) const
    {
        for (size_t I = 0; I < 3; I++) D3d[static_cast<size_t>(Perm[I])] = Sign[I] * World[I];
    }

    void TexelDirection(int Face, int Size, double X, double Y, double (&Dir)[3])
    {
        const Frame& F = kFrames[Face];
        const double S = 2.0 * X / Size - 1.0, T = 1.0 - 2.0 * Y / Size;
        for (size_t A = 0; A < 3; A++) Dir[A] = F.Fwd[A] + S * F.Right[A] + T * F.Up[A];
    }

    bool DirectionToTexel(const double (&Dir)[3], int Size, int& Face, double& X, double& Y)
    {
        const double Ax = std::fabs(Dir[0]), Ay = std::fabs(Dir[1]), Az = std::fabs(Dir[2]);
        if (Ax == 0 && Ay == 0 && Az == 0) return false;
        if (Ax >= Ay && Ax >= Az) Face = Dir[0] > 0 ? 0 : 1;
        else if (Ay >= Az) Face = Dir[1] > 0 ? 2 : 3;
        else Face = Dir[2] > 0 ? 4 : 5;
        const Frame& F = kFrames[Face];
        const double Along = Dot(Dir, F.Fwd);
        const double S = Dot(Dir, F.Right) / Along, T = Dot(Dir, F.Up) / Along;
        X = (S + 1.0) * 0.5 * Size;
        Y = (1.0 - T) * 0.5 * Size;
        return true;
    }

    void LatLongDirection(int I, int J, int W, int H, double (&Dir)[3])
    {
        const double U = (I + 0.5) / W, V = (J + 0.5) / H;
        const double Lon = (0.5 - U) * 2.0 * std::numbers::pi, Colat = V * std::numbers::pi;
        Dir[0] = std::sin(Colat) * std::cos(Lon);
        Dir[1] = std::sin(Colat) * std::sin(Lon);
        Dir[2] = std::cos(Colat);
    }

    void LatLongSize(int FaceSize, int& W, int& H)
    {
        W = std::clamp(FaceSize * 4, 64, 8192);
        H = W / 2;
    }

    bool ResampleToLatLong(const Cube& C, const CubeConvention& Conv, float Scale, int W, int H, std::vector<uint16_t>& HalfRgb, std::string& Why, int Threads)
    {
        if (C.Size < 2 || W < 2 || H < 2)
        {
            Why = "the cube or the lat-long is smaller than 2 texels";
            return false;
        }
        const size_t Face = static_cast<size_t>(C.Size) * static_cast<size_t>(C.Size) * 3;
        for (const auto& F : C.Rgb)
            if (F.size() != Face)
            {
                Why = "a cube face does not hold size * size * 3 floats";
                return false;
            }
        HalfRgb.assign(static_cast<size_t>(W) * static_cast<size_t>(H) * 3, 0);
        const int N = C.Size;
        const auto Fetch = [&](int Fc, int X, int Y, float (&Out)[3]) {
            X = std::clamp(X, 0, N - 1), Y = std::clamp(Y, 0, N - 1);
            const std::span<const float> P = std::span<const float>(C.Rgb[static_cast<size_t>(Fc)]).subspan((static_cast<size_t>(Y) * static_cast<size_t>(N) + static_cast<size_t>(X)) * 3, 3);
            Out[0] = P[0], Out[1] = P[1], Out[2] = P[2];
        };
        const auto Rows = [&](int From, int To) {
            for (int J = From; J < To; J++)
                for (int I = 0; I < W; I++)
                {
                    double World[3], D3d[3];
                    LatLongDirection(I, J, W, H, World);
                    Conv.ToD3d(World, D3d);
                    int Fc = 0;
                    double X = 0, Y = 0;
                    if (!DirectionToTexel(D3d, N, Fc, X, Y)) continue;
                    // bilinear between the four texels around (X, Y), a texel's centre at +0.5
                    const double Fx = X - 0.5, Fy = Y - 0.5;
                    const int X0 = static_cast<int>(std::floor(Fx)), Y0 = static_cast<int>(std::floor(Fy));
                    const float Tx = static_cast<float>(Fx - X0), Ty = static_cast<float>(Fy - Y0);
                    float A[3], B[3], Cc[3], D[3];
                    Fetch(Fc, X0, Y0, A), Fetch(Fc, X0 + 1, Y0, B), Fetch(Fc, X0, Y0 + 1, Cc), Fetch(Fc, X0 + 1, Y0 + 1, D);
                    const std::span<uint16_t> Out = std::span<uint16_t>(HalfRgb).subspan((static_cast<size_t>(J) * static_cast<size_t>(W) + static_cast<size_t>(I)) * 3, 3);
                    for (size_t K = 0; K < 3; K++)
                    {
                        const float Top = A[K] + (B[K] - A[K]) * Tx, Bottom = Cc[K] + (D[K] - Cc[K]) * Tx;
                        Out[K] = Exr::FloatToHalf((Top + (Bottom - Top) * Ty) * Scale);
                    }
                }
        };
        const int Count = std::max(1, Threads > 0 ? Threads : static_cast<int>(std::thread::hardware_concurrency()));
        std::vector<std::thread> Pool;
        for (int T = 0; T < Count; T++)
        {
            const int From = static_cast<int>(static_cast<int64_t>(H) * T / Count), To = static_cast<int>(static_cast<int64_t>(H) * (T + 1) / Count);
            Pool.emplace_back(Rows, From, To);
        }
        for (auto& T : Pool) T.join();
        return true;
    }

    Peak Brightest(const Cube& C, const CubeConvention& Conv, double MinWorldZ)
    {
        Peak P;
        if (C.Size < 1) return P;
        double Sum = 0;
        size_t Count = 0;
        for (int Fc = 0; Fc < 6; Fc++)
        {
            const auto& F = C.Rgb[static_cast<size_t>(Fc)];
            for (size_t K = 0; K + 2 < F.size(); K += 3)
            {
                const double L = Lum(F[K], F[K + 1], F[K + 2]);
                Sum += L, Count++;
                P.MaxChannel = std::max({ P.MaxChannel, static_cast<double>(F[K]), static_cast<double>(F[K + 1]), static_cast<double>(F[K + 2]) });
                if (L > P.Luminance)
                {
                    const size_t Texel = K / 3;
                    const double X = static_cast<double>(Texel % static_cast<size_t>(C.Size)) + 0.5, Y = static_cast<double>(Texel / static_cast<size_t>(C.Size)) + 0.5;
                    if (MinWorldZ > -1.5)
                    {
                        double D3d[3], World[3];
                        TexelDirection(Fc, C.Size, X, Y, D3d);
                        Conv.ToWorld(D3d, World);
                        if (World[2] / std::sqrt(Dot(World, World)) < MinWorldZ) continue;
                    }
                    P.Luminance = L, P.Face = Fc, P.X = X, P.Y = Y, P.Found = true;
                }
            }
        }
        P.Mean = Count != 0 ? Sum / static_cast<double>(Count) : 0;
        if (!P.Found) return P;
        TexelDirection(P.Face, C.Size, P.X, P.Y, P.Dir);
        const double Len = std::sqrt(Dot(P.Dir, P.Dir));
        for (double& V : P.Dir) V /= Len;
        return P;
    }

    double AngleBetween(const double (&A)[3], const double (&B)[3])
    {
        const double La = std::sqrt(Dot(A, A)), Lb = std::sqrt(Dot(B, B));
        if (La == 0 || Lb == 0) return 180.0;
        return std::acos(std::clamp(Dot(A, B) / (La * Lb), -1.0, 1.0)) * 180.0 / std::numbers::pi;
    }
}
