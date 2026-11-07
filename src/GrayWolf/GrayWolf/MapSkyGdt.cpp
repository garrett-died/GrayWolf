#include "stdafx.h"

#include "MapSkyGdt.h"

#include "Bo3Target.Techsets.h"
#include "ModToolsSky.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace Maps
{
    namespace
    {
        bool Safe(const std::string& S)
        {
            if (S.empty() || S.size() > 63) return false;
            return std::all_of(S.begin(), S.end(), [](char C) { return std::isalnum(static_cast<unsigned char>(C)) || C == '_' || C == '.' || C == '-'; });
        }

        bool SafePath(const std::string& P)
        {
            // segments of Safe text joined by single backslashes
            size_t From = 0;
            if (P.empty()) return false;
            while (From <= P.size())
            {
                const size_t To = P.find('\\', From);
                const std::string Part = P.substr(From, To == std::string::npos ? std::string::npos : To - From);
                if (!Safe(Part) && Part != "t7_skybox") return false;
                if (To == std::string::npos) break;
                From = To + 1;
            }
            return true;
        }

        bool KeyLess(const std::string& A, const std::string& B)
        {
            return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end(), [](char X, char Y) { return std::tolower(static_cast<unsigned char>(X)) < std::tolower(static_cast<unsigned char>(Y)); });
        }

        void SetField(Gdt::Fields& F, const std::string& Key, const std::string& Value)
        {
            for (auto& [K, V] : F)
                if (K == Key)
                {
                    V = Value;
                    return;
                }
            F.emplace_back(Key, Value);
        }
    }

    bool ParseSkyStock(const std::string& Json, SkyStock& Out, std::string& Why)
    {
        try
        {
            const nlohmann::json J = nlohmann::json::parse(Json);
            Out = SkyStock();
            Out.Source = J.value("source", std::string());
            Out.Materials = J.value("materials", static_cast<size_t>(0));
            for (const auto& [K, V] : J.at("material_values").items())
                if (V.is_string()) Out.MaterialValues.emplace_back(K, V.get<std::string>());
            for (const auto& [K, V] : J.at("image_values").items())
                if (V.is_string()) Out.ImageValues.emplace_back(K, V.get<std::string>());
            Out.XmodelFile = J.at("xmodel_values").value("filename", std::string());
            if (J.contains("stock_entries"))
                for (const auto& [Part, Target] : { std::pair<const char*, Gdt::Fields*>{ "image", &Out.StockImage }, { "material", &Out.StockMaterial }, { "xmodel", &Out.StockXmodel } })
                    for (const auto& Pair : J.at("stock_entries").at(Part))
                        Target->emplace_back(Pair.at(0).get<std::string>(), Pair.at(1).get<std::string>());
            for (const auto& N : J.at("names")) Out.Names.insert(N.get<std::string>());
        }
        catch (const nlohmann::json::exception& Ex)
        {
            Why = std::string("the stock sky file is not readable: ") + Ex.what();
            return false;
        }
        if (Out.MaterialValues.empty() || Out.XmodelFile.empty() || Out.Names.empty())
        {
            Why = "the stock sky file holds no material values, xmodel file or names";
            return false;
        }
        return true;
    }

    bool LoadSkyStock(SkyStock& Out, std::string& Why)
    {
        wchar_t Buf[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, Buf, static_cast<DWORD>(std::size(Buf)));
        std::error_code Ec;
        for (fs::path P = fs::path(Buf).parent_path(); !P.empty(); P = P.parent_path())
        {
            const fs::path Profiles = P / "zonekit" / "profiles";
            const fs::path File = Profiles / "t7" / "target" / "sky-stock.json";
            if (fs::exists(File, Ec))
            {
                std::ifstream In(File, std::ios::binary);
                std::stringstream Text;
                Text << In.rdbuf();
                if (!ParseSkyStock(Text.str(), Out, Why)) return false;
                Bo3Target::Techsets::Data Data;
                Data.Load(Profiles);
                if (!Data.HasDefaults())
                {
                    Why = "material-defaults.json was not found or is empty: the sky material would lack the stock keys";
                    return false;
                }
                const Bo3Target::Techsets::Techset* T = Data.Find("sky_latlong_hdr");
                Gdt::Fields Base;
                if (T != nullptr)
                    for (const auto& S : T->Slots)
                        if (!S.Key.empty()) SetField(Base, S.Key, std::string());
                for (const auto& [Key, Value] : Data.Defaults())
                    if (T == nullptr || T->ByKey(Key) == nullptr) SetField(Base, Key, Value);
                Out.MaterialBase = std::move(Base);
                return true;
            }
            if (P == P.parent_path()) break;
        }
        Why = "zonekit/profiles/t7/target/sky-stock.json was not found";
        return false;
    }

    bool SkyNamesFor(const std::string& Map, const SkyStock& Stock, SkyNames& Out)
    {
        if (!Safe(Map) || Map.size() > 40) return false;
        std::string Suffix;
        for (int Tries = 0; Tries < 8; Tries++)
        {
            Out.Image = "skybox_" + Map + Suffix, Out.Xmodel = Out.Image, Out.Material = "mtl_skybox_" + Map + Suffix;
            if (Stock.Names.count(Out.Image) == 0 && Stock.Names.count(Out.Material) == 0) return true;
            Suffix += "_gw";
        }
        return false;
    }

    std::string SkyGdtText(const SkyNames& Names, const std::string& BaseImage, const SkyStock& Stock, double Rotation)
    {
        if (!Safe(Names.Image) || !Safe(Names.Material) || !Safe(Names.Xmodel) || !SafePath(BaseImage) || !SafePath(Stock.XmodelFile) || !std::isfinite(Rotation)) return std::string();
        const auto Sorted = [](Gdt::Fields F) {
            std::stable_sort(F.begin(), F.end(), [](const auto& A, const auto& B) { return KeyLess(A.first, B.first); });
            return F;
        };
        Gdt::Fields Image = Stock.StockImage.empty() ? Stock.ImageValues : Stock.StockImage;
        SetField(Image, "baseImage", Gdt::Path(BaseImage));
        Gdt::Fields Material = Stock.StockMaterial.empty() ? Stock.MaterialBase : Stock.StockMaterial;
        for (const auto& [K, V] : Stock.MaterialValues) SetField(Material, K, V);
        SetField(Material, "colorMap", Names.Image);
        SetField(Material, "skyRotation", std::format("{:g}", Rotation));
        // the stock xmodel entry: the stock sky mesh, its one material and the skin override (literal \r\n, as APE writes them)
        Gdt::Fields Xmodel = Stock.StockXmodel;
        if (Xmodel.empty())
            Xmodel = { { "CollisionMap", "" }, { "materials", "mtl_skybox_default\\r\\n" }, { "type", "rigid" }, { "usage_attachment", "0" }, { "usage_hero", "0" }, { "usage_view", "0" },
                       { "usage_weapon", "0" }, { "usage_zombie_body", "0" }, { "usage_zombie_world_prop", "0" } };
        SetField(Xmodel, "filename", Gdt::Path(Stock.XmodelFile));
        SetField(Xmodel, "skinOverride", "mtl_skybox_default " + Names.Material + "\\r\\n");
        return Gdt::Document({ Gdt::Entry(Names.Material, "material.gdf", Sorted(Material), "\r\n"), Gdt::Entry(Names.Image, "image.gdf", Sorted(Image), "\r\n"),
                               Gdt::Entry(Names.Xmodel, "xmodel.gdf", Sorted(Xmodel), "\r\n") }, "\r\n");
    }

    int SkyGdtSelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* What, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::string("sky gdt: ") + What + (Got.empty() ? "" : " (" + Got + ")") + "\n";
        };
        const std::string Json = R"({"source":"test","materials":58,
            "material_values":{"materialType":"sky_latlong_hdr","skyStops":"9.305","skyScaleRGB":"1722.16","skySize":"65000","sort":"sky"},
            "image_values":{"coreSemantic":"HDR","semantic":"HDR","streamable":"1","type":"image","imageType":"Texture","compressionMethod":"compressed","compositeType":"BaseSpecularGlossMap"},
            "xmodel_values":{"filename":"t6_props\\vista\\skybox\\t6_skybox.xmodel_bin"},
            "names":["skybox_mp_taken","mtl_skybox_mp_taken","skybox_mp_taken_gw","other"]})";
        SkyStock Stock;
        std::string Why;
        Check(ParseSkyStock(Json, Stock, Why) && Stock.Materials == 58 && Stock.Names.size() == 4 && Stock.XmodelFile == "t6_props\\vista\\skybox\\t6_skybox.xmodel_bin", "the stock file parses", Why);
        Stock.MaterialBase = { { "adsZscaleOn", "0" }, { "colorMap", "" }, { "skyRotation", "99" }, { "aiClip", "0" } };
        SkyNames N;
        Check(SkyNamesFor("mp_crash", Stock, N) && N.Image == "skybox_mp_crash" && N.Xmodel == "skybox_mp_crash" && N.Material == "mtl_skybox_mp_crash", "the names follow the Mod Tools' convention");
        Check(SkyNamesFor("mp_taken", Stock, N) && N.Image == "skybox_mp_taken_gw_gw" && N.Material == "mtl_skybox_mp_taken_gw_gw", "a stock name is avoided by suffix until nothing clashes", N.Image);
        Check(!SkyNamesFor("mp bad", Stock, N) && !SkyNamesFor("mp\"x", Stock, N) && !SkyNamesFor("", Stock, N), "a map name that is not plain text is refused");
        SkyNamesFor("mp_crash", Stock, N);
        const std::string Base = "model_export\\t7_skybox\\mp_crash\\skybox_mp_crash_ll.exr";
        const std::string Text = SkyGdtText(N, Base, Stock, 0.0);
        const auto Has = [&](const std::string& S) { return Text.find(S) != std::string::npos; };
        Check(Has("\t\"skybox_mp_crash\" ( \"image.gdf\" )") && Has("\t\"mtl_skybox_mp_crash\" ( \"material.gdf\" )") && Has("\t\"skybox_mp_crash\" ( \"xmodel.gdf\" )"), "an image, a material and an xmodel entry");
        Check(Has("\"baseImage\" \"model_export\\\\t7_skybox\\\\mp_crash\\\\skybox_mp_crash_ll.exr\""), "the baseImage path with the doubled backslashes APE writes");
        Check(Has("\"skinOverride\" \"mtl_skybox_default mtl_skybox_mp_crash\\r\\n\"") && Has("\"materials\" \"mtl_skybox_default\\r\\n\"") && Has("\"filename\" \"t6_props\\\\vista\\\\skybox\\\\t6_skybox.xmodel_bin\""),
              "the xmodel names the stock sky mesh and swaps its material, literal \\r\\n");
        Check(Has("\"colorMap\" \"skybox_mp_crash\"") && Has("\"skyRotation\" \"0\"") && !Has("\"skyRotation\" \"99\"") && Has("\"materialType\" \"sky_latlong_hdr\"") && Has("\"adsZscaleOn\" \"0\"") && Has("\"skyStops\" \"9.305\""),
              "the material takes the stock keys, its image and the rotation");
        Check(Text.find("\"adsZscaleOn\"") < Text.find("\"colorMap\"") && Text.find("\"colorMap\"") < Text.find("\"skyScaleRGB\""), "the keys are sorted");
        // the real stock file: every key of the stock entries is written (a sky image without clampU / noMipMaps gets mips and wraps)
        {
            SkyStock Real;
            std::string RealWhy;
            if (!LoadSkyStock(Real, RealWhy))
                Check(false, "the real sky-stock.json loads", RealWhy);
            else
            {
                const std::string T = SkyGdtText(N, Base, Real, 0.0);
                const auto Count = [&](const char* Gdf) {
                    const size_t At = T.find(std::string("( \"") + Gdf + "\" )");
                    const size_t End = T.find("\r\n\t}", At);
                    size_t Keys = 0;
                    for (size_t P = T.find("\r\n\t\t\"", At); P != std::string::npos && P < End; P = T.find("\r\n\t\t\"", P + 1)) Keys++;
                    return Keys;
                };
                Check(Count("image.gdf") >= 42 && Count("xmodel.gdf") >= 78 && Count("material.gdf") >= 907, "the sky entries carry every key of the stock ones (image 42, xmodel 78 of 213, material 907 of 909: the rest the install's awi or catalog does not declare)",
                      std::format("{} {} {}", Count("image.gdf"), Count("xmodel.gdf"), Count("material.gdf")));
                Check(T.find("\"clampU\" \"1\"") != std::string::npos && T.find("\"clampV\" \"1\"") != std::string::npos && T.find("\"noMipMaps\" \"1\"") != std::string::npos && T.find("\"mipMode\"") != std::string::npos &&
                          T.find("\"mipBase\"") != std::string::npos && T.find("\"colorSRGB\" \"0\"") != std::string::npos,
                      "the sky image clamps, has no mips and is linear, as the stock skies' do");
                Check(T.find("mtl_skybox_mp_chinatown") == std::string::npos && T.find("skybox_mp_chinatown") == std::string::npos, "no stock entry's own name is left in the written entries");
            }
        }
        Check(SkyGdtText(N, "model_export\\bad\"x.exr", Stock, 0).empty() && SkyGdtText(N, "a b\\c.exr", Stock, 0).empty(), "an unsafe path is refused");
        SkyNames Bad = N;
        Bad.Material = "mtl x";
        Check(SkyGdtText(Bad, Base, Stock, 0).empty(), "an unsafe name is refused");

        // read back through the preview's own chain (ModToolsSky): a Mod Tools tree built in a temp folder holding the entries and the file
        {
            const fs::path Tree = fs::temp_directory_path() / "graywolf-sky-gdt-selftest";
            std::error_code Ec;
            fs::remove_all(Tree, Ec);
            fs::create_directories(Tree / "model_export" / "t7_skybox" / "mp_crash", Ec);
            fs::create_directories(Tree / "source_data", Ec);
            { std::ofstream(Tree / "model_export" / "t7_skybox" / "mp_crash" / "skybox_mp_crash_ll.exr", std::ios::binary) << "x"; }
            { std::ofstream(Tree / "model_export" / "mp_crash_sky.gdt", std::ios::binary) << Text; }
            { std::ofstream(Tree / "source_data" / "ssi_mp_crash.gdt", std::ios::binary) << "{\n\t\"graywolf_mp_crash\" ( \"ssi.gdf\" )\n\t{\n\t\t\"skyboxmodel\" \"skybox_mp_crash\"\n\t\t\"type\" \"ssi\"\n\t}\n}\n"; }
            ModToolsSky::Location Where;
            Where.Root = Tree.string();
            ModToolsSky::SkySource Source;
            Why.clear();
            const bool Ok = ModToolsSky::ResolveSsiEntry(Where, "graywolf_mp_crash", Source, Why);
            Check(Ok && Source.Kind == ModToolsSky::SourceKind::LatLong && Source.Material == "mtl_skybox_mp_crash" && Source.Image == "skybox_mp_crash" && !Source.Fallback &&
                      fs::path(Source.LatLong).filename() == "skybox_mp_crash_ll.exr",
                  "the sky reads back through the preview's chain ssi -> skyboxmodel -> skinOverride -> colorMap -> baseImage", Why);
            Check(!ModToolsSky::ResolveSsiEntry(Where, "no_such_entry", Source, Why), "an ssi entry that is not there does not resolve");
            // an xmodel whose skinOverride names another material breaks the chain at the material
            std::string Other = Text;
            const size_t At = Other.find("mtl_skybox_default mtl_skybox_mp_crash");
            if (At != std::string::npos) Other.replace(At, 38, "mtl_skybox_default mtl_skybox_other___");
            { std::ofstream(Tree / "model_export" / "mp_crash_sky.gdt", std::ios::binary) << Other; }
            Check(!ModToolsSky::ResolveSsiEntry(Where, "graywolf_mp_crash", Source, Why), "a skinOverride naming a material the GDT lacks breaks the chain", Why);
            fs::remove_all(Tree, Ec);
        }
        return Failed;
    }
}
