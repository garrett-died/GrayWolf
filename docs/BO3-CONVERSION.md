# Converting to Black Ops III Mod Tools

How `--target` works, what has been checked, and what has not. The README has the short version.

## Flow

Game readers are unchanged. A generic step runs after an export, so a game joins by describing its
materials, not by having conversion code of its own:

1. The export writes models, images and a `<model>.materials.json` sidecar per model.
2. `Bo3Target.Publish` reads the sidecars and, for each model, its materials and images.
3. `Bo3Target.Techsets` picks a Black Ops III techset for each material.
4. `Bo3Target.ImageUnpack` and `Bo3Target.MaterialImages` turn packed maps into plain ones.
5. `Bo3Target.GdtDb` checks what the Mod Tools already hold.
6. `Bo3Target.Placement` chooses the folder and GDT.
7. `Bo3Target.Output` writes files, merges GDTs and keeps the manifest.

`--target none`, the default, skips all of this. A plain export's GDT is unchanged; the one
difference is an extra `"pack"` key in the sidecars of games with packed maps.

## Techset choice

`zonekit/profiles/t7/target/techsets.json` lists the 1,154 stock techsets with their slots, built by
`tools/bo3target/gen_techsets.py`. Rules, first match wins: override, exact name, normalized name, sky,
family, exact slot set, slot superset, render class overlap, default. The slot rules need only the
slots a material fills, so they serve every game.

`lit` has only colour and normal slots, so it is chosen only for a material with nothing else. A
choice other than an exact one is listed as `material.techset_fallback`. Camo techsets are never
chosen: the Mod Tools cannot register `lit_weapon_camo`. Weapons-category and distortion techsets such as `lit_paintshop` are
not candidates either.

Black Ops III needs no mapping: `materialType` is the source techset name. A Black Ops III material with no techset, or a
name the catalog lacks, falls to the slot rules with a `material.techset_fallback` row. A material with a `_combo` image is
skipped, as HydraX does, with a `material.skipped_combo_image` row.

Black Ops 4 and Cold War hash their techset names. `update-name-db` fetches cod-name-db's `cod_techsets` list into
`package_index`, and the name rules run on the names it knows: 630 of 869 techsets on `mp_frenetic`. Without the list the
slot rules still run, and the run writes a `material.techset_names_missing` row. The rule behind each choice is in
`graywolf_techset_rules.tsv`.

## Image packing

Declared per game on `Materials::Map` in the game's `MaterialAdapter`, never guessed from file names.
Stock suffixes cannot be trusted: `_m` means three different things in stock data.

| Pack | Games so far | Output |
|---|---|---|
| Normal/gloss/occlusion | Black Ops 6, Black Ops 7 | RGB normal with Z in blue, grey gloss, grey occlusion |
| Fused colour, metal in a mask | WWII | Colour and colour specular |
| Fused colour, metal in alpha | Function ready, no game sets it yet | Colour and colour specular |
| Roughness | Any game whose adapter describes a roughness map | Inverted to gloss |
| Other channel | Any | Grey image |

The Black Ops 6/7 and WWII packs are self-tested, not yet proven on real images.

## Stock detection

A `gdt.db` row is stock when its GDT is in no manifest and has no `hydrax_` or `graywolf_` prefix.
`gdt.db` is read through the Mod Tools' own `sqlite64r.dll`, since ordinary SQLite builds refuse it for
having too many columns. Nothing is written beside it: when its log is in use, a copy is read instead.

## Placement accuracy

`bo3-placement-backtest` predicts every stock asset's GDT from its name alone: 20.4% exact, 16.9% right
folder, on 83,659 rows. Most misses are images and materials, which in stock sit with their model; the
backtest has no owner information but a real export does. By rule, name only: fxanim 94%, characters
89%, vehicles 69%, weapon models 59%. Exports are not scored by this number.

## Maps

A map reader fills a `MapScene` (`MapScene.h`); `--map-radiant on` writes it as a Radiant `.map`. Three options
shape that file for the Mod Tools.

**`--map-prefabs LIST`** (with `--map-radiant on`). Each listed part goes to a map of its own,
`<map>_prefab_<part>.map` (a worldspawn and the part), and the main map holds one `misc_prefab` for each, with
`model` `graywolf_maps/<file>` and origin 0 0 0. Parts: brushes, patches, decals, models, effects, entities,
brush-entities, lights, or `all`; an empty part writes no file. `--target` places the files in
`map_source\graywolf_maps`. Checked: the split and the calls on synthetic scenes (self-test) and on the Black Ops 4
`mp_frenetic` zone (seven prefab files written and placed). Not checked: Radiant loading a prefab call.

