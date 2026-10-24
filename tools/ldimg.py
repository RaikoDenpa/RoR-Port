"""Now-loading image (CMNTEX/LOADING.BIN; the EU build asks for CMNTEX/LOAD_EU.BIN, absent from the disc).

Usage:
  python ldimg.py <file> --dump              print layout facts
  python ldimg.py <file> --png <out.png>     write the image as PNG

Layout (system/sysNowLoading.cpp, 0x2064e4/0x206644):
  0x00000  256*256 u8 pixel indices, row-major      -> GS texture PSMT8, TBW 4 (256 px), TW=TH=8
  0x10000  256 * u32 RGBA CLUT, CSM1 order          -> uploaded as a 16x16 PSMCT32 image to CBP 0x1f40
  0x10400  padding (LOADING.BIN: 0xE0 zero bytes)
Alpha is PS2-style (0x80 = opaque) and is scaled to 0..255.
"""
import argparse
import struct
import sys
import zlib

W = H = 256
CLUT_OFF = W * H


def csm1(i):
    """CSM1 CLUT index order: within every 32 entries, entries 8-15 and 16-23 are swapped."""
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def palette(data):
    raw = struct.unpack_from('<256I', data, CLUT_OFF)
    pal = []
    for i in range(256):
        c = raw[csm1(i)]
        r, g, b, a = c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, (c >> 24) & 0xff
        pal.append((r, g, b, min(255, a * 2)))
    return pal


def write_png(path, w, h, rows_rgba):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows_rgba)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        f.write(chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('file')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--png', metavar='OUT')
    a = ap.parse_args()
    data = open(a.file, 'rb').read()
    if len(data) < CLUT_OFF + 1024:
        sys.exit('too small for 256x256 PSMT8 + CLUT (%d bytes)' % len(data))
    pal = palette(data)
    if a.dump:
        used = sorted(set(data[:CLUT_OFF]))
        print('size %d (pixels 0x10000, CLUT 0x400, tail %d bytes, tail all zero: %s)' % (
            len(data), len(data) - CLUT_OFF - 1024, not any(data[CLUT_OFF + 1024:])))
        print('distinct indices %d: %s' % (len(used), used[:32]))
        print('alpha values used:', sorted({pal[i][3] for i in used}))
    if a.png:
        rows = []
        for y in range(H):
            rows.append(b''.join(bytes(pal[p]) for p in data[y * W:(y + 1) * W]))
        write_png(a.png, W, H, rows)
        print('wrote', a.png)
    if not (a.dump or a.png):
        ap.print_help()


if __name__ == '__main__':
    main()
