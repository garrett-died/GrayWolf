#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

// The walk of one input's zones in pool-coverage, with the retry through the install's own list. Pure: the walk and the list are callbacks, so a test needs no
// install. A zone that fails to walk (a name the install lists but that is no file as given: mp_crash for mp_crash.ff) is retried through the list at most once
// per input; only the failed entry is dropped from Walks (an earlier zone's walk stays), and a resolved zone that is already walked, by its exact name, is not
// walked again. `x.ff` and `x.zone` are two names and stay two walks.
namespace PoolWalkRetry
{
    // WalkOne(Zone) appends one entry (with .Name) to Walks and returns whether the zone opened; Resolve(Resolved) fills the install's zones for the input and
    // returns whether it did.
    template <class Walk, class WalkOne, class ResolveFn, class SkippedFn>
    void WalkInput(std::vector<Walk>& Walks, const std::vector<std::string>& Zones, bool HaveInstall, WalkOne&& Walk1, ResolveFn&& Resolve, SkippedFn&& Skipped)
    {
        bool Retried = false;
        for (const std::string& Zone : Zones)
        {
            // a zone the retry of an earlier entry already walked (by its exact name) is not walked again
            if (std::any_of(Walks.begin(), Walks.end(), [&](const Walk& W) { return W.Name == Zone; }))
            {
                Skipped(Zone);   // reported by the caller: the zone is walked once
                continue;
            }
            const size_t Before = Walks.size();
            if (Walk1(Zone) || !HaveInstall || Retried) continue;
            Retried = true;
            std::vector<std::string> Resolved;
            if (!Resolve(Resolved) || Resolved.empty() || Resolved == Zones) continue;
            if (Walks.size() > Before) Walks.erase(Walks.begin() + static_cast<std::ptrdiff_t>(Before), Walks.end());
            for (const std::string& Zone2 : Resolved)
            {
                const bool Walked = std::any_of(Walks.begin(), Walks.end(), [&](const Walk& W) { return W.Name == Zone2; });
                if (!Walked) Walk1(Zone2);
                else Skipped(Zone2);
            }
        }
    }
}
