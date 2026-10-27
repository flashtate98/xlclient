"""Shows the texture parameters of retail split ADTs (*_tex0.adt): per texture its MTXF flags (texture
scale in bits 4-7) and its MTXP height values, as rows for the client's TerrainTextureParams table.

    python tex0_params.py <file or folder>... [--listfile listfile.csv] [--csv out.csv] [--all]

- Textures are named by MTEX (MoP to Legion) or by FileDataID (MDID, BfA on); --listfile ("id;path"
  per line, e.g. wow.export's or MultiConverter's) turns the IDs into paths. It is read once, keeping
  only the IDs the files use.
- Only textures with something to carry over are listed (a texture scale, a flag, height values other
  than 0 / 1); --all lists every texture.
- --csv writes Scope;Texture;MTXF;MTXFMask;HeightScale;HeightOffset: one global row (empty Scope) per
  texture with the values most tiles agree on, and a row for each tile that differs (Scope the tile's
  name, "Kalimdor_23_30" from "Kalimdor_23_30_tex0.adt"); --per-tile writes a row per tile and texture.
  MTXFMask takes the "no specular" bit and the scale bits, so a row sets exactly what retail has.
- --cdbc merges the same rows into a TerrainTextureParams.cdbc (the client's DBFilesClient folder; it
  reloads the loose file): a row for the same Scope and texture is updated, others are added.
"""
import argparse
import csv
import os
import struct
import sys

SCALE_BITS = 0xF0


def chunks(data):
    """(magic, payload) of each top-level chunk; magics are stored reversed on disk."""
    pos = 0
    while pos + 8 <= len(data):
        magic = data[pos:pos + 4][::-1].decode('ascii', 'replace')
        size = struct.unpack_from('<I', data, pos + 4)[0]
        yield magic, data[pos + 8:pos + 8 + size]
        pos += 8 + size


