#include "stdafx.h"

#include "MapSunProbes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <sstream>

namespace fs = std::filesystem;

namespace Maps
{
    namespace
    {
        std::string Num(double V)
        {
            if (std::fabs(V - std::round(V)) < 1e-9) return std::format("{}", static_cast<long long>(std::llround(V)));
            std::string S = std::format("{:.4f}", V);
            while (!S.empty() && S.back() == '0') S.pop_back();
            if (!S.empty() && S.back() == '.') S.pop_back();
            return S;
        }

        using Point = std::array<double, 3>;

        // One face of the box: three points on the plane whose (p1 - p0) x (p2 - p0) points INTO the box (the stock volume_sun's order: its bottom face has +Z)
        std::string Face(const Point& P0, const Point& P1, const Point& P2, const char* Material = "sun_volume")
        {
            const auto Pt = [](const Point& P) { return "( " + Num(P[0]) + " " + Num(P[1]) + " " + Num(P[2]) + " )"; };
            return " " + Pt(P0) + " " + Pt(P1) + " " + Pt(P2) + " " + Material + " 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0\n";
        }
    }

    std::array<SkySlab, 6> SkyShell(const std::array<double, 3>& A, const std::array<double, 3>& B, double T)
    {
        return { { { { A[0] - T, A[1] - T, A[2] - T }, { A[0], B[1] + T, B[2] + T } },
                   { { B[0], A[1] - T, A[2] - T }, { B[0] + T, B[1] + T, B[2] + T } },
                   { { A[0], A[1] - T, A[2] - T }, { B[0], A[1], B[2] + T } },
                   { { A[0], B[1], A[2] - T }, { B[0], B[1] + T, B[2] + T } },
                   { { A[0], A[1], A[2] - T }, { B[0], B[1], A[2] } },
                   { { A[0], A[1], B[2] }, { B[0], B[1], B[2] + T } } } };
    }

    std::string SkyShellEntity(size_t Entity, const std::array<double, 3>& A, const std::array<double, 3>& B, double T)
    {
        std::string Out = std::format("// entity {}\n{{\n\"classname\" \"func_group\"\n", Entity);
        size_t N = 0;
        for (const SkySlab& S : SkyShell(A, B, T))
        {
            Out += std::format("// brush {}\n{{\n", N++);
            const double X0 = S.Lo[0], Y0 = S.Lo[1], Z0 = S.Lo[2], X1 = S.Hi[0], Y1 = S.Hi[1], Z1 = S.Hi[2];
            using Triple = std::array<Point, 3>;
            const Triple Faces[6] = { { { { X0, Y0, Z0 }, { X1, Y0, Z0 }, { X1, Y1, Z0 } } }, { { { X0, Y0, Z1 }, { X0, Y1, Z1 }, { X1, Y1, Z1 } } },
                                      { { { X0, Y0, Z0 }, { X0, Y1, Z0 }, { X0, Y1, Z1 } } }, { { { X1, Y0, Z0 }, { X1, Y0, Z1 }, { X1, Y1, Z1 } } },
                                      { { { X0, Y0, Z0 }, { X0, Y0, Z1 }, { X1, Y0, Z1 } } }, { { { X0, Y1, Z0 }, { X1, Y1, Z0 }, { X1, Y1, Z1 } } } };
            for (const Triple& F : Faces) Out += Face(F[0], F[1], F[2], "sky");
            Out += "}\n";
        }
        Out += "}\n";
        return Out;
    }

    bool SafeText(const std::string& S)
    {
        if (S.empty() || S.size() > 128) return false;
        return std::all_of(S.begin(), S.end(), [](char C) { return std::isalnum(static_cast<unsigned char>(C)) || C == '_' || C == '.' || C == '/' || C == '-'; });
    }

    bool SunAngles(std::span<const float, 3> Toward, double& Pitch, double& Yaw)
    {
        const double X = Toward[0], Y = Toward[1], Z = Toward[2];
        const double L = std::sqrt(X * X + Y * Y + Z * Z);
        if (!std::isfinite(L) || L < 1e-6) return false;
        const double Elevation = std::asin(std::clamp(Z / L, -1.0, 1.0)) * 180.0 / std::numbers::pi;
        Pitch = 180.0 - Elevation;
        if (Pitch > 180.0) Pitch -= 360.0;
        Yaw = std::hypot(X, Y) < 1e-6 * L ? 0.0 : std::atan2(Y, X) * 180.0 / std::numbers::pi;
        if (Yaw < 0) Yaw += 360.0;
        return true;
    }

