"""ELFR script modules (`.seq`, magic ELFR101) -> analysis dumps.

The modules are MIPS code run by the VM in sequence/sequence.cpp (notes/script-vm.md). This tool decodes them the
way the VM does and tracks values through each function, to answer: what does a module touch outside its own
window, what does it pass to the natives, and which strings label its events.

Address model (VM, notes/script-vm.md):
  code   VM address = file offset + 0x8130 (jal uses imm16 << 2; file 0x30 = 0x8160 = linked 0x408160);
         targets <= 0x815f are native calls, index (t - 0x144) >> 6
  data   lui 0x41/0x45 -> base, lui 0x42/0x46 -> base + 0x10000 (handler 0x211d54); every other lui value is used raw
         base = W + 0x450000 - hdr[0x10], W = buf + text + data + hdr[0xc] + 0x8000 (slot setup 0x215824)
  stack  gp = W, sp = W + 0x780; the 0x780 bytes above W are saved/restored per context (0x2160bc / 0x215f40)

Value tracking is intra-procedural and linear (registers reset at branch targets and after calls; -O0 stack slots
keep the last value written in program order), with one interprocedural step for pointer arguments.

Usage:
  python elfr.py <module> --dump                    header, windows, functions, native calls
  python elfr.py <files|dirs...> --survey           opcode use as the VM dispatches it
  python elfr.py <files|dirs...> --mem              every load/store classified by what its base points at
  python elfr.py <files|dirs...> --abs              non-relocated lui values: what they build, how they are used
  python elfr.py <files|dirs...> --natives          (native, a0 sub-code) use, with nearby native-32 labels
  python elfr.py <files|dirs...> --strings [OUT]    strings passed to native 32 with a0 = 10 (game text: default
                                                    extracted/script-strings.tsv; regenerate with
                                                    `python tools/elfr.py extracted/rtpk/DATA/SEQ_ENG --strings`)
  python elfr.py <module> --ghidra OUT.tsv          annotations for tools/ghidra/ImportElfr.java
"""
import argparse
import collections
import glob
import os
import struct
import sys

MAGIC = b'ELFR101\0'
CODE_BASE = 0x8130          # VM code address of file offset 0
LINK_BASE = 0x400000        # modules are linked with VM address 0 at 0x400000
NATIVE_END = 0x8160         # VM addresses below this are native stubs
RELOC = {0x41: 0, 0x45: 0, 0x42: 0x10000, 0x46: 0x10000}
STACK = 0x780
END_MARK = {-0x20: 'shared buffer A (seq+0xdc1c)', -0x10: 'shared buffer B (seq+0xdc20)'}
M32 = 0xffffffff

SPECIAL = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 12: 'syscall',
           13: 'break', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 24: 'mult', 25: 'multu', 26: 'div',
           27: 'divu', 32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor',
           42: 'slt', 43: 'sltu'}
PRIMARY = {2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz', 8: 'addi', 9: 'addiu', 10: 'slti',
           11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui', 28: 'mul', 32: 'lb', 33: 'lh', 34: 'lwl',
           35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr', 40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 46: 'swr',
           49: 'lwc1', 53: 'ldc1', 57: 'swc1', 61: 'sdc1'}
REGIMM = {0: 'bltz', 1: 'bgez'}
COP1_S = {0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 5: 'abs.s', 6: 'mov.s', 7: 'neg.s', 13: 'trunc.w.s',
          18: 'c.eq.s(18)', 28: 'c.lt.s(28)', 30: 'c.le.s(30)', 50: 'c.eq.s', 60: 'c.lt.s', 62: 'c.le.s'}
