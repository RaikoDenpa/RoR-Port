"""VU microcode in SLES_542.18 (.vutext) -> program list and disassembly.

The game uploads its VU code once at boot (notes/engine.md): a DMA ref of 0x354 qwords at 0x2932b0 on VIF1 (VU1) and
0x9c qwords at 0x2969f0 on VIF0 (VU0). This tool walks that VIF stream the way the VIF would: MPG blocks place code
in micro memory, UNPACK/STROW/STCOL/STMASK/DIRECT carry data, BASE/OFFSET/STCYCL/ITOP/MSCAL are recorded. Each MPG
block is reported as a program with statistics that hint at its job, and can be disassembled.

Usage:
  python vudis.py <SLES_542.18> --dump                       programs of both streams
  python vudis.py <SLES_542.18> --dis vu1|vu0 [--out FILE]   full disassembly (to extracted/, it is game code)
"""
import argparse
import collections
import struct
import sys

STREAMS = {'vu1': (0x2932b0, 0x354), 'vu0': (0x2969f0, 0x9c)}
DEST = lambda d: ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b)
BC = 'xyzw'


class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        ph, = struct.unpack_from('<I', self.d, 0x1c)
        n, = struct.unpack_from('<H', self.d, 0x2c)
        self.segs = [struct.unpack_from('<8I', self.d, ph + 32 * i) for i in range(n)]

    def read(self, va, size):
        for s in self.segs:
            if s[0] == 1 and s[2] <= va and va + size <= s[2] + s[4]:
                return self.d[s[1] + va - s[2]:s[1] + va - s[2] + size]
        raise ValueError('address %#x not in a loaded segment' % va)


def upper(w):
    fd, fs, ft, dest, op = (w >> 6) & 31, (w >> 11) & 31, (w >> 16) & 31, (w >> 21) & 15, w & 63
    flags = ''.join(f for f, b in (('[I]', 31), ('[E]', 30), ('[M]', 29), ('[D]', 28), ('[T]', 27)) if w >> b & 1)
    d = DEST(dest)
    if op < 0x3c:
        base = {0: 'add', 1: 'sub', 2: 'madd', 3: 'msub', 4: 'max', 5: 'mini', 6: 'mul'}
        if op < 0x1c:
            name = '%s%s.%s' % (base[op >> 2], BC[op & 3], d)
            return name, 'vf%d, vf%d, vf%d%s' % (fd, fs, ft, BC[op & 3]), flags
        tbl = {0x1c: 'mulq', 0x1d: 'maxi', 0x1e: 'muli', 0x1f: 'minii', 0x20: 'addq', 0x21: 'maddq', 0x22: 'addi',
               0x23: 'maddi', 0x24: 'subq', 0x25: 'msubq', 0x26: 'subi', 0x27: 'msubi', 0x28: 'add', 0x29: 'madd',
               0x2a: 'mul', 0x2b: 'max', 0x2c: 'sub', 0x2d: 'msub', 0x2e: 'opmsub', 0x2f: 'mini'}
        n = tbl.get(op, 'upper%#x' % op)
        args = 'vf%d, vf%d' % (fd, fs) + ('' if n[-1] in 'qi' and n not in ('mini',) and op < 0x28 else ', vf%d' % ft)
        return n + '.' + d, args, flags
    ext = ((w >> 6) & 31) << 2 | (op & 3)
    if ext < 0x10:
        b = {0: 'adda', 1: 'suba', 2: 'madda', 3: 'msuba'}[ext >> 2]
        return '%s%s.%s' % (b, BC[ext & 3], d), 'acc, vf%d, vf%d%s' % (fs, ft, BC[ext & 3]), flags
    if 0x18 <= ext < 0x1c:
        return 'mula%s.%s' % (BC[ext & 3], d), 'acc, vf%d, vf%d%s' % (fs, ft, BC[ext & 3]), flags
    tbl = {0x10: 'itof0', 0x11: 'itof4', 0x12: 'itof12', 0x13: 'itof15', 0x14: 'ftoi0', 0x15: 'ftoi4',
           0x16: 'ftoi12', 0x17: 'ftoi15', 0x1c: 'mulaq', 0x1d: 'abs', 0x1e: 'mulai', 0x1f: 'clipw', 0x20: 'addaq',
           0x21: 'maddaq', 0x22: 'addai', 0x23: 'maddai', 0x24: 'subaq', 0x25: 'msubaq', 0x26: 'subai',
           0x27: 'msubai', 0x28: 'adda', 0x29: 'madda', 0x2a: 'mula', 0x2b: 'suba', 0x2c: 'msuba', 0x2e: 'opmula',
           0x2f: 'nop'}
    n = tbl.get(ext, 'uext%#x' % ext)
    if n == 'nop':
        return 'nop', '', flags
    if n.startswith(('itof', 'ftoi', 'abs')):
        return n + '.' + d, 'vf%d, vf%d' % (ft, fs), flags
    if n == 'clipw':
        return 'clipw.' + d, 'vf%d, vf%dw' % (fs, ft), flags
    return n + '.' + d, 'acc, vf%d, vf%d' % (fs, ft), flags


