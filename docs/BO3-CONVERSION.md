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
chosen: the Mod Tools cannot register `lit_weapon_camo`.

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

## Known limits

- Not loaded in the Asset Property Editor, not linked, not seen in game.
- `--target modtools` has never run on a real install.
- Only Black Ops 4 has run end to end. Other games use the same path.
- Black Ops 4 decals are not detected, since their techset names are hashes.
- `surfaceType` is `<none>`; `usage` is `<not in editor>`, so Radiant will not list these materials yet.
- Constants exist for Black Ops 4 techsets with a proven layout only. Others keep stock defaults and
  are listed as `material.constants_defaulted`.
- A colour specular map on a scalar slot is read as a mask, which loses colour. It is counted.
- Infinite Warfare's export patches its images first, so its packs are not routed through this yet.
- Prop sets per DLC, animations, sounds, weapons, effects and maps are not part of this step yet.
