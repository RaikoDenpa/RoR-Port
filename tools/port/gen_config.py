"""Generate the PS2Recomp config for SLES_542.18 from our Ghidra project.

Inputs
  extracted/ps2recomp/ghidra/{functions,switches}.tsv   RoRExportRecomp.java (qualified names, jr sites + targets)
  tools/ghidra_scripts/data/jumptables.tsv              our jump-table overrides (no Ghidra references)
  tools/port/bindings.tsv                          the allowlist: every stub/syscall/override binding
  disc/SLES_542.18                                      the ELF (table words, instruction patterns)
  tools/PS2Recomp/ps2xRuntime/include/ps2_call_list.h  the runtime's handler names (PS2Recomp binds by name)
Outputs (extracted/ps2recomp/)
  functions.csv, config.toml, gen/ror_bindings.inc      (the .inc is the override list compiled into src/)

Checks; any failure writes nothing and exits 1:
  - each allowlist row names a function start, with the Ghidra name it was written against;
  - no stub/syscall row (a runtime *library* handler) is a game function (GAME_RANGES), and its handler exists;
  - no address is bound twice;
  - no exported name can be matched by name to a runtime handler (exact, or one leading '_' added/removed, as
    ps2_runtime_calls.h resolves them; a `handler@addr` stub also binds every function *named* handler):
    colliding names get the suffix `_guest`, so the only bindings are the allowlisted addresses;
  - every j/jal target lies in an exported function;
  - every `jr reg` site gets a table PS2Recomp can key (address computed with its own lui/addu/lw match) whose
    ELF entries include Ghidra's targets and stay inside the function (or its trailing gap), except the computed
    jumps in DYNAMIC_JR.

  python tools/port/gen_config.py [--dump]
"""
import argparse, bisect, collections, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'extracted', 'ps2recomp')

# Game code may only be bound to our own overrides, never to a runtime library handler (memory-map.md, sdk.md).
GAME_RANGES = [(0x100000, 0x239f60, '.text game code, i3d/i3r and vector-math libraries'),
               (0x282dc8, 0x297eb8, '.gnu.linkonce.t (inline/template game functions)')]

# Computed jumps that are not switch tables: runtime dispatch is correct for them.
DYNAMIC_JR = {
    0x275128: 'libgcc longjmp-style unwind (restores sp/fp from a buffer, jr to the saved pc)',
    0x275aa8: 'libgcc EH return (jr a0 after the handler search, sp adjusted in the delay slot)',
    0x275c08: 'libgcc EH return (second copy)',
}

# Instruction patches, with the reason each one exists (port-strategy.md 2.1, run 1).
PATCHES = [
    (0x2695c8, 0x03e00008, 'jr ra: SDK kernel patch called by _InitSys scans kernel memory 0x80000000-0x80080000 '
                           'for two code patterns and loops until their addresses agree; PS2Recomp has no kernel '
                           'image there, so it never ends. The runtime is the kernel: skip it'),
    (0x2695cc, 0x00000000, 'nop (delay slot of the jr ra above)'),
]


def tsv(path):
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\r\n')
        if line and not line.startswith('#'):
            yield line.split('\t')


class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        phoff, = struct.unpack_from('<I', self.d, 0x1c)
        phnum, = struct.unpack_from('<H', self.d, 0x2c)
        self.segs = []
        for i in range(phnum):
            typ, off, va, _, fsz, _, _, _ = struct.unpack_from('<8I', self.d, phoff + 32 * i)
            if typ == 1:
                self.segs.append((va, va + fsz, off))

    def word(self, va):
        for lo, hi, off in self.segs:
            if lo <= va and va + 4 <= hi:
                return struct.unpack_from('<I', self.d, off + va - lo)[0]
        return None


def runtime_names(path):
    """X(name) entries of PS2_SYSCALL_LIST and PS2_STUB_LIST."""
    text = open(path, encoding='utf-8').read()
    out = {}
    for macro in ('PS2_SYSCALL_LIST', 'PS2_STUB_LIST'):
        m = re.search(r'#define\s+%s\(X\)((?:.*\\\n)*.*)' % macro, text)
        if not m:
            sys.exit('%s not found in %s' % (macro, path))
        out[macro] = set(re.findall(r'X\((\w+)\)', m.group(1)))
    return out['PS2_SYSCALL_LIST'], out['PS2_STUB_LIST']


