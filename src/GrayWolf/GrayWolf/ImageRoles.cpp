#include "stdafx.h"

// See ImageRoles.h.

#include "ImageRoles.h"

#include "Bo3Target.MaterialImages.h"
#include "Bo3Target.Techsets.h"
#include "ExportNaming.h"
#include "ExportWriters.h"
#include "MemoryBudget.h"
#include "SettingsManager.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // The slots of the BO3 techset definitions (share/raw/techsetdefs_stable, Texture("...")) that
    // take a normal map. Black Ops 4 and Cold War keep the same hashes ("one name means one slot in
    // each", TreyarchMaterials.h).
    //
    // Are the detail slots really normals? Mostly, and not always. Evidence for: the stock GDT census in
    // the research note bo3-stock-naming.md (local, not in the repository) counts the images bound to the material field `detailMap`
    // as semantic normalMap 7,901 times (measured), and tools/bo3target/image_oracle.py maps the stock
    // key detailMap to the role "normal" ("detailMap holds the detail normals"); the names detailNormal1-4
    // say it themselves. Evidence against: the same notes list lit_micro_tile_blend's colorMap01 as a
    // detail COLOUR, and BO4's detailMap2 lands on BO3's colorMap00, a second detail normal in one
    // family and not shown to be one in the others. So a detail slot is a normal by census, not by rule.
    // That is why every target, not only an image bound to both kinds of slot, passes LooksLikeNormal on
    // its pixels before its blue is rewritten.
    const std::set<uint32_t>& NormalSlots()
    {
        static const std::set<uint32_t> Slots = [] {
            std::set<uint32_t> S;
            for (const char* Name : { "normalMap", "normalMap2", "normalMap3", "normalMap4", "normalMapHi", "transNormalMap",
                                      "characterNormalMap", "bentNormalMap", "crackNormalMap", "meltNormalMap", "metalFlakeNormalMap",
                                      "normalBodyMap", "normalSwatch1Map", "normalSwatch2Map", "normalTexture", "transitionNormal",
                                      "detailNormal1", "detailNormal2", "detailNormal3", "detailNormal4",
                                      "detailMap", "detailMap1", "detailMap2", "detailMap3" })
                S.insert(Bo3Target::Techsets::SlotHash(Name));
            return S;
        }();
        return Slots;
    }

    std::string Lower(const std::string& S)
    {
        return ExportNaming::Lower(S);
    }

    // Core's NormalZ (Image.cpp), as the byte DirectXTex stores for the float
    uint8_t ZByte(uint8_t R, uint8_t G)
    {
        const float X = 2.0f * (R / 255.0f) - 1.0f;
        const float Y = 2.0f * (G / 255.0f) - 1.0f;
        const float Q = 1.0f - X * X - Y * Y;
        const float Z = Q > 0.0f ? std::sqrt(Q) : 0.0f;
        const float Channel = std::clamp((Z + 1.0f) / 2.0f, 0.0f, 1.0f);
        return (uint8_t)(Channel * 255.0f + 0.5f);
    }

    // Memory the loads in flight may hold: a task costs about four copies of its pixels (the decoded
    // scratch, the converted RGBA, the buffer, the encoder's scratch), and the worker count alone does not
    // bound that for 4096x4096 and larger. A task waits until its cost fits the budget; when nothing is in
    // flight one always proceeds, so an image over the budget runs alone.
    class Gate
    {
    public:
        explicit Gate(uint64_t BudgetBytes) : Budget(BudgetBytes) {}
        void Acquire(uint64_t Cost)
        {
            std::unique_lock<std::mutex> L(M);
            Cv.wait(L, [&] { return Used == 0 || Used + Cost <= Budget; });
            Used += Cost;
        }
        void Release(uint64_t Cost)
        {
            {
                std::lock_guard<std::mutex> L(M);
                Used -= Cost;
            }
            Cv.notify_all();
        }

    private:
        std::mutex M;
        std::condition_variable Cv;
        uint64_t Budget, Used = 0;
    };
}

namespace ImageRoles
{
    bool IsNormalSlot(uint32_t Semantic)
    {
        return NormalSlots().count(Semantic) != 0;
    }

