#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Where a world's blended surfaces keep their per-vertex layer data (Modern Warfare Remastered: gfx_map's vertex layer data, a surface's u32 at 0 is the byte offset of its
// slice). A slice has no recorded size: it runs to the next slice's offset, or to the end of the data for the last one. Every surface of the world has such an offset, a surface
// that is no blend one that means nothing, so an offset at or past the end of the data is not a slice start (it would otherwise end the last real slice beyond the data).
// A slice is accepted only when it is exactly Count x Stride bytes and lies inside the data: no stride is guessed and nothing is read outside the data.
class BlendSlices
{
public:
    // Starts: every surface's offset (any value); DataBytes: the size of the data
    BlendSlices(std::vector<uint32_t> Starts, uint64_t DataBytes) : Bytes(DataBytes)
    {
        std::erase_if(Starts, [&](uint32_t S) { return S >= DataBytes; });
        std::sort(Starts.begin(), Starts.end());
        Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
        Sorted = std::move(Starts);
    }

    // The end of the slice that starts at At: the next start above it, else the end of the data
    uint64_t EndOf(uint32_t At) const
    {
        const auto Next = std::upper_bound(Sorted.begin(), Sorted.end(), At);
        return Next == Sorted.end() ? Bytes : *Next;
    }

    // True when the slice at At holds exactly Count vertices of Stride bytes inside the data
    bool Fits(uint32_t At, uint32_t Count, uint32_t Stride) const
    {
        if (At >= Bytes || Count == 0 || Stride == 0) return false;
        const uint64_t End = EndOf(At), Need = static_cast<uint64_t>(Count) * Stride;
        return End <= Bytes && End > At && End - At == Need;
    }

    size_t Starts() const { return Sorted.size(); }
    static int SelfTest(std::string& Failures);

private:
    std::vector<uint32_t> Sorted;
    uint64_t Bytes;
};
