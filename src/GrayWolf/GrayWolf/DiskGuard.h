#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

// The free space floor of an export. A write never ends the process (a library call must not end a GUI session or skip the end-of-run reports): below the floor
// the guard sets a process-wide state, Admit returns false (the image write fails and leaves no file), and the export loop and the publish step see the state,
// start no new work, finish what is in flight, write every report (a disk_floor_reached row in graywolf_disk_floor.tsv in the run's report folder) and only the
// CLI layer turns the state into exit code 75. The floor is off (negative) until a CLI export command sets its default (8 GB) or --min-free-gb: the GUI never does.
// A full-map PNG export of a large map writes tens of GB; the run that filled C: to 0 bytes is what this prevents.
namespace DiskGuard
{
    inline double& FloorGb()
    {
        static double Gb = -1.0;   // negative: not set (off); 0: off by request; --min-free-gb or the CLI default sets it
        return Gb;
    }

    // Where graywolf_disk_floor.tsv goes (the run's report folder), set by the CLI export command
    inline std::string& ReportDir()
    {
        static std::string Dir;
        return Dir;
    }

    inline bool IsBelowFloor(uint64_t FreeBytes, double Gb) { return static_cast<double>(FreeBytes) < Gb * 1073741824.0; }

    // The bytes an image would take in the target: a copied file is its own size; one converted to TIFF is uncompressed 4 bytes a pixel plus a small header
    inline uint64_t EstimateImageBytes(bool Converted, uint64_t FileBytes, uint32_t W, uint32_t H)
    {
        return Converted ? static_cast<uint64_t>(W) * H * 4ull + 1024ull : FileBytes;
    }

    // True when the estimate plus the floor does not fit in the free space
    inline bool PlanExceeds(uint64_t EstimateBytes, uint64_t FreeBytes, double Gb) { return static_cast<double>(EstimateBytes) + Gb * 1073741824.0 > static_cast<double>(FreeBytes); }

    // The free bytes of Path's drive. A test (never a product flag) sets GRAYWOLF_TEST_FREE_BYTES to inject a value.
    inline bool Available(const std::filesystem::path& Path, uint64_t& Free)
    {
        char Injected[32] = {};
        size_t Len = 0;
        if (getenv_s(&Len, Injected, sizeof(Injected), "GRAYWOLF_TEST_FREE_BYTES") == 0 && Len > 1 && Injected[0] != 0)
        {
            Free = std::strtoull(Injected, nullptr, 10);
            return true;
        }
        std::error_code Ec;
        std::filesystem::path Dir = Path.has_parent_path() ? Path.parent_path() : std::filesystem::current_path(Ec);
        while (!Dir.empty() && !std::filesystem::exists(Dir, Ec) && Dir.has_parent_path() && Dir.parent_path() != Dir) Dir = Dir.parent_path();
        const auto Space = std::filesystem::space(Dir, Ec);
        if (Ec) return false;
        Free = static_cast<uint64_t>(Space.available);
        return true;
    }

    // Checks the drive of Path against Gb. True and a row text when the free space is under the floor; false when there is room (or the space cannot be read: not judged).
    inline bool Check(const std::filesystem::path& Path, std::string& Row, double Gb)
    {
        uint64_t Free = 0;
        if (Gb <= 0.0 || !Available(Path, Free) || !IsBelowFloor(Free, Gb)) return false;
        Row = "disk_floor_reached\t" + Path.parent_path().string() + "\tfree_bytes=" + std::to_string(Free) + "\tfloor_gb=" + std::to_string(Gb) +
              "\tno image was written after " + Path.string() + "; free space on the output drive is under the floor (--min-free-gb)\n";
        return true;
    }

    struct State
    {
        std::atomic<bool> Reached{ false };
        std::atomic<uint32_t> Calls{ 0 };
        std::mutex Lock;
        std::string Row;
    };
    inline State& Global()
    {
        static State S;
        return S;
    }

    // True once the floor was reached in this process (until Reset)
    inline bool Reached() { return Global().Reached.load(); }
    inline std::string RowText()
    {
        std::lock_guard<std::mutex> G(Global().Lock);
        return Global().Row;
    }
    inline void Reset()
    {
        State& S = Global();
        std::lock_guard<std::mutex> G(S.Lock);
        S.Reached = false, S.Calls = 0;
        S.Row.clear();
    }

    // The writers' call, from every image writer through this one path: true when the write may go on. The drive is read on the first call and every 64th across
    // all writers; once the floor is reached every call is false. A false never ends the process.
    inline bool Admit(const std::filesystem::path& Path)
    {
        State& S = Global();
        if (S.Reached.load()) return false;
        if (FloorGb() <= 0.0 || (S.Calls.fetch_add(1) & 63) != 0) return true;
        std::string Row;
        if (!Check(Path, Row, FloorGb())) return true;
        {
            std::lock_guard<std::mutex> G(S.Lock);
            if (S.Row.empty()) S.Row = Row;
        }
        S.Reached = true;
        return false;
    }

    // Writes the row into the report folder (the CLI layer, at the end of the run, after the other reports). True when there was a row.
    inline bool WriteReport(const std::filesystem::path& Fallback)
    {
        if (!Reached()) return false;
        const std::filesystem::path Dir = ReportDir().empty() ? Fallback : std::filesystem::path(ReportDir());
        std::error_code Ec;
        std::filesystem::create_directories(Dir, Ec);
        std::ofstream(Dir / "graywolf_disk_floor.tsv", std::ios::binary | std::ios::trunc) << "code\tdrive_folder\tdetail\tmessage\n" << RowText();
        return true;
    }

    // The exit code the CLI layer returns for a run that reached the floor
    constexpr int ExitCode = 75;
}
