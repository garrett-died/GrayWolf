#!/usr/bin/env python3
"""Builds GrayWolf's BO3 target data from HydraX's reference data.

Reads (read-only):
  <hydrax>/docs/references/techsets/catalog.json      the Black Ops III techset catalog (7.6 MB)
  <hydrax>/packages/v02/src/HydraX.Kernel/TechsetRenderTable.g.cs   each techset's render class (blend, traits)
  <hydrax>/docs/references/bo3/material-defaults.json  the Mod Tools' modal value of each material key
  <hydrax>/docs/references/bo4/constant-layouts.json   BO4 constant buffer offsets per techset hash

Writes under <out> (default: zonekit/profiles next to this repo's tools folder):
  t7/target/techsets.json            trimmed catalog (name, category, render flags, class, slots)
  t7/target/material-defaults.json   copy of the defaults
  bo4/target/constant-layouts.json   copy of the layouts

techsets.json (version 1): {"version":1, "techsets":[{"n":name,"c":category,"f":renderFlags,
"b":blend,"t":traits,"s":[[slot name, GDT key or null, semantic or null, default image or null], ...]}]}
  - a slot's GDT key is its imageSlot, else its name when it declares no default image at all (HydraX
    TechsetProjection.ImageProperty); null means the slot has no material key (an engine-only texture)
  - the default image is kept only for a slot that has a key (a material leaves that image out)
  - b is the TechsetBlend name and t the TechsetTraits bit set of TechsetRenderTable.g.cs (absent from the
    table: "Other" and 0)

Run: python -I tools/bo3target/gen_techsets.py --hydrax DIR [--out DIR]
"""
import argparse
import json
import os
import re
import shutil
import sys



def read_enum(text, name):
    m = re.search(r'enum\s+' + name + r'\s*\{(.*?)\n\}', text, re.S)
    if not m:
        raise SystemExit('enum %s not found in the render table' % name)
    values = {}
    for em in re.finditer(r'^\s*(\w+)\s*=\s*(\d+),', m.group(1), re.M):
        values[em.group(1)] = int(em.group(2))
    return values


def read_render_table(path):
    text = open(path, encoding='utf8').read()
    traits = read_enum(text, 'TechsetTraits')
    table = {}
    for m in re.finditer(r'\["([^"]+)"\]\s*=\s*\(TechsetBlend\.(\w+),\s*([^)]*)\),', text):
        bits = 0
        for part in m.group(3).split('|'):
            part = part.strip().replace('TechsetTraits.', '')
            if part:
                bits |= traits[part]
        table[m.group(1)] = (m.group(2), bits)
    return table


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser()
    ap.add_argument('--hydrax', required=True, help='a HydraX checkout')
    ap.add_argument('--out', default=os.path.join(repo, 'zonekit', 'profiles'))
    a = ap.parse_args()

    catalog_path = os.path.join(a.hydrax, 'docs', 'references', 'techsets', 'catalog.json')
    table_path = os.path.join(a.hydrax, 'packages', 'v02', 'src', 'HydraX.Kernel', 'TechsetRenderTable.g.cs')
    defaults_path = os.path.join(a.hydrax, 'docs', 'references', 'bo3', 'material-defaults.json')
    layouts_path = os.path.join(a.hydrax, 'docs', 'references', 'bo4', 'constant-layouts.json')
    for p in (catalog_path, table_path, defaults_path, layouts_path):
        if not os.path.isfile(p):
            raise SystemExit('missing input: ' + p)

    render = read_render_table(table_path)
    catalog = json.load(open(catalog_path, encoding='utf8'))
    out = []
    for t in catalog['techsets']:
        slots = []
        for s in t.get('textureSlots', []):
            image_slot = s.get('imageSlot')
            default = s.get('defaultImage')
            key = image_slot if image_slot else (None if default else s['name'])
            slots.append([s['name'], key, s.get('semantic') or None, default if (image_slot and default) else None])
        blend, traits = render.get(t['name'], ('Other', 0))
        out.append({'n': t['name'], 'c': t.get('category', ''), 'f': t.get('renderFlags', ''), 'b': blend, 't': traits, 's': slots})
    out.sort(key=lambda x: x['n'])

    t7 = os.path.join(a.out, 't7', 'target')
    bo4 = os.path.join(a.out, 'bo4', 'target')
    os.makedirs(t7, exist_ok=True)
    os.makedirs(bo4, exist_ok=True)
    target = os.path.join(t7, 'techsets.json')
    with open(target, 'w', encoding='utf8', newline='\n') as f:
        json.dump({'version': 1, 'source': 'HydraX docs/references/techsets/catalog.json + TechsetRenderTable.g.cs', 'techsets': out},
                  f, separators=(',', ':'), ensure_ascii=False)
        f.write('\n')
    shutil.copyfile(defaults_path, os.path.join(t7, 'material-defaults.json'))
    shutil.copyfile(layouts_path, os.path.join(bo4, 'constant-layouts.json'))
    print('techsets: %d, %d bytes' % (len(out), os.path.getsize(target)))
    print('material-defaults.json: %d bytes' % os.path.getsize(os.path.join(t7, 'material-defaults.json')))
    print('constant-layouts.json: %d bytes' % os.path.getsize(os.path.join(bo4, 'constant-layouts.json')))
    missing = sorted(t['n'] for t in out if t['n'] not in render)
    print('techsets without a render class (Other, 0): %d' % len(missing))


if __name__ == '__main__':
    sys.exit(main())