    void Book::Note(const std::string& Image, uint32_t Semantic)
    {
        std::string Name = Image;
        if (!Name.empty() && Name[0] == ',') Name.erase(0, 1);
        if (Name.empty()) return;
        ByName[Lower(Name)].insert(Semantic);
    }

    void Book::Add(const DiskGame::MaterialBook& Materials)
    {
        for (const auto& [Material, Images] : Materials)
            for (const auto& I : Images) Note(I.Image, I.Hash);
    }

    Book::Role Book::RoleOf(const std::string& Image) const
    {
        const auto It = ByName.find(Lower(Image));
        if (It == ByName.end()) return Role::Unknown;
        const bool Normal = std::any_of(It->second.begin(), It->second.end(), IsNormalSlot);
        const bool Other = std::any_of(It->second.begin(), It->second.end(), [](uint32_t S) { return !IsNormalSlot(S); });
        return Normal && Other ? Role::Conflict : Normal ? Role::Normal : Role::Other;
    }

    std::vector<std::string> Book::Normals() const
    {
        std::vector<std::string> Out;
        for (const auto& [Name, Slots] : ByName)
            if (RoleOf(Name) == Role::Normal) Out.push_back(Name);
        return Out;
    }

    size_t Book::Conflicts() const
    {
        return (size_t)std::count_if(ByName.begin(), ByName.end(), [this](const auto& E) { return RoleOf(E.first) == Role::Conflict; });
    }

    bool RebuildZ(Rgba8Image& Image)
    {
        if (!Image.IsValid()) return false;
        const size_t Count = (size_t)Image.Width * Image.Height;
        size_t Fit = 0;
        for (size_t I = 0; I < Count; I++)
        {
            const uint8_t* P = &Image.Pixels[I * 4];
            if (std::abs((int)P[2] - (int)ZByte(P[0], P[1])) <= 12) Fit++;
        }
        if ((double)Fit >= 0.97 * (double)Count) return false;
        for (size_t I = 0; I < Count; I++)
        {
            uint8_t* P = &Image.Pixels[I * 4];
            P[2] = ZByte(P[0], P[1]);
        }
        return true;
    }

    bool LooksLikeNormal(const Rgba8Image& Image, bool Strict)
    {
        if (!Image.IsValid()) return false;
        const size_t Count = (size_t)Image.Width * Image.Height;
        double SumR = 0, SumG = 0, SumRR = 0, SumGG = 0;
        size_t Inside = 0;
        for (size_t I = 0; I < Count; I++)
        {
            const double R = Image.Pixels[I * 4] / 255.0, G = Image.Pixels[I * 4 + 1] / 255.0;
            SumR += R, SumG += G, SumRR += R * R, SumGG += G * G;
            const double X = 2 * R - 1, Y = 2 * G - 1;
            if (X * X + Y * Y <= 1.05) Inside++;
        }
        const double MeanR = SumR / Count, MeanG = SumG / Count;
        const double DevR = std::sqrt(std::max(0.0, SumRR / Count - MeanR * MeanR)), DevG = std::sqrt(std::max(0.0, SumGG / Count - MeanG * MeanG));
        // Strict (an image bound to both kinds of slot): centred, spread under 0.35, 97% in the disc. Not strict (the
        // slot says normal; the pixels only have to be a normal's and not colour, noise or a stub): a strongly tilted map
        // is off centre (measured: a teeth normal at mean red 0.63, a vent at mean green 0.27), so means within 0.25, and
        // 90% in the disc. The proxy stubs of the Black Ops 4 props (8x32 and 64x64 'proxy_normal': 0% to 77% in the
        // disc) fail it.
        const double Centre = Strict ? 0.12 : 0.25, Spread = Strict ? 0.35 : 0.40, Disc = Strict ? 0.97 : 0.90;
        return std::abs(MeanR - 0.5) < Centre && std::abs(MeanG - 0.5) < Centre && DevR < Spread && DevG < Spread && (double)Inside >= Disc * Count;
    }

    Outcome RebuildNormals(const Book& Roles, const fs::path& ImagesDir, DiskGame::Totals& T)
    {
        if (SettingsManager::GetSetting("patchnormals", "true") != "true")
        {
            Outcome Off;
            Off.Roles = Roles.Images();
            Off.Conflict = Roles.Conflicts();
            return Off;
        }
        return RebuildNormalsIn(Roles, ImagesDir, ExportWriters::ImageFormatSetting(), T);
    }

