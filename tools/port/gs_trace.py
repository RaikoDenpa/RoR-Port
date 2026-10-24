"""Summarise a one-frame GS trace written by src/ror_gs.cpp (ROR_GS_TRACE=N -> run/gs_frame_N.tsv).

Prints the privileged display registers, draw counts, and the distinct draw states (the registers the symptoms
depend on) with how many draws use each and their screen boxes; --where filters draws (e.g. "tpsm=T4", "csa!=0",
"abe=1"), --dump prints every row.

  python tools/port/gs_trace.py [TRACE] [--where KEY=VALUE ...] [--dump]
"""
import argparse, collections, glob, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUN = os.path.join(ROOT, 'extracted', 'ps2recomp', 'run')

STATE = ['prim', 'tme', 'abe', 'fpsm', 'fbw', 'fbp', 'tpsm', 'tbp0', 'tw', 'th', 'tcc', 'tfx', 'cpsm', 'csm', 'csa',
         'cld', 'alpha_a', 'alpha_b', 'alpha_c', 'alpha_d', 'fix', 'ate', 'atst', 'aref', 'afail', 'date', 'zte',
         'ztst', 'ta0', 'aem', 'ta1', 'fba', 'pabe', 'dthe', 'mmag', 'mmin', 'fbmsk']


def parse(path):
    rows = []
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        parts = line.rstrip('\n').split('\t')
        row = {'kind': parts[0]}
        for p in parts[1:]:
            if '=' in p:
                k, v = p.split('=', 1)
                row[k] = v
            else:
                row['index'] = p
        rows.append(row)
    return rows


def matches(row, conds):
    for c in conds:
        neg = '!=' in c
        k, v = c.split('!=' if neg else '=', 1)
        if (row.get(k) == v) == neg:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('trace', nargs='?')
    ap.add_argument('--where', action='append', default=[])
    ap.add_argument('--dump', action='store_true')
    args = ap.parse_args()
    path = args.trace or max(glob.glob(os.path.join(RUN, 'gs_frame_*.tsv')), key=os.path.getmtime)
    rows = parse(path)
    print('trace', path)

    for i, r in enumerate(x for x in rows if x['kind'] == 'priv'):
        print('\nprivileged registers (%s of the frame):' % ('start', 'end')[min(i, 1)])
        print('  PMODE %s: EN1=%s EN2=%s MMOD=%s AMOD=%s SLBG=%s ALP=%s   SMODE2 %s: INT=%s FFMD=%s DPMS=%s   BGCOLOR %s'
              % (r['pmode'], r['en1'], r['en2'], r['mmod'], r['amod'], r['slbg'], r['alp'], r['smode2'], r['int'],
                 r['ffmd'], r['dpms'], r['bgcolor']))
        for n in ('1', '2'):
            d, f = 'display' + n, 'dispfb' + n
            print('  DISPLAY%s %s: DX=%s DY=%s MAGH=%s MAGV=%s DW=%s DH=%s   DISPFB%s %s: FBP=%s FBW=%s PSM=%s DBX=%s DBY=%s'
                  % (n, r[d], r[d + '_dx'], r[d + '_dy'], r[d + '_magh'], r[d + '_magv'], r[d + '_dw'], r[d + '_dh'],
                     n, r[f], r[f + '_fbp'], r[f + '_fbw'], r[f + '_psm'], r[f + '_dbx'], r[f + '_dby']))

    draws = [r for r in rows if r['kind'] == 'draw' and matches(r, args.where)]
    print('\ndraws %d (of %d), CLUT loads %d, transfers %d' % (
        len(draws), sum(1 for r in rows if r['kind'] == 'draw'), sum(1 for r in rows if r['kind'] == 'clut'),
        sum(1 for r in rows if r['kind'] == 'transfer')))
    print('by primitive:', dict(collections.Counter(r['prim'] for r in draws)))

    groups = collections.OrderedDict()
    for r in draws:
        key = tuple(r.get(k, '') for k in STATE)
        g = groups.setdefault(key, {'n': 0, 'first': int(r['index']), 'box': [1e9, 1e9, -1e9, -1e9], 'rgba': set()})
        g['n'] += 1
        b = g['box']
        b[0], b[1] = min(b[0], float(r['x0'])), min(b[1], float(r['y0']))
        b[2], b[3] = max(b[2], float(r['x1'])), max(b[3], float(r['y1']))
        if len(g['rgba']) < 4:
            g['rgba'].add(r['rgba0'])
    print('\ndistinct draw states: %d' % len(groups))
    for key, g in groups.items():
        s = dict(zip(STATE, key))
        tex = ('tex %s tbp %s %sx%s tcc %s tfx %s clut %s csm %s csa %s cld %s %s'
               % (s['tpsm'], s['tbp0'], 1 << int(s['tw']), 1 << int(s['th']), s['tcc'], s['tfx'], s['cpsm'], s['csm'],
                  s['csa'], s['cld'], 'bilinear' if s['mmag'] == '1' else 'nearest')) if s['tme'] == '1' else 'untextured'
        blend = ('ABE (%s-%s)*%s+%s fix %s' % (s['alpha_a'], s['alpha_b'], s['alpha_c'], s['alpha_d'], s['fix'])
                 if s['abe'] == '1' else 'no blend')
        print('  #%-5d x%-4d %-9s box %4.0f,%4.0f-%4.0f,%4.0f  fb %s@%s w%s msk %s | %s | %s | ATE %s/%s ref %s fail %s ZTE %s/%s'
              ' | TEXA %s/%s/%s FBA %s PABE %s DTHE %s | rgba %s'
              % (g['first'], g['n'], s['prim'], *g['box'], s['fpsm'], s['fbp'], s['fbw'], s['fbmsk'], tex, blend,
                 s['ate'], s['atst'], s['aref'], s['afail'], s['zte'], s['ztst'], s['ta0'], s['aem'], s['ta1'], s['fba'],
                 s['pabe'], s['dthe'], ','.join(sorted(g['rgba']))))

    cluts = [r for r in rows if r['kind'] == 'clut']
    if cluts:
        print('\nCLUT loads (CLD != 0):')
        for r in cluts:
            print('  after draw %-5s %s cbp %s clut %s csm %s csa %s cld %s texclut %s/%s/%s'
                  % (r['after_draw'], r['psm'], r['cbp'], r['cpsm'], r['csm'], r['csa'], r['cld'], r['cbw'], r['cou'], r['cov']))
    if args.dump:
        for r in rows:
            print('\t'.join('%s=%s' % kv for kv in r.items()))


if __name__ == '__main__':
    main()
