"""Decode a VIF1 stream dumped by ROR_VIF1_DUMP (src/ror_vif1dump.cpp): what VU1 receives per MSCAL.

  python tools/port/vif1_chain.py vif1_F.bin                    summary: VIFcodes, MSCALs, programs
  python tools/port/vif1_chain.py vif1_F.bin --scan [--huge X]  NaN / Inf / |x| > X (default 1e8) in 32-bit float UNPACKs
  python tools/port/vif1_chain.py vif1_F.bin --list A-B         MSCALs A..B: VIF state and every unpacked vector
  python tools/port/vif1_chain.py vif1_F.bin --at 0x3f4,0x3f5   VU1 data words at those addresses at each MSCAL (when changed)

VU1 data memory is simulated (1024 quadwords, from zero: the boot upload of constants is not in a frame's chain, so
--at shows only what this frame writes). UNPACK honours STCYCL (skipping and filling writes), STMASK/STROW/STCOL,
STMOD (offset/difference), USN and the FLG (+TOPS) bit; MSCAL/MSCNT latch TOP = TOPS and ITOP = ITOPS, then swap the
double buffer (TOPS = BASE or BASE + OFFSET, DBF toggled), as PS2Memory::processVIF1Data does.
"""
import argparse, math, struct, sys

NAMES = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD', 0x06: 'MSKPATH3',
         0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA', 0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT',
         0x20: 'STMASK', 0x30: 'STROW', 0x31: 'STCOL', 0x4a: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}
FMT = {(0, 0): 'S-32', (0, 1): 'S-16', (0, 2): 'S-8', (1, 0): 'V2-32', (1, 1): 'V2-16', (1, 2): 'V2-8', (2, 0): 'V3-32',
       (2, 1): 'V3-16', (2, 2): 'V3-8', (3, 0): 'V4-32', (3, 1): 'V4-16', (3, 2): 'V4-8', (3, 3): 'V4-5'}


def f32(u):
    return struct.unpack('<f', struct.pack('<I', u & 0xffffffff))[0]