def lower(w, pc):
    op = w >> 25
    it, is_, imm11 = (w >> 16) & 31, (w >> 11) & 31, w & 0x7ff
    dest = DEST((w >> 21) & 15)
    s11 = imm11 - 0x800 if imm11 & 0x400 else imm11
    if op == 0x40:
        fn = w & 63
        id_ = (w >> 6) & 31
        if fn < 0x3c:
            t = {0x30: 'iadd', 0x31: 'isub', 0x32: 'iaddi', 0x34: 'iand', 0x35: 'ior'}
            n = t.get(fn, 'lower%#x' % fn)
            if n == 'iaddi':
                imm5 = id_ - 32 if id_ & 16 else id_
                return n, 'vi%d, vi%d, %d' % (it, is_, imm5)
            return n, 'vi%d, vi%d, vi%d' % (id_, is_, it)
        ext = id_ << 2 | (fn & 3)
        t = {0x30: 'move', 0x31: 'mr32', 0x34: 'lqi', 0x35: 'sqi', 0x36: 'lqd', 0x37: 'sqd', 0x38: 'div',
             0x39: 'sqrt', 0x3a: 'rsqrt', 0x3b: 'waitq', 0x3c: 'mtir', 0x3d: 'mfir', 0x3e: 'ilwr', 0x3f: 'iswr',
             0x40: 'rnext', 0x41: 'rget', 0x42: 'rinit', 0x43: 'rxor', 0x64: 'mfp', 0x68: 'xtop', 0x69: 'xitop',
             0x6c: 'xgkick', 0x70: 'esadd', 0x71: 'ersadd', 0x72: 'eleng', 0x73: 'erleng', 0x74: 'eatanxy',
             0x75: 'eatanxz', 0x76: 'esum', 0x78: 'esqrt', 0x79: 'ersqrt', 0x7a: 'ercpr', 0x7b: 'waitp',
             0x7c: 'esin', 0x7d: 'eatan', 0x7e: 'eexp'}
        n = t.get(ext, 'lext%#x' % ext)
        if n == 'move' and w == 0x8000033c:
            return 'nop', ''
        if n in ('move', 'mr32', 'mfp', 'rnext', 'rget'):
            return n + '.' + dest, 'vf%d, vf%d' % (it, is_)
        if n in ('lqi', 'lqd'):
            return n + '.' + dest, 'vf%d, (vi%d%s)' % (it, is_, '++' if n == 'lqi' else '--')
        if n in ('sqi', 'sqd'):
            return n + '.' + dest, 'vf%d, (vi%d%s)' % (is_, it, '++' if n == 'sqi' else '--')
        if n in ('div', 'rsqrt'):
            return n, 'q, vf%d%s, vf%d%s' % (is_, BC[(w >> 21) & 3], it, BC[(w >> 23) & 3])
        if n == 'sqrt':
            return n, 'q, vf%d%s' % (it, BC[(w >> 23) & 3])
        if n in ('xgkick', 'xtop', 'xitop'):
            return n, 'vi%d' % (is_ if n == 'xgkick' else it)
        if n in ('mtir', 'mfir'):
            return n, 'vi%d, vf%d%s' % (it, is_, BC[(w >> 21) & 3])
        if n in ('ilwr', 'iswr'):
            return n + '.' + dest, 'vi%d, (vi%d)' % (it, is_)
        if n.startswith('e'):
            return n, 'p, vf%d' % is_
        return n, ''
    t = {0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x10: 'fceq',
         0x11: 'fcset', 0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand', 0x17: 'fsor',
         0x18: 'fmeq', 0x1a: 'fmand', 0x1b: 'fmor', 0x1c: 'fcget', 0x20: 'b', 0x21: 'bal', 0x24: 'jr', 0x25: 'jalr',
         0x28: 'ibeq', 0x29: 'ibne', 0x2c: 'ibltz', 0x2d: 'ibgtz', 0x2e: 'iblez', 0x2f: 'ibgez'}
    n = t.get(op, 'lop%#x' % op)
    tgt = pc + 8 + s11 * 8
    if n in ('lq', 'ilw'):
        return n + '.' + dest, '%s%d, %d(vi%d)' % ('vf' if n == 'lq' else 'vi', it, s11 * (16 if n == 'lq' else 1), is_)
    if n in ('sq', 'isw'):
        return n + '.' + dest, '%s%d, %d(vi%d)' % ('vf' if n == 'sq' else 'vi', is_ if n == 'sq' else it, s11, it if n == 'sq' else is_)
    if n in ('iaddiu', 'isubiu'):
        imm15 = (w >> 10) & 0x7800 | imm11
        return n, 'vi%d, vi%d, %#x' % (it, is_, imm15)
    if n in ('b', 'bal'):
        return n, ('vi%d, ' % it if n == 'bal' else '') + '%#x' % tgt
    if n in ('jr', 'jalr'):
        return n, ('vi%d, ' % it if n == 'jalr' else '') + 'vi%d' % is_
    if n.startswith('ib'):
        return n, ('vi%d, vi%d, ' % (it, is_) if n in ('ibeq', 'ibne') else 'vi%d, ' % is_) + '%#x' % tgt
    if n.startswith('f'):
        return n, 'vi1, %#x' % (w & 0xffffff) if n in ('fceq', 'fcset', 'fcand', 'fcor') else 'vi%d, ...' % it
    return n, '%08x' % w


