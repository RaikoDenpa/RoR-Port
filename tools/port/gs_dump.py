"""Inspect a PCSX2 GS dump (src/ror_gsdump.cpp writes them; format of PCSX2 v2.8.2, GSDumpFile::ReadFile).

  python tools/port/gs_dump.py DUMP.gs [--draws] [--regs]
  python tools/port/gs_dump.py DUMP.gs --frames
  python tools/port/gs_dump.py DUMP_vif1.gs --strip-vif1 OUT.gs    a plain PCSX2 GS dump without records 0x40/0x41

DUMP may be compressed as PCSX2 writes it: .gs.xz (lzma) or .gs.zst (zstd through the zstd.dll of the PCSX2 build in
extracted/pcsx2, so nothing needs installing).

Prints the header, the frozen state's main registers, then per VSync: transfers per path and, parsing each path's GIF
stream the way GSState::Transfer<index> does (tags continue across transfers), the primitives drawn (--draws: each
with PRIM, vertex count and the register state at the kick) and the register writes (--regs).
--frames: per game frame instead (a frame starts at each PATH3 transfer: the sceGsPutDrawEnv packet of gs_swap_env
0x25b908), with the PATH1 primitives whose kick vertex has a negative Z or lies off the 512 x 448 screen.
"""
import argparse, ctypes, glob, lzma, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

REG_NAMES = {0x00: 'PRIM', 0x01: 'RGBAQ', 0x02: 'ST', 0x03: 'UV', 0x04: 'XYZF2', 0x05: 'XYZ2', 0x06: 'TEX0_1',
             0x07: 'TEX0_2', 0x08: 'CLAMP_1', 0x09: 'CLAMP_2', 0x0a: 'FOG', 0x0c: 'XYZF3', 0x0d: 'XYZ3',
             0x14: 'TEX1_1', 0x15: 'TEX1_2', 0x16: 'TEX2_1', 0x17: 'TEX2_2', 0x18: 'XYOFFSET_1', 0x19: 'XYOFFSET_2',
             0x1a: 'PRMODECONT', 0x1b: 'PRMODE', 0x1c: 'TEXCLUT', 0x22: 'SCANMSK', 0x3b: 'TEXA', 0x3d: 'FOGCOL',
             0x3f: 'TEXFLUSH', 0x40: 'SCISSOR_1', 0x41: 'SCISSOR_2', 0x42: 'ALPHA_1', 0x43: 'ALPHA_2', 0x44: 'DIMX',
             0x45: 'DTHE', 0x46: 'COLCLAMP', 0x47: 'TEST_1', 0x48: 'TEST_2', 0x49: 'PABE', 0x4a: 'FBA_1',
             0x4b: 'FBA_2', 0x4c: 'FRAME_1', 0x4d: 'FRAME_2', 0x4e: 'ZBUF_1', 0x4f: 'ZBUF_2', 0x50: 'BITBLTBUF',
             0x51: 'TRXPOS', 0x52: 'TRXREG', 0x53: 'TRXDIR', 0x54: 'HWREG', 0x60: 'SIGNAL', 0x61: 'FINISH',
             0x62: 'LABEL'}
PRIMS = ['point', 'line', 'linestrip', 'tri', 'tristrip', 'trifan', 'sprite', 'prim7']
VERTS = [1, 2, 2, 3, 3, 3, 2, 1]
PATHS = {1: 'PATH2', 2: 'PATH3', 3: 'PATH1', 0: 'PATH1old'}