    std::string SsiGdtEntry(const SsiEntry& E)
    {
        if (!SafeText(E.Name) || (!E.SkyboxModel.empty() && !SafeText(E.SkyboxModel))) return std::string();
        Gdt::Fields F = E.Stock;
        F.push_back({ "enablesun", "1" });
        if (E.HasColor) F.push_back({ "colorSRGB", Num(E.ColorSRGB[0]) + " " + Num(E.ColorSRGB[1]) + " " + Num(E.ColorSRGB[2]) + " 1" });
        F.push_back({ "pitch", Num(E.Pitch) });
        if (!E.SkyboxModel.empty()) F.push_back({ "skyboxmodel", E.SkyboxModel });
        F.push_back({ "type", "ssi" });
        F.push_back({ "yaw", Num(E.Yaw) });
        // APE writes an entry's keys sorted, case aside (bo3-verify's gdt.order)
        std::stable_sort(F.begin(), F.end(), [](const auto& A, const auto& B) {
            return std::lexicographical_compare(A.first.begin(), A.first.end(), B.first.begin(), B.first.end(), [](char X, char Y) { return std::tolower(static_cast<unsigned char>(X)) < std::tolower(static_cast<unsigned char>(Y)); });
        });
        return Gdt::Entry(E.Name, "ssi.gdf", F, "\r\n");
    }

    bool ParseSsiStock(const std::string& Json, SsiStock& Out, std::string& Why)
    {
        try
        {
            const nlohmann::json J = nlohmann::json::parse(Json);
            Out = SsiStock();
            Out.Source = J.value("source", std::string());
            Out.Entries = J.value("entries", static_cast<size_t>(0));
            if (J.contains("values"))
                for (const auto& [K, V] : J.at("values").items())
                    if (V.is_string() && SafeText(V.get<std::string>()) && SafeText(K)) Out.Values.push_back({ K, V.get<std::string>() });
            if (J.contains("stock_names"))
                for (const auto& N : J.at("stock_names")) Out.StockNames.push_back(N.get<std::string>());
        }
        catch (const nlohmann::json::exception& Ex)
        {
            Why = std::string("the stock ssi file is not readable: ") + Ex.what();
            return false;
        }
        if (Out.Values.empty())
        {
            Why = "the stock ssi file holds no value";
            return false;
        }
        return true;
    }

    bool LoadSsiStock(SsiStock& Out, std::string& Why)
    {
        wchar_t Buf[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, Buf, static_cast<DWORD>(std::size(Buf)));
        std::error_code Ec;
        // <exe dir>\zonekit\profiles, else a checkout above it
        for (fs::path P = fs::path(Buf).parent_path(); !P.empty(); P = P.parent_path())
        {
            const fs::path File = P / "zonekit" / "profiles" / "t7" / "target" / "ssi-stock-median.json";
            if (fs::exists(File, Ec))
            {
                std::ifstream In(File, std::ios::binary);
                std::stringstream Text;
                Text << In.rdbuf();
                return ParseSsiStock(Text.str(), Out, Why);
            }
            if (P == P.parent_path()) break;
        }
        Why = "zonekit/profiles/t7/target/ssi-stock-median.json was not found";
        return false;
    }

