"""Resolve a game-thread profile (src/ror_profile_win32.cpp) with the runner's linker map and split the time.

Each sample is a stack (leaf first) of exe offsets or module names. A sample is charged to the first frame, from
the leaf up, that is not C runtime or another module (memcpy, sleep and allocator time go to their caller), and the
frame's object file decides the category. Time spent sleeping in our vsync wait is reported separately.

  python tools/port/profile.py [--profile extracted/ps2recomp/run/profile.tsv] [--map ...ps2EntryRunner.map]
                               [--top N] [--dump]
"""
import argparse, bisect, collections, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'extracted', 'ps2recomp')

# Object file (runtime library objects are "lib:object") -> category.
CATEGORIES = [
    (r'^unity_\d+_cxx\.obj$|_0x[0-9a-f]+\.obj$', 'recompiled game code'),
    (r'(^|:)(gs_cpu_backend|gs_frontend|ps2_gs_memory|ps2_gif_arbiter)\.obj$', 'GS emulation'),
    (r'(^|:)ps2_vif1_interpreter\.obj$', 'VIF1 emulation'),
    (r'(^|:)ps2_vu1_\w+\.obj$|(^|:)vu\w*\.obj$', 'VU1 emulation'),
    (r'(^|:)ps2_memory\.obj$', 'runtime: memory / DMA'),
    (r'(^|:)ps2_runtime\.obj$', 'runtime: dispatch'),
    (r'(^|:)EeScheduler\.obj$', 'runtime: scheduler'),
    (r'(^|:)ps2_iop\w*\.obj$|^ps2_iop:', 'runtime: IOP'),
    (r'^ps2_runtime:', 'runtime: other'),
    (r'^ror_', 'our overrides'),
    (r'^raylib:|^rl\w+\.obj$', 'host window (raylib)'),
]
PASS_THROUGH = re.compile(r'^(libcmt|libvcruntime|libucrt|libcpmt|msvcprt|msvcrt|ucrt|vcruntime|LIBCMT|LIBCPMT|libcpmt\w*|oldnames)', re.I)


def demangle(name):
    """Readable form of an MSVC name: ?f@C@N@@... -> N::C::f (good enough for reports)."""
    if not name.startswith('?'):
        return name
    body = name[1:].split('@@', 1)[0]
    parts = [p for p in body.split('@') if p]
    if not parts:
        return name
    if parts[0].startswith('?'):  # special names (??0 ctor, ??1 dtor, ...)
        parts[0] = parts[0][1:]
    return '::'.join(reversed(parts))


def load_map(path):
    base, syms = 0x140000000, []
    rx = re.compile(r'^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$', re.I)
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\s*Preferred load address is ([0-9a-f]+)', line, re.I)
        if m:
            base = int(m.group(1), 16)
            continue
        m = rx.match(line)
        if m:
            addr = int(m.group(2), 16)
            if addr >= base:
                syms.append((addr - base, demangle(m.group(1)), m.group(3)))
    syms.sort()
    return syms


def category(obj):
    for rx, name in CATEGORIES:
        if re.search(rx, obj):
            return name
    return 'runtime: other'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--profile', default=os.path.join(OUT, 'run', 'profile.tsv'))
    ap.add_argument('--map', default=os.path.join(OUT, 'tree', 'out', 'build', 'ps2xRuntime', 'Release', 'ps2EntryRunner.map'))
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--dump', action='store_true', help='print every sample with its charged frame')
    args = ap.parse_args()

    syms = load_map(args.map)
    rvas = [s[0] for s in syms]

    def resolve(tok):
        if not tok.startswith('e'):
            return ('module', tok, tok)
        rva = int(tok[1:], 16)
        i = bisect.bisect_right(rvas, rva) - 1
        if i < 0:
            return ('?', tok, '?')
        _, name, obj = syms[i]
        return ('exe', name, obj)

    meta, samples = {}, []
    for line in open(args.profile, encoding='utf-8'):
        if line.startswith('#'):
            k, *v = line[1:].strip().split('\t')
            meta[k.strip()] = v
            continue
        frame, stack = line.rstrip('\n').split('\t')
        samples.append((int(frame), [resolve(t) for t in stack.split()]))
    seconds = float(meta['seconds'][0])
    f0, f1 = int(meta['frames'][0]), int(meta['frames'][1])
    frames = max(1, f1 - f0)

    cats, funcs = collections.Counter(), collections.defaultdict(collections.Counter)
    for frame, stack in samples:
        charged = next((f for f in stack if f[0] == 'exe' and not PASS_THROUGH.match(f[2])), None)
        if charged is None:
            cat, fn = 'other (outside the exe)', stack[0][1]
        else:
            cat, fn = category(charged[2]), charged[1]
            if cat == 'our overrides' and stack[0][0] == 'module' and ('waitVblank' in fn or 'wait_vsync_swap' in fn):
                cat = 'vsync wait (sleeping)'
        cats[cat] += 1
        funcs[cat][fn] += 1
        if args.dump:
            print(frame, cat, fn, ' <- '.join(f[1] for f in stack[:6]))

    n = len(samples)
    per_frame = 1000.0 * seconds / frames
    busy = n - cats['vsync wait (sleeping)']
    print('%d samples over %.1f s, frames %d-%d: %.1f ms per frame, of which %.1f ms working (not in the vsync sleep)'
          % (n, seconds, f0, f1, per_frame, per_frame * busy / n))
    print('\n%-30s %8s %8s %10s' % ('category', 'samples', '% busy', 'ms/frame'))
    for cat, c in cats.most_common():
        share = 100.0 * c / busy if cat != 'vsync wait (sleeping)' and busy else float('nan')
        print('%-30s %8d %8.1f %10.1f' % (cat, c, share, per_frame * c / n))
    for cat, _ in cats.most_common():
        print('\n%s: top functions' % cat)
        for fn, c in funcs[cat].most_common(args.top):
            print('  %6.1f%%  %s' % (100.0 * c / cats[cat], fn))


if __name__ == '__main__':
    main()