**BO3 light keys.** A Black Ops III compiled light (564 bytes) is written with the keys fitted against the stock
`zm_giant` source (443 lights paired one to one with the zone `zm_factory`): `PRIMARY_TYPE`, `origin`, `angles`
pitch and yaw, `radius`, `fov_outer`, `near_edge`, `far_edge`, `falloffdistance` (or `ENABLE_FALLOFF 0`),
`spec_comp`, `cut_on`, `penumbraRadius`, `bulbRadius` and, for spots, `superellipse`. Fits: 443 of 443 for each of
these but `far_edge` (438 as stored, 443 when raised to `near_edge`), `falloffdistance` (438 of 438 where on),
`bulbRadius` (421 of 421 where present) and `superellipse` (234 of 235 spots). Roll is written 0 and `_color` and
`stops` are not written: roll has no field found (273 of 443 stock lights have roll 0), and the compiled light keeps
only their product, which is kept as `graywolf_unresolved_radiance` (the split reproduces the stock keys for 392 of
443, under the 95% bar). Black Ops 4 lights keep unresolved keys: there is no stock oracle. Details and the method:
[LIGHT-CONVERSION.md](LIGHT-CONVERSION.md). Not checked: Radiant loading these light entities.

**`--full-map`** (needs `--target`; off by default). The map also brings what it names into the Mod Tools layout,
through the same publish step as models (GDTDB check, placement, techsets, image conversion, manifest):

1. The map export writes a list of what the scene names, `<map>.full_map.tsv`, from the `MapScene`: static, dynamic
   and splined models, a destructible's other model, the sky model, entity `model` values (not `*N` brush or `?N`
   trigger models), the keys `skyboxmodel`, `*_model`, `*Model<n>` (xmodels), `lutmaterial` (a material),
   `*Anim<n>` (xanims), `fxdef`, `*efx` and `.efx` values (effects), and the materials of the map's own render
   surfaces. The key names are the vocabulary of Black Ops 3 and 4.
2. The models it names are exported by the model kind (XMODEL_BIN, materials sidecar, images), the ones another zone
   defines pulled from that zone. The geometry materials are written as the sidecar of a model that has no file
   (`<map>_geometry`), so they are placed and converted like any model's. With `--map-radiant` the placed models are
   not written into `<map>_radiant`, which is not created, and no GDT of its own is written; the map names the models
   as the publish step does. Pieces of the world geometry no patch can carry become XMODEL_BIN models too.
3. The publish step walks each name (model, its materials, their images), also through what the Mod Tools hold, and
   writes `graywolf_full_map_report.tsv` (`map kind type name via detail`): `exported`, `in_mod_tools`,
   `unresolved` (the zone does not hold it, or it was not published), `not_exported_yet` (effects and xanims, which
   do not publish yet), and a summary line. `--plan` lists without writing; counters are `publish_full_map_*`.
4. A material a world surface draws gets the editor usage Radiant lists it under (decal, window, floor, ceiling,
   interior wall), also when a prop uses it; a material only props use stays `<not in editor>`.

Checked on the Black Ops 4 zone `mp_frenetic` (`--target folder`, nothing written to an install): 6,036 names walked:
3,533 exported, 2,496 held by the Mod Tools, 4 unresolved (two hashed models, the rope model `cylinder`, the
`lutmaterial`), 3 effects not exported yet. 1,195 distinct model values in the written `.map` files: 825 published,
369 held, 1 unresolved. All 490 image references of the published materials that the output does not define are
held. No file named by a GDT is missing, no name is defined twice, no file is unlisted; a second run changes nothing
in the target. Only that zone, only the folder target. Not checked: the output in the Asset Property Editor, a link,
Radiant, the game; games other than Black Ops 4; the lut material is not read by the Black Ops 4 map reader; models
the games name only by a hash stay unresolved. Games whose map reader is not a zone reader (`IW1IW2`) get the list and the
geometry materials but export no model for it.

## Checking output

`bo3-verify` runs at the end of `--target` and by hand; `Bo3Target.Awi` reads `deffiles\*.awi` and `Bo3Target.Verify`
holds the checks. It writes `graywolf_gate.tsv` (`check, asset, severity, detail`) and a summary. Exit 0 pass, 1 warnings or
incomplete, 2 failure, 3 reports not written. Zero files or zero entries is a failure. Rows are labelled static: a pass is
not proof Black Ops III accepts the asset. `pool-coverage` puts each pool in one state: `CONVERTED`, `EXTRACTED_NEUTRAL`,
`INERT`, `FAILED`, or the non-final `NOT_SEEN`, `WALKED_ONLY`, `STATIC_ONLY`, `UNMAPPED`. An export report is accepted only if
its zones are the zones walked, and a gate only if its GDT hash still holds. `converted` counts are per GDF in that run, not
per asset of a zone.

