#include "stdafx.h"

#include "Games/IW/BlendSlices.h"

int BlendSlices::SelfTest(std::string& Failures)
{
    int Failed = 0;
    const auto Check = [&](bool Ok, const char* What) {
        if (Ok) return;
        Failed++;
        Failures += std::string("blend slices: ") + What + "\n";
    };
    // data of 48 bytes, slices at 0 (3 vertices of 8) and 24 (3 vertices of 8): the last slice ends at the end of the data
    {
        const BlendSlices S({ 0, 24 }, 48);
        Check(S.Fits(0, 3, 8) && S.Fits(24, 3, 8), "two slices that tile the data, the last one ending at its end");
        Check(!S.Fits(0, 3, 12) && !S.Fits(0, 2, 8), "a stride or a count that is not the slice's size is refused, not guessed");
        Check(!S.Fits(24, 4, 8) && !S.Fits(48, 1, 8) && !S.Fits(100, 1, 8), "a slice that would run past the data, or start at or past its end, is refused");
    }
    // a sentinel offset past the data (a surface that is no blend) is no slice start: it neither ends the last real slice beyond the data nor makes it fit
    {
        const BlendSlices S({ 0, 24, 0xFFFFFFFFu, 4096 }, 16);
        Check(S.Starts() == 1 && S.EndOf(0) == 16, "offsets at or past the end of the data are dropped; the last real slice ends at the end of the data");
        Check(!S.Fits(0, 3, 8), "LayerBytes 16, starts {0, 24}, 3 vertices of 8: 24 - 0 == 3 x 8 would pass if 24 were a start, and runs past the data");
        Check(S.Fits(0, 2, 8), "the same slice with the two vertices that are inside the data fits");
    }
    // no next slice: the last slice ends at the data's end, and the data not being tiled shows
    {
        const BlendSlices S({ 8 }, 40);
        Check(S.EndOf(8) == 40 && S.Fits(8, 4, 8) && !S.Fits(8, 3, 8), "no next slice: the end of the data ends it");
    }
    // a garbage start inside the data cuts the slice before it short: the mismatch is seen (unread), not read
    {
        const BlendSlices S({ 0, 10, 40 }, 40);
        Check(!S.Fits(0, 5, 8) && S.Fits(10, 3, 10), "a garbage start inside the data makes the slice before it a counted mismatch");
    }
    // two surfaces at one offset are one slice
    {
        const BlendSlices S({ 0, 0, 16, 16 }, 32);
        Check(S.Starts() == 2 && S.Fits(0, 2, 8) && S.Fits(16, 2, 8), "equal offsets are one start");
    }
    return Failed;
}