def unpack_size(cmd, num, cl, wl):
    vn, vl = (cmd >> 2) & 3, cmd & 3
    bits = (32, 16, 8, 5)[vl] * (vn + 1)
    if (cmd & 0xf) == 0xf:
        bits = 16
    n = num if num else 256
    if wl <= cl or not cl:
        count = n
    else:
        count = cl * (n // wl) + min(n % wl, cl)
    return (bits * count + 31) // 32 * 4


def walk(data):
    """VIF stream -> list of events: ('MPG', vu_addr_bytes, [(lower, upper)]), ('UNPACK', ...), ('CODE', name, imm)."""
    out, p, cl, wl = [], 0, 1, 1
    while p + 4 <= len(data):
        code, = struct.unpack_from('<I', data, p)
        cmd, num, imm = (code >> 24) & 0x7f, (code >> 16) & 0xff, code & 0xffff
        p += 4
        if cmd == 0x4a:
            n = num or 256
            ins = [struct.unpack_from('<II', data, p + 8 * k) for k in range(n)]
            out.append(('MPG', imm * 8, ins, p))
            p += 8 * n
        elif cmd in (0x50, 0x51):
            q = imm or 65536
            p = (p + 15) & ~15
            out.append(('DIRECT', q, p))
            p += 16 * q
        elif cmd >= 0x60:
            size = unpack_size(cmd, num, cl, wl)
            out.append(('UNPACK', cmd, num, imm & 0x3ff, (imm >> 15) & 1, size))
            p += size
        elif cmd in (0x30, 0x31):
            out.append(('STROW' if cmd == 0x30 else 'STCOL',))
            p += 16
        elif cmd == 0x20:
            out.append(('STMASK',))
            p += 4
        else:
            if cmd == 0x01:
                cl, wl = imm & 0xff, (imm >> 8) & 0xff
            names = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD',
                     0x06: 'MSKPATH3', 0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL',
                     0x15: 'MSCALF', 0x17: 'MSCNT'}
            out.append(('CODE', names.get(cmd, 'cmd%#x' % cmd), imm))
    return out