def read_tex0(path):
    with open(path, 'rb') as f:
        data = f.read()
    names, ids, heights, flags, params = [], [], [], [], []
    for magic, body in chunks(data):
        if magic == 'MTEX':
            names = [n.decode('latin-1') for n in body.split(b'\0') if n]
        elif magic == 'MDID':
            ids = list(struct.unpack('<%dI' % (len(body) // 4), body[:len(body) // 4 * 4]))
        elif magic == 'MHID':
            heights = list(struct.unpack('<%dI' % (len(body) // 4), body[:len(body) // 4 * 4]))
        elif magic == 'MTXF':
            flags = list(struct.unpack('<%dI' % (len(body) // 4), body[:len(body) // 4 * 4]))
        elif magic == 'MTXP':
            params = [struct.unpack_from('<Iff', body, i * 16) for i in range(len(body) // 16)]
    count = max(len(names), len(ids), len(flags), len(params))
    textures = []
    for i in range(count):
        p = params[i] if i < len(params) else None
        textures.append({
            'name': names[i] if i < len(names) else None,
            'id': ids[i] if i < len(ids) else None,
            'height_id': heights[i] if i < len(heights) else None,
            # MTXP's flags are MTXF's (taken from there when no MTXP)
            'mtxf': p[0] if p else (flags[i] if i < len(flags) else 0),
            'height_scale': p[1] if p else 0.0,
            'height_offset': p[2] if p else 1.0,
        })
    return textures


def tile_name(path):
    base = os.path.basename(path)
    return base[:-len('_tex0.adt')] if base.lower().endswith('_tex0.adt') else os.path.splitext(base)[0]


def gather(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                out += [os.path.join(root, f) for f in sorted(files) if f.lower().endswith('_tex0.adt')]
        else:
            out.append(p)
    return out


def load_listfile(path, wanted):
    names = {}
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            sep = line.find(';')
            if sep <= 0:
                continue
            try:
                fid = int(line[:sep])
            except ValueError:
                continue
            if fid in wanted:
                names[fid] = line[sep + 1:].strip()
    return names


def collapse(rows):
    """One global row (empty Scope) per texture with the values most tiles agree on; a tile row only
    where a tile differs from it."""
    by_texture = {}
    for scope, name, *values in rows:
        by_texture.setdefault(name.lower(), (name, []))[1].append((scope, tuple(values)))
    out = []
    for name, tiles in by_texture.values():
        counts = {}
        for _, values in tiles:
            counts[values] = counts.get(values, 0) + 1
        common = max(counts, key=counts.get)
        out.append(['', name, *common])
        out += [[scope, name, *values] for scope, values in tiles if values != common]
    return out


# TerrainTextureParams' columns (SXC/Terrain/TextureParams.cpp): ID, Scope, Texture, MTXF, MTXFMask,
# HeightScale, HeightOffset, Comment, NormalStrength, ParallaxDepth, ParallaxSmooth (the last three
# appended later: an older file reads with 0 there).
CDBC_FIELDS = 11
CDBC_OLDER_FIELDS = (8, 9, 10)
CDBC_STRINGS = (1, 2, 7)
CDBC_FLOATS = (5, 6, 8, 9, 10)


def read_cdbc(path):
    """The rows of a TerrainTextureParams.cdbc as lists of Python values; [] when there is no file."""
    if not os.path.exists(path):
        return []
    with open(path, 'rb') as f:
        data = f.read()
    magic, count, fields, size, string_size = struct.unpack_from('<4s4I', data, 0)
    if magic != b'WDBC' or fields not in (CDBC_FIELDS, *CDBC_OLDER_FIELDS) or size != 4 * fields:
        sys.exit('%s: not a TerrainTextureParams.cdbc of %d columns; left as it is' % (path, CDBC_FIELDS))
    strings = data[20 + count * size:20 + count * size + string_size]
    rows = []
    for r in range(count):
        raw = struct.unpack_from('<%dI' % fields, data, 20 + r * size)
        row = []
        for i, v in enumerate(raw):
            if i in CDBC_STRINGS:
                end = strings.find(b'\0', v)
                row.append(strings[v:end].decode('utf-8', 'replace'))
            elif i in CDBC_FLOATS:
                row.append(struct.unpack('<f', struct.pack('<I', v))[0])
            else:
                row.append(v)
        row += [0.0] * (CDBC_FIELDS - fields)  # the columns an older file lacks (floats, 0)
        rows.append(row)
    return rows


def write_cdbc(path, rows):
    rows = sorted(rows, key=lambda r: r[0])
    block, offsets, records = bytearray(b'\0'), {'': 0}, bytearray()
    for row in rows:
        for i, v in enumerate(row):
            if i in CDBC_STRINGS:
                if v not in offsets:
                    offsets[v] = len(block)
                    block += v.encode('utf-8') + b'\0'
                records += struct.pack('<I', offsets[v])
            elif i in CDBC_FLOATS:
                records += struct.pack('<f', v)
            else:
                records += struct.pack('<I', v)
    with open(path, 'wb') as f:
        f.write(struct.pack('<4s4I', b'WDBC', len(rows), CDBC_FIELDS, 4 * CDBC_FIELDS, len(block)))
        f.write(records)
        f.write(block)


def merge_cdbc(path, rows, source):
    """Adds the rows to the table; a row for the same Scope and texture is updated instead."""
    existing = read_cdbc(path)
    index = {(r[1].lower(), r[2].lower().replace('/', '\\')): r for r in existing}
    next_id = max((r[0] for r in existing), default=0) + 1
    added = updated = 0
    for scope, name, mtxf, mask, hs, ho in rows:
        key = (scope.lower(), name.lower().replace('/', '\\'))
        if key in index:
            index[key][3:7] = [mtxf, mask, hs, ho]
            updated += 1
        else:
            row = [next_id, scope, name, mtxf, mask, hs, ho, 'from ' + source, 0.0, 0.0, 0.0]
            existing.append(row)
            index[key] = row
            next_id += 1
            added += 1
    write_cdbc(path, existing)
    print('%s: %d added, %d updated' % (path, added, updated))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='+', help='_tex0.adt files or folders holding them')
    ap.add_argument('--listfile', help='"id;path" lines, for FileDataIDs (MDID)')
    ap.add_argument('--csv', help='write the listed rows here')
    ap.add_argument('--all', action='store_true', help='list every texture, not only those with values')
    ap.add_argument('--per-tile', action='store_true', help='a row per tile and texture, not one global row per texture')
    ap.add_argument('--cdbc', help='merge the rows into this TerrainTextureParams.cdbc (the client reloads it)')
    args = ap.parse_args()

    files = gather(args.paths)
    if not files:
        sys.exit('no _tex0.adt files found')
    tiles = [(f, read_tex0(f)) for f in files]

    names = {}
    if args.listfile:
        wanted = {t[k] for _, ts in tiles for t in ts for k in ('id', 'height_id') if t[k]}
        names = load_listfile(args.listfile, wanted)

    rows = []
    for path, textures in tiles:
        scope = tile_name(path)
        print('%s (%d textures)' % (scope, len(textures)))
        for t in textures:
            mtxf, hs, ho = t['mtxf'], t['height_scale'], t['height_offset']
            interesting = mtxf != 0 or hs != 0.0 or ho != 1.0
            if not (interesting or args.all):
                continue
            name = t['name'] or names.get(t['id']) or ('FileDataID %d' % t['id'] if t['id'] else '?')
            height = names.get(t['height_id']) or ('FileDataID %d' % t['height_id'] if t['height_id'] else '')
            scale = (mtxf >> 4) & 0xF
            print('  %-70s MTXF 0x%-4X%s  height %.3f / %.3f%s' % (
                name, mtxf, ('  scale x%d' % (1 << scale)) if scale else '', hs, ho, ('  _h: ' + height) if height else ''))
            rows.append([scope, name, mtxf, 0x1 | SCALE_BITS, round(hs, 4), round(ho, 4)])

    if not args.per_tile:
        rows = collapse(rows)

    if args.csv:
        with open(args.csv, 'w', newline='', encoding='utf-8') as f:
            w = csv.writer(f, delimiter=';')
            w.writerow(['Scope', 'Texture', 'MTXF', 'MTXFMask', 'HeightScale', 'HeightOffset'])
            w.writerows(rows)
        print('%d row(s) -> %s' % (len(rows), args.csv))

    if args.cdbc:
        merge_cdbc(args.cdbc, rows, os.path.basename(os.path.normpath(args.paths[0])))


if __name__ == '__main__':
    main()