    Outcome RebuildNormalsIn(const Book& Roles, const fs::path& ImagesDir, const std::string& Format, DiskGame::Totals& T)
    {
        Outcome Out;
        Out.Roles = Roles.Images();
        Out.Conflict = Roles.Conflicts();
        std::error_code Ec;
        if (Roles.Images() == 0 || !fs::is_directory(ImagesDir, Ec)) return Out;
        const std::string Ext = ExportWriters::ImageExtension(Format);

        // what the folder holds: lower case stem -> file. Only a file this run wrote (ExportWriters::ImageWritten)
        // may be rewritten: the others (an earlier run, a --limit run's neighbours, a repainted normal, another
        // game's file) are never touched.
        std::map<std::string, fs::path> Files;
        for (const auto& E : fs::directory_iterator(ImagesDir, Ec))
        {
            if (!E.is_regular_file(Ec) || Lower(E.path().extension().string()) != Ext) continue;
            Files.emplace(Lower(E.path().stem().string()), E.path());
        }
        // Book names by the file stem they are written under: SafeFileName is many-to-one ("a/b" and "a_b"), so a
        // stem several names share has no one role
        std::map<std::string, std::vector<std::string>> NamesOfStem;
        for (const auto& [Name, Slots] : Roles.All()) NamesOfStem[Lower(ExportNaming::SafeFileName(Name))].push_back(Name);
        // GW_ROLE_DUMP=<file>: every image the materials bind, its slots' semantic hashes, its role, and whether its file is here
        char DumpPath[MAX_PATH] = {};
        if (GetEnvironmentVariableA("GW_ROLE_DUMP", DumpPath, sizeof(DumpPath)) != 0)
        {
            const char Tab = '\t', Nl = '\n';
            std::string Dump = std::string("image") + Tab + "role" + Tab + "semantics" + Tab + "file" + Nl;
            for (const auto& [Name, Slots] : Roles.All())
            {
                std::string Hashes;
                for (const uint32_t S : Slots) Hashes += (Hashes.empty() ? "" : ",") + std::format("{:08X}", S);
                const char* Role = Roles.RoleOf(Name) == Book::Role::Normal ? "normal" : Roles.RoleOf(Name) == Book::Role::Conflict ? "conflict" : "other";
                Dump += Name + Tab + Role + Tab + Hashes + Tab + (Files.count(Lower(ExportNaming::SafeFileName(Name))) ? "1" : "0") + Nl;
            }
            for (const auto& [Stem, Path] : Files)
                if (!NamesOfStem.count(Stem)) Dump += Path.stem().string() + Tab + "unknown" + Tab + Tab + "1" + Nl;
            ExportWriters::WriteBytes(fs::path(DumpPath), (const uint8_t*)Dump.data(), Dump.size());
        }
        for (const auto& [Stem, Path] : Files)
            if (!NamesOfStem.count(Stem)) Out.Unknown++;

        struct Target
        {
            fs::path File;
            bool Tie;   // bound to both kinds of slot
        };
        std::vector<Target> Targets;   // one per file: NamesOfStem is keyed by the stem
        for (const auto& [Stem, Names] : NamesOfStem)
        {
            const auto It = Files.find(Stem);
            if (Names.size() > 1)
            {
                if (It != Files.end()) Out.Collision++;   // one file, several images: whose role is it? unknown, left as it is
                continue;
            }
            const Book::Role Role = Roles.RoleOf(Names.front());
            if (Role != Book::Role::Normal && Role != Book::Role::Conflict) continue;
            if (It == Files.end())
                Out.NotInFolder++;
            else if (!ExportWriters::ImageWritten(It->second))
                Out.NotThisRun++;
            else
                Targets.push_back({ It->second, Role == Book::Role::Conflict });
        }
        if (Format == "DDS")
        {
            Out.LeftDds = Targets.size();   // the game's own BC5: two components, nothing to add the third to
        }
        else
        {
            std::atomic<size_t> Rebuilt{ 0 }, AlreadyZ{ 0 }, Failed{ 0 }, ConflictRebuilt{ 0 }, ConflictLeft{ 0 }, NotNormal{ 0 }, Layout{ 0 };
            Gate Memory(MemoryBudget::For(0.5, 0));
            {
                ExportWriters::WorkerPool Pool;
                for (const Target& Each : Targets)
                    Pool.Post([&Memory, &Rebuilt, &AlreadyZ, &Failed, &ConflictRebuilt, &ConflictLeft, &NotNormal, &Layout, File = Each.File, Tie = Each.Tie] {
                        size_t Channels = 0, Bits = 0, Width = 0, Height = 0;
                        // a one-channel or 16-bit file is never widened to RGBA8 and written back
                        if (!Bo3Target::ImageFileLayout(File, Channels, Bits, Width, Height))
                        {
                            Failed++;
                            return;
                        }
                        if (Bits != 8 || (Channels != 3 && Channels != 4))
                        {
                            Layout++;
                            return;
                        }
                        const uint64_t Cost = 16ull * Width * Height;
                        Memory.Acquire(Cost);
                        Rgba8Image Image;
                        std::string Error;
                        if (!Bo3Target::LoadRgba8(File, Image, Error))
                            Failed++;
                        else if (!LooksLikeNormal(Image, Tie))
                            (Tie ? ConflictLeft : NotNormal)++;
                        else if (!RebuildZ(Image))
                            AlreadyZ++;
                        else if (Bo3Target::ReplaceRgba8(File, Image, Error))
                            (Tie ? ConflictRebuilt : Rebuilt)++;
                        else
                            Failed++;
                        Memory.Release(Cost);
                    });
                Pool.Drain();
            }
            Out.Rebuilt = Rebuilt;
            Out.AlreadyZ = AlreadyZ;
            Out.Failed = Failed;
            Out.ConflictRebuilt = ConflictRebuilt;
            Out.ConflictLeft = ConflictLeft;
            Out.NotNormal = NotNormal;
            Out.SkippedLayout = Layout;
        }
        T.Counters["normals_z_rebuilt"] += Out.Rebuilt;
        T.Counters["normals_z_already"] += Out.AlreadyZ;
        T.Counters["normals_z_failed"] += Out.Failed;
        T.Counters["normals_left_dds"] += Out.LeftDds;
        T.Counters["normals_role_conflict"] += Out.Conflict;
        T.Counters["normals_conflict_rebuilt"] += Out.ConflictRebuilt;
        T.Counters["normals_conflict_left"] += Out.ConflictLeft;
        T.Counters["normals_pixels_not_normal"] += Out.NotNormal;
        T.Counters["normals_skipped_layout"] += Out.SkippedLayout;
        T.Counters["normals_not_this_run"] += Out.NotThisRun;
        T.Counters["normals_name_collision"] += Out.Collision;
        T.Counters["images_role_unknown"] += Out.Unknown;
        T.Counters["image_roles_known"] += Out.Roles;
        T.Counters["normals_not_in_folder"] += Out.NotInFolder;
        if (Out.Failed) T.Fail("", DiskGame::Kind::Image, "", std::to_string(Out.Failed) + " normal maps whose Z was not rebuilt");
        return Out;
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Condition, const char* Name, const std::string& Got) {
            if (Condition) return;
            ++Failed;
            Failures += std::format("FAIL ImageRoles {} (got '{}')\n", Name, Got);
        };

