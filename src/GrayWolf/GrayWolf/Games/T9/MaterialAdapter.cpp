#include "stdafx.h"

#include "MaterialDescription.h"
#include "Games/Treyarch/TreyarchMaterials.h"
#include "SurfaceTypes.h"

// Black Ops Cold War materials: Treyarch's semantics (Games/Treyarch/TreyarchMaterials.h); a material
// they do not match is read by its names.
// The surface type is the Black Ops 4 table applied to the u32 at +0x20 (zonekit/profiles/t9/flag_names.txt): tested on a Cold War zone against
// same-named Black Ops III and Black Ops 4 materials, not proven (docs/hydrax-port/p2-surface-decals.md). A word that names no type stays unknown.
namespace
{
    class T9Materials final : public Materials::SemanticAdapter
    {
    public:
        T9Materials() : SemanticAdapter("coldwar", TreyarchMaterials::Table(), Unmatched::Names) {}
    protected:
        void Refine(const Materials::Source& M, Materials::Description& D, bool) const override { SurfaceTypes::Apply("t9", M, D); }
    };
    const T9Materials Adapter;
    const Materials::AdapterRegistration Registered("T9", Adapter);
}
