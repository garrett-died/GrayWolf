#!/usr/bin/env python3
"""Writes zonekit/profiles/<profile>/pool_map.json, one row per pool of types.json.

    python -I tools/bo3target/gen_pool_map.py --mod-tools-root <BO3 Mod Tools root> [--profiles zonekit/profiles] [--games t7,bo4,...] [--check]

A row is {pool, kind, bo3_awi, neutral_form, route} (docs/hydrax-port/p1-pool-coverage.md):

  bo3_awi       the pool's name when <root>/deffiles/<name>.awi exists, or the target of an explicit alias below
                (checked against deffiles too). Never guessed.
  kind          the `--type` kind of GrayWolf's disk-export that writes the pool, read off the games' code
                (DIRECT / MAP_POOLS / CARRIED below, each cut to the kinds the game really has: GAME_KINDS,
                which is `game_kinds` of `pool-coverage <game>`'s JSON). null when nothing exports the pool.
  neutral_form  how an exporter writes a pool that has no .awi. null when nothing does: no form is made up.
  route         direct (the kind counts the pool), carried:<pool> (written inside another pool's output),
                map (part of the map export) or none.

Only the Mod Tools root is machine specific, and it is an argument.
"""
import argparse
import json
import os
import sys

# the kinds `pool-coverage <game>` printed for each game's profile (game_kinds of graywolf_pool_coverage.json:
# the exporters the game's code registers). Update when a game gains one.
GAME_KINDS = {
    "t7": {"anim", "data", "fx", "gdt", "image", "localize", "map", "material", "model", "rawfile", "script", "sound", "weaponstats"},
    "bo4": {"aitypes", "anim", "bundles", "data", "fx", "gdt", "image", "localize", "map", "mapents", "mapinfo", "material", "matlinks", "model",
            "rawfile", "refs", "script", "sndlinks", "sound", "weapons", "weaponstats"},
    "t9": {"aitypes", "anim", "bundles", "data", "fx", "gdt", "image", "localize", "map", "mapents", "mapinfo", "model", "rawfile", "refs",
           "script", "sound", "weapons", "weaponstats"},
    "h1": {"anim", "data", "fx", "image", "localize", "map", "material", "model", "rawfile", "script", "sound", "weapons", "weaponstats"},
    "s2": {"anim", "data", "fx", "image", "localize", "map", "material", "model", "rawfile", "script", "sound", "weapons", "weaponstats"},
    "t5": {"anim", "data", "fx", "image", "localize", "map", "material", "model", "rawfile", "script", "sound", "weapons", "weaponstats"},
    "t4": {"anim", "data", "fx", "image", "localize", "map", "material", "model", "rawfile", "script", "sound", "weapons", "weaponstats"},
}

# pool name (in a game's types.json) -> .awi, where the names differ but are the same asset family
ALIASES = {
    "vehicledef": "vehicle",
    "weapon_camo": "weaponcamo",
    "attachment_unique": "attachmentunique",
    "footstep_table": "footsteptable",
    "cgmedia": "cgmediatable",
    "objective_list": "objectivelist",
    "light_description": "lightdescription",
    "playerfx": "playerfxtable",
    "playersounds": "playersoundstable",
    "surfacefx_table": "surfacefxtable",
    "impact_sound": "impactsoundstable",
    "impact_fx": "impactsfxtable",
    "glasses": "glass",
    "bulletpenetration": "bullet_penetration",
}

AUDIO = "audio files (wav, flac or opus)"
RAW = "raw files (share/raw layout)"
JSON = "json"

# pool -> (kind, neutral form), for the pools a game's exporter writes on their own. Per game below.
COMMON = {
    "xmodel": ("model", None), "xanim": ("anim", None), "image": ("image", None), "material": ("material", None),
    "sound": ("sound", AUDIO), "rawfile": ("rawfile", RAW), "stringtable": ("data", "csv"),
    "localize": ("localize", "csv"), "localize_entry": ("localize", "csv"), "localizeentry": ("localize", "csv"),
    "fx": ("fx", JSON), "techset": ("techsets", "techsets folder (json and shader text)"),
    "technique_set": ("techsets", "techsets folder (json and shader text)"),
}
PER_GAME = {
    "t7": {"scriptparsetree": ("script", "gsc source (decompiled) or compiled bytes")},
    "bo4": {
        "scriptparsetree": ("script", "gsc source (decompiled) or compiled bytes"), "luafile": ("rawfile", RAW),
        "weapon": ("weapons", JSON), "weapon_tunables": ("weapons", JSON), "attachment": ("weapons", JSON), "attachment_unique": ("weapons", JSON),
        "scriptbundle": ("bundles", JSON), "aitype": ("aitypes", JSON), "maptable": ("mapinfo", JSON), "maptable_list": ("mapinfo", JSON),
    },
    "t9": {
        "scriptparsetree": ("script", "gsc source (decompiled) or compiled bytes"), "luafile": ("rawfile", RAW),
        "weapon": ("weapons", JSON), "weapontunables": ("weapons", JSON), "weaponstylesettings": ("weapons", JSON), "weaponfull": ("weapons", JSON),
        "weaponblueprint": ("weapons", JSON), "weaponsecondarymovement": ("weapons", JSON), "attachment": ("weapons", JSON),
        "attachmentunique": ("weapons", JSON), "attachmenttable": ("weapons", JSON),
        "scriptbundle": ("bundles", JSON), "aitype": ("aitypes", JSON),
        "maptable": ("mapinfo", JSON), "maptableentry": ("mapinfo", JSON), "maptablelist": ("mapinfo", JSON),
    },
    "h1": {"scriptfile": ("script", "gsc source (decompiled) or compiled bytes"), "luafile": ("rawfile", RAW),
           "weapon": ("weapons", JSON), "attachment": ("weapons", JSON)},
    "s2": {"scriptfile": ("script", "gsc source (decompiled) or compiled bytes"), "luafile": ("rawfile", RAW),
           "weapon": ("weapons", JSON), "attachment": ("weapons", JSON)},
    "t5": {"weapon": ("weapons", "weapon files (OpenAssetTools text)")},
    "t4": {"weapon": ("weapons", "weapon files (OpenAssetTools text)"), "loaded_sound": ("sound", AUDIO)},
}
MAP_POOLS = {"gfx_map", "gfxworld", "clipmap", "comworld", "gameworld", "com_map", "col_map", "col_map_mp", "col_map_sp",
             "game_map", "game_map_mp", "game_map_sp", "map_ents", "addon_map_ents", "phys_collmap", "phys_worldmap",
             "fx_map", "glass_map", "ui_map", "gfx_map_trzone", "col_map_trzone"}
