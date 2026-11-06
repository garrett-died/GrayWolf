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
- The sun disc: texels above 1000 are cut before the bake (HydraX's exposure cutoff), so the disc does not light the model a
  second time on top of the preset's sun. **Hypothesis** that this looks right.
- Night is baked at 0.06 / 0.18 of the others' strength (HydraX's per-state target grey, calibrated by eye there, not the game's value).