LOADS = {'lw', 'lb', 'lbu', 'lh', 'lhu', 'lwl', 'lwr', 'lwc1'}
STORES = {'sw', 'sb', 'sh', 'swl', 'swr', 'swc1'}
BRANCH2 = {'beq', 'bne'}
BRANCH1 = {'blez', 'bgtz', 'bltz', 'bgez', 'bc1t', 'bc1f'}
ARGREGS = (4, 5, 6, 7, 8, 9)  # a0-a3, t0, t1: the six arguments the step function (0x216500) passes to a native
REGN = ('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra').split()


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def decode(w):
    """-> (mnemonic, rs, rt, rd, imm16, word). Names follow the VM's tables (0x2ef5e8 / 0x2ef6e8 / 0x2ef798)."""
    op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if w == 0:
        n = 'nop'
    elif op == 0:
        n = SPECIAL.get(w & 63, 'special%d' % (w & 63))
    elif op == 1:
        n = REGIMM.get(rt, 'regimm%d' % rt)
    elif op == 17:
        n = {0: 'mfc1', 4: 'mtc1'}.get(rs)
        if n is None:
            if rs == 8:
                n = 'bc1t' if rt & 1 else 'bc1f'
            elif rs == 16:
                n = COP1_S.get(w & 63, 'cop1.s.%d' % (w & 63))
            elif rs == 20 and w & 63 == 32:
                n = 'cvt.s.w'
            else:
                n = 'cop1.%d.%d' % (rs, w & 63)
    else:
        n = PRIMARY.get(op, 'op%d' % op)
    return n, rs, rt, rd, w & 0xffff, w


