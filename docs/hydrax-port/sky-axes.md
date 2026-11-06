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

## Looked at in the previewer (U9, owner's install, read only; numbers only, no pixels kept)

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

## U9b: the sky's scale, the sun twice, Sunset, Morning's sun (owner's install, read only; figures only)

**How HydraX does it** (read in `SkyImage.cs`, `Shading.cs`, `ProceduralEnvironment.cs`, `Mesh.hlsl`): the sky cube is only the **backdrop**
there (scaled by `TargetMid / geometric-mean luminance`, 0.18, Night 0.06, by eye); the model's **light** is a procedural gradient from the
preset's probe colours (`ProbeScale` 0.55, Night x 0.1), so the sun disc never lights anything in HydraX. The 1000 cutoff only keeps the
disc out of that mean. GrayWolf bakes the sky into the light, so it needs a rule of its own. Apex has no sky exposure of its own (HDR buffer
times the global exposure, `led-and-sky.md`).

**The rule now (hypothesis, supported):** a sky texel is multiplied by `2^stops / Exposure` of its state (`SkyGain`: Day 2.11, Morning 1.43,
Sunset 2.83, Night 0.22), the same number the sun's radiance has, so sky and sun keep HydraX's ratio; no mean normalisation, no cut.
Checked against the other source in HydraX, the LED's probe colours (0.55 x luminance): the sky's mean times this gain is 0.58 (Morning),
0.36 (Day) and 0.56 (Sunset) against 0.55: within 6, 35 and 3 percent. **Night disagrees**: its sky's mean is 0.001, so 0.0002 on screen
against 0.055 from HydraX's probe rule (about 275 times darker); Night is now close to black, lit by the preset's sun only. Unknown which
is the game's. The Night ssi also carries `evcmp 2.5` (+2.5 EV), which is not applied anywhere.

**Sun twice, measured, so left alone:** no texel in the four skies is above 14, so the old 1000 cut did nothing. The brightest cluster
(Morning, max 13.6) integrates to 0.012 in sun units (sum of L times solid angle, 0.0014 sr): 1.2 percent of the preset sun's light, and Day,
Sunset and Night hold no disc (their brightest area is cloud). It stays in the image light and in the backdrop, as in the asset.

**Light colour:** HydraX shades the sun as `albedo / pi * SunRadiance * N.L`, the preview as `albedo * LightColor * N.L`, so
`LightColor = SunRadiance / pi` (the earlier 0.5 was a guess).

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

**Turn:** the environment's Turn now turns the preset sun with the sky (`TurnedSunDirection`), proven by `ui-selftest`.
