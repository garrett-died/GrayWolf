#include "stdafx.h"

#include "MapFullExport.h"
#include "MapMesh.h"
#include "MapRadiantRules.h"
#include "MapRadiantTextures.h"

#include <cmath>
#include <limits>
#include <format>
#include <string>
#include <vector>

// Self-test of the brush side texturing (M2d): the first fit, the second fit (HydraX RenderFit's selection, BrushFaceUv::Solve numbers, front-facing triangles only), the seven decimal
// face numbers and the StrictMaterials trait.
namespace Maps
{
namespace
{
    using V3 = DrawnWorld::V3;

    // a 64 x 64 square on z = 0 as two triangles of Material, its corner (64, 64) lifted by Lift, vertex normals along Facing, texture coordinates from Numbers (BrushFaceUv, z-facing)
    Surface Square(const std::string& Material, double Lift, float Facing, const BrushFaceNumbers& Numbers)
    {
        Surface S;
        S.Material = Material;
        const float P[4][3] = { { 0, 0, 0 }, { 64, 0, 0 }, { 64, 64, static_cast<float>(Lift) }, { 0, 64, 0 } };
        for (const auto& C : P)
        {
            Vertex V;
            V.Pos[0] = C[0], V.Pos[1] = C[1], V.Pos[2] = C[2];
            V.Normal[0] = 0, V.Normal[1] = 0, V.Normal[2] = Facing;
            V.NormalRead = true;
            const auto [U, W] = BrushFaceUv::Forward({ 0, 0, 0 }, { 0, 64, 0 }, { 64, 0, 0 }, Numbers, { C[0], C[1], C[2] });
            V.Uv[0] = static_cast<float>(U), V.Uv[1] = static_cast<float>(W);
            S.Vertices.push_back(V);
        }
        S.Indices = { 0, 1, 2, 0, 2, 3 };
        return S;
    }

    // rectangles on z = 0 (their corner (x1, y1) lifted by Lift), each its own two triangles of one surface; Du offsets the u of a rectangle
    struct Quad
    {
        double X0, X1, Y0, Y1, Du;
    };
    Surface Rects(const std::string& Material, double Lift, const BrushFaceNumbers& Numbers, const std::vector<Quad>& Quads)
    {
        Surface S;
        S.Material = Material;
        for (const Quad& Q : Quads)
        {
            const float P[4][3] = { { static_cast<float>(Q.X0), static_cast<float>(Q.Y0), 0 }, { static_cast<float>(Q.X1), static_cast<float>(Q.Y0), 0 },
                                    { static_cast<float>(Q.X1), static_cast<float>(Q.Y1), static_cast<float>(Lift) }, { static_cast<float>(Q.X0), static_cast<float>(Q.Y1), 0 } };
            const uint32_t Base = static_cast<uint32_t>(S.Vertices.size());
            for (const auto& C : P)
            {
                Vertex V;
                V.Pos[0] = C[0], V.Pos[1] = C[1], V.Pos[2] = C[2];
                V.Normal[2] = 1;
                V.NormalRead = true;
                const auto [U, W] = BrushFaceUv::Forward({ 0, 0, 0 }, { 0, 64, 0 }, { 64, 0, 0 }, Numbers, { C[0], C[1], C[2] });
                V.Uv[0] = static_cast<float>(U + Q.Du), V.Uv[1] = static_cast<float>(W);
                S.Vertices.push_back(V);
            }
            for (const uint32_t K : { 0u, 1u, 2u, 0u, 2u, 3u }) S.Indices.push_back(Base + K);
        }
        return S;
    }

    MapScene SceneOf(const std::vector<Surface>& Surfaces)
    {
        MapScene Scene;
        Mesh M;
        M.Name = "world";
        M.Surfaces = Surfaces;
        Scene.Objects.push_back(M);
        return Scene;
    }

