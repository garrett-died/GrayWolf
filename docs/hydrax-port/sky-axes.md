# Sky axes: the convention of the BO3 sky bake

Ported from HydraX (`HydraX.Preview/SkyImage.cs` `SkyMath.FaceAxes`, `EquirectToCube`, `SkyCubeLayout.cs`). Labels: **proven**
(a self-test fails when it is wrong), **tested** (checked against the code of HydraX or on the owner's install), **hypothesis**,
**unknown**.

## Game space

Right-handed, Z up, +X forward, +Y left (the model preview's own world space, the one the baked environments are sampled in).

## The six Mod Tools faces (`ft bk lf rt up dn`, in this order everywhere)

Image rows run top to bottom, columns left to right. For texel (x, y) of a face of size n,
`s = 2(x + 0.5)/n - 1`, `t = 1 - 2(y + 0.5)/n`, and the texel looks along `forward + s * right + t * up`.

| face | forward | right (columns) | up (rows climb) |
|---|---|---|---|
| ft | +X | -Y | +Z |
| bk | -X | +Y | +Z |
| lf | +Y | +X | +Z |
| rt | -Y | -X | +Z |
| up | +Z | +Y | +X |
| dn | -Z | -Y | +X |

Source: HydraX `SkyMath.FaceAxes`. **Hypothesis, not checked against the game** (HydraX says the same: its `phase-p2-step4-sky.md`
spike S-c is the check against a test pattern in the game). What is **proven** here is that the bake follows this table and
nothing else.

## Lat-long

The image centre looks along +X, the left edge towards +Y, the top is +Z. `u = 0.5 - atan2(y, x) / 2pi`, `v = acos(z) / pi`
(equivalently `0.5 - lat / pi`), bilinear, wrapping across, clamped down. The same convention as the `.hdr` bake
(`bake-environment`), and as HydraX's `EquirectToCube`. **Tested** by `sky-bake-selftest` (a lat-long that encodes its own
direction, and a bright spot at +Y).

## Direct3D's cube

GrayWolf samples its environment cubes with the **world** direction (`PreviewEnvironment.cpp` `CubeDir`): Direct3D's standard face
table applied to world axes, no Y/Z swap. The faces in D3D order are `+X -X +Y -Y +Z -Z`; the bake puts the Mod Tools face
that looks along an axis on the D3D face of that axis:

| D3D face | looks along | Mod Tools face |
|---|---|---|
| 0 | +X | ft |
| 1 | -X | bk |
| 2 | +Y | lf |
| 3 | -Y | rt |
| 4 | +Z | up |
| 5 | -Z | dn |

Texel directions of a D3D face (u, v in -1..1, v down): `+X (1,-v,-u)`, `-X (-1,-v,u)`, `+Y (u,1,v)`, `-Y (u,-1,-v)`,
`+Z (u,-v,1)`, `-Z (-u,-v,-1)`.

HydraX's renderer swaps Y and Z (engine to its own cube space) and stores `up`/`dn` rotated a quarter turn, because its shader
samples in that other space. GrayWolf does not: its sky is **resampled by direction** into a cube of its own at bake time, so
there is no per-face rotation to get wrong. Where HydraX's table and this one must agree is the direction each texel looks along,
and that is what the self-test compares.

## What proves it (`sky-bake-selftest`)

- one distinct colour per Mod Tools face: each D3D face shows the right one (a face swap fails);
- a cube whose every texel encodes the direction it looks along: after the bake, every D3D texel decodes to its own world
  direction (a mirrored or rotated face fails);
- a lat-long that encodes its own direction, and a bright spot a quarter in from the left edge that must light +Y only (a mirrored
  sky fails).

## Open

- Whether the Mod Tools' own faces and lat-longs follow this table in the game: **unknown** until compared with Apex or the game by
  eye (a mirrored or turned sky would show against the sun's direction: the preset sun is at the pitch and yaw of `LightingPreset`).
- The preset's sun direction versus the sky's visible sun: HydraX has a per-state yaw offset, 0 for all four, for that comparison.
  Not ported (not needed until the sky is drawn behind a model).
- The sun disc and Night's strength: see "U9b" below; the 1000 cut and the 0.06 / 0.18 factors are gone.

## Looked at in the previewer (U9, with the first bake; the Night and 1000-cut remarks are superseded by U9b below)

The four states shown on the demo sphere with the viewport's own Lighting menu (the sun is the key light at the preset's direction, the
sky is the environment and the backdrop). Labels as above.

- **Tested, by eye:** the Day, Morning and Night skies are the right way up (sky above, ground below), the horizon is level and sits at the
  screen's middle for a level camera, and the preset sun lights the sphere from the side the ground shadow says (the shadow falls away from it).
- **Tested, by decoding the EXRs** (brightest texels of each sky against the preset's sun, in the world axes of this file): Morning's disc is at
  azimuth -97.9 and elevation 7.1 degrees, the preset's sun at -97.0 and 15.0: the azimuth agrees, the elevation is 8 degrees lower in the
  image. Day's and Sunset's lat-longs hold **no sun disc** (brightest texel 2.1 and 4.1), so there is nothing to compare, and Night's brightest
  texel is about 0.001. **Unknown** whether the game puts the preset sun at the image's disc or at the preset's number; HydraX keeps its per-state
  yaw offset at 0 for the same reason.
- **The 1000 cut is a no-op on this install** (tested): no texel of any of the four skies is above 14, so nothing is cut. Morning's disc
  (13.6) stays in the baked light and lights the model a second time next to the preset's sun. A cut that does anything here would have to be
  near 5. The hypothesis "the cut keeps the disc from lighting twice" does **not hold** for these files.
- **Night at 0.06 / 0.18 looks too bright** (judged by eye): the sky's own mean is about 0.001 against Day's 0.16, yet each state is baked
  to the same mean first and Night then multiplied by one third, so Night reads as dusk (a clearly lit teal sphere), not night. Hypothesis
  stays a hypothesis; the source's real ratio is about 160 to 1.
- **Sunset's sky is a blue-sky mountain scene** in the image the chain resolves (not a fallback), with the preset's orange sun on the
  model: sun and sky do not match in colour. Unknown whether that is the game's own asset or a wrong link of the chain.

## U9b: HydraX's sky rules, ported (owner's install, read only; figures only)

Paths are under `packages/v02/src/` of HydraX. Everything below is **read in the code** (proven as source) and ported with the same numbers:

| What | HydraX | Value / rule | GrayWolf |
|---|---|---|---|
| Backdrop grey target | `HydraX.Preview/SkyImage.cs:130` | 0.18, Night 0.06 (by eye there) | `TargetMid` |
| Backdrop gain | `SkyImage.cs:139-143` | `TargetMid / ExposureEstimate` | `BakeState` scale |
| Exposure estimate | `SkyImage.cs:198-224` | geometric mean luminance over the six faces' texels, `log(lum + 1e-4)`, texels above 1000 left out | `ExposureEstimate` |
| The 1000 cut | `SkyImage.cs:37` | only for that estimate; the image keeps its disc | not applied to the image |
| Face size | `HydraX.Application/Sky/SkyFaceCache.cs:28, :248` | at most 1024; a lat-long's width / 4 | `BakeState` `FaceSize` |
| Cache cap | `SkyFaceCache.cs:31` | 200 MB | `BakeSettings::CapBytes` |
| Backdrop draw | `HydraX.Preview.D3D11/Shaders/Mesh.hlsl:550-554` | the cube, sharp, turned by the sky yaw, times the gain | `DrawBackdrop`, blur 0 for a sky |
| Model light | `HydraX.Preview/ProceduralEnvironment.cs:19-25` | gradient: zenith = probe colour * g, horizon = (probe * 0.5 + 0.5) * g, ground = (0.35, 0.32, 0.28) * g; g = `ProbeScale` * (1, or 0.1 when stops <= 5) | `ProceduralSkyOf` baked as `skylight-<state>-v1` |
| ProbeScale / NightGain | `HydraX.Preview/Shading.cs:33`, `ProceduralEnvironment.cs:15` | 0.55 / 0.1 | constants |
| Diffuse and specular environment | `ProceduralEnvironment.cs:37, :42`, `Mesh.hlsl:114-127, :499` | diffuse = lerp(ground, zenith, 0.5 n.z + 0.5); specular = lerp(Sky(r), horizon, roughness^2), **not** the sky image | the gradient cube's mips (roughness = mip / (mips - 1)) and irradiance |
| Sun | `Shading.cs:46-49, :145` | colour * 2^stops / Exposure, shaded `albedo / pi * SunRadiance * N.L` | `SunRadiance / pi` (the preview's light is `albedo * LightColor * N.L`) |
| evcmp | none | HydraX never applies it (only a BO4 map parser reads it) | not applied |
| Sky yaw | `SkyImage.cs:150-160` | 0 for every state | none |

So the sky image is **only the backdrop**; the model is lit by the gradient, so the disc never lights anything (the earlier worry about the
sun lighting twice does not arise). Studio sky and the other environments are untouched. In the app, a sky state sets `Environment` to the gradient,
`BackdropEnvironment` to the sky cube and a separate blur (`preview_sky_backdrop_blur`, 0 by default; the others keep 0.5).

**Cost of a state in the cache** (measured, BC6H with mips): the sky cube is 8.39 MB at 1024 per face (all four skies came out at 1024), the gradient
light 0.31 MB, so four states are 34 MB of the 200 MB; over the cap the oldest entry goes first (`PruneCache`, the entry being baked and its light kept).

**A model pixel against HydraX's rule** (demo sphere, default camera, `ui-shot`; HydraX's `Shade` evaluated by hand for the same normal, albedo read
from a Fullbright shot, specular ignored; GrayWolf's pixel put back to linear through its own tone curve): GrayWolf is 1.05 to 1.24 times HydraX's linear
value on patches lit by the sky, and 1.14 to 1.45 times on the patch facing the sun (its specular and Disney diffuse add what the hand rule leaves out); for
example Day, centre patch: HydraX 0.087 0.125 0.217, GrayWolf 0.103 0.142 0.240. Display values differ more in the darks (Night 6 against 16) because
GrayWolf encodes with a 2.2 power and HydraX with exact sRGB (not changed here).

**Sunset (traced):** `ssi.gdt` `default_sunset` has `skyboxmodel skybox_default_sunset`; that xmodel's skinOverride material has `colorMap
skybox_default_sunset_ft`; that image's `baseImage` is `model_export\t7_skybox\skybox_default\skybox_default_sunset_ll.exr`, a lat-long (2048 x 1024
after downsampling). The folder also holds `_ft _lf _rt _up _dn` for sunset but **no `_bk`**, and nothing in the GDT points at them; the day-clear
cube is complete (`_bk` present) and Night's cube lives in `zm_factory\Temp` (`zm_skybox_factory_05_*`, all six). So the chain is right and follows
what the Mod Tools hold: Sunset's own image is a blue-sky snow-mountain lat-long with its brightest area at elevation 1 degree, azimuth -10 (no sun,
max 4.1), while the preset's sun is orange at azimuth -60 and elevation 22. Whether APE shows the same is for the owner to compare.

**Morning's sun (traced):** the sky is `skybox_default_day_clear_0700` (a 07:00 sky; the folder also has `_0615` and `_1830`). Its disc is at
azimuth -97.9, elevation 7.1; the preset (`default_morning` pitch 165, yaw 263) gives -97.0 and 15.0, and HydraX's per-state yaw offset is 0 for all four
(no pitch offset exists). The azimuth agrees to 1 degree; the 8 degree elevation difference is between two assets of the Mod Tools (the sun lamp
and the sky image), not a bake error.

**Turn:** the environment's Turn now turns the backdrop and the preset sun together (the gradient is the same all round) (`TurnedSunDirection`), proven by `ui-selftest`.

**Night's grain is in the source (tested):** `zm_skybox_factory_05` (1024 per face, six EXRs) decoded directly against the baked backdrop cube, same
faces, regions of 254 x 254 texels (luminance; roughness is the mean of |texel - mean of its 3 x 3| over the mean). The baked cube equals the source times the
gain times 1 / 0.45 (the preview's IBL scale) to within the compression (ratio 2.22 everywhere), and the roughness is the same to the third digit:
+X 0.029 / 0.034 source, 0.030 / 0.034 baked; +Z 0.020 / 0.070 and 0.021 / 0.071; -Z (ground) 0.094 / 0.083 and 0.094 / 0.084. So the speckle in Night's
clouds and ground is noise in the EXR (3 to 9 percent texel to texel), shown as it is because the sky is exposed 78 times up (`TargetMid 0.06 / estimate`),
as HydraX does it; BC6H at these values (about 0.1 to 0.2 in the cube) adds nothing measurable. Nothing was changed.