        // the slots: by semantic, never by an image's name
        Check(IsNormalSlot(0x59D30D0Fu) && IsNormalSlot(0xAB07D475u), "slots: normalMap and transNormalMap are normals", "");
        Check(!IsNormalSlot(0xA0AB1041u) && !IsNormalSlot(0x6D0A6C98u) && !IsNormalSlot(0x07176BF2u) && !IsNormalSlot(0xEC443804u),
              "slots: colour, gloss, occlusion and specular are not", "");

        // the Book: a name does not decide; one image under two kinds of slot is a conflict
        {
            Book B;
            DiskGame::MaterialBook M;
            M["m1"] = { { 2, 0x59D30D0Fu, "detail_noise" }, { 1, 0xA0AB1041u, "mtl_x_c" } };
            M["m2"] = { { 1, 0xA0AB1041u, "mtl_y_n" }, { 1, 0xA0AB1041u, "Both" }, { 2, 0x59D30D0Fu, "both" }, { 2, 0x59D30D0Fu, "$identitynormalmap" } };
            B.Add(M);
            Check(B.RoleOf("detail_noise") == Book::Role::Normal, "book: detail_noise is a normal by its slot", "");
            Check(B.RoleOf("mtl_y_n") == Book::Role::Other, "book: a name ending _n in a colour slot is not a normal", "");
            Check(B.RoleOf("MTL_X_C") == Book::Role::Other, "book: lookups ignore case", "");
            Check(B.RoleOf("both") == Book::Role::Conflict && B.Conflicts() == 1, "book: two kinds of slot is a conflict", "");
            Check(B.RoleOf("never_seen") == Book::Role::Unknown, "book: an unbound image is unknown", "");
            Check(B.RoleOf("$identitynormalmap") == Book::Role::Normal, "book: an engine image is a normal by its slot too", "");
            Check(B.Normals() == std::vector<std::string>({ "$identitynormalmap", "detail_noise" }), "book: the normals", std::to_string(B.Normals().size()));
        }