class Module:
    def __init__(self, data, name=''):
        if data[:8] != MAGIC:
            raise ValueError('not an ELFR101 module')
        self.d, self.name = data, name
        f = struct.unpack_from('<8I', data, 8)
        self.h0c = f[1] - (1 << 32) if f[1] & 0x80000000 else f[1]
        self.h10, self.text, self.data, self.bss, self.h20, self.h24 = f[2], f[3], f[4], f[5], f[6], f[7]
        self.data_end = self.text + self.data
        self.w_off = self.data_end + self.h0c + 0x8000        # gp; stack = [W, W + 0x780)
        self.base_off = self.w_off + 0x450000 - self.h10       # lui 0x41/0x45
        self.window = self.w_off + STACK
        self.words = struct.unpack_from('<%dI' % ((self.text - 0x30) // 4), data, 0x30)

    def off_of(self, i):
        return 0x30 + 4 * i

    def vm_addr(self, off):
        return off + CODE_BASE

    def link_addr(self, off):
        return off + CODE_BASE + LINK_BASE

    def cstring(self, off, limit=0x200):
        if not 0 <= off < len(self.d):
            return None
        e = self.d.find(b'\0', off, off + limit)
        raw = self.d[off:e if e >= 0 else off + limit]
        try:
            return raw.decode('cp932')
        except UnicodeDecodeError:
            return raw.decode('latin-1')

    def region(self, off):
        """Name of the module-window region holding file offset `off`."""
        if off is None:
            return 'window'
        if off < 0:
            return 'before buffer'
        if off < self.text:
            return 'text'
        if off < self.data_end:
            rel = off - self.data_end
            return END_MARK.get(rel, 'data')
        if off < self.w_off:
            return 'bss'
        if off < self.window:
            return 'stack'
        return 'past window'


def functions(m):
    """Function starts (word indices): the entry, jal targets inside the module, and `addiu sp,sp,-N` prologues."""
    starts = {0}
    for i, w in enumerate(m.words):
        if w >> 16 == 0x27bd and w & 0x8000:
            starts.add(i)
        elif w >> 26 == 3:
            t = (w & 0xffff) << 2
            if t >= NATIVE_END:
                k = (t - CODE_BASE - 0x30) // 4
                if 0 <= k < len(m.words):
                    starts.add(k)
    return sorted(starts)


def branch_targets(m):
    tg = set()
    for i, w in enumerate(m.words):
        n = decode(w)[0]
        if n in BRANCH2 or n in BRANCH1:
            tg.add(i + 1 + s16(w & 0xffff))
    return tg


# Abstract values: ('k', value, lui_hi or None) constant | ('m', off) module window | ('sp', off) stack |
# ('arg', i) incoming a0-a3 | ('ret', native, sub) native result | ('rets', func) script result |
# ('ld', base_value, off) word loaded from memory | None unknown

def add_const(v, c):
    if v is None:
        return None
    t = v[0]
    if t == 'k':
        return None if c is None else ('k', (v[1] + c) & M32, v[2])
    if t in ('m', 'sp'):
        return (t, None if v[1] is None or c is None else v[1] + c)
    if t in ('ret', 'arg', 'ld', 'rets'):
        return ('ptr+', v, c)
    if t == 'ptr+':
        return ('ptr+', v[1], None if v[2] is None or c is None else v[2] + c)
    return None


def is_ptrish(v):
    return v is not None and v[0] in ('m', 'sp', 'ret', 'arg', 'ld', 'rets', 'ptr+')


class Analysis:
    """Runs the value tracker over one module and records events."""

    def __init__(self, m):
        self.m = m
        self.funcs = functions(m)
        self.targets = branch_targets(m)
        self.accesses = []    # (word index, 'load'|'store', base value, imm, mnemonic)
        self.natives = []     # (word index, native, [arg values])
        self.calls = []       # (word index, callee func word index, [arg values])
        self.uses = []        # (word index, lui value tuple, use string)
        self.addr_forms = []  # (word index, file offset) for addiu results that are module-window addresses
        self.anomalies = collections.Counter()
        fstart = set(self.funcs)
        bounds = self.funcs + [len(m.words)]
        for a, b in zip(bounds, bounds[1:]):
            self._run(a, b, fstart)

    def _use(self, i, v, what):
        if v is not None and v[0] == 'k' and v[2] is not None:
            self.uses.append((i, v, what))

    def _run(self, a, b, fstart):
        m = self.m
        regs = {}
        slots = {}

        def reset(entry=False):
            regs.clear()
            regs[29] = ('sp', 0) if entry else regs_sp[0]
            regs[30] = regs_fp[0]
            regs[28] = ('m', m.w_off)
            if entry:
                for k in range(4):
                    regs[4 + k] = ('arg', k)

        regs_sp, regs_fp = [('sp', 0)], [None]
        reset(True)
        i = a
        pending = None   # (kind, data) of a jal whose delay slot is being executed
        while i < b:
            if i in self.targets and i != a and pending is None:
                regs_sp[0], regs_fp[0] = regs.get(29), regs.get(30)
                reset()
            n, rs, rt, rd, imm, w = decode(m.words[i])
            g = lambda r: ('k', 0, None) if r == 0 else regs.get(r)

            def put(r, v):
                if r:
                    regs[r] = v

            if n == 'lui':
                put(rt, ('m', m.base_off + RELOC[imm]) if imm in RELOC else ('k', imm << 16, imm))
            elif n in ('addiu', 'addi'):
                v = add_const(g(rs), s16(imm))
                if v is not None and v[0] == 'm' and v[1] is not None and rs != 29:
                    self.addr_forms.append((i, v[1]))
                put(rt, v)
            elif n == 'ori':
                src = g(rs)
                if src is not None and src[0] == 'k':
                    put(rt, ('k', src[1] | imm, src[2]))
                elif src is not None and src[0] == 'm':
                    self.anomalies['ori on relocated lui'] += 1
                    put(rt, add_const(src, imm))
                else:
                    self._use(i, src, 'arith')
                    put(rt, None)
            elif n in ('addu', 'or', 'add'):
                x, y = g(rs), g(rt)
                if rt == 0:
                    put(rd, x)
                elif rs == 0:
                    put(rd, y)
                elif x is not None and y is not None and x[0] == 'k' and y[0] == 'k':
                    v = (x[1] + y[1]) & M32 if n != 'or' else x[1] | y[1]
                    put(rd, ('k', v, x[2] or y[2]))
                elif is_ptrish(x) and (y is None or y[0] == 'k'):
                    self._use(i, y, 'index')
                    put(rd, add_const(x, y[1]) if y is not None else add_const(x, None) if x[0] in ('m', 'sp')
                        else ('ptr+', x, None))
                elif is_ptrish(y) and (x is None or x[0] == 'k'):
                    self._use(i, x, 'index')
                    put(rd, add_const(y, x[1]) if x is not None else add_const(y, None) if y[0] in ('m', 'sp')
                        else ('ptr+', y, None))
                else:
                    self._use(i, x, 'arith')
                    self._use(i, y, 'arith')
                    put(rd, None)
            elif n == 'subu':
                x, y = g(rs), g(rt)
                self._use(i, x, 'arith')
                self._use(i, y, 'arith')
                put(rd, ('k', (x[1] - y[1]) & M32, None) if x and y and x[0] == y[0] == 'k' else None)
            elif n in LOADS or n in STORES:
                base, off = g(rs), s16(imm)
                kind = 'load' if n in LOADS else 'store'
                self.accesses.append((i, kind, base, off, n))
                self._use(i, base, kind + ' base')
                addr = add_const(base, off)
                if n == 'sw':
                    self._use(i, g(rt), 'stored value')
                if kind == 'store':
                    if addr is not None and addr[0] == 'sp' and addr[1] is not None and n == 'sw':
                        slots[addr[1]] = g(rt)
                elif n == 'lw':
                    if addr is not None and addr[0] == 'sp' and addr[1] is not None:
                        put(rt, slots.get(addr[1]))
                    elif base is not None:
                        put(rt, ('ld', addr, 0))
                    else:
                        put(rt, None)
                elif n != 'lwc1':
                    put(rt, None)
            elif n == 'mtc1':
                self._use(i, g(rt), 'mtc1 (float)')
            elif n == 'mfc1':
                put(rt, None)
            elif n in BRANCH2:
                self._use(i, g(rs), 'compare')
                self._use(i, g(rt), 'compare')
            elif n in ('slti', 'sltiu', 'slt', 'sltu', 'andi', 'xori', 'and', 'xor', 'nor', 'sll', 'srl', 'sra',
                       'sllv', 'srlv', 'srav', 'mult', 'multu', 'div', 'divu', 'mul'):
                for r in (rs, rt) if n not in ('slti', 'sltiu', 'andi', 'xori') else (rs,):
                    self._use(i, g(r), 'compare' if n.startswith('slt') else 'arith')
                dst = rt if n in ('slti', 'sltiu', 'andi', 'xori') else rd
                if n == 'andi' and g(rs) is not None and g(rs)[0] == 'k':
                    put(dst, ('k', g(rs)[1] & imm, None))
                elif n not in ('mult', 'multu', 'div', 'divu'):
                    put(dst, None)
            elif n in ('mfhi', 'mflo'):
                put(rd, None)
            elif n == 'jr':
                if rs == 31:
                    self._use(i, g(2), 'returned')
            elif n == 'jal':
                t = (imm << 2)
                if t < NATIVE_END:
                    pending = ('native', (t - 0x144) >> 6)
                else:
                    pending = ('call', (t - CODE_BASE - 0x30) // 4)
                i += 1
                continue
            elif n in ('j', 'jalr', 'syscall', 'break') or n.startswith(('op', 'special', 'regimm', 'cop1.')):
                self.anomalies['unexpected ' + n] += 1
            if pending is not None:
                args = [g(r) for r in ARGREGS]
                kind, data = pending
                if kind == 'native':
                    self.natives.append((i - 1, data, args))
                    sub = args[0][1] if args[0] is not None and args[0][0] == 'k' else None
                    for k, v in enumerate(args):
                        self._use(i - 1, v, 'native %d sub %s %s' % (data, sub, REGN[ARGREGS[k]]))
                    ret = ('ret', data, sub)
                else:
                    self.calls.append((i - 1, data, args))
                    for k, v in enumerate(args[:4]):
                        self._use(i - 1, v, 'script call arg %s' % REGN[ARGREGS[k]])
                    ret = ('rets', data)
                for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                    regs.pop(r, None)
                regs[2] = ret
                pending = None
            i += 1


def resolve_arg(an, func_of, v, depth=0):
    """Replace ('arg', k) by the set of values callers pass (one level of callers per depth step)."""
    if v is None or v[0] != 'arg' or depth > 4:
        return [v]
    f = func_of
    out = []
    for i, callee, args in an.calls:
        if callee == f[0] and args[v[1]] is not None:
            a = args[v[1]]
            if a[0] == 'arg':
                caller = an.func_at(i)
                out += resolve_arg(an, (caller,), a, depth + 1)
            else:
                out.append(a)
    return out or [None]


def describe_base(m, v):
    """Classify a load/store base value -> (class, detail)."""
    if v is None:
        return 'unknown', ''
    t = v[0]
    if t == 'm':
        return 'module ' + m.region(v[1]), ''
    if t == 'sp':
        return 'module stack', ''
    if t == 'k':
        return ('engine absolute' if v[2] is not None else 'constant address'), '%#x' % v[1]
    if t == 'ptr+':
        return describe_base(m, v[1])
    if t == 'ld':
        inner = v[1]
        if inner is not None and inner[0] == 'm':
            r = m.region(inner[1])
            if r.startswith('shared buffer'):
                return 'via ' + r, ''
            return 'via pointer in module ' + r, '+%#x' % inner[1] if inner[1] is not None else ''
        if inner is not None and inner[0] == 'sp':
            return 'via pointer on stack', ''
        if inner is not None and inner[0] == 'ptr+':
            c, d = describe_base(m, ('ld', inner[1], 0))
            return c, d
        return 'via loaded pointer', ''
    if t == 'ret':
        return 'native result', 'native %s sub %s' % (v[1], v[2])
    if t == 'rets':
        return 'script function result', ''
    if t == 'arg':
        return 'argument', ''
    return 'unknown', ''


def iter_modules(paths):
    for p in paths:
        files = [p] if os.path.isfile(p) else sorted(glob.glob(os.path.join(p, '**', '*'), recursive=True))
        for f in files:
            if not os.path.isfile(f):
                continue
            with open(f, 'rb') as h:
                if h.read(8) != MAGIC:
                    continue
                h.seek(0)
                yield f, Module(h.read(), f)


def short_name(path):
    """extracted/rtpk/DATA/SEQ_ENG/W001/AREA.RPK/a164.ARA.d/0002 -> ENG/W001/a164"""
    parts = path.replace('\\', '/').split('/')
    lang = next((x[4:] for x in parts if x.startswith('SEQ_')), '?')
    tail = [x for x in parts[parts.index('SEQ_' + lang) + 1:] if x]
    if tail and tail[0] == 'G000.GRB':
        return '%s/G000' % lang
    w = tail[0] if tail else '?'
    if len(tail) > 1 and tail[1] == 'W000.WRD':
        return '%s/%s/W000' % (lang, w)
    area = next((x.split('.')[0] for x in tail if x.lower().endswith(('.ara.d', '.ara'))), '?')
    return '%s/%s/%s' % (lang, w, area)


def native32_strings(m, an):
    out = []
    for i, nat, args in an.natives:
        if nat == 32 and args[0] == ('k', 10, None) and args[1] is not None and args[1][0] == 'm':
            out.append((m.link_addr(m.off_of(i)), m.cstring(args[1][1])))
    return out


def func_index(an):
    import bisect
    starts = an.funcs

    def at(i):
        return starts[bisect.bisect_right(starts, i) - 1]
    return at


STRINGS_TSV = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'extracted',
                                            'script-strings.tsv'))
NATIVES_TSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'notes', 'script-natives.tsv')


