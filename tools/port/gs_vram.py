"""Decode textures and CLUTs from a GS local-memory dump (src/ror_gs.cpp, ROR_GS_VRAM=1 -> gs_frame_N_*.vram).

Addressing follows the GS block/column layout (the same tables as PS2Recomp's ps2_gs_psmct32.h / ps2_gs_psmt8.h).
A CT32 CLUT stored CSM1 is a 16x16 (256 colours) or 8x2 (16 colours) block; entry i sits at x = (i & 7) | ((i >> 1) & 8),
y = ((i >> 3) & 1) | ((i >> 4) & 0xE), i.e. index bits 3 and 4 swapped against a linear 16-wide layout.

  python tools/port/gs_vram.py VRAM --clut CBP [--dump]                 print the CLUT (CT32, CSM1)
  python tools/port/gs_vram.py VRAM --t8 TBP TBW W H --clut CBP --png OUT
  python tools/port/gs_vram.py VRAM --t8 TBP TBW W H --clut CBP --tim2 FILE.tm2   compare indices and CLUT with a TIM2
  python tools/port/gs_vram.py VRAM --ct32 BP BW W H --png OUT            a CT32 buffer (frame or texture)
"""
import argparse, struct, sys, zlib

BLOCK = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29],
         [10, 11, 14, 15, 26, 27, 30, 31]]
COL32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29],
         [18, 19, 22, 23, 26, 27, 30, 31], [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
         [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
COL8 = [[0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
        [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
        [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
        [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
        [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
        [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
        [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
        [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
        [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
        [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
        [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
        [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
        [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
        [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
        [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
        [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255]]


def addr32(bp, bw, x, y):
    page = (bp >> 5) + (y >> 5) * max(bw, 1) + (x >> 6)
    block = (bp & 31) + BLOCK[(y >> 3) & 3][(x >> 3) & 7]
    return ((page << 13) + ((block >> 5) << 13) + (block & 31) * 256 + COL32[y & 7][x & 7] * 4) & 0x3FFFFF


def addr8(bp, bw, x, y):
    page = (bp >> 5) + (y >> 6) * max(bw >> 1, 1) + (x >> 7)
    block = (bp & 31) + BLOCK[(y >> 4) & 3][(x >> 4) & 7]
    return ((page << 13) + ((block >> 5) << 13) + (block & 31) * 256 + COL8[y & 15][x & 15]) & 0x3FFFFF


def clut_csm1(vram, cbp, n=256):
    out = []
    for i in range(n):
        x = (i & 7) | ((i >> 1) & 8)
        y = ((i >> 3) & 1) | ((i >> 4) & 0xE)
        out.append(struct.unpack_from('<I', vram, addr32(cbp, 1, x, y))[0])
    return out


def write_png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def rgba(c):
    a = (c >> 24) & 0xff
    return [c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, min(255, a * 2)]


def read_tim2(path):
    d = open(path, 'rb').read()
    off = 0x10
    total, clut_size, image_size, hdr, colors, _, _, clut_type, image_type, w, h = struct.unpack_from('<IIIHHBBBBHH', d, off)
    image = d[off + hdr:off + hdr + image_size]
    raw = d[off + hdr + image_size:off + hdr + image_size + clut_size]
    clut = list(struct.unpack_from('<%dI' % (clut_size // 4), raw))
    if not clut_type & 0x80:  # CSM1 storage: entries 8-15 and 16-23 of every 32 swapped
        clut = [clut[(i & ~0x18) | ((i & 8) << 1) | ((i & 0x10) >> 1)] for i in range(len(clut))]
    return w, h, image, clut


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('vram')
    ap.add_argument('--clut', type=int)
    ap.add_argument('--t8', nargs=4, type=int, metavar=('TBP', 'TBW', 'W', 'H'))
    ap.add_argument('--ct32', nargs=4, type=int, metavar=('BP', 'BW', 'W', 'H'))
    ap.add_argument('--png')
    ap.add_argument('--tim2')
    ap.add_argument('--dump', action='store_true')
    args = ap.parse_args()
    vram = open(args.vram, 'rb').read()

    clut = clut_csm1(vram, args.clut) if args.clut is not None else None
    if clut and args.dump:
        for i in range(0, 256, 8):
            print('%3d: %s' % (i, ' '.join('%08x' % c for c in clut[i:i + 8])))
    if args.ct32:
        bp, bw, w, h = args.ct32
        px = []
        for y in range(h):
            for x in range(w):
                c = struct.unpack_from('<I', vram, addr32(bp, bw, x, y))[0]
                px += rgba(c)[:3] + [255]
        write_png(args.png, w, h, px)
        print('wrote', args.png)
    if args.t8:
        tbp, tbw, w, h = args.t8
        idx = bytes(vram[addr8(tbp, tbw, x, y)] for y in range(h) for x in range(w))
        if args.png:
            px = []
            for i in idx:
                px += rgba(clut[i])
            write_png(args.png, w, h, px)
            print('wrote', args.png)
        if args.tim2:
            tw, th, image, fclut = read_tim2(args.tim2)
            same = sum(1 for y in range(min(h, th)) for x in range(min(w, tw)) if idx[y * w + x] == image[y * tw + x])
            print('indices: %d of %d equal to %s' % (same, min(h, th) * min(w, tw), args.tim2))
            diff = [i for i in range(256) if clut[i] != fclut[i]]
            print('CLUT: %d of 256 entries differ%s' % (len(diff), (' (first: ' + ', '.join(
                '%d vram %08x file %08x' % (i, clut[i], fclut[i]) for i in diff[:6]) + ')') if diff else ''))


if __name__ == '__main__':
    main()
