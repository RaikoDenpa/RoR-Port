"""Check PS2Recomp's generated C++ against the allowlist and our jump tables (run after every recompile).

Fails (exit 1) when
  - a function is bound to a runtime handler (listed in ps2_recompiled_stubs.h) that bindings.tsv does not list
    with that handler, or one in game code (gen_config.GAME_RANGES) is bound at all;
  - a bindings.tsv stub/syscall row did not become a stub, or an override row has no generated function to replace;
  - a `jr reg` (not $ra) has no resolved case list, unless it is in gen_config.DYNAMIC_JR;
  - a generated function calls another one directly instead of through the function table (overrides would be
    bypassed; patch 0002 routes tail calls through the table).

  python tools/port/check_output.py [--output DIR] [--dump]
"""
import argparse, glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_config as g

STUB_CALL = re.compile(r'ctx->pc = getRegU32\(ctx, 31\);\s*ps2_(stubs|syscalls)::(\w+)\(rdram, ctx, runtime\);(?:.*?"([^"]*)")?')
JR = re.compile(r'// 0x([0-9a-f]+): 0x[0-9a-f]+\s+jr\s+\$(\w+)')
DIRECT_CALL = re.compile(r'\s*\w+_0x[0-9a-f]+\(rdram, ctx, runtime\);')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--output', default=os.path.join(g.OUT, 'output'))
    ap.add_argument('--bindings', default=os.path.join(g.ROOT, 'tools', 'port', 'bindings.tsv'))
    ap.add_argument('--dump', action='store_true')
    args = ap.parse_args()
    errors = []

    want, traces = {}, {}
    for row in g.tsv(args.bindings):
        (traces if row[1] == 'trace' else want)[int(row[0], 16)] = (row[1], row[2], row[3])

    # Stubs: the header lists them all; each one's file names the handler it calls.
    stubs = {}
    for name in re.findall(r'void (\w+_0x[0-9a-f]+)\(', open(os.path.join(args.output, 'ps2_recompiled_stubs.h'), encoding='utf-8').read()):
        a = int(name.rsplit('_0x', 1)[1], 16)
        src = open(os.path.join(args.output, name + '.cpp'), encoding='utf-8').read()
        m = STUB_CALL.search(src)
        stubs[a] = (name, ('syscall' if m.group(1) == 'syscalls' else 'stub', m.group(2)) if m else ('?', src))
    for a, (name, (kind, handler)) in sorted(stubs.items()):
        if g.in_game(a):
            errors.append('%s: game function bound to runtime handler %s (%s)' % (name, handler, g.in_game(a)))
        w = want.get(a)
        if not w or w[0] != kind or w[1] != handler:
            errors.append('%s: bound to %s %s, not in bindings.tsv' % (name, kind, handler))
        if args.dump:
            print('stub %#010x %-8s %-28s %s' % (a, kind, handler, name))
    for a, (kind, handler, gname) in sorted(want.items()) + sorted(traces.items()):
        if kind in ('stub', 'syscall') and a not in stubs:
            errors.append('%#x %s: allowlisted %s %s did not become a stub' % (a, gname, kind, handler))
        if kind in ('override', 'trace') and not glob.glob(os.path.join(args.output, '*_0x%x.cpp' % a)):
            errors.append('%#x %s: %s target has no generated function' % (a, gname, kind))

    # Indirect jumps: every non-ra jr needs a case list. Direct calls between generated functions are refused.
    sites = unresolved = direct = 0
    for path in glob.glob(os.path.join(args.output, '*_0x*.cpp')):
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        for i, line in enumerate(lines):
            if DIRECT_CALL.match(line):
                direct += 1
                if direct <= 20:
                    errors.append('%s: direct call %s' % (os.path.basename(path), line.strip()))
            m = JR.search(line)
            if not m or m.group(2) == 'ra':
                continue
            sites += 1
            a = int(m.group(1), 16)
            cases = 0
            for l2 in lines[i + 1:i + 2000]:
                if re.match(r'\s+case 0x[0-9A-F]+u: goto', l2):
                    cases += 1
                if 'dispatchGuestBranch' in l2:
                    break
            if not cases and a not in g.DYNAMIC_JR:
                unresolved += 1
                errors.append('jr %#x (%s): no case list, runtime dispatch only' % (a, os.path.basename(path)))
    print('stubs %d (allowlist %d stub/syscall rows), jr sites %d, unresolved %d (+%d dynamic), direct calls %d'
          % (len(stubs), sum(1 for w in want.values() if w[0] != 'override'), sites, unresolved, len(g.DYNAMIC_JR), direct))
    for e in errors:
        print('ERROR', e, file=sys.stderr)
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
