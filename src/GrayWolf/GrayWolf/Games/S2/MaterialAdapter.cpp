#include "stdafx.h"

#include "MaterialDescription.h"

// WWII materials (MaterialDescription.h): the semantics the live reader names. Unlike AW's, the
// specular map (34ECCCB3) is not a specular colour with gloss in alpha; measured over the 733
// sidecars of a WWII export (2026-10-25): alpha is 255 everywhere, r is 0.19 on non-metals (wood,
// brick, cloth, lens, lashes) and 0.65-0.86 on gun metal and gold, following the colour map (r with
// its luminance +0.6..+0.95, with its saturation +1.0 on gold: the colour map holds the metal's
// colour, a metal workflow), b the gloss (glass 0.59, gold 0.46, gun metal 0.3, cloth 0.15, brick
// 0), g near 1 (a cavity term, not the occlusion map: r +0.1..+0.3 with it). Read as metalness r,
// gloss b; the occlusion is its own map (6001F931, the images named "_occ").
//
// The metalness image is a metal mask for the colour map (the metal's colour is in the colour map: a fused
// pack, GameImageUtil's WWII mask mode: metal from r, the insulator range from a), so the metalness
// map is marked ColorMetalMask. Gloss in b and the rest are plain channels of the same file.
namespace
{
    using Materials::Role;
    class WorldWar2Materials final : public Materials::SemanticAdapter
    {
    public:
        WorldWar2Materials()
            : SemanticAdapter("ww2", {
                  { 0xA0AB1041, Role::Albedo },
                  { 0x59D30D0F, Role::Normal },
                  { 0x34ECCCB3, Role::Metalness, 0 },
                  { 0x34ECCCB3, Role::Gloss, 2 },
                  { 0x6001F931, Role::Occlusion, 0 },
              }) {}
    protected:
        void Refine(const Materials::Source&, Materials::Description& D, bool) const override
        {
            if (!D.Metalness.Empty() && !D.Albedo.Empty() && D.Metalness.Path != D.Albedo.Path)
                D.Metalness.Pack = Materials::PackKind::ColorMetalMask;
        }
    };
    const WorldWar2Materials Adapter;
    const Materials::AdapterRegistration Registered("S2", Adapter);
}