    bool CollisionBounds(const MapScene& Scene, std::array<float, 3>& Lo, std::array<float, 3>& Hi, const std::set<const Brush*>* Skip, std::array<std::string, 6>* Why)
    {
        bool Any = false;
        const char* Source = "collision soup vertex";
        const auto Take = [&](std::span<const float, 3> P) {
            for (size_t A = 0; A < 3; A++)
                if (!std::isfinite(P[A])) return;
            for (size_t A = 0; A < 3; A++)
            {
                if (!Any || P[A] < Lo[A])
                {
                    Lo[A] = P[A];
                    if (Why != nullptr) (*Why)[A * 2] = std::format("{} at ({:.0f}, {:.0f}, {:.0f})", Source, P[0], P[1], P[2]);
                }
                if (!Any || P[A] > Hi[A])
                {
                    Hi[A] = P[A];
                    if (Why != nullptr) (*Why)[A * 2 + 1] = std::format("{} at ({:.0f}, {:.0f}, {:.0f})", Source, P[0], P[1], P[2]);
                }
            }
            Any = true;
        };
        const Collision& C = Scene.Collision;
        const std::span<const float> Soup(C.Vertices);
        for (size_t K = 0; K + 2 < Soup.size(); K += 3) Take(Soup.subspan(K, 3).first<3>());
        for (const Brush& B : C.Brushes)
        {
            if (Skip != nullptr && Skip->count(&B) != 0) continue;
            Source = "world brush corner";
            const std::span<const float> Corners(B.Vertices);
            for (size_t K = 0; K + 2 < Corners.size(); K += 3) Take(Corners.subspan(K, 3).first<3>());
        }
        return Any;
    }

    bool WrittenBounds(const MapScene& Scene, std::array<float, 3>& Lo, std::array<float, 3>& Hi, std::array<std::string, 6>* Why, const char* SkipModelContaining, float SkipOriginBeyond,
                       const std::set<const Brush*>* SkipBrushes)
    {
        bool Any = CollisionBounds(Scene, Lo, Hi, SkipBrushes, Why);
        const auto Take = [&](const float (&P)[3], const auto& Label) {
            for (size_t A = 0; A < 3; A++)
                if (!std::isfinite(P[A])) return;
            for (size_t A = 0; A < 3; A++)
            {
                if (!Any || P[A] < Lo[A])
                {
                    Lo[A] = P[A];
                    if (Why != nullptr) (*Why)[A * 2] = Label(P);
                }
                if (!Any || P[A] > Hi[A])
                {
                    Hi[A] = P[A];
                    if (Why != nullptr) (*Why)[A * 2 + 1] = Label(P);
                }
            }
            Any = true;
        };
        const auto IsSky = [&](const std::string& Name) {
            std::string L = Name;
            for (char& C : L) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            if (L.find("sky") != std::string::npos) return true;
            const auto It = Scene.Materials.find(Name);
            if (It == Scene.Materials.end()) return false;
            std::string T = It->second.Techset;
            for (char& C : T) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            return T.find("sky") != std::string::npos;
        };
        for (const Mesh& M : Scene.Objects)
            for (const Surface& S : M.Surfaces)
            {
                if (IsSky(S.Material)) continue;
                for (const Vertex& V : S.Vertices)
                    Take(V.Pos, [&](const float (&P)[3]) { return std::format("render vertex of {} ({}) at ({:.0f}, {:.0f}, {:.0f})", M.Name, S.Material, P[0], P[1], P[2]); });
            }
        for (const Instance& I : Scene.StaticInstances)
        {
            if (SkipModelContaining != nullptr && I.Model.find(SkipModelContaining) != std::string::npos) continue;
            if (SkipOriginBeyond > 0.0f && (std::fabs(I.Origin[0]) >= SkipOriginBeyond || std::fabs(I.Origin[1]) >= SkipOriginBeyond || std::fabs(I.Origin[2]) >= SkipOriginBeyond)) continue;
            Take(I.Origin, [&](const float (&P)[3]) { return std::format("static instance {} of {} at ({:.0f}, {:.0f}, {:.0f})", I.Name, I.Model, P[0], P[1], P[2]); });
        }
        return Any;
    }

    void SunBox(const std::array<float, 3>& Lo, const std::array<float, 3>& Hi, float Pad, float Range, std::array<double, 3>& A, std::array<double, 3>& B)
    {
        for (size_t K = 0; K < 3; K++)
        {
            A[K] = std::clamp(std::floor(static_cast<double>(Lo[K]) - Pad), -static_cast<double>(Range), static_cast<double>(Range));
            B[K] = std::clamp(std::ceil(static_cast<double>(Hi[K]) + Pad), -static_cast<double>(Range), static_cast<double>(Range));
        }
    }