def resolves(name, names):
    """ps2_runtime_calls::resolveNameWithOptionalLeadingUnderscoreAlias."""
    return name in names or (name.startswith('_') and name[1:] in names) or ('_' + name) in names


def c_name(qualified):
    return re.sub(r'[^A-Za-z0-9_]', '_', qualified.replace('::', '__'))


def in_game(a):
    return next((why for lo, hi, why in GAME_RANGES if lo <= a < hi), None)


def table_address(elf, start, jr):
    """PS2Recomp ControlFlowAnalyzer: lw (20 back from jr) <- addu (10 back) <- lui + addiu/ori (20 back)."""
    ins = [elf.word(a) for a in range(start, jr + 4, 4)]
    j = len(ins) - 1
    rs = lambda w: (w >> 21) & 31
    rt = lambda w: (w >> 16) & 31
    s16 = lambda w: (w & 0xffff) - 0x10000 if w & 0x8000 else w & 0xffff
    jreg = rs(ins[j])
    lw = next((i for i in range(j - 1, max(-1, j - 21), -1) if ins[i] >> 26 in (0x23, 0x27) and rt(ins[i]) == jreg), None)
    if lw is None:
        return None
    base, off = rs(ins[lw]), s16(ins[lw])
    addu = next((i for i in range(lw - 1, max(-1, lw - 11), -1)
                 if ins[i] >> 26 == 0 and ins[i] & 63 == 0x21 and (ins[i] >> 11) & 31 == base), None)
    if addu is None:
        return None
    regs = (rs(ins[addu]), rt(ins[addu]))
    for i in range(addu - 1, max(-1, addu - 21), -1):
        w = ins[i]
        if w >> 26 == 15 and rt(w) in regs:
            low = 0
            for k in range(i + 1, addu):
                x = ins[k]
                if rs(x) == rt(w) and rt(x) == rt(w):
                    if x >> 26 == 9:
                        low = s16(x)
                    elif x >> 26 == 13:
                        low = x & 0xffff
            return ((w & 0xffff) << 16) + low + off & 0xffffffff
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ghidra', default=os.path.join(OUT, 'ghidra'))
    ap.add_argument('--elf', default=os.path.join(ROOT, 'disc', 'SLES_542.18'))
    ap.add_argument('--calls', default=os.path.join(ROOT, 'tools', 'PS2Recomp', 'ps2xRuntime', 'include', 'ps2_call_list.h'))
    ap.add_argument('--bindings', default=os.path.join(ROOT, 'tools', 'port', 'bindings.tsv'))
    ap.add_argument('--jumptables', default=os.path.join(ROOT, 'tools', 'ghidra_scripts', 'data', 'jumptables.tsv'))
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--dump', action='store_true', help='print every binding, rename and table')
    args = ap.parse_args()

    elf = Elf(args.elf)
    syscalls, stubs = runtime_names(args.calls)
    runtime = syscalls | stubs
    errors = []

    funcs = [(int(a, 16), int(e, 16), int(n), q) for a, e, n, q in tsv(os.path.join(args.ghidra, 'functions.tsv'))]
    by_start = {f[0]: f for f in funcs}

    # Names: C-safe, unique, never resolvable to a runtime handler.
    count = collections.Counter(c_name(q) for _, _, _, q in funcs)
    names, renamed = {}, []
    for a, _, _, q in funcs:
        n = c_name(q)
        if count[n] > 1:
            n = '%s_%x' % (n, a)
        if resolves(n, runtime):
            renamed.append((a, q, n + '_guest'))
            n += '_guest'
        names[a] = n
    assert not any(resolves(n, runtime) for n in names.values())

    # Allowlist.
    binds = []
    seen = {}
    for row in tsv(args.bindings):
        if len(row) < 4:
            errors.append('bindings.tsv: short row %r' % row)
            continue
        a, kind, handler, gname = int(row[0], 16), row[1], row[2], row[3]
        note = row[4] if len(row) > 4 else ''
        where = '%#x %s' % (a, gname)
        if a not in by_start:
            errors.append('%s: no function starts there' % where)
        elif by_start[a][3] != gname:
            errors.append('%s: Ghidra now calls it %s' % (where, by_start[a][3]))
        if kind != 'trace':  # a trace wraps whatever is bound there
            if a in seen:
                errors.append('%s: bound twice (%s and %s)' % (where, seen[a], handler))
            seen[a] = handler
        if kind == 'trace':
            pass
        elif kind in ('stub', 'syscall'):
            game = in_game(a)
            if game:
                errors.append('%s: game function bound to runtime library handler %s (%s)' % (where, handler, game))
            if handler not in (syscalls if kind == 'syscall' else stubs):
                errors.append('%s: runtime has no %s handler %s' % (where, kind, handler))
        elif kind == 'override':
            if not re.fullmatch(r'[A-Za-z_]\w*', handler):
                errors.append('%s: override handler %r is not a C identifier' % (where, handler))
        else:
            errors.append('%s: unknown kind %s' % (where, kind))
        binds.append((a, kind, handler, gname, note))

    # Jump tables. A table is read while its words stay inside the function or the gap up to the next function:
    # Ghidra drops entries the index cannot reach (0x1280f8 switches on an even value; the odd entries are the
    # default), and a few functions end before their last cases (0x21ac80, 0x21ace0, 0x21ad38): their CSV end is
    # moved to the next function start so PS2Recomp decodes the cases.
    starts = sorted(by_start)
    next_start = {a: (starts[i + 1] if i + 1 < len(starts) else a) for i, a in enumerate(starts)}
    ends = {a: by_start[a][1] for a in starts}
    extended = {}
    ours = {int(f, 16): (int(t, 16), int(n)) for f, t, n in tsv(args.jumptables)}
    tables, dynamic = [], []
    for jr, fn, targets in (r + [''] * (3 - len(r)) for r in tsv(os.path.join(args.ghidra, 'switches.tsv'))):
        jr, fn = int(jr, 16), int(fn, 16)
        if jr in DYNAMIC_JR:
            dynamic.append(jr)
            continue
        tgt = {int(t, 16) for t in targets.split(',') if t}
        addr = table_address(elf, fn, jr)
        if addr is None:
            errors.append('jr %#x (fn %#x): PS2Recomp cannot key a table here (no lui/addu/lw match)' % (jr, fn))
            continue
        limit = max(next_start[fn], ends[fn])
        if tgt:
            words = []
            while len(words) < 1024 and fn <= (elf.word(addr + 4 * len(words)) or 0) < limit:
                words.append(elf.word(addr + 4 * len(words)))
            if not tgt <= set(words):
                errors.append('jr %#x: table %#x (%d words) misses %d of Ghidra\'s targets' % (jr, addr, len(words), len(tgt - set(words))))
                continue
            src = 'ghidra'
        elif fn in ours and ours[fn][0] == addr:
            words = [elf.word(addr + 4 * i) for i in range(ours[fn][1])]
            src = 'jumptables.tsv'
        else:
            errors.append('jr %#x (fn %#x): no targets from Ghidra and no jumptables.tsv row for table %#x' % (jr, fn, addr))
            continue
        outside = [w for w in words if not fn <= w < limit]
        if outside:
            errors.append('jr %#x: %d targets outside %#x-%#x' % (jr, len(outside), fn, limit))
        elif max(words) >= ends[fn]:
            extended[fn] = (ends[fn], limit)
            ends[fn] = limit
        tables.append((addr, jr, fn, src, words))
    # Every static call/jump target must lie in an exported function range: PS2Recomp has no code anywhere else, and
    # the thread is parked at run time (0x1bf078, 0x21a348 were tail-jump targets Ghidra had merged elsewhere).
    order = sorted(ends)
    for a in order:
        for va in range(a, ends[a], 4):
            w = elf.word(va)
            if w is None or w >> 26 not in (2, 3):
                continue
            t = ((va + 4) & 0xf0000000) | ((w & 0x3ffffff) << 2)
            if not (0x100000 <= t < 0x297eb8):
                continue
            i = bisect.bisect_right(order, t) - 1
            if i < 0 or not order[i] <= t < ends[order[i]]:
                errors.append('%s %#x -> %#x: target is in no function' % ('jal' if w >> 26 == 3 else 'j', va, t))
    keyed = collections.Counter(t[0] for t in tables)
    for addr, n in keyed.items():
        if n > 1 and len({tuple(sorted(set(t[4]))) for t in tables if t[0] == addr}) > 1:
            errors.append('table %#x is used by %d sites with different targets' % (addr, n))

    if args.dump:
        for a, kind, handler, gname, _ in sorted(binds):
            print('bind   %#010x %-8s %-28s %s' % (a, kind, handler, gname))
        for a, q, n in renamed:
            print('rename %#010x %s -> %s' % (a, q, n))
        for a, (old, new) in sorted(extended.items()):
            print('extend %#010x %s end %#x -> %#x (jump-table cases)' % (a, by_start[a][3], old, new))
        for addr, jr, fn, src, words in sorted(tables, key=lambda t: t[1]):
            print('table  jr %#x fn %#x table %#x entries %d (%s)' % (jr, fn, addr, len(words), src))
    if errors:
        for e in errors:
            print('ERROR', e, file=sys.stderr)
        sys.exit(1)

    # Write.
    os.makedirs(os.path.join(args.out, 'gen'), exist_ok=True)
    csv = os.path.join(args.out, 'functions.csv')
    with open(csv, 'w', encoding='utf-8', newline='\n') as f:
        f.write('Name,Start,End,Size\n')
        for a, _, n, _ in funcs:
            f.write('%s,0x%08X,0x%08X,%d\n' % (names[a], a, ends[a], n))
    lib = sorted(b for b in binds if b[1] in ('stub', 'syscall'))
    ovr = sorted(b for b in binds if b[1] == 'override')
    trc = sorted(b for b in binds if b[1] == 'trace')
    fwd = lambda p: p.replace('\\', '/')
    with open(os.path.join(args.out, 'config.toml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Generated by tools/port/gen_config.py. Do not edit: change its inputs and re-run.\n')
        f.write('# %d functions (%d renamed with _guest), %d runtime library bindings from bindings.tsv,\n'
                '# %d jump tables (%d computed jumps left to runtime dispatch).\n\n'
                % (len(funcs), len(renamed), len(lib), len(tables), len(dynamic)))
        f.write('[general]\ninput = "%s"\noutput = "%s"\nghidra_output = "%s"\n'
                % (fwd(args.elf), fwd(os.path.join(args.out, 'output')), fwd(csv)))
        f.write('single_file_output = false\npatch_syscalls = false\npatch_cop0 = true\npatch_cache = true\n')
        f.write('# By address only (handler@0xADDR); every row is in tools/port/bindings.tsv.\nstubs = [\n')
        for a, kind, handler, gname, note in lib:
            f.write('  "%s@0x%08X",  # %s%s\n' % (handler, a, gname, (': ' + note) if note else ''))
        f.write(']\nskip = []\n\n[patches]\ninstructions = [\n')
        for a, v, why in PATCHES:
            f.write('  # %#x: %s\n  { address = "0x%08X", value = "0x%08X" },\n' % (a, why, a, v))
        f.write(']\n\n[jump_tables]\n')
        for addr, jr, fn, src, words in sorted(tables):
            f.write('\n# jr %#x in %s (%s)\n[[jump_tables.table]]\naddress = 0x%08X\nentries = [\n' % (jr, by_start[fn][3], src, addr))
            for i, w in enumerate(words):
                f.write('  { index = %d, target = 0x%08X },\n' % (i, w))
            f.write(']\n')
    with open(os.path.join(args.out, 'gen', 'ror_bindings.inc'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/port/gen_config.py from tools/port/bindings.tsv. Do not edit.\n')
        f.write('// ROR_BIND(guest address, override handler in src/, Ghidra name)\n')
        for a, _, handler, gname, _ in ovr:
            f.write('ROR_BIND(0x%08x, %s, "%s")\n' % (a, handler, gname))
        f.write('// ROR_TRACE(guest address, Ghidra name): log the call, then run whatever is bound there\n')
        for a, _, _, gname, _ in trc:
            f.write('ROR_TRACE(0x%08x, "%s")\n' % (a, gname))
    print('functions %d (%d renamed, %d extended), library bindings %d, overrides %d, jump tables %d, dynamic jr %d -> %s'
          % (len(funcs), len(renamed), len(extended), len(lib), len(ovr), len(tables), len(dynamic), args.out))


if __name__ == '__main__':
    main()
