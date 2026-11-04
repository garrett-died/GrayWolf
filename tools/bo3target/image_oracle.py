#!/usr/bin/env python3
"""Scores exported images against the stock Black Ops III Mod Tools sources they were made from.

Read-only on both trees. For every image in an export folder whose name also exists as a stock source
(texture_assets, model_export, art_assets of the Mod Tools root): the stock source is shrunk to the
shipped size (box filter) and each channel is scored by PSNR (0..1 range, 99 = identical). Every role
is scored, not only normals:

  normal      x (red), y (green), z (blue: the shipped blue against the stock blue), and the ceiling:
              the Z stock's own x and y imply, against stock's own blue
  colour      red, green, blue; alpha when both sides have one; `opq`: the colour over the texels where the
              shipped alpha is opaque (a shipped decal has its colour zeroed where it is transparent, a
              stock source has no alpha: only the opaque texels can match)
  gloss       the red channel (BC4 one-channel images export as red)
  occlusion   the red channel
  specular    red, green, blue

The role of an image is the `semantic` of the stock GDT entry for that image (texture_assets, model_export
and source_data *.gdt text): diffuseMap, normalMap, glossMap, occlusionMap, specularMap/specularMask.
Where the tree has no image entry for a name (many shipped sources are bare files), the role is what the
stock materials bind it to (normalMap, colorMap*, cosinePowerMap, occMap, specColorMap), voted over every
material that names it, and marked `via material` in the listing. `--roles FILE` gives (or overrides)
roles from a list instead: lines of `image<TAB>role`, role one of normal colour gloss occlusion
specular. A role is never taken from a file name.

A low score is not always the exporter. The listing prints, beside each score, the correlation of the
two images, the stock source's size and the folder it was found in: a low PSNR with a high correlation is
a tone or encoding difference, a low correlation (or a stock source smaller than the shipped image) is a
source that is not what shipped.

A Mod Tools tree that has been used for ports holds, next to the real stock sources, images made from
other exports. A source that equals the shipped image to 80 dB on every channel (a block-compressed
image is never that close to its uncompressed original) is such a copy and not an independent source:
it is counted and summarised on its own line, and left out of the role medians, which describe the
independent pairs. `--keep-copies` puts them back.

Run:  python -E -s tools/bo3target/image_oracle.py EXPORT_FOLDER MODTOOLS_ROOT [--worst 15] [--roles FILE]
"""
import argparse
import os
import re
import sys
from collections import Counter, defaultdict

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

IMAGE_EXTS = (".png", ".tif", ".tiff", ".tga")
SEMANTIC_ROLE = {
    "diffusemap": "colour",
    "normalmap": "normal",
    "glossmap": "gloss",
    "occlusionmap": "occlusion",
    "specularmap": "specular",
    "specularmask": "specular",
}
# the material keys of the stock GDTs and what an image bound to them is (the fallback for a name that
# has no image entry of its own): detailMap holds the detail normals
MATERIAL_KEY_ROLE = {
    "normalMap": "normal",
    "detailMap": "normal",
    "colorMap": "colour",
    "colorMap00": "colour",
    "colorMap01": "colour",
    "colorMap02": "colour",
    "colorMap03": "colour",
    "colorMap04": "colour",
    "colorMap05": "colour",
    "colorMap06": "colour",
    "cosinePowerMap": "gloss",
    "occMap": "occlusion",
    "specColorMap": "specular",
}
ROLES = ("normal", "colour", "gloss", "occlusion", "specular")
CHANNELS = {
    "normal": ("x", "y", "z", "ceil"),
    "colour": ("r", "g", "b", "a", "opq"),
    "gloss": ("v",),
    "occlusion": ("v",),
    "specular": ("r", "g", "b"),
}
ENTRY = re.compile(r'^\t"([^"]+)" \( "image\.gdf" \)\n\t\{\n(.*?)^\t\}', re.M | re.S)
PROP = re.compile(r'^\t\t"([^"]+)" "([^"]*)"$', re.M)


