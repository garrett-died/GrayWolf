#include "stdafx.h"

// A made-up asset list for screenshots (ui-shot's "demo" step), as HydraX's HYDRAX_UNO_DEMO_ROWS
// does: the rows are complete as given, no file is behind them, and nothing on the machine is read.
// A model or map row shows a sphere, an image row a gradient, so the preview is not empty.
#include "ImGuiShell.Internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ShellUi
{
    namespace
    {
        constexpr const char* DemoScheme = "demo://";
        constexpr const char* DemoGame = "Sample Game";

        // a small deterministic generator: the same list on every run
        struct Lcg
        {
            uint32_t State = 0x2468ACE1u;
            uint32_t Next() { State = State * 1664525u + 1013904223u; return State >> 8; }
            uint32_t Below(uint32_t N) { return N ? Next() % N : 0; }
        };

        template <size_t N>
        const char* Pick(Lcg& R, const char* (&List)[N]) { return List[R.Below((uint32_t)N)]; }
    }

    size_t AssetCatalog::AddUnscanned(std::vector<Item>& Batch)
    {
        const size_t Sorted = Models.size();
        for (Item& Md : Batch)
        {
            if (!KnownPaths.insert(Md.Path).second) continue;
            Md.Id = NextItemId++;
            IdToIndex[Md.Id] = Models.size();
            Models.push_back(std::move(Md));
        }
        return Sorted;
    }

    bool IsDemoRow(const Item& Row) { return Row.Path.rfind(DemoScheme, 0) == 0; }

    void AddDemoRows(ShellState& S, int Count)
    {
        static const char* Prefix[] = { "prop", "veh", "wpn", "char", "env", "fx", "ui", "gen" };
        static const char* Thing[] = { "crate", "barrel", "lantern", "console", "turret", "fence", "bench", "radio",
                                       "terminal", "pylon", "generator", "locker", "banner", "pipe", "ladder", "drone" };
        static const char* Detail[] = { "wood", "steel", "rust", "clean", "worn", "painted", "frozen", "mossy" };
        static const char* Map[] = { "frostline", "harbor_dusk", "quarry_run", "signal_hill", "glasshouse", "tidepool" };
        static const char* Sound[] = { "amb_wind_loop", "ui_click", "door_slide", "engine_idle", "steps_gravel", "alarm_short" };
        static const char* Anim[] = { "idle_loop", "walk_fwd", "sprint", "crouch_in", "reload_fast", "door_open" };

        Count = std::clamp(Count, 1, 5000);
        S.DemoInstalls = true;
        Lcg R;
        std::vector<Item> Rows;
        Rows.reserve((size_t)Count);
        for (int i = 0; i < Count; i++)
        {
            Item It;
            It.Game = DemoGame;
            It.Root = "demo";
            It.Written = 1700000000 + i * 60;
            const uint32_t Roll = R.Below(100);
            char Name[96];
            if (Roll < 36)       // model
            {
                It.Kind = 0; It.Format = "cast";
                snprintf(Name, sizeof(Name), "%s_%s_%s_%02d", Pick(R, Prefix), Pick(R, Thing), Pick(R, Detail), i % 40);
                It.Bones = R.Below(4) ? 1 + R.Below(24) : 40 + R.Below(120);
                It.Meshes = 1 + R.Below(5); It.Mats = 1 + R.Below(4);
                It.SizeBytes = 30000 + (long long)R.Below(900000);
            }
            else if (Roll < 72)  // image
            {
                It.Kind = 1; It.Format = R.Below(3) ? "dds" : "png";
                snprintf(Name, sizeof(Name), "%s_%s_%s_%s", Pick(R, Prefix), Pick(R, Thing), Pick(R, Detail), R.Below(2) ? "c" : "n");
                static const int Side[] = { 256, 512, 1024, 2048 };
                It.ImgW = It.ImgH = Side[R.Below(4)];
                It.SizeBytes = (long long)It.ImgW * It.ImgH * (It.Format == "dds" ? 1 : 3) / 2;
            }
            else if (Roll < 84)  // sound
            {
                It.Kind = 2; It.Format = "opus";
                snprintf(Name, sizeof(Name), "%s_%02d", Pick(R, Sound), i % 30);
                It.SizeBytes = 8000 + (long long)R.Below(400000);
            }
            else if (Roll < 93)  // anim
            {
                It.Kind = 3; It.Format = "seanim";
                snprintf(Name, sizeof(Name), "%s_%s_%02d", Pick(R, Prefix), Pick(R, Anim), i % 20);
                It.Bones = 60 + R.Below(80); It.Frames = 20 + R.Below(200); It.FrameRate = 30.0f; It.Looping = R.Below(2) != 0;
                It.SizeBytes = 20000 + (long long)R.Below(300000);
            }
            else if (Roll < 96)  // map
            {
                It.Kind = 4; It.Format = "glb";
                snprintf(Name, sizeof(Name), "%s_%02d", Pick(R, Map), i % 10);
                It.Meshes = 200 + R.Below(900); It.Verts = 400000 + R.Below(3000000); It.Tris = It.Verts / 2;
                It.SizeBytes = 40'000'000 + (long long)R.Below(200'000'000);
            }
            else                 // effect
            {
                It.Kind = 5; It.Format = "json";
                snprintf(Name, sizeof(Name), "fx_%s_%s_%02d", Pick(R, Thing), R.Below(2) ? "smoke" : "sparks", i % 25);
                It.SizeBytes = 4000 + (long long)R.Below(60000);
            }
            It.Name = Name;
            static const char* Folder[] = { "models", "images", "sounds", "anims", "maps", "fx" };
            It.Path = std::string(DemoScheme) + Folder[It.Kind] + "/" + It.Name + "." + It.Format;
            Rows.push_back(std::move(It));
        }
        SortModels(S, S.Catalog.AddUnscanned(Rows));
    }

    namespace
    {
        // A UV sphere around the origin, Z up, one material carrying a two-tone checker
        PreviewGeometry DemoSphere()
        {
            PreviewGeometry G;
            constexpr int Rings = 32, Segs = 64;
            const float Pi = 3.14159265f, Radius = 60.0f;
            for (int r = 0; r <= Rings; r++)
            {
                const float V = (float)r / Rings, Phi = V * Pi;
                for (int s = 0; s <= Segs; s++)
                {
                    const float U = (float)s / Segs, Theta = U * 2.0f * Pi;
                    const float Nx = std::sin(Phi) * std::cos(Theta), Ny = std::sin(Phi) * std::sin(Theta), Nz = std::cos(Phi);
                    G.Pos.insert(G.Pos.end(), { Nx * Radius, Ny * Radius, Nz * Radius });
                    G.Nrm.insert(G.Nrm.end(), { Nx, Ny, Nz });
                    G.UV.insert(G.UV.end(), { U * 4.0f, V * 2.0f });
                    G.VtxMat.push_back(0);
                }
            }
            for (int r = 0; r < Rings; r++)
                for (int s = 0; s < Segs; s++)
                {
                    const uint32_t A = (uint32_t)(r * (Segs + 1) + s), B = A + 1, C = A + (uint32_t)(Segs + 1), D = C + 1;
                    G.Idx.insert(G.Idx.end(), { A, C, B, B, C, D });
                }
            PreviewTexture T;
            T.Width = T.Height = 128;
            T.Rgba.resize((size_t)T.Width * T.Height * 4);
            for (int y = 0; y < T.Height; y++)
                for (int x = 0; x < T.Width; x++)
                {
                    const bool Odd = ((x / 16) + (y / 16)) & 1;
                    const size_t At = ((size_t)y * T.Width + x) * 4;
                    T.Rgba[At] = Odd ? 196 : 120; T.Rgba[At + 1] = Odd ? 202 : 132; T.Rgba[At + 2] = Odd ? 214 : 150; T.Rgba[At + 3] = 255;
                }
            G.Materials.push_back(std::move(T));
            return G;
        }
    }

    void LoadDemoSelection(ShellState& S, int i)
    {
        ViewportState& V = S.Viewport;
        const Item& Sel = S.Catalog.Models[i];
        if (Sel.Kind == 0 || Sel.Kind == 4)
        {
            V.Loader.Forget();
            V.Mesh = UploadPreviewMesh(S.Dev.Get(), DemoSphere());
            ResetCamera(V);
        }
        else if (Sel.Kind == 1)
        {
            const int W = 512, H = 512;
            std::vector<uint8_t> Px((size_t)W * H * 4);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    const size_t At = ((size_t)y * W + x) * 4;
                    const float Fx = (float)x / W, Fy = (float)y / H;
                    const bool Line = (x % 64 == 0) || (y % 64 == 0);
                    Px[At] = (uint8_t)(Line ? 255 : 40 + 180 * Fx);
                    Px[At + 1] = (uint8_t)(Line ? 255 : 60 + 120 * Fy);
                    Px[At + 2] = (uint8_t)(Line ? 255 : 150 + 80 * (1.0f - Fx));
                    Px[At + 3] = 255;
                }
            D3D11_TEXTURE2D_DESC Td{};
            Td.Width = W; Td.Height = H; Td.MipLevels = 1; Td.ArraySize = 1; Td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            Td.SampleDesc.Count = 1; Td.Usage = D3D11_USAGE_IMMUTABLE; Td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA Sd{ Px.data(), (UINT)(W * 4), 0 };
            Microsoft::WRL::ComPtr<ID3D11Texture2D> Tex;
            if (SUCCEEDED(S.Dev->CreateTexture2D(&Td, &Sd, &Tex)))
                S.Dev->CreateShaderResourceView(Tex.Get(), nullptr, &V.ImageSRV);
            V.ImageW = W; V.ImageH = H;
        }
    }
}
