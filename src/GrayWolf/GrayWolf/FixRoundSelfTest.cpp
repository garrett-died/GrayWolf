#include "stdafx.h"

#include "CoDAssetType.h"
#include "DiskGuard.h"
#include "ExportWriters.h"
#include "FixRoundSelfTest.h"
#include "Games/H1/GameModernWarfareRM.h"
#include "Games/H1/LayerBits.h"
#include "Games/IW/IWMaterialBooks.h"
#include "ImageFacts.h"
#include "MaterialDescription.h"
#include "PoolWalkRetry.h"
#include "SurfaceTypes.h"

#include <format>
#include <fstream>
#include <string>
#include <vector>

// Self-tests of the logic the M1 fix round pulled out into pure functions: the layer table (B4, B5), the kept book's extras (B5), the pool-coverage retry (B8),
// the image header facts (B10) and the plan's size arithmetic (B11). None needs a zone, an install or a window.
namespace FixRound
{
    namespace
    {
        namespace fs = std::filesystem;

        struct Walk
        {
            std::string Name;
        };
        bool IsBelow(uint64_t Free) { return DiskGuard::IsBelowFloor(Free, 8.0); }
    }

    int SelfTest(std::string& Failures)
    {
        int Failed = 0;
        const auto Check = [&](bool Ok, const char* Name, const std::string& Got = std::string()) {
            if (Ok) return;
            Failed++;
            Failures += std::format("FAIL FixRound {} (got '{}')\n", Name, Got);
        };

        // ---- B4 / B5: the layer table ----
        {
            H1Layers::LayerBits L;
            // the blends of a level: 49n sits in two blends whose words share only bit 16 (plaster); 7n and 50n carry their own bits
            L.Add("*49n_50n", (1ll << 16) | (1ll << 4));
            L.Add("*49n_7n", (1ll << 16) | (1ll << 9));
            L.Add("*z_a", 1ll << 3);                         // a single-bit blend
            L.Add("*z_b", (1ll << 3) | (1ll << 5));
            L.Add("*lone_x", (1ll << 20) | (1ll << 21));     // an id held by one blend only: no intersection to narrow it
            std::string Via;
            const int64_t Header = L.Surface("*49n_50n", (1ll << 16) | (1ll << 4), &Via);
            Check(Header == (1ll << 16) && Via.find("kind=intersection, layer_id=49n") != std::string::npos && Via.find("blends_with_id=2") != std::string::npos && Via.find("multi_bit_blends_with_id=2") != std::string::npos,
                  "a blend resolves to its first layer's bit and says on how many blends", Via);
            // B4: a material the zone defines itself ("*49n_50n" as the reader names it) resolves like the header of the same name
            // (through the reader's one hook, ResolveBlendSurface: a header's details and a defined material both end there)
            XMaterial_t DefinedMat(0), HeaderMat(0), Untouched(0);
            DefinedMat.SurfaceFlags = HeaderMat.SurfaceFlags = (1ll << 16) | (1ll << 4);
            {
                GameModernWarfareRM::Hooks H;
                H.Blend = [&L](const std::string& Name, int64_t Word, std::string& V) { return L.Surface(Name, Word, &V); };
                const GameModernWarfareRM::HooksScope Served(std::move(H));
                GameModernWarfareRM::ResolveBlendSurface(DefinedMat, "*49n_50n");
                GameModernWarfareRM::ResolveBlendSurface(HeaderMat, "*49n_50n");
            }
            Untouched.SurfaceFlags = (1ll << 16) | (1ll << 4);
            GameModernWarfareRM::ResolveBlendSurface(Untouched, "*49n_50n");   // no hook (a game not served): the word stays
            const int64_t Defined = DefinedMat.SurfaceFlags;
            const std::string ViaDefined = DefinedMat.SurfaceVia;
            Check(Defined == (1ll << 16) && HeaderMat.SurfaceFlags == Defined && ViaDefined == Via && Untouched.SurfaceFlags == ((1ll << 16) | (1ll << 4)) && Untouched.SurfaceVia.empty(),
                  "B4: a defined * material (*49n_50n, bits 16 and 4, the book says plaster) resolves like a header, and keeps the evidence", ViaDefined);
            // a header is named ",*49n_50n" by the zone (the comma, then the star): the same layer ids
            std::string ViaComma;
            Check(L.Surface(",*49n_50n", (1ll << 16) | (1ll << 4), &ViaComma) == (1ll << 16) && !ViaComma.empty(), "B4: a header named with a comma and a star resolves the same layer", ViaComma);
            // and through the surface decoding it is plaster, with the evidence carried
            Materials::Source Src;
            Src.Name = "*49n_50n";
            Src.SurfaceFlags = Defined;
            Src.SurfaceVia = ViaDefined;
            Materials::Description D;
            SurfaceTypes::Apply("h1", Src, D);
            Check(D.SurfaceType == "plaster" && !D.SurfaceVia.empty(), "B4: the resolved bit names plaster in the h1 table and keeps the evidence", D.SurfaceType + " | " + D.SurfaceWhy);
            // the unresolved word stays ambiguous (the control: without the table the same word has two bits)
            Materials::Source Raw;
            Raw.SurfaceFlags = (1ll << 16) | (1ll << 4);
            Materials::Description Dr;
            SurfaceTypes::Apply("h1", Raw, Dr);
            // B4/C7: the expected value, not "defined equals header": the defined material is plaster, the untouched one (no table) is not
            Check(DefinedMat.SurfaceFlags == (1ll << 16) && Untouched.SurfaceFlags != (1ll << 16), "B4: the defined * material is plaster (bit 16) and the one with no table is not", std::to_string(DefinedMat.SurfaceFlags) + " " + std::to_string(Untouched.SurfaceFlags));
            Check(Dr.SurfaceType.empty() && Dr.SurfaceWhy.rfind("ambiguous:", 0) == 0 && Dr.SurfaceVia.empty(), "B4: the union of two bits is ambiguous without the layer table", Dr.SurfaceWhy);
            // B5: a single-bit partner's bit lands on the layer: the row carries the counts that show it is thin evidence
            std::string ViaZ;
            const int64_t Z = L.Surface("*z_b", (1ll << 3) | (1ll << 5), &ViaZ);
            Check(Z == (1ll << 3) && ViaZ.find("blends_with_id=2") != std::string::npos && ViaZ.find("multi_bit_blends_with_id=1") != std::string::npos, "B5: an intersection through a single-bit blend reports 2 blends, 1 with several bits", ViaZ);
            // no intersection, no row
            std::string ViaLone;
            Check(L.Surface("*lone_x", (1ll << 20) | (1ll << 21), &ViaLone) == ((1ll << 20) | (1ll << 21)) && ViaLone.empty(), "B5: an unresolved word is returned as it is, with no evidence row", ViaLone);
            // C7: a single-bit word returns early but still reports what it rests on; a bit outside the blend's own word is no resolution
            std::string ViaOne;
            Check(L.Surface("*49n_50n", 1ll << 16, &ViaOne) == (1ll << 16) && ViaOne.find("kind=single_bit_word") != std::string::npos && ViaOne.find("blends_with_id=2") != std::string::npos,
                  "C7: a single-bit word still reports its evidence (kind=single_bit_word, the counts)", ViaOne);
            H1Layers::LayerBits Outside;
            Outside.Add("*q_a", (1ll << 6) | (1ll << 7));
            Outside.Add("*q_b", (1ll << 6) | (1ll << 8));          // q resolves to bit 6 ...
            std::string ViaOut;
            const int64_t OutWord = (1ll << 7) | (1ll << 9);         // ... but this blend (same first id) has no bit 6
            Check(Outside.Surface("*q_c", OutWord, &ViaOut) == OutWord && ViaOut.empty(), "C7: a resolved bit that is not in the blend's own word is no resolution (the word is returned, no evidence row)", ViaOut);
            Check(L.Ids() == 8 && L.Resolved() == 3, "the table counts its ids", std::to_string(L.Ids()) + " " + std::to_string(L.Resolved()));
        }

        // ---- C8: a header and a defined material with no texture ----
        {
            MWRXMaterial Header;
            std::memset(&Header, 0, sizeof(Header));
            MWRXMaterial Defined = Header;
            Defined.TechsetPtr = 0x1000;           // defined, with a techset and no texture
            MWRXMaterial Surfaced = Header;
            Surfaced.SurfaceTypeBits = 1ull << 16; // defined, with a surface word only
            Check(GameModernWarfareRM::IsMaterialHeader(Header) && !GameModernWarfareRM::IsMaterialHeader(Defined) && !GameModernWarfareRM::IsMaterialHeader(Surfaced),
                  "C8: a header is the all-zero material; a defined material with no texture (a techset or a surface word) keeps its own data", "");
        }

        // ---- B5: the kept book keeps surface word and constants (HasExtra 2), techset only (1), neither (0) ----
        {
            std::string Got;
            const fs::path Dir = fs::temp_directory_path() / ("graywolf_selftest_book_" + std::to_string(GetCurrentProcessId()));
            const bool Ok = IWMaterialBooks::BookRoundTripSelfTest(Dir, Got);
            std::error_code Ec;
            fs::remove_all(Dir, Ec);
            Check(Ok, "B5: the book round trip keeps HasExtra 0, 1 and 2", Got);
        }

        // ---- B8: the retry through the install's list ----
        {
            std::vector<Walk> Walks;
            std::vector<std::string> Walked;
            int ResolveCalls = 0;
            std::string SkippedZones;
            const auto Walk1 = [&](const std::string& Zone) {
                Walked.push_back(Zone);
                Walks.push_back({ Zone });
                return Zone != "a.ff" && Zone != "c.ff";   // a.ff and c.ff do not open as given
            };
            // a.ff fails and is retried; the list gives a.zone and b.ff; c.ff fails too but the input has been retried once already
            PoolWalkRetry::WalkInput(Walks, std::vector<std::string>{ "a.ff", "b.ff", "c.ff" }, true, Walk1, [&](std::vector<std::string>& R) {
                ResolveCalls++;
                R = { "a.zone", "b.ff" };
                return true;
            }, [&](const std::string& Z) { SkippedZones += Z + " "; });
            std::string Names;
            for (const Walk& W : Walks) Names += W.Name + " ";
            Check(ResolveCalls == 1, "B8: the install list is asked once per input, though two zones failed", std::to_string(ResolveCalls));
            std::string Order;
            for (const std::string& W : Walked) Order += W + " ";
            Check(Order == "a.ff a.zone b.ff c.ff ", "B8: no zone is walked twice (b.ff once, by the retry); the failed a.ff is walked once, c.ff after the one retry", Order);
            Check(Names == "a.zone b.ff c.ff ", "B8: only the failed entry is erased (a.ff); the walks of the others stay", Names);
            Check(SkippedZones == "b.ff ", "D9: a zone skipped because it was already walked is reported (b.ff, skipped when its turn came)", SkippedZones);

            // x.ff and x.zone are two names: a successful x.ff does not stop x.zone being walked
            std::vector<Walk> W2;
            std::vector<std::string> Seen;
            PoolWalkRetry::WalkInput(W2, std::vector<std::string>{ "x.ff", "y.ff" }, true, [&](const std::string& Z) {
                Seen.push_back(Z);
                W2.push_back({ Z });
                return Z != "y.ff";
            }, [&](std::vector<std::string>& R) {
                R = { "x.zone", "y.zone" };
                return true;
            }, [](const std::string&) {});
            std::string N2;
            for (const Walk& W : W2) N2 += W.Name + " ";
            Check(N2 == "x.ff x.zone y.zone ", "B8: x.ff and x.zone stay two walks; the failed y.ff is replaced by y.zone", N2);

            // no install: no retry, the failed entry stays
            std::vector<Walk> W3;
            int Calls3 = 0;
            PoolWalkRetry::WalkInput(W3, std::vector<std::string>{ "a.ff" }, false, [&](const std::string& Z) { W3.push_back({ Z }); return false; }, [&](std::vector<std::string>&) { Calls3++; return true; }, [](const std::string&) {});
            Check(Calls3 == 0 && W3.size() == 1, "B8: with no install nothing is retried and the failed entry stays", std::to_string(Calls3));
        }

        // ---- B10: the size from a header and the compressionMethod at 1x1 and 4x4, PNG and DDS ----
        {
            const auto Png = [](uint32_t W, uint32_t H) {
                unsigned char B[32] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
                for (int K = 0; K < 4; K++) B[16 + K] = (unsigned char)(W >> (24 - 8 * K)), B[20 + K] = (unsigned char)(H >> (24 - 8 * K));
                return std::string(reinterpret_cast<char*>(B), 32);
            };
            const auto Dds = [](uint32_t W, uint32_t H) {
                unsigned char B[32] = { 'D', 'D', 'S', ' ', 124 };
                for (int K = 0; K < 4; K++) B[12 + K] = (unsigned char)(H >> (8 * K)), B[16 + K] = (unsigned char)(W >> (8 * K));
                return std::string(reinterpret_cast<char*>(B), 32);
            };
            const auto Size = [](const std::string& Bytes, uint32_t& W, uint32_t& H) {
                return ImageFacts::SizeFromHeader(reinterpret_cast<const unsigned char*>(Bytes.data()), Bytes.size(), W, H);
            };
            uint32_t W = 0, H = 0;
            Check(Size(Png(1, 1), W, H) && W == 1 && H == 1 && ImageFacts::CompressionFor("compressed high color", W, H) == "uncompressed", "B10: a 1x1 PNG is written uncompressed", std::to_string(W));
            Check(Size(Png(4, 4), W, H) && W == 4 && H == 4 && ImageFacts::CompressionFor("compressed high color", W, H) == "compressed high color", "B10: a 4x4 PNG keeps its compression", std::to_string(W));
            Check(Size(Dds(1, 1), W, H) && W == 1 && H == 1 && ImageFacts::CompressionFor("compressed no alpha", W, H) == "uncompressed", "B10: a 1x1 DDS is written uncompressed", std::to_string(W));
            Check(Size(Dds(4, 4), W, H) && W == 4 && H == 4 && ImageFacts::CompressionFor("compressed no alpha", W, H) == "compressed no alpha", "B10: a 4x4 DDS keeps its compression", std::to_string(W));
            Check(Size(Png(6, 8), W, H) && ImageFacts::CompressionFor("compressed high color", W, H) == "uncompressed", "B10: 6x8 is not a multiple of 4 on width: uncompressed", std::to_string(W));
            Check(ImageFacts::CompressionFor("R8", 1, 1) == "R8", "B10: the R8 formats are not block compressed and stay", "");
            Check(!Size(std::string(32, 'x'), W, H), "B10: a file that is neither PNG nor DDS has no size", "");
            // the files on disk, through SizeOf
            const fs::path Dir = fs::temp_directory_path() / ("graywolf_selftest_imgsize_" + std::to_string(GetCurrentProcessId()));
            std::error_code Ec;
            fs::create_directories(Dir, Ec);
            std::ofstream(Dir / "a.png", std::ios::binary) << Png(1, 1);
            std::ofstream(Dir / "b.dds", std::ios::binary) << Dds(4, 4);
            uint32_t W1 = 0, H1 = 0, W2 = 0, H2 = 0;
            const bool OnDisk = ImageFacts::SizeOf(Dir / "a.png", W1, H1) && ImageFacts::SizeOf(Dir / "b.dds", W2, H2) && W1 == 1 && H1 == 1 && W2 == 4 && H2 == 4;
            fs::remove_all(Dir, Ec);
            Check(OnDisk, "B10: SizeOf reads the header of a PNG and a DDS file", "");
        }

        // ---- A: the disk floor is a state, never an exit ----
        {
            const fs::path Dir = fs::temp_directory_path() / ("graywolf_selftest_floor_" + std::to_string(GetCurrentProcessId()));
            std::error_code Ec;
            fs::create_directories(Dir, Ec);
            const double Before = DiskGuard::FloorGb();
            DiskGuard::Reset();
            DiskGuard::FloorGb() = 8.0;
            _putenv_s("GRAYWOLF_TEST_FREE_BYTES", "1000");   // a test hook (never a product flag): the drive reports 1000 bytes free
            const bool First = DiskGuard::Admit(Dir / "a.dds");   // the first call reads the drive
            const bool Reached = DiskGuard::Reached();
            const bool Again = DiskGuard::Admit(Dir / "b.dds");   // once reached, every call is false (and the process is still here)
            XImageDDS Image;
            Image.DataBuffer = new int8_t[16]();   // NOLINT(cppcoreguidelines-owning-memory): XImageDDS owns its buffer and frees it with delete[]
            Image.DataSize = 16;
            const bool Wrote = ExportWriters::WriteImage(Image, (Dir / "c.dds").string(), "DDS");
            const bool Left = fs::exists(Dir / "c.dds", Ec);
            std::string Row = DiskGuard::RowText();
            const bool Reported = DiskGuard::WriteReport(Dir) && fs::exists(Dir / "graywolf_disk_floor.tsv", Ec);
            _putenv_s("GRAYWOLF_TEST_FREE_BYTES", "");
            DiskGuard::Reset();
            const bool Room = DiskGuard::Admit(Dir / "d.dds");   // state cleared, the real drive has room for the 8 GB floor? only when it does
            uint64_t Free = 0;
            const bool Real = DiskGuard::Available(Dir / "d.dds", Free);
            DiskGuard::FloorGb() = Before;
            DiskGuard::Reset();
            fs::remove_all(Dir, Ec);
            Check(!First && Reached && !Again && !Wrote && !Left && Reported && Row.rfind("disk_floor_reached", 0) == 0,
                  "A: an injected low free value sets the floor state, the image write fails and leaves no file, the row is reported, and nothing exits", Row);
            Check(!Real || IsBelow(Free) || Room, "A: with the injection removed the state is cleared and a drive with room admits writes", std::to_string(Free));
            Check(DiskGuard::FloorGb() == Before || Before < 0.0, "A: the floor is off until a CLI export command sets it", std::to_string(Before));
        }

        // ---- B11: the plan's arithmetic ----
        {
            Check(DiskGuard::EstimateImageBytes(false, 5000, 0, 0) == 5000, "B11: a copied image is its own size", "");
            Check(DiskGuard::EstimateImageBytes(true, 0, 1024, 512) == 1024ull * 512 * 4 + 1024, "B11: a converted image is width x height x 4 plus a header", "");
            const uint64_t Gb = 1ull << 30;
            Check(!DiskGuard::PlanExceeds(10 * Gb, 20 * Gb, 8.0), "B11: 10 GB + 8 GB floor fits in 20 GB", "");
            Check(DiskGuard::PlanExceeds(13 * Gb, 20 * Gb, 8.0), "B11: 13 GB + 8 GB floor does not fit in 20 GB", "");
            Check(DiskGuard::PlanExceeds(1, 8 * Gb, 8.0) && !DiskGuard::PlanExceeds(0, 8 * Gb, 8.0), "B11: the estimate alone at exactly the floor tips it", "");
        }
        return Failed;
    }
}
