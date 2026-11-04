#include "stdafx.h"

// See Bo3Target.Publish.h.

#include "Bo3Target.Publish.h"

#include "Bo3Target.MaterialImages.h"
#include "DiskGame.h"
#include "ExportNaming.h"
#include "GdtWriter.h"
#include "MaterialGdt.h"
#include "MaterialDescription.h"
#include "XMEImport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace Bo3Target::Publish
{
    namespace Pl = Placement;
    namespace Out = Output;

    bool ParseTarget(std::string_view Text, Target& Result)
    {
        std::string Low(Text);
        for (char& C : Low)
        {
            if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
        }
        if (Low == "none") { Result = Target::None; return true; }
        if (Low == "folder") { Result = Target::Folder; return true; }
        if (Low == "modtools") { Result = Target::ModTools; return true; }
        return false;
    }

    bool ResolveRoot(const std::string& Given, const std::string& InstallScan, const std::string& TaToolsPath, fs::path& Result, std::string& How)
    {
        std::error_code Ec;
        const auto Try = [&](const std::string& Text, const char* Name) {
            if (Text.empty() || !fs::is_directory(fs::path(Text), Ec)) return false;
            Result = fs::absolute(fs::path(Text), Ec);
            How = Name;
            return true;
        };
        if (!Given.empty()) return Try(Given, "--mod-tools-root");
        return Try(InstallScan, "the BO3 install found by the install scan") || Try(TaToolsPath, "TA_TOOLS_PATH");
    }

    bool ChooseRoot(Target Which, const std::string& Given, const std::string& InstallScan, const std::string& TaToolsPath, fs::path& Result,
                    std::string& How, std::string& Error)
    {
        if (Which == Target::ModTools && Given.empty())
        {
            Error = "--target modtools writes into a Mod Tools install, so it needs --mod-tools-root <folder> on the command line "
                    "(the saved setting, the install scan and TA_TOOLS_PATH are used only to read the gdt.db and the stock index)";
            return false;
        }
        const bool Found = ResolveRoot(Given, Which == Target::ModTools ? std::string() : InstallScan, Which == Target::ModTools ? std::string() : TaToolsPath, Result, How);
        if (!Found && !Given.empty())
        {
            Error = "--mod-tools-root " + Given + " is not a folder";
            return false;
        }
        return true;
    }

    // ---- the Mod Tools knowledge ------------------------------------------------------------------------

    namespace
    {
        class NoTimes final : public GdtDb::IFileTimes
        {
        public:
            bool ModifiedSeconds(const std::string&, int64_t&) override { return false; }
        };
    }

    struct Loaded::State
    {
        std::unique_ptr<GdtDb::Database> Db;
        std::vector<GdtDb::Row> Rows;
        GdtDb::Catalog Held;
        bool HaveCatalog = false;
        bool Check = true;
        std::unordered_set<std::string> Manifests;
        std::unique_ptr<GdtDb::IFileTimes> Times;
        std::unique_ptr<GdtDb::StockIndex> Stock;
    };

    Loaded::Loaded() : Data(std::make_unique<State>()) {}
    Loaded::~Loaded() = default;

    void Loaded::Open(const fs::path& Root, bool Check, std::vector<std::string>& Warnings)
    {
        State& S = *Data;
        S.Check = Check;
        const auto Empty = [&] {
            S.Times = std::make_unique<NoTimes>();
            S.Stock = std::make_unique<GdtDb::StockIndex>(S.Rows, GdtDb::StockRules{}, *S.Times);
        };
        if (Root.empty())
        {
            Warnings.push_back("no Mod Tools root was found: the gdt.db is not consulted and no stock path is known");
            Empty();
            return;
        }
        GdtDb::Status St = GdtDb::Database::Open(Root, S.Db);
        bool HasStock = false, Typed = false;
        if (St.Ok) St = S.Db->Load(S.Rows, Typed, HasStock);
        if (!St.Ok)
        {
            Warnings.push_back("gdt.db unreadable, everything is exported: " + St.Reason);
            S.Rows.clear();
            Empty();
            return;
        }
        for (const GdtDb::Row& Entry : S.Rows) S.Held.Add(Entry);
        S.HaveCatalog = true;
        GdtDb::StockRules Rules;
        std::error_code Ec;
        const auto Time = fs::last_write_time(Root / "bin" / "linker_modtools.exe", Ec);
        if (!Ec) Rules.ReferenceSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(Time).time_since_epoch()).count();
        for (const char* Folder : { "hydrax", "graywolf" })
        {
            const GdtDb::Status M = GdtDb::ReadManifestPaths(Root / Folder / "manifest.json", S.Manifests);
            if (!M.Ok) Warnings.push_back("the stock index ignores an unreadable manifest: " + M.Reason);
        }
        Rules.RootPrefix = Root.string();
        Rules.Manifests = &S.Manifests;
        S.Times = GdtDb::MakeDiskFileTimes(Root);
        S.Stock = std::make_unique<GdtDb::StockIndex>(S.Rows, Rules, *S.Times);
    }

    Knowledge Loaded::View() const
    {
        return { Data->HaveCatalog && Data->Check ? &Data->Held : nullptr, Data->Stock.get() };
    }

    // ---- preflight ----------------------------------------------------------------------------------------

    std::string Preflight(const Options& O)
    {
        if (O.Which == Target::None) return {};
        if (O.OutputRoot.empty()) return "the publish step has no output folder";
        if (O.Which == Target::Folder)
        {
            fs::path Found;
            if (Out::Touches(O.OutputRoot, &Found)) return "--target folder refused: the output lies in the Mod Tools install " + Found.string();
        }
        else
        {
            if (O.ModToolsRoot.empty()) return "--target modtools needs --mod-tools-root <folder> on the command line";
            if (!Out::IsInstall(O.ModToolsRoot)) return O.ModToolsRoot.string() + " is not a Mod Tools folder (no BlackOps3.exe, bin\\linker_modtools.exe, share\\raw, or map_source with source_data)";
        }
        Out::Manifest Listed;
        const Out::Status M = Listed.Load(O.OutputRoot / "graywolf" / "manifest.json");
        if (!M.Ok) return M.Reason + " (nothing was written)";
        return {};
    }

    // ---- helpers --------------------------------------------------------------------------------------------------

    namespace
    {
        std::string Lower(std::string_view Text)
        {
            std::string R(Text);
            for (char& C : R)
            {
                if (C >= 'A' && C <= 'Z') C = static_cast<char>(C - 'A' + 'a');
            }
            return R;
        }

        std::string Clean(std::string Text)
        {
            for (char& C : Text)
            {
                if (C == '\t' || C == '\r' || C == '\n') C = ' ';
            }
            return Text;
        }

        std::string Join(std::string_view Folder, std::string_view File)
        {
            if (Folder.empty()) return std::string(File);
            while (!Folder.empty() && Folder.back() == '\\') Folder.remove_suffix(1);
            return std::string(Folder) + "\\" + std::string(File);
        }

        bool StartsWith(std::string_view Text, std::string_view Prefix)
        {
            return Text.size() >= Prefix.size() && Lower(Text.substr(0, Prefix.size())) == Prefix;
        }

        // the name without folder, i_ and mtl_: the stem the game token is read from
        std::string StemOf(std::string_view Name)
        {
            std::string S(Name);
            if (S.size() > 2 && StartsWith(S, "i_")) S = S.substr(2);
            if (S.size() > 4 && StartsWith(S, "mtl_")) S = S.substr(4);
            return S;
        }

        std::string Slurp(const fs::path& File)
        {
            std::ifstream In(File, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        }

        std::string NowText()
        {
            const auto Now = std::chrono::system_clock::now();
            return std::format("{:%Y-%m-%dT%H:%M:%S}", Now) + "+00:00";
        }

        // The bone count of an XMODEL_BIN: the version block (0x24D1, int16) is followed by the bone count
        // block (0x76BA, int16). -1 when the file cannot be read or the blocks are not where they are written.
        int BoneCountOfBin(const fs::path& File)
        {
            std::vector<uint8_t> Bytes;
            std::string Error;
            if (!CodXMBReader::ReadFile(File.string(), Bytes, Error)) return -1;
            for (size_t I = 0; I + 8 <= Bytes.size(); ++I)
            {
                if (Bytes[I] == 0xD1 && Bytes[I + 1] == 0x24 && Bytes[I + 4] == 0xBA && Bytes[I + 5] == 0x76)
                    return static_cast<int>(static_cast<int16_t>(Bytes[I + 6] | (Bytes[I + 7] << 8)));
            }
            return -1;
        }

        // APE writes an entry's keys sorted, case aside
        void SortKeys(Gdt::Fields& F)
        {
            std::stable_sort(F.begin(), F.end(), [](const auto& A, const auto& B) {
                return std::lexicographical_compare(A.first.begin(), A.first.end(), B.first.begin(), B.first.end(),
                    [](char X, char Y) { return std::tolower(static_cast<unsigned char>(X)) < std::tolower(static_cast<unsigned char>(Y)); });
            });
        }

        struct Model
        {
            std::string Name;                               // the xmodel's asset name
            std::vector<fs::path> Sidecars;
            std::vector<std::pair<int, fs::path>> Bins;     // LOD number, XMODEL_BIN
        };

        struct File
        {
            std::string Rel;
            fs::path Source;
            std::string Hash;
            std::string Bytes;          // a converted image: the bytes to write (Source is then not copied)
            bool Converted = false;
            Out::Verdict V = Out::Verdict::Create;
        };

        struct Asset
        {
            std::string Kind, Gdf, Name, From;      // From: the name before Shorten
            std::string Action, Reason;
            bool Active = true;
            std::string Dest;                       // the gdt, root-relative
            Pl::Result Placed;
            Out::GdtEntryText Entry;
            std::vector<File> Files;
            std::string ManifestType;
        };

        struct Row
        {
            std::string Kind, Name, Action, Reason, Gdt, Files;
        };
        struct Approx
        {
            std::string Code, Asset, Detail, Message;
        };

        std::string Tsv(const std::vector<std::string>& Cells)
        {
            std::string Line;
            for (size_t I = 0; I < Cells.size(); ++I) Line += (I ? "\t" : "") + Clean(Cells[I]);
            return Line + "\n";
        }

        struct GdtState
        {
            bool Exists = false;
            Out::Verdict V = Out::Verdict::Create;      // of the file as it lies (not of the merged bytes)
            std::string OldText;
            std::vector<Out::GdtEntryText> Old;
            bool Parsed = true;
            std::string Merged;                         // the file to write
            std::string Hash;
            Out::Verdict Final = Out::Verdict::Create;
            size_t Kept = 0;
            std::vector<Asset*> Assets;
        };
    }

    // ---- the step ---------------------------------------------------------------------------------------------------

    Result Run(const Options& O, const Knowledge& Know, const fs::path& RunRoot, const std::string& GameId)
    {
        Result R;
        auto& C = R.Counters;
        const auto Count = [&](const std::string& What, size_t N = 1) { C["publish_" + What] += N; };
        if (O.Which == Target::None) return R;
        if (Know.Stock == nullptr)
        {
            R.Error = "internal: the publish step needs a stock index";
            return R;
        }
        std::string Title = GameId;
        for (const auto& G : Pl::GameTable())
        {
            if (Lower(G.GameId) == Lower(GameId)) Title = G.Title;
        }
        // HydraX's form: "BlackOps4"
        if (Title.starts_with("Call of Duty: ")) Title = Title.substr(14);
        Title.erase(std::remove_if(Title.begin(), Title.end(), [](char Ch) { return Ch == ' ' || Ch == ':'; }), Title.end());

        Out::Manifest Manifest;
        const fs::path ManifestFile = O.OutputRoot / "graywolf" / "manifest.json";
        if (const Out::Status M = Manifest.Load(ManifestFile); !M.Ok)
        {
            R.Error = M.Reason + " (nothing was written)";
            return R;
        }
        Out::Gate Gate(O.OutputRoot, Manifest);
        std::error_code Ec;

        std::vector<Row> Rows;
        std::vector<Approx> Approxes;

        // 1. the models: their sidecars and XMODEL_BIN files
        const fs::path ModelsDir = RunRoot / DiskGame::KindFolder(DiskGame::Kind::Model);
        std::map<std::string, Model> Models;
        {
            const std::regex Lod(R"(_lod(\d+)$)", std::regex::icase);
            for (fs::recursive_directory_iterator It(ModelsDir, Ec), End; !Ec && It != End; It.increment(Ec))
            {
                std::error_code Ec2;
                if (!It->is_regular_file(Ec2)) continue;
                const std::string FileName = Lower(It->path().filename().string());
                const bool Sidecar = FileName.ends_with(".materials.json"), Bin = FileName.ends_with(".xmodel_bin");
                if (!Sidecar && !Bin) continue;
                const std::string Name = MaterialGdt::AssetName(It->path().parent_path().filename().string());
                if (Name.empty()) continue;
                Model& M = Models[Name];
                M.Name = Name;
                if (Sidecar) M.Sidecars.push_back(It->path());
                else
                {
                    std::smatch Match;
                    const std::string Stem = It->path().stem().string();
                    int Number = 0;
                    if (std::regex_search(Stem, Match, Lod)) Number = MaterialGdt::LodNumber(Match[1].str());
                    M.Bins.emplace_back(Number, It->path());
                }
            }
        }
        Count("models", Models.size());

        // 2. the materials and images of all of them
        // a sidecar is data: a path outside the run (absolute, ../) gives no baseImage, so no slot, read or write
        MaterialGdt::Builder B([&RunRoot](const fs::path& File) { return Out::IsUnder(RunRoot, File) ? File.generic_string() : std::string(); }, "m_");
        B.SetRunRoot(RunRoot);
        B.ConvertImages(true);
        B.TechsetData(&Techsets::Shared());     // a missing data file is counted (m_techset_data_missing) and reported below
        B.PlanOnly(O.Plan);
        std::map<std::string, std::vector<std::string>> MaterialModels;     // material asset name -> models using it
        std::map<std::string, std::string> TechsetOf;
        for (auto& [Name, M] : Models)
        {
            std::sort(M.Sidecars.begin(), M.Sidecars.end());
            for (const fs::path& S : M.Sidecars)
            {
                Materials::Sidecar Side;
                if (!Materials::ReadSidecar(S.string(), Side))
                {
                    Count("sidecars_unread");
                    continue;
                }
                Count("sidecars");
                // the sidecar keeps each material's source techset id; ReadSidecar does not hand it back
                std::vector<std::string> Techsets;
                try
                {
                    std::ifstream In(S, std::ios::binary);
                    const nlohmann::json Root = nlohmann::json::parse(In);
                    for (const auto& J : Root.value("materials", nlohmann::json::array())) Techsets.push_back(J.value("techset", std::string()));
                }
                catch (const std::exception&)   // boundary: a sidecar the reader took may still hold odd JSON; the techset is then unknown
                {
                    Techsets.clear();
                }
                for (size_t I = 0; I < Side.Materials.size(); ++I)
                {
                    const Materials::Description& D = Side.Materials[I];
                    B.Add(D, S.parent_path(), I < Side.Sources.size() ? &Side.Sources[I] : nullptr, Side.Game);
                    const std::string Asset = MaterialGdt::AssetName(D.Name);
                    if (Asset.empty()) continue;
                    auto& Users = MaterialModels[Asset];
                    if (std::find(Users.begin(), Users.end(), Name) == Users.end()) Users.push_back(Name);
                    if (I < Techsets.size() && !Techsets[I].empty()) TechsetOf.emplace(Asset, Techsets[I]);
                }
            }
        }
        const MaterialGdt::Builder::Resolved Resolved = B.Resolve();
        for (const auto& [K, N] : B.Counters()) C["publish_builder_" + K.substr(2)] += N;
        // the data of the BO3 techset rules is read at run time: when a file is missing the old rules ran, and that is said once
        for (const std::string& Missing : Techsets::Shared().Problems())
            Approxes.push_back({ "material.techset_data_missing", "", "file=" + Missing, "the BO3 techset data was not found: materials were written with the fixed techset rules and the few keys of old" });

        // which models use each image (through the materials that name it)
        std::map<std::string, std::vector<std::string>> ImageModels;
        for (const auto& M : Resolved.Materials)
        {
            const auto Users = MaterialModels.find(M.Name);
            if (Users == MaterialModels.end()) continue;
            for (const std::string& Image : M.Images)
            {
                auto& List = ImageModels[Image];
                for (const std::string& Model : Users->second)
                {
                    if (std::find(List.begin(), List.end(), Model) == List.end()) List.push_back(Model);
                }
            }
        }

        // 3. the assets, named (Shorten), checked against the gdt.db and placed
        std::vector<Asset> Assets;
        std::map<std::string, std::string> Renamed;     // an entity's name -> the name it is written under
        const auto Short = [&](const std::string& Name, const std::string& Kind) {
            const std::string S = Pl::Shorten(Name);
            if (S != Name)
            {
                Renamed[Name] = S;
                Approxes.push_back({ "name_shortened", S, "kind=" + Kind + ", from=" + Name, "the name is over 63 characters: shortened as MaterialEntryNames.Shorten does" });
                Count("name_shortened");
            }
            return S;
        };
        const auto Held = [&](const Asset& A) {
            if (Know.Held == nullptr || !Know.Held->Matches(O.Match, Lower(A.Kind), A.Name)) return 0;
            const std::string Token = Pl::NameGameOfStem(StemOf(A.Name));
            return !Token.empty() && Token != "t7" ? 2 : 1;
        };
        const auto Place = [&](Asset& A, const std::string& Root, int Families) {
            Pl::Request Q;
            Q.Kind = A.Kind;
            Q.Name = A.Name;
            Q.GameId = GameId;
            Q.FamilyRoot = Root;
            Q.FamilyCount = Families;
            A.Placed = Pl::Place(Q, *Know.Stock);
            A.Dest = A.Placed.GdtPath;
            if (A.Placed.Redirected) Count("stock_path_redirects");
        };
        const auto Decide = [&](Asset& A, const std::string& Root, int Families) {
            const int H = Held(A);
            if (H != 0)
            {
                A.Active = false;
                A.Action = H == 1 ? "already_in_mod_tools" : "name_conflict";
                A.Reason = std::format("gdt.db holds {} {} (match={}){}", Lower(A.Kind), A.Name, O.Match == GdtDb::MatchMode::Type ? "type" : "name",
                                       H == 2 ? ", a name of another game" : "");
                Count(A.Action);
                return;
            }
            Place(A, Root, Families);
            if (A.Placed.Rejected || A.Dest.empty())
            {
                A.Active = false;
                A.Action = "placement_rejected";
                A.Reason = "the name gives no safe path: " + A.Placed.Reason;
            }
        };

        for (const auto& [Name, M] : Models)
        {
            if (M.Bins.empty()) continue;
            Asset A;
            A.Kind = "xmodel";
            A.Gdf = "xmodel.gdf";
            A.From = Name;
            A.Name = Short(Name, "xmodel");
            A.ManifestType = "xmodel_bin";
            Decide(A, A.Name, 0);
            Assets.push_back(std::move(A));
        }
        for (const auto& [Name, M] : Models)
        {
            if (M.Bins.empty() && !M.Sidecars.empty()) Count("models_without_xmodel_bin");
        }
        for (const auto& M : Resolved.Materials)
        {
            Asset A;
            A.Kind = "material";
            A.Gdf = "material.gdf";
            A.From = M.Name;
            A.Name = Short(M.Name, "material");
            A.ManifestType = "gdt";
            const auto Users = MaterialModels.find(M.Name);
            Decide(A, Users == MaterialModels.end() ? std::string() : Pl::Shorten(Users->second.front()), 0);
            // the techset came from the catalog: only what was not exact is reported, with what the rule did
            for (const auto& N : M.Notes) Approxes.push_back({ N.Code, A.Name, N.Detail, N.Message });
            if (!M.Targeted && Lower(GameId) != "bo3")
            {
                const auto T = TechsetOf.find(M.Name);
                std::string Type;
                for (const auto& [K, V] : M.Fields)
                {
                    if (K == "materialType") Type = V;
                }
                Approxes.push_back({ "material.techset_description", A.Name, "materialType=" + Type + ", source_techset=" + (T == TechsetOf.end() ? "" : T->second),
                                     "the techset follows the material description, not an exact match of the source game's technique" });
            }
            Gdt::Fields F = M.Fields;
            for (auto& [Key, Value] : F)
            {
                if (const auto Hit = Renamed.find(Value); Hit != Renamed.end() && Value.size() > 63) Value = Hit->second;
            }
            // an image slot names the image entity as written: shorten it as the image will be
            for (auto& [Key, Value] : F)
            {
                if (std::find(M.Images.begin(), M.Images.end(), Value) != M.Images.end()) Value = Pl::Shorten(Value);
            }
            A.Entry = Out::EntryOf(A.Name, A.Gdf, F);
            Assets.push_back(std::move(A));
        }
        for (const auto& I : Resolved.ImagesOut)
        {
            Asset A;
            A.Kind = "image";
            A.Gdf = "image.gdf";
            A.From = I.Name;
            A.Name = Short(I.Name, "image");
            A.ManifestType = "image";
            const auto Users = ImageModels.find(I.Name);
            const int Families = Users == ImageModels.end() ? 0 : static_cast<int>(Users->second.size());
            Decide(A, Users == ImageModels.end() || Users->second.empty() ? std::string() : Pl::Shorten(Users->second.front()), Families);
            if (A.Active)
            {
                std::error_code Ec2;
                // a plan writes no derived image: one the builder worked out is a file this run would make
                const bool PlannedOnly = O.Plan && B.IsDerived(I.File) && !fs::is_regular_file(I.File, Ec2);
                if (!PlannedOnly && !fs::is_regular_file(I.File, Ec2))
                {
                    A.Active = false;
                    A.Action = "no_source";
                    A.Reason = "the image file is missing: " + I.File.string();
                    Count("images_missing");
                }
                else
                {
                    const std::string Ext = Lower(I.File.extension().string());
                    const bool Direct = Ext == ".tif" || Ext == ".tiff" || Ext == ".png";
                    std::string Rel = Pl::ImagePath(A.Placed.FilesFolder, A.Name, false);
                    if (Ext == ".png") Rel = Rel.substr(0, Rel.size() - 4) + ".png";
                    File F;
                    F.Rel = Rel;
                    F.Source = I.File;
                    if (!Direct)
                    {
                        F.Converted = true;
                        Approxes.push_back({ "image.format_converted", A.Name, "from=" + Ext + ", to=.tif", "the image is not a TIFF or PNG: converted to TIFF" });
                        if (!O.Plan)
                        {
                            Rgba8Image Pixels;
                            std::string Error;
                            // a temp name nobody holds: SaveRgba8 never replaces, so a taken name is skipped for the next, and
                            // only a file this run made is removed
                            static std::atomic<unsigned> TempCounter{ 0 };
                            if (LoadRgba8(I.File, Pixels, Error))
                            {
                                for (int Try = 0; Try < 100; ++Try)
                                {
                                    const fs::path Temp = fs::temp_directory_path(Ec) / std::format("graywolf_publish_{}_{}_{}.tif", GetCurrentProcessId(), Assets.size(), TempCounter.fetch_add(1));
                                    if (SaveRgba8(Temp, Pixels, Error))
                                    {
                                        F.Bytes = Slurp(Temp);
                                        F.Hash = Out::Sha256Hex(F.Bytes);
                                        fs::remove(Temp, Ec);
                                        break;
                                    }
                                    if (Error != "exists") break;
                                }
                            }
                            if (F.Hash.empty())
                            {
                                A.Active = false;
                                A.Action = "no_source";
                                A.Reason = "the image cannot be converted to TIFF: " + Error;
                            }
                        }
                    }
                    else if (PlannedOnly)
                    {
                        F.Hash = Out::Sha256Hex("planned:" + I.File.generic_string());
                        Approxes.push_back({ "image.derived_in_plan", A.Name, "", "a derived image: its bytes are not computed in a plan, it would be written" });
                    }
                    else if (!Out::Sha256File(I.File, F.Hash))
                    {
                        A.Active = false;
                        A.Action = "no_source";
                        A.Reason = "the image file cannot be read: " + I.File.string();
                    }
                    if (A.Active)
                    {
                        Gdt::Fields Fields = I.Fields;
                        for (auto& [Key, Value] : Fields)
                        {
                            if (Key == "baseImage") Value = Gdt::Path(Rel);
                        }
                        A.Entry = Out::EntryOf(A.Name, A.Gdf, Fields);
                        A.Files.push_back(std::move(F));
                    }
                }
            }
            Assets.push_back(std::move(A));
        }

        // the xmodels' files and entries (placed above)
        for (Asset& A : Assets)
        {
            if (A.Kind != "xmodel" || !A.Active) continue;
            Model* M = nullptr;
            for (auto& [Name, Candidate] : Models)
            {
                if (Pl::Shorten(Name) == A.Name) M = &Candidate;
            }
            if (M == nullptr) continue;
            std::sort(M->Bins.begin(), M->Bins.end(), [](const auto& X, const auto& Y) { return X.first != Y.first ? X.first < Y.first : X.second < Y.second; });
            static const char* const LodKeys[] = { "filename", "mediumLod", "lowLod", "lowestLod" };
            Gdt::Fields F;
            std::string Folder = A.Placed.FilesFolder.empty() ? A.Dest.substr(0, A.Dest.rfind('\\') == std::string::npos ? 0 : A.Dest.rfind('\\')) : A.Placed.FilesFolder;
            for (size_t K = 0; K < M->Bins.size(); ++K)
            {
                const fs::path& Bin = M->Bins[K].second;
                File Fi;
                Fi.Rel = Join(Folder, Lower(Bin.stem().string()) + ".xmodel_bin");
                Fi.Source = Bin;
                if (!Out::Sha256File(Bin, Fi.Hash))
                {
                    A.Active = false;
                    A.Action = "no_source";
                    A.Reason = "the XMODEL_BIN cannot be read: " + Bin.string();
                    break;
                }
                if (K < 4)
                {
                    // filename is relative to model_export
                    std::string Relative = Fi.Rel;
                    if (StartsWith(Relative, "model_export\\")) Relative = Relative.substr(13);
                    F.emplace_back(LodKeys[K], Gdt::Path(Relative));
                }
                A.Files.push_back(std::move(Fi));
            }
            if (!A.Active) continue;
            const int Bones = BoneCountOfBin(M->Bins.front().second);
            if (Bones < 0)
            {
                Approxes.push_back({ "xmodel.type_assumed", A.Name, "type=rigid", "the bone count of the XMODEL_BIN could not be read: the model is written as rigid" });
                Count("xmodel_type_assumed");
            }
            F.emplace_back("type", Bones > 1 ? "animated" : "rigid");
            SortKeys(F);
            A.Entry = Out::EntryOf(A.Name, A.Gdf, F);
        }

        // 4. a gdt that exists and is not GrayWolf's own (or was changed since) takes all its assets with it
        std::map<std::string, GdtState> Gdts;    // by path key
        for (Asset& A : Assets)
        {
            if (!A.Active) continue;
            GdtState& G = Gdts[Out::PathKey(A.Dest)];
            G.Assets.push_back(&A);
        }
        for (auto& [Key, G] : Gdts)
        {
            const std::string Rel = G.Assets.front()->Dest;
            const fs::path Abs = Gate.Absolute(Rel);
            G.Exists = fs::exists(Abs, Ec);
            if (!G.Exists) continue;
            const Out::ManifestEntry* Listed = Manifest.Find(Rel);
            std::string OnDisk;
            if (Listed == nullptr || !fs::is_regular_file(Abs, Ec) || !Out::Sha256File(Abs, OnDisk)) G.V = Out::Verdict::NotListed;
            else if (OnDisk != Listed->Sha256) G.V = Out::Verdict::Modified;
            else
            {
                G.V = Out::Verdict::Replace;
                G.OldText = Slurp(Abs);
            }
            if (!Out::Allowed(G.V))
            {
                for (Asset* A : G.Assets)
                {
                    A->Active = false;
                    A->Action = "placement_conflict";
                    A->Reason = std::format("{} is {}", Rel, Out::VerdictName(G.V));
                    Count("placement_conflict");
                }
            }
        }

        // 5. each (name, gdf) in exactly one gdt: another file of this root that holds it, or another asset of this run
        std::map<std::string, std::string> InOtherFiles;     // "name|gdf" -> the owned gdt holding it
        {
            std::set<std::string> Dests;
            for (const auto& [Key, G] : Gdts) Dests.insert(Key);
            for (const auto& [Key, E] : Manifest.All())
            {
                if (!Key.ends_with(".gdt") || Dests.count(Key)) continue;
                const fs::path Abs = Gate.Absolute(E.Path);
                std::error_code Ec2;
                if (!fs::is_regular_file(Abs, Ec2)) continue;
                std::vector<Out::GdtEntryText> Entries;
                std::string Error;
                if (!Out::ParseGdt(Slurp(Abs), Entries, Error)) continue;
                for (const auto& Entry : Entries) InOtherFiles.emplace(Lower(Entry.Name) + "|" + Lower(Entry.Gdf), E.Path);
            }
        }
        // An (name, gdf) that GrayWolf's own gdt of this root, one this run also writes, already defines stays there:
        // an asset whose placement now differs (an image the second model shares moves to the texture library) is not
        // defined a second time elsewhere. Overwrite mode drops those old entries, so nothing is kept in place then.
        std::map<std::string, std::string> HeldInPlace;     // "name|gdf" -> the gdt (root-relative) that holds it
        if (O.Merge)
        {
            for (const auto& [Key, G] : Gdts)
            {
                if (!G.Exists || G.V != Out::Verdict::Replace || G.OldText.empty()) continue;
                std::vector<Out::GdtEntryText> Entries;
                std::string Error;
                if (!Out::ParseGdt(G.OldText, Entries, Error)) continue;
                for (const auto& Entry : Entries) HeldInPlace.emplace(Lower(Entry.Name) + "|" + Lower(Entry.Gdf), G.Assets.front()->Dest);
            }
        }
        {
            std::map<std::string, Asset*> Winner;
            std::map<std::string, Asset*> NameWinner;      // one name once per gdt file, whatever its gdf
            std::vector<Asset*> Order;
            for (Asset& A : Assets)
            {
                if (A.Active) Order.push_back(&A);
            }
            for (Asset* A : Order)
            {
                if (!A->Active) continue;
                const std::string Key = Lower(A->Name) + "|" + Lower(A->Gdf);
                if (const auto Other = InOtherFiles.find(Key); Other != InOtherFiles.end())
                {
                    A->Active = false;
                    A->Action = "gdt_duplicate_name";
                    A->Reason = std::format("{} {} is already defined in {}", A->Kind, A->Name, Other->second);
                    Count("gdt_duplicate_name");
                    continue;
                }
                if (const auto Here = HeldInPlace.find(Key); Here != HeldInPlace.end() && Out::PathKey(Here->second) != Out::PathKey(A->Dest))
                {
                    A->Active = false;
                    A->Action = "kept_in_place";
                    A->Reason = std::format("{} {} is already defined in {}; placement now says {}", A->Kind, A->Name, Here->second, A->Dest);
                    Count("kept_in_place");
                    continue;
                }
                const auto Held2 = Winner.find(Key);
                if (Held2 == Winner.end())
                {
                    // a name is unique in a file: another type of the same name placed in the same gdt is dropped, reported
                    const std::string NameKey = Out::PathKey(A->Dest) + "|" + Lower(A->Name);
                    if (const auto Same = NameWinner.find(NameKey); Same != NameWinner.end())
                    {
                        A->Active = false;
                        A->Action = "gdt_duplicate_name";
                        A->Reason = std::format("{} {} has the name of the {} {} in {}", A->Kind, A->Name, Same->second->Kind, Same->second->Name, A->Dest);
                        Count("gdt_duplicate_name");
                        continue;
                    }
                    NameWinner[NameKey] = A;
                    Winner[Key] = A;
                    continue;
                }
                Asset* Keep = Held2->second;
                if (Keep->Entry.Text == A->Entry.Text && Out::PathKey(Keep->Dest) == Out::PathKey(A->Dest))
                {
                    // the same definition twice: dropped quietly (it is one entity)
                    A->Active = false;
                    A->Action = "unchanged";
                    A->Reason = "the same definition as " + Keep->Name;
                    continue;
                }
                const auto Rank = [](const Asset* X) { return std::make_pair(!StartsWith(X->Dest, "texture_assets\\"), Out::PathKey(X->Dest)); };
                Asset* Lose = Rank(A) < Rank(Keep) ? Keep : A;
                if (Lose == Keep) Winner[Key] = A;
                Lose->Active = false;
                Lose->Action = "gdt_duplicate_name";
                Lose->Reason = std::format("{} {} is also placed in {}", Lose->Kind, Lose->Name, (Lose == Keep ? A : Keep)->Dest);
                Count("gdt_duplicate_name");
            }
        }

        // 6. the files: GrayWolf's own or new ones only
        for (Asset& A : Assets)
        {
            if (!A.Active) continue;
            for (File& F : A.Files)
            {
                F.V = Gate.Check(F.Rel, F.Hash);
                if (!Out::Allowed(F.V))
                {
                    A.Active = false;
                    A.Action = "placement_conflict";
                    A.Reason = std::format("{} is {}", F.Rel, Out::VerdictName(F.V));
                    Count("placement_conflict");
                    break;
                }
            }
            if (!A.Active) continue;
            for (const File& F : A.Files) Gate.Claim(F.Rel, F.Hash);
        }

        // 7. write (or plan) the files first: a file that fails takes its asset, and so its gdt entry, out
        const std::string When = NowText();
        const auto Record = [&](const std::string& Rel, const std::string& Hash, const std::string& Type, const std::string& Name) {
            Manifest.Record({ Rel, Hash, Title, Type, Name, When });
        };
        std::string WriteError;
        bool SaveFailed = false;
        // the manifest is saved after the files and after each gdt, so what is on disk is always listed;
        // a failed save is an error of the run (non-zero exit), not a warning
        const auto SaveManifest = [&] {
            if (O.Plan || SaveFailed || !Manifest.Changed()) return;
            if (const Out::Status Saved = Manifest.Save(ManifestFile); !Saved.Ok)
            {
                SaveFailed = true;
                R.Error = Saved.Reason + " (files written so far may be missing from the manifest and are then treated as foreign on the next run)";
                Count("manifest_save_failed");
            }
        };
        for (Asset& A : Assets)
        {
            if (!A.Active) continue;
            bool Changed = false;
            for (const File& F : A.Files)
            {
                if (F.V == Out::Verdict::Unchanged) continue;
                Changed = true;
                if (O.Plan) continue;
                const fs::path Abs = Gate.Absolute(F.Rel);
                bool Ok;
                WriteError.clear();
                // Create: nothing may be there at the move, so a file that appeared since the check is not replaced
                const bool Replace = F.V != Out::Verdict::Create;
                if (F.Converted)
                {
                    Ok = Out::WriteAtomic(Abs, F.Bytes.data(), F.Bytes.size(), WriteError, Replace);
                }
                else
                {
                    const std::string Data = Slurp(F.Source);
                    Ok = Out::Sha256Hex(Data) == F.Hash && Out::WriteAtomic(Abs, Data.data(), Data.size(), WriteError, Replace);
                    if (!Ok && WriteError.empty()) WriteError = "the source changed while it was read: " + F.Source.string();
                }
                if (!Ok)
                {
                    A.Active = false;
                    A.Action = "write_failed";
                    A.Reason = WriteError;
                    Count("write_failed");
                    break;
                }
                Record(F.Rel, F.Hash, A.ManifestType, A.Name);
                Count("files_written");
            }
            if (!A.Active) continue;
            A.Action = Changed ? (O.Plan ? "would_write" : "written") : "unchanged";
            A.Reason = A.Placed.Reason;
            if (A.Kind != "xmodel" && !Changed) A.Action = "unchanged";
        }
        SaveManifest();

        // 8. the gdts as they will be: the old file's other entries kept, the entries of the assets still active in
        std::vector<std::pair<std::string, GdtState*>> ToWrite;
        std::set<std::string> BackedUp;
        for (auto& [Key, G] : Gdts)
        {
            std::vector<Out::GdtEntryText> Incoming;
            std::string Dest;
            for (Asset* A : G.Assets)
            {
                if (!A->Active) continue;
                Incoming.push_back(A->Entry);
                Dest = A->Dest;
            }
            if (Incoming.empty()) continue;
            if (G.Exists && !G.OldText.empty())
            {
                std::string Error;
                G.Parsed = Out::ParseGdt(G.OldText, G.Old, Error);
                if (!G.Parsed)
                {
                    G.Old.clear();
                    Approxes.push_back({ "gdt.unparsable_backed_up", Dest, "", "the old file did not parse: copied to a .bak before it is replaced (" + Error + ")" });
                }
            }
            std::vector<Out::GdtMergeNote> MergeNotes;
            const std::vector<Out::GdtEntryText> Merged = Out::MergeGdt(G.Old, Incoming, !O.Merge, &MergeNotes);
            for (const Out::GdtMergeNote& N : MergeNotes)
            {
                Approxes.push_back({ N.Code, N.Name, "gdf_kept=" + N.KeptGdf + ", gdf_other=" + N.OtherGdf + ", gdt=" + Dest,
                                     N.Code == "gdt_duplicate_name" ? "two entries of one name in one gdt: the " + N.KeptGdf + " is written, the " + N.OtherGdf + " is not"
                                                                    : "the old " + N.OtherGdf + " entry of this name was replaced by a " + N.KeptGdf + " entry" });
                Count(N.Code);
            }
            G.Kept = Merged.size() > Incoming.size() ? Merged.size() - Incoming.size() : 0;
            if (!O.Merge && G.Exists)
            {
                const size_t Lost = G.Old.size() > Incoming.size() ? G.Old.size() - Incoming.size() : 0;
                if (Lost != 0) Count("gdt_entries_dropped_by_overwrite", Lost);
            }
            G.Merged = Out::RenderGdt(Merged);
            G.Hash = Out::Sha256Hex(G.Merged);
            G.Final = Gate.Check(Dest, G.Hash);
            if (!Out::Allowed(G.Final))
            {
                for (Asset* A : G.Assets)
                {
                    if (!A->Active) continue;
                    A->Active = false;
                    A->Action = "placement_conflict";
                    A->Reason = std::format("{} is {}", Dest, Out::VerdictName(G.Final));
                    Count("placement_conflict");
                }
                continue;
            }
            Gate.Claim(Dest, G.Hash);
            ToWrite.emplace_back(Dest, &G);
        }

        // 9. write (or plan) the gdts
        for (const auto& [Dest, G] : ToWrite)
        {
            const bool Changed = G->Final != Out::Verdict::Unchanged;
            Row Info{ "gdt", Dest, Changed ? (O.Plan ? "would_write" : "written") : "unchanged", "", Dest, "" };
            Info.Reason = std::format("{} entries, {} kept from the old file", G->Assets.size(), G->Kept);
            if (Changed && !O.Plan && SaveFailed)
            {
                Info.Action = "write_failed";
                Info.Reason = "not written: the manifest could not be saved";
            }
            else if (Changed && !O.Plan)
            {
                const fs::path Abs = Gate.Absolute(Dest);
                std::string BackupError;
                if (G->Exists && !G->Parsed)
                {
                    const fs::path Backup = Out::FreeBackupPath(Abs);
                    std::error_code CopyEc;
                    fs::copy_file(Abs, Backup, fs::copy_options::none, CopyEc);
                    if (CopyEc)
                        BackupError = "the old file could not be copied to " + Backup.filename().string() + " (" + CopyEc.message() + "): not replaced";
                    else
                    {
                        Count("gdt_backups");
                        Info.Reason += ", old file copied to " + Backup.filename().string();
                    }
                }
                std::string Error = BackupError;
                if (!BackupError.empty() || !Out::WriteAtomic(Abs, G->Merged.data(), G->Merged.size(), Error, G->Final != Out::Verdict::Create))
                {
                    Info.Action = "write_failed";
                    Info.Reason = Error;
                    Count("write_failed");
                    for (Asset* A : G->Assets)
                    {
                        if (A->Active)
                        {
                            A->Action = "write_failed";
                            A->Reason = Error;
                        }
                    }
                }
                else
                {
                    Record(Dest, G->Hash, "gdt", fs::path(Dest).stem().string());
                    Count("gdt_files_written");
                    if (G->Kept != 0) Count("gdt_entries_kept", G->Kept);
                    SaveManifest();
                }
            }
            else if (Changed)
            {
                if (G->Kept != 0) Count("gdt_entries_kept", G->Kept);
            }
            else
            {
                Count("gdt_files_unchanged");
            }
            Rows.push_back(std::move(Info));
        }
        // an entry of a surviving asset whose gdt was unchanged still counts as unchanged; a changed gdt makes
        // the asset written even when its own files were already there
        for (const auto& [Dest, G] : ToWrite)
        {
            if (G->Final == Out::Verdict::Unchanged) continue;
            for (Asset* A : G->Assets)
            {
                if (A->Active && A->Action == "unchanged") A->Action = O.Plan ? "would_write" : "written";
            }
        }
        SaveManifest();

        // 10. the reports
        std::string Placement = Tsv({ "kind", "name", "action", "reason", "gdt", "files" });
        for (Asset& A : Assets)
        {
            if (A.Action.empty()) A.Action = "unchanged";
            std::string Files;
            for (const File& F : A.Files) Files += (Files.empty() ? "" : ";") + F.Rel;
            Count(A.Action);
            Count(A.Action + "_" + A.Kind);
            Placement += Tsv({ A.Kind, A.Name, A.Action, A.Reason.empty() ? A.Placed.Reason : A.Reason, A.Active || !A.Dest.empty() ? A.Dest : std::string(), Files });
        }
        for (const Row& Info : Rows) Placement += Tsv({ Info.Kind, Info.Name, Info.Action, Info.Reason, Info.Gdt, Info.Files });
        for (const std::string& W : R.Warnings) Approxes.push_back({ "publish.warning", "", "", W });
        std::sort(Approxes.begin(), Approxes.end(), [](const Approx& X, const Approx& Y) {
            return std::tie(X.Code, X.Asset, X.Detail, X.Message) < std::tie(Y.Code, Y.Asset, Y.Detail, Y.Message);
        });
        Approxes.erase(std::unique(Approxes.begin(), Approxes.end(), [](const Approx& X, const Approx& Y) {
            return std::tie(X.Code, X.Asset, X.Detail, X.Message) == std::tie(Y.Code, Y.Asset, Y.Detail, Y.Message);
        }), Approxes.end());
        std::string Approximations = Tsv({ "code", "asset", "detail", "message" });
        for (const Approx& A : Approxes) Approximations += Tsv({ A.Code, A.Asset, A.Detail, A.Message });
        Count("approximations", Approxes.size());
        std::string Error;
        if (!Out::WriteAtomic(RunRoot / "graywolf_placement.tsv", Placement.data(), Placement.size(), Error) ||
            !Out::WriteAtomic(RunRoot / "graywolf_approximations.tsv", Approximations.data(), Approximations.size(), Error))
            R.Warnings.push_back("a report was not written: " + Error);
        Count("assets", Assets.size());
        return R;
    }

    // ---- self-test ------------------------------------------------------------------------------------------------------

    namespace
    {
        // one model with its sidecar (materials named, images in <run>/images) and one XMODEL_BIN
        void MakeModel(const fs::path& Run, const std::string& Model, const std::vector<std::pair<std::string, std::vector<std::string>>>& Materials)
        {
            std::error_code Ec;
            const fs::path Dir = Run / "models" / Model;
            fs::create_directories(Dir, Ec);
            fs::create_directories(Run / "images", Ec);
            Materials::Sidecar Side;
            Side.Game = "bo4";
            Side.Adapter = "bo4";
            for (const auto& [Name, Images] : Materials)
            {
                Materials::Description D;
                D.Name = Name;
                for (const std::string& Image : Images)
                {
                    const std::string Path = "../../images/" + Image + ".tiff";
                    Rgba8Image Pixels;
                    Pixels.Width = 1;
                    Pixels.Height = 1;
                    Pixels.Pixels = { 200, 100, 50, 255 };
                    std::string Error;
                    if (!fs::exists(Run / "images" / (Image + ".tiff"), Ec)) SaveRgba8(Run / "images" / (Image + ".tiff"), Pixels, Error);
                    if (Image.ends_with("_n")) D.Normal = { Path, 0, false };
                    else D.Albedo = { Path, 0, false };
                }
                Side.Materials.push_back(D);
            }
            Materials::WriteSidecar((Dir / (Model + "_LOD0.materials.json")).string(), Side);
            std::ofstream(Dir / (Model + "_LOD0.XMODEL_BIN"), std::ios::binary) << "not a real bin " << Model;
        }

        std::map<std::string, std::string> Tree(const fs::path& Root)
        {
            std::map<std::string, std::string> Out2;
            std::error_code Ec;
            for (fs::recursive_directory_iterator It(Root, Ec), End; !Ec && It != End; It.increment(Ec))
            {
                std::error_code Ec2;
                if (!It->is_regular_file(Ec2)) continue;
                std::string Hash;
                Out::Sha256File(It->path(), Hash);
                Out2[fs::relative(It->path(), Root, Ec2).generic_string()] = Hash;
            }
            return Out2;
        }

        std::vector<std::vector<std::string>> ReadTsv(const fs::path& File)
        {
            std::vector<std::vector<std::string>> Rows;
            std::ifstream In(File, std::ios::binary);
            std::string Line;
            while (std::getline(In, Line))
            {
                if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                std::vector<std::string> Cells;
                size_t Start = 0;
                for (;;)
                {
                    const size_t Tab = Line.find('\t', Start);
                    Cells.push_back(Line.substr(Start, Tab == std::string::npos ? std::string::npos : Tab - Start));
                    if (Tab == std::string::npos) break;
                    Start = Tab + 1;
                }
                Rows.push_back(std::move(Cells));
            }
            return Rows;
        }

        // the action of the first row of a kind and name
        std::string ActionOf(const fs::path& Tsv, const std::string& Kind, const std::string& Name)
        {
            for (const auto& Row : ReadTsv(Tsv))
            {
                if (Row.size() >= 3 && Row[0] == Kind && Row[1] == Name) return Row[2];
            }
            return "(no row)";
        }

        std::string RowOf(const fs::path& Tsv, const std::string& Kind, const std::string& Name)
        {
            for (const auto& Row : ReadTsv(Tsv))
            {
                if (Row.size() >= 3 && Row[0] == Kind && Row[1] == Name)
                {
                    std::string Joined;
                    for (const auto& Cell : Row) Joined += Cell + " | ";
                    return Joined;
                }
            }
            return "(no row)";
        }
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL Publish {} (got '{}')\n", Name, Got);
        };

        std::error_code Ec;
        const fs::path Base = fs::temp_directory_path() / std::format("graywolf_selftest_publish_{}", static_cast<unsigned long long>(GetCurrentProcessId()));
        fs::remove_all(Base, Ec);

        // The Mod Tools as the tests know them: stock xmodels in p7_crate (a stock gdt path), a held material
        const std::vector<GdtDb::Row> StockRows = {
            { 1, "p7_crate_a", "xmodel", "model_export\\t7_props\\p7_crate\\p7_crate.gdt" },
            { 2, "i_p7_crate_a_c", "image", "model_export\\t7_props\\p7_crate\\p7_crate.gdt" },
            { 3, "mtl_held_one", "material", "texture_assets\\t7_stuff.gdt" },
            { 4, "mtl_t8_foreign", "material", "texture_assets\\t7_stuff.gdt" },
        };
        NoTimes Times;
        const GdtDb::StockIndex Stock(StockRows, GdtDb::StockRules{}, Times);
        GdtDb::Catalog Catalog;
        for (const auto& Row : StockRows) Catalog.Add(Row);
        const Knowledge Know{ &Catalog, &Stock };

        // a run with two models: p7_crate_x (stock-near: P17) and p7_widget_big_02 (a prop set); both use mtl_shared
        const auto MakeRun = [&](const fs::path& Run) {
            MakeModel(Run, "p7_crate_x", { { "mtl_shared", { "i_mtl_shared_c", "i_mtl_shared_n" } }, { "mtl_crate_only", { "i_mtl_crate_only_c" } },
                                           { "mtl_held_one", { "i_mtl_held_one_c" } }, { "mtl_t8_foreign", { "i_mtl_t8_foreign_c" } } });
            MakeModel(Run, "p7_widget_big_02", { { "mtl_shared", { "i_mtl_shared_c", "i_mtl_shared_n" } } });
        };
        const auto Go = [&](const fs::path& RunDir, const fs::path& Root, const Knowledge& K, bool Plan = false, bool Merge = true) {
            Options O;
            O.Which = Target::Folder;
            O.OutputRoot = Root;
            O.Plan = Plan;
            O.Merge = Merge;
            return Publish::Run(O, K, RunDir, "bo4");
        };
        const auto Exists = [&](const fs::path& P) { std::error_code E; return fs::exists(P, E); };
        const auto Contains = [](const std::string& Text, const std::string& Part) { return Text.find(Part) != std::string::npos; };
        // the file listed in the manifest again with the hash it has now (an edit a run of GrayWolf made)
        const auto Relist = [&](const fs::path& Root, const std::string& Rel) {
            Out::Manifest M;
            M.Load(Root / "graywolf" / "manifest.json");
            std::string Hash;
            Out::Sha256File(Root / fs::path(Rel), Hash);
            M.Record({ Rel, Hash, "BlackOps4", "gdt", "x", "t" });
            M.Save(Root / "graywolf" / "manifest.json");
        };
        const std::string CrateGdt = "model_export\\t7_props\\p7_crate\\graywolf_p7_crate.gdt";
        const std::string WidgetGdt = "model_export\\t7_props\\p7_widget_big\\p7_widget_big.gdt";
        const std::string CrateBin = "model_export\\t7_props\\p7_crate\\p7_crate_x_lod0.xmodel_bin";
        const std::string CrateImage = "model_export\\t7_props\\p7_crate\\mtl_crate_only_c.tif";

        // 1. a first run: stock-like paths, the stock gdt path redirected, the gdt.db's assets held
        const fs::path Run1 = Base / "t1" / "bo4", Root1 = Run1 / "bo3";
        {
            MakeRun(Run1);
            Result R = Go(Run1, Root1, Know);
            const fs::path Tsv = Run1 / "graywolf_placement.tsv";
            Check(R.Error.empty(), "run 1: no error", R.Error);
            Check(Exists(Root1 / CrateBin) && Slurp(Root1 / CrateBin) == "not a real bin p7_crate_x", "run 1: the XMODEL_BIN is under the model folder, as .xmodel_bin", "");
            Check(Exists(Root1 / CrateGdt) && !Exists(Root1 / "model_export\\t7_props\\p7_crate\\p7_crate.gdt"), "run 1: a stock gdt path is redirected to graywolf_<file>.gdt", "");
            Check(Exists(Root1 / WidgetGdt), "run 1: a prop set gdt is <set>\\<set>.gdt", "");
            const std::string Gdt1 = Slurp(Root1 / CrateGdt);
            Check(Contains(Gdt1, "\"p7_crate_x\" ( \"xmodel.gdf\" )") && Contains(Gdt1, "\"filename\" \"t7_props\\\\p7_crate\\\\p7_crate_x_lod0.xmodel_bin\""),
                  "run 1: the xmodel entry filename is relative to model_export, backslashes doubled", Gdt1);
            Check(Contains(Gdt1, "\"type\" \"rigid\""), "run 1: the xmodel entry has a type", Gdt1);
            Check(Exists(Root1 / CrateImage) && !Exists(Root1 / "model_export\\t7_props\\p7_crate\\i_mtl_crate_only_c.tif") && !Exists(Root1 / "model_export\\t7_props\\p7_crate\\mtl_crate_only_c.tiff"),
                  "run 1: image files are .tif and their stems drop i_", "");
            Check(Contains(Gdt1, "\"baseImage\" \"model_export\\\\t7_props\\\\p7_crate\\\\mtl_crate_only_c.tif\"") && Contains(Gdt1, "\"i_mtl_crate_only_c\" ( \"image.gdf\" )"),
                  "run 1: baseImage is root-relative with doubled backslashes", Gdt1);
            Check(ActionOf(Tsv, "xmodel", "p7_crate_x") == "written" && ActionOf(Tsv, "image", "i_mtl_crate_only_c") == "written", "run 1: assets are written", RowOf(Tsv, "xmodel", "p7_crate_x"));
            Check(ActionOf(Tsv, "material", "mtl_held_one") == "already_in_mod_tools", "run 1: a material the gdt.db holds is already_in_mod_tools", RowOf(Tsv, "material", "mtl_held_one"));
            Check(ActionOf(Tsv, "image", "i_mtl_held_one_c") == "written", "run 1: a held material image, missing from the Mod Tools, is still exported", RowOf(Tsv, "image", "i_mtl_held_one_c"));
            Check(ActionOf(Tsv, "material", "mtl_t8_foreign") == "name_conflict", "run 1: a held name of another game is name_conflict", RowOf(Tsv, "material", "mtl_t8_foreign"));
            Check(!Contains(Gdt1, "\"mtl_held_one\"") && !Contains(Slurp(Root1 / WidgetGdt), "\"mtl_held_one\""), "run 1: a held asset has no entry", "");
            const std::string Shared = RowOf(Tsv, "image", "i_mtl_shared_c");
            Check(Contains(Shared, "texture_assets\\"), "run 1: an image two models use goes to the texture library", Shared);
            Out::Manifest M;
            Check(M.Load(Root1 / "graywolf" / "manifest.json").Ok && M.Find(CrateBin) != nullptr && M.Find(CrateGdt) != nullptr &&
                  M.Find(CrateBin)->Title == "BlackOps4" && M.Find(CrateBin)->AssetType == "xmodel_bin", "run 1: the manifest lists every file written", std::to_string(M.Size()));
            std::string Hash;
            Out::Sha256File(Root1 / CrateBin, Hash);
            Check(M.Find(CrateBin) != nullptr && M.Find(CrateBin)->Sha256 == Hash, "run 1: the manifest hash is the file hash", "");
            Check(R.Counters.count("publish_stock_path_redirects") != 0, "run 1: the redirect is counted", "");
            Check(Exists(Run1 / "graywolf_approximations.tsv"), "run 1: the approximations are reported", "");
            Check(R.Counters.count("publish_written") != 0 && R.Counters.at("publish_already_in_mod_tools") >= 1 && R.Counters.at("publish_name_conflict") >= 1, "run 1: counters", "");
        }

        // 2. a second run changes nothing
        {
            const auto Before = Tree(Root1);
            Result R = Go(Run1, Root1, Know);
            const auto After = Tree(Root1);
            Check(Before == After, "run 2: the tree is byte-identical", std::format("{} vs {} files", Before.size(), After.size()));
            Check(R.Counters["publish_files_written"] == 0 && R.Counters["publish_gdt_files_written"] == 0, "run 2: nothing is written", std::to_string(R.Counters["publish_files_written"]));
            bool Written = false;
            for (const auto& Row : ReadTsv(Run1 / "graywolf_placement.tsv"))
            {
                if (Row.size() >= 3 && (Row[2] == "written" || Row[2] == "would_write")) Written = true;
            }
            Check(!Written, "run 2: no row says written", "");
        }

        // 3. a modified owned file: the asset is skipped as placement_conflict, the file keeps its edit
        {
            { std::ofstream(Root1 / CrateBin, std::ios::binary) << "edited by the user"; }
            Result R = Go(Run1, Root1, Know);
            Check(ActionOf(Run1 / "graywolf_placement.tsv", "xmodel", "p7_crate_x") == "placement_conflict" &&
                  Contains(RowOf(Run1 / "graywolf_placement.tsv", "xmodel", "p7_crate_x"), "modified_since_write"), "modified file: placement_conflict, hash mismatch", RowOf(Run1 / "graywolf_placement.tsv", "xmodel", "p7_crate_x"));
            Check(Slurp(Root1 / CrateBin) == "edited by the user", "modified file: the edit is kept", "");
            Check(R.Counters["publish_placement_conflict"] >= 1, "modified file: counted", "");
        }

        // 4. an unlisted file at a planned path is foreign
        {
            const fs::path RunDir = Base / "t4" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            std::error_code E;
            fs::create_directories((Root / CrateImage).parent_path(), E);
            { std::ofstream(Root / CrateImage, std::ios::binary) << "the user own picture"; }
            Result R = Go(RunDir, Root, Know);
            Check(ActionOf(RunDir / "graywolf_placement.tsv", "image", "i_mtl_crate_only_c") == "placement_conflict" &&
                  Contains(RowOf(RunDir / "graywolf_placement.tsv", "image", "i_mtl_crate_only_c"), "foreign_file"), "foreign file: placement_conflict", RowOf(RunDir / "graywolf_placement.tsv", "image", "i_mtl_crate_only_c"));
            Check(Slurp(Root / CrateImage) == "the user own picture", "foreign file: never overwritten", "");
            Check(!Contains(Slurp(Root / CrateGdt), "\"i_mtl_crate_only_c\" ( \"image.gdf\" )") && ActionOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_crate_x") == "written",
                  "foreign file: only that asset is skipped, with no entry", RowOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_crate_x") + Slurp(Root / CrateGdt));
        }

        // 5. merge keeps the entries only the old file holds; overwrite does not
        {
            const fs::path RunDir = Base / "t5" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            Go(RunDir, Root, Know);
            std::string Text = Slurp(Root / WidgetGdt);
            const std::string Other = "\t\"zz_other_model\" ( \"xmodel.gdf\" )\r\n\t{\r\n\t\t\"type\" \"animated\"\r\n\t}\r\n";
            Text.insert(Text.find("{\r\n") + 3, Other);
            { std::ofstream(Root / WidgetGdt, std::ios::binary) << Text; }
            Relist(Root, WidgetGdt);
            Result R = Go(RunDir, Root, Know);
            const std::string After = Slurp(Root / WidgetGdt);
            Check(Contains(After, "\"zz_other_model\" ( \"xmodel.gdf\" )\r\n\t{\r\n\t\t\"type\" \"animated\"\r\n\t}\r\n") && Contains(After, "\"p7_widget_big_02\" ( \"xmodel.gdf\" )"),
                  "merge: an entry only the old file holds is kept byte for byte, the new one is there", After);
            Check(R.Counters["publish_gdt_entries_kept"] >= 1, "merge: the kept entries are counted", "");
            Result Over = Go(RunDir, Root, Know, false, false);
            Check(!Contains(Slurp(Root / WidgetGdt), "zz_other_model") && Over.Counters["publish_gdt_entries_dropped_by_overwrite"] >= 1, "overwrite: the file is replaced whole, the loss counted", "");
        }

        // 6. an unlisted gdt takes its assets with it, and is not touched
        {
            const fs::path RunDir = Base / "t6" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            std::error_code E;
            fs::create_directories((Root / WidgetGdt).parent_path(), E);
            { std::ofstream(Root / WidgetGdt, std::ios::binary) << "{\r\n}\r\n"; }
            Go(RunDir, Root, Know);
            Check(Slurp(Root / WidgetGdt) == "{\r\n}\r\n" && ActionOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_widget_big_02") == "placement_conflict",
                  "foreign gdt: not merged into, its asset is placement_conflict", RowOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_widget_big_02"));
            Check(Exists(Root / CrateGdt), "foreign gdt: other gdts are still written", "");
        }

        // 7. an owned gdt that no longer parses is copied to .bak first
        {
            const fs::path RunDir = Base / "t7" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            Go(RunDir, Root, Know);
            { std::ofstream(Root / WidgetGdt, std::ios::binary) << "this is not a gdt"; }
            Relist(Root, WidgetGdt);
            Result R = Go(RunDir, Root, Know);
            std::vector<Out::GdtEntryText> Entries;
            std::string Error;
            Check(Exists(Root / (WidgetGdt + ".bak")) && Slurp(Root / (WidgetGdt + ".bak")) == "this is not a gdt" && Out::ParseGdt(Slurp(Root / WidgetGdt), Entries, Error) && !Entries.empty(),
                  "unparsable gdt: copied to .bak, then replaced", Error);
            Check(R.Counters["publish_gdt_backups"] == 1, "unparsable gdt: counted", "");
        }

        // 8. each entry in exactly one gdt: one another owned file already holds is reported, not written twice
        {
            const fs::path RunDir = Base / "t8" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            const std::string Other = "source_data\\other.gdt";
            std::error_code E;
            fs::create_directories((Root / Other).parent_path(), E);
            { std::ofstream(Root / Other, std::ios::binary) << Out::RenderGdt({ Out::EntryOf("mtl_crate_only", "material.gdf", { { "materialType", "lit" } }) }); }
            Relist(Root, Other);
            Go(RunDir, Root, Know);
            Check(ActionOf(RunDir / "graywolf_placement.tsv", "material", "mtl_crate_only") == "gdt_duplicate_name" && !Contains(Slurp(Root / CrateGdt), "\"mtl_crate_only\" ( \"material.gdf\" )"),
                  "duplicate name: reported gdt_duplicate_name, not written a second time", RowOf(RunDir / "graywolf_placement.tsv", "material", "mtl_crate_only"));
        }

        // 9. a plan writes nothing but its reports
        {
            const fs::path RunDir = Base / "t9" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            const auto ImagesBefore = Tree(RunDir / "images");
            Result R = Go(RunDir, Root, Know, true);
            Check(Tree(Root).empty() && !Exists(Root / "graywolf"), "plan: no file in the target", std::to_string(Tree(Root).size()));
            Check(Tree(RunDir / "images") == ImagesBefore, "plan: no image written in the run either", "");
            Check(ActionOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_crate_x") == "would_write" && Exists(RunDir / "graywolf_approximations.tsv"), "plan: the reports say what would be written", RowOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_crate_x"));
            Check(R.Counters["publish_files_written"] == 0, "plan: no file counted written", "");
        }

        // 10. an unreadable manifest aborts before any write
        {
            const fs::path RunDir = Base / "t10" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            std::error_code E;
            fs::create_directories(Root / "graywolf", E);
            { std::ofstream(Root / "graywolf" / "manifest.json", std::ios::binary) << "{ broken"; }
            Result R = Go(RunDir, Root, Know);
            Check(!R.Error.empty() && Tree(Root).size() == 1 && !Exists(RunDir / "graywolf_placement.tsv"), "unreadable manifest: aborts before any write", R.Error);
            Options O;
            O.Which = Target::Folder;
            O.OutputRoot = Root;
            Check(!Preflight(O).empty(), "unreadable manifest: refused up front too", "");
        }

        // 11. the install guard
        {
            const fs::path Fake = Base / "t11" / "install";
            std::error_code E;
            fs::create_directories(Fake / "bin", E);
            { std::ofstream(Fake / "bin" / "linker_modtools.exe") << "x"; }
            Options O;
            O.Which = Target::Folder;
            O.OutputRoot = Fake / "out" / "bo3";
            Check(!Preflight(O).empty(), "install guard: --target folder inside an install is refused", Preflight(O));
            O.OutputRoot = Base / "t11" / "elsewhere" / "bo3";
            Check(Preflight(O).empty(), "install guard: a folder outside is accepted", Preflight(O));
            O.Which = Target::ModTools;
            O.ModToolsRoot = Base / "t11" / "elsewhere";
            Check(!Preflight(O).empty(), "modtools: a folder that is no install is refused", Preflight(O));
            O.ModToolsRoot = Fake;
            O.OutputRoot = Fake;
            Check(Preflight(O).empty(), "modtools: an install is accepted", Preflight(O));
            Target T;
            Check(ParseTarget("FOLDER", T) && T == Target::Folder && ParseTarget("modtools", T) && T == Target::ModTools && ParseTarget("none", T) && T == Target::None && !ParseTarget("", T) && !ParseTarget("root", T),
                  "target words", "");
        }

        // 12. names over 63 characters are shortened
        {
            const fs::path RunDir = Base / "t12" / "bo4", Root = RunDir / "bo3";
            const std::string Long = "mtl_p7_crate_with_a_very_long_name_that_goes_past_sixty_three_characters_ok";
            MakeModel(RunDir, "p7_crate_y", { { Long, { "i_" + Long + "_c" } } });
            Go(RunDir, Root, Know);
            const std::string Short = Pl::Shorten(Long), ShortImage = Pl::Shorten("i_" + Long + "_c");
            const std::string Gdt = Slurp(Root / CrateGdt);
            Check(Short.size() == 63 && Contains(Gdt, "\"" + Short + "\" ( \"material.gdf\" )") && !Contains(Gdt, "\"" + Long + "\"") && Contains(Gdt, "\"colorMap\" \"" + ShortImage + "\""),
                  "names over 63: shortened in the entry and in the slot that names the image", Gdt);
            Check(Contains(Slurp(RunDir / "graywolf_approximations.tsv"), "name_shortened"), "names over 63: reported", "");
        }

        // 13. no gdt.db check: a held asset is written
        {
            const fs::path RunDir = Base / "t13" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            const Knowledge NoCheck{ nullptr, &Stock };
            Go(RunDir, Root, NoCheck);
            Check(ActionOf(RunDir / "graywolf_placement.tsv", "material", "mtl_held_one") == "written", "check off: a held asset is written", RowOf(RunDir / "graywolf_placement.tsv", "material", "mtl_held_one"));
        }

        // 14. a sidecar is data: an absolute path or ../ makes nothing outside the run read, written or deleted
        {
            const fs::path RunDir = Base / "t14" / "bo4", Root = RunDir / "bo3", Outside = Base / "t14" / "outside";
            std::error_code E;
            fs::create_directories(Outside, E);
            const auto Pixel = [](uint8_t V) {
                Rgba8Image P;
                P.Width = 1;
                P.Height = 1;
                P.Pixels = { V, 128, 64, 255 };
                return P;
            };
            std::string Error;
            SaveRgba8(Outside / "o_nog.png", Pixel(10), Error);
            SaveRgba8(Outside / "o2_nog.png", Pixel(20), Error);
            SaveRgba8(Outside / "o_nog_n.png", Pixel(30), Error);       // a file a derive would have replaced
            SaveRgba8(Outside / "o2_nog_o.png", Pixel(40), Error);
            const auto OutsideBefore = Tree(Outside);
            const fs::path Dir = RunDir / "models" / "p7_esc";
            fs::create_directories(Dir, E);
            Materials::Sidecar Side;
            Side.Game = "bo4";
            Side.Adapter = "bo4";
            Materials::Description D;
            D.Name = "mtl_escape";
            D.Albedo = { (Outside / "o_nog.png").string(), 0, false };                                  // absolute
            D.Normal = { (Outside / "o_nog.png").string(), 0, false, Materials::PackKind::Nog };         // absolute, to be derived
            D.Occlusion = { "../../../outside/o2_nog.png", 2, false, Materials::PackKind::Nog };        // ../ out of the run
            Side.Materials.push_back(D);
            Materials::WriteSidecar((Dir / "p7_esc_LOD0.materials.json").string(), Side);
            std::ofstream(Dir / "p7_esc_LOD0.XMODEL_BIN", std::ios::binary) << "bin";
            Result R = Go(RunDir, Root, Know);
            Check(R.Error.empty(), "sidecar escape: no error", R.Error);
            Check(Tree(Outside) == OutsideBefore, "sidecar escape: nothing outside the run read into a write, written or deleted", std::format("{} vs {} files", OutsideBefore.size(), Tree(Outside).size()));
            Check(R.Counters["publish_builder_images_outside"] >= 1 && R.Counters["publish_builder_images_derived"] == 0, "sidecar escape: counted images_outside, nothing derived",
                  std::format("{} {}", R.Counters["publish_builder_images_outside"], R.Counters["publish_builder_images_derived"]));
            bool Leaks = false;
            for (const auto& [Rel, Hash] : Tree(Root))
            {
                if (Rel.find("outside") != std::string::npos || Rel.find("o_nog") != std::string::npos || Rel.find("o2_nog") != std::string::npos) Leaks = true;
            }
            Check(!Leaks, "sidecar escape: no image of the outside is published", "");
        }

        // 15. a failed write takes its asset, and so its gdt entry, out; a failed manifest save is the run's error
        {
            const fs::path RunDir = Base / "t15" / "bo4", Root = RunDir / "bo3";
            MakeRun(RunDir);
            Go(RunDir, Root, Know);
            Check(Contains(Slurp(Root / CrateGdt), "\"p7_crate_x\" ( \"xmodel.gdf\" )"), "failed write: the first run has the entry", "");
            // the bin changes (so it is replaced) and the old file is read-only (so the replacement fails)
            std::ofstream(RunDir / "models" / "p7_crate_x" / "p7_crate_x_LOD0.XMODEL_BIN", std::ios::binary | std::ios::trunc) << "a new bin";
            SetFileAttributesW((Root / CrateBin).c_str(), FILE_ATTRIBUTE_READONLY);
            Result R = Go(RunDir, Root, Know, false, false);
            SetFileAttributesW((Root / CrateBin).c_str(), FILE_ATTRIBUTE_NORMAL);
            Check(ActionOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_crate_x") == "write_failed" && R.Counters["publish_write_failed"] >= 1,
                  "failed write: the asset is write_failed", RowOf(RunDir / "graywolf_placement.tsv", "xmodel", "p7_crate_x"));
            Check(!Contains(Slurp(Root / CrateGdt), "\"p7_crate_x\" ( \"xmodel.gdf\" )") && Slurp(Root / CrateBin) == "not a real bin p7_crate_x",
                  "failed write: its gdt entry is not written and its file is as it was", Slurp(Root / CrateGdt));

            // a new model, and a manifest that cannot be replaced: an error, not a warning
            MakeModel(RunDir, "p7_gadget_z", { { "mtl_gadget", { "i_mtl_gadget_c" } } });
            const fs::path ManifestFile = Root / "graywolf" / "manifest.json";
            SetFileAttributesW(ManifestFile.c_str(), FILE_ATTRIBUTE_READONLY);
            Result Bad = Go(RunDir, Root, Know);
            SetFileAttributesW(ManifestFile.c_str(), FILE_ATTRIBUTE_NORMAL);
            Check(!Bad.Error.empty() && Contains(Bad.Error, "manifest"), "manifest save failure: the run has an error", Bad.Error);
            Check(Bad.Counters["publish_manifest_save_failed"] == 1, "manifest save failure: counted once, the run stops saving", std::to_string(Bad.Counters["publish_manifest_save_failed"]));
        }

        // 16. derived images: a rerun is byte-identical, a plan says would_write, a real image is never replaced
        {
            const fs::path RunDir = Base / "t16" / "bo4", Root = RunDir / "bo3", PlanRoot = Base / "t16" / "plan";
            std::error_code E;
            const fs::path Dir = RunDir / "models" / "p7_pack_a";
            fs::create_directories(Dir, E);
            fs::create_directories(RunDir / "images", E);
            std::string Error;
            Rgba8Image Nog;
            Nog.Width = 2;
            Nog.Height = 1;
            Nog.Pixels = { 64, 255, 192, 0,  64, 128, 192, 128 };
            SaveRgba8(RunDir / "images" / "pk_nog.png", Nog, Error);
            // a real exported image that sits where the first derived name would go
            Rgba8Image Real;
            Real.Width = 1;
            Real.Height = 1;
            Real.Pixels = { 1, 2, 3, 255 };
            SaveRgba8(RunDir / "images" / "pk_nog_n.png", Real, Error);
            std::string RealHash;
            Out::Sha256File(RunDir / "images" / "pk_nog_n.png", RealHash);
            Materials::Sidecar Side;
            Side.Game = "bo4";
            Side.Adapter = "bo4";
            Materials::Description D;
            D.Name = "mtl_pack_a";
            D.Normal = { "../../images/pk_nog.png", 0, false, Materials::PackKind::Nog };
            Side.Materials.push_back(D);
            Materials::WriteSidecar((Dir / "p7_pack_a_LOD0.materials.json").string(), Side);
            std::ofstream(Dir / "p7_pack_a_LOD0.XMODEL_BIN", std::ios::binary) << "bin";

            Result Plan = Go(RunDir, PlanRoot, Know, true);
            const std::string PlannedImage = ActionOf(RunDir / "graywolf_placement.tsv", "image", "i_pk_nog_n_2");
            Check(PlannedImage == "would_write", "derived plan: a derived image would be written, not no_source",
                  RowOf(RunDir / "graywolf_placement.tsv", "image", "i_pk_nog_n_2") + Plan.Error);
            Check(!Exists(RunDir / "images" / "pk_nog_n_2.png") && Tree(PlanRoot).empty(), "derived plan: nothing written", "");

            Result First = Go(RunDir, Root, Know);
            Out::Sha256File(RunDir / "images" / "pk_nog_n.png", Error);
            Check(Error == RealHash, "derived: a real image of the same name is not replaced", Error);
            Check(Exists(RunDir / "images" / "pk_nog_n_2.png"), "derived: the next free name is taken", "");
            const auto ImagesBefore = Tree(RunDir / "images"), RootBefore = Tree(Root);
            Result Second = Go(RunDir, Root, Know);
            Check(Tree(RunDir / "images") == ImagesBefore, "derived rerun: the run's images are byte-identical", std::format("{} vs {}", ImagesBefore.size(), Tree(RunDir / "images").size()));
            Check(Tree(Root) == RootBefore && Second.Counters["publish_files_written"] == 0, "derived rerun: the published tree is byte-identical, nothing written", std::to_string(Second.Counters["publish_files_written"]));
            Check(First.Error.empty() && Second.Error.empty(), "derived rerun: no errors", First.Error + Second.Error);
        }

        // 18. an image the second model shares stays in the gdt that already defines it: each (name, gdf) in one gdt
        {
            const fs::path RunDir = Base / "t18" / "bo4", Root = RunDir / "bo3";
            MakeModel(RunDir, "p7_crate_x", { { "mtl_shared", { "i_mtl_shared_c" } }, { "mtl_crate_only", { "i_mtl_crate_only_c" } } });
            Result First = Go(RunDir, Root, Know);
            MakeModel(RunDir, "p7_widget_big_02", { { "mtl_shared", { "i_mtl_shared_c" } } });
            Result Second = Go(RunDir, Root, Know);
            const auto Defined = [&](std::map<std::string, int>& Counts) {
                Counts.clear();
                std::error_code E;
                for (fs::recursive_directory_iterator It(Root, E), End; !E && It != End; It.increment(E))
                {
                    if (!It->is_regular_file(E) || Lower(It->path().extension().string()) != ".gdt") continue;
                    std::vector<Out::GdtEntryText> Entries;
                    std::string Error;
                    if (!Out::ParseGdt(Slurp(It->path()), Entries, Error)) continue;
                    for (const auto& Entry : Entries) ++Counts[Lower(Entry.Name) + "|" + Lower(Entry.Gdf)];
                }
            };
            std::map<std::string, int> Counts;
            Defined(Counts);
            size_t Twice = 0;
            for (const auto& [Key, N] : Counts) Twice += N > 1 ? 1 : 0;
            Check(First.Error.empty() && Second.Error.empty() && !Counts.empty() && Twice == 0, "kept in place: every name is in exactly one gdt", std::format("{} twice of {}", Twice, Counts.size()));
            Check(Counts["i_mtl_shared_c|image.gdf"] == 1, "kept in place: the shared image has one definition", std::to_string(Counts["i_mtl_shared_c|image.gdf"]));
            Check(ActionOf(RunDir / "graywolf_placement.tsv", "image", "i_mtl_shared_c") == "kept_in_place" && Second.Counters["publish_kept_in_place"] >= 1,
                  "kept in place: reported", RowOf(RunDir / "graywolf_placement.tsv", "image", "i_mtl_shared_c"));
            const auto Before = Tree(Root);
            Result Third = Go(RunDir, Root, Know);
            Defined(Counts);
            Twice = 0;
            for (const auto& [Key, N] : Counts) Twice += N > 1 ? 1 : 0;
            Check(Tree(Root) == Before && Twice == 0 && Third.Counters["publish_files_written"] == 0, "kept in place: a third run is byte-identical", std::to_string(Twice));
        }

        // 19. a model and a material of one name in one gdt: the first stays, the other is reported
        {
            const fs::path RunDir = Base / "t19" / "bo4", Root = RunDir / "bo3";
            MakeModel(RunDir, "p7_dup_a", { { "p7_dup_a", { "i_p7_dup_a_c" } } });
            Result R = Go(RunDir, Root, Know);
            const fs::path Tsv = RunDir / "graywolf_placement.tsv";
            const std::string Kinds = ActionOf(Tsv, "xmodel", "p7_dup_a") + "/" + ActionOf(Tsv, "material", "p7_dup_a");
            Check(R.Error.empty() && Kinds == "written/gdt_duplicate_name", "same name, two types: the model stays, the material is gdt_duplicate_name", Kinds);
        }

        // 20. images written to a folder outside the ANSI code page, and never over a file
        {
            const fs::path Dir = Base / "t20" / (std::wstring(L"w\u00e9ird_\u4e2d\u6587_\u0416"));
            Rgba8Image Pixels;
            Pixels.Width = 1;
            Pixels.Height = 1;
            Pixels.Pixels = { 10, 20, 30, 255 };
            for (const char* Name : { "wide.tif", "wide.png", "wide.tga" })
            {
                std::string Error;
                Rgba8Image Back;
                const bool Saved = SaveRgba8(Dir / Name, Pixels, Error);
                Check(Saved && LoadRgba8(Dir / Name, Back, Error) && Back.Pixels == Pixels.Pixels, (std::string("wide image: ") + Name + " saved and read back").c_str(), Error);
            }
            std::string Error;
            std::string Hash0, Hash1;
            Out::Sha256File(Dir / "wide.png", Hash0);
            Rgba8Image Other = Pixels;
            Other.Pixels = { 90, 80, 70, 255 };
            Check(!SaveRgba8(Dir / "wide.png", Other, Error) && Error == "exists", "image save: an existing file is not replaced", Error);
            Out::Sha256File(Dir / "wide.png", Hash1);
            size_t Files = 0;
            for (const auto& E : fs::directory_iterator(Dir, Ec)) { (void)E; ++Files; }
            Check(Hash0 == Hash1 && Files == 3, "image save: the existing file is as it was and no temp is left", std::to_string(Files));
        }

        // 17. ChooseRoot: --target modtools takes its root from the command line only
        {
            const fs::path Scan = Base / "t17" / "scan", Named = Base / "t17" / "named";
            std::error_code E;
            fs::create_directories(Scan, E);
            fs::create_directories(Named, E);
            fs::path Root;
            std::string How, Error;
            Check(!ChooseRoot(Target::ModTools, "", Scan.string(), Scan.string(), Root, How, Error) && Contains(Error, "--mod-tools-root") && Root.empty(),
                  "modtools root: refused without --mod-tools-root, whatever the scan or TA_TOOLS_PATH say", Error);
            Check(ChooseRoot(Target::ModTools, Named.string(), Scan.string(), "", Root, How, Error) && Root == fs::absolute(Named, E), "modtools root: the named folder", Root.string());
            Check(!ChooseRoot(Target::ModTools, (Base / "t17" / "missing").string(), "", "", Root, How, Error) && Contains(Error, "not a folder"), "modtools root: a named non-folder is refused", Error);
            Root.clear();
            Check(ChooseRoot(Target::Folder, "", Scan.string(), "", Root, How, Error) && Root == fs::absolute(Scan, E), "folder root: the scan still serves the read-only lookups", Root.string());
            Root.clear();
            Check(ChooseRoot(Target::Folder, "", "", "", Root, How, Error) && Root.empty(), "folder root: none found is not an error", Error);
        }
        fs::remove_all(Base, Ec);
        return Failed;
    }
}