def zstd_decompress(data):
    """A zstd stream (also frames without a content size) through zstd.dll's streaming API."""
    dlls = [p for d in ('tree-run', 'run', 'deps-v2.8.2/bin')
            for p in glob.glob(os.path.join(ROOT, 'extracted', 'pcsx2', d, 'zstd.dll'))]
    if not dlls:
        sys.exit('no zstd.dll under extracted/pcsx2 (build.py pcsx2-deps) to read a .zst dump')
    z = ctypes.CDLL(dlls[0])

    class Buf(ctypes.Structure):
        _fields_ = [('ptr', ctypes.c_void_p), ('size', ctypes.c_size_t), ('pos', ctypes.c_size_t)]
    z.ZSTD_createDStream.restype = ctypes.c_void_p
    z.ZSTD_decompressStream.restype = ctypes.c_size_t
    z.ZSTD_decompressStream.argtypes = [ctypes.c_void_p, ctypes.POINTER(Buf), ctypes.POINTER(Buf)]
    z.ZSTD_isError.argtypes = [ctypes.c_size_t]
    z.ZSTD_freeDStream.argtypes = [ctypes.c_void_p]
    ds = z.ZSTD_createDStream()
    src = ctypes.create_string_buffer(data, len(data))
    inb = Buf(ctypes.cast(src, ctypes.c_void_p), len(data), 0)
    out = ctypes.create_string_buffer(1 << 22)
    chunks = []
    try:
        while True:
            outb = Buf(ctypes.cast(out, ctypes.c_void_p), len(out), 0)
            r = z.ZSTD_decompressStream(ds, ctypes.byref(outb), ctypes.byref(inb))
            if z.ZSTD_isError(r):
                sys.exit('zstd: corrupt stream')
            chunks.append(out.raw[:outb.pos])
            if inb.pos == inb.size and outb.pos < len(out):
                break
    finally:
        z.ZSTD_freeDStream(ds)
    return b''.join(chunks)


def load(path):
    """The dump's bytes, decompressed for .xz / .zst."""
    d = open(path, 'rb').read()
    if path.endswith('.xz'):
        return lzma.decompress(d)
    if path.endswith('.zst'):
        return zstd_decompress(d)
    return d


def header(d):
    """(serial, crc, version, state offset, state size, offset of the first packet)"""
    crc, ss = struct.unpack_from('<II', d, 0)
    h = struct.unpack_from('<9I', d, 8)
    serial = d[8 + h[2]:8 + h[2] + h[3]].decode('ascii', 'replace')
    return serial, h[4], h[0], 8 + ss, h[1], 8 + ss + h[1] + 8192