        // the rebuild: blue is the Z of red and green, alpha stays, a true XYZ normal is left alone
        {
            Rgba8Image I;
            I.Width = 4;
            I.Height = 1;
            // flat (128,128), tilted (192,128), at the rim (255,255), a corner of the square (0,0); blue holds a variance term, alpha 7
            I.Pixels = { 128, 128, 9, 7,  192, 128, 3, 7,  255, 255, 0, 7,  0, 0, 1, 7 };
            const bool Changed = RebuildZ(I);
            Check(Changed, "rebuild: a normal without Z is rebuilt", "");
            Check(std::abs((int)I.Pixels[2] - 255) <= 1, "rebuild: flat points straight out", std::to_string(I.Pixels[2]));
            const float X = 2.0f * (192 / 255.0f) - 1.0f;
            const int Tilt = (int)std::lround((std::sqrt(1.0f - X * X) + 1.0f) / 2.0f * 255.0f);
            Check(std::abs((int)I.Pixels[6] - Tilt) <= 1, "rebuild: a tilted pixel", std::to_string(I.Pixels[6]));
            Check(std::abs((int)I.Pixels[10] - 128) <= 1 && std::abs((int)I.Pixels[14] - 128) <= 1,
                  "rebuild: Z = 0 where X and Y reach unit length is stored 0.5", std::format("{} {}", I.Pixels[10], I.Pixels[14]));
            Check(I.Pixels[3] == 7 && I.Pixels[7] == 7 && I.Pixels[0] == 128 && I.Pixels[4] == 192 && I.Pixels[9] == 255,
                  "rebuild: red, green and alpha are untouched", "");
            const std::vector<uint8_t> Before = I.Pixels;
            Check(!RebuildZ(I) && I.Pixels == Before, "rebuild: a normal that has its Z is left alone (idempotent)", "");
            Rgba8Image Tilted;
            Tilted.Width = 4;
            Tilted.Height = 1;
            Tilted.Pixels = { 128, 128, 0, 255,  192, 128, 0, 255,  100, 150, 0, 255,  128, 60, 0, 255 };
            Check(LooksLikeNormal(Tilted), "tie: flat and tilted pixels look like a normal", "");
            Check(!LooksLikeNormal(I), "tie: pixels outside the unit disc do not", "");
            Rgba8Image Colour;
            Colour.Width = 2;
            Colour.Height = 1;
            Colour.Pixels = { 250, 20, 30, 255,  240, 10, 20, 255 };
            Check(!LooksLikeNormal(Colour), "tie: a saturated colour does not look like a normal", "");
            // a slot that says normal: a strongly tilted map (mean red 0.63) passes, a stub of dark noise does not
            Rgba8Image Tilted2;
            Tilted2.Width = 4;
            Tilted2.Height = 1;
            Tilted2.Pixels = { 160, 95, 0, 255,  165, 100, 0, 255,  158, 92, 0, 255,  170, 105, 0, 255 };
            Rgba8Image Stub;
            Stub.Width = 4;
            Stub.Height = 1;
            Stub.Pixels = { 4, 4, 4, 255,  3, 5, 4, 255,  4, 3, 5, 255,  5, 4, 3, 255 };
            Check(!LooksLikeNormal(Tilted2) && LooksLikeNormal(Tilted2, false), "slot: a tilted normal passes the slot check, not the tie check", "");
            Check(!LooksLikeNormal(Stub, false) && !LooksLikeNormal(Colour, false), "slot: a dark stub and a colour do not", "");
        }
        // the pass over the files: only what this run wrote, one role per file, 8-bit RGB(A) only, normal-looking pixels only
        {
            const fs::path Dir = fs::temp_directory_path() / std::format("gw_roles_selftest_{}_{}", GetCurrentProcessId(), GetTickCount64());
            std::error_code Ec;
            fs::create_directories(Dir, Ec);
            const auto Normal = [](uint8_t Blue) {
                Rgba8Image I;
                I.Width = 4;
                I.Height = 1;
                I.Pixels = { 128, 128, Blue, 255,  192, 128, Blue, 255,  100, 150, Blue, 255,  128, 60, Blue, 255 };
                return I;
            };
            const auto Save = [&](const std::string& Stem, const Rgba8Image& I, bool ThisRun) {
                std::string Error;
                const fs::path File = Dir / (Stem + ".tga");
                Bo3Target::SaveRgba8(File, I, Error);
                if (ThisRun) ExportWriters::NoteImageWritten(File);
                return File;
            };
            const auto Bytes = [](const fs::path& File) {
                std::vector<uint8_t> B;
                ExportWriters::ReadBytes(File, B);
                return B;
            };
            Rgba8Image Colour;
            Colour.Width = 2;
            Colour.Height = 1;
            Colour.Pixels = { 250, 20, 30, 255,  240, 10, 20, 255 };
            const fs::path Written = Save("n_written", Normal(9), true);
            const fs::path Old = Save("n_old", Normal(9), false);       // a normal no writer of this run wrote
            const fs::path Pair = Save("a_b", Normal(9), true);         // "a/b" and "a_b" are written to this one file
            const fs::path Paint = Save("n_paint", Colour, true);       // bound to a normal slot, pixels of a colour
            // a one-channel TGA, by hand: type 3, 8 bits, top left origin, 4x1
            const fs::path Gray = Dir / "n_gray.tga";
            {
                const std::vector<uint8_t> Tga = { 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 1, 0, 8, 0x20, 10, 20, 30, 40 };
                ExportWriters::WriteBytes(Gray, Tga.data(), Tga.size());
                ExportWriters::NoteImageWritten(Gray);
            }
            const auto BeforeOld = Bytes(Old), BeforeWritten = Bytes(Written), BeforePair = Bytes(Pair), BeforePaint = Bytes(Paint), BeforeGray = Bytes(Gray);
            Book B;
            for (const char* Name : { "n_written", "n_old", "a/b", "a_b", "n_paint", "n_gray" }) B.Note(Name, 0x59D30D0Fu);
            Check(ExportNaming::SafeFileName("a/b") == ExportNaming::SafeFileName("a_b"), "files: the collision case collides", ExportNaming::SafeFileName("a/b"));
            DiskGame::Totals T;
            const Outcome O = RebuildNormalsIn(B, Dir, "TGA", T);
            Check(Bytes(Written) != BeforeWritten && O.Rebuilt == 1, "files: a normal this run wrote is rebuilt", std::to_string(O.Rebuilt));
            Check(Bytes(Old) == BeforeOld && O.NotThisRun == 1 && T.Counters["normals_not_this_run"] == 1,
                  "files: a file not written this run is untouched and counted", std::to_string(O.NotThisRun));
            Check(Bytes(Pair) == BeforePair && O.Collision == 1, "files: two names, one file: left alone, counted", std::to_string(O.Collision));
            Check(Bytes(Gray) == BeforeGray && O.SkippedLayout == 1, "files: a one-channel image is skipped", std::to_string(O.SkippedLayout));
            Check(Bytes(Paint) == BeforePaint && O.NotNormal == 1, "files: a normal slot with colour pixels is left", std::to_string(O.NotNormal));
            // nothing else is left in the folder: no temp
            size_t Count = 0;
            for (const auto& E : fs::directory_iterator(Dir, Ec)) Count++, (void)E;
            Check(Count == 5, "files: no temp file is left behind", std::to_string(Count));
            Check(O.Failed == 0, "files: no failure", std::to_string(O.Failed));
            fs::remove_all(Dir, Ec);
        }
        return Failed;
    }
}