def native_names(path=NATIVES_TSV):
    """notes/script-natives.tsv -> {(native, sub or '*'): name}"""
    names = {}
    with open(path, encoding='utf-8') as f:
        next(f)
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 3:
                names[(int(p[0]), p[1] if p[1] == '*' else int(p[1], 0))] = p[2]
    return names


def native_label(names, nat, sub):
    base = names.get((nat, '*'), 'n%d' % nat)
    if sub is None:
        return base
    if (nat, sub) in names:
        return '%s.%s' % (base, names[(nat, sub)])
    if sub > 0xffffff and all(32 < (sub >> s) & 0xff < 127 for s in (0, 8, 16, 24)):
        return "%s.'%s'" % (base, sub.to_bytes(4, 'big').decode())   # multi-char constant, as written in C
    return '%s.sub%d' % (base, sub)


def tsv_row(*fields):
    return '\t'.join(str(x).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ') for x in fields)


def write_ghidra(path, out):
    """Annotation TSV for tools/ghidra/ImportElfr.java, in linked addresses. Code: file offset + 0x408130. Data, bss,
    stack: file offset + lo, lo = 0x410000 - base, which is 0x408130 plus a gap of 0xd0 (0xb0 in G000) that the
    ELFR packing removed. lui 0x45/0x46 address the same bytes 0x40000 higher (the VM gives 0x41 and 0x45 the
    same base), so the importer adds that alias."""
    import re
    m = Module(open(path, 'rb').read(), path)
    an = Analysis(m)
    names = native_names()
    hi = 0x410000 - m.base_off
    rows = [tsv_row('MOD', short_name(path), '%#x' % len(m.d), '%#x' % m.text, '%#x' % (CODE_BASE + LINK_BASE),
                    '%#x' % (hi + m.text), '%#x' % (hi + m.window), '%#x' % (hi + m.w_off))]
    for i in range(512):
        rows.append(tsv_row('NATIVE', '%#x' % (LINK_BASE + 0x144 + 64 * i),
                            'native_%d_%s' % (i, names.get((i, '*'), 'stub'))))
    labels = collections.defaultdict(list)
    at = func_index(an)
    for i, nat, args in an.natives:
        if nat == 32 and args[0] == ('k', 10, None) and args[1] is not None and args[1][0] == 'm':
            labels[at(i)].append((i, m.cstring(args[1][1]) or ''))
    used = collections.Counter()
    for f in an.funcs:
        name = None
        first = labels.get(f, [])
        for _, s in first[:1]:
            mm = (re.match(r'\s*\[ev\]\s*(\w+)', s) or re.match(r'\s*\[Area init\]\s*(\w+)', s) or
                  re.match(r'[\s>#=*:-]*(?:world|area|global)?\s*:?\s*([A-Za-z_]\w*)\s*(?:\(|enter\b)', s))
            if mm:
                name = ('ev_' if s.lstrip().startswith('[ev]') else 'area_init_' if 'Area init' in s else '') + \
                    mm.group(1)
        va = m.link_addr(m.off_of(f))
        if f == 0:
            name = 'module_entry'
        name = name or 'fn_%06x' % va
        used[name] += 1
        if used[name] > 1:
            name = '%s_%d' % (name, used[name])
        rows.append(tsv_row('FUNC', '%#x' % va, name))
        if first:
            txt = ' | '.join(s.strip() for _, s in first[:12])
            rows.append(tsv_row('PLATE', '%#x' % va, 'labels: ' + txt + (' ...' if len(first) > 12 else '')))
    for i, nat, args in an.natives:
        sub = args[0][1] if args[0] is not None and args[0][0] == 'k' else None
        if nat == 32:
            s = m.cstring(args[1][1]) if sub == 10 and args[1] is not None and args[1][0] == 'm' else None
            txt = 'debug_print %r' % s.strip() if s is not None else 'debug_print sub %s' % sub
        else:
            argtxt = ', '.join('%s=%s' % (REGN[ARGREGS[k]], ('%#x' % v[1]) if v[1] > 9 else str(v[1]))
                               for k, v in enumerate(args[1:], 1) if v is not None and v[0] == 'k')
            txt = native_label(names, nat, sub) + ('  (' + argtxt + ')' if argtxt else '')
        rows.append(tsv_row('CMT', '%#x' % m.link_addr(m.off_of(i)), txt))
    refs = set()
    for i, kind, base, off, n in an.accesses:
        addr = add_const(base, off)
        if addr is not None and addr[0] == 'm' and addr[1] is not None:
            refs.add((m.link_addr(m.off_of(i)), hi + addr[1], 'W' if kind == 'store' else 'R'))
    for i, off in an.addr_forms:
        refs.add((m.link_addr(m.off_of(i)), hi + off, 'A'))
    for src, dst, k in sorted(refs):
        rows.append(tsv_row('REF', '%#x' % src, '%#x' % dst, k))
    for rel, what in END_MARK.items():
        rows.append(tsv_row('LABEL', '%#x' % (hi + m.data_end + rel), 'ptr_' + what.split(' (')[0].replace(' ', '_')))
    rows.append(tsv_row('LABEL', '%#x' % (hi + m.w_off), 'stack_base_gp'))
    with open(out, 'w', encoding='utf-8', newline='') as fh:
        fh.write('\n'.join(rows) + '\n')
    print('wrote %s: %d functions, %d native calls, %d refs' % (out, len(an.funcs), len(an.natives), len(refs)))


def cmd_dump(path):
    m = Module(open(path, 'rb').read(), path)
    an = Analysis(m)
    print('%s: %d bytes, text %#x, data %#x, bss %#x, hdr[0xc] %d, hdr[0x10] %#x, hdr[0x20] %#x, hdr[0x24] %#x' % (
        path, len(m.d), m.text, m.data, m.bss, m.h0c, m.h10, m.h20, m.h24))
    print('window: code file 0x30.. (VM %#x), data %#x..%#x, bss to %#x, W (gp) %#x, stack to %#x, '
          'lui 0x41 base = file %#x' % (m.vm_addr(0x30), m.text, m.data_end, m.data_end + m.bss, m.w_off,
                                        m.window, m.base_off))
    print('%d functions, %d native calls, %d script calls, %d loads/stores' % (
        len(an.funcs), len(an.natives), len(an.calls), len(an.accesses)))
    c = collections.Counter((nat, a[0][1] if a[0] is not None and a[0][0] == 'k' else None)
                            for _, nat, a in an.natives)
    print('natives:', ', '.join('%d/%s:%d' % (k[0], k[1], v) for k, v in sorted(c.items(), key=str)))
    for k, v in an.anomalies.items():
        print('anomaly:', k, v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--survey', action='store_true')
    ap.add_argument('--mem', action='store_true')
    ap.add_argument('--abs', action='store_true')
    ap.add_argument('--natives', action='store_true')
    ap.add_argument('--strings', nargs='?', const=STRINGS_TSV)
    ap.add_argument('--ghidra')
    a = ap.parse_args()
    sys.stdout.reconfigure(errors='backslashreplace')
    if a.dump:
        for p in a.paths:
            cmd_dump(p)
        return
    if a.ghidra:
        write_ghidra(a.paths[0], a.ghidra)
        return
    ops, opmods = collections.Counter(), collections.defaultdict(set)
    mem = collections.Counter()
    mem_detail = collections.defaultdict(collections.Counter)
    abs_uses = collections.defaultdict(collections.Counter)
    abs_mods = collections.defaultdict(set)
    nat_use = collections.Counter()
    nat_mods = collections.defaultdict(set)
    nat_labels = collections.defaultdict(collections.Counter)
    anomalies = collections.Counter()
    strings = []
    nmod = 0
    for path, m in iter_modules(a.paths):
        nmod += 1
        name = short_name(path)
        if a.survey:
            for w in m.words:
                n = decode(w)[0]
                ops[n] += 1
                opmods[n].add(name)
            continue
        an = Analysis(m)
        anomalies.update(an.anomalies)
        if a.mem:
            at = func_index(an)
            an.func_at = at
            for i, kind, base, off, n in an.accesses:
                bases = resolve_arg(an, (at(i),), base) if base is not None and base[0] == 'arg' else [base]
                for b in bases:
                    cls, det = describe_base(m, add_const(b, off) if b is not None and b[0] in ('m', 'sp', 'k')
                                             else b)
                    if cls == 'argument':
                        cls = 'argument (unresolved)'
                    mem[(cls, kind)] += 1
                    if det:
                        mem_detail[(cls, kind)][det] += 1
        if a.abs:
            for i, v, use in an.uses:
                abs_uses[v[1]][use] += 1
                abs_mods[v[1]].add(name)
        if a.natives or a.strings:
            labels = native32_strings(m, an)
            if a.strings:
                strings += [(name, '%#x' % addr, s) for addr, s in labels]
            if a.natives:
                import bisect
                laddr = [x[0] for x in labels]
                for i, nat, args in an.natives:
                    sub = args[0][1] if args[0] is not None and args[0][0] == 'k' else None
                    nat_use[(nat, sub)] += 1
                    nat_mods[(nat, sub)].add(name)
                    if nat == 32:
                        continue
                    k = bisect.bisect_right(laddr, m.link_addr(m.off_of(i))) - 1
                    if k >= 0 and m.link_addr(m.off_of(i)) - laddr[k] < 0x100:
                        nat_labels[(nat, sub)][(labels[k][1] or '').strip().replace('\n', ' ')] += 1
    print('modules', nmod)
    for k, v in anomalies.items():
        print('anomaly: %s %d' % (k, v))
    if a.survey:
        for n, c in ops.most_common():
            print('  %-14s %9d  in %4d modules' % (n, c, len(opmods[n])))
    if a.mem:
        tot = sum(mem.values())
        print('loads/stores: %d' % tot)
        for (cls, kind), c in sorted(mem.items(), key=lambda kv: -kv[1]):
            det = mem_detail.get((cls, kind))
            print('  %-40s %-5s %8d  %s' % (cls, kind, c, ', '.join('%s:%d' % x for x in det.most_common(12))
                                             if det else ''))
    if a.abs:
        for v in sorted(abs_uses):
            u = abs_uses[v]
            print('%#010x  %6d uses  %4d modules  %s' % (v, sum(u.values()), len(abs_mods[v]),
                                                        ', '.join('%s:%d' % x for x in u.most_common(6))))
    if a.natives:
        for (nat, sub), c in sorted(nat_use.items(), key=lambda kv: (kv[0][0], -1 if kv[0][1] is None
                                                                       else kv[0][1])):
            lab = nat_labels.get((nat, sub))
            print('%3d  sub %-6s %6d calls %4d modules  %s' % (
                nat, sub, c, len(nat_mods[(nat, sub)]),
                ' | '.join('%s (%d)' % x for x in lab.most_common(3)) if lab else ''))
    if a.strings:
        with open(a.strings, 'w', encoding='utf-8', newline='') as f:
            f.write('module\tscript_addr\tstring\n')
            for name, addr, s in strings:
                f.write('%s\t%s\t%s\n' % (name, addr, (s or '').replace('\t', '\\t').replace('\n', '\\n')
                                          .replace('\r', '\\r')))
        print('wrote %s: %d strings' % (a.strings, len(strings)))


if __name__ == '__main__':
    main()