def parse_gdts(root, dirs):
    """image entries {name: (semantic, base image path)} and material votes {name: Counter(role)}"""
    entries = {}
    votes = defaultdict(Counter)
    files = 0
    for top in dirs:
        base = os.path.join(root, top)
        for folder, _, names in os.walk(base):
            for n in names:
                if not n.lower().endswith(".gdt"):
                    continue
                files += 1
                with open(os.path.join(folder, n), encoding="utf-8", errors="replace", newline="") as f:
                    text = f.read().replace("\r", "")
                for m in ENTRY.finditer(text):
                    props = dict(PROP.findall(m.group(2)))
                    entries[m.group(1).lower()] = (props.get("semantic", ""), props.get("baseImage", ""))
                for key, value in PROP.findall(text):
                    role = MATERIAL_KEY_ROLE.get(key)
                    if role and value and value != "$":
                        votes[value.lower()][role] += 1
    return entries, votes, files


def index_sources(root, dirs):
    """lower-case file stem -> path of a source image under the Mod Tools root"""
    index = {}
    for top in dirs:
        for folder, _, names in os.walk(os.path.join(root, top)):
            for n in names:
                stem, ext = os.path.splitext(n)
                if ext.lower() in IMAGE_EXTS:
                    index.setdefault(stem.lower(), os.path.join(folder, n))
    return index


def load(path):
    """float32 array (h, w, 4) in 0..1 and whether the file has a real alpha"""
    img = Image.open(path)
    img.load()
    if img.mode in ("I;16", "I;16L", "I;16B", "I"):
        a = np.asarray(img, dtype=np.float32) / 65535.0
        a = np.repeat(a[..., None], 3, axis=2)
        return np.dstack([a, np.ones(a.shape[:2], np.float32)]), False
    if img.mode in ("L", "1", "P") and "transparency" not in img.info:
        img = img.convert("L")
        a = np.asarray(img, dtype=np.float32) / 255.0
        a = np.repeat(a[..., None], 3, axis=2)
        return np.dstack([a, np.ones(a.shape[:2], np.float32)]), False
    has_alpha = "A" in img.getbands() or "transparency" in img.info
    a = np.asarray(img.convert("RGBA"), dtype=np.float32) / 255.0
    return a, has_alpha and float(a[..., 3].min()) < 0.999


def shrink(path, size):
    """the stock source at the shipped size; None when it is smaller than the shipped image"""
    img = Image.open(path)
    img.load()
    if img.size[0] < size[0] or img.size[1] < size[1]:
        return None, img.size
    if img.mode not in ("I;16", "I;16L", "I;16B", "I", "L", "1", "RGBA", "RGB"):
        img = img.convert("RGBA")
    if img.mode.startswith("I"):
        arr = np.asarray(img, dtype=np.float32) / 65535.0
        img = Image.fromarray(arr, mode="F")
    out = img.resize(size, Image.BOX) if img.size != size else img
    return out, img.size


def to_array(img):
    if img.mode == "F":
        a = np.asarray(img, dtype=np.float32)
        a = np.repeat(a[..., None], 3, axis=2)
        return np.dstack([a, np.ones(a.shape[:2], np.float32)]), False
    if img.mode in ("L", "1"):
        a = np.asarray(img.convert("L"), dtype=np.float32) / 255.0
        a = np.repeat(a[..., None], 3, axis=2)
        return np.dstack([a, np.ones(a.shape[:2], np.float32)]), False
    a = np.asarray(img.convert("RGBA"), dtype=np.float32) / 255.0
    return a, float(a[..., 3].min()) < 0.999


def psnr(a, b):
    m = float(((a - b) ** 2).mean())
    return 99.0 if m == 0 else 10.0 * np.log10(1.0 / m)


def corr(a, b):
    a, b = a.ravel() - a.mean(), b.ravel() - b.mean()
    d = float(np.sqrt((a * a).sum() * (b * b).sum()))
    return float("nan") if d == 0 else float((a * b).sum() / d)   # a flat image has no correlation


def normal_z(r, g):
    x, y = r * 2 - 1, g * 2 - 1
    return np.sqrt(np.clip(1 - x * x - y * y, 0, 1)) * 0.5 + 0.5