    std::string SunVolumeEntities(size_t Entity, const std::array<double, 3>& A, const std::array<double, 3>& B, const std::string& Target, const std::string& Ssi)
    {
        std::string Out = std::format("// entity {}\n{{\n\"classname\" \"info_null\"\n\"origin\" \"{} {} {}\"\n\"targetname\" \"{}\"\n}}\n", Entity, Num((A[0] + B[0]) / 2), Num((A[1] + B[1]) / 2), Num((A[2] + B[2]) / 2), Target);
        Out += std::format("// entity {}\n{{\n\"classname\" \"volume_sun\"\n\"target\" \"{}\"\n", Entity + 1, Target);
        for (int S = 1; S <= 4; S++) Out += std::format("\"ssi{}\" \"{}\"\n", S, Ssi);
        Out += "\"global_fill_color\" \"0 0 0\"\n\"global_fill_intensity\" \"1\"\n\"grid_density\" \"32\"\n\"respectLightLods\" \"1\"\n\"shadowBiasScale\" \"1\"\n\"shadowSplitDistance\" \"2000\"\n\"shadowVistaDetail\" \"1\"\n\"streamLighting\" \"1\"\n";
        Out += "// brush 0\n{\n";
        const double X0 = A[0], Y0 = A[1], Z0 = A[2], X1 = B[0], Y1 = B[1], Z1 = B[2];
        // every triple is ordered so that (p1 - p0) x (p2 - p0) points into the box
        using Triple = std::array<Point, 3>;
        const Triple Faces[6] = { { { { X0, Y0, Z0 }, { X1, Y0, Z0 }, { X1, Y1, Z0 } } },   // bottom
                                  { { { X0, Y0, Z1 }, { X0, Y1, Z1 }, { X1, Y1, Z1 } } },   // top
                                  { { { X0, Y0, Z0 }, { X0, Y1, Z0 }, { X0, Y1, Z1 } } },   // x = lo
                                  { { { X1, Y0, Z0 }, { X1, Y0, Z1 }, { X1, Y1, Z1 } } },   // x = hi
                                  { { { X0, Y0, Z0 }, { X0, Y0, Z1 }, { X1, Y0, Z1 } } },   // y = lo
                                  { { { X0, Y1, Z0 }, { X1, Y1, Z0 }, { X1, Y1, Z1 } } } };  // y = hi
        for (const Triple& F : Faces) Out += Face(F[0], F[1], F[2]);
        Out += "}\n}\n";
        return Out;
    }

    std::string ReflectionProbeEntity(size_t Entity, size_t Index, const Probe& P)
    {
        std::string Out = std::format("// entity {}\n{{\n\"classname\" \"reflection_probe\"\n\"name\" \"probe{}\"\n\"origin\" \"{} {} {}\"\n", Entity, Index, Num(P.Origin[0]), Num(P.Origin[1]), Num(P.Origin[2]));
        if (P.SizeRead)
        {
            Out += std::format("\"size_max\" \"{} {} {}\"\n", Num(P.SizeMax[0]), Num(P.SizeMax[1]), Num(P.SizeMax[2]));
            Out += std::format("\"size_min\" \"{} {} {}\"\n", Num(P.SizeMin[0]), Num(P.SizeMin[1]), Num(P.SizeMin[2]));
        }
        Out += "}\n";
        return Out;
    }

    std::string EffectPlacementEntity(size_t Entity, const EffectPlacement& E)
    {
        if (!SafeText(E.Kind) || !SafeText(E.Name)) return std::string();
        return std::format("// entity {}\n{{\n\"classname\" \"script_struct\"\n\"targetname\" \"graywolf_createfx\"\n\"script_noteworthy\" \"{}\"\n\"script_string\" \"{}\"\n\"origin\" \"{} {} {}\"\n\"angles\" \"{} {} {}\"\n}}\n",
                           Entity, E.Kind, E.Name, Num(E.Origin[0]), Num(E.Origin[1]), Num(E.Origin[2]), Num(E.Angles[0]), Num(E.Angles[1]), Num(E.Angles[2]));
    }