    const std::vector<V3> Face = { { 0, 0, 0 }, { 64, 0, 0 }, { 64, 64, 0 }, { 0, 64, 0 } };
}

int RadiantTexturesSelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* Name, const std::string& Got = std::string()) {
        if (Ok) return;
        Failed++;
        Failures += std::format("FAIL RadiantTextures {} (got '{}')\n", Name, Got);
    };
    const V3 Up = { 0, 0, 1 };
    const BrushFaceNumbers Plain = { 64, 64, 10, 5, 0, 0 };

    // ---- the first fit is untouched: a face drawn within the tolerance is fitted by it, the second fit is not tried
    {
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ Square("mat_a", 0, 1, Plain) }), Stats);
        SideTexture T;
        const bool Ok = World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats);
        Check(Ok && T.Material == "mat_a" && std::fabs(T.Width - 64) < 1e-6 && std::fabs(T.Height - 64) < 1e-6, "the first fit textures a face drawn on its plane", std::format("{} {} {}", Ok, T.Material, T.Width));
        Check(Stats.RefitTried == 0 && Stats.SidesTextured == 1, "the second fit is not tried when the first succeeds", std::format("{} {}", Stats.RefitTried, Stats.SidesTextured));
        Check(T.Shear == 0, "the first fit's shear is 0");
    }
    // ---- a corner 0.08 off the plane is beyond the first fit's 0.05 and inside the second's 0.1
    {
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ Square("mat_a", 0.08, 1, Plain) }), Stats);
        SideTexture T;
        const bool Ok = World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats);
        Check(Ok && T.Material == "mat_a", "a corner 0.08 off the plane is textured by the second fit", std::format("{}", Ok));
        Check(Stats.RefitTried == 1 && Stats.RefitTextured == 1 && Stats.SidesTextured == 1, "the second fit is counted", std::format("{} {} {}", Stats.RefitTried, Stats.RefitTextured, Stats.SidesTextured));
        Check(std::fabs(T.Width - 64) < 1e-3 && std::fabs(T.ShiftX - 10) < 1e-3, "the second fit's numbers are the drawn ones", std::format("{} {}", T.Width, T.ShiftX));
        Check(World.Removed(0, 0, 0) && World.Removed(0, 0, 1), "the triangles the second fit draws are removed");
        SideTexture Lifted;
        RadiantTextureStats Other;
        DrawnWorld Far(SceneOf({ Square("mat_a", 0.2, 1, Plain) }), Other);
        Check(!Far.Texture(Face, Up, 0, Up, 0, nullptr, Lifted, Other), "a corner 0.2 off the plane is textured by neither", std::format("{}", Other.RefitNone));
    }
    // ---- only triangles facing the side's way count (HydraX counts the back faces too: 139 of its 157 wrong materials on zm_giant)
    {
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ Square("mat_a", 0.08, -1, Plain) }), Stats);
        SideTexture T;
        const bool Ok = World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats);
        Check(!Ok && Stats.RefitNone == 1, "triangles facing against the side do not texture it", std::format("{} {}", Ok, Stats.RefitNone));
    }
    // ---- a shear is not the first fit's to express: the second fit solves it (BrushFaceUv)
    {
        RadiantTextureStats Stats;
        const BrushFaceNumbers Sheared = { 64, 64, 10, 5, 0, 0.25 };
        DrawnWorld World(SceneOf({ Square("mat_a", 0, 1, Sheared) }), Stats);
        SideTexture T;
        const bool Ok = World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats);
        Check(Ok && Stats.RefitTextured == 1 && std::fabs(T.Shear - 0.25) < 1e-4, "a sheared projection is solved by the second fit", std::format("{} {} {}", Ok, Stats.RefitTextured, T.Shear));
        Check(Stats.RefitSheared == 1, "a sheared side is counted", std::format("{}", Stats.RefitSheared));
    }
    // ---- two materials covering the face alike: a strict game leaves the side a tool, a game that is not strict takes the larger
    {
        const std::vector<Surface> Two = { Square("mat_a", 0.08, 1, Plain), Square("mat_b", 0.08, 1, Plain) };
        RadiantTextureStats Strict, Loose;
        DrawnWorld A(SceneOf(Two), Strict), B(SceneOf(Two), Loose);
        SideTexture T, U;
        Check(!A.Texture(Face, Up, 0, Up, 0, nullptr, T, Strict, true) && Strict.RefitAmbiguous == 1, "a strict game leaves an ambiguous face a tool", std::format("{}", Strict.RefitAmbiguous));
        Check(B.Texture(Face, Up, 0, Up, 0, nullptr, U, Loose, false) && Loose.RefitAmbiguous == 1, "a game that is not strict takes the larger material, and the ambiguity is counted", std::format("{}", Loose.RefitAmbiguous));
    }
    // ---- numbers that draw too little of the material's area on the face are not the face's: ten strips, two mapped another way (80% drawn the solved way: Solve takes it, the gate refuses)
    {
        std::vector<Quad> Q;
        for (int K = 0; K < 10; K++) Q.push_back({ K * 6.4, (K + 1) * 6.4, 0, 64, (K == 3 || K == 7) ? 0.37 : 0.0 });
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ Rects("mat_a", 0.08, Plain, Q) }), Stats);
        SideTexture T;
        Check(!World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats) && Stats.RefitMixed == 1 && Stats.RefitNoSolve == 0, "numbers drawing 80% of the area leave the side a tool (the gate)",
              std::format("mixed {} no_solve {} textured {}", Stats.RefitMixed, Stats.RefitNoSolve, Stats.SidesTextured));
        Check(!World.Removed(0, 0, 0), "nothing is removed for a refused side");
        // 95% drawn the solved way (twenty strips, one other): textured, counted as between
        std::vector<Quad> P;
        for (int K = 0; K < 20; K++) P.push_back({ K * 3.2, (K + 1) * 3.2, 0, 64, K == 5 ? 0.37 : 0.0 });
        RadiantTextureStats Stats2;
        DrawnWorld World2(SceneOf({ Rects("mat_a", 0.08, Plain, P) }), Stats2);
        SideTexture T2;
        Check(World2.Texture(Face, Up, 0, Up, 0, nullptr, T2, Stats2) && Stats2.RefitGateBetween == 1, "95% drawn the solved way is textured and counted between", std::format("{} {}", Stats2.RefitGateBetween, Stats2.RefitMixed));
    }
    // ---- no whole triangle on the face: the numbers cannot be verified, the side stays a tool (a mesh over a smaller face would be drawn twice)
    {
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ Rects("mat_a", 0.08, { 64, 64, 10, 5, 0, 0.25 }, { { 0, 128, 0, 128, 0 } }) }), Stats);
        SideTexture T;
        Check(!World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats) && Stats.RefitNoWhole == 1 && !World.Removed(0, 0, 0), "a mesh larger than the face verifies nothing", std::format("{} {}", Stats.RefitNoWhole, Stats.SidesTextured));
    }
    // ---- half of the face drawn and the rest in the open: the first fit's hidden test applies to the second fit too
    {
        const std::vector<Quad> Half = { { 0, 64, 0, 36, 0 } };
        RadiantTextureStats Open, Hid;
        DrawnWorld A(SceneOf({ Rects("mat_a", 0.08, Plain, Half) }), Open), B(SceneOf({ Rects("mat_a", 0.08, Plain, Half) }), Hid);
        SideTexture T, U;
        const DrawnWorld::HiddenTest Nothing = [](const V3&) { return false; }, Everything = [](const V3&) { return true; };
        Check(!A.Texture(Face, Up, 0, Up, 0, &Nothing, T, Open) && Open.RefitUndrawn == 1, "the exposed rest of a half drawn face leaves it a tool", std::format("{}", Open.RefitUndrawn));
        Check(B.Texture(Face, Up, 0, Up, 0, &Everything, U, Hid), "the same face with the rest hidden by another brush is textured");
    }
    // ---- a NaN in v alone is a miss (std::max of a finite and a NaN keeps the finite one): twenty strips, one with a NaN v, are 95% matched, not 100%
    {
        std::vector<Quad> Q;
        for (int K = 0; K < 20; K++) Q.push_back({ K * 3.2, (K + 1) * 3.2, 0, 64, 0.0 });
        Surface S = Rects("mat_a", 0.08, Plain, Q);
        S.Vertices[4 * 5].Uv[1] = std::nanf("");
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ S }), Stats);
        SideTexture T;
        Check(World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats) && Stats.RefitGateBetween == 1, "a NaN v is a miss", std::format("between {} mixed {}", Stats.RefitGateBetween, Stats.RefitMixed));
    }
    // ---- two coincident triangles over half of a face add up to more than the face: the exposed half is still found by the sample points
    {
        const std::vector<Quad> Twice = { { 0, 64, 0, 36, 0 }, { 0, 64, 0, 36, 0 } };
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ Rects("mat_a", 0.08, Plain, Twice) }), Stats);
        SideTexture T;
        const DrawnWorld::HiddenTest Nothing = [](const V3&) { return false; };
        Check(!World.Texture(Face, Up, 0, Up, 0, &Nothing, T, Stats) && Stats.RefitUndrawn == 1, "coincident triangles do not cover the exposed half", std::format("{}", Stats.RefitUndrawn));
    }
    // ---- a face 100 by 0.1: the hidden test samples across the narrow axis too (a fixed 0.5 step gave no sample there and passed on nothing)
    {
        const std::vector<V3> Thin = { { 0, 0, 0 }, { 100, 0, 0 }, { 100, 0.1, 0 }, { 0, 0.1, 0 } };
        Surface S = Rects("mat_a", 0, Plain, { { 0, 60, 0, 0.1, 0 } });
        for (Vertex& V : S.Vertices) V.Pos[2] = 0.07f;   // beyond the first fit's 0.05, inside the second's 0.1
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ S }), Stats);
        SideTexture T;
        const DrawnWorld::HiddenTest Nothing = [](const V3&) { return false; };
        Check(!World.Texture(Thin, Up, 0, Up, 0, &Nothing, T, Stats) && Stats.RefitUndrawn == 1, "a thin face with 40% exposed is refused", std::format("{} {}", Stats.RefitUndrawn, Stats.RefitTextured));
    }
    // ---- a triangle one side took is not claimed by the next
    {
        RadiantTextureStats Stats;
        DrawnWorld World(SceneOf({ Square("mat_a", 0.08, 1, Plain) }), Stats);
        SideTexture T, U;
        Check(World.Texture(Face, Up, 0, Up, 0, nullptr, T, Stats) && !World.Texture(Face, Up, 0, Up, 0, nullptr, U, Stats) && Stats.RefitNone == 1, "a taken triangle is not claimed again", std::format("{}", Stats.RefitNone));
    }
    // ---- a side's numbers are written with seven decimals, expected text fixed by hand
    {
        SideTexture T;
        T.Width = 64.12345678, T.Height = -0.5, T.ShiftX = 0.00000004, T.ShiftY = 10, T.Rotation = 90, T.Shear = 0.00012346;
        Check(RadiantSideNumbers(T) == " 64.1234568 -0.5 0 10 90 0.0001235", "the side numbers text", RadiantSideNumbers(T));
    }
    // ---- the face numbers are written with seven decimals (four cost a texel at 1024 units)
    Check(RadiantNumber7(0.12345678) == "0.1234568", "seven decimals", RadiantNumber7(0.12345678));
    Check(RadiantNumber7(64) == "64" && RadiantNumber7(-0.00000001) == "0" && RadiantNumber7(-31.25) == "-31.25", "whole numbers and trailing zeros", RadiantNumber7(-31.25));
    Check(RadiantNumber(0.12345678) == "0.1235", "the other numbers keep four decimals", RadiantNumber(0.12345678));
    // ---- the trait: HydraX has Black Ops 4 strict and Black Ops III not; Modern Warfare Remastered measured strict (docs/hydrax-port/m2d.md)
    Check(StrictMaterials("bo4") && !StrictMaterials("bo3") && StrictMaterials("mwr") && StrictMaterials("BO4"), "StrictMaterials per game");
    return Failed;
}
}
