"""List or extract the files of a PS2 ISO9660 disc image.

Usage:
  python iso9660.py <image.iso> --dump                 list every file: LBA, size, path
  python iso9660.py <image.iso> --extract <out_dir>    copy every file out (out_dir must not be a tracked path)

Only plain ISO9660 (no Joliet/UDF needed for PS2 discs). Version suffixes (";1") are stripped on extract.
"""
import argparse
import os
import struct
import sys

SECTOR = 2048


def read_sectors(f, lba, count):
    f.seek(lba * SECTOR)
    return f.read(count * SECTOR)


def walk(f, lba, size, prefix=''):
    """Yield (path, lba, size, is_dir) for every record below the directory at lba."""
    data = read_sectors(f, lba, (size + SECTOR - 1) // SECTOR)
    i = 0
    while i < len(data):
        rec_len = data[i]
        if rec_len == 0:  # records never cross a sector boundary; skip padding
            i = (i // SECTOR + 1) * SECTOR
            continue
        e_lba = struct.unpack_from('<I', data, i + 2)[0]
        e_size = struct.unpack_from('<I', data, i + 10)[0]
        flags = data[i + 25]
        name_len = data[i + 32]
        name = data[i + 33:i + 33 + name_len]
        i += rec_len
        if name in (b'\0', b'\1'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0]
        path = prefix + '/' + name
        if flags & 2:
            yield path, e_lba, e_size, True
            yield from walk(f, e_lba, e_size, path)
        else:
            yield path, e_lba, e_size, False


def entries(image):
    f = open(image, 'rb')
    pvd = read_sectors(f, 16, 1)
    if pvd[1:6] != b'CD001':
        sys.exit('not an ISO9660 image')
    root = pvd[156:190]
    lba, size = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
    return f, list(walk(f, lba, size))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('image')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--extract', metavar='OUT_DIR')
    a = ap.parse_args()
    f, ents = entries(a.image)
    if a.dump:
        for path, lba, size, is_dir in ents:
            print('%8d %11s %s%s' % (lba, '<dir>' if is_dir else size, path, '/' if is_dir else ''))
    if a.extract:
        for path, lba, size, is_dir in ents:
            out = os.path.join(a.extract, path.lstrip('/'))
            if is_dir:
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            f.seek(lba * SECTOR)
            with open(out, 'wb') as o:
                left = size
                while left:
                    chunk = f.read(min(left, 1 << 20))
                    o.write(chunk)
                    left -= len(chunk)
        print('extracted %d files to %s' % (sum(1 for e in ents if not e[3]), a.extract))
    if not (a.dump or a.extract):
        ap.print_help()


if __name__ == '__main__':
    main()