def score(role, s, o, s_alpha, o_alpha):
    """per channel PSNR of shipped s against stock o (both h, w, 4), and the correlation over the colour channels"""
    if role == "normal":
        z_ceiling = psnr(normal_z(o[..., 0], o[..., 1]), o[..., 2])
        ch = [psnr(s[..., 0], o[..., 0]), psnr(s[..., 1], o[..., 1]), psnr(s[..., 2], o[..., 2]), z_ceiling]
        return ch, corr(s[..., :2], o[..., :2])
    if role in ("gloss", "occlusion"):
        return [psnr(s[..., 0], o[..., 0])], corr(s[..., 0], o[..., 0])
    ch = [psnr(s[..., i], o[..., i]) for i in range(3)]
    if role == "colour":
        ch.append(psnr(s[..., 3], o[..., 3]) if (s_alpha and o_alpha) else float("nan"))
        opaque = s[..., 3] >= 0.95
        ch.append(psnr(s[..., :3][opaque], o[..., :3][opaque]) if s_alpha and opaque.any() else float("nan"))
    return ch, corr(s[..., :3], o[..., :3])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("export", help="folder of exported images (png, tif, tga)")
    ap.add_argument("modtools", help="Black Ops III Mod Tools root (texture_assets, model_export ...)")
    ap.add_argument("--worst", type=int, default=15, help="worst pairs listed per role (15)")
    ap.add_argument("--roles", help="FILE of image<TAB>role lines: roles from a list, taking precedence over the GDTs")
    ap.add_argument("--limit", type=int, default=0, help="score at most this many pairs (0: all)")
    ap.add_argument("--keep-copies", action="store_true", help="count sources identical to the shipped image (80 dB on every channel) in the role medians")
    ap.add_argument("--rows", help="FILE: every scored pair as a TSV row (image, role, via, sizes, correlation, dB per channel)")
    args = ap.parse_args()

    gdt_dirs = [d for d in ("texture_assets", "model_export", "source_data") if os.path.isdir(os.path.join(args.modtools, d))]
    src_dirs = [d for d in ("texture_assets", "model_export", "art_assets") if os.path.isdir(os.path.join(args.modtools, d))]
    if not gdt_dirs or not src_dirs:
        print("no texture_assets / model_export under", args.modtools)
        return 2
    entries, votes, nfiles = parse_gdts(args.modtools, gdt_dirs)
    sources = index_sources(args.modtools, src_dirs)
    listed = {}
    if args.roles:
        for line in open(args.roles, encoding="utf-8"):
            parts = line.rstrip("\r\n").split("\t")
            if len(parts) >= 2 and parts[1] in ROLES:
                listed[parts[0].lower()] = parts[1]
    print("gdt files %d, image entries %d, names bound by materials %d, source images %d" % (nfiles, len(entries), len(votes), len(sources)))

    rows = defaultdict(list)       # role -> [(name, shipped size, stock size, channels, corr, via, source folder)]
    copies = defaultdict(list)     # the same for sources identical to the shipped image
    skipped = Counter()
    names = sorted(f for f in os.listdir(args.export) if f.lower().endswith(IMAGE_EXTS))
    pairs = 0
    for f in names:
        stem = os.path.splitext(f)[0].lower()
        keys = [stem] + ([stem[2:]] if stem.startswith("i_") else ["i_" + stem])
        if stem.startswith("$"):
            skipped["engine image"] += 1
            continue
        entry = next((entries[k] for k in keys if k in entries), None)
        src = None
        if entry and entry[1]:
            p = os.path.join(args.modtools, *entry[1].replace("\\", "/").split("/"))
            if os.path.isfile(p):
                src = p
        if src is None:
            src = next((sources[k] for k in keys if k in sources), None)
        if src is None:
            skipped["no stock source of that name"] += 1
            continue
        role, via = None, ""
        for k in keys:
            if k in listed:
                role, via = listed[k], "list"
                break
        if role is None and entry and entry[0]:
            role, via = SEMANTIC_ROLE.get(entry[0].lower()), "gdt"
            if role is None:
                skipped["semantic %s" % entry[0]] += 1
                continue
        if role is None:
            v = next((votes[k] for k in keys if k in votes), None)
            if v:
                role, via = v.most_common(1)[0][0], "material"
        if role is None:
            skipped["no known role"] += 1
            continue
        try:
            shipped, s_alpha = load(os.path.join(args.export, f))
            small, stock_size = shrink(src, (shipped.shape[1], shipped.shape[0]))
            if small is None:
                skipped["stock source smaller than shipped"] += 1
                continue
            stock, o_alpha = to_array(small)
        except Exception as e:  # an unreadable file is a result here, counted and named
            skipped["unreadable (%s)" % type(e).__name__] += 1
            continue
        ch, c = score(role, shipped, stock, s_alpha, o_alpha)
        folder = "/".join(os.path.relpath(src, args.modtools).replace("\\", "/").split("/")[:2])
        pair = (f, "%dx%d" % (shipped.shape[1], shipped.shape[0]), "%dx%d" % stock_size, ch, c, via, folder)
        measured = [x for i, x in enumerate(ch) if not np.isnan(x) and CHANNELS[role][i] != "ceil"]
        if measured and min(measured) >= 80.0 and not args.keep_copies:
            copies[role].append(pair)
        else:
            rows[role].append(pair)
        pairs += 1
        if args.limit and pairs >= args.limit:
            break

    if args.rows:
        with open(args.rows, "w", encoding="utf-8") as out:
            out.write("\t".join(["image", "role", "via", "shipped", "stock", "corr", "channels_db", "source", "copy_of_shipped"]) + "\n")
            for kind, table in (("0", rows), ("1", copies)):
                for role, rs in table.items():
                    for f, ss, os_, ch, c, v, d in rs:
                        out.write("\t".join([f, role, v, ss, os_, "%.4f" % c, ",".join("%.2f" % x for x in ch), d, kind]) + "\n")
    print("images in folder %d, pairs scored %d" % (len(names), pairs))
    for k, v in sorted(skipped.items()):
        print("  skipped %-40s %d" % (k, v))
    nc = sum(len(v) for v in copies.values())
    if nc:
        folders = Counter(x[6] for v in copies.values() for x in v)
        print("  %d pairs whose source equals the shipped image (80 dB on every channel): a copy of an export in the tree, not an independent"
              " source; left out of the medians. Folders: %s" % (nc, ", ".join("%s %d" % kv for kv in folders.most_common(4))))
        for role in ROLES:
            c = copies.get(role)
            if c and role == "normal":
                print("    their normals' own-Z ceiling, median %.1f dB (stock Z against the Z its own x and y imply)" % float(np.median([x[3][3] for x in c])))
    for role in ROLES:
        r = rows.get(role, [])
        if not r:
            continue
        cols = CHANNELS[role]
        arr = np.array([x[3] for x in r], dtype=np.float64)
        med = ["%s %.1f" % (c, float(np.nanmedian(arr[:, i])) if not np.all(np.isnan(arr[:, i])) else float("nan")) for i, c in enumerate(cols)]
        via = Counter(x[5] for x in r)
        print("\n== %s: %d pairs (%s)   median dB: %s   median corr %.3f" % (
            role, len(r), ", ".join("%s %d" % kv for kv in sorted(via.items())), "  ".join(med), float(np.nanmedian([x[4] for x in r])) if not np.all(np.isnan([x[4] for x in r])) else float("nan")))
        used = [i for i, c in enumerate(cols) if c not in ("ceil", "opq")]
        worst = sorted(r, key=lambda x: float(np.nanmean([x[3][i] for i in used])))[: args.worst]
        print("   worst %d by mean dB over %s:" % (len(worst), "/".join(cols[i] for i in used)))
        print("   %-44s %-9s %-9s %s %5s  %s" % ("image", "shipped", "stock", " ".join("%6s" % c for c in cols), "corr", "via, source folder"))
        for f, ss, os_, ch, c, v, d in worst:
            print("   %-44s %-9s %-9s %s %5.2f  %s, %s" % (f[:44], ss, os_, " ".join("%6.1f" % x for x in ch), c, v, d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