JOB_HINTS = [  # (mnemonic prefix, label)
    ('xgkick', 'GS output'), ('clipw', 'clipping'), ('div', 'perspective divide'), ('ftoi4', 'screen coords'),
    ('itof', 'int->float input'), ('ersqrt', 'normalize (lighting)'), ('eleng', 'vector length'),
    ('esin', 'sin'), ('eexp', 'exp (fog?)'), ('ercpr', 'reciprocal'), ('eatan', 'atan'),
    ('ilw', 'integer loads (indices/weights)'), ('mtir', 'float->index (matrix palette?)'),
]


def stats(prog):
    c = collections.Counter()
    loops = 0
    for k, (lo, up) in enumerate(prog['ins']):
        pc = prog['addr'] + 8 * k
        if lo == 0x8000033c and up & 0x7fffffff == 0x2ff:
            c['nop'] += 1
        un = upper(up)[0]
        ln = lower(lo, pc)[0] if not up >> 31 else 'loi'
        for n in (un, ln):
            c[n.split('.')[0]] += 1
        if ln.startswith(('ib', 'b')) and not up >> 31:
            off = lo & 0x7ff
            if off & 0x400:
                loops += 1
        if up >> 30 & 1:
            c['[E]'] += 1
    return c, loops


def programs(elf, which):
    va, qwc = STREAMS[which]
    ev = walk(elf.read(va, qwc * 16))
    progs = [dict(addr=e[1], ins=e[2], ee=va + e[3]) for e in ev if e[0] == 'MPG']
    return progs, ev


def cmd_dump(elf):
    for which in ('vu1', 'vu0'):
        progs, ev = programs(elf, which)
        other = collections.Counter(e[0] if e[0] != 'CODE' else e[1] for e in ev if e[0] != 'MPG')
        print('== %s stream %#x: %d MPG blocks, other VIF codes %s' % (which, STREAMS[which][0], len(progs),
                                                                     dict(other)))
        for e in ev:
            if e[0] == 'UNPACK':
                print('   UNPACK cmd %#x num %d -> vu data %#x%s' % (e[1], e[2], e[3], ' (+TOPS)' if e[4] else ''))
            elif e[0] == 'CODE' and e[1] not in ('NOP',):
                print('   %s %#x' % (e[1], e[2]))
        for i, pg in enumerate(progs):
            c, loops = stats(pg)
            n = len(pg['ins'])
            ends = [pg['addr'] + 8 * k for k, (lo, up) in enumerate(pg['ins']) if up >> 30 & 1]
            hints = ['%s %d' % (lab, sum(v for k, v in c.items() if k.startswith(pre))) for pre, lab in JOB_HINTS
                     if any(k.startswith(pre) for k in c)]
            fm = sum(v for k, v in c.items() if k.startswith(('mula', 'madda', 'madd', 'mul', 'opm')))
            print('  %s prog %2d: vu addr %#06x..%#06x (%3d instr, EE %#x) [E] at %s; backward branches %d; '
                  'mul/madd %d; %s' % (which, i, pg['addr'], pg['addr'] + 8 * n, n, pg['ee'],
                                       ','.join('%#x' % x for x in ends[:4]), loops, fm, '; '.join(hints)))


def cmd_dis(elf, which, out):
    progs, _ = programs(elf, which)
    f = open(out, 'w', encoding='utf-8', newline='\n') if out else sys.stdout
    for i, pg in enumerate(progs):
        f.write('; %s program %d at vu %#x (EE %#x)\n' % (which, i, pg['addr'], pg['ee']))
        for k, (lo, up) in enumerate(pg['ins']):
            pc = pg['addr'] + 8 * k
            un, ua, fl = upper(up)
            if up >> 31:
                ln, la = 'loi', '%g' % struct.unpack('<f', struct.pack('<I', lo))[0]
            else:
                ln, la = lower(lo, pc)
            f.write('%04x: %-14s %-24s %-5s| %-10s %s\n' % (pc, un, ua, fl, ln, la))
    if out:
        f.close()
        print('wrote', out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('elf')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--dis', choices=('vu1', 'vu0'))
    ap.add_argument('--out')
    a = ap.parse_args()
    elf = Elf(a.elf)
    if a.dump:
        cmd_dump(elf)
    if a.dis:
        cmd_dis(elf, a.dis, a.out)
    if not (a.dump or a.dis):
        ap.print_help()


if __name__ == '__main__':
    main()