    int SunProbesSelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("sun and probes: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        // known directions, worked by hand (not through the formula): toward +Z is pitch 90; toward +X on the horizon is pitch 180, yaw 0; toward +Y is pitch 180, yaw 90;
        // straight down is pitch 270 = -90 once wrapped; mp_crash's sun (-0.321, -0.557, 0.766) is the worldspawn's sundirection angles (-50, 240): 50 degrees up, so pitch 130, yaw 240
        struct Case
        {
            std::array<float, 3> Toward;
            double Pitch, Yaw;
            const char* What;
        };
        const Case Cases[] = { { { 0, 0, 1 }, 90, 0, "straight up" },
                               { { 1, 0, 0 }, 180, 0, "+X on the horizon" },
                               { { 0, 1, 0 }, 180, 90, "+Y on the horizon" },
                               { { 0, 0, -1 }, -90, 0, "straight down wraps to -90" },
                               { { -0.3213938f, -0.5566704f, 0.7660444f }, 130, 240, "mp_crash's sun" },
                               { { 0.8660254f, 0, -0.5f }, -150, 0, "30 degrees below the horizon wraps from 210 to -150" } };
        for (const Case& C : Cases)
        {
            double Pitch = 0, Yaw = 0;
            const bool Ok = SunAngles(C.Toward, Pitch, Yaw);
            Check(Ok && std::fabs(Pitch - C.Pitch) < 1e-3 && std::fabs(Yaw - C.Yaw) < 1e-3, C.What, std::format("{} {}", Pitch, Yaw));
        }
        {
            // the stock presets (ssi.gdt, ModToolsSky): the direction their pitch and yaw give, back to the same numbers
            const double Pi = std::numbers::pi;
            for (const auto& [P, Y] : { std::pair{ 125.0, 150.0 }, std::pair{ 165.0, 263.0 }, std::pair{ 158.0, 300.0 }, std::pair{ 130.0, 140.0 } })
            {
                const std::array<float, 3> T = { static_cast<float>(-std::cos(P * Pi / 180) * std::cos(Y * Pi / 180)), static_cast<float>(-std::cos(P * Pi / 180) * std::sin(Y * Pi / 180)),
                                                 static_cast<float>(std::sin(P * Pi / 180)) };
                double Pitch = 0, Yaw = 0;
                Check(SunAngles(T, Pitch, Yaw) && std::fabs(Pitch - P) < 1e-3 && std::fabs(Yaw - Y) < 1e-3, "a stock preset's pitch and yaw come back from their direction", std::format("{} {} -> {} {}", P, Y, Pitch, Yaw));
            }
            double Pitch = 0, Yaw = 0;
            const std::array<float, 3> Zero = { 0, 0, 0 };
            Check(!SunAngles(Zero, Pitch, Yaw), "no direction, no angles");
        }
        Check(SafeText("graywolf_mp_crash") && SafeText("fx/smoke-1.x") && !SafeText("a\"b") && !SafeText("a\nb") && !SafeText("a\\b") && !SafeText("a b") && !SafeText(std::string()) && !SafeText(std::string("a\0b", 3)),
              "SafeText accepts names and refuses quotes, backslashes, spaces, newlines and NUL");
        SsiEntry Entry{ "mp_x", "skybox_mp_x", 130, 240, true, { 1, 1, 0.5f }, { { "stops", "11" } } };
        const std::string Text = SsiGdtEntry(Entry);
        Check(Text.find("\"mp_x\" ( \"ssi.gdf\" )") != std::string::npos && Text.find("\"pitch\" \"130\"") != std::string::npos && Text.find("\"yaw\" \"240\"") != std::string::npos &&
                  Text.find("\"skyboxmodel\" \"skybox_mp_x\"") != std::string::npos && Text.find("\"colorSRGB\" \"1 1 0.5 1\"") != std::string::npos && Text.find("\"stops\" \"11\"") != std::string::npos,
              "the ssi entry holds the stock fields, the angles, the sky and the colour", Text);
        Entry.Stock.clear();
        Check(SsiGdtEntry(Entry).find("\"stops\"") == std::string::npos, "no stock field, no stops");
        Entry.Name = "bad\"name";
        Check(SsiGdtEntry(Entry).empty(), "an ssi name with a quote is refused");
        Entry.Name = "mp_x", Entry.SkyboxModel = "sky\nbox";
        Check(SsiGdtEntry(Entry).empty(), "a sky name with a newline is refused");
        {
            SsiStock S;
            std::string Why;
            Check(ParseSsiStock(R"({"source":"s","entries":2,"values":{"stops":"11","evmin":"10","bad":"a\"b"},"stock_names":["default_day","x"]})", S, Why) && S.Values.size() == 2 && S.StockNames.size() == 2 && S.Entries == 2,
                  "the stock file is read, a value with a quote is dropped", Why);
            Check(!ParseSsiStock("{", S, Why) && !ParseSsiStock(R"({"values":{}})", S, Why), "an unreadable or empty stock file is refused");
        }
        // the sun box: from the collision, never from a far-off sky mesh
        {
            MapScene Scene;
            Mesh Sky;
            Surface S;
            S.Material = "sky";
            S.Vertices.resize(3);
            S.Vertices[0].Pos[0] = -126000, S.Vertices[1].Pos[0] = 123000, S.Vertices[2].Pos[2] = 26000;
            S.Indices = { 0, 1, 2 };
            Sky.Surfaces.push_back(S);
            Scene.Objects.push_back(Sky);
            std::array<float, 3> Lo{}, Hi{};
            Check(!CollisionBounds(Scene, Lo, Hi), "a scene with a sky mesh and no collision has no bounds");
            Scene.Collision.Vertices = { -100, -200, -10, 300, 400, 50 };
            Brush B;
            B.Vertices = { 500, 0, 0 };
            Scene.Collision.Brushes.push_back(B);
            Check(CollisionBounds(Scene, Lo, Hi) && Lo[0] == -100 && Hi[0] == 500 && Lo[1] == -200 && Hi[1] == 400 && Hi[2] == 50, "the bounds are the collision's, the sky mesh is ignored", std::format("{} {}", Lo[0], Hi[0]));
            std::array<double, 3> A{}, Bx{};
            SunBox(Lo, Hi, 256, 65536, A, Bx);
            Check(A[0] == -356 && Bx[0] == 756 && A[2] == -266 && Bx[2] == 306, "the box is the bounds plus the padding", std::format("{} {}", A[0], Bx[0]));
            SunBox({ -126000, 0, 0 }, { 123000, 10, 10 }, 256, 65536, A, Bx);
            Check(A[0] == -65536 && Bx[0] == 65536, "the box is clamped to the range");
        }
        // the volume: an info_null at the centre, six faces each with its normal into the box
        const std::array<double, 3> A = { -116, -216, -26 }, B = { 316, 416, 66 };
        const std::string Vol = SunVolumeEntities(3, A, B, "gw_sun_target", "mp_x");
        size_t Faces = 0, Inward = 0;
        for (size_t At = Vol.find("( "); At != std::string::npos;)
        {
            double P[3][3];
            size_t Cur = At;
            bool Ok = true;
            for (auto& Pt : P)
            {
                const size_t Open = Vol.find("( ", Cur);
                double X = 0, Y = 0, Z = 0;
                if (Open == std::string::npos || sscanf_s(Vol.substr(Open).c_str(), "( %lf %lf %lf )", &X, &Y, &Z) != 3)
                {
                    Ok = false;
                    break;
                }
                Pt[0] = X, Pt[1] = Y, Pt[2] = Z;
                Cur = Vol.find(')', Open) + 1;
            }
            if (!Ok) break;
            const double E1[3] = { P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2] }, E2[3] = { P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2] };
            const double N[3] = { E1[1] * E2[2] - E1[2] * E2[1], E1[2] * E2[0] - E1[0] * E2[2], E1[0] * E2[1] - E1[1] * E2[0] };
            const double Centre[3] = { 100, 100, 20 };
            Faces++;
            Inward += (N[0] * (Centre[0] - P[0][0]) + N[1] * (Centre[1] - P[0][1]) + N[2] * (Centre[2] - P[0][2])) > 0 ? 1 : 0;
            At = Vol.find("( ", Cur);
        }
        Check(Faces == 6 && Inward == 6, "a volume_sun is a box of six faces, every normal into the box (the stock order)", std::format("{} faces, {} inward", Faces, Inward));
        Check(Vol.find("// entity 3\n{\n\"classname\" \"info_null\"\n\"origin\" \"100 100 20\"\n\"targetname\" \"gw_sun_target\"") == 0 && Vol.find("// entity 4\n{\n\"classname\" \"volume_sun\"\n\"target\" \"gw_sun_target\"") != std::string::npos &&
                  Vol.find("\"ssi1\" \"mp_x\"") != std::string::npos && Vol.find("\"ssi4\" \"mp_x\"") != std::string::npos && Vol.find("( -116 -216 -26 )") != std::string::npos,
              "an info_null at the box centre, the volume targets it and names its ssi four times", Vol);
        Probe P;
        P.Origin[0] = 1.5f, P.Origin[1] = -2, P.Origin[2] = 3;
        Check(ReflectionProbeEntity(1, 2, P).find("size_min") == std::string::npos && ReflectionProbeEntity(1, 2, P).find("\"origin\" \"1.5 -2 3\"") != std::string::npos, "a probe with no size read writes none");
        P.SizeRead = true, P.SizeMin[0] = 10, P.SizeMax[1] = 20;
        Check(ReflectionProbeEntity(1, 2, P).find("\"size_min\" \"10 0 0\"") != std::string::npos && ReflectionProbeEntity(1, 2, P).find("\"size_max\" \"0 20 0\"") != std::string::npos, "a probe with a size writes it");
        EffectPlacement Fx;
        Fx.Kind = "fx", Fx.Name = "smoldering_smk_thin_black", Fx.Origin[0] = 786.87f, Fx.Origin[2] = 151.89f, Fx.Angles[0] = 270;
        const std::string Fe = EffectPlacementEntity(7, Fx);
        Check(Fe.find("\"classname\" \"script_struct\"") != std::string::npos && Fe.find("\"targetname\" \"graywolf_createfx\"") != std::string::npos && Fe.find("\"script_string\" \"smoldering_smk_thin_black\"") != std::string::npos &&
                  Fe.find("\"origin\" \"786.87 0 151.89\"") != std::string::npos && Fe.find("\"angles\" \"270 0 0\"") != std::string::npos && Fe.find("// entity 7\n") == 0,
              "an effect placement is a script_struct with its origin, angles and name", Fe);
        Fx.Name = "a\"b\nc";
        Check(EffectPlacementEntity(7, Fx).empty(), "an effect name with a quote or newline is refused");
        // the written bounds: the collision, the drawn world (not a far sky mesh) and the instances' origins
        {
            MapScene S;
            S.Collision.Vertices = { 0, 0, 0, 10, 10, 10 };
            Mesh World, SkyMesh;
            Surface Ground, Sky;
            Ground.Material = "ground";
            Ground.Vertices.resize(2);
            Ground.Vertices[0].Pos[0] = -500, Ground.Vertices[1].Pos[2] = 900;
            Sky.Material = "hdr_sky_x";
            Sky.Vertices.resize(1);
            Sky.Vertices[0].Pos[0] = 90000;
            World.Surfaces.push_back(Ground);
            SkyMesh.Surfaces.push_back(Sky);
            S.Objects = { World, SkyMesh };
            Instance I;
            I.Origin[1] = 2000;
            S.StaticInstances.push_back(I);
            std::array<float, 3> Lo{}, Hi{};
            Check(WrittenBounds(S, Lo, Hi) && Lo[0] == -500 && Hi[0] == 10 && Hi[1] == 2000 && Hi[2] == 900 && Lo[1] == 0, "the written bounds take the drawn world and the instances, not the sky mesh",
                  std::format("{} {} {} / {} {} {}", Lo[0], Lo[1], Lo[2], Hi[0], Hi[1], Hi[2]));
        }
        // the sky shell around a 100 x 200 x 50 box with 10 thick slabs. Stated independently: the shell is every point of the outer box (inner grown by 10) that is not in the inner
        // box, so each probe point of a grid over the outer box lies in exactly one slab if it is outside the inner box and in none if inside, and the volumes add up
        {
            const std::array<double, 3> A = { 0, 0, 0 }, B = { 100, 200, 50 };
            const double T = 10;
            const std::array<SkySlab, 6> Slabs = SkyShell(A, B, T);
            size_t Gaps = 0, Doubles = 0, Inside = 0, Probed = 0;
            for (double X = -9.5; X < 110; X += 3)
                for (double Y = -9.5; Y < 210; Y += 3)
                    for (double Z = -9.5; Z < 60; Z += 3)
                    {
                        int Hits = 0;
                        for (const SkySlab& S : Slabs) Hits += X > S.Lo[0] && X < S.Hi[0] && Y > S.Lo[1] && Y < S.Hi[1] && Z > S.Lo[2] && Z < S.Hi[2];
                        const bool InInner = X > 0 && X < 100 && Y > 0 && Y < 200 && Z > 0 && Z < 50;
                        Probed++;
                        if (InInner) Inside += Hits != 0;
                        else if (Hits == 0) Gaps++;
                        else if (Hits > 1) Doubles++;
                    }
            Check(Probed > 1000 && Gaps == 0 && Doubles == 0 && Inside == 0, "the sky shell has no gap, no overlap and leaves the inside empty", std::format("{} gaps {} overlaps {} inside of {}", Gaps, Doubles, Inside, Probed));
            double Volume = 0;
            for (const SkySlab& S : Slabs) Volume += (S.Hi[0] - S.Lo[0]) * (S.Hi[1] - S.Lo[1]) * (S.Hi[2] - S.Lo[2]);
            Check(std::fabs(Volume - (120.0 * 220.0 * 70.0 - 100.0 * 200.0 * 50.0)) < 1e-6, "the slabs' volumes add up to outer box minus inner box", std::format("{}", Volume));
            // the text: six brushes of six `sky` faces, every face's normal (from its three points) pointing into its brush
            const std::string Text = SkyShellEntity(3, A, B, T);
            size_t SkyFaces = 0, Brushes = 0, Outward = 0, Faces = 0;
            for (size_t At = Text.find("// brush"); At != std::string::npos; At = Text.find("// brush", At + 1))
            {
                Brushes++;
                const size_t End = Text.find("\n}\n", At);
                std::vector<std::array<double, 9>> Planes;
                double Centre[3] = { 0, 0, 0 };
                size_t Pos = Text.find('\n', At);
                while ((Pos = Text.find("( ", Pos)) != std::string::npos && Pos < End)
                {
                    std::array<double, 9> P{};
                    size_t At2 = Pos;
                    int Read = 0;
                    for (int K = 0; K < 3; K++)
                    {
                        At2 = Text.find('(', At2) + 1;
                        for (int A = 0; A < 3; A++)
                        {
                            const char* Start = &Text[At2];
                            char* E = nullptr;
                            P[K * 3 + A] = std::strtod(Start, &E);
                            Read += E != Start;
                            At2 += static_cast<size_t>(std::distance(Start, static_cast<const char*>(E)));
                        }
                        At2 = Text.find(')', At2);
                    }
                    if (Read != 9) break;
                    Pos = At2;
                    SkyFaces += Text.compare(At2, 6, ") sky ") == 0;
                    Planes.push_back(P);
                }
                for (const auto& P : Planes)
                    for (int K = 0; K < 3; K++) Centre[K] += (P[K] + P[3 + K] + P[6 + K]) / 3.0 / static_cast<double>(Planes.size());
                for (const auto& P : Planes)
                {
                    const double U[3] = { P[3] - P[0], P[4] - P[1], P[5] - P[2] }, V[3] = { P[6] - P[0], P[7] - P[1], P[8] - P[2] };
                    const double N[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
                    const double Along = N[0] * (Centre[0] - P[0]) + N[1] * (Centre[1] - P[1]) + N[2] * (Centre[2] - P[2]);
                    Faces++;
                    Outward += Along <= 0;
                }
            }
            Check(Brushes == 6 && Faces == 36 && SkyFaces == 36 && Outward == 0 && Text.find("\"classname\" \"func_group\"") != std::string::npos, "six brushes of six sky faces, every normal into its brush",
                  std::format("{} brushes {} faces {} sky {} outward", Brushes, Faces, SkyFaces, Outward));
        }
        return Failed;
    }
}