Checked on the Black Ops 4 zone `mp_frenetic` (25 models): 0 failures and 47 warnings: 11 `rules.diffuse_range` and 36 `install.image_overwrite`. `surfaceType <none>` passes only with a
report row showing the flag word was read and names no surface type; with no such row it is a failure. Compared with t7kb
`gdt_check` on the same files. Not built: sound, weapon, animation and stock comparison checks. A slot bound to a stock
image is judged from the stock GDT. A code image with no Mod Tools semantic stays incomplete, so that output does not reach exit 0. `--target modtools` has not been run.

## Known limits

- First link test: two Black Ops 4 models and six test materials linked with 0 errors, and 8-bit single-channel PNG and TIFF
  were accepted. That covers a small sample. Nothing has been loaded in the Asset Property Editor or Radiant, or seen in game.
- A model's GDT is not self-contained: its materials can sit in the `texture_assets` GDTs, so copy the whole output.
- `--target modtools` has never run on a real install.
- Black Ops 4 has run end to end. Black Ops III, Black Ops 4, Cold War and WWII have run through the normal map fix
  below; WWII also through `--target` on a real map zone, which worked. Black Ops 6 and Black Ops 7 data was not
  installed here, so those two games are untested.
- Gloss, occlusion, reveal, thickness and mask specular images that are exactly grey are written as 8-bit single-channel
  files in the target layout.
- The non-metal specular level 56/255 applies only to the fused colour and metal split. In that split it is correct at metal 0
  and 1 and an approximation in between. Black Ops III, Black Ops 4 and Cold War specular images are copied as authored.
- `rules.specular_level` and `rules.bare_metal` are warnings, since stock Black Ops III art breaks both guidelines too.
- Gloss range: Black Ops 4 uses its own constants. A material without a usable range keeps the default and gets a
  `material.gloss_range_default` row. Black Ops 1 and World at War gloss is unverified.
- Decorated source names (`packed_*`, `&`, `~`, or a hash token) become `i_<material>_<suffix>`: `c`, `n`, `g`, `s`, `o`, `m`, `t`, `e`.
  An image several materials share takes the first material by name in that run, so a later run with an earlier name can rename
  it and leave the old file in a reused output folder. A clean name that looks like a hash, such as `decade00`, is renamed the same way.
- `bo3-verify` also runs `model.material_closure` (every material a model names must exist), `install.image_overwrite` and
  `install.name_collision` (both warnings, read-only, and only with a Mod Tools root).
- "Stock" means a GDT saved within a day of the linker, or a row at or below an id bound computed from those GDTs. The owner's
  own content does not teach placement, but it still blocks a name. A new file aimed at any GDT this tool does not own goes to
  a `graywolf_` file in the same folder. `bo3-gdtdb-info` prints the numbers.
- `surfaceType` comes from the material flags in Black Ops III, Black Ops 4 and Cold War. Cold War rests on measurement only
  (165 of 173 stock names on one map). Other games write `<none>` and a `material.surface_unknown` row: GrayWolf does not read
  their flag word yet.
- Black Ops 4 decals come from the techset `renderFlags`. A material whose techset record could not be read gets no class and a
  `material.render_class_unread` row: 335 of 2,209 materials on `mp_frenetic`. Cold War has no decal class yet.
- `usage` is set for a map's geometry materials (`--full-map`); every other material
  stays `<not in editor>`, so Radiant lists the map's geometry and not yet a model's materials.
- Normal maps are rebuilt per material slot, not per file name. The Black Ops III, 4 and Cold War readers keep a
  normal as two components (X in red, Y in green, blue holds a variance term), so the export rebuilds blue as the Z
  of red and green (Z = 0 is stored 0.5: a channel holds (Z + 1) / 2). Only a file this run wrote is rewritten, only
  when its header says 8-bit with three or four channels, and only when its pixels look like a normal map; an image
  whose file name several images share is left alone. Each case is counted (`normals_*` in the report). Measured
  against stock sources of other folders, with the pairs that are copies of an earlier export left out
  (`tools/bo3target/image_oracle.py`): Black Ops 4, 162 normals, Z 38.0 dB median against a ceiling of 38.8 dB (the
  stock Z against the Z its own X and Y imply); Black Ops III, 476 normals, Z 36.1 dB against 33.6 dB. `.dds`
  output keeps the game's two channels (BC5 has no third), counted `normals_left_dds`. 5 Cold War images with
  no material role stay unconverted.
- Radiant geometry: no Black Ops 4 terrain yet; patches are 2x2; collision triangles are written as thin brushes.
- Constants exist for Black Ops 4 techsets with a proven layout only. Others keep stock defaults and
  are listed as `material.constants_defaulted`.
- A colour specular map on a scalar slot is read as a mask, which loses colour. It is counted.
- Infinite Warfare's export patches its images first, so its packs are not routed through this yet.
- Prop sets per DLC, animations, sounds, weapons and effects are not part of this step yet. A map's effects and xanims are listed `not_exported_yet` by `--full-map`.
