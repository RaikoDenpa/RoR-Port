"""XVAB sound-effect banks (DATA/SND/XV/NNMM.SE) -> dump / WAV.

Layout and semantics: notes/formats/xvab.md. In short: 'XVAB', u8 1, u8 1, u16 0, u32 header size, then chunks
{char tag[4], u32 payload size}: HEAD TGIF TGPG TGSP SEIF SEGP SERQ SESQ VELT PANT LFOT. The header goes to IOP
memory, the PS-ADPCM body (at align16(header size), HEAD+4 bytes) to SPU RAM. Programs (TGIF -> TGPG) hold 36-byte
splits (TGSP: key range, root note, ADSR, pitch, sample offset). SEs (SEIF group -> SEGP -> SERQ -> SESQ) are short
bytecode sequences, in the data always a note-on (program, key, velocity), sometimes preceded by a note-off.

Usage:
  python xvab.py <bank.SE> --dump [--names G000_TBL.BIN]
  python xvab.py <bank.SE> --wav <out_dir> [--names G000_TBL.BIN]   one WAV per SE, at the driver's pitch
  python xvab.py <bank.SE> --samples <out_dir>                     each sample once, at its split's base rate
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psadpcm  # noqa: E402

SPU_RATE = 48000  # SPU pitch 0x1000

# sceSdNote2Pitch (the disc's LIBSD.IRX, export 13 at 0x3bd0) multiplies two u16 tables at 0x54c0 / 0x54d8. Both equal
# 0x8000 * 2^(n/12) and 0x8000 * 2^(f/1536) truncated (checked entry by entry), so they are generated here.
SEMITONE = [int(0x8000 * 2 ** (n / 12)) for n in range(12)]
FINE = [int(0x8000 * 2 ** (f / 1536)) for f in range(128)]


def note2pitch(center_note, center_fine, note, fine=0):
    """libsd sceSdNote2Pitch for non-negative fine values (the only case in the data)."""
    f = fine + center_fine
    if f < 0:
        return int(round(0x1000 * 2 ** ((note - center_note) / 12 + f / 1536))) & 0xffff
    q, f = f >> 7, f & 0x7f
    n = note + q - center_note
    octave, step = int(n / 12), int(n - 12 * int(n / 12))  # C division / remainder
    shift = octave - 2
    if step < 0:
        step += 12
        shift -= 1
    v = SEMITONE[step] * FINE[f] >> 16
    if shift < 0:
        v = (v + (1 << (-shift - 1))) >> -shift
    return v & 0xffff


class Bank:
    def __init__(self, data):
        if data[:4] != b'XVAB':
            raise ValueError('not an XVAB bank')
        self.d = d = data
        self.version = struct.unpack_from('<H', d, 4)[0]
        self.header_size = struct.unpack_from('<I', d, 8)[0]
        self.chunks = {}
        p = 0xc
        while p + 8 <= self.header_size:
            tag, size = d[p:p + 4].decode('ascii', 'replace'), struct.unpack_from('<I', d, p + 4)[0]
            self.chunks[tag] = (p + 8, size)
            p += 8 + size
        h = self.chunks['HEAD'][0]
        self.head = struct.unpack_from('<7I', d, h)
        self.body = (self.header_size + 15) & ~15
        self.body_size = self.head[1]

    def u16(self, o):
        return struct.unpack_from('<H', self.d, o)[0]

    def u32(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def programs(self):
        """[(tgpg bytes 0-1, [split dict...])] indexed by program number (TGIF order)."""
        tgif = self.head[2]
        out = []
        for i in range(self.u16(tgif)):
            pg = self.u32(tgif + 4 + 4 * i)
            if not pg:
                out.append(None)
                continue
            cnt, off = self.u16(pg + 2), self.u32(pg + 4)
            out.append((self.d[pg:pg + 2], [self.split(off + 0x24 * k) for k in range(cnt)]))
        return out

    def split(self, o):
        s = self.d[o:o + 0x24]
        return {
            'flags': s[0], 'b1': s[1], 'group': s[2], 'b3': s[3], 'key_lo': s[4], 'key_hi': s[5],
            'root': s[6], 'root_fine': s[7], 'velt': s[8], 'pant': s[9], 'vol_l': s[10], 'vol_r': s[11],
            'pitch': struct.unpack_from('<H', s, 0xc)[0], 'adsr1': struct.unpack_from('<H', s, 0x10)[0],
            'adsr2': struct.unpack_from('<H', s, 0x12)[0], 'offset': struct.unpack_from('<I', s, 0x14)[0],
            'lfo_tables': tuple(s[0x18:0x1b]), 'lfo_depth': tuple(s[0x1b:0x1e]), 'lfo_rate': tuple(s[0x1e:0x21]),
            'bend': (s[0x21], s[0x22]),
        }

    def ses(self):
        """{(group, number): (serq bytes a, b, flags, [commands])}."""
        seif = self.head[3]
        out = {}
        for g in range(self.u16(seif)):
            go = self.u32(seif + 4 + 4 * g)
            if not go:
                continue
            for k in range(self.u16(go)):
                rq = self.u32(go + 4 + 4 * k)
                if rq:
                    a, b, fl, sq = struct.unpack_from('<BBHI', self.d, rq)
                    out[(g, k)] = (a, b, fl, self.sequence(sq))
        return out

    def varint(self, p):
        v = 0
        while True:
            b = self.d[p]
            p += 1
            v = v << 7 | (b & 0x7f)
            if not b & 0x80:
                return v, p

    def sequence(self, p):
        """Decode SESQ bytecode (interpreter 0x7260 in RXSNDDRV.IRX): op = byte & 0x3f, flag = byte >> 6, operands,
        then a varint delta time. Returns [(name, flag, operands, delta)]."""
        cmds = []
        for _ in range(256):
            c = self.d[p]
            op, flag = c & 0x3f, c >> 6
            p += 1
            if op == 0:
                cmds.append(('end', flag, (), 0))
                break
            if op == 2:
                args = (self.d[p],)
                p += 1
            elif op == 4:
                tag, p = self.varint(p)
                chan, p = self.varint(p)
                args = (tag, chan, self.d[p], self.d[p + 1], self.d[p + 2])
                p += 3
            elif op in (5, 6):
                tag, p = self.varint(p)
                args = (tag,)
            elif op in (7, 9):
                tag, p = self.varint(p)
                args = (tag, self.d[p], self.d[p + 1])
                p += 2
            elif op in (8, 10):
                tag, p = self.varint(p)
                a, b = self.d[p], self.d[p + 1]
                c2, p = self.varint(p + 2)
                args = (tag, a, b, c2)
            elif op in (1, 3, 0xb):
                args = ()
            else:
                cmds.append(('op%d?' % op, flag, (), 0))
                break
            delta, p = self.varint(p)
            name = {1: 'nop', 2: 'loop', 3: 'next', 4: 'note_on', 5: 'note_off', 6: 'note_off6', 7: 'ctl7',
                    8: 'ctl8', 9: 'ctl9', 10: 'ctl10', 11: 'nop'}[op]
            cmds.append((name, flag, args, delta))
        return cmds

    def sample(self, offset):
        """Decode one sample from body + offset until a frame with the end flag; returns (samples, loop_frame)."""
        p = self.body + offset
        loop = None
        frames = bytearray()
        k = 0
        while p + 16 <= len(self.d):
            fr = self.d[p:p + 16]
            if fr[1] & 4 and loop is None:
                loop = k
            frames += fr
            k += 1
            p += 16
            if fr[1] & 1:
                break
        return psadpcm.decode(bytes(frames)), (loop if self.d[p - 15] & 2 else None)


def split_rate(sp, key):
    center = (key, 0) if sp['flags'] & 0x40 else (sp['root'], sp['root_fine'])
    pitch = note2pitch(center[0], center[1], key) * sp['pitch'] >> 12
    return min(pitch, 0x3fff) * SPU_RATE / 0x1000, pitch


def table_names(path, slot):
    """G000_TBL.BIN: 0x38-byte records {char name[0x1c], u32 id, u16 slot, u16 group, u16 number, u16 volume,
    u16 pan, ...}; returns {(group, number): name} for one bank slot."""
    d = open(path, 'rb').read()
    out = {}
    for i in range(len(d) // 0x38):
        r = d[i * 0x38:(i + 1) * 0x38]
        sid, s, g, n = struct.unpack_from('<IHHH', r, 0x1c)
        if sid != 0xffffffff and s == slot:
            # Shift-JIS: one name has a full-width letter ('wo_ｄoors_open'), kept as the developers wrote it
            out.setdefault((g, n), r[:0x1c].split(b'\0')[0].decode('cp932', 'replace'))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('bank')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--wav', metavar='OUT_DIR')
    ap.add_argument('--samples', metavar='OUT_DIR')
    ap.add_argument('--names', help='G000_TBL.BIN, to name SEs (bank slot = first two digits of the file name)')
    a = ap.parse_args()
    sys.stdout.reconfigure(errors='backslashreplace')
    bank = Bank(open(a.bank, 'rb').read())
    base = os.path.splitext(os.path.basename(a.bank))[0]
    slot = int(base[:2]) if base[:2].isdigit() else None
    names = table_names(a.names, slot) if a.names and slot is not None else {}
    progs = bank.programs()
    ses = bank.ses()

    def resolve(cmds):
        for name, _flag, args, _delta in cmds:
            if name == 'note_on':
                prog, key, vel = args[2], args[3], args[4]
                entry = progs[prog] if prog < len(progs) else None
                return [(sp, key, vel) for sp in (entry[1] if entry else []) if sp['key_lo'] <= key <= sp['key_hi']]
        return []

    if a.dump:
        print('%s: XVAB v%x, slot %s, header %#x, body %#x + %#x, chunks %s' % (
            a.bank, bank.version, slot, bank.header_size, bank.body, bank.body_size, ' '.join(bank.chunks)))
        for i, entry in enumerate(progs):
            if entry is None:
                continue
            print('program %d: %d splits (TGPG bytes %s)' % (i, len(entry[1]), entry[0].hex()))
            for sp in entry[1]:
                print('   keys %#04x-%#04x root %#04x+%3d/128 pitch %#06x vol %d/%d adsr %04x/%04x sample +%#x' % (
                    sp['key_lo'], sp['key_hi'], sp['root'], sp['root_fine'], sp['pitch'], sp['vol_l'], sp['vol_r'],
                    sp['adsr1'], sp['adsr2'], sp['offset']))
        for (g, n), (ra, rb, fl, cmds) in sorted(ses.items()):
            txt = '; '.join('%s%s %s +%d' % (c[0], '*' if c[1] else '', ','.join('%#x' % x for x in c[2]), c[3])
                            for c in cmds)
            notes = resolve(cmds)
            rates = ', '.join('%.0f Hz' % split_rate(sp, key)[0] for sp, key, _ in notes)
            print('SE %d/%d %-28s serq %02x %02x %04x | %s | %s' % (g, n, names.get((g, n), ''), ra, rb, fl, txt, rates))
    if a.samples:
        os.makedirs(a.samples, exist_ok=True)
        done = set()
        for entry in progs:
            for sp in (entry[1] if entry else []):
                if sp['offset'] in done:
                    continue
                done.add(sp['offset'])
                pcm, loop = bank.sample(sp['offset'])
                rate = round(sp['pitch'] * SPU_RATE / 0x1000)
                path = os.path.join(a.samples, '%s_%06x%s.wav' % (base, sp['offset'], '_loop' if loop is not None else ''))
                psadpcm.write_wav(path, rate, [pcm])
        print('wrote %d samples to %s' % (len(done), a.samples))
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)
        n = 0
        for (g, k), (_a, _b, _f, cmds) in sorted(ses.items()):
            notes = resolve(cmds)
            if not notes:
                continue
            sp, key, _vel = notes[0]
            rate, _pitch = split_rate(sp, key)
            pcm, _loop = bank.sample(sp['offset'])
            label = names.get((g, k), '')
            path = os.path.join(a.wav, '%s_%02d_%03d%s.wav' % (base, g, k, '_' + label if label else ''))
            psadpcm.write_wav(path, round(rate), [pcm])
            n += 1
        print('wrote %d SE WAVs to %s' % (n, a.wav))
    if not (a.dump or a.wav or a.samples):
        ap.print_help()


if __name__ == '__main__':
    main()
