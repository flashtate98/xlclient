"""Write a small Data/Retail/listfile.csv for the Retail files you actually ship.

The client resolves the FileDataIDs inside Retail models (textures, skins, .anim, .skel) through
"<FileDataID>;<path>" rows. The full community listfile works but holds millions of rows; this
keeps only the rows whose file exists in your export folder (for example a wow.export output that
you pack into a patch MPQ), so the client loads it instantly.

    python make_listfile.py --listfile community-listfile.csv --export C:/exports/wow --out <client>/Data/Retail/listfile.csv
"""
import argparse
import os
import sys


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--listfile', required=True, help='community listfile (FileDataID;path rows)')
    ap.add_argument('--export', required=True, help='folder holding the exported files, laid out by their listfile paths')
    ap.add_argument('--out', required=True, help='where to write the trimmed listfile')
    args = ap.parse_args(argv)

    present = set()
    for folder, _, files in os.walk(args.export):
        for name in files:
            rel = os.path.relpath(os.path.join(folder, name), args.export)
            present.add(rel.replace('\\', '/').lower())
    if not present:
        print('no files under %s' % args.export, file=sys.stderr)
        return 2

    kept = 0
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.listfile, encoding='utf-8', errors='replace') as src, open(args.out, 'w', encoding='utf-8', newline='\n') as dst:
        for line in src:
            line = line.strip()
            sep = line.find(';')
            if sep <= 0:
                continue
            path = line[sep + 1:].replace('\\', '/').lower()
            if path in present:
                dst.write('%s;%s\n' % (line[:sep], path))
                kept += 1
    print('%d of %d exported files have a listfile row -> %s' % (kept, len(present), args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