class Vif:
    def __init__(self):
        self.mem = [[0, 0, 0, 0] for _ in range(1024)]
        self.cl = self.wl = 1
        self.base = self.ofst = self.tops = self.itops = self.top = self.itop = 0
        self.dbf = 0
        self.mode = 0
        self.mask = 0
        self.row = [0, 0, 0, 0]
        self.col = [0, 0, 0, 0]
        self.calls = []      # per MSCAL: dict
        self.unpacks = []    # since the last MSCAL: (vu addr, fmt, [vectors as u32 lists], float?)
        self.counts = {}

    def unpack(self, cmd, data, pos):
        op, num, imm = (cmd >> 24) & 0x7f, (cmd >> 16) & 0xff or 256, cmd & 0xffff
        vn, vl, masked = (op >> 2) & 3, op & 3, (op >> 4) & 1
        comps = vn + 1
        bits = 16 if (vl == 3 and vn == 3) else comps * (32 >> vl)
        # PCSX2 vifUnpackSetup (Vif_Unpack.cpp): WL 0 means 256; CL is taken as is, so CL < WL is a filling write
        # that reads CL * (NUM / WL) + min(NUM % WL, CL) vectors (CL 0: none, every value from the mask).
        cl, wl = self.cl, self.wl or 256
        if wl <= cl:
            nsrc = num
        else:
            nsrc = (num // wl) * cl + min(num % wl, cl)
            self.fills = getattr(self, 'fills', 0) + 1
            if cl == 0:
                self.cl0 = getattr(self, 'cl0', 0) + 1
        size = (nsrc * bits + 31) // 32 * 4
        src = data[pos:pos + size]
        addr = imm & 0x3ff
        if imm & 0x8000:
            addr = (addr + self.tops) & 0x3ff
        usn = (imm >> 14) & 1
        out, si = [], 0
        for i in range(num):
            cyc = i % wl
            dest = (addr + (i // wl) * cl + cyc) & 0x3ff if wl <= cl else (addr + i) & 0x3ff
            fill = cl < wl and cyc >= cl
            vec = None
            if not fill and si < nsrc:
                if vl == 3 and vn == 3:
                    v, = struct.unpack_from('<H', src, si * 2)
                    vec = [(v & 0x1f) << 3, ((v >> 5) & 0x1f) << 3, ((v >> 10) & 0x1f) << 3, ((v >> 15) & 1) << 7]
                else:
                    w = 32 >> vl
                    vec = []
                    for c in range(comps):
                        off = si * comps * w // 8 + c * w // 8
                        v = int.from_bytes(src[off:off + w // 8], 'little')
                        if w < 32 and not usn and v >> (w - 1):
                            v -= 1 << w
                        vec.append(v & 0xffffffff)
                    if vn == 0:
                        vec = vec * 4
                si += 1
            cur = self.mem[dest]
            for c in range(4):
                m = (self.mask >> ((min(cyc, 3) * 4 + c) * 2)) & 3 if masked else 0
                if vec is None and not masked:
                    m = 1 if fill else 3
                if m == 0:
                    if vec is None or c >= len(vec):
                        continue
                    v = vec[c]
                    if self.mode == 1:
                        v = (v + self.row[c]) & 0xffffffff
                    elif self.mode == 2:
                        v = (v + self.row[c]) & 0xffffffff
                        self.row[c] = v
                    cur[c] = v
                elif m == 1:
                    cur[c] = self.row[c]
                elif m == 2:
                    cur[c] = self.col[min(cyc, 3)]
            out.append((dest, list(cur)))
        self.unpacks.append((addr, FMT.get((vn, vl), '?'), vl == 0, out))
        return size

    def run(self, data):
        pos = 0
        while pos + 4 <= len(data):
            cmd, = struct.unpack_from('<I', data, pos)
            pos += 4
            op, imm = (cmd >> 24) & 0x7f, cmd & 0xffff
            name = 'UNPACK' if op & 0x60 == 0x60 else NAMES.get(op, '?%02x' % op)
            self.counts[name] = self.counts.get(name, 0) + 1
            if op == 0x01:
                self.cl, self.wl = imm & 0xff, imm >> 8
            elif op == 0x02:
                self.ofst, self.tops, self.dbf = imm & 0x3ff, self.base, 0
            elif op == 0x03:
                self.base = imm & 0x3ff
            elif op == 0x04:
                self.itops = imm & 0x3ff
            elif op == 0x05:
                self.mode = imm & 3
            elif op in (0x14, 0x15, 0x17):
                self.top, self.itop = self.tops, self.itops
                self.calls.append({'pc': imm * 8 if op != 0x17 else None, 'top': self.top, 'itop': self.itop,
                                   'base': self.base, 'ofst': self.ofst, 'dbf': self.dbf, 'unpacks': self.unpacks,
                                   'pos': pos - 4})
                self.unpacks = []
                self.tops = self.base if self.dbf else (self.base + self.ofst) & 0x3ff
                self.dbf ^= 1
            elif op == 0x20:
                self.mask, = struct.unpack_from('<I', data, pos)
                pos += 4
            elif op == 0x30:
                self.row = list(struct.unpack_from('<4I', data, pos))
                pos += 16
            elif op == 0x31:
                self.col = list(struct.unpack_from('<4I', data, pos))
                pos += 16
            elif op == 0x4a:
                pos += ((cmd >> 16) & 0xff or 256) * 8
            elif op in (0x50, 0x51):
                pos += (imm or 65536) * 16
            elif op & 0x60 == 0x60:
                pos += self.unpack(cmd, data, pos)
        return self


def bad(u, huge):
    x = f32(u)
    return math.isnan(x) or math.isinf(x) or abs(x) > huge


def fmtvec(vec, isfloat):
    return ' '.join(('%12.5g' % f32(v)) if isfloat else ('%08x' % v) for v in vec)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump')
    ap.add_argument('--scan', action='store_true')
    ap.add_argument('--huge', type=float, default=1e8)
    ap.add_argument('--list')
    ap.add_argument('--at')
    args = ap.parse_args()
    d = open(args.dump, 'rb').read()
    magic, frame, head, tte = struct.unpack_from('<4I', d, 0)
    if magic != 0x31464956:
        sys.exit('not a vif1 dump')
    v = Vif().run(d[16:])
    print('frame %d, chain %08x, TTE %d, %d bytes: %d MSCALs, %d filling UNPACKs (%d with CL 0); VIFcodes %s' % (
        frame, head, tte, len(d) - 16, len(v.calls), getattr(v, 'fills', 0), getattr(v, 'cl0', 0),
        dict(sorted(v.counts.items()))))
    if args.scan:
        hits = 0
        for i, c in enumerate(v.calls):
            for addr, fmt, isfloat, vecs in c['unpacks']:
                if not isfloat:
                    continue
                for dest, vec in vecs:
                    if any(bad(x, args.huge) for x in vec):
                        hits += 1
                        if hits <= 40:
                            print('  MSCAL %4d pc %s TOP %03x  %s -> %03x: %s' % (i, c['pc'] and hex(c['pc']), c['top'],
                                                                             fmt, dest, fmtvec(vec, True)))
        print('%d vectors with NaN / Inf / |x| > %g' % (hits, args.huge))
    if args.list:
        a, b = (int(x, 0) for x in args.list.split('-'))
        for i in range(a, min(b + 1, len(v.calls))):
            c = v.calls[i]
            print('MSCAL %d: pc %s TOP %03x ITOP %03x BASE %03x OFFSET %03x DBF %d' % (
                i, c['pc'] and hex(c['pc']), c['top'], c['itop'], c['base'], c['ofst'], c['dbf']))
            for addr, fmt, isfloat, vecs in c['unpacks']:
                print('  UNPACK %s -> %03x, %d vectors' % (fmt, addr, len(vecs)))
                for dest, vec in vecs[:64]:
                    print('    %03x: %s' % (dest, fmtvec(vec, isfloat)))
    if args.at:
        want = [int(x, 0) for x in args.at.split(',')]
        last = {}
        for i, c in enumerate(v.calls):
            for addr, fmt, isfloat, vecs in c['unpacks']:
                for dest, vec in vecs:
                    if dest in want and last.get(dest) != vec:
                        last[dest] = vec
                        print('  before MSCAL %4d: %03x = %s' % (i, dest, fmtvec(vec, isfloat)))


if __name__ == '__main__':
    main()