CARRIED = {"xmodelmesh": "xmodel", "xmodelsurfs": "xmodel", "xmodelbase": "xmodel", "xsurfshared": "xmodel", "xmodelpieces": "xmodel"}
NOT_POOLS = {"", "?", "<unused>"}


def awi_for(pool, awis):
    """(awi or None, warning or None)"""
    if pool in awis:
        return pool, None
    alias = ALIASES.get(pool)
    if alias is None:
        return None, None
    if alias in awis:
        return alias, None
    return None, "alias %s -> %s has no %s.awi in deffiles" % (pool, alias, alias)


def rows_for(profile, pools, awis):
    kinds = GAME_KINDS[profile]
    direct = dict(COMMON)
    direct.update(PER_GAME.get(profile, {}))
    rows, warnings = [], []
    for pool in sorted(pools):
        awi, warn = awi_for(pool, awis)
        if warn:
            warnings.append(warn)
        kind, neutral, route = None, None, "none"
        if pool in direct:
            kind, neutral = direct[pool]
            route = "direct"
        elif pool in CARRIED:
            kind, route = "model", "carried:" + CARRIED[pool]
        elif pool in MAP_POOLS:
            kind, route = "map", "map"
        if kind is not None and kinds is not None and kind not in kinds:
            kind, neutral, route = None, None, "none"
        if awi is not None:
            neutral = None   # an awi pool is converted, not left in a neutral form
        rows.append({"pool": pool, "kind": kind, "bo3_awi": awi, "neutral_form": neutral, "route": route})
    return rows, warnings


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mod-tools-root", required=True, help="BO3 Mod Tools root (read only: deffiles/*.awi)")
    ap.add_argument("--profiles", default=os.path.join(here, "..", "..", "zonekit", "profiles"))
    ap.add_argument("--games", default=",".join(GAME_KINDS))
    ap.add_argument("--check", action="store_true", help="write nothing; exit 1 when a file would change")
    args = ap.parse_args()

    deffiles = os.path.join(args.mod_tools_root, "deffiles")
    if not os.path.isdir(deffiles):
        sys.exit("no deffiles folder in " + args.mod_tools_root)
    awis = {f[:-4] for f in os.listdir(deffiles) if f.endswith(".awi")}
    print("%d .awi files" % len(awis))

    changed = 0
    for profile in args.games.split(","):
        if profile not in GAME_KINDS:
            sys.exit("unknown game " + profile)
        folder = os.path.join(args.profiles, profile)
        with open(os.path.join(folder, "types.json"), encoding="utf-8") as f:
            names = json.load(f)["names"]
        pools = {n for n in names.values() if n not in NOT_POOLS}
        rows, warnings = rows_for(profile, pools, awis)
        text = json.dumps({"game": profile, "generated_by": "tools/bo3target/gen_pool_map.py", "pools": rows}, indent=1) + "\n"
        out = os.path.join(folder, "pool_map.json")
        old = open(out, encoding="utf-8").read() if os.path.exists(out) else None
        if old != text:
            changed += 1
            if not args.check:
                with open(out, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
        by_route = {}
        for r in rows:
            by_route[r["route"].split(":")[0]] = by_route.get(r["route"].split(":")[0], 0) + 1
        print("%-4s %3d pools, %2d with an awi, routes %s%s" % (profile, len(rows), sum(1 for r in rows if r["bo3_awi"]),
              json.dumps(by_route, sort_keys=True), "" if old == text else "  (written)" if not args.check else "  (would change)"))
        for w in warnings:
            print("  warning:", w)
    if args.check and changed:
        sys.exit(1)


if __name__ == "__main__":
    main()
