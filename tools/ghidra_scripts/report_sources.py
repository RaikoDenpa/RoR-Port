"""Format RoRSourceFiles.java reports (funcs.tsv, rodata.tsv) into the tables of notes/source-files.md.

Usage: python report_sources.py <report_dir> [--dump]
  Writes markdown to stdout: one row per source file in code order, then the unattributed gaps between them.
  --dump prints each contiguous code segment instead.
"""
import csv
import sys
from collections import Counter


def main():
    rep = sys.argv[1]
    rows = list(csv.DictReader(open(rep + '/funcs.tsv', encoding='utf-8'), delimiter='\t'))
    spans = {r['file']: (r['rodata_lo'], r['rodata_hi'])
             for r in csv.DictReader(open(rep + '/rodata.tsv', encoding='utf-8'), delimiter='\t')}
    segs = []  # [file, first_row_index, last_row_index]
    for i, r in enumerate(rows):
        f = r['file'] or None
        if segs and segs[-1][0] == f:
            segs[-1][2] = i
        else:
            segs.append([f, i, i])
    if '--dump' in sys.argv:
        for f, a, b in segs:
            print(f, rows[a]['start'], rows[b]['end'], b - a + 1)
        return

    out = ['## Files (code order)\n',
           '| # | File | Code start | Code end | Funcs | direct / run / rodata / class | rodata span | Confidence |',
           '|---|---|---|---|---|---|---|---|']
    n = 0
    for f, a, b in segs:
        if not f:
            continue
        n += 1
        m = Counter(rows[k]['method'] for k in range(a, b + 1))
        conf = 'high' if m['direct'] >= 2 else 'medium'
        lo, hi = spans.get(f, ('', ''))
        out.append('| %d | `%s` | %s | %s | %d | %d / %d / %d / %d | %s–%s | %s |' % (
            n, f, rows[a]['start'], rows[b]['end'], b - a + 1, m['direct'], m['run'], m['rodata'], m['class'], lo, hi, conf))
    out.append('')
    out.append('## Unattributed gaps\n')
    out.append('Functions between two attributed files that carry no evidence. Each gap may belong to either neighbour '
               'or to TUs without path strings (confidence: low).\n')
    out.append('| Start | End | Funcs | After | Before |')
    out.append('|---|---|---|---|---|')
    for i, (f, a, b) in enumerate(segs):
        if f:
            continue
        prev = next((s[0] for s in reversed(segs[:i]) if s[0]), '—')
        nxt = next((s[0] for s in segs[i + 1:] if s[0]), '—')
        out.append('| %s | %s | %d | `%s` | `%s` |' % (rows[a]['start'], rows[b]['end'], b - a + 1, prev, nxt))
    sys.stdout.reconfigure(encoding='utf-8')
    print('\n'.join(out))


if __name__ == '__main__':
    main()
