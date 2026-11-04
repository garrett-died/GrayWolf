# Light conversion: compiled lights to Radiant `light` keys

Status 2026-11-04. Offline, from zone files only; no game, no Radiant. Labels as the project uses them:
**proven** (fitted against the stock data, count given), **tested** (unit test only), **assumed**, **unknown**.

HydraX said this comparison needed a live BO3 session. It did not: the stock `zm_giant.led` holds the
stock keys of every stock light, and the `zm_factory.ff` zone holds the compiled lights, so the two can be
fitted against each other offline.

## 1. Method

- Compiled side: GrayWolf's `Treyarch::Lights` (`Games/Treyarch/TreyarchMaps.cpp`) reads the `com_map` light
  list of `zm_factory.ff`. Black Ops III records are 564 bytes. A run with the environment variable
  `GH_MAPDEBUG` set to a file path now also writes each record whole as hex (`[lightraw]` lines), which is what
  the fits below read.
- Stock side: the entity section of `share/raw/maps/zm/zm_giant.led` in the BO3 install (magic `LED\0`,
  length-prefixed strings; HydraX `LedEntities.cs` documents the layout). It holds 527 `light` entities with
  all their keys. Only `zm_giant` is a valid oracle (user rule), and `zm_factory.ff` is the zone of that map.
- Pairing:
  - All 527 compiled lights (292 spot, 235 omni, no sun) have a stock light at their origin within 0.01
    (547 pairs: 20 compiled lights have two stock lights at one origin). The earlier "399 of 527" was
    not reproduced, because that figure compared against something other than the `.led`; origin alone pairs
    all 527.
  - The u32 at record offset 68 is the first four bytes of the stock `guid` (`{0351DCD6-...}` is
    `0x0351DCD6`). It pairs 443 compiled lights to exactly one stock light. The other 84 share a guid prefix
    (copied lights). **The fits below use these 443 unambiguous pairs**; the radius, fov, near, far, spec,
    cut_on and penumbra fits were also run on the 527 origin pairs and agree.
- Tolerance: relative 1e-3 for values, 0.01 degree for angles, 0.01 for stops-like sums where stated.
- A key that fits under 95% is **unproven and not written**.

## 2. BO3: field, formula and fit (proven, 443 pairs)

Offsets are record-relative, little-endian floats unless stated.

