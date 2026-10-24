"""RTPK containers (the game's "PFH" archives: .RPK, .WRD, .GRB, some .BIN), read by bed/bedpfh.cpp.

Usage:
  python rtpk.py <archive> --dump [-r]                 list entries (offset, size, name); -r descends into nested RTPKs
  python rtpk.py <archive> --extract <out_dir> [-r]    write entries; nested archives go to <name>.d/ with -r

Layout (all little-endian; see notes/formats/pfh.md):
  0x00 'RTPK'   0x04 u32 total size   0x08 u16 0   0x0a u16 flags   0x0c u16 alignment   0x0e u16 count
  0x10 u32 name-table size   0x14..0x1f zero
  then, in this order, only if the flag is set:
    bit 0       u32 size[count]
    bit 1       u32 offset[count]          (absolute, from the archive start)
    bit 4+k     u16 table_k[count]         (one table per consecutive set bit from bit 4; unused on this disc)
    bit 15      count NUL-terminated names
  bit 14 marks archives whose names carry no extension.
  Without the offset table, entry i starts at the aligned end of entry i-1 (first at the aligned header end);
  without the size table, entry i ends where entry i+1 starts (the last at the total size).
"""
import argparse
import mmap
import os
import posixpath
import struct
import sys

MAGIC = b'RTPK'
F_SIZES, F_OFFSETS, F_NOEXT, F_NAMES = 0x0001, 0x0002, 0x4000, 0x8000


def align(x, a):
    return (x + a - 1) // a * a if a > 1 else x


class Entry:
    __slots__ = ('index', 'name', 'offset', 'size', 'extra')

    def __init__(self, index, name, offset, size, extra):
        self.index, self.name, self.offset, self.size, self.extra = index, name, offset, size, extra


class Archive:
    """Parsed RTPK header over a bytes-like buffer (bytes, memoryview or mmap) starting at `base`."""

    def __init__(self, buf, base=0):
        if bytes(buf[base:base + 4]) != MAGIC:
            raise ValueError('not an RTPK archive')
        self.buf, self.base = buf, base
        (self.total, self.reserved, self.flags, self.align, self.count,
         self.names_size) = struct.unpack_from('<IHHHHI', buf, base + 4)
        n, pos = self.count, base + 0x20
        sizes = offsets = None
        if self.flags & F_SIZES:
            sizes = list(struct.unpack_from('<%dI' % n, buf, pos)); pos += 4 * n
        if self.flags & F_OFFSETS:
            offsets = list(struct.unpack_from('<%dI' % n, buf, pos)); pos += 4 * n
        self.u16_tables = []
        k = 0
        while self.flags & (0x10 << k):
            self.u16_tables.append(struct.unpack_from('<%dH' % n, buf, pos)); pos += 2 * n
            k += 1
        names = [None] * n
        if self.flags & F_NAMES:
            q = pos
            for i in range(n):
                e = buf.find(b'\0', q)
                names[i] = bytes(buf[q:e]).decode('ascii', 'replace')
                q = e + 1
            pos = q
        self.header_end = pos - base
        if offsets is None:
            offsets, o = [], align(self.header_end, self.align)
            for s in sizes:
                offsets.append(o)
                o = align(o + s, self.align)
        if sizes is None:
            sizes = [offsets[i + 1] - offsets[i] for i in range(n - 1)] + [self.total - offsets[-1]] if n else []
        self.entries = [Entry(i, names[i], offsets[i], sizes[i], tuple(t[i] for t in self.u16_tables)) for i in range(n)]

    def data(self, e):
        a = self.base + e.offset
        return self.buf[a:a + e.size]

    def is_nested(self, e):
        return e.size >= 0x20 and bytes(self.buf[self.base + e.offset:self.base + e.offset + 4]) == MAGIC

    def validate(self):
        """Return a list of problems (empty when the header is consistent with the data)."""
        p = []
        if self.reserved:
            p.append('reserved u16 at +8 is %#x' % self.reserved)
        ents = self.entries
        if ents and ents[0].offset != align(self.header_end, self.align):
            p.append('first offset %#x != aligned header end %#x' % (ents[0].offset, align(self.header_end, self.align)))
        for a, b in zip(ents, ents[1:]):
            if b.offset < a.offset + a.size or b.offset - (a.offset + a.size) >= max(self.align, 1):
                p.append('entry %d..%d gap/overlap' % (a.index, b.index))
                break
        if ents and ents[-1].offset + ents[-1].size > self.total:
            p.append('last entry overflows total size')
        names_bytes = sum(len(e.name) + 1 for e in ents) if self.flags & F_NAMES else 0
        if names_bytes != self.names_size:
            p.append('name-table size %#x != %#x' % (self.names_size, names_bytes))
        return p


def open_file(path):
    f = open(path, 'rb')
    return mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)


def entry_name(e, arc):
    return e.name if e.name is not None else '%04d' % e.index


def safe_relpath(name):
    """Entry names may contain '..' (e.g. 'ev110c/ev110c/../camera/x.bin'): resolve them, never leave the out dir."""
    p = posixpath.normpath(name.replace('\\', '/').lstrip('/'))
    parts = [s for s in p.split('/') if s not in ('', '.', '..')]
    return parts or ['%s' % name.replace('/', '_')]


def dump(arc, recursive, indent=''):
    print('%sRTPK flags=%04x align=%d count=%d total=%#x header=%#x%s' % (
        indent, arc.flags, arc.align, arc.count, arc.total, arc.header_end,
        '' if not arc.validate() else '  PROBLEMS: ' + '; '.join(arc.validate())))
    for e in arc.entries:
        nested = arc.is_nested(e)
        print('%s  %4d %10x %10d  %s%s' % (indent, e.index, e.offset, e.size, entry_name(e, arc), '  [RTPK]' if nested else ''))
        if recursive and nested:
            dump(Archive(arc.buf, arc.base + e.offset), True, indent + '    ')


def extract(arc, out, recursive, seen=None):
    """Write entries below out; returns the number of files written. Names that resolve to an already written
    path (e.g. several '../../dummy.dat') get '.<index>' appended so nothing is overwritten."""
    seen = set() if seen is None else seen
    os.makedirs(out, exist_ok=True)
    n = 0
    for e in arc.entries:
        dest = os.path.join(out, *safe_relpath(entry_name(e, arc)))
        if os.path.normcase(dest) in seen:
            dest = '%s.%d' % (dest, e.index)
        seen.add(os.path.normcase(dest))
        os.makedirs(os.path.dirname(dest) or out, exist_ok=True)
        if recursive and arc.is_nested(e):
            n += extract(Archive(arc.buf, arc.base + e.offset), dest + '.d', True, seen)
            continue
        with open(dest, 'wb') as o:
            o.write(arc.data(e))
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('archive')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--extract', metavar='OUT_DIR')
    ap.add_argument('-r', '--recursive', action='store_true')
    a = ap.parse_args()
    buf = open_file(a.archive)
    arc = Archive(buf)
    if a.dump:
        dump(arc, a.recursive)
    if a.extract:
        print('wrote %d files to %s' % (extract(arc, a.extract, a.recursive), a.extract))
    if not (a.dump or a.extract):
        ap.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
