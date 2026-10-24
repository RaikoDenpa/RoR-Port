"""Disc data coverage: classify every byte of every disc file, descending into RTPK archives.

Usage:
  python coverage.py <extracted_disc_dir> --dump [--leaves <out.tsv>]

Tiers:
  parsed      structure decoded by our tools (RTPK headers/padding via rtpk.py, now-loading image via ldimg.py)
  standard    a documented public format (MPEG-PS movies, TIM2, ELF/IRX, IOPRP romdir, PS2 icon, text)
  identified  recognised by magic/name, but the game/middleware format is not reversed yet
  unknown     no recognisable signature
--leaves writes one row per leaf (path, offset, size, format, tier). Output must go to an ignored folder.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rtpk  # noqa: E402

# magic prefix -> (format, tier)
MAGIC = [
    (b'\x00\x00\x01\xba', 'PSS movie (MPEG-2 PS)', 'standard'),
    (b'TIM2', 'TIM2 texture', 'standard'),
    (b'\x7fELF', 'ELF / IRX', 'standard'),
    (b'RESET', 'IOPRP image (romdir)', 'standard'),
    (b'\x00\x00\x01\x00', 'PS2 memory card icon', 'standard'),
    (b'I3D_', 'I3D model/scene data', 'identified'),
    (b'ELFR', 'ELFR', 'identified'),
    (b'XVAB', 'XVAB sound bank', 'identified'),
    (b'CHAR', 'CHAR (event scene)', 'identified'),
    (b'UNQ_', 'UNQ_ (uniq.bin)', 'identified'),
    (b'DOBJ', 'DOBJ (door objects)', 'identified'),
    (b'RTFT', 'RTFT font', 'identified'),
    (b'cursor_', 'XV sound name table', 'identified'),
    (b'M00\x00', 'XWS stream table', 'identified'),
    (b'dumy', 'dummy placeholder', 'identified'),
]


def classify(head, path, size):
    for m, fmt, tier in MAGIC:
        if head.startswith(m):
            return fmt, tier
    name = path.replace('\\', '/').rsplit('/', 1)[-1].upper()
    if name.endswith('.STR') and head[:4] in (b'\x0c\x00\x00\x00', b'\x0c\x02\x00\x00'):
        return 'STR sound stream', 'identified'
    if name.endswith(('.CNF', '.INI', '.TXT')):
        return 'text config', 'standard'
    if name in ('LOADING.BIN', 'LOAD_EU.BIN') and size >= 0x10400:
        return 'now-loading image', 'parsed'
    if head[:16] == b'\xcc' * 16 or head[:16] == b'\0' * 16 and size <= 64:
        return 'filler', 'identified'
    return None, 'unknown'


def group_key(path):
    """Group unknown leaves: drop the disc dir, replace digit runs with #."""
    p = path.replace('\\', '/')
    parts = p.split('/')
    top = '/'.join(parts[:3]) if len(parts) > 2 else p
    leaf = parts[-1]
    ext = os.path.splitext(leaf)[1].lower()
    tail = re.sub(r'\d+', '#', ext if ext else leaf)
    return '%s ... %s' % (re.sub(r'\d+', '#', top), tail)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('disc_dir')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--leaves', metavar='OUT_TSV')
    a = ap.parse_args()

    tiers = collections.Counter()
    fmts = collections.Counter()
    fmt_n = collections.Counter()
    unknown = collections.defaultdict(lambda: [0, 0, None])
    rows = []

    def leaf(buf, off, size, path):
        if size == 0:  # empty archive slot: no bytes, and its "head" would be the next entry's data
            fmt_n[('parsed', 'empty entries')] += 1
            fmts[('parsed', 'empty entries')] += 0
            return
        head = bytes(buf[off:off + 16])
        fmt, tier = classify(head, path, size)
        fmt = fmt or 'unknown'
        tiers[tier] += size
        fmts[(tier, fmt)] += size
        fmt_n[(tier, fmt)] += 1
        if tier == 'unknown':
            g = unknown[group_key(path)]
            g[0] += size; g[1] += 1; g[2] = g[2] or (path, head[:8].hex())
        if a.leaves:
            rows.append((path, off, size, fmt, tier))

    def walk(buf, base, path):
        arc = rtpk.Archive(buf, base)
        overhead = arc.total - sum(e.size for e in arc.entries)
        tiers['parsed'] += overhead
        fmts[('parsed', 'RTPK header/padding')] += overhead
        fmt_n[('parsed', 'RTPK header/padding')] += 1
        for e in arc.entries:
            p = '%s/%s' % (path, e.name or '%04d' % e.index)
            if arc.is_nested(e):
                child_total = walk(buf, base + e.offset, p)
                slack = e.size - child_total  # a nested archive may be shorter than its slot
                tiers['parsed'] += slack
                fmts[('parsed', 'RTPK header/padding')] += slack
            else:
                leaf(buf, base + e.offset, e.size, p)
        return arc.total

    total = 0
    for d, _, fs in os.walk(a.disc_dir):
        for f in sorted(fs):
            full = os.path.join(d, f)
            rel = os.path.relpath(full, a.disc_dir).replace('\\', '/')
            size = os.path.getsize(full)
            total += size
            if size == 0:
                continue
            buf = rtpk.open_file(full)
            if bytes(buf[:4]) == rtpk.MAGIC:
                walk(buf, 0, rel)
            else:
                leaf(buf, 0, size, rel)

    if a.dump:
        print('disc file bytes: %d' % total)
        for t in ('parsed', 'standard', 'identified', 'unknown'):
            print('  %-11s %13d  %6.2f%%' % (t, tiers[t], 100.0 * tiers[t] / total))
        print('\nby format:')
        for (t, fmt), s in sorted(fmts.items(), key=lambda kv: -kv[1]):
            print('  %-11s %-28s %13d  %6.2f%%  n=%d' % (t, fmt, s, 100.0 * s / total, fmt_n[(t, fmt)]))
        print('\nunknown groups (largest first):')
        for k, (s, n, ex) in sorted(unknown.items(), key=lambda kv: -kv[1][0])[:40]:
            print('  %12d  %6.3f%%  n=%-5d %s   e.g. %s [%s]' % (s, 100.0 * s / total, n, k, ex[0], ex[1]))
        print('  (%d unknown groups)' % len(unknown))
    if a.leaves:
        with open(a.leaves, 'w', encoding='utf-8') as o:
            o.write('path\toffset\tsize\tformat\ttier\n')
            for r in rows:
                o.write('%s\t%d\t%d\t%s\t%s\n' % r)
        print('wrote %d leaf rows to %s' % (len(rows), a.leaves))


if __name__ == '__main__':
    main()