| Stock key | Field | Formula | Fit | Written |
|---|---|---|---|---|
| `PRIMARY_TYPE` | u32 at +64 | 4 `PRIMARY_OMNI`, 2 `PRIMARY_SPOT` | 443/443 | yes (type 1, sun, does not occur) |
| `origin` | +72 | identity | 443/443 | yes |
| `angles` pitch, yaw | forward vector +312 | `pitch = asin(-fz)`, `yaw = atan2(fy, fx)`, degrees, 0 to 360 | 443/443 (pitch compared modulo 360; yaw not compared at pitch +-90) | yes |
| `angles` roll | not found | no field correlates (scan of every offset, as degrees, radians, sin, cos) | 273/443 have roll 0 | roll written 0; **unproven** (170 of 443 stock lights have a roll) |
| `radius` | +108 | identity | 443/443 | yes |
| `fov_outer` | +268 (cosine of the half angle) | `2 * acos(c)` degrees | 443/443 (spot 235/235; omni gives 90 as stock) | yes |
| `near_edge` | +112 | identity | 443/443 (36/36 nonzero) | yes |
| `far_edge` | +116 | identity, except stock `far_edge < near_edge`, which the compile raises to `near_edge` | 438/443 identity; 443/443 as `max(far, near)` (the 5 misses are all far below near) | yes (the compiled value) |
| `falloffdistance` | +84 | identity while the falloff is on | 438/438 with `ENABLE_FALLOFF 1` | yes |
| `ENABLE_FALLOFF` | +84 against +108 | `0` exactly when `+84 == 2 * radius` | 443/443 (5 lights, all with stock `falloffdistance` 12) | yes, only the `0` case; then no `falloffdistance` (Radiant's default stays) |
| `spec_comp` | +100 (the scale) | `log2(scale)` | 443/443 (36/36 nonzero) | yes |
| `cut_on` | +104 | identity | 443/443 (203/203 nonzero) | yes |
| `penumbraRadius` | +148 | identity | 443/443 (values 1, 2, 3) | yes |
| `bulbRadius` | +308 | identity | 421/421 where the key exists (3 not 0.01) | yes |
| `superellipse` | +280, +284, +288, +292 | identity | spot 234/235 (99.6%); omni 0.75 1 0.75 1 is not carried | yes, spots only |
| `_color` and `stops`, jointly | RGB at +88, +92, +96 | `rgb = srgb_to_linear(_color) * 2^stops` (sRGB piecewise curve) | 443/443 on all three channels (527/527 on the origin pairs) | see below |

### `_color` and `stops` apart: unproven

The compiled light keeps only the product, the linear radiance. No other field holds `_color`, `2^stops` or
`stops` (every offset scanned against `_color`, its linear form, `stops` and `2^stops`). The stock lights are not
normalised: the split "`stops = log2(max channel)`, `_color = srgb(rgb / max)`" reproduces the stock keys for
392/443 (88.5%), under 95%. So **`_color` and `stops` are not written**. The exact product is written as
`graywolf_unresolved_radiance "r g b"` (linear), which keeps the data and which Radiant ignores. Setting both
keys from the product would give the right light and the wrong keys; that is a convention to choose, not a measurement.

### Keys of the stock light that are not written

| Key | Why |
|---|---|
| `roundness` | `+276` fits 315/443 (71%): unproven |
| `bake_intensity_scale`, `volumetric*`, `shadowmapScale`, `dirSpan`, `min_light_cutoff`, `cull_dist`, `bulbLength`, `PRIMARY_NOSHADOWMAP` and the rest | constants in the stock lights, or no field found; nothing was fitted. Left to Radiant's defaults |
| `def` and `name` (the stock light definition and name) | strings in the stock map source; the record has none at a known place for BO3. `name` is `graywolf_light_<guid prefix>` |

## 3. What the BO3 writer emits

`Maps::LightEntityKeys` (`MapRadiantRules.cpp`), for `Light::Bo3`: `classname`, `origin`, `angles`
(`pitch yaw 0`), `PRIMARY_TYPE`, `name`, `radius`, `fov_outer`, `near_edge`, `far_edge`, `falloffdistance` or
`ENABLE_FALLOFF 0`, `spec_comp`, `cut_on`, `penumbraRadius`, `bulbRadius`, `superellipse` (spots), and
`graywolf_unresolved_radiance`. Tested in `RadiantRulesSelfTest` ("a Black Ops III light"). Not run in Radiant
(no Radiant in this task): that the entity loads is **unknown**.

## 4. BO4 (600-byte record)

Data: the 709 compiled lights of `mp_frenetic.ff` (450 omni, 259 spot), records read whole. There is **no BO4
oracle**: BO4 has no `.led`, and no stock BO4 light keys exist to fit against. So nothing below is a fit against
stock values; it is only a check of whether the BO3 formulas and structure carry over.

| Check | BO3 (proven) | BO4 (measured) | Carries over? |
|---|---|---|---|
| radiance RGB | +88, values 1 to 47,975 (`2^stops` at most 2^15.55) | +168, values 298 to 4.8e11 (median 2.7e6) | **no**: five orders of magnitude apart, so the unit is not the same and `srgb_to_linear(_color) * 2^stops` cannot be checked or inverted |
| field layout | scalars at +84 to +116, forward at +312 | basis (forward, right, up) at +84, +96, +108; the scalars sit elsewhere | **no**: the same structure does not hold, a field cannot be named by its BO3 offset |
| radius | +108, 7 to 22,000 | +204 (5 to 10,000, 239 distinct) and +448 (4.7 to 17,300, 312 distinct) are both radius-sized; +188 is in -1 to 1 | **unknown**: two candidates, no oracle to tell them apart (HydraX left both unresolved) |
| cone | +268 cosine of the half angle | not located | unknown |
| type | u32 at +64: 4 omni, 2 spot | same values at +64 (450 and 259) | yes (already used) |
| origin | +72 | +72 | yes (already used) |

Decision: **BO4 keeps its unresolved keys** (`graywolf_unresolved_rgb_at_168`, `..._float_at_204`,
`..._float_at_448`, `..._def`) and writes no `_color`, `stops`, `radius`, `fov_outer`, `near_edge`, `far_edge` or
`falloffdistance`. Assumed only, never measured for BO4: that its compile keeps the same sRGB-to-linear and
`2^stops` product idea. Proven for BO3 only: everything in section 2.

To get BO4 values later one needs stock BO4 light keys (a BO4 map source or lighting file with a known compiled
zone), the way `zm_giant.led` serves BO3; then the same pairing (a guid prefix at +68 is also present in the BO4
record) and the same scan can be run with `GH_MAPDEBUG`.

## 5. Reproducing

1. `GH_MAPDEBUG=<file>` and a map run (`disk-export --game bo3 ... zm_factory.ff`) write the `[lightraw]` lines.
2. The `.led` entities are read with the layout in HydraX's `LedEntities.cs`.
3. Pair by the guid prefix at +68 (or origin), then compare each stock key with each float offset under the
   candidate formulas. The scan scripts were scratch work; the table in section 2 is the result.

## 6. Open items

- Roll of a light: not in the record as far as scanned; written 0 (170 of 443 stock lights have a roll).
- `_color` and `stops` apart: needs a convention; the product is exact.
- `roundness` (71% fit), `def` (the light definition) and `name`: no field found.
- The Radiant load of the written BO3 light entities was not run.