class Gs:
    def __init__(self, state):
        self.kick_hook = None  # kick_hook(gs, path, ctxt) at each primitive kick (gs_census.py)
        self.reg = {}
        self.prim = struct.unpack_from('<Q', state, 4)[0]
        names = [0x1a, 0x1c, 0x22, 0x3b, 0x3d, 0x44, 0x45, 0x46, 0x49, 0x50, 0x53, 0x51, 0x52, 0x52]
        for i, a in enumerate(names):
            self.reg[a] = struct.unpack_from('<Q', state, 12 + 8 * i)[0]
        ctx = [0x18, 0x06, 0x14, 0x08, 0x34, 0x36, 0x40, 0x42, 0x47, 0x4a, 0x4c, 0x4e]
        for c in range(2):
            for i, a in enumerate(ctx):
                self.reg[a + c] = struct.unpack_from('<Q', state, 124 + 96 * c + 8 * i)[0]
        self.verts = 0
        self.draws = []
        self.writes = []
        self.paths = {}

    def write(self, a, v, path):
        self.writes.append((path, a, v))
        if a == 0x00:
            self.prim = v & 0x7ff if self.reg.get(0x1a, 1) & 1 else (self.prim & ~7) | (v & 7)
            self.verts = 0
        elif a in (0x04, 0x05, 0x0c, 0x0d):
            self.verts += 1
            t = self.prim & 7
            kick = a in (0x04, 0x05)
            if kick and self.verts >= VERTS[t] and (t not in (0, 1, 3, 6) or self.verts % VERTS[t] == 0):
                ctxt = (self.prim >> 9) & 1
                if self.kick_hook:
                    self.kick_hook(self, path, ctxt)
                x, y = (v & 0xffff) / 16.0, ((v >> 16) & 0xffff) / 16.0
                off = self.reg.get(0x18 + ctxt, 0)
                self.draws.append((path, PRIMS[t], ctxt, x - (off & 0xffff) / 16.0, y - ((off >> 32) & 0xffff) / 16.0,
                                   (v >> 32) & 0xffffffff, self.reg.get(0x4c + ctxt, 0), self.reg.get(0x4e + ctxt, 0),
                                   self.reg.get(0x47 + ctxt, 0), self.reg.get(0x06 + ctxt, 0), self.prim))
        elif a in (0x16, 0x17):  # TEX2_1/2: PSM and the CLUT fields of TEX0_1/2, as GSState::GIFRegHandlerTEX2
            mask = (0x3f << 20) | (0x7ffffff << 37)
            self.reg[a - 0x10] = (self.reg.get(a - 0x10, 0) & ~mask) | (v & mask)
            self.reg[a] = v
        else:
            self.reg[a] = v

    def transfer(self, path, data):
        st = self.paths.setdefault(path, {'left': 0, 'pad': False})
        pos, n = 0, len(data)
        while pos < n:
            if st['left'] == 0:
                if st['pad']:
                    pos += 8
                    st['pad'] = False
                    continue
                if pos + 16 > n:
                    break
                lo, hi = struct.unpack_from('<QQ', data, pos)
                pos += 16
                nloop, flg, nreg = lo & 0x7fff, (lo >> 58) & 3, (lo >> 60) & 0xf or 16
                st.update(flg=flg, nreg=nreg, regs=hi, index=0)
                if flg == 0 and (lo >> 46) & 1:
                    self.write(0x00, (lo >> 47) & 0x7ff, path)
                st['left'] = nloop if flg >= 2 else nloop * nreg
                st['pad'] = flg == 1 and st['left'] & 1
                continue
            if st['flg'] == 0:
                if pos + 16 > n:
                    break
                lo, hi = struct.unpack_from('<QQ', data, pos)
                pos += 16
                d = (st['regs'] >> (4 * st['index'])) & 0xf
                if d == 0xe:
                    self.write(hi & 0xff, lo, path)
                elif d == 0x4:
                    self.write(0x0c if (hi >> 47) & 1 else 0x04, (lo & 0xffff) | (((lo >> 32) & 0xffff) << 16) |
                               (((hi >> 4) & 0xffffff) << 32), path)
                elif d == 0x5:
                    self.write(0x0d if (hi >> 47) & 1 else 0x05, (lo & 0xffff) | (((lo >> 32) & 0xffff) << 16) |
                               ((hi & 0xffffffff) << 32), path)
                elif d == 0x0:
                    self.write(0x00, lo & 0x7ff, path)
                elif d != 0xf:
                    self.write(d, lo, path)
            elif st['flg'] == 1:
                if pos + 8 > n:
                    break
                v, = struct.unpack_from('<Q', data, pos)
                pos += 8
                d = (st['regs'] >> (4 * st['index'])) & 0xf
                if d < 0xe:
                    self.write(d, v, path)
            else:
                k = min(st['left'], (n - pos) // 16)
                if k == 0:
                    break
                pos += 16 * k
                st['left'] -= k
                continue
            st['left'] -= 1
            st['index'] = (st['index'] + 1) % st['nreg']


def strip_vif1(path, out):
    """Drops the VU1-state and VIF1-stream records (ROR_GSDUMP_VIF1): the rest is a dump stock PCSX2 replays."""
    d = load(path)
    crc, ss = struct.unpack_from('<II', d, 0)
    h = struct.unpack_from('<9I', d, 8)
    p = 8 + ss + h[1] + 8192
    parts, dropped = [d[:p]], 0
    while p < len(d):
        t, q = d[p], p + 1
        if t == 0:
            q += 5 + struct.unpack_from('<I', d, q + 1)[0]
        elif t == 1:
            q += 1
        elif t == 2:
            q += 4
        elif t == 3:
            q += 8192
        elif t in (0x40, 0x41):
            q += 4 + struct.unpack_from('<I', d, q)[0]
            dropped += 1
            p = q
            continue
        else:
            sys.exit('bad packet type %d at %d' % (t, p))
        parts.append(d[p:q])
        p = q
    open(out, 'wb').write(b''.join(parts))
    print('wrote %s: %d VU1/VIF1 records dropped' % (out, dropped))


def frames(path):
    d = load(path)
    crc, ss = struct.unpack_from('<II', d, 0)
    h = struct.unpack_from('<9I', d, 8)
    p = 8 + ss
    gs = Gs(d[p:p + h[1]])
    p += h[1] + 8192
    out = []

    def close():
        p1 = [x for x in gs.draws if x[0] == 3]
        neg = sum(1 for x in p1 if x[5] >= 0x80000000)
        off = sum(1 for x in p1 if not (0 <= x[3] < 512 and 0 <= x[4] < 448))
        out.append((len(gs.draws), len(p1), neg, off))
        gs.draws = []

    started = False
    while p < len(d):
        t = d[p]
        p += 1
        if t == 0:
            path, n = d[p], struct.unpack_from('<I', d, p + 1)[0]
            if path == 2:
                if started:
                    close()
                started = True
            gs.transfer(path, d[p + 5:p + 5 + n])
            p += 5 + n
        elif t == 1:
            p += 1
        elif t == 2:
            p += 4
        elif t == 3:
            p += 8192
        elif t in (0x40, 0x41):  # VU1 state / VIF1 stream (ROR_GSDUMP_VIF1)
            p += 4 + struct.unpack_from('<I', d, p)[0]
    if started and gs.draws:
        close()
    print('frame  primitives  PATH1  Z<0  off-screen   (frame 0 = the first PATH3 packet after the dump start)')
    for i, (n, p1, neg, off) in enumerate(out):
        print('%5d  %10d  %5d  %4d  %10d' % (i, n, p1, neg, off))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dump')
    ap.add_argument('--draws', action='store_true')
    ap.add_argument('--regs', action='store_true')
    ap.add_argument('--frames', action='store_true')
    ap.add_argument('--strip-vif1', metavar='OUT')
    args = ap.parse_args()
    if args.frames:
        return frames(args.dump)
    if args.strip_vif1:
        return strip_vif1(args.dump, args.strip_vif1)
    d = load(args.dump)
    crc, ss = struct.unpack_from('<II', d, 0)
    h = struct.unpack_from('<9I', d, 8)
    serial = d[8 + h[2]:8 + h[2] + h[3]].decode('ascii', 'replace')
    print('header: version %d, state %d bytes, serial %s, crc %08x' % (h[0], h[1], serial, h[4]))
    p = 8 + ss
    gs = Gs(d[p:p + h[1]])
    p += h[1] + 8192
    print('state: PRIM %x PRMODECONT %x  FRAME_1 %x ZBUF_1 %x TEST_1 %x XYOFFSET_1 %x SCISSOR_1 %x' % (
        gs.prim, gs.reg[0x1a], gs.reg[0x4c], gs.reg[0x4e], gs.reg[0x47], gs.reg[0x18], gs.reg[0x40]))
    frame, counts = 0, {}
    while p < len(d):
        t = d[p]
        p += 1
        if t == 0:
            path, n = d[p], struct.unpack_from('<I', d, p + 1)[0]
            gs.transfer(path, d[p + 5:p + 5 + n])
            counts[PATHS.get(path, path)] = counts.get(PATHS.get(path, path), 0) + 1
            p += 5 + n
        elif t == 1:
            p += 1
            if counts or gs.draws:
                print('vsync %d: transfers %s, %d primitives' % (frame, counts, len(gs.draws)))
                for dr in gs.draws if args.draws else []:
                    print('    %-6s %-8s ctxt %d  xy %7.1f %7.1f  z %08x  FRAME %x ZBUF %x TEST %x TEX0 %x' % (
                        PATHS.get(dr[0]), dr[1], dr[2], dr[3], dr[4], dr[5], dr[6], dr[7], dr[8], dr[9]))
                for path, a, v in gs.writes if args.regs else []:
                    print('    %-6s %-10s %016x' % (PATHS.get(path), REG_NAMES.get(a, '%02x' % a), v))
            frame += 1
            counts, gs.draws, gs.writes = {}, [], []
        elif t == 2:
            p += 4
        elif t == 3:
            p += 8192
        elif t in (0x40, 0x41):  # VU1 state / VIF1 stream (ROR_GSDUMP_VIF1)
            p += 4 + struct.unpack_from('<I', d, p)[0]
        else:
            sys.exit('bad packet type %d at %d' % (t, p - 1))
    open_paths = {PATHS.get(k): v['left'] for k, v in gs.paths.items() if v['left']}
    print('end: paths inside a GIF packet: %s' % (open_paths or 'none'))


if __name__ == '__main__':
    main()
